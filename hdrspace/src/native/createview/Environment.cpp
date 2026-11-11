#include "Environment.h"

#include "Environment.h"
#include "Geometry.h"
#include "SceneFile.h"
#include "ViewFile.h"
#include "../core/TextUtil.h"
#include "hdrimage.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace hdrspace::createview {

using namespace hdrspace::text;

std::vector<std::string> createViewHeaderValues(const HdrImage& image, const std::string& key) {
    std::vector<std::string> values;
    const std::string prefix = key + "=";
    for (const auto& line : image.headerLines) {
        if (line.rfind(prefix, 0) == 0) {
            values.push_back(trim(line.substr(prefix.size())));
        }
    }
    return values;
}

std::string createViewHeaderValue(const HdrImage& image, const std::string& key) {
    const auto values = createViewHeaderValues(image, key);
    if (values.empty()) {
        return {};
    }
    std::string joined;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            joined += " ";
        }
        joined += values[i];
    }
    return joined;
}

double parseCreateViewInlineRotateZDegrees(const std::string& text) {
    const auto tokens = splitWhitespace(text);
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == "-rz") {
            double value = 0.0;
            if (tryParseFloating(tokens[i + 1], value)) {
                return value;
            }
        }
    }
    return 0.0;
}

std::vector<CreateViewInlinePrimitive> parseCreateViewInlinePrimitives(const std::string& text) {
    std::vector<CreateViewInlinePrimitive> primitives;
    const auto tokens = splitWhitespace(text);
    size_t i = 0;
    while (i + 6 <= tokens.size()) {
        CreateViewInlinePrimitive primitive;
        primitive.modifier = tokens[i++];
        primitive.type = tokens[i++];
        primitive.identifier = tokens[i++];

        int n1 = 0;
        int n2 = 0;
        int n3 = 0;
        // Counts must be non-negative and the words they announce must exist: a negative count
        // wrapped the size_t bound, and a primitive cut off after its strings read past the end.
        if (!tryParseInteger(tokens[i++], n1) || n1 < 0 || i + static_cast<size_t>(n1) > tokens.size()) {
            break;
        }
        i += static_cast<size_t>(n1);
        if (i >= tokens.size() || !tryParseInteger(tokens[i++], n2) || n2 < 0 || i + static_cast<size_t>(n2) > tokens.size()) {
            break;
        }
        i += static_cast<size_t>(n2);
        if (i >= tokens.size() || !tryParseInteger(tokens[i++], n3) || n3 < 0 || i + static_cast<size_t>(n3) > tokens.size()) {
            break;
        }
        primitive.numericArgs.reserve(static_cast<size_t>(std::max(0, n3)));
        for (int j = 0; j < n3; ++j) {
            double value = 0.0;
            if (tryParseFloating(tokens[i + static_cast<size_t>(j)], value)) {
                primitive.numericArgs.push_back(value);
            }
        }
        i += static_cast<size_t>(n3);
        primitives.push_back(std::move(primitive));
    }
    return primitives;
}

bool parseCreateViewEnvironmentSourceFromHeader(
    const std::string& text,
    std::array<double, 3>& direction,
    double& solidAngle,
    std::array<float, 3>* color) {
    const auto primitives = parseCreateViewInlinePrimitives(text);
    if (primitives.empty()) {
        return false;
    }

    std::map<std::string, std::array<float, 3>> emitters;
    for (const auto& primitive : primitives) {
        const std::string type = createViewLower(primitive.type);
        if ((type == "light" || type == "glow") && primitive.numericArgs.size() >= 3) {
            emitters[primitive.identifier] = {
                static_cast<float>(std::max(0.0, primitive.numericArgs[0])),
                static_cast<float>(std::max(0.0, primitive.numericArgs[1])),
                static_cast<float>(std::max(0.0, primitive.numericArgs[2])),
            };
        }
    }

    for (const auto& primitive : primitives) {
        if (createViewLower(primitive.type) != "source" || primitive.numericArgs.size() < 4) {
            continue;
        }
        direction = createViewVec3Normalize({
            primitive.numericArgs[0],
            primitive.numericArgs[1],
            primitive.numericArgs[2]
        }, {0.0, -1.0, 0.0});
        const double angleDegrees = primitive.numericArgs[3];
        const double halfAngle = std::clamp(angleDegrees * 0.5 * M_PI / 180.0, 0.0, M_PI * 0.5);
        solidAngle = 2.0 * M_PI * (1.0 - std::cos(halfAngle));
        if (color) {
            auto it = emitters.find(primitive.modifier);
            if (it == emitters.end()) {
                it = emitters.find(primitive.identifier);
            }
            if (it != emitters.end()) {
                *color = it->second;
                const float maxChannel = std::max({(*color)[0], (*color)[1], (*color)[2], 1e-6f});
                (*color)[0] = std::pow(std::clamp((*color)[0] / maxChannel, 0.0f, 1.0f), 0.7f);
                (*color)[1] = std::pow(std::clamp((*color)[1] / maxChannel, 0.0f, 1.0f), 0.7f);
                (*color)[2] = std::pow(std::clamp((*color)[2] / maxChannel, 0.0f, 1.0f), 0.7f);
            }
        }
        return true;
    }
    return false;
}

std::string sanitizeCreateViewEnvironmentRadText(const std::string& text) {
    std::ostringstream out;
    std::istringstream in(text);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        if (!first) {
            out << "\n";
        }
        out << line;
        first = false;
    }
    return out.str();
}

bool parseCreateViewEnvironmentSourcesFromRadText(
    const std::string& text,
    std::array<double, 3>& skyDirection,
    std::array<float, 3>& skyColor,
    std::array<double, 3>& sunDirection,
    double& sunSolidAngle,
    std::array<float, 3>& sunColor,
    bool& hasSky,
    bool& hasSun
) {
    hasSky = false;
    hasSun = false;
    const auto primitives = parseCreateViewInlinePrimitives(text);
    if (primitives.empty()) {
        return false;
    }

    std::map<std::string, std::array<float, 3>> emitters;
    for (const auto& primitive : primitives) {
        const std::string type = createViewLower(primitive.type);
        if ((type == "light" || type == "glow") && primitive.numericArgs.size() >= 3) {
            emitters[primitive.identifier] = {
                static_cast<float>(std::max(0.0, primitive.numericArgs[0])),
                static_cast<float>(std::max(0.0, primitive.numericArgs[1])),
                static_cast<float>(std::max(0.0, primitive.numericArgs[2])),
            };
        }
    }

    const auto lookupColor = [&](const CreateViewInlinePrimitive& primitive) {
        std::array<float, 3> color{1.0f, 1.0f, 1.0f};
        auto it = emitters.find(primitive.modifier);
        if (it == emitters.end()) {
            it = emitters.find(primitive.identifier);
        }
        if (it != emitters.end()) {
            color = it->second;
            const float maxChannel = std::max({color[0], color[1], color[2], 1e-6f});
            color[0] = std::pow(std::clamp(color[0] / maxChannel, 0.0f, 1.0f), 0.7f);
            color[1] = std::pow(std::clamp(color[1] / maxChannel, 0.0f, 1.0f), 0.7f);
            color[2] = std::pow(std::clamp(color[2] / maxChannel, 0.0f, 1.0f), 0.7f);
        }
        return color;
    };

    for (const auto& primitive : primitives) {
        if (createViewLower(primitive.type) != "source" || primitive.numericArgs.size() < 4) {
            continue;
        }
        const std::array<double, 3> direction = createViewVec3Normalize({
            primitive.numericArgs[0],
            primitive.numericArgs[1],
            primitive.numericArgs[2]
        }, {0.0, -1.0, 0.0});
        const double angleDegrees = primitive.numericArgs[3];
        const double halfAngle = std::clamp(angleDegrees * 0.5 * M_PI / 180.0, 0.0, M_PI * 0.5);
        const double solidAngle = 2.0 * M_PI * (1.0 - std::cos(halfAngle));
        const auto color = lookupColor(primitive);
        const std::string idLower = createViewLower(primitive.identifier);

        const bool isSky = angleDegrees >= 179.0 || idLower.find("sky") != std::string::npos;
        if (isSky && !hasSky) {
            skyDirection = direction;
            skyColor = color;
            hasSky = true;
            continue;
        }
        if (!hasSun) {
            sunDirection = direction;
            sunSolidAngle = solidAngle;
            sunColor = color;
            hasSun = true;
        }
    }
    return hasSky || hasSun;
}

std::vector<CreateViewEnvironmentInfo::Patch> buildCreateViewUniformEnvironmentPatches(
    const std::array<double, 3>& worldCenterDir,
    const std::array<double, 3>& worldUp,
    const std::array<float, 3>& color,
    float alpha) {
    constexpr int kElevationSteps = 9;
    constexpr int kAzimuthSteps = 24;
    std::vector<CreateViewEnvironmentInfo::Patch> patches;
    patches.reserve(static_cast<size_t>(kElevationSteps * kAzimuthSteps));

    for (int el = 0; el < kElevationSteps; ++el) {
        const double e0 = (static_cast<double>(el) / static_cast<double>(kElevationSteps)) * (M_PI * 0.5);
        const double e1 = (static_cast<double>(el + 1) / static_cast<double>(kElevationSteps)) * (M_PI * 0.5);
        for (int az = 0; az < kAzimuthSteps; ++az) {
            const double p0 = static_cast<double>(az) / static_cast<double>(kAzimuthSteps) * (2.0 * M_PI);
            const double p1 = static_cast<double>(az + 1) / static_cast<double>(kAzimuthSteps) * (2.0 * M_PI);
            CreateViewEnvironmentInfo::Patch patch;
            patch.directions = {
                createViewDirectionFromAzimuthElevation(worldCenterDir, worldUp, p0, e0),
                createViewDirectionFromAzimuthElevation(worldCenterDir, worldUp, p1, e0),
                createViewDirectionFromAzimuthElevation(worldCenterDir, worldUp, p1, e1),
                createViewDirectionFromAzimuthElevation(worldCenterDir, worldUp, p0, e1),
            };
            patch.color = color;
            patch.alpha = alpha;
            patches.push_back(std::move(patch));
        }
    }
    return patches;
}

std::array<float, 3> sampleCreateViewHdrAngularFisheye(
    const HdrImage& image,
    const std::array<double, 3>& direction,
    const std::array<double, 3>& viewDir,
    const std::array<double, 3>& up,
    double imageZRotationDegrees) {
    if (image.width == 0 || image.height == 0 || image.rgb.empty()) {
        return {0.0f, 0.0f, 0.0f};
    }

    const auto forward = createViewVec3Normalize(viewDir, {0.0, -1.0, 0.0});
    auto right = createViewVec3Normalize(createViewVec3Cross(forward, up), {1.0, 0.0, 0.0});
    auto screenUp = createViewVec3Normalize(createViewVec3Cross(right, forward), {0.0, 0.0, 1.0});

    const double cosTheta = std::clamp(createViewVec3Dot(direction, forward), -1.0, 1.0);
    const double theta = std::acos(cosTheta);
    if (theta > M_PI * 0.5 + 1e-6) {
        return {0.0f, 0.0f, 0.0f};
    }

    const double x = createViewVec3Dot(direction, right);
    const double y = createViewVec3Dot(direction, screenUp);
    const double rotation = imageZRotationDegrees * M_PI / 180.0;
    const double xr = x * std::cos(rotation) - y * std::sin(rotation);
    const double yr = x * std::sin(rotation) + y * std::cos(rotation);
    const double phi = std::atan2(yr, xr);
    const double r = std::clamp(theta / (M_PI * 0.5), 0.0, 1.0);
    const double u = 0.5 + 0.5 * r * std::cos(phi);
    const double v = 0.5 + 0.5 * r * std::sin(phi);

    const size_t ix = static_cast<size_t>(std::clamp(std::round(u * static_cast<double>(image.width - 1)), 0.0, static_cast<double>(image.width - 1)));
    const size_t iy = static_cast<size_t>(std::clamp(std::round(v * static_cast<double>(image.height - 1)), 0.0, static_cast<double>(image.height - 1)));
    const size_t index = (iy * image.width + ix) * 3;
    if (index + 2 >= image.rgb.size()) {
        return {0.0f, 0.0f, 0.0f};
    }
    return {
        std::max(0.0f, image.rgb[index + 0]),
        std::max(0.0f, image.rgb[index + 1]),
        std::max(0.0f, image.rgb[index + 2]),
    };
}

std::vector<CreateViewEnvironmentInfo::Patch> buildCreateViewEnvironmentPatches(
    const HdrImage& image,
    const std::array<double, 3>& worldCenterDir,
    const std::array<double, 3>& worldUp,
    const std::array<double, 3>& imageViewDir,
    const std::array<double, 3>& imageUp,
    double imageZRotationDegrees
) {
    constexpr int kAzimuthSteps = 72;
    constexpr int kElevationSteps = 24;

    struct RawPatch {
        CreateViewEnvironmentInfo::Patch patch;
        std::array<float, 3> raw{0.0f, 0.0f, 0.0f};
    };

    std::vector<RawPatch> rawPatches;
    rawPatches.reserve(static_cast<size_t>(kAzimuthSteps * kElevationSteps));
    std::vector<float> luminances;
    luminances.reserve(rawPatches.capacity());

    for (int elev = 0; elev < kElevationSteps; ++elev) {
        const double t0 = static_cast<double>(elev) / static_cast<double>(kElevationSteps);
        const double t1 = static_cast<double>(elev + 1) / static_cast<double>(kElevationSteps);
        const double e0 = t0 * (M_PI * 0.5);
        const double e1 = t1 * (M_PI * 0.5);
        for (int az = 0; az < kAzimuthSteps; ++az) {
            const double p0 = static_cast<double>(az) / static_cast<double>(kAzimuthSteps) * (2.0 * M_PI);
            const double p1 = static_cast<double>(az + 1) / static_cast<double>(kAzimuthSteps) * (2.0 * M_PI);
            RawPatch rawPatch;
            rawPatch.patch.directions = {
                createViewDirectionFromAzimuthElevation(worldCenterDir, worldUp, p0, e0),
                createViewDirectionFromAzimuthElevation(worldCenterDir, worldUp, p1, e0),
                createViewDirectionFromAzimuthElevation(worldCenterDir, worldUp, p1, e1),
                createViewDirectionFromAzimuthElevation(worldCenterDir, worldUp, p0, e1),
            };
            const auto worldCenter = createViewDirectionFromAzimuthElevation(worldCenterDir, worldUp, (p0 + p1) * 0.5, (e0 + e1) * 0.5);
            std::array<float, 3> accum{0.0f, 0.0f, 0.0f};
            int sampleCount = 0;
            const auto accumulateSample = [&](const std::array<double, 3>& sampleDir) {
                const auto rgb = sampleCreateViewHdrAngularFisheye(image, sampleDir, imageViewDir, imageUp, imageZRotationDegrees);
                accum[0] += rgb[0];
                accum[1] += rgb[1];
                accum[2] += rgb[2];
                ++sampleCount;
            };
            accumulateSample(worldCenter);
            for (const auto& direction : rawPatch.patch.directions) {
                accumulateSample(direction);
            }
            rawPatch.raw = {
                accum[0] / static_cast<float>(std::max(1, sampleCount)),
                accum[1] / static_cast<float>(std::max(1, sampleCount)),
                accum[2] / static_cast<float>(std::max(1, sampleCount)),
            };
            const float luminance = std::max(0.0f, 0.26507413f * rawPatch.raw[0] + 0.67011463f * rawPatch.raw[1] + 0.06481124f * rawPatch.raw[2]);
            luminances.push_back(luminance);
            rawPatches.push_back(std::move(rawPatch));
        }
    }

    float referenceLum = 1.0f;
    if (!luminances.empty()) {
        auto sorted = luminances;
        const size_t percentileIndex = static_cast<size_t>(std::clamp<double>(std::floor(sorted.size() * 0.80), 0.0, static_cast<double>(sorted.size() - 1)));
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(percentileIndex), sorted.end());
        referenceLum = std::max(0.02f, sorted[percentileIndex]);
    }
    const float exposure = 1.6f / referenceLum;

    std::vector<CreateViewEnvironmentInfo::Patch> patches;
    patches.reserve(rawPatches.size());
    for (auto& rawPatch : rawPatches) {
        CreateViewEnvironmentInfo::Patch patch = rawPatch.patch;
        for (int c = 0; c < 3; ++c) {
            const float scaled = std::max(0.0f, rawPatch.raw[c] * exposure);
            patch.color[static_cast<size_t>(c)] = std::pow(1.0f - std::exp(-scaled), 1.0f / 2.0f);
        }
        patch.alpha = 0.90f;
        patches.push_back(std::move(patch));
    }
    return patches;
}

CreateViewEnvironmentInfo parseCreateViewEnvironment(const std::string& spec, const fs::path& workingDir) {
    CreateViewEnvironmentInfo info;
    const auto value = normalizedTextBoxValue(spec);
    info.sourceLabel = value.empty() ? "--" : fs::path(value).filename().string();
    if (value.empty()) {
        info.note = "Choose a 180° fisheye HDR environment map or a simple Radiance .rad source file.";
        return info;
    }

    fs::path path{value};
    // error_code overloads: an odd or very long entry must not throw out of the refresh.
    std::error_code pathError;
    if (path.is_relative()) {
        const fs::path resolved = fs::weakly_canonical(workingDir / path, pathError);
        path = pathError ? workingDir / path : resolved;
    }
    if (!fs::exists(path, pathError)) {
        info.note = "Environment HDR file not found.";
        return info;
    }

    try {
        if (createViewLower(path.extension().string()) == ".rad") {
            const std::string rawText = readFile(path);
            const std::string envText = sanitizeCreateViewEnvironmentRadText(rawText);
            std::array<float, 3> skyColor{1.0f, 1.0f, 1.0f};
            info.sourcePath = path;
            info.sourceLabel = path.filename().string();
            info.hasView = false;
            info.direction = {0.0, -1.0, 0.0};
            info.up = {0.0, 0.0, 1.0};
            info.vh = 180.0;
            info.vv = 180.0;
            info.projectionLabel = "Radiance source hemisphere";
            info.rawSkySource = envText;
            info.rawSolarSource.clear();

            if (!parseCreateViewEnvironmentSourcesFromRadText(
                    envText,
                    info.skyDirection,
                    skyColor,
                    info.sunDirection,
                    info.sunSolidAngle,
                    info.sunColor,
                    info.hasSkySource,
                    info.hasSun)) {
                info.note = "Could not parse Radiance environment source file.";
                return info;
            }
            if (info.hasSkySource) {
                info.patches = buildCreateViewUniformEnvironmentPatches(info.skyDirection, info.up, skyColor, 0.90f);
            }
            info.valid = info.hasSkySource || info.hasSun;
            info.note = info.valid
                ? "Radiance environment source parsed for the preview hemisphere"
                : "Could not parse Radiance environment source file";
            return info;
        } else {
            const HdrImage image = readRadianceHDR(path.string());
            info.valid = true;
            info.hasImage = true;
            info.sourcePath = path;
            info.sourceLabel = path.filename().string();
            info.width = image.width;
            info.height = image.height;
            info.headerLines = image.headerLines;
            info.orientation = image.header.contains("ORIENTATION") ? image.header.at("ORIENTATION") : "";
            info.skyDescription = image.header.contains("SKYDESC") ? image.header.at("SKYDESC") : "";
            if (image.header.contains("HASDIRECTSUN")) {
                const std::string hasDirect = createViewLower(image.header.at("HASDIRECTSUN"));
                info.hasDirectSun = (hasDirect == "true" || hasDirect == "1" || hasDirect == "yes");
            }

            const std::string viewLine = createViewHeaderValue(image, "VIEW");
            if (!viewLine.empty()) {
                CreateViewViewInfo parsedView;
                auto tokens = splitWhitespace(viewLine);
                if (!tokens.empty() && parseCreateViewViewTokens(tokens, parsedView)) {
                    info.hasView = true;
                    info.direction = createViewVec3Normalize(parsedView.direction, {0.0, -1.0, 0.0});
                    info.up = createViewVec3Normalize(parsedView.up, {0.0, 0.0, 1.0});
                    info.vh = parsedView.vh;
                    info.vv = parsedView.vv;
                    info.projectionLabel = parsedView.projectionLabel;
                }
            }
            if (!info.hasView) {
                info.direction = {0.0, -1.0, 0.0};
                info.up = {0.0, 0.0, 1.0};
                info.projectionLabel = "Assumed angular fisheye";
            }

            // SKYSOURCE / SOLARSOURCE header text is copied into the scene that oconv reads, and oconv runs
            // a '!' word as a shell command. Real source descriptions never contain '!', so such text
            // in a picture's header is ignored instead of being run.
            std::string skySource = createViewHeaderValue(image, "SKYSOURCE");
            std::string solarSource = createViewHeaderValue(image, "SOLARSOURCE");
            std::vector<std::string> ignoredSources;
            if (skySource.find('!') != std::string::npos) {
                ignoredSources.push_back("SKYSOURCE");
                skySource.clear();
            }
            if (solarSource.find('!') != std::string::npos) {
                ignoredSources.push_back("SOLARSOURCE");
                solarSource.clear();
            }
            if (!ignoredSources.empty()) {
                info.note = std::format("Ignored the {} header text: it contains '!', which oconv would run as a command.",
                                        ignoredSources.size() == 2 ? std::string("SKYSOURCE and SOLARSOURCE") : ignoredSources.front());
            }
            if (!skySource.empty()) {
                info.rawSkySource = skySource;
                info.imageZRotationDegrees = parseCreateViewInlineRotateZDegrees(skySource);
                double skyOmega = 0.0;
                info.hasSkySource = parseCreateViewEnvironmentSourceFromHeader(skySource, info.skyDirection, skyOmega, nullptr);
            }
            if (!solarSource.empty()) {
                info.rawSolarSource = solarSource;
                info.hasSun = parseCreateViewEnvironmentSourceFromHeader(solarSource, info.sunDirection, info.sunSolidAngle, &info.sunColor);
            }

            info.patches = buildCreateViewEnvironmentPatches(
                image,
                info.hasSkySource ? info.skyDirection : info.direction,
                info.up,
                info.direction,
                info.up,
                info.imageZRotationDegrees
            );
            const std::string ignoredNote = info.note;
            if (info.patches.empty()) {
                info.note = "Environment header was parsed, but preview patches could not be generated.";
            } else {
                info.note = "HDR fisheye projected onto a hemisphere using header view orientation.";
            }
            if (!ignoredNote.empty()) {
                info.note += " " + ignoredNote;
            }
        }
    } catch (const std::exception& e) {
        info.valid = false;
        info.note = std::string("Could not parse environment: ") + e.what();
    }

    return info;
}

std::string buildCreateViewEnvironmentRadText(const CreateViewEnvironmentInfo& environmentInfo, const fs::path& fisheyeCalPath) {
    if (!environmentInfo.valid) {
        return {};
    }
    // A path is one word in a scene file: Radiance's fgetword() reads a quoted word, so a path with
    // spaces is quoted (unquoted, "/Users/me/Lighting Study/sky.hdr" split the colorpict arguments).
    const auto radianceWord = [](const std::string& word) {
        if (word.find_first_of(" \t") == std::string::npos) {
            return word;
        }
        return word.find('"') == std::string::npos ? "\"" + word + "\"" : "'" + word + "'";
    };
    const std::string fisheyeCal = radianceWord(fisheyeCalPath.empty() ? std::string("fisheye.cal") : fisheyeCalPath.string());

    // A .rad environment keeps its lines (commands and comments end at the line end), and its relative
    // file names are made absolute from the .rad's folder, as oconv runs in the room folder. Before,
    // the file was joined into one line and its first .hdr name was replaced by the .rad file itself.
    if (createViewLower(environmentInfo.sourcePath.extension().string()) == ".rad") {
        const fs::path radFolder = environmentInfo.sourcePath.parent_path();
        std::ostringstream out;
        std::istringstream in(environmentInfo.rawSkySource);
        std::string line;
        while (std::getline(in, line)) {
            const std::string trimmed = trim(line);
            if (trimmed.empty() || trimmed.front() == '!' || trimmed.front() == '#') {
                out << line << "\n";
                continue;
            }
            std::vector<std::string> words;
            {
                std::string word;
                char quote = '\0';
                bool inWord = false;
                for (const char c : trimmed) {
                    if (quote != '\0' && c == quote) {
                        quote = '\0';
                    } else if (quote == '\0' && (c == '"' || c == '\'')) {
                        quote = c;
                        inWord = true;
                    } else if (quote == '\0' && std::isspace(static_cast<unsigned char>(c))) {
                        if (inWord) {
                            words.push_back(word);
                            word.clear();
                            inWord = false;
                        }
                    } else {
                        word += c;
                        inWord = true;
                    }
                }
                if (inWord) {
                    words.push_back(word);
                }
            }
            for (size_t i = 0; i < words.size(); ++i) {
                std::string word = words[i];
                const fs::path wordPath{word};
                std::error_code existsError;
                if (!word.empty() && word.front() != '-' && wordPath.has_extension() && wordPath.is_relative() &&
                    fs::exists(radFolder / wordPath, existsError)) {
                    word = (radFolder / wordPath).lexically_normal().string();
                } else if (word == "fisheye.cal") {
                    out << (i > 0 ? " " : "") << fisheyeCal;
                    continue;
                }
                out << (i > 0 ? " " : "") << radianceWord(word);
            }
            out << "\n";
        }
        return out.str();
    }

    auto rewriteInlineSource = [&](const std::string& text) {
        auto tokens = splitWhitespace(text);
        bool replacedHdr = false;
        for (auto& token : tokens) {
            const auto lower = createViewLower(token);
            if (!replacedHdr && lower.size() >= 4 && lower.substr(lower.size() - 4) == ".hdr") {
                token = radianceWord(environmentInfo.sourcePath.string());
                replacedHdr = true;
            } else if (token == "fisheye.cal") {
                token = fisheyeCal;
            }
        }
        std::ostringstream joined;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (i > 0) {
                joined << ' ';
            }
            joined << tokens[i];
        }
        return joined.str();
    };

    std::ostringstream out;
    if (!environmentInfo.rawSolarSource.empty()) {
        out << environmentInfo.rawSolarSource << "\n";
    }

    if (!environmentInfo.rawSkySource.empty()) {
        out << rewriteInlineSource(environmentInfo.rawSkySource) << "\n";
    } else {
        const auto& skyDir = environmentInfo.hasSkySource
            ? environmentInfo.skyDirection
            : environmentInfo.direction;
        out << "void colorpict imgfunc\n";
        out << "9 red green blue " << radianceWord(environmentInfo.sourcePath.string()) << " " << fisheyeCal << " fish_u fish_v -rz 180\n";
        out << "0\n";
        out << "0\n\n";
        out << "imgfunc glow imgglow\n";
        out << "0\n";
        out << "0\n";
        out << "4 1 1 1 0\n\n";
        out << "imgglow source sky\n";
        out << "0\n";
        out << "0\n";
        out << "4 " << skyDir[0] << " " << skyDir[1] << " " << skyDir[2] << " 180\n";
    }
    return out.str();
}

} // namespace hdrspace::createview
