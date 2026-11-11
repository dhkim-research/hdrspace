#include "SceneFile.h"

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

#if defined(_WIN32)
#include "platform/WinPosix.h"
#else
#include <glob.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace hdrspace::createview {

using namespace hdrspace::text;

bool isCreateViewGeometryType(const std::string& type) {
    static const std::set<std::string> kTypes{
        "polygon", "sphere", "bubble", "cylinder", "tube", "cone", "cup", "ring", "instance", "mesh"
    };
    return kTypes.count(createViewLower(type)) > 0;
}

std::string inferCreateViewSemanticLabel(const std::string& identifier, const std::string& modifier, const std::string& materialType) {
    const std::string combined = createViewLower(identifier + " " + modifier + " " + materialType);
    // A frame or sill next to a window is not glazing, and "panel" is not a "pane" (window_frame,
    // wall_panel and ceiling_panel were labelled Windows and counted in the window FoV).
    if (createViewNameLooksGlazing(combined) && !createViewNameLooksFrame(combined)) {
        return "Windows";
    }
    if (combined.find("door") != std::string::npos) {
        return "Doors";
    }
    if (combined.find("floor") != std::string::npos || combined.find("slab") != std::string::npos ||
        combined.find("ground") != std::string::npos) {
        return "Floors";
    }
    if (combined.find("ceil") != std::string::npos) {
        return "Ceilings";
    }
    if (combined.find("roof") != std::string::npos) {
        return "Roofs";
    }
    if (combined.find("wall") != std::string::npos || combined.find("partition") != std::string::npos) {
        return "Walls";
    }
    if (combined.find("chair") != std::string::npos || combined.find("desk") != std::string::npos ||
        combined.find("table") != std::string::npos || combined.find("cabinet") != std::string::npos ||
        combined.find("sofa") != std::string::npos || combined.find("bed") != std::string::npos ||
        combined.find("furniture") != std::string::npos) {
        return "Furniture";
    }
    if (combined.find("column") != std::string::npos || combined.find("beam") != std::string::npos ||
        combined.find("frame") != std::string::npos || combined.find("struct") != std::string::npos) {
        return "Structure";
    }
    return "Other";
}

CreateViewPreviewState::Surface resolveCreateViewSurfaceStyle(
    const std::string& identifier,
    const std::string& modifier,
    const std::string& primitiveType,
    const std::map<std::string, CreateViewMaterialInfo>& materials
) {
    CreateViewPreviewState::Surface surface;
    surface.identifier = identifier;
    surface.modifier = modifier;
    surface.type = primitiveType;

    CreateViewMaterialInfo material;
    auto it = materials.find(modifier);
    if (it != materials.end()) {
        material = it->second;
    }
    const std::string materialType = createViewLower(material.type);
    surface.semanticLabel = inferCreateViewSemanticLabel(identifier, modifier, materialType);

    if (surface.semanticLabel == "Walls") {
        surface.color = {0.76f, 0.80f, 0.86f};
    } else if (surface.semanticLabel == "Windows") {
        surface.color = {0.50f, 0.77f, 0.95f};
        surface.alpha = 0.18f;
        surface.transparent = true;
    } else if (surface.semanticLabel == "Doors") {
        surface.color = {0.62f, 0.42f, 0.28f};
    } else if (surface.semanticLabel == "Floors") {
        surface.color = {0.54f, 0.50f, 0.46f};
    } else if (surface.semanticLabel == "Ceilings") {
        surface.color = {0.88f, 0.88f, 0.86f};
    } else if (surface.semanticLabel == "Roofs") {
        surface.color = {0.50f, 0.35f, 0.31f};
    } else if (surface.semanticLabel == "Furniture") {
        surface.color = {0.78f, 0.63f, 0.42f};
    } else if (surface.semanticLabel == "Structure") {
        surface.color = {0.56f, 0.66f, 0.78f};
    }

    if (material.numericArgs.size() >= 3) {
        surface.color = {
            static_cast<float>(std::clamp(material.numericArgs[0], 0.0, 1.0)),
            static_cast<float>(std::clamp(material.numericArgs[1], 0.0, 1.0)),
            static_cast<float>(std::clamp(material.numericArgs[2], 0.0, 1.0)),
        };
    }

    if (materialType == "glass" || materialType == "dielectric" || materialType == "trans" ||
        materialType == "trans2" || materialType == "bsdf" || materialType == "mixfunc") {
        surface.transparent = true;
        const double trans = material.numericArgs.size() >= 3
            ? (material.numericArgs[0] + material.numericArgs[1] + material.numericArgs[2]) / 3.0
            : 0.75;
        surface.alpha = static_cast<float>(std::clamp(0.30 - trans * 0.18, 0.08, 0.34));
        if (surface.semanticLabel == "Other") {
            surface.semanticLabel = "Windows";
        }
    } else if (materialType == "mirror") {
        surface.color = {0.78f, 0.82f, 0.88f};
        surface.alpha = 0.30f;
    } else if (materialType == "metal" || materialType == "metal2") {
        surface.alpha = 0.86f;
    } else if (materialType == "light" || materialType == "glow") {
        surface.color = {1.0f, 0.86f, 0.52f};
        surface.alpha = 0.92f;
    }

    return surface;
}

void createViewAddPointSample(
    CreateViewRoomInfo& info,
    const std::array<double, 3>& position,
    const std::array<float, 3>& color,
    float alpha
) {
    constexpr size_t kMaxPointSamples = 14000;
    if (info.pointCloud.size() >= kMaxPointSamples) {
        const size_t index = (info.pointCloud.size() * 2654435761u) % kMaxPointSamples;
        info.pointCloud[index] = {position, color, alpha};
        return;
    }
    info.pointCloud.push_back({position, color, alpha});
}

void refreshCreateViewSurfaceStyles(CreateViewRoomInfo& info) {
    info.semanticCounts.clear();
    for (auto& surface : info.surfaces) {
        const auto refreshed = resolveCreateViewSurfaceStyle(surface.identifier, surface.modifier, surface.type, info.materials);
        surface.semanticLabel = refreshed.semanticLabel;
        surface.color = refreshed.color;
        surface.alpha = refreshed.alpha;
        surface.transparent = refreshed.transparent;
        ++info.semanticCounts[surface.semanticLabel];
    }
    for (auto& sample : info.pointCloud) {
        sample.alpha = std::clamp(sample.alpha, 0.08f, 0.9f);
    }
}

std::vector<fs::path> expandCreateViewPaths(const fs::path& baseDir, const std::string& pattern) {
    std::vector<fs::path> out;
    fs::path candidate = fs::path(pattern);
    if (candidate.is_relative()) {
        candidate = baseDir / candidate;
    }
    // A path that exists is used as it is, and only the include argument decides on a pattern:
    // brackets in a folder above the scene ("Office [v2]") were read as a glob class.
    std::error_code existsError;
    if (fs::exists(candidate, existsError)) {
        out.push_back(candidate);
        return out;
    }
    if (pattern.find_first_of("*?[") != std::string::npos) {
#if defined(_WIN32)
        for (const fs::path& match : hdrspaceWildcardPaths(candidate)) {
            out.push_back(match);
        }
#else
        std::string globPattern;
        if (fs::path(pattern).is_relative()) {
            for (const char ch : baseDir.string()) {
                if (ch == '*' || ch == '?' || ch == '[' || ch == ']' || ch == '\\') {
                    globPattern += '\\';
                }
                globPattern += ch;
            }
            globPattern += "/" + pattern;
        } else {
            globPattern = pattern;
        }
        glob_t matches{};
        if (glob(globPattern.c_str(), GLOB_TILDE, nullptr, &matches) == 0) {
            for (size_t i = 0; i < matches.gl_pathc; ++i) {
                out.emplace_back(matches.gl_pathv[i]);
            }
        }
        globfree(&matches);
#endif
    } else {
        out.push_back(candidate);
    }
    return out;
}

void parseCreateViewPrimitiveTokens(const std::vector<std::string>& tokens, const fs::path& sourcePath, const CreateViewTransform& transform, CreateViewRoomInfo& info) {
    size_t i = 0;
    while (i + 6 <= tokens.size()) {
        const std::string modifier = tokens[i++];
        const std::string type = tokens[i++];
        const std::string identifier = tokens[i++];

        int n1 = 0;
        int n2 = 0;
        int n3 = 0;
        // Counts must be non-negative and the words they announce must exist: a negative count
        // wrapped the size_t bound, and a primitive cut off after its strings read past the end.
        if (!tryParseInteger(tokens[i++], n1) || n1 < 0 || i + static_cast<size_t>(n1) > tokens.size()) {
            break;
        }
        std::vector<std::string> sargs1(tokens.begin() + static_cast<std::ptrdiff_t>(i),
                                        tokens.begin() + static_cast<std::ptrdiff_t>(i + static_cast<size_t>(n1)));
        i += static_cast<size_t>(n1);
        if (i >= tokens.size() || !tryParseInteger(tokens[i++], n2) || n2 < 0 || i + static_cast<size_t>(n2) > tokens.size()) {
            break;
        }
        std::vector<std::string> sargs2(tokens.begin() + static_cast<std::ptrdiff_t>(i),
                                        tokens.begin() + static_cast<std::ptrdiff_t>(i + static_cast<size_t>(n2)));
        i += static_cast<size_t>(n2);
        if (i >= tokens.size() || !tryParseInteger(tokens[i++], n3) || n3 < 0 || i + static_cast<size_t>(n3) > tokens.size()) {
            break;
        }

        std::vector<std::string> sargs3(tokens.begin() + static_cast<std::ptrdiff_t>(i),
                                        tokens.begin() + static_cast<std::ptrdiff_t>(i + static_cast<size_t>(n3)));
        std::vector<double> args;
        args.reserve(std::max(0, n3));
        for (int j = 0; j < n3; ++j) {
            double valueToken = 0.0;
            if (tryParseFloating(tokens[i + static_cast<size_t>(j)], valueToken)) {
                args.push_back(valueToken);
            } else {
                args.clear();
                break;
            }
        }
        i += static_cast<size_t>(n3);

        if (!isCreateViewGeometryType(type)) {
            CreateViewMaterialInfo material;
            material.name = identifier;
            material.modifier = modifier;
            material.type = type;
            material.sargs1 = std::move(sargs1);
            material.sargs2 = std::move(sargs2);
            material.sargs3 = std::move(sargs3);
            material.numericArgs = args;
            material.sourcePath = sourcePath;
            info.materials[identifier] = std::move(material);
            continue;
        }

        ++info.primitiveCount;
        if (!modifier.empty() && modifier != "void" && !info.modifierSourcePaths.contains(modifier)) {
            info.modifierSourcePaths.emplace(modifier, sourcePath);
        }
        if (type == "polygon" && !args.empty() && args.size() % 3 == 0) {
            ++info.polygonCount;
            auto surface = resolveCreateViewSurfaceStyle(identifier, modifier, type, info.materials);
            for (size_t j = 0; j + 2 < args.size(); j += 3) {
                growCreateViewBoundsTransformed(info, transform, args[j], args[j + 1], args[j + 2]);
                const auto p = createViewApply(transform, args[j], args[j + 1], args[j + 2]);
                surface.vertices.push_back(p);
            }
            ++info.semanticCounts[surface.semanticLabel];
            info.surfaces.push_back(std::move(surface));
        } else if ((type == "sphere" || type == "bubble") && args.size() >= 4) {
            const double r = std::abs(args[3]);
            for (double sx : {-r, r}) {
                for (double sy : {-r, r}) {
                    for (double sz : {-r, r}) {
                        growCreateViewBoundsTransformed(info, transform, args[0] + sx, args[1] + sy, args[2] + sz);
                    }
                }
            }
            auto surface = resolveCreateViewSurfaceStyle(identifier, modifier, type, info.materials);
            for (int lat = -2; lat <= 2; ++lat) {
                const double phi = static_cast<double>(lat) * (M_PI / 10.0);
                const double z = std::sin(phi) * r;
                const double ringR = std::cos(phi) * r;
                for (int step = 0; step < 12; ++step) {
                    const double angle = static_cast<double>(step) * (M_PI / 6.0);
                    const auto p = createViewApply(transform, args[0] + std::cos(angle) * ringR, args[1] + std::sin(angle) * ringR, args[2] + z);
                    if (lat == 0) {
                        surface.vertices.push_back(p);
                    }
                    createViewAddPointSample(info, p, surface.color, surface.transparent ? 0.26f : 0.5f);
                }
            }
            ++info.semanticCounts[surface.semanticLabel];
            info.surfaces.push_back(std::move(surface));
        } else if ((type == "cylinder" || type == "tube") && args.size() >= 7) {
            const double r = std::abs(args[6]);
            auto surface = resolveCreateViewSurfaceStyle(identifier, modifier, type, info.materials);
            const std::array<double, 3> a{args[0], args[1], args[2]};
            const std::array<double, 3> b{args[3], args[4], args[5]};
            for (int endpoint = 0; endpoint < 2; ++endpoint) {
                const double cx = endpoint == 0 ? a[0] : b[0];
                const double cy = endpoint == 0 ? a[1] : b[1];
                const double cz = endpoint == 0 ? a[2] : b[2];
                for (double sx : {-r, r}) {
                    for (double sy : {-r, r}) {
                        for (double sz : {-r, r}) {
                            growCreateViewBoundsTransformed(info, transform, cx + sx, cy + sy, cz + sz);
                        }
                    }
                }
            }
            const std::array<double, 3> axis{
                b[0] - a[0], b[1] - a[1], b[2] - a[2]
            };
            const double axisLen = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
            const std::array<double, 3> tangent = axisLen > 1e-9
                ? std::array<double, 3>{axis[0] / axisLen, axis[1] / axisLen, axis[2] / axisLen}
                : std::array<double, 3>{0.0, 0.0, 1.0};
            std::array<double, 3> ref = std::abs(tangent[2]) > 0.8 ? std::array<double, 3>{0.0, 1.0, 0.0} : std::array<double, 3>{0.0, 0.0, 1.0};
            std::array<double, 3> u{
                tangent[1] * ref[2] - tangent[2] * ref[1],
                tangent[2] * ref[0] - tangent[0] * ref[2],
                tangent[0] * ref[1] - tangent[1] * ref[0],
            };
            const double uLen = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            if (uLen > 1e-9) {
                u = {u[0] / uLen, u[1] / uLen, u[2] / uLen};
            }
            const std::array<double, 3> v{
                tangent[1] * u[2] - tangent[2] * u[1],
                tangent[2] * u[0] - tangent[0] * u[2],
                tangent[0] * u[1] - tangent[1] * u[0],
            };
            for (int ring = 0; ring < 3; ++ring) {
                const double t = ring / 2.0;
                const std::array<double, 3> c{
                    a[0] + axis[0] * t, a[1] + axis[1] * t, a[2] + axis[2] * t
                };
                for (int step = 0; step < 12; ++step) {
                    const double angle = static_cast<double>(step) * (M_PI / 6.0);
                    const std::array<double, 3> p3{
                        c[0] + (u[0] * std::cos(angle) + v[0] * std::sin(angle)) * r,
                        c[1] + (u[1] * std::cos(angle) + v[1] * std::sin(angle)) * r,
                        c[2] + (u[2] * std::cos(angle) + v[2] * std::sin(angle)) * r,
                    };
                    const auto p = createViewApply(transform, p3[0], p3[1], p3[2]);
                    if (ring != 1) {
                        surface.vertices.push_back(p);
                    }
                    createViewAddPointSample(info, p, surface.color, surface.transparent ? 0.24f : 0.48f);
                }
            }
            ++info.semanticCounts[surface.semanticLabel];
            info.surfaces.push_back(std::move(surface));
        } else if ((type == "cone" || type == "cup") && args.size() >= 8) {
            auto surface = resolveCreateViewSurfaceStyle(identifier, modifier, type, info.materials);
            for (int endpoint = 0; endpoint < 2; ++endpoint) {
                const double cx = args[static_cast<size_t>(endpoint * 3 + 0)];
                const double cy = args[static_cast<size_t>(endpoint * 3 + 1)];
                const double cz = args[static_cast<size_t>(endpoint * 3 + 2)];
                const double r = std::abs(args[static_cast<size_t>(6 + endpoint)]);
                for (double sx : {-r, r}) {
                    for (double sy : {-r, r}) {
                        for (double sz : {-r, r}) {
                            growCreateViewBoundsTransformed(info, transform, cx + sx, cy + sy, cz + sz);
                        }
                    }
                }
                const auto p = createViewApply(transform, cx, cy, cz);
                surface.vertices.push_back(p);
                createViewAddPointSample(info, p, surface.color, surface.transparent ? 0.24f : 0.48f);
            }
            for (int endpoint = 0; endpoint < 2; ++endpoint) {
                const double cx = args[static_cast<size_t>(endpoint * 3 + 0)];
                const double cy = args[static_cast<size_t>(endpoint * 3 + 1)];
                const double cz = args[static_cast<size_t>(endpoint * 3 + 2)];
                const double r = std::abs(args[static_cast<size_t>(6 + endpoint)]);
                if (r <= 1e-6) {
                    continue;
                }
                for (int step = 0; step < 12; ++step) {
                    const double angle = static_cast<double>(step) * (M_PI / 6.0);
                    const auto p = createViewApply(transform, cx + std::cos(angle) * r, cy + std::sin(angle) * r, cz);
                    createViewAddPointSample(info, p, surface.color, surface.transparent ? 0.22f : 0.42f);
                }
            }
            ++info.semanticCounts[surface.semanticLabel];
            info.surfaces.push_back(std::move(surface));
        }
    }
}

void parseCreateViewRoomRecursive(const fs::path& path, const CreateViewTransform& transform, std::set<fs::path>& activePaths, CreateViewRoomInfo& info) {
    std::error_code pathError;
    if (!fs::is_regular_file(path, pathError)) {
        return;
    }
    fs::path resolved = fs::weakly_canonical(path, pathError);
    if (pathError) {
        resolved = path;
    }
    if (!activePaths.insert(resolved).second) {
        return;
    }

    std::ifstream input(resolved);
    if (!input) {
        activePaths.erase(resolved);
        return;
    }
    info.parsedPaths.insert(resolved);

    // Read like Radiance readobj(): at each object boundary a '#' starts a comment to the end of the
    // line and a '!' a command line; an object is modifier, type, identifier and three counted argument
    // lists, except "alias", which has one reference word. Words are read like fgetword() (quotes).
    // The previous reader skipped only lines starting with '#', so a comment after a primitive ended
    // the reading of the file without a note.
    std::ostringstream fileText;
    fileText << input.rdbuf();
    const std::string text = fileText.str();
    size_t pos = 0;
    const auto nextWord = [&text, &pos](std::string& word) -> bool {
        word.clear();
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
        if (pos >= text.size()) {
            return false;
        }
        char quote = '\0';
        while (pos < text.size()) {
            const char c = text[pos];
            if (quote != '\0' && c == quote) {
                quote = '\0';
            } else if (quote == '\0' && (c == '"' || c == '\'')) {
                quote = c;
            } else if (quote == '\0' && std::isspace(static_cast<unsigned char>(c))) {
                break;
            } else {
                word += c;
            }
            ++pos;
        }
        return true;
    };

    std::vector<std::string> primitiveTokens;
    std::vector<std::pair<std::string, std::string>> aliases;
    size_t objectIndex = 0;
    while (true) {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
        if (pos >= text.size()) {
            break;
        }
        if (text[pos] == '#') {
            const size_t end = text.find('\n', pos);
            pos = end == std::string::npos ? text.size() : end;
            continue;
        }
        if (text[pos] != '!') {
            std::vector<std::string> objectTokens(3);
            if (!nextWord(objectTokens[0]) || !nextWord(objectTokens[1]) || !nextWord(objectTokens[2])) {
                info.parseWarnings.push_back(std::format("{}: incomplete primitive at the end of the file", resolved.filename().string()));
                break;
            }
            ++objectIndex;
            if (objectTokens[1] == "alias") {
                std::string reference;
                if (!nextWord(reference)) {
                    info.parseWarnings.push_back(std::format("{}: alias {} has no reference", resolved.filename().string(), objectTokens[2]));
                    break;
                }
                aliases.emplace_back(objectTokens[2], reference);
                continue;
            }
            bool complete = true;
            for (int list = 0; list < 3 && complete; ++list) {
                std::string countWord;
                int count = 0;
                if (!nextWord(countWord) || !tryParseInteger(countWord, count) || count < 0) {
                    complete = false;
                    break;
                }
                objectTokens.push_back(countWord);
                for (int k = 0; k < count && complete; ++k) {
                    std::string word;
                    complete = nextWord(word);
                    objectTokens.push_back(word);
                }
            }
            if (!complete) {
                info.parseWarnings.push_back(std::format("{}: stopped at primitive {} ({} {} {}): its argument counts could not be read",
                                                         resolved.filename().string(), objectIndex, objectTokens[0], objectTokens[1], objectTokens[2]));
                break;
            }
            primitiveTokens.insert(primitiveTokens.end(), objectTokens.begin(), objectTokens.end());
            continue;
        }
        const size_t lineEnd = text.find('\n', pos);
        const std::string line = trim(text.substr(pos, lineEnd == std::string::npos ? std::string::npos : lineEnd - pos));
        pos = lineEnd == std::string::npos ? text.size() : lineEnd;

        ++info.commandCount;
        const auto commandTokens = splitShellLike(line.substr(1));
        if (commandTokens.empty()) {
            continue;
        }

        if (commandTokens[0] == "include" && commandTokens.size() >= 2) {
            ++info.includeCount;
            for (size_t ti = 1; ti < commandTokens.size(); ++ti) {
                for (const auto& child : expandCreateViewPaths(resolved.parent_path(), commandTokens[ti])) {
                    parseCreateViewRoomRecursive(child, transform, activePaths, info);
                }
            }
            continue;
        }

        if (commandTokens[0] == "xform") {
            ++info.xformCount;
            CreateViewTransform localOps;
            std::vector<std::string> childSpecs;
            for (size_t ti = 1; ti < commandTokens.size(); ++ti) {
                const std::string& token = commandTokens[ti];
                auto prependOp = [&](const CreateViewTransform& op) {
                    localOps = createViewTransformMultiply(op, localOps);
                };
                if (token == "-t" && ti + 3 < commandTokens.size()) {
                    double tx = 0.0, ty = 0.0, tz = 0.0;
                    if (tryParseFloating(commandTokens[ti + 1], tx) &&
                        tryParseFloating(commandTokens[ti + 2], ty) &&
                        tryParseFloating(commandTokens[ti + 3], tz)) {
                        prependOp(createViewTranslate(tx, ty, tz));
                        ti += 3;
                        continue;
                    }
                } else if (token == "-s" && ti + 1 < commandTokens.size()) {
                    double s = 1.0;
                    if (tryParseFloating(commandTokens[ti + 1], s)) {
                        prependOp(createViewScale(s, s, s));
                        ++ti;
                        continue;
                    }
                } else if (token == "-mx") {
                    prependOp(createViewScale(-1.0, 1.0, 1.0));
                    continue;
                } else if (token == "-my") {
                    prependOp(createViewScale(1.0, -1.0, 1.0));
                    continue;
                } else if (token == "-mz") {
                    prependOp(createViewScale(1.0, 1.0, -1.0));
                    continue;
                } else if (token == "-rx" && ti + 1 < commandTokens.size()) {
                    double angle = 0.0;
                    if (tryParseFloating(commandTokens[ti + 1], angle)) {
                        prependOp(createViewRotateX(angle));
                        ++ti;
                        continue;
                    }
                } else if (token == "-ry" && ti + 1 < commandTokens.size()) {
                    double angle = 0.0;
                    if (tryParseFloating(commandTokens[ti + 1], angle)) {
                        prependOp(createViewRotateY(angle));
                        ++ti;
                        continue;
                    }
                } else if (token == "-rz" && ti + 1 < commandTokens.size()) {
                    double angle = 0.0;
                    if (tryParseFloating(commandTokens[ti + 1], angle)) {
                        prependOp(createViewRotateZ(angle));
                        ++ti;
                        continue;
                    }
                }
                if (!token.empty() && token[0] != '-') {
                    childSpecs.push_back(token);
                }
            }
            const CreateViewTransform combined = createViewTransformMultiply(transform, localOps);
            for (const auto& childSpec : childSpecs) {
                for (const auto& child : expandCreateViewPaths(resolved.parent_path(), childSpec)) {
                    parseCreateViewRoomRecursive(child, combined, activePaths, info);
                }
            }
        }
    }

    parseCreateViewPrimitiveTokens(primitiveTokens, resolved, transform, info);
    // An alias names the material it refers to (after this file's definitions, as Radiance reads them
    // in order; a reference that is not known is left to the synthesis of missing materials).
    for (const auto& [name, reference] : aliases) {
        const auto referenceIt = info.materials.find(reference);
        if (referenceIt == info.materials.end()) {
            continue;
        }
        CreateViewMaterialInfo alias = referenceIt->second;
        alias.name = name;
        alias.sourcePath = resolved;
        alias.synthesized = false;
        alias.aliasOf = reference;
        info.materials[name] = std::move(alias);
    }
    activePaths.erase(resolved);
}

std::set<std::string> collectCreateViewUnresolvedModifiers(const CreateViewRoomInfo& info) {
    std::set<std::string> unresolved;
    for (const auto& surface : info.surfaces) {
        if (surface.modifier.empty() || surface.modifier == "void") {
            continue;
        }
        if (!info.materials.contains(surface.modifier)) {
            unresolved.insert(surface.modifier);
        }
    }
    return unresolved;
}

std::vector<fs::path> discoverCreateViewSupplementalMaterialFiles(const fs::path& roomPath, const CreateViewRoomInfo& info) {
    const auto unresolved = collectCreateViewUnresolvedModifiers(info);
    if (unresolved.empty()) {
        return {};
    }

    std::vector<fs::path> out;
    std::set<fs::path> seen;
    const fs::path canonicalRoomPath = fs::weakly_canonical(roomPath);
    const fs::path roomDir = canonicalRoomPath.parent_path();
    const fs::path projectDir = roomDir.parent_path();

    auto addFile = [&](const fs::path& candidate) {
        if (candidate.empty() || !fs::exists(candidate) || !fs::is_regular_file(candidate)) {
            return;
        }
        if (createViewLower(candidate.extension().string()) != ".rad") {
            return;
        }
        std::error_code ec;
        const fs::path canonical = fs::weakly_canonical(candidate, ec);
        const fs::path normalized = ec ? candidate : canonical;
        if (normalized == canonicalRoomPath || !seen.insert(normalized).second) {
            return;
        }
        out.push_back(normalized);
    };

    auto addDirFiles = [&](const fs::path& dir) {
        if (dir.empty() || !fs::exists(dir) || !fs::is_directory(dir)) {
            return;
        }
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                addFile(entry.path());
            }
        }
    };

    addFile(roomDir / "materials.rad");
    addFile(roomDir / "solskin_materials.rad");
    addFile(projectDir / "materials.rad");
    addFile(projectDir / "solskin_materials.rad");
    addDirFiles(roomDir / "MATERIAL");
    addDirFiles(projectDir / "MATERIAL");

    return out;
}

std::vector<fs::path> collectCreateViewRenderDependencyPaths(const CreateViewRoomInfo& roomInfo, const fs::path& roomPath) {
    std::vector<fs::path> out;
    std::set<fs::path> seen;
    std::set<std::string> referencedModifiers;
    for (const auto& surface : roomInfo.surfaces) {
        if (!surface.modifier.empty() && surface.modifier != "void") {
            referencedModifiers.insert(surface.modifier);
        }
    }

    std::error_code roomEc;
    const fs::path canonicalRoomPath = fs::weakly_canonical(roomPath, roomEc);
    const fs::path normalizedRoomPath = roomEc ? roomPath : canonicalRoomPath;

    auto addPath = [&](const fs::path& candidate) {
        if (candidate.empty() || !fs::exists(candidate) || !fs::is_regular_file(candidate)) {
            return;
        }
        std::error_code ec;
        const fs::path canonical = fs::weakly_canonical(candidate, ec);
        const fs::path normalized = ec ? candidate : canonical;
        // Files the room loads itself (through !include / !xform, with their transforms) must not be
        // given to oconv again: only the supplemental material files the room does not load.
        if (normalized == normalizedRoomPath || roomInfo.roomLoadedPaths.contains(normalized) ||
                !seen.insert(normalized).second) {
            return;
        }
        out.push_back(normalized);
    };

    for (const auto& modifierName : referencedModifiers) {
        const auto materialIt = roomInfo.materials.find(modifierName);
        if (materialIt != roomInfo.materials.end()) {
            addPath(materialIt->second.sourcePath);
        }
        const auto modifierIt = roomInfo.modifierSourcePaths.find(modifierName);
        if (modifierIt != roomInfo.modifierSourcePaths.end()) {
            addPath(modifierIt->second);
        }
    }

    return out;
}

void synthesizeCreateViewReferencedMaterials(CreateViewRoomInfo& info) {
    for (const auto& surface : info.surfaces) {
        if (surface.modifier.empty() || surface.modifier == "void") {
            continue;
        }
        if (info.materials.contains(surface.modifier)) {
            continue;
        }

        CreateViewMaterialInfo material;
        material.name = surface.modifier;
        material.modifier = "void";
        material.type = surface.transparent ? "glass" : "plastic";
        material.sourcePath = info.modifierSourcePaths.contains(surface.modifier)
            ? info.modifierSourcePaths[surface.modifier]
            : fs::path{};
        material.numericArgs = {
            std::clamp(static_cast<double>(surface.color[0]), 0.0, 1.0),
            std::clamp(static_cast<double>(surface.color[1]), 0.0, 1.0),
            std::clamp(static_cast<double>(surface.color[2]), 0.0, 1.0),
            0.0,
            0.15
        };
        if (surface.transparent) {
            material.numericArgs.resize(3);
        }
        material.synthesized = true;
        info.materials[material.name] = std::move(material);
    }
}

CreateViewRoomInfo parseCreateViewRoom(const std::string& spec, const fs::path& workingDir) {
    CreateViewRoomInfo info;
    const auto value = normalizedTextBoxValue(spec);
    info.sourceLabel = value.empty() ? "--" : fs::path(value).filename().string();
    if (value.empty()) {
        info.note = "Choose a room .rad file.";
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
        info.note = "Room file not found.";
        return info;
    }

    std::set<fs::path> activePaths;
    parseCreateViewRoomRecursive(path, CreateViewTransform{}, activePaths, info);
    info.roomLoadedPaths = info.parsedPaths;
    const auto supplementalMaterialFiles = discoverCreateViewSupplementalMaterialFiles(path, info);
    for (const auto& materialFile : supplementalMaterialFiles) {
        std::set<fs::path> materialActivePaths;
        parseCreateViewRoomRecursive(materialFile, CreateViewTransform{}, materialActivePaths, info);
    }
    // Collected before the synthesis, which defines every missing material (afterwards the set was
    // always empty and the stand-ins were never reported).
    const auto unresolvedBeforeSynthesis = collectCreateViewUnresolvedModifiers(info);
    synthesizeCreateViewReferencedMaterials(info);
    refreshCreateViewSurfaceStyles(info);

    info.valid = info.hasBounds || info.primitiveCount > 0 || info.commandCount > 0;
    if (!supplementalMaterialFiles.empty()) {
        info.note = std::format("Preview auto-loaded {} nearby material file(s).", supplementalMaterialFiles.size());
    } else if (info.includeCount > 0 || info.xformCount > 0) {
        info.note = "Preview applied direct !xform / !include paths. Other generator commands are still skipped.";
    } else if (!info.hasBounds) {
        info.note = "No directly bounded geometry found in this .rad file.";
    } else {
        info.note = "Room extents were estimated from direct Radiance primitives.";
    }
    if (!unresolvedBeforeSynthesis.empty()) {
        std::string names;
        size_t listed = 0;
        for (const auto& name : unresolvedBeforeSynthesis) {
            if (listed == 5) {
                names += ", ...";
                break;
            }
            names += (listed == 0 ? "" : ", ") + name;
            ++listed;
        }
        info.note += std::format(" {} material(s) not found ({}): grey or glass stand-ins are used in the preview and render.",
                                 unresolvedBeforeSynthesis.size(), names);
    }
    for (const auto& warning : info.parseWarnings) {
        info.note += " Reading stopped early: " + warning + ".";
    }
    return info;
}

fs::path writeCreateViewRenderRoomWrapper(const CreateViewRoomInfo& roomInfo, const std::map<std::string, CreateViewMaterialOverride>& materialOverrides, const fs::path& roomPath, const fs::path& wrapperPath, bool includeTemporaryOverrides) {
    std::ofstream out(wrapperPath);
    if (!out) {
        throw std::runtime_error("Could not write temporary room wrapper .rad file.");
    }

    out << "# hdrspace temporary Create View wrapper\n";

    for (const auto& [name, material] : roomInfo.materials) {
        const auto overrideIt = includeTemporaryOverrides ? materialOverrides.find(name) : materialOverrides.end();
        if (includeTemporaryOverrides && overrideIt != materialOverrides.end()) {
            out << formatCreateViewMaterialBlock(material, &overrideIt->second) << "\n\n";
        } else if (material.synthesized) {
            out << formatCreateViewMaterialBlock(material) << "\n\n";
        }
    }
    std::ifstream roomIn(roomPath);
    if (!roomIn) {
        throw std::runtime_error("Could not read room .rad file for temporary wrapper.");
    }
    out << "\n# original room content follows\n";
    out << roomIn.rdbuf();
    if (out.tellp() > 0) {
        out << "\n";
    }
    return wrapperPath;
}

size_t writeCreateViewWindowMaskScene(const CreateViewRoomInfo& roomInfo, const fs::path& scenePath) {
    std::ofstream out(scenePath);
    if (!out) {
        throw std::runtime_error("Could not write temporary window-mask scene.");
    }

    out << "# hdrspace temporary window mask scene\n\n";
    out << "void plastic cv_mask_black\n";
    out << "0\n";
    out << "0\n";
    out << "5 0 0 0 0 0\n\n";
    out << "void glow cv_mask_white\n";
    out << "0\n";
    out << "0\n";
    out << "4 1 1 1 0\n\n";

    size_t writtenPolygonCount = 0;
    size_t whitePolygonCount = 0;
    for (const auto& surface : roomInfo.surfaces) {
        if (createViewLower(surface.type) != "polygon" || surface.vertices.size() < 3) {
            continue;
        }

        const bool isWindowMaskSurface = createViewSurfaceCountsAsMaskWindow(surface, roomInfo);
        if (isWindowMaskSurface) {
            ++whitePolygonCount;
        }

        out << (isWindowMaskSurface ? "cv_mask_white" : "cv_mask_black")
            << " polygon cv_mask_poly_" << writtenPolygonCount << "\n";
        out << "0\n";
        out << "0\n";
        out << surface.vertices.size() * 3 << "\n";
        for (const auto& vertex : surface.vertices) {
            out << vertex[0] << " " << vertex[1] << " " << vertex[2] << "\n";
        }
        out << "\n";
        ++writtenPolygonCount;
    }

    if (writtenPolygonCount == 0) {
        throw std::runtime_error("The parsed room did not yield any polygon geometry for the temporary window mask.");
    }
    if (whitePolygonCount == 0) {
        throw std::runtime_error("No likely glazing/window polygons were found for the temporary window mask.");
    }

    return whitePolygonCount;
}

bool createViewTypeMatchesSemantic(const std::string& semanticLabel, const std::string& objectType) {
    const std::string semantic = createViewLower(semanticLabel);
    const std::string object = createViewLower(objectType);
    if (semantic.empty() || object.empty()) {
        return false;
    }
    if (semantic == "windows") {
        return object.find("window") != std::string::npos || object.find("glass") != std::string::npos ||
            object.find("glaz") != std::string::npos || object.find("blind") != std::string::npos ||
            object.find("shade") != std::string::npos || object.find("curtain") != std::string::npos;
    }
    if (semantic == "doors") {
        return object.find("door") != std::string::npos;
    }
    if (semantic == "floors") {
        return object.find("floor") != std::string::npos || object.find("carpet") != std::string::npos ||
            object.find("tile") != std::string::npos;
    }
    if (semantic == "ceilings") {
        return object.find("ceiling") != std::string::npos || object.find("acoustic") != std::string::npos;
    }
    if (semantic == "walls") {
        return object.find("wall") != std::string::npos || object.find("paint") != std::string::npos ||
            object.find("partition") != std::string::npos;
    }
    if (semantic == "furniture") {
        return object.find("chair") != std::string::npos || object.find("desk") != std::string::npos ||
            object.find("table") != std::string::npos || object.find("furniture") != std::string::npos ||
            object.find("cabinet") != std::string::npos;
    }
    if (semantic == "structure") {
        return object.find("column") != std::string::npos || object.find("beam") != std::string::npos ||
            object.find("struct") != std::string::npos;
    }
    return false;
}

bool createViewRadianceTypesAreCompatible(const std::string& lhs, const std::string& rhs) {
    const std::string a = createViewLower(lhs);
    const std::string b = createViewLower(rhs);
    if (a.empty() || b.empty()) {
        return false;
    }
    if (a == b) {
        return true;
    }
    const auto isGlassLike = [](const std::string& value) {
        return value.find("glass") != std::string::npos || value.find("trans") != std::string::npos || value.find("dielectric") != std::string::npos;
    };
    const auto isMetalLike = [](const std::string& value) {
        return value.find("metal") != std::string::npos;
    };
    if (isGlassLike(a) && isGlassLike(b)) {
        return true;
    }
    if (isMetalLike(a) && isMetalLike(b)) {
        return true;
    }
    return false;
}

std::string formatCreateViewMaterialBlock(const CreateViewMaterialInfo& material, const CreateViewMaterialOverride* overrideValue) {
    const std::string modifier = (overrideValue && !overrideValue->modifier.empty()) ? overrideValue->modifier
        : (material.modifier.empty() ? "void" : material.modifier);
    const std::string type = (overrideValue && !overrideValue->radianceType.empty()) ? overrideValue->radianceType : material.type;
    const std::vector<double>* numericArgs = overrideValue ? nullptr : &material.numericArgs;
    std::array<double, 5> overrideArgs{};
    if (overrideValue) {
        overrideArgs = overrideValue->numericArgs;
    }

    auto buildArgsLine = [&]() -> std::string {
        const std::string loweredType = createViewLower(type);
        const bool isGlassLike = loweredType == "glass";
        std::ostringstream oss;
        oss << (isGlassLike ? 3 : 5);
        if (overrideValue) {
            const int count = isGlassLike ? 3 : 5;
            for (int i = 0; i < count; ++i) {
                oss << " " << std::format("{:.4f}", overrideArgs[static_cast<size_t>(i)]);
            }
            return oss.str();
        }
        if (!numericArgs || numericArgs->empty()) {
            oss << (isGlassLike ? " 0.7000 0.7000 0.7000" : " 0.7000 0.7000 0.7000 0.0000 0.1500");
            return oss.str();
        }
        const size_t count = isGlassLike ? std::min<size_t>(3, numericArgs->size()) : std::min<size_t>(5, numericArgs->size());
        for (size_t i = 0; i < count; ++i) {
            oss << " " << std::format("{:.4f}", (*numericArgs)[i]);
        }
        return oss.str();
    };

    std::ostringstream block;
    block << modifier << " " << type << " " << material.name << "\n";
    block << "0\n";
    block << "0\n";
    block << buildArgsLine();
    return block.str();
}

std::string formatCreateViewMaterialArgLine(const CreateViewMaterialInfo& material, const CreateViewMaterialOverride* overrideValue) {
    const std::string block = formatCreateViewMaterialBlock(material, overrideValue);
    const size_t lastBreak = block.find_last_of('\n');
    if (lastBreak == std::string::npos) {
        return block;
    }
    return block.substr(lastBreak + 1);
}

bool parseCreateViewMaterialOverrideText(const CreateViewMaterialInfo& material, const std::string& text, CreateViewMaterialOverride& overrideValue, std::string& error) {
    std::string normalized = text;
    std::replace(normalized.begin(), normalized.end(), '\n', ' ');
    std::replace(normalized.begin(), normalized.end(), '\r', ' ');
    const auto tokens = splitWhitespace(normalized);
    if (tokens.empty()) {
        error = "Paste a Radiance argument line such as `5 r g b spec rough` or a full primitive definition.";
        return false;
    }

    overrideValue = {};
    overrideValue.modifier = material.modifier;
    overrideValue.radianceType = material.type;
    overrideValue.numericArgs = {0.7, 0.7, 0.7, 0.0, 0.15};

    auto parseArgLine = [&](size_t startIndex, int count, const std::string& typeHint) -> bool {
        if (startIndex + static_cast<size_t>(count) >= tokens.size()) {
            return false;
        }
        std::array<double, 5> parsed{0.7, 0.7, 0.7, 0.0, 0.15};
        for (int i = 0; i < count; ++i) {
            double value = 0.0;
            if (!tryParseFloating(tokens[startIndex + 1 + static_cast<size_t>(i)], value)) {
                return false;
            }
            if (i < 5) {
                parsed[static_cast<size_t>(i)] = value;
            }
        }
        overrideValue.numericArgs = parsed;
        if (!typeHint.empty()) {
            overrideValue.radianceType = typeHint;
        } else if (count == 3) {
            overrideValue.radianceType = "glass";
        }
        const std::string loweredType = createViewLower(overrideValue.radianceType);
        if (loweredType.find("glass") != std::string::npos || loweredType.find("trans") != std::string::npos || loweredType.find("dielectric") != std::string::npos) {
            overrideValue.transparent = true;
            overrideValue.previewAlpha = 0.18f;
            if (count == 3) {
                overrideValue.radianceType = "glass";
            }
        } else if (loweredType.find("metal") != std::string::npos) {
            overrideValue.previewAlpha = 0.86f;
        } else {
            overrideValue.previewAlpha = 0.88f;
        }
        overrideValue.previewColor = {
            static_cast<float>(std::clamp(overrideValue.numericArgs[0], 0.0, 1.0)),
            static_cast<float>(std::clamp(overrideValue.numericArgs[1], 0.0, 1.0)),
            static_cast<float>(std::clamp(overrideValue.numericArgs[2], 0.0, 1.0)),
        };
        return true;
    };

    int count = 0;
    if (tryParseInteger(tokens[0], count)) {
        if ((count == 3 || count == 5) && parseArgLine(0, count, material.type)) {
            return true;
        }
        error = "The pasted argument line should start with `3` or `5` followed by numeric values.";
        return false;
    }

    if (tokens.size() >= 6) {
        const std::string modifier = tokens[0];
        const std::string type = tokens[1];
        size_t index = 3;
        int n1 = 0;
        int n2 = 0;
        int n3 = 0;
        if (!tryParseInteger(tokens[index++], n1)) {
            error = "Could not parse the full Radiance primitive. Try pasting only the last numeric line.";
            return false;
        }
        index += static_cast<size_t>(std::max(0, n1));
        if (index >= tokens.size() || !tryParseInteger(tokens[index++], n2)) {
            error = "Could not parse the full Radiance primitive. Try pasting only the last numeric line.";
            return false;
        }
        index += static_cast<size_t>(std::max(0, n2));
        if (index >= tokens.size() || !tryParseInteger(tokens[index], n3)) {
            error = "Could not parse the full Radiance primitive. Try pasting only the last numeric line.";
            return false;
        }
        overrideValue.modifier = modifier;
        if ((n3 == 3 || n3 == 5) && parseArgLine(index, n3, type)) {
            return true;
        }
    }

    error = "Paste either a final numeric line like `5 r g b spec rough` or a full Radiance primitive definition.";
    return false;
}

bool rewriteCreateViewMaterialDefinition(const fs::path& filePath, const CreateViewMaterialInfo& material, const CreateViewMaterialOverride& overrideValue, std::string& error) {
    std::ifstream input(filePath);
    if (!input) {
        error = std::format("Could not open {} for editing.", filePath.string());
        return false;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    input.close();

    if (!material.aliasOf.empty()) {
        error = std::format("{} is an alias of {} in {}; edit {} instead.", material.name, material.aliasOf,
                            filePath.string(), material.aliasOf);
        return false;
    }

    // Write a sibling file first and rename it over the original, so a failed write never leaves the
    // scene file cut short.
    auto writeLinesAtomically = [&](const std::vector<std::string>& outputLines) -> bool {
        fs::path temporaryPath = filePath;
        temporaryPath += ".hdrspace-tmp";
        {
            std::ofstream output(temporaryPath, std::ios::trunc);
            if (!output) {
                error = std::format("Could not write {}.", filePath.string());
                return false;
            }
            for (const std::string& outputLine : outputLines) {
                output << outputLine << "\n";
            }
            output.close();
            if (!output) {
                std::error_code removeError;
                fs::remove(temporaryPath, removeError);
                error = std::format("Could not write {}.", filePath.string());
                return false;
            }
        }
        std::error_code renameError;
        fs::rename(temporaryPath, filePath, renameError);
        if (renameError) {
            std::error_code removeError;
            fs::remove(temporaryPath, removeError);
            error = std::format("Could not replace {}: {}", filePath.string(), renameError.message());
            return false;
        }
        return true;
    };

    auto buildBlock = [&]() {
        std::ostringstream block;
        const std::string modifier = overrideValue.modifier.empty() ? (material.modifier.empty() ? "void" : material.modifier) : overrideValue.modifier;
        const std::string type = overrideValue.radianceType.empty() ? material.type : overrideValue.radianceType;
        block << modifier << " " << type << " " << material.name << "\n";
        block << "0\n0\n";
        if (createViewLower(type) == "glass") {
            block << std::format("3 {:.6f} {:.6f} {:.6f}", overrideValue.numericArgs[0], overrideValue.numericArgs[1], overrideValue.numericArgs[2]);
            block << "\n";
        } else {
            block << std::format("5 {:.6f} {:.6f} {:.6f} {:.6f} {:.6f}",
                overrideValue.numericArgs[0],
                overrideValue.numericArgs[1],
                overrideValue.numericArgs[2],
                overrideValue.numericArgs[3],
                overrideValue.numericArgs[4]
            ) << "\n";
        }
        return block.str();
    };

    {
        // The editor writes plastic/metal (5 values) and glass (3 values); other types have other
        // argument layouts and would be damaged by that block.
        const std::string outputType = createViewLower(overrideValue.radianceType.empty() ? material.type : overrideValue.radianceType);
        if (outputType != "plastic" && outputType != "metal" && outputType != "glass") {
            error = std::format("Write to source supports plastic, metal and glass definitions; {} uses another argument layout.",
                                overrideValue.radianceType.empty() ? material.type : overrideValue.radianceType);
            return false;
        }
    }

    for (size_t start = 0; start < lines.size(); ++start) {
        const auto head = splitWhitespace(trim(lines[start]));
        if (head.size() < 3) {
            continue;
        }
        if (head[2] != material.name || createViewLower(head[1]) != createViewLower(material.type)) {
            continue;
        }

        // Radiance allows each argument count and its values on any lines; usually the values follow
        // the count on the same line ("5 .5 .5 .5 0 0"). Walk the tokens after the name across
        // lines, so exactly this primitive's lines are replaced and the next primitive is kept.
        std::vector<std::string> lineTokens(head.begin() + 3, head.end());
        size_t lineIndex = start;
        size_t tokenIndex = 0;
        auto nextToken = [&](std::string& token) -> bool {
            while (tokenIndex >= lineTokens.size()) {
                if (lineIndex + 1 >= lines.size()) {
                    return false;
                }
                ++lineIndex;
                const std::string trimmed = trim(lines[lineIndex]);
                lineTokens = (!trimmed.empty() && trimmed[0] == '#') ? std::vector<std::string>{} : splitWhitespace(trimmed);
                tokenIndex = 0;
            }
            token = lineTokens[tokenIndex++];
            return true;
        };
        bool parsed = true;
        for (int section = 0; section < 3 && parsed; ++section) {
            std::string countToken;
            int count = 0;
            if (!nextToken(countToken) || !tryParseInteger(countToken, count) || count < 0) {
                parsed = false;
                break;
            }
            for (int i = 0; i < count && parsed; ++i) {
                std::string skipped;
                parsed = nextToken(skipped);
            }
        }
        if (!parsed) {
            continue;
        }
        if (tokenIndex < lineTokens.size()) {
            error = std::format("The definition of {} in {} shares a line with other text; edit it by hand.", material.name, filePath.string());
            return false;
        }
        const size_t pos = lineIndex + 1;

        const std::string replacement = buildBlock();
        std::vector<std::string> replacementLines;
        std::istringstream replacementStream(replacement);
        std::string replacementLine;
        while (std::getline(replacementStream, replacementLine)) {
            replacementLines.push_back(replacementLine);
        }
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(start), lines.begin() + static_cast<std::ptrdiff_t>(pos));
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(start), replacementLines.begin(), replacementLines.end());

        return writeLinesAtomically(lines);
    }

    // No definition in this file (a material that had to be stood in for): it is inserted before the
    // first primitive that uses it, because Radiance needs a modifier defined before its use. Appended
    // at the end of the file it came after the surfaces, and the old material stayed in effect.
    for (size_t index = 0; index < lines.size(); ++index) {
        const auto head = splitWhitespace(trim(lines[index]));
        if (head.size() < 3 || head[0] != material.name) {
            continue;
        }
        std::vector<std::string> blockLines;
        std::istringstream blockStream(buildBlock());
        std::string blockLine;
        while (std::getline(blockStream, blockLine)) {
            blockLines.push_back(blockLine);
        }
        blockLines.emplace_back();
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index), blockLines.begin(), blockLines.end());
        return writeLinesAtomically(lines);
    }
    error = std::format("Neither a definition of {} nor a primitive using it was found in {}; nothing was written.",
                        material.name, filePath.string());
    return false;
}

} // namespace hdrspace::createview
