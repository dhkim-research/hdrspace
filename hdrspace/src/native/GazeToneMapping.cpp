#include "native/GazeToneMapping.h"

#include <tev/Channel.h>
#include <tev/imageio/Colors.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <format>
#include <limits>
#include <numeric>
#include <optional>
#include <ranges>
#include <sstream>

namespace native {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLuminanceFloor = 1e-8;
constexpr double kWhiteEfficacy = 179.0;
constexpr std::array<double, 3> kRec709LuminanceWeights{0.2126, 0.7152, 0.0722};
constexpr std::array<double, 3> kRadianceLuminanceWeights{0.26507413, 0.67011464, 0.06481124};

std::string lowerCopy(std::string_view text) {
    std::string result{text};
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::optional<std::string_view> findAttributeValue(
    std::span<const tev::AttributeNode> nodes,
    std::string_view name
) {
    for (const auto& node : nodes) {
        if (node.name == name) {
            return node.value;
        }
        if (const auto child = findAttributeValue(node.children, name)) {
            return child;
        }
    }
    return std::nullopt;
}

void findAttributeValues(
    std::span<const tev::AttributeNode> nodes,
    std::string_view name,
    std::vector<std::string_view>& result
) {
    for (const auto& node : nodes) {
        if (node.name == name) {
            result.push_back(node.value);
        }
        findAttributeValues(node.children, name, result);
    }
}

std::vector<double> parseFloatList(std::string_view value) {
    std::vector<double> result;
    std::istringstream input{std::string{value}};
    double parsed = 0.0;
    while (input >> parsed) {
        result.push_back(parsed);
    }
    return result;
}

bool validWeights(const std::array<double, 3>& weights) {
    const double sum = weights[0] + weights[1] + weights[2];
    return std::isfinite(sum) && sum > 1e-8 &&
        std::ranges::all_of(weights, [](double value) {
            return std::isfinite(value) && value >= 0.0;
        });
}

nanogui::Matrix3f primariesToXyzMatrix(const tev::chroma_t& chroma) {
    nanogui::Matrix3f pxyz{1.0f};
    for (int c = 0; c < 3; ++c) {
        const float x = chroma[static_cast<size_t>(c)].x();
        const float y = chroma[static_cast<size_t>(c)].y();
        const float z = 1.0f - x - y;
        pxyz.m[static_cast<size_t>(c)][0] = x;
        pxyz.m[static_cast<size_t>(c)][1] = y;
        pxyz.m[static_cast<size_t>(c)][2] = z;
    }

    nanogui::Vector3f white;
    white[0] = chroma[3].x() / chroma[3].y();
    white[1] = 1.0f;
    white[2] = (1.0f - chroma[3].x() - chroma[3].y()) / chroma[3].y();
    const nanogui::Vector3f scales = inverse(pxyz) * white;

    nanogui::Matrix3f result = pxyz;
    for (int c = 0; c < 3; ++c) {
        result.m[static_cast<size_t>(c)][0] *= scales[c];
        result.m[static_cast<size_t>(c)][1] *= scales[c];
        result.m[static_cast<size_t>(c)][2] *= scales[c];
    }
    return result;
}

std::optional<std::array<double, 3>> weightsFromChroma(const tev::chroma_t& chroma) {
    for (const auto& point : chroma) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
            point.x() <= 0.0f || point.y() <= 0.0f) {
            return std::nullopt;
        }
    }

    const auto matrix = primariesToXyzMatrix(chroma);
    std::array<double, 3> weights{
        matrix.m[0][1],
        matrix.m[1][1],
        matrix.m[2][1],
    };
    if (!validWeights(weights)) {
        return std::nullopt;
    }
    return weights;
}

std::optional<tev::chroma_t> parseTargetChroma(std::span<const tev::AttributeNode> nodes) {
    const auto targetPrimaries = findAttributeValue(nodes, "TargetPrimaries");
    const auto targetWhite = findAttributeValue(nodes, "TargetWhitePoint");
    if (targetPrimaries && targetWhite) {
        const auto p = parseFloatList(*targetPrimaries);
        const auto w = parseFloatList(*targetWhite);
        if (p.size() == 6 && w.size() == 2) {
            tev::chroma_t result;
            result[0] = nanogui::Vector2f{static_cast<float>(p[0]), static_cast<float>(p[1])};
            result[1] = nanogui::Vector2f{static_cast<float>(p[2]), static_cast<float>(p[3])};
            result[2] = nanogui::Vector2f{static_cast<float>(p[4]), static_cast<float>(p[5])};
            result[3] = nanogui::Vector2f{static_cast<float>(w[0]), static_cast<float>(w[1])};
            if (weightsFromChroma(result)) {
                return result;
            }
        }
    }

    const auto radiancePrimaries = findAttributeValue(nodes, "PRIMARIES");
    if (!radiancePrimaries) {
        return std::nullopt;
    }
    const auto values = parseFloatList(*radiancePrimaries);
    if (values.size() != 8) {
        return std::nullopt;
    }
    tev::chroma_t result;
    result[0] = nanogui::Vector2f{static_cast<float>(values[0]), static_cast<float>(values[1])};
    result[1] = nanogui::Vector2f{static_cast<float>(values[2]), static_cast<float>(values[3])};
    result[2] = nanogui::Vector2f{static_cast<float>(values[4]), static_cast<float>(values[5])};
    result[3] = nanogui::Vector2f{static_cast<float>(values[6]), static_cast<float>(values[7])};
    if (!weightsFromChroma(result)) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::array<double, 3>> parseHeaderLuminanceWeights(
    std::span<const tev::AttributeNode> nodes
) {
    if (const auto value = findAttributeValue(nodes, "LuminanceRGB")) {
        const auto parsed = parseFloatList(*value);
        if (parsed.size() == 3) {
            std::array<double, 3> weights{parsed[0], parsed[1], parsed[2]};
            if (validWeights(weights)) {
                return weights;
            }
        }
    }

    const auto parseMatrix = [&](std::string_view name) -> std::optional<std::array<double, 3>> {
        const auto value = findAttributeValue(nodes, name);
        if (!value) {
            return std::nullopt;
        }
        const auto parsed = parseFloatList(*value);
        if (parsed.size() != 9) {
            return std::nullopt;
        }
        std::array<double, 3> weights{parsed[3], parsed[4], parsed[5]};
        return validWeights(weights) ? std::optional{weights} : std::nullopt;
    };

    if (const auto sensorWeights = parseMatrix("SENSOR2XYZ")) {
        return sensorWeights;
    }

    if (const auto xyzcamValue = findAttributeValue(nodes, "XYZCAM")) {
        const auto parsed = parseFloatList(*xyzcamValue);
        if (parsed.size() == 9) {
            nanogui::Matrix3f xyzCam{1.0f};
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    xyzCam.m[col][row] = static_cast<float>(parsed[static_cast<size_t>(row * 3 + col)]);
                }
            }
            if (const auto premultValue = findAttributeValue(nodes, "CAM_PREMULTIPLIERS")) {
                const auto premults = parseFloatList(*premultValue);
                if (premults.size() >= 3) {
                    for (int row = 0; row < 3; ++row) {
                        for (int col = 0; col < 3; ++col) {
                            xyzCam.m[col][row] *= static_cast<float>(premults[static_cast<size_t>(row)]);
                        }
                    }
                }
            }
            const auto sensorToXyz = inverse(xyzCam);
            std::array<double, 3> weights{
                sensorToXyz.m[0][1],
                sensorToXyz.m[1][1],
                sensorToXyz.m[2][1],
            };
            if (validWeights(weights)) {
                return weights;
            }
        }
    }

    if (const auto chroma = parseTargetChroma(nodes)) {
        return weightsFromChroma(*chroma);
    }
    return std::nullopt;
}

double exposureCompensationScale(std::span<const tev::AttributeNode> nodes) {
    std::vector<std::string_view> values;
    findAttributeValues(nodes, "EXPOSURE", values);
    double scale = 1.0;
    for (const auto value : values) {
        for (const double parsed : parseFloatList(value)) {
            if (std::isfinite(parsed) && parsed > 0.0) {
                scale /= parsed;
            }
        }
    }
    return scale;
}

std::string radianceFormat(std::span<const tev::AttributeNode> nodes) {
    if (const auto format = findAttributeValue(nodes, "FORMAT")) {
        return lowerCopy(*format);
    }
    return {};
}

void parseView(
    std::string_view viewValue,
    EGazeProjection& projection,
    double& horizontalDegrees,
    double& verticalDegrees
) {
    std::istringstream input{std::string{viewValue}};
    std::vector<std::string> tokens;
    std::string token;
    while (input >> token) {
        tokens.push_back(token);
    }

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == "-vta") {
            projection = EGazeProjection::AngularFisheye;
        } else if (tokens[i] == "-vth") {
            projection = EGazeProjection::HemisphericalFisheye;
        } else if (tokens[i] == "-vtv") {
            projection = EGazeProjection::Perspective;
        } else if (tokens[i] == "-vh" && i + 1 < tokens.size()) {
            const double value = std::atof(tokens[i + 1].c_str());
            if (std::isfinite(value) && value > 0.0) {
                horizontalDegrees = value;
            }
        } else if (tokens[i] == "-vv" && i + 1 < tokens.size()) {
            const double value = std::atof(tokens[i + 1].c_str());
            if (std::isfinite(value) && value > 0.0) {
                verticalDegrees = value;
            }
        }
    }
}

GazeProjectionInfo detectProjection(const tev::Image& image, EGazeProjection projectionOverride) {
    GazeProjectionInfo info;
    info.projection = EGazeProjection::Perspective;
    info.horizontalDegrees = 180.0;
    info.verticalDegrees = 180.0;
    info.source = "Auto fallback";

    EGazeProjection viewProjection = EGazeProjection::Auto;
    if (const auto view = findAttributeValue(image.attributes(), "VIEW")) {
        parseView(*view, viewProjection, info.horizontalDegrees, info.verticalDegrees);
        if (viewProjection != EGazeProjection::Auto) {
            info.projection = viewProjection;
            info.source = "Radiance VIEW";
        }
    }

    if (viewProjection == EGazeProjection::Auto) {
        if (const auto projection = findAttributeValue(image.attributes(), "MERGEHDR_PROJECTION")) {
            const std::string name = lowerCopy(*projection);
            if (name.find("equidistant") != std::string::npos ||
                name.find("equiangular") != std::string::npos ||
                name.find("angular") != std::string::npos) {
                info.projection = EGazeProjection::AngularFisheye;
                info.source = "MERGEHDR_PROJECTION";
            } else if (name.find("hemispherical") != std::string::npos ||
                       name.find("orthographic") != std::string::npos ||
                       name.find("equisolid") != std::string::npos ||
                       name == "vth") {
                info.projection = EGazeProjection::HemisphericalFisheye;
                info.source = "MERGEHDR_PROJECTION";
            }
        }
    }

    if (projectionOverride != EGazeProjection::Auto) {
        info.projection = projectionOverride;
        info.source = "Manual override";
    }
    return info;
}

std::string channelTailLower(std::string_view name) {
    return lowerCopy(tev::Channel::tail(name));
}

struct ChannelSelection {
    const tev::Channel* red = nullptr;
    const tev::Channel* green = nullptr;
    const tev::Channel* blue = nullptr;
    const tev::Channel* monochrome = nullptr;
    const tev::Channel* alpha = nullptr;
    bool usedSourceChannels = false;
    std::string description;
};

struct ResolvedLuminance {
    bool available = false;
    bool useYChannel = false;
    std::array<double, 3> weights = kRec709LuminanceWeights;
    tev::ituth273::ETransfer transfer = tev::ituth273::ETransfer::Linear;
    GazeLuminanceInfo info;
    std::string error;
};

ResolvedLuminance resolveLuminance(
    const tev::Image& image,
    const GazeLuminanceSettings& settings
) {
    ResolvedLuminance result;
    result.info.requestedEncoding = settings.encoding;
    result.info.resolvedEncoding = settings.encoding;
    result.info.usedSourceChannels = image.hasSourceChannels();

    const auto attributes = image.attributes();
    const std::string format = radianceFormat(attributes);
    const double exposureScale = exposureCompensationScale(attributes);

    auto configureRgb = [&](EGazeInputEncoding encoding,
                            const std::array<double, 3>& weights,
                            tev::ituth273::ETransfer transfer,
                            std::string description,
                            bool absolute,
                            double defaultScale,
                            std::string scaleSource) {
        result.available = true;
        result.useYChannel = false;
        result.weights = weights;
        result.transfer = transfer;
        result.info.resolvedEncoding = encoding;
        result.info.description = std::move(description);
        result.info.absoluteLuminance = absolute;
        result.info.luminanceScale = defaultScale;
        result.info.scaleSource = std::move(scaleSource);
    };

    auto configureY = [&](EGazeInputEncoding encoding,
                          std::string description,
                          bool absolute,
                          double defaultScale,
                          std::string scaleSource) {
        result.available = true;
        result.useYChannel = true;
        result.transfer = tev::ituth273::ETransfer::Linear;
        result.info.resolvedEncoding = encoding;
        result.info.description = std::move(description);
        result.info.absoluteLuminance = absolute;
        result.info.luminanceScale = defaultScale;
        result.info.scaleSource = std::move(scaleSource);
    };

    const double radianceScale = kWhiteEfficacy * exposureScale;
    if (settings.encoding == EGazeInputEncoding::Auto) {
        if (format.find("xyze") != std::string::npos) {
            configureY(
                EGazeInputEncoding::RadianceXyze,
                "Radiance XYZE from FORMAT header",
                true,
                radianceScale,
                std::format("179 / EXPOSURE = {:.7g}", radianceScale)
            );
        } else if (format.find("rgbe") != std::string::npos) {
            std::array<double, 3> weights = kRadianceLuminanceWeights;
            std::string source = "standard Radiance RGB primaries";
            if (const auto headerWeights = parseHeaderLuminanceWeights(attributes)) {
                weights = *headerWeights;
                source = findAttributeValue(attributes, "LuminanceRGB")
                    ? "LuminanceRGB header"
                    : "color-matrix/primaries header";
            }
            configureRgb(
                EGazeInputEncoding::RadianceRgb,
                weights,
                tev::ituth273::ETransfer::Linear,
                "Radiance RGBE from FORMAT; " + source,
                true,
                radianceScale,
                std::format("179 / EXPOSURE = {:.7g}", radianceScale)
            );
        } else if (const auto headerWeights = parseHeaderLuminanceWeights(attributes)) {
            configureRgb(
                EGazeInputEncoding::LinearRec709Rgb,
                *headerWeights,
                image.hasSourceChannels()
                    ? image.nativeMetadata().transfer.value_or(tev::ituth273::ETransfer::Linear)
                    : tev::ituth273::ETransfer::Linear,
                "RGB using luminance/color-matrix header",
                false,
                exposureScale,
                exposureScale == 1.0
                    ? "header-relative ×1"
                    : std::format("1 / EXPOSURE = {:.7g}", exposureScale)
            );
        } else if (image.nativeMetadata().chroma.has_value()) {
            const auto weights = image.hasSourceChannels()
                ? weightsFromChroma(*image.nativeMetadata().chroma)
                : std::optional{kRec709LuminanceWeights};
            if (weights) {
                configureRgb(
                    EGazeInputEncoding::LinearRec709Rgb,
                    *weights,
                    image.hasSourceChannels()
                        ? image.nativeMetadata().transfer.value_or(tev::ituth273::ETransfer::Linear)
                        : tev::ituth273::ETransfer::Linear,
                    image.hasSourceChannels()
                        ? "native color profile/transfer"
                        : "viewer-decoded linear Rec.709",
                    false,
                    1.0,
                    "relative ×1"
                );
            }
        }

        if (!result.available) {
            result.error =
                "The header does not define a reliable color encoding. "
                "Choose Radiance RGBE/XYZE, linear RGB, sRGB, CIE XYZ, or Luminance Y manually.";
            return result;
        }
    } else {
        switch (settings.encoding) {
            case EGazeInputEncoding::RadianceRgb:
                configureRgb(
                    settings.encoding,
                    kRadianceLuminanceWeights,
                    tev::ituth273::ETransfer::Linear,
                    "manual Radiance RGBE",
                    true,
                    radianceScale,
                    std::format("179 / EXPOSURE = {:.7g}", radianceScale)
                );
                break;
            case EGazeInputEncoding::RadianceXyze:
                configureY(
                    settings.encoding,
                    "manual Radiance XYZE",
                    true,
                    radianceScale,
                    std::format("179 / EXPOSURE = {:.7g}", radianceScale)
                );
                break;
            case EGazeInputEncoding::LinearRec709Rgb:
                configureRgb(
                    settings.encoding,
                    kRec709LuminanceWeights,
                    tev::ituth273::ETransfer::Linear,
                    "manual linear Rec.709 RGB",
                    false,
                    1.0,
                    "relative ×1"
                );
                break;
            case EGazeInputEncoding::SrgbEncodedRgb:
                configureRgb(
                    settings.encoding,
                    kRec709LuminanceWeights,
                    tev::ituth273::ETransfer::SRGB,
                    "manual sRGB-encoded RGB",
                    false,
                    1.0,
                    "relative ×1 after sRGB decoding"
                );
                break;
            case EGazeInputEncoding::CieXyz:
                configureY(
                    settings.encoding,
                    "manual CIE XYZ; Y is luminance",
                    true,
                    1.0,
                    "Y ×1 cd/m²"
                );
                break;
            case EGazeInputEncoding::LuminanceY:
                configureY(
                    settings.encoding,
                    "manual luminance Y channel",
                    true,
                    1.0,
                    "Y ×1 cd/m²"
                );
                break;
            case EGazeInputEncoding::Auto:
                break;
        }
    }

    if (settings.scaleOverride.has_value()) {
        const double scale = *settings.scaleOverride;
        if (!std::isfinite(scale) || scale <= 0.0) {
            result.available = false;
            result.error = "The luminance scale must be a finite number greater than zero.";
            return result;
        }
        result.info.luminanceScale = scale;
        result.info.absoluteLuminance = true;
        result.info.scaleSource = std::format("manual cd/m² scale ×{:.7g}", scale);
    }
    return result;
}

const tev::Channel* selectedChannel(
    const tev::Image& image,
    std::string_view name,
    bool useSourceChannels
) {
    if (useSourceChannels) {
        if (const auto* source = image.sourceChannel(name)) {
            return source;
        }
    }
    return image.channel(name);
}

ChannelSelection selectChannels(
    const tev::Image& image,
    std::string_view channelGroup,
    const ResolvedLuminance& luminance
) {
    auto resolveNames = [&](std::span<const std::string> names) {
        ChannelSelection selection;
        std::vector<const tev::Channel*> colorChannels;
        selection.usedSourceChannels = luminance.info.usedSourceChannels;
        const tev::Channel* yChannel = nullptr;

        for (const auto& name : names) {
            const tev::Channel* channel = selectedChannel(image, name, selection.usedSourceChannels);
            if (!channel) {
                continue;
            }
            const std::string tail = channelTailLower(name);
            if (tail == "a" || tail == "alpha") {
                selection.alpha = channel;
                continue;
            }
            if (tail == "r" || tail == "red") {
                selection.red = channel;
            } else if (tail == "g" || tail == "green") {
                selection.green = channel;
            } else if (tail == "b" || tail == "blue") {
                selection.blue = channel;
            } else if (tail == "y") {
                yChannel = channel;
            }
            colorChannels.push_back(channel);
        }

        if (luminance.useYChannel) {
            selection.monochrome =
                yChannel ? yChannel : (!colorChannels.empty() ? colorChannels.front() : nullptr);
            selection.description =
                luminance.info.resolvedEncoding == EGazeInputEncoding::RadianceXyze
                ? "XYZE Y channel"
                : (luminance.info.resolvedEncoding == EGazeInputEncoding::CieXyz
                    ? "CIE XYZ Y channel"
                    : "luminance Y channel");
            return selection;
        }

        if (selection.red && selection.green && selection.blue) {
            selection.description = "named RGB channels";
        } else if (colorChannels.size() >= 3) {
            selection.red = colorChannels[0];
            selection.green = colorChannels[1];
            selection.blue = colorChannels[2];
            selection.description = "first three color channels";
        }
        return selection;
    };

    auto usable = [&](const ChannelSelection& selection) {
        return luminance.useYChannel
            ? selection.monochrome != nullptr
            : selection.red && selection.green && selection.blue;
    };

    ChannelSelection selection = resolveNames(image.channelsInGroup(channelGroup));
    if (usable(selection)) {
        return selection;
    }

    // The canvas can retain a synthetic display group such as "R,G,B" that is
    // not one of the image's stored group names. Find the first real compatible
    // group before falling back to the raw source-channel list.
    for (const auto& group : image.channelGroups()) {
        selection = resolveNames(group.channels);
        if (usable(selection)) {
            selection.description += " (compatible image group)";
            return selection;
        }
    }

    if (image.hasSourceChannels()) {
        std::vector<std::string> sourceNames;
        sourceNames.reserve(image.sourceChannels().size());
        for (const auto& channel : image.sourceChannels()) {
            sourceNames.emplace_back(channel.name());
        }
        selection = resolveNames(sourceNames);
        if (usable(selection)) {
            selection.description += " (source-channel fallback)";
        }
    }
    return selection;
}

double angularDistance(const nanogui::Vector3f& a, const nanogui::Vector3f& b) {
    const double dot = std::clamp(
        static_cast<double>(a.x()) * b.x() +
        static_cast<double>(a.y()) * b.y() +
        static_cast<double>(a.z()) * b.z(),
        -1.0,
        1.0
    );
    return std::acos(dot);
}

} // namespace

std::string_view gazeMeteringModeLabel(EGazeMeteringMode mode) {
    switch (mode) {
        case EGazeMeteringMode::UniformAverage: return "Uniform average";
        case EGazeMeteringMode::CenterWeighted: return "Center-weighted";
        case EGazeMeteringMode::GaussianGazeSpot: return "Gaussian gaze spot";
    }
    return "Unknown";
}

std::string_view gazeProjectionLabel(EGazeProjection projection) {
    switch (projection) {
        case EGazeProjection::Auto: return "Auto";
        case EGazeProjection::AngularFisheye: return "Angular fisheye (-vta)";
        case EGazeProjection::HemisphericalFisheye: return "Hemispherical fisheye (-vth)";
        case EGazeProjection::Perspective: return "Perspective / flat";
    }
    return "Unknown";
}

std::string_view gazeInputEncodingLabel(EGazeInputEncoding encoding) {
    switch (encoding) {
        case EGazeInputEncoding::Auto: return "Auto from HDR header";
        case EGazeInputEncoding::RadianceRgb: return "Radiance RGBE";
        case EGazeInputEncoding::RadianceXyze: return "Radiance XYZE";
        case EGazeInputEncoding::LinearRec709Rgb: return "Linear RGB (Rec.709)";
        case EGazeInputEncoding::SrgbEncodedRgb: return "sRGB-encoded RGB";
        case EGazeInputEncoding::CieXyz: return "CIE XYZ";
        case EGazeInputEncoding::LuminanceY: return "Luminance Y";
    }
    return "Unknown";
}

bool GazeAdaptationSampler::directionForPixel(
    nanogui::Vector2i pixel,
    nanogui::Vector3f& direction
) const {
    if (imageSize_.x() <= 0 || imageSize_.y() <= 0) {
        return false;
    }

    const double x = (static_cast<double>(pixel.x()) + 0.5) / imageSize_.x() - 0.5;
    const double y = 0.5 - (static_cast<double>(pixel.y()) + 0.5) / imageSize_.y();
    const double horizontalRadians = projectionInfo_.horizontalDegrees * kPi / 180.0;
    const double verticalRadians = projectionInfo_.verticalDegrees * kPi / 180.0;

    switch (projectionInfo_.projection) {
        case EGazeProjection::AngularFisheye: {
            double u = x * projectionInfo_.horizontalDegrees / 180.0;
            double v = y * projectionInfo_.verticalDegrees / 180.0;
            const double radiusSquared = u * u + v * v;
            if (radiusSquared > 1.0) {
                return false;
            }
            const double radius = std::sqrt(radiusSquared);
            const double z = std::cos(kPi * radius);
            const double scale = radius <= 1e-12 ? kPi : std::sin(kPi * radius) / radius;
            direction = {
                static_cast<float>(u * scale),
                static_cast<float>(z),
                static_cast<float>(v * scale),
            };
            return true;
        }
        case EGazeProjection::HemisphericalFisheye: {
            const double horizontalScale = 2.0 * std::sin(horizontalRadians * 0.5);
            const double verticalScale = 2.0 * std::sin(verticalRadians * 0.5);
            const double u = x * horizontalScale;
            const double v = y * verticalScale;
            const double zSquared = 1.0 - u * u - v * v;
            if (zSquared < 0.0) {
                return false;
            }
            direction = {
                static_cast<float>(u),
                static_cast<float>(std::sqrt(zSquared)),
                static_cast<float>(v),
            };
            return true;
        }
        case EGazeProjection::Perspective:
        case EGazeProjection::Auto: {
            const double safeHorizontal = std::clamp(horizontalRadians, 1e-6, kPi - 1e-6);
            const double safeVertical = std::clamp(verticalRadians, 1e-6, kPi - 1e-6);
            const double u = 2.0 * x * std::tan(safeHorizontal * 0.5);
            const double v = 2.0 * y * std::tan(safeVertical * 0.5);
            const double invLength = 1.0 / std::sqrt(1.0 + u * u + v * v);
            direction = {
                static_cast<float>(u * invLength),
                static_cast<float>(invLength),
                static_cast<float>(v * invLength),
            };
            return true;
        }
    }
    return false;
}

bool GazeAdaptationSampler::rebuild(
    const tev::Image& image,
    std::string_view channelGroup,
    EGazeProjection projectionOverride,
    const GazeLuminanceSettings& luminanceSettings,
    int maximumSamplesPerAxis
) {
    samples_.clear();
    statusMessage_.clear();
    imageSize_ = image.size();
    projectionInfo_ = detectProjection(image, projectionOverride);

    const ResolvedLuminance resolved = resolveLuminance(image, luminanceSettings);
    luminanceInfo_ = resolved.info;
    if (!resolved.available) {
        statusMessage_ = resolved.error;
        return false;
    }

    const ChannelSelection channels = selectChannels(image, channelGroup, resolved);
    luminanceInfo_.usedSourceChannels = channels.usedSourceChannels;
    channelDescription_ = std::format(
        "{}; {}{}",
        channels.description,
        luminanceInfo_.description,
        channels.usedSourceChannels ? "; source channels" : "; viewer-linear channels"
    );
    if ((!channels.monochrome && (!channels.red || !channels.green || !channels.blue)) ||
        imageSize_.x() <= 0 || imageSize_.y() <= 0) {
        statusMessage_ = resolved.useYChannel
            ? "The selected group has no usable Y/luminance channel."
            : "The selected group has no usable three-channel RGB data.";
        return false;
    }

    maximumSamplesPerAxis = std::clamp(maximumSamplesPerAxis, 64, 1024);
    const int step = std::max(
        1,
        static_cast<int>(std::ceil(
            static_cast<double>(std::max(imageSize_.x(), imageSize_.y())) /
            maximumSamplesPerAxis
        ))
    );
    const size_t estimatedSamples =
        static_cast<size_t>((imageSize_.x() + step - 1) / step) *
        static_cast<size_t>((imageSize_.y() + step - 1) / step);
    samples_.reserve(estimatedSamples);

    for (int y = step / 2; y < imageSize_.y(); y += step) {
        for (int x = step / 2; x < imageSize_.x(); x += step) {
            const nanogui::Vector2i pixel{x, y};
            nanogui::Vector3f direction;
            if (!directionForPixel(pixel, direction)) {
                continue;
            }

            if (channels.alpha) {
                const float alpha = channels.alpha->dynamicAt(pixel);
                if (!std::isfinite(alpha) || alpha <= 0.0f) {
                    continue;
                }
            }

            const double alpha = channels.alpha
                ? std::max(1e-12, static_cast<double>(channels.alpha->dynamicAt(pixel)))
                : 1.0;
            double signal = 0.0;
            double peakSignal = 0.0;
            if (channels.monochrome) {
                signal = channels.monochrome->dynamicAt(pixel) / alpha;
                peakSignal = signal;
            } else {
                const double red = tev::ituth273::invTransferComponent(
                    resolved.transfer,
                    static_cast<float>(channels.red->dynamicAt(pixel) / alpha)
                );
                const double green = tev::ituth273::invTransferComponent(
                    resolved.transfer,
                    static_cast<float>(channels.green->dynamicAt(pixel) / alpha)
                );
                const double blue = tev::ituth273::invTransferComponent(
                    resolved.transfer,
                    static_cast<float>(channels.blue->dynamicAt(pixel) / alpha)
                );
                signal =
                    resolved.weights[0] * red +
                    resolved.weights[1] * green +
                    resolved.weights[2] * blue;
                peakSignal = std::max({red, green, blue});
            }

            if (!std::isfinite(signal) || !std::isfinite(peakSignal)) {
                continue;
            }
            signal = std::max(kLuminanceFloor, signal);
            peakSignal = std::max(kLuminanceFloor, peakSignal);
            const double luminance = std::max(
                kLuminanceFloor,
                signal * luminanceInfo_.luminanceScale
            );
            samples_.push_back({
                signal,
                peakSignal,
                luminance,
                std::log(signal),
                std::log(std::max(kLuminanceFloor, luminance)),
                direction,
                pixel,
            });
        }
    }
    if (samples_.empty()) {
        statusMessage_ = "No finite luminance samples remained after decoding the input values.";
        return false;
    }
    statusMessage_ = "Luminance samples ready.";
    return true;
}

LuminanceDistributionResult computeLuminanceDistribution(
    const tev::Image& image,
    std::string_view channelGroup,
    EGazeProjection projectionOverride,
    const GazeLuminanceSettings& luminanceSettings,
    const LuminanceDistributionOptions& options
) {
    LuminanceDistributionResult result;
    result.projection = detectProjection(image, projectionOverride);

    const ResolvedLuminance resolved = resolveLuminance(image, luminanceSettings);
    result.luminance = resolved.info;
    if (!resolved.available) {
        result.statusMessage = resolved.error;
        return result;
    }

    const ChannelSelection channels = selectChannels(image, channelGroup, resolved);
    result.luminance.usedSourceChannels = channels.usedSourceChannels;
    result.channelDescription = std::format(
        "{}; {}{}",
        channels.description,
        result.luminance.description,
        channels.usedSourceChannels ? "; source channels" : "; viewer-linear channels"
    );
    if (!channels.monochrome && (!channels.red || !channels.green || !channels.blue)) {
        result.statusMessage = resolved.useYChannel
            ? "The selected group has no usable Y/luminance channel."
            : "The selected group has no usable three-channel RGB data.";
        return result;
    }

    const nanogui::Vector2i imageSize = image.size();
    if (imageSize.x() <= 0 || imageSize.y() <= 0) {
        result.statusMessage = "The current image has no pixels.";
        return result;
    }

    int cropX = 0;
    int cropY = 0;
    int cropWidth = imageSize.x();
    int cropHeight = imageSize.y();
    if (options.autoCropLeftEye && imageSize.x() == imageSize.y() * 2) {
        cropWidth = imageSize.y();
        result.croppedLeftEye = true;
    }

    const bool fisheyeProjection =
        result.projection.projection == EGazeProjection::AngularFisheye ||
        result.projection.projection == EGazeProjection::HemisphericalFisheye;
    result.excludedOutsideFisheye =
        options.excludeOutsideFisheye &&
        cropWidth == cropHeight &&
        (result.croppedLeftEye || fisheyeProjection);

    const double centerX = static_cast<double>(cropX) +
        (static_cast<double>(cropWidth) - 1.0) * 0.5;
    const double centerY = static_cast<double>(cropY) +
        (static_cast<double>(cropHeight) - 1.0) * 0.5;
    const double radius = (static_cast<double>(std::min(cropWidth, cropHeight)) - 1.0) * 0.5;
    const double radiusSquared = radius * radius;

    std::vector<float> luminances;
    const size_t estimatedPixels = result.excludedOutsideFisheye
        ? static_cast<size_t>(
            std::ceil(kPi * radiusSquared)
        )
        : static_cast<size_t>(cropWidth) * static_cast<size_t>(cropHeight);
    luminances.reserve(estimatedPixels);
    long double luminanceSum = 0.0L;

    for (int y = cropY; y < cropY + cropHeight; ++y) {
        for (int x = cropX; x < cropX + cropWidth; ++x) {
            if (result.excludedOutsideFisheye) {
                const double dx = static_cast<double>(x) - centerX;
                const double dy = static_cast<double>(y) - centerY;
                if (dx * dx + dy * dy > radiusSquared) {
                    continue;
                }
            }

            const nanogui::Vector2i pixel{x, y};
            double alpha = 1.0;
            if (channels.alpha) {
                alpha = static_cast<double>(channels.alpha->dynamicAt(pixel));
                if (!std::isfinite(alpha) || alpha <= 0.0) {
                    continue;
                }
            }
            alpha = std::max(alpha, 1e-12);

            double signal = 0.0;
            if (channels.monochrome) {
                signal = static_cast<double>(channels.monochrome->dynamicAt(pixel)) / alpha;
            } else {
                const double red = tev::ituth273::invTransferComponent(
                    resolved.transfer,
                    static_cast<float>(channels.red->dynamicAt(pixel) / alpha)
                );
                const double green = tev::ituth273::invTransferComponent(
                    resolved.transfer,
                    static_cast<float>(channels.green->dynamicAt(pixel) / alpha)
                );
                const double blue = tev::ituth273::invTransferComponent(
                    resolved.transfer,
                    static_cast<float>(channels.blue->dynamicAt(pixel) / alpha)
                );
                signal =
                    resolved.weights[0] * red +
                    resolved.weights[1] * green +
                    resolved.weights[2] * blue;
            }

            const double luminance = signal * result.luminance.luminanceScale;
            if (!std::isfinite(luminance)) {
                continue;
            }
            const float clamped = static_cast<float>(std::max(0.0, luminance));
            luminances.push_back(clamped);
            luminanceSum += static_cast<long double>(clamped);
        }
    }

    if (luminances.empty()) {
        result.statusMessage = "No finite luminance pixels remained after decoding and masking.";
        return result;
    }

    std::sort(luminances.begin(), luminances.end());
    result.pixelCount = luminances.size();
    result.minimumLuminance = luminances.front();
    result.maximumLuminance = luminances.back();
    result.meanLuminance = static_cast<double>(
        luminanceSum / static_cast<long double>(luminances.size())
    );

    const auto quantileValue = [&](double q) {
        const double bounded = std::clamp(q, 0.0, 1.0);
        const double position = bounded * static_cast<double>(luminances.size() - 1);
        const size_t lower = static_cast<size_t>(std::floor(position));
        const size_t upper = std::min(luminances.size() - 1, lower + 1);
        const double fraction = position - static_cast<double>(lower);
        return static_cast<double>(luminances[lower]) * (1.0 - fraction) +
            static_cast<double>(luminances[upper]) * fraction;
    };

    std::vector<size_t> graphIndices;
    const size_t graphPointCount = std::clamp<size_t>(
        options.graphPointCount,
        2,
        luminances.size()
    );
    graphIndices.reserve(graphPointCount + 16);
    for (size_t i = 0; i < graphPointCount; ++i) {
        const double q = static_cast<double>(i) /
            static_cast<double>(graphPointCount - 1);
        graphIndices.push_back(static_cast<size_t>(
            std::llround(q * static_cast<double>(luminances.size() - 1))
        ));
    }
    for (const double q : {
            0.001, 0.01, 0.1, 0.5, 0.9, 0.95, 0.98,
            0.99, 0.995, 0.999, 0.9995, 0.9999}) {
        graphIndices.push_back(static_cast<size_t>(
            std::llround(q * static_cast<double>(luminances.size() - 1))
        ));
    }
    std::sort(graphIndices.begin(), graphIndices.end());
    graphIndices.erase(
        std::unique(graphIndices.begin(), graphIndices.end()),
        graphIndices.end()
    );
    result.quantile.reserve(graphIndices.size());
    result.luminanceAtQuantile.reserve(graphIndices.size());
    for (const size_t index : graphIndices) {
        result.quantile.push_back(static_cast<float>(
            static_cast<double>(index) /
            static_cast<double>(luminances.size() - 1)
        ));
        result.luminanceAtQuantile.push_back(luminances[index]);
    }

    result.thresholds.reserve(options.thresholds.size());
    for (const double threshold : options.thresholds) {
        const auto first = std::lower_bound(
            luminances.begin(),
            luminances.end(),
            static_cast<float>(threshold)
        );
        const size_t count = static_cast<size_t>(luminances.end() - first);
        result.thresholds.push_back({
            threshold,
            count,
            static_cast<double>(count) / static_cast<double>(luminances.size()),
        });
    }

    result.tails.reserve(options.tailFractions.size());
    for (const double requestedFraction : options.tailFractions) {
        const double topFraction = std::clamp(requestedFraction, 0.0, 1.0);
        const size_t count = std::clamp<size_t>(
            static_cast<size_t>(
                std::ceil(topFraction * static_cast<double>(luminances.size()))
            ),
            1,
            luminances.size()
        );
        const size_t firstIndex = luminances.size() - count;
        const long double tailSum = std::accumulate(
            luminances.begin() + static_cast<std::ptrdiff_t>(firstIndex),
            luminances.end(),
            0.0L
        );
        result.tails.push_back({
            topFraction,
            quantileValue(1.0 - topFraction),
            luminanceSum > 0.0L
                ? static_cast<double>(tailSum / luminanceSum)
                : 0.0,
        });
    }

    result.ok = true;
    result.statusMessage = std::format(
        "{} luminance pixels ready{}{}.",
        result.pixelCount,
        result.croppedLeftEye ? "; left 180-degree eye" : "",
        result.excludedOutsideFisheye ? "; fisheye corners excluded" : ""
    );
    return result;
}

GazeAdaptationResult GazeAdaptationSampler::evaluate(const GazeAdaptationOptions& options) const {
    GazeAdaptationResult result;
    result.projection = projectionInfo_;
    result.luminance = luminanceInfo_;
    result.channelDescription = channelDescription_;
    result.gazePoint = options.gazePoint;
    result.sampleCount = samples_.size();
    if (samples_.empty()) {
        result.statusMessage = "No usable HDR luminance samples.";
        return result;
    }

    nanogui::Vector3f referenceDirection{0.0f, 1.0f, 0.0f};
    double sigmaRadians = options.centerSigmaDegrees * kPi / 180.0;
    if (options.meteringMode == EGazeMeteringMode::GaussianGazeSpot) {
        if (!directionForPixel(options.gazePoint, referenceDirection)) {
            result.statusMessage = "The gaze point is outside the valid projection.";
            return result;
        }
        sigmaRadians = options.gaussianSigmaDegrees * kPi / 180.0;
    } else if (options.meteringMode == EGazeMeteringMode::CenterWeighted) {
        const nanogui::Vector2i center{imageSize_.x() / 2, imageSize_.y() / 2};
        if (!directionForPixel(center, referenceDirection)) {
            result.statusMessage = "Could not resolve the image center direction.";
            return result;
        }
    }
    sigmaRadians = std::max(0.1 * kPi / 180.0, sigmaRadians);

    const auto displayDirectionForPixel = [&](const nanogui::Vector2i& pixel) {
        const auto& display = options.displayGeometry;
        const double fitScale = std::min(
            static_cast<double>(display.pixelWidth) / std::max(1, imageSize_.x()),
            static_cast<double>(display.pixelHeight) / std::max(1, imageSize_.y())
        );
        const double millimetresPerDisplayPixel = 25.4 / display.pixelsPerInch;
        const double xMm =
            (static_cast<double>(pixel.x()) + 0.5 - 0.5 * imageSize_.x()) *
            fitScale * millimetresPerDisplayPixel;
        const double yMm =
            (0.5 * imageSize_.y() - static_cast<double>(pixel.y()) - 0.5) *
            fitScale * millimetresPerDisplayPixel;
        return nanogui::normalize(nanogui::Vector3f{
            static_cast<float>(xMm),
            static_cast<float>(yMm),
            static_cast<float>(display.viewingDistanceMm),
        });
    };
    const bool useDisplayVisualAngle =
        options.displayGeometry.enabled &&
        options.displayGeometry.pixelWidth > 0 &&
        options.displayGeometry.pixelHeight > 0 &&
        options.displayGeometry.pixelsPerInch > 0.0 &&
        options.displayGeometry.viewingDistanceMm > 0.0;
    nanogui::Vector3f displayReferenceDirection{0.0f, 0.0f, 1.0f};
    if (useDisplayVisualAngle) {
        const nanogui::Vector2i displayReferencePixel =
            options.meteringMode == EGazeMeteringMode::GaussianGazeSpot
            ? options.gazePoint
            : nanogui::Vector2i{imageSize_.x() / 2, imageSize_.y() / 2};
        displayReferenceDirection = displayDirectionForPixel(displayReferencePixel);
    }

    double weightedLogSignalSum = 0.0;
    double weightedLogLuminanceSum = 0.0;
    double weightedSignalSum = 0.0;
    double weightedLuminanceSum = 0.0;
    double weightSum = 0.0;
    double highlightScaleLimit = std::numeric_limits<double>::infinity();
    const bool protectHighlights =
        options.protectGazeHighlights &&
        options.meteringMode == EGazeMeteringMode::GaussianGazeSpot;
    const double highlightRadius =
        std::max(0.1, options.highlightRadiusSigmas) * sigmaRadians;
    const double highlightHeadroom = std::clamp(options.highlightHeadroom, 0.05, 0.999);
    result.highlightProtectionRequested = protectHighlights;
    for (const auto& sample : samples_) {
        double weight = 1.0;
        double angle = 0.0;
        if (options.meteringMode == EGazeMeteringMode::CenterWeighted) {
            angle = useDisplayVisualAngle
                ? angularDistance(displayDirectionForPixel(sample.pixel), displayReferenceDirection)
                : angularDistance(sample.direction, referenceDirection);
            const double gaussian = std::exp(-0.5 * angle * angle / (sigmaRadians * sigmaRadians));
            weight = 0.15 + 0.85 * gaussian;
        } else if (options.meteringMode == EGazeMeteringMode::GaussianGazeSpot) {
            angle = useDisplayVisualAngle
                ? angularDistance(displayDirectionForPixel(sample.pixel), displayReferenceDirection)
                : angularDistance(sample.direction, referenceDirection);
            weight = std::exp(-0.5 * angle * angle / (sigmaRadians * sigmaRadians));
            if (angle <= highlightRadius) {
                result.gazeHighlightLuminance =
                    std::max(result.gazeHighlightLuminance, sample.luminance);
                result.gazeHighlightPeakSignal =
                    std::max(result.gazeHighlightPeakSignal, sample.peakSignal);
                ++result.highlightSampleCount;

                if (protectHighlights) {
                    double allowedScale = std::numeric_limits<double>::infinity();
                    if (options.outputMapping == EGazeOutputMapping::Reinhard) {
                        const double denominator =
                            sample.peakSignal - highlightHeadroom * sample.signal;
                        if (denominator > kLuminanceFloor) {
                            allowedScale = highlightHeadroom / denominator;
                        }
                    } else {
                        allowedScale = highlightHeadroom / sample.peakSignal;
                    }
                    if (std::isfinite(allowedScale) && allowedScale > 0.0) {
                        highlightScaleLimit = std::min(highlightScaleLimit, allowedScale);
                    }
                }
            }
        }
        weightedSignalSum += weight * sample.signal;
        weightedLuminanceSum += weight * sample.luminance;
        weightedLogSignalSum += weight * sample.logSignal;
        weightedLogLuminanceSum += weight * sample.logLuminance;
        weightSum += weight;
    }

    if (!(weightSum > 0.0) ||
        !std::isfinite(weightedLogSignalSum) ||
        !std::isfinite(weightedLogLuminanceSum)) {
        result.statusMessage = "The adaptation weighting produced no valid samples.";
        return result;
    }

    if (options.meteringMode == EGazeMeteringMode::GaussianGazeSpot) {
        // A gaze spot represents spatial pooling around fixation. Filtering in
        // linear luminance lets a bright attended region drive adaptation even
        // when it is surrounded by a much darker field.
        result.adaptationSignal = weightedSignalSum / weightSum;
        result.adaptationLuminance = weightedLuminanceSum / weightSum;
    } else {
        result.adaptationSignal = std::exp(weightedLogSignalSum / weightSum);
        result.adaptationLuminance = std::exp(weightedLogLuminanceSum / weightSum);
    }
    result.weightSum = weightSum;
    const double targetGrey = std::clamp(options.targetGrey, 0.001, 0.95);
    const double displayScale = std::max(1e-12, options.displayScale);
    result.unconstrainedExposureEv = std::clamp(
        std::log2(targetGrey / (result.adaptationSignal * displayScale)),
        -16.0,
        16.0
    );
    result.targetExposureEv = result.unconstrainedExposureEv;
    result.highlightExposureLimitEv = 16.0;
    if (protectHighlights && std::isfinite(highlightScaleLimit)) {
        result.highlightExposureLimitEv = std::clamp(
            std::log2(highlightScaleLimit / displayScale),
            -16.0,
            16.0
        );
        result.targetExposureEv = std::min(
            result.unconstrainedExposureEv,
            result.highlightExposureLimitEv
        );
        result.highlightProtectionActive =
            result.highlightExposureLimitEv < result.unconstrainedExposureEv - 1e-6;
    }
    result.ok = std::isfinite(result.targetExposureEv);
    result.statusMessage = result.ok ? "Adaptation ready." : "Could not compute a finite exposure.";
    return result;
}

} // namespace native
