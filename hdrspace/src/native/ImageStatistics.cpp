#include "native/ImageStatistics.h"

#include <tev/Channel.h>
#include <tev/imageio/Colors.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <ranges>
#include <sstream>

using namespace nanogui;

namespace native {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kWhiteEfficacy = 179.0f;

enum class EDetectedProjection {
    Unknown,
    Equisolid,
    Equidistant,
};

struct ProjectionMetadata {
    EDetectedProjection projection = EDetectedProjection::Unknown;
    bool cropped = false;
    bool square = false;
    double horizontalViewDegrees = 180.0;
    double verticalViewDegrees = 180.0;
};

enum class EStatisticsLuminanceMode {
    Unavailable,
    Weighted,
    Monochrome,
};

struct StatisticsColorInfo {
    bool usingSourceChannels = true;
    tev::ituth273::ETransfer sourceTransfer = tev::ituth273::ETransfer::Linear;
    float sourceScale = 1.0f;
    EStatisticsLuminanceMode luminanceMode = EStatisticsLuminanceMode::Unavailable;
    Matrix3f sourceToXyz = Matrix3f{1.0f};
    std::array<float, 3> luminanceWeights{0.0f, 0.0f, 0.0f};
    std::string description;
};

struct SampleValue {
    double sortKey = 0.0;
    double omega = 0.0;
    std::array<double, 3> values{0.0, 0.0, 0.0};
    int nValues = 1;
    int x = 0;
    int y = 0;
};

struct LineAggregate {
    int index = 0;
    double axisX = 0.0;
    double omega = 0.0;
    size_t sampleCount = 0;
    std::array<double, 3> sum{0.0, 0.0, 0.0};
    std::array<double, 3> mean{0.0, 0.0, 0.0};
    std::array<double, 3> cumulativeMean{0.0, 0.0, 0.0};
    double cumulativeOmega = 0.0;
};

std::string trimCopy(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string toLowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::optional<std::string_view> findAttributeValue(std::span<const tev::AttributeNode> nodes, std::string_view name) {
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

void findProjectionCommandValues(std::span<const tev::AttributeNode> nodes, std::vector<std::string_view>& values) {
    for (const auto& node : nodes) {
        if (node.name == "MERGEHDR_COMMAND" || node.name.rfind("MERGEHDR_STEP_", 0) == 0) {
            values.push_back(node.value);
        }
        findProjectionCommandValues(node.children, values);
    }
}

EDetectedProjection parseProjectionName(std::string_view value) {
    const std::string token = toLowerCopy(trimCopy(std::string{value}));
    if (token == "equisolid") {
        return EDetectedProjection::Equisolid;
    }
    if (token == "equidistant" || token == "equiangular") {
        return EDetectedProjection::Equidistant;
    }
    return EDetectedProjection::Unknown;
}

void parseViewAngles(std::string_view viewValue, double& horizontalDegrees, double& verticalDegrees) {
    std::istringstream iss(std::string{viewValue});
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == "-vh") {
            const double parsed = std::atof(tokens[i + 1].c_str());
            if (parsed > 0.0) {
                horizontalDegrees = parsed;
            }
        } else if (tokens[i] == "-vv") {
            const double parsed = std::atof(tokens[i + 1].c_str());
            if (parsed > 0.0) {
                verticalDegrees = parsed;
            }
        }
    }
}

ProjectionMetadata detectProjectionMetadata(const tev::Image& image) {
    ProjectionMetadata metadata;
    const auto attributes = image.attributes();
    metadata.cropped = findAttributeValue(attributes, "MERGEHDR_CROP").has_value();
    metadata.square = image.size().x() == image.size().y();
    if (const auto view = findAttributeValue(attributes, "VIEW")) {
        parseViewAngles(*view, metadata.horizontalViewDegrees, metadata.verticalViewDegrees);
    }

    if (const auto explicitProjection = findAttributeValue(attributes, "MERGEHDR_PROJECTION")) {
        metadata.projection = parseProjectionName(*explicitProjection);
        if (metadata.projection != EDetectedProjection::Unknown) {
            return metadata;
        }
    }

    if (const auto view = findAttributeValue(attributes, "VIEW")) {
        if (view->find("-vta") != std::string_view::npos) {
            metadata.projection = EDetectedProjection::Equidistant;
            return metadata;
        }
    }

    std::vector<std::string_view> commands;
    findProjectionCommandValues(attributes, commands);
    for (const auto value : commands) {
        if (value.find("--solid2ang") != std::string_view::npos ||
            (value.find("--fisheye") != std::string_view::npos && value.find("--no-fisheye") == std::string_view::npos)) {
            metadata.projection = EDetectedProjection::Equidistant;
            return metadata;
        }
    }

    if (metadata.cropped && metadata.square) {
        for (const auto value : commands) {
            if (value.find("--no-fisheye") != std::string_view::npos) {
                metadata.projection = EDetectedProjection::Equisolid;
                return metadata;
            }
        }
    }

    return metadata;
}

bool attributeContainsInsensitive(std::span<const tev::AttributeNode> nodes, std::string_view needle) {
    const auto loweredNeedle = toLowerCopy(std::string{needle});
    for (const auto& node : nodes) {
        if (toLowerCopy(std::string{node.name}).find(loweredNeedle) != std::string::npos ||
            toLowerCopy(std::string{node.value}).find(loweredNeedle) != std::string::npos) {
            return true;
        }
        if (attributeContainsInsensitive(node.children, needle)) {
            return true;
        }
    }
    return false;
}

std::vector<float> parseAttributeFloatList(std::string_view value) {
    std::vector<float> result;
    for (const auto token : tev::splitWhitespace(trimCopy(std::string{value}))) {
        float parsed = 0.0f;
        if (tev::fromChars(token, parsed)) {
            result.push_back(parsed);
        }
    }
    return result;
}

size_t countNonAlphaChannels(std::span<const std::string> channels) {
    size_t count = 0;
    for (const auto& channel : channels) {
        if (!tev::Channel::isAlpha(channel)) {
            ++count;
        }
    }
    return count;
}

void findAttributeValues(std::span<const tev::AttributeNode> nodes, std::string_view name, std::vector<std::string_view>& result) {
    for (const auto& node : nodes) {
        if (node.name == name) {
            result.push_back(node.value);
        }
        findAttributeValues(node.children, name, result);
    }
}

Matrix3f primariesToXyzMatrix(const tev::chroma_t& chroma) {
    Matrix3f pxyz = Matrix3f{1.0f};
    for (int c = 0; c < 3; ++c) {
        const float x = chroma[static_cast<size_t>(c)].x();
        const float y = chroma[static_cast<size_t>(c)].y();
        const float z = 1.0f - x - y;
        pxyz.m[static_cast<size_t>(c)][0] = x;
        pxyz.m[static_cast<size_t>(c)][1] = y;
        pxyz.m[static_cast<size_t>(c)][2] = z;
    }

    Vector3f wxyz;
    wxyz[0] = chroma[3].x() / chroma[3].y();
    wxyz[1] = 1.0f;
    wxyz[2] = (1.0f - chroma[3].x() - chroma[3].y()) / chroma[3].y();

    const Vector3f scales = inverse(pxyz) * wxyz;
    Matrix3f rgbToXyz = pxyz;
    for (int c = 0; c < 3; ++c) {
        rgbToXyz.m[static_cast<size_t>(c)][0] *= scales[c];
        rgbToXyz.m[static_cast<size_t>(c)][1] *= scales[c];
        rgbToXyz.m[static_cast<size_t>(c)][2] *= scales[c];
    }

    return rgbToXyz;
}

std::optional<tev::chroma_t> parseTargetChroma(std::span<const tev::AttributeNode> nodes) {
    const auto validChroma = [](const tev::chroma_t& chroma) {
        return std::ranges::all_of(chroma, [](const Vector2f& point) {
            return std::isfinite(point.x()) &&
                std::isfinite(point.y()) &&
                point.x() > 0.0f &&
                point.y() > 0.0f;
        });
    };

    const auto primariesValue = findAttributeValue(nodes, "TargetPrimaries");
    const auto whiteValue = findAttributeValue(nodes, "TargetWhitePoint");
    if (primariesValue && whiteValue) {
        const auto primaries = parseAttributeFloatList(*primariesValue);
        const auto white = parseAttributeFloatList(*whiteValue);
        if (primaries.size() == 6 && white.size() == 2) {
            tev::chroma_t result;
            result[0] = Vector2f{primaries[0], primaries[1]};
            result[1] = Vector2f{primaries[2], primaries[3]};
            result[2] = Vector2f{primaries[4], primaries[5]};
            result[3] = Vector2f{white[0], white[1]};
            if (validChroma(result)) {
                return result;
            }
        }
    }

    const auto radiancePrimariesValue = findAttributeValue(nodes, "PRIMARIES");
    if (!radiancePrimariesValue) {
        return std::nullopt;
    }

    const auto values = parseAttributeFloatList(*radiancePrimariesValue);
    if (values.size() != 8) {
        return std::nullopt;
    }

    tev::chroma_t result;
    result[0] = Vector2f{values[0], values[1]};
    result[1] = Vector2f{values[2], values[3]};
    result[2] = Vector2f{values[4], values[5]};
    result[3] = Vector2f{values[6], values[7]};
    return validChroma(result) ? std::optional{result} : std::nullopt;
}

std::optional<float> parseExposureCompensationScale(std::span<const tev::AttributeNode> nodes) {
    const auto formatValue = findAttributeValue(nodes, "FORMAT");
    if (!formatValue) {
        return std::nullopt;
    }

    std::string format = std::string{*formatValue};
    std::ranges::transform(format, format.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (format.find("rgbe") == std::string::npos && format.find("xyze") == std::string::npos) {
        return std::nullopt;
    }

    std::vector<std::string_view> values;
    findAttributeValues(nodes, "EXPOSURE", values);
    if (values.empty()) {
        return std::nullopt;
    }

    float scale = 1.0f;
    bool found = false;
    for (const auto value : values) {
        for (const float parsed : parseAttributeFloatList(value)) {
            if (parsed > 0.0f && std::isfinite(parsed)) {
                scale /= parsed;
                found = true;
            }
        }
    }

    if (!found) {
        return std::nullopt;
    }

    return scale;
}

std::optional<Matrix3f> parseSensorToXyz(std::span<const tev::AttributeNode> nodes) {
    if (const auto sensorToXyzValue = findAttributeValue(nodes, "SENSOR2XYZ")) {
        const auto sensorToXyzValues = parseAttributeFloatList(*sensorToXyzValue);
        if (sensorToXyzValues.size() == 9) {
            Matrix3f sensorToXyz = Matrix3f{1.0f};
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    sensorToXyz.m[col][row] = sensorToXyzValues[static_cast<size_t>(row * 3 + col)];
                }
            }
            return sensorToXyz;
        }
    }

    const auto xyzcamValue = findAttributeValue(nodes, "XYZCAM");
    if (!xyzcamValue) {
        return std::nullopt;
    }

    const auto xyzcamValues = parseAttributeFloatList(*xyzcamValue);
    if (xyzcamValues.size() != 9) {
        return std::nullopt;
    }

    Matrix3f xyzCam = Matrix3f{1.0f};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            xyzCam.m[col][row] = xyzcamValues[static_cast<size_t>(row * 3 + col)];
        }
    }

    if (const auto premultValue = findAttributeValue(nodes, "CAM_PREMULTIPLIERS")) {
        const auto premults = parseAttributeFloatList(*premultValue);
        if (premults.size() >= 3) {
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    xyzCam.m[col][row] *= premults[static_cast<size_t>(row)];
                }
            }
        }
    }

    return inverse(xyzCam);
}

tev::ituth273::ETransfer statisticsSourceTransfer(const tev::Image& image, bool usingSourceChannels) {
    if (!usingSourceChannels) {
        return tev::ituth273::ETransfer::Linear;
    }

    return image.nativeMetadata().transfer.value_or(tev::ituth273::ETransfer::Linear);
}

float statisticsSourceScale(const tev::Image& image) {
    return parseExposureCompensationScale(image.attributes()).value_or(1.0f);
}

bool parseLuminanceRgbWeights(std::span<const tev::AttributeNode> nodes, std::array<float, 3>& weights) {
    const auto value = findAttributeValue(nodes, "LuminanceRGB");
    if (!value) {
        return false;
    }
    const auto parsed = parseAttributeFloatList(*value);
    if (parsed.size() != 3) {
        return false;
    }
    weights = {parsed[0], parsed[1], parsed[2]};
    return true;
}

bool looksLikeTechnoteamOrLmk(std::span<const tev::AttributeNode> nodes) {
    return attributeContainsInsensitive(nodes, "technoteam") ||
           attributeContainsInsensitive(nodes, "lmk") ||
           attributeContainsInsensitive(nodes, "pftopic") ||
           attributeContainsInsensitive(nodes, "pcftoxyz") ||
           attributeContainsInsensitive(nodes, "programversion=standard color") ||
           attributeContainsInsensitive(nodes, "camera=svs") ||
           attributeContainsInsensitive(nodes, "camera=tt");
}

bool imageChannelsAreMonochrome(
    const tev::Image& image,
    std::span<const std::string> channels,
    bool usingSourceChannels
) {
    if (channels.size() < 3) {
        return false;
    }

    const tev::Channel* c0 = usingSourceChannels ? image.sourceChannel(channels[0]) : image.channel(channels[0]);
    const tev::Channel* c1 = usingSourceChannels ? image.sourceChannel(channels[1]) : image.channel(channels[1]);
    const tev::Channel* c2 = usingSourceChannels ? image.sourceChannel(channels[2]) : image.channel(channels[2]);
    if (!c0 || !c1 || !c2) {
        return false;
    }

    constexpr float epsilon = 1e-6f;
    for (int y = 0; y < image.size().y(); ++y) {
        for (int x = 0; x < image.size().x(); ++x) {
            const Vector2i coords{x, y};
            const float r = c0->evalOrZero(coords);
            const float g = c1->evalOrZero(coords);
            const float b = c2->evalOrZero(coords);
            const float scale = std::max({1.0f, std::abs(r), std::abs(g), std::abs(b)});
            if (std::abs(r - g) > epsilon * scale || std::abs(r - b) > epsilon * scale) {
                return false;
            }
        }
    }
    return true;
}

StatisticsColorInfo resolveStatisticsColorInfo(
    const tev::Image& image,
    std::span<const std::string> channels,
    bool usingSourceChannels
) {
    StatisticsColorInfo info;
    info.usingSourceChannels = usingSourceChannels;
    info.sourceTransfer = statisticsSourceTransfer(image, usingSourceChannels);
    info.sourceScale = statisticsSourceScale(image);

    if (!usingSourceChannels) {
        info.luminanceMode = EStatisticsLuminanceMode::Weighted;
        info.sourceToXyz = primariesToXyzMatrix(tev::rec709Chroma());
        info.description = "display/derived channels";
        return info;
    }

    if (channels.size() < 3) {
        info.luminanceMode = EStatisticsLuminanceMode::Monochrome;
        info.description = "single-channel luminance";
        return info;
    }

    if (imageChannelsAreMonochrome(image, channels, usingSourceChannels)) {
        info.luminanceMode = EStatisticsLuminanceMode::Monochrome;
        info.description = "monochrome RGB input";
        return info;
    }

    std::array<float, 3> luminanceWeights{};
    if (parseLuminanceRgbWeights(image.attributes(), luminanceWeights)) {
        info.luminanceMode = EStatisticsLuminanceMode::Weighted;
        info.luminanceWeights = luminanceWeights;
        info.description = "LuminanceRGB header";
        return info;
    }

    if (const auto sensorToXyz = parseSensorToXyz(image.attributes())) {
        info.luminanceMode = EStatisticsLuminanceMode::Weighted;
        info.sourceToXyz = *sensorToXyz;
        info.description = findAttributeValue(image.attributes(), "SENSOR2XYZ").has_value() ? "SENSOR2XYZ header" : "XYZCAM header";
        return info;
    }

    if (const auto targetChroma = parseTargetChroma(image.attributes())) {
        info.luminanceMode = EStatisticsLuminanceMode::Weighted;
        info.sourceToXyz = primariesToXyzMatrix(*targetChroma);
        info.description = findAttributeValue(image.attributes(), "TargetPrimaries").has_value()
            ? "TargetPrimaries/TargetWhitePoint header"
            : "Radiance PRIMARIES header";
        return info;
    }

    if (image.nativeMetadata().chroma.has_value()) {
        info.luminanceMode = EStatisticsLuminanceMode::Weighted;
        info.sourceToXyz = primariesToXyzMatrix(*image.nativeMetadata().chroma);
        info.description = "native image color profile";
        return info;
    }

    if (looksLikeTechnoteamOrLmk(image.attributes())) {
        info.luminanceMode = EStatisticsLuminanceMode::Weighted;
        info.luminanceWeights = {0.26507413f, 0.67011464f, 0.06481124f};
        info.description = "Technoteam/LMK Radiance fallback";
        return info;
    }

    if (const auto format = findAttributeValue(image.attributes(), "FORMAT");
        format && toLowerCopy(std::string{*format}).find("rgbe") != std::string::npos) {
        info.luminanceMode = EStatisticsLuminanceMode::Weighted;
        info.luminanceWeights = {0.26507413f, 0.67011464f, 0.06481124f};
        info.description = "standard Radiance RGBE fallback";
        return info;
    }

    info.luminanceMode = EStatisticsLuminanceMode::Unavailable;
    info.description = "No usable source color metadata was found. Need LuminanceRGB, SENSOR2XYZ/XYZCAM, or primaries; monochrome RGB is also supported.";
    return info;
}

Vector3f clampToHdrDomain(const Vector3f& value) {
    return {
        std::max(0.0f, value.x()),
        std::max(0.0f, value.y()),
        std::max(0.0f, value.z()),
    };
}

Vector2i displayCoordsForImageCoords(const tev::Image& image, Vector2i imageCoords) {
    return imageCoords + (image.dataWindow().min - image.displayWindow().min);
}

Vector2i imageCoordsForDisplayCoords(const tev::Image& image, Vector2i displayCoords) {
    return displayCoords - (image.dataWindow().min - image.displayWindow().min);
}

Vector3f normalizeVec(const Vector3f& value) {
    const float len = std::sqrt(dot(value, value));
    if (len <= 1e-9f) {
        return {0.0f, 0.0f, 0.0f};
    }
    return value / len;
}

std::optional<Vector3f> directionForPixel(const tev::Image& image, const ProjectionMetadata& metadata, double px, double py) {
    if (metadata.projection == EDetectedProjection::Unknown || !metadata.square) {
        return std::nullopt;
    }

    const double width = static_cast<double>(image.size().x());
    const double height = static_cast<double>(image.size().y());
    const double nx = (px + 0.5) / width - 0.5;
    const double ny = (py + 0.5) / height - 0.5;
    const double radius = std::sqrt(nx * nx + ny * ny);
    if (radius > 0.5) {
        return std::nullopt;
    }

    const double horizontalRadians = std::max(1e-9, metadata.horizontalViewDegrees * kPi / 180.0);
    const double verticalRadians = std::max(1e-9, metadata.verticalViewDegrees * kPi / 180.0);
    double theta = 0.0;
    if (metadata.projection == EDetectedProjection::Equidistant) {
        const double u = nx * horizontalRadians;
        const double v = ny * verticalRadians;
        theta = std::sqrt(u * u + v * v);
        if (theta <= 1e-12) {
            return Vector3f{0.0f, 1.0f, 0.0f};
        }
        const double sinTheta = std::sin(theta);
        const double scale = sinTheta / theta;
        return normalizeVec({
            static_cast<float>(u * scale),
            static_cast<float>(std::cos(theta)),
            static_cast<float>(v * scale),
        });
    } else {
        const double scaled = std::clamp(radius * std::sqrt(2.0), 0.0, 1.0);
        theta = 2.0 * std::asin(scaled);
    }

    if (radius <= 1e-12) {
        return Vector3f{0.0f, 1.0f, 0.0f};
    }

    const double sinTheta = std::sin(theta);
    const double scale = sinTheta / radius;
    return normalizeVec({
        static_cast<float>(nx * scale),
        static_cast<float>(std::cos(theta)),
        static_cast<float>(ny * scale),
    });
}

double pixelSolidAngle(const tev::Image& image, const ProjectionMetadata& metadata, double px, double py) {
    const double width = static_cast<double>(image.size().x());
    const double height = static_cast<double>(image.size().y());
    if (width <= 0.0 || height <= 0.0) {
        return 0.0;
    }
    const double pixelArea = 1.0 / (width * height);

    if (metadata.projection == EDetectedProjection::Unknown || !metadata.square) {
        return pixelArea;
    }
    const double nx = (px + 0.5) / width - 0.5;
    const double ny = (py + 0.5) / height - 0.5;
    const double radius = std::sqrt(nx * nx + ny * ny);
    if (radius > 0.5) {
        return 0.0;
    }

    if (metadata.projection == EDetectedProjection::Equisolid) {
        return 8.0 * pixelArea;
    }

    const double horizontalRadians = std::max(1e-9, metadata.horizontalViewDegrees * kPi / 180.0);
    const double verticalRadians = std::max(1e-9, metadata.verticalViewDegrees * kPi / 180.0);
    const double u = nx * horizontalRadians;
    const double v = ny * verticalRadians;
    const double theta = std::sqrt(u * u + v * v);
    const double duDv = (horizontalRadians * verticalRadians) / (width * height);
    if (radius <= 1e-12) {
        return duDv;
    }

    return duDv * std::sin(theta) / std::max(theta, 1e-12);
}

double unsignedAngleDegrees(const Vector3f& a, const Vector3f& b) {
    return std::acos(std::clamp(static_cast<double>(dot(a, b)), -1.0, 1.0)) * 180.0 / kPi;
}

std::vector<float> fetchRawSample(
    const tev::Image& image,
    const std::shared_ptr<tev::Image>& reference,
    std::span<const std::string> channels,
    tev::EMetric metric,
    Vector2i imageCoords
) {
    std::vector<float> values;
    values.reserve(channels.size());
    for (const auto& channelName : channels) {
        const tev::Channel* channel = image.hasSourceChannels() ? image.sourceChannel(channelName) : image.channel(channelName);
        values.push_back(channel ? channel->evalOrZero(imageCoords) : 0.0f);
    }

    if (!reference) {
        return values;
    }

    const Vector2i displayCoords = displayCoordsForImageCoords(image, imageCoords);
    const Vector2i referenceCoords = imageCoordsForDisplayCoords(*reference, displayCoords);
    for (size_t i = 0; i < values.size(); ++i) {
        const bool isAlpha = tev::Channel::isAlpha(channels[i]);
        const float defaultValue = isAlpha && reference->contains(referenceCoords) ? 1.0f : 0.0f;
        const tev::Channel* refChannel =
            reference->hasSourceChannels() ? reference->sourceChannel(channels[i]) : reference->channel(channels[i]);
        const float referenceValue = refChannel ? refChannel->evalOrZero(referenceCoords) : defaultValue;
        values[i] = isAlpha ? 0.5f * (values[i] + referenceValue) : applyMetric(values[i], referenceValue, metric);
    }

    return values;
}

std::vector<std::string> resolveStatisticsSampleChannels(
    const tev::Image& image,
    const tev::ImageCanvas& canvas,
    EImageStatisticsValueMode valueMode
) {
    std::vector<std::string> requestedChannels;
    for (const auto& channel : image.channelsInGroup(canvas.requestedChannelGroup())) {
        requestedChannels.push_back(channel);
    }

    if (!image.hasSourceChannels()) {
        if (valueMode != EImageStatisticsValueMode::RGB || countNonAlphaChannels(requestedChannels) >= 3) {
            return requestedChannels;
        }

        for (const auto& group : image.channelGroups()) {
            const auto groupChannels = image.channelsInGroup(group.name);
            if (countNonAlphaChannels(groupChannels) < 3) {
                continue;
            }

            std::vector<std::string> resolved;
            resolved.reserve(groupChannels.size());
            for (const auto& channel : groupChannels) {
                resolved.push_back(channel);
            }
            return resolved;
        }

        return requestedChannels;
    }

    bool requestedResolvable = !requestedChannels.empty();
    size_t requestedColorCount = 0;
    for (const auto& channelName : requestedChannels) {
        if (!image.sourceChannel(channelName)) {
            requestedResolvable = false;
            break;
        }
        if (!tev::Channel::isAlpha(channelName)) {
            ++requestedColorCount;
        }
    }
    if (requestedResolvable && requestedColorCount > 0) {
        return requestedChannels;
    }

    std::vector<std::string> fallbackChannels;
    std::optional<std::string> alphaChannel;
    for (const auto& channel : image.sourceChannels()) {
        const std::string name = std::string{channel.name()};
        if (tev::Channel::isAlpha(name)) {
            if (!alphaChannel.has_value()) {
                alphaChannel = name;
            }
            continue;
        }
        if (valueMode == EImageStatisticsValueMode::RGB && fallbackChannels.size() >= 3) {
            continue;
        }
        if (valueMode == EImageStatisticsValueMode::Luminance && !fallbackChannels.empty()) {
            continue;
        }
        fallbackChannels.push_back(name);
    }

    if (fallbackChannels.empty()) {
        return requestedChannels;
    }

    if (alphaChannel.has_value()) {
        fallbackChannels.push_back(*alphaChannel);
    }
    return fallbackChannels;
}

std::vector<float> applyInspectionTransform(
    std::vector<float> values,
    bool hasAlpha,
    const StatisticsColorInfo& colorInfo,
    bool inspectionPremultipliedAlpha,
    EImageStatisticsValueMode valueMode
) {
    if (values.empty()) {
        return values;
    }

    const size_t nColorChannels = values.size() - (hasAlpha ? 1 : 0);
    if (valueMode == EImageStatisticsValueMode::RGB) {
        std::vector<float> rgb;
        rgb.reserve(std::min<size_t>(3, nColorChannels));
        for (size_t c = 0; c < std::min<size_t>(3, nColorChannels); ++c) {
            rgb.push_back(values[c]);
        }
        return rgb;
    }

    const float alpha = hasAlpha && !inspectionPremultipliedAlpha ? values.back() : 1.0f;
    const float alphaFactor = alpha == 0.0f ? 0.0f : 1.0f / alpha;

    if (nColorChannels >= 3) {
        Vector3f rgb;
        for (size_t c = 0; c < 3; ++c) {
            rgb[c] = values[c] * alphaFactor;
        }
        rgb = clampToHdrDomain(tev::ituth273::invTransfer(colorInfo.sourceTransfer, rgb));
        rgb *= colorInfo.sourceScale;
        if (valueMode == EImageStatisticsValueMode::Luminance) {
            if (colorInfo.luminanceMode == EStatisticsLuminanceMode::Monochrome) {
                return {rgb.y() * kWhiteEfficacy};
            }
            if (colorInfo.luminanceWeights[0] != 0.0f || colorInfo.luminanceWeights[1] != 0.0f || colorInfo.luminanceWeights[2] != 0.0f) {
                const float luminance =
                    rgb.x() * colorInfo.luminanceWeights[0] +
                    rgb.y() * colorInfo.luminanceWeights[1] +
                    rgb.z() * colorInfo.luminanceWeights[2];
                return {std::max(0.0f, luminance) * kWhiteEfficacy};
            }
            const auto xyz = clampToHdrDomain(colorInfo.sourceToXyz * rgb);
            return {xyz.y() * kWhiteEfficacy};
        }
    }

    for (size_t c = 0; c < nColorChannels; ++c) {
        values[c] = std::max(
            0.0f,
            tev::ituth273::invTransferComponent(colorInfo.sourceTransfer, values[c] * alphaFactor) * colorInfo.sourceScale
        ) * kWhiteEfficacy;
    }

    if (valueMode == EImageStatisticsValueMode::Luminance) {
        return {values.empty() ? 0.0f : values.front()};
    }
    values.resize(std::max<size_t>(1, nColorChannels));
    return values;
}

std::string formatNumber(double value) {
    if (!std::isfinite(value)) {
        return "";
    }
    if ((std::abs(value) > 0.0 && std::abs(value) < 0.01) || std::abs(value) >= 1000.0) {
        return std::format("{:.6e}", value);
    }
    return std::format("{:.6f}", value);
}

std::string formatAnglePair(double widthDegrees, double heightDegrees) {
    return std::format("{:.2f}° x {:.2f}°", widthDegrees, heightDegrees);
}

std::array<double, 3> paletteColorRgb(const Color& color) {
    return {
        color.r(),
        color.g(),
        color.b(),
    };
}

std::shared_ptr<tev::PixelBuffer> buildCircleOverlayPixels(
    const tev::Image& image,
    const ProjectionMetadata& metadata,
    const CircleStatisticsRegion& circle
) {
    auto pixels = std::make_shared<tev::PixelBuffer>(tev::PixelBuffer::alloc(
        static_cast<size_t>(image.size().x()) * static_cast<size_t>(image.size().y()) * 4,
        tev::EPixelFormat::F32
    ));
    std::fill(pixels->data<float>(), pixels->data<float>() + static_cast<size_t>(image.size().x()) * static_cast<size_t>(image.size().y()) * 4, 0.0f);

    const auto centerDir = directionForPixel(image, metadata, circle.center.x(), circle.center.y());
    if (!centerDir) {
        return pixels;
    }

    const double limit = circle.radiusDegrees;
    auto* out = pixels->data<float>();
    for (int y = 0; y < image.size().y(); ++y) {
        for (int x = 0; x < image.size().x(); ++x) {
            const auto dir = directionForPixel(image, metadata, x, y);
            if (!dir) {
                continue;
            }
            const double angle = unsignedAngleDegrees(*centerDir, *dir);
            if (angle > limit) {
                continue;
            }
            const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(image.size().x()) + static_cast<size_t>(x)) * 4;
            out[idx + 0] = 0.22f;
            out[idx + 1] = 0.75f;
            out[idx + 2] = 1.0f;
            out[idx + 3] = 0.45f;
        }
    }

    return pixels;
}

ImageStatisticsGraph buildProfileGraph(
    std::string title,
    std::string subtitle,
    std::string xLabel,
    std::vector<LineAggregate> aggregates,
    EImageStatisticsValueMode valueMode
) {
    ImageStatisticsGraph graph;
    graph.title = std::move(title);
    graph.subtitle = std::move(subtitle);
    graph.xLabel = std::move(xLabel);
    graph.yLabel = valueMode == EImageStatisticsValueMode::Luminance ? "cd/m²" : "RGB";

    if (valueMode == EImageStatisticsValueMode::Luminance) {
        tev::ProfileGraphSeries series;
        series.label = "L";
        series.color = Color(255, 209, 84, 255);
        for (const auto& aggregate : aggregates) {
            series.x.push_back(static_cast<float>(aggregate.axisX));
            series.y.push_back(static_cast<float>(aggregate.mean[0]));
        }
        graph.series.push_back(std::move(series));
    } else {
        const std::array<Color, 3> colors = {
            Color(255, 110, 110, 255),
            Color(120, 230, 150, 255),
            Color(120, 180, 255, 255),
        };
        const std::array<std::string, 3> labels = {"R", "G", "B"};
        for (int c = 0; c < 3; ++c) {
            tev::ProfileGraphSeries series;
            series.label = labels[static_cast<size_t>(c)];
            series.color = colors[static_cast<size_t>(c)];
            for (const auto& aggregate : aggregates) {
                series.x.push_back(static_cast<float>(aggregate.axisX));
                series.y.push_back(static_cast<float>(aggregate.mean[static_cast<size_t>(c)]));
            }
            graph.series.push_back(std::move(series));
        }
    }

    return graph;
}

ImageStatisticsGraph buildRankedGraph(
    std::string title,
    std::string subtitle,
    std::string xLabel,
    const std::vector<SampleValue>& samples,
    bool useCumulativeOmega,
    EImageStatisticsValueMode valueMode
) {
    ImageStatisticsGraph graph;
    graph.title = std::move(title);
    graph.subtitle = std::move(subtitle);
    graph.xLabel = std::move(xLabel);
    graph.yLabel = valueMode == EImageStatisticsValueMode::Luminance ? "cd/m²" : "RGB";

    double cumulativeOmega = 0.0;
    if (valueMode == EImageStatisticsValueMode::Luminance) {
        tev::ProfileGraphSeries series;
        series.label = "L";
        series.color = Color(255, 209, 84, 255);
        for (size_t i = 0; i < samples.size(); ++i) {
            cumulativeOmega += samples[i].omega;
            series.x.push_back(static_cast<float>(useCumulativeOmega ? cumulativeOmega : static_cast<double>(i + 1)));
            series.y.push_back(static_cast<float>(samples[i].values[0]));
        }
        graph.series.push_back(std::move(series));
    } else {
        const std::array<Color, 3> colors = {
            Color(255, 110, 110, 255),
            Color(120, 230, 150, 255),
            Color(120, 180, 255, 255),
        };
        const std::array<std::string, 3> labels = {"R", "G", "B"};
        std::array<tev::ProfileGraphSeries, 3> series;
        for (int c = 0; c < 3; ++c) {
            series[static_cast<size_t>(c)].label = labels[static_cast<size_t>(c)];
            series[static_cast<size_t>(c)].color = colors[static_cast<size_t>(c)];
        }
        for (size_t i = 0; i < samples.size(); ++i) {
            cumulativeOmega += samples[i].omega;
            const float x = static_cast<float>(useCumulativeOmega ? cumulativeOmega : static_cast<double>(i + 1));
            for (int c = 0; c < 3; ++c) {
                series[static_cast<size_t>(c)].x.push_back(x);
                series[static_cast<size_t>(c)].y.push_back(static_cast<float>(samples[i].values[static_cast<size_t>(c)]));
            }
        }
        graph.series.assign(series.begin(), series.end());
    }

    return graph;
}

void appendProfileRows(
    ImageStatisticsTable& table,
    std::string_view profileName,
    std::span<const LineAggregate> aggregates,
    EImageStatisticsValueMode valueMode
) {
    for (const auto& aggregate : aggregates) {
        std::vector<std::string> row;
        row.push_back(std::string(profileName));
        row.push_back(std::to_string(aggregate.index));
        row.push_back(formatNumber(aggregate.axisX));
        row.push_back(formatNumber(aggregate.omega));
        row.push_back(formatNumber(aggregate.cumulativeOmega));
        if (valueMode == EImageStatisticsValueMode::Luminance) {
            row.push_back(formatNumber(aggregate.mean[0]));
            row.push_back(formatNumber(aggregate.cumulativeMean[0]));
        } else {
            for (int c = 0; c < 3; ++c) {
                row.push_back(formatNumber(aggregate.mean[static_cast<size_t>(c)]));
            }
            for (int c = 0; c < 3; ++c) {
                row.push_back(formatNumber(aggregate.cumulativeMean[static_cast<size_t>(c)]));
            }
        }
        table.rows.push_back(std::move(row));
    }
}

void appendRankedRows(ImageStatisticsTable& table, const std::vector<SampleValue>& samples, bool useCumulativeOmega, EImageStatisticsValueMode valueMode) {
    std::array<double, 3> cumulativeSum{0.0, 0.0, 0.0};
    double cumulativeWeight = 0.0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const double weight = useCumulativeOmega ? std::max(0.0, samples[i].omega) : 1.0;
        cumulativeWeight += weight;
        for (int c = 0; c < samples[i].nValues; ++c) {
            cumulativeSum[static_cast<size_t>(c)] += samples[i].values[static_cast<size_t>(c)] * weight;
        }

        std::vector<std::string> row;
        row.push_back(std::to_string(i + 1));
        row.push_back(std::to_string(samples[i].x));
        row.push_back(std::to_string(samples[i].y));
        row.push_back(formatNumber(samples[i].omega));
        row.push_back(formatNumber(useCumulativeOmega ? cumulativeWeight : static_cast<double>(i + 1)));
        if (valueMode == EImageStatisticsValueMode::Luminance) {
            row.push_back(formatNumber(samples[i].values[0]));
            row.push_back(formatNumber(cumulativeWeight > 0.0 ? cumulativeSum[0] / cumulativeWeight : 0.0));
        } else {
            for (int c = 0; c < 3; ++c) {
                row.push_back(formatNumber(samples[i].values[static_cast<size_t>(c)]));
            }
            for (int c = 0; c < 3; ++c) {
                row.push_back(formatNumber(cumulativeWeight > 0.0 ? cumulativeSum[static_cast<size_t>(c)] / cumulativeWeight : 0.0));
            }
        }
        table.rows.push_back(std::move(row));
    }
}

std::vector<SampleValue> gatherSamples(
    const tev::Image& image,
    const tev::ImageCanvas& canvas,
    const std::shared_ptr<tev::Image>& reference,
    const ProjectionMetadata& metadata,
    const ImageStatisticsOptions& options
) {
    std::vector<SampleValue> samples;

    std::vector<std::string> channels = resolveStatisticsSampleChannels(image, canvas, options.valueMode);
    if (channels.empty()) {
        return samples;
    }

    const bool hasAlphaChannel = channels.size() > 1 && tev::Channel::isAlpha(channels.back());
    const size_t colorChannelCount = channels.size() - (hasAlphaChannel ? 1u : 0u);
    const bool usingSourceChannels = image.hasSourceChannels();
    const StatisticsColorInfo colorInfo = resolveStatisticsColorInfo(image, std::span<const std::string>{channels.data(), std::min<size_t>(3, colorChannelCount)}, usingSourceChannels);
    if (options.valueMode == EImageStatisticsValueMode::Luminance &&
            colorChannelCount >= 3 &&
            colorInfo.luminanceMode == EStatisticsLuminanceMode::Unavailable) {
        throw std::runtime_error(colorInfo.description);
    }

    const auto region = options.regionType == EImageStatisticsRegionType::Rectangle && options.rectangleRegion.has_value()
        ? *options.rectangleRegion
        : tev::Box2i{0};
    std::optional<Vector3f> centerDir;
    if (options.regionType == EImageStatisticsRegionType::Circle && options.circleRegion.has_value()) {
        centerDir = directionForPixel(image, metadata, options.circleRegion->center.x(), options.circleRegion->center.y());
    }

    const auto appendSampleAt = [&](int x, int y) {
        const auto raw = fetchRawSample(image, reference, channels, canvas.metric(), {x, y});
        const bool hasAlpha = raw.size() > 1 && tev::Channel::isAlpha(channels.back());
        auto displayed = applyInspectionTransform(raw, hasAlpha, colorInfo, canvas.inspectionPremultipliedAlpha(), options.valueMode);
        if (displayed.empty()) {
            return;
        }

        SampleValue sample;
        sample.nValues = static_cast<int>(std::min<size_t>(3, displayed.size()));
        for (int i = 0; i < sample.nValues; ++i) {
            sample.values[static_cast<size_t>(i)] = displayed[static_cast<size_t>(i)];
        }
        sample.x = x;
        sample.y = y;
        sample.omega = pixelSolidAngle(image, metadata, x, y);
        sample.sortKey = sample.values[0];
        samples.push_back(sample);
    };

    if (options.regionType == EImageStatisticsRegionType::Rectangle) {
        const auto rect = region.intersect(tev::Box2i{{0, 0}, image.size()});
        if (!rect.isValid()) {
            return samples;
        }
        for (int y = rect.min.y(); y < rect.max.y(); ++y) {
            for (int x = rect.min.x(); x < rect.max.x(); ++x) {
                appendSampleAt(x, y);
            }
        }
        return samples;
    }

    if (options.regionType == EImageStatisticsRegionType::SelectionMask) {
        if (options.selectionMask.size() != static_cast<size_t>(image.size().x()) * static_cast<size_t>(image.size().y())) {
            return samples;
        }

        const auto scanBounds = options.selectionBounds.has_value()
            ? options.selectionBounds->intersect(tev::Box2i{{0, 0}, image.size()})
            : tev::Box2i{{0, 0}, image.size()};
        if (!scanBounds.isValid()) {
            return samples;
        }

        for (int y = scanBounds.min.y(); y < scanBounds.max.y(); ++y) {
            for (int x = scanBounds.min.x(); x < scanBounds.max.x(); ++x) {
                const size_t maskIndex = static_cast<size_t>(y) * static_cast<size_t>(image.size().x()) + static_cast<size_t>(x);
                if (maskIndex >= options.selectionMask.size() || options.selectionMask[maskIndex] == 0) {
                    continue;
                }
                appendSampleAt(x, y);
            }
        }
        return samples;
    }

    if (!options.circleRegion.has_value() || !centerDir) {
        return samples;
    }

    const auto& circle = *options.circleRegion;
    for (int y = 0; y < image.size().y(); ++y) {
        for (int x = 0; x < image.size().x(); ++x) {
            const auto dir = directionForPixel(image, metadata, x, y);
            if (!dir) {
                continue;
            }
            if (unsignedAngleDegrees(*centerDir, *dir) > circle.radiusDegrees) {
                continue;
            }
            appendSampleAt(x, y);
        }
    }

    return samples;
}

std::vector<LineAggregate> buildHorizontalProfile(
    const std::vector<SampleValue>& samples,
    const tev::Image& image,
    const ProjectionMetadata& metadata,
    const ImageStatisticsOptions& options
) {
    std::map<int, LineAggregate> aggregates;
    int centerX = 0;
    if (options.regionType == EImageStatisticsRegionType::Rectangle && options.rectangleRegion.has_value()) {
        centerX = (options.rectangleRegion->min.x() + options.rectangleRegion->max.x() - 1) / 2;
    } else if (options.regionType == EImageStatisticsRegionType::SelectionMask && options.selectionBounds.has_value()) {
        centerX = (options.selectionBounds->min.x() + options.selectionBounds->max.x() - 1) / 2;
    } else if (options.circleRegion.has_value()) {
        centerX = options.circleRegion->center.x();
    }
    for (const auto& sample : samples) {
        auto& agg = aggregates[sample.y];
        agg.index = sample.y;
        ++agg.sampleCount;
        agg.omega += sample.omega;
        for (int c = 0; c < sample.nValues; ++c) {
            agg.sum[static_cast<size_t>(c)] += sample.values[static_cast<size_t>(c)];
        }
    }

    const int firstIndex = aggregates.empty() ? 0 : aggregates.begin()->first;
    std::optional<Vector3f> axisStartDir;
    if (options.axisMode == EImageStatisticsAxisMode::AngleDegrees && !aggregates.empty()) {
        axisStartDir = directionForPixel(image, metadata, centerX, firstIndex);
    }

    std::vector<LineAggregate> result;
    result.reserve(aggregates.size());
    double cumulativeOmega = 0.0;
    double cumulativeWeight = 0.0;
    std::array<double, 3> cumulativeSum{0.0, 0.0, 0.0};
    for (auto& [row, agg] : aggregates) {
        if (agg.sampleCount == 0) {
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            agg.mean[static_cast<size_t>(c)] = agg.sum[static_cast<size_t>(c)] / static_cast<double>(agg.sampleCount);
        }
        if (options.axisMode == EImageStatisticsAxisMode::AngleDegrees && axisStartDir) {
            if (const auto dir = directionForPixel(image, metadata, centerX, row)) {
                agg.axisX = unsignedAngleDegrees(*axisStartDir, *dir);
            }
        } else {
            agg.axisX = static_cast<double>(row - firstIndex);
        }
        cumulativeOmega += agg.omega;
        const double weight = agg.omega > 0.0 ? agg.omega : static_cast<double>(agg.sampleCount);
        cumulativeWeight += weight;
        for (int c = 0; c < 3; ++c) {
            cumulativeSum[static_cast<size_t>(c)] += agg.mean[static_cast<size_t>(c)] * weight;
            agg.cumulativeMean[static_cast<size_t>(c)] =
                cumulativeWeight > 0.0 ? cumulativeSum[static_cast<size_t>(c)] / cumulativeWeight : agg.mean[static_cast<size_t>(c)];
        }
        agg.cumulativeOmega = cumulativeOmega;
        result.push_back(agg);
    }
    return result;
}

std::vector<LineAggregate> buildVerticalProfile(
    const std::vector<SampleValue>& samples,
    const tev::Image& image,
    const ProjectionMetadata& metadata,
    const ImageStatisticsOptions& options
) {
    std::map<int, LineAggregate> aggregates;
    int centerY = 0;
    if (options.regionType == EImageStatisticsRegionType::Rectangle && options.rectangleRegion.has_value()) {
        centerY = (options.rectangleRegion->min.y() + options.rectangleRegion->max.y() - 1) / 2;
    } else if (options.regionType == EImageStatisticsRegionType::SelectionMask && options.selectionBounds.has_value()) {
        centerY = (options.selectionBounds->min.y() + options.selectionBounds->max.y() - 1) / 2;
    } else if (options.circleRegion.has_value()) {
        centerY = options.circleRegion->center.y();
    }
    for (const auto& sample : samples) {
        auto& agg = aggregates[sample.x];
        agg.index = sample.x;
        ++agg.sampleCount;
        agg.omega += sample.omega;
        for (int c = 0; c < sample.nValues; ++c) {
            agg.sum[static_cast<size_t>(c)] += sample.values[static_cast<size_t>(c)];
        }
    }

    const int firstIndex = aggregates.empty() ? 0 : aggregates.begin()->first;
    std::optional<Vector3f> axisStartDir;
    if (options.axisMode == EImageStatisticsAxisMode::AngleDegrees && !aggregates.empty()) {
        axisStartDir = directionForPixel(image, metadata, firstIndex, centerY);
    }

    std::vector<LineAggregate> result;
    result.reserve(aggregates.size());
    double cumulativeOmega = 0.0;
    double cumulativeWeight = 0.0;
    std::array<double, 3> cumulativeSum{0.0, 0.0, 0.0};
    for (auto& [column, agg] : aggregates) {
        if (agg.sampleCount == 0) {
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            agg.mean[static_cast<size_t>(c)] = agg.sum[static_cast<size_t>(c)] / static_cast<double>(agg.sampleCount);
        }
        if (options.axisMode == EImageStatisticsAxisMode::AngleDegrees && axisStartDir) {
            if (const auto dir = directionForPixel(image, metadata, column, centerY)) {
                agg.axisX = unsignedAngleDegrees(*axisStartDir, *dir);
            }
        } else {
            agg.axisX = static_cast<double>(column - firstIndex);
        }
        cumulativeOmega += agg.omega;
        const double weight = agg.omega > 0.0 ? agg.omega : static_cast<double>(agg.sampleCount);
        cumulativeWeight += weight;
        for (int c = 0; c < 3; ++c) {
            cumulativeSum[static_cast<size_t>(c)] += agg.mean[static_cast<size_t>(c)] * weight;
            agg.cumulativeMean[static_cast<size_t>(c)] =
                cumulativeWeight > 0.0 ? cumulativeSum[static_cast<size_t>(c)] / cumulativeWeight
                                      : agg.mean[static_cast<size_t>(c)];
        }
        agg.cumulativeOmega = cumulativeOmega;
        result.push_back(agg);
    }
    return result;
}

std::string csvEscape(std::string_view value) {
    if (value.find_first_of(",\"\n") == std::string_view::npos) {
        return std::string{value};
    }
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

void blendPixel(std::vector<uint8_t>& rgba, int width, int height, int x, int y, const std::array<uint8_t, 4>& color) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
    const float alpha = color[3] / 255.0f;
    rgba[idx + 0] = static_cast<uint8_t>(std::clamp((1.0f - alpha) * rgba[idx + 0] + alpha * color[0], 0.0f, 255.0f));
    rgba[idx + 1] = static_cast<uint8_t>(std::clamp((1.0f - alpha) * rgba[idx + 1] + alpha * color[1], 0.0f, 255.0f));
    rgba[idx + 2] = static_cast<uint8_t>(std::clamp((1.0f - alpha) * rgba[idx + 2] + alpha * color[2], 0.0f, 255.0f));
    rgba[idx + 3] = 255;
}

void drawLine(std::vector<uint8_t>& rgba, int width, int height, int x0, int y0, int x1, int y1, const std::array<uint8_t, 4>& color) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int x = x0;
    int y = y0;
    while (true) {
        blendPixel(rgba, width, height, x, y, color);
        if (x == x1 && y == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
    }
}

void fillCircle(std::vector<uint8_t>& rgba, int width, int height, int cx, int cy, int radius, const std::array<uint8_t, 4>& color) {
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) {
                blendPixel(rgba, width, height, cx + x, cy + y, color);
            }
        }
    }
}

std::string formatProfileNumberSoft(float value) {
    if (std::abs(value) < 0.05f) {
        value = 0.0f;
    }
    const float absValue = std::abs(value);
    if (absValue >= 1000000000.0f) {
        return std::format("{:.1f}B", value / 1000000000.0f);
    }
    if (absValue >= 1000000.0f) {
        return std::format("{:.1f}M", value / 1000000.0f);
    }
    if (absValue >= 1000.0f) {
        return std::format("{:.1f}k", value / 1000.0f);
    }
    return std::format("{:.1f}", value);
}

struct SoftPlotBounds {
    float minX = 0.0f;
    float maxX = 1.0f;
    float minY = 0.0f;
    float maxY = 1.0f;
    float minPositiveY = std::numeric_limits<float>::infinity();
    bool valid = false;
};

SoftPlotBounds computeSoftBounds(std::span<const tev::ProfileGraphSeries> series) {
    SoftPlotBounds bounds;
    bounds.minX = std::numeric_limits<float>::infinity();
    bounds.maxX = -std::numeric_limits<float>::infinity();
    bounds.minY = std::numeric_limits<float>::infinity();
    bounds.maxY = -std::numeric_limits<float>::infinity();

    for (const auto& item : series) {
        const size_t count = std::min(item.x.size(), item.y.size());
        for (size_t i = 0; i < count; ++i) {
            const float x = item.x[i];
            const float y = item.y[i];
            if (!std::isfinite(x) || !std::isfinite(y)) {
                continue;
            }
            bounds.minX = std::min(bounds.minX, x);
            bounds.maxX = std::max(bounds.maxX, x);
            bounds.minY = std::min(bounds.minY, y);
            bounds.maxY = std::max(bounds.maxY, y);
            if (y > 0.0f) {
                bounds.minPositiveY = std::min(bounds.minPositiveY, y);
            }
            bounds.valid = true;
        }
    }

    if (!bounds.valid) {
        bounds.minX = 0.0f;
        bounds.maxX = 1.0f;
        bounds.minY = 0.0f;
        bounds.maxY = 1.0f;
        bounds.minPositiveY = 1.0f;
        return bounds;
    }

    if (bounds.minX == bounds.maxX) {
        bounds.minX -= 0.5f;
        bounds.maxX += 0.5f;
    }
    if (bounds.minY == bounds.maxY) {
        const float pad = bounds.minY == 0.0f ? 1.0f : std::abs(bounds.minY) * 0.1f;
        bounds.minY -= pad;
        bounds.maxY += pad;
    }

    return bounds;
}

struct SoftAxisLayout {
    float minValue = 0.0f;
    float maxValue = 1.0f;
    std::vector<float> ticks{0.0f, 0.5f, 1.0f};
};

double softNiceTickStep(double rawStep) {
    if (!std::isfinite(rawStep) || rawStep <= 0.0) {
        return 1.0;
    }

    const double exponent = std::floor(std::log10(rawStep));
    const double fraction = rawStep / std::pow(10.0, exponent);
    double niceFraction = 1.0;
    if (fraction < 1.5) {
        niceFraction = 1.0;
    } else if (fraction < 3.0) {
        niceFraction = 2.0;
    } else if (fraction < 7.0) {
        niceFraction = 5.0;
    } else {
        niceFraction = 10.0;
    }
    return niceFraction * std::pow(10.0, exponent);
}

std::vector<float> buildSoftTicks(float minValue, float maxValue, float preferredStep, int targetCount = 5) {
    if (!std::isfinite(minValue) || !std::isfinite(maxValue)) {
        return {0.0f, 1.0f};
    }

    if (maxValue < minValue) {
        std::swap(minValue, maxValue);
    }

    double step = preferredStep > 0.0f
        ? static_cast<double>(preferredStep)
        : softNiceTickStep(static_cast<double>(maxValue - minValue) / std::max(1, targetCount - 1));
    if (!std::isfinite(step) || step <= 0.0) {
        step = 1.0;
    }

    const double start = std::ceil(static_cast<double>(minValue) / step) * step;
    const double end = std::floor(static_cast<double>(maxValue) / step) * step;

    std::vector<float> ticks;
    if (start <= end + step * 0.5) {
        for (double value = start; value <= end + step * 0.5; value += step) {
            if (ticks.size() >= 64) {
                break;
            }
            ticks.push_back(static_cast<float>(value));
        }
    }

    if (ticks.empty()) {
        ticks.push_back(minValue);
        if (maxValue != minValue) {
            ticks.push_back(maxValue);
        }
    } else {
        if (std::abs(ticks.front() - minValue) > static_cast<float>(step * 0.35)) {
            ticks.insert(ticks.begin(), minValue);
        } else {
            ticks.front() = minValue;
        }
        if (std::abs(ticks.back() - maxValue) > static_cast<float>(step * 0.35)) {
            ticks.push_back(maxValue);
        } else {
            ticks.back() = maxValue;
        }
    }

    return ticks;
}

std::vector<float> buildSoftLogTicks(float minValue, float maxValue) {
    std::vector<float> ticks;
    if (!(std::isfinite(minValue) && std::isfinite(maxValue)) || minValue <= 0.0f || maxValue <= 0.0f || maxValue < minValue) {
        return ticks;
    }

    const int minExp = static_cast<int>(std::floor(std::log10(minValue)));
    const int maxExp = static_cast<int>(std::ceil(std::log10(maxValue)));
    constexpr std::array<float, 3> multiples = {1.0f, 2.0f, 5.0f};
    for (int exp = minExp; exp <= maxExp; ++exp) {
        const float decade = std::pow(10.0f, static_cast<float>(exp));
        for (float multiple : multiples) {
            const float tick = multiple * decade;
            if (tick < minValue * 0.999f || tick > maxValue * 1.001f) {
                continue;
            }
            ticks.push_back(tick);
            if (ticks.size() >= 64) {
                return ticks;
            }
        }
    }

    if (ticks.empty()) {
        ticks.push_back(minValue);
        if (maxValue > minValue) {
            ticks.push_back(maxValue);
        }
    } else {
        if (std::abs(ticks.front() - minValue) / std::max(minValue, 1e-12f) > 0.15f) {
            ticks.insert(ticks.begin(), minValue);
        }
        if (std::abs(ticks.back() - maxValue) / std::max(maxValue, 1e-12f) > 0.15f) {
            ticks.push_back(maxValue);
        }
    }

    return ticks;
}

SoftAxisLayout buildSoftAxisLayout(
    float minValue,
    float maxValue,
    float preferredStep,
    int targetCount = 5,
    bool clampMinToZero = false,
    std::optional<float> minOverride = std::nullopt,
    std::optional<float> maxOverride = std::nullopt,
    bool logScale = false,
    float positiveMin = std::numeric_limits<float>::infinity()
) {
    SoftAxisLayout layout;
    if (!std::isfinite(minValue) || !std::isfinite(maxValue)) {
        return layout;
    }

    if (minOverride.has_value()) {
        minValue = *minOverride;
    }
    if (maxOverride.has_value()) {
        maxValue = *maxOverride;
    }

    if (maxValue < minValue) {
        std::swap(minValue, maxValue);
    }

    if (logScale) {
        float axisMin = minOverride.value_or((std::isfinite(positiveMin) ? positiveMin : minValue));
        float axisMax = maxOverride.value_or(maxValue);
        if (axisMin <= 0.0f) {
            axisMin = std::isfinite(positiveMin) ? positiveMin : 0.1f;
        }
        if (axisMax <= axisMin) {
            axisMax = axisMin * 10.0f;
        }
        layout.minValue = axisMin;
        layout.maxValue = axisMax;
        layout.ticks = preferredStep > 0.0f
            ? buildSoftTicks(axisMin, axisMax, preferredStep, targetCount)
            : buildSoftLogTicks(axisMin, axisMax);
        return layout;
    }

    if (minValue == maxValue) {
        if (clampMinToZero && minValue >= 0.0f) {
            minValue = 0.0f;
            maxValue = maxValue == 0.0f ? 1.0f : maxValue * 1.1f;
        } else {
            const float pad = minValue == 0.0f ? 1.0f : std::abs(minValue) * 0.1f;
            minValue -= pad;
            maxValue += pad;
        }
    }

    double step = preferredStep > 0.0f
        ? static_cast<double>(preferredStep)
        : softNiceTickStep(static_cast<double>(maxValue - minValue) / std::max(1, targetCount - 1));
    if (!std::isfinite(step) || step <= 0.0) {
        step = 1.0;
    }

    double axisMin = clampMinToZero && minValue >= 0.0f
        ? 0.0
        : std::floor(static_cast<double>(minValue) / step) * step;
    double axisMax = std::ceil(static_cast<double>(maxValue) / step) * step;
    if (axisMax <= axisMin) {
        axisMax = axisMin + step;
    }

    layout.minValue = static_cast<float>(axisMin);
    layout.maxValue = static_cast<float>(axisMax);
    layout.ticks = buildSoftTicks(layout.minValue, layout.maxValue, static_cast<float>(step), targetCount);
    return layout;
}

float softColorLuminance(const Color& color) {
    return 0.2126f * color.r() + 0.7152f * color.g() + 0.0722f * color.b();
}

struct SoftDerivedGraphColors {
    Color panelBackground;
    Color plotBackground;
    Color border;
    Color gridMajor;
    Color gridMinor;
    Color primaryText;
    Color secondaryText;
};

SoftDerivedGraphColors deriveSoftGraphColors(const tev::ProfileGraphStyle& style) {
    const bool lightBackground = !style.transparentBackground && softColorLuminance(style.backgroundColor) > 0.6f;

    SoftDerivedGraphColors out;
    out.panelBackground = style.backgroundColor;
    out.plotBackground = style.transparentBackground
        ? Color(0, 0)
        : (lightBackground ? Color(255, 255, 255, 182) : Color(13, 17, 24, 188));
    out.border = lightBackground ? Color(0, 0, 0, 42) : Color(255, 232, 238, 28);
    out.gridMajor = lightBackground ? Color(0, 0, 0, 28) : Color(255, 220, 230, 24);
    out.gridMinor = lightBackground ? Color(0, 0, 0, 14) : Color(255, 196, 214, 14);
    out.primaryText = lightBackground ? Color(28, 34, 44, 255) : Color(241, 243, 248, 255);
    out.secondaryText = lightBackground ? Color(78, 88, 104, 220) : Color(217, 199, 213, 220);
    return out;
}

std::array<std::string_view, 7> graphGlyphRows(char c) {
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(c)))) {
        case 'A': return {" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"};
        case 'B': return {"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "};
        case 'C': return {" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "};
        case 'D': return {"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "};
        case 'E': return {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"};
        case 'F': return {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "};
        case 'G': return {" ### ", "#   #", "#    ", "#  ##", "#   #", "#   #", " ### "};
        case 'H': return {"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"};
        case 'I': return {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "#####"};
        case 'J': return {"#####", "    #", "    #", "    #", "#   #", "#   #", " ### "};
        case 'K': return {"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"};
        case 'L': return {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"};
        case 'M': return {"#   #", "## ##", "# # #", "#   #", "#   #", "#   #", "#   #"};
        case 'N': return {"#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"};
        case 'O': return {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "};
        case 'P': return {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "};
        case 'Q': return {" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"};
        case 'R': return {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"};
        case 'S': return {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "};
        case 'T': return {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "};
        case 'U': return {"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "};
        case 'V': return {"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "};
        case 'W': return {"#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"};
        case 'X': return {"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"};
        case 'Y': return {"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "};
        case 'Z': return {"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"};
        case '0': return {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "};
        case '1': return {"  #  ", " ##  ", "# #  ", "  #  ", "  #  ", "  #  ", "#####"};
        case '2': return {" ### ", "#   #", "    #", " ### ", "#    ", "#    ", "#####"};
        case '3': return {" ### ", "#   #", "    #", " ### ", "    #", "#   #", " ### "};
        case '4': return {"#   #", "#   #", "#   #", "#####", "    #", "    #", "    #"};
        case '5': return {"#####", "#    ", "#    ", "#### ", "    #", "#   #", " ### "};
        case '6': return {" ### ", "#   #", "#    ", "#### ", "#   #", "#   #", " ### "};
        case '7': return {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "};
        case '8': return {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "};
        case '9': return {" ### ", "#   #", "#   #", " ####", "    #", "#   #", " ### "};
        case '.': return {"     ", "     ", "     ", "     ", "     ", " ### ", " ### "};
        case ',': return {"     ", "     ", "     ", "     ", "  ## ", "  ## ", " ##  "};
        case '-': return {"     ", "     ", "     ", "#####", "     ", "     ", "     "};
        case '+': return {"     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     "};
        case ':': return {"     ", " ### ", " ### ", "     ", " ### ", " ### ", "     "};
        case '/': return {"    #", "   # ", "   # ", "  #  ", " #   ", " #   ", "#    "};
        case '(': return {"   # ", "  #  ", " #   ", " #   ", " #   ", "  #  ", "   # "};
        case ')': return {" #   ", "  #  ", "   # ", "   # ", "   # ", "  #  ", " #   "};
        case '_': return {"     ", "     ", "     ", "     ", "     ", "     ", "#####"};
        case ' ': return {"     ", "     ", "     ", "     ", "     ", "     ", "     "};
        default:  return {"     ", "     ", "     ", "     ", "     ", "     ", "     "};
    }
}

std::string sanitizeGraphText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch < 128) {
            out.push_back(static_cast<char>(std::toupper(ch)));
            continue;
        }
        out.push_back(' ');
    }
    return out;
}

int graphTextWidth(std::string_view text, int scale = 1) {
    if (text.empty()) {
        return 0;
    }
    return ((static_cast<int>(text.size()) * 6) - 1) * std::max(scale, 1);
}

void drawGraphTextBitmap(
    std::vector<uint8_t>& rgba,
    int width,
    int height,
    int x,
    int y,
    std::string_view text,
    const std::array<uint8_t, 4>& color,
    int scale = 1
) {
    const int clampedScale = std::max(scale, 1);
    int penX = x;
    const std::string sanitized = sanitizeGraphText(text);
    for (char c : sanitized) {
        const auto rows = graphGlyphRows(c);
        for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
            for (int col = 0; col < static_cast<int>(rows[row].size()); ++col) {
                if (rows[row][col] == ' ') {
                    continue;
                }
                for (int sy = 0; sy < clampedScale; ++sy) {
                    for (int sx = 0; sx < clampedScale; ++sx) {
                        blendPixel(
                            rgba,
                            width,
                            height,
                            penX + col * clampedScale + sx,
                            y + row * clampedScale + sy,
                            color
                        );
                    }
                }
            }
        }
        penX += 6 * clampedScale;
    }
}

void drawGraphVerticalTextBitmap(
    std::vector<uint8_t>& rgba,
    int width,
    int height,
    int x,
    int y,
    std::string_view text,
    const std::array<uint8_t, 4>& color,
    int scale = 1
) {
    int penY = y;
    const int step = 8 * std::max(scale, 1);
    const std::string sanitized = sanitizeGraphText(text);
    for (char c : sanitized) {
        std::string one(1, c);
        drawGraphTextBitmap(rgba, width, height, x, penY, one, color, scale);
        penY += step;
    }
}

} // namespace

bool imageStatisticsSupportsAngularAxis(const tev::Image& image) {
    const auto metadata = detectProjectionMetadata(image);
    return metadata.projection != EDetectedProjection::Unknown && metadata.square;
}

std::string imageStatisticsAngularAxisMessage(const tev::Image& image) {
    const auto metadata = detectProjectionMetadata(image);
    if (metadata.projection == EDetectedProjection::Unknown) {
        return "Angular axis requires valid VIEW/projection metadata.";
    }
    if (!metadata.square) {
        return "Angular axis currently requires a square projected image.";
    }
    return {};
}

ImageStatisticsResult computeImageStatistics(
    const tev::Image& image,
    const tev::ImageCanvas& canvas,
    const std::shared_ptr<tev::Image>& reference,
    const ImageStatisticsOptions& options
) {
    ImageStatisticsResult result;
    result.rectangleRegion = options.rectangleRegion;
    result.circleRegion = options.circleRegion;

    const auto metadata = detectProjectionMetadata(image);
    result.hasAngularAxis = imageStatisticsSupportsAngularAxis(image);

    if (options.regionType == EImageStatisticsRegionType::Rectangle) {
        if (!options.rectangleRegion.has_value()) {
            result.statusMessage = "Set an ROI or crop before running statistics.";
            return result;
        }
        const auto region = options.rectangleRegion->intersect(tev::Box2i{{0, 0}, image.size()});
        if (!region.isValid()) {
            result.statusMessage = "Rectangle ROI is outside the current image.";
            return result;
        }
        result.rectangleRegion = region;
    } else if (options.regionType == EImageStatisticsRegionType::Circle) {
        if (!options.circleRegion.has_value()) {
            result.statusMessage = "Set circle center and opening angle first.";
            return result;
        }
        if (options.circleRegion->radiusDegrees <= 0.0) {
            result.statusMessage = "Circle opening angle must be positive.";
            return result;
        }
        if (!image.contains(options.circleRegion->center)) {
            result.statusMessage = "Circle center must lie inside the current image.";
            return result;
        }
        if (options.axisMode == EImageStatisticsAxisMode::AngleDegrees && !result.hasAngularAxis) {
            result.statusMessage = imageStatisticsAngularAxisMessage(image);
            return result;
        }
        if (!result.hasAngularAxis) {
            result.statusMessage = "Circle statistics require valid VIEW/projection metadata.";
            return result;
        }
        result.circleRegion = options.circleRegion;
        result.overlayPixels = buildCircleOverlayPixels(image, metadata, *options.circleRegion);
    } else {
        if (options.selectionMask.size() != static_cast<size_t>(image.size().x()) * static_cast<size_t>(image.size().y())) {
            result.statusMessage = "Selection mask does not match the current image size.";
            return result;
        }
        const bool hasAny = std::ranges::any_of(options.selectionMask, [](uint8_t value) { return value != 0; });
        if (!hasAny) {
            result.statusMessage = "Selection mask is empty.";
            return result;
        }
    }

    if (options.axisMode == EImageStatisticsAxisMode::AngleDegrees && !result.hasAngularAxis) {
        result.statusMessage = imageStatisticsAngularAxisMessage(image);
        return result;
    }

    {
        const auto resolvedChannels = resolveStatisticsSampleChannels(image, canvas, options.valueMode);
        const bool hasAlphaChannel = resolvedChannels.size() > 1 && tev::Channel::isAlpha(resolvedChannels.back());
        const size_t colorChannelCount = resolvedChannels.size() - (hasAlphaChannel ? 1u : 0u);
        if (options.valueMode == EImageStatisticsValueMode::RGB && colorChannelCount < 3) {
            result.statusMessage = "RGB statistics require three source RGB channels.";
            return result;
        }
    }

    auto samples = gatherSamples(image, canvas, reference, metadata, options);
    if (samples.empty()) {
        result.statusMessage = "No valid pixels found in the selected region.";
        return result;
    }

    result.ok = true;
    result.pixelCount = samples.size();
    result.solidAngle = std::accumulate(samples.begin(), samples.end(), 0.0, [](double sum, const SampleValue& sample) {
        return sum + sample.omega;
    });
    result.selectionMask.assign(static_cast<size_t>(image.size().x()) * static_cast<size_t>(image.size().y()), uint8_t{0});
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    int maxX = std::numeric_limits<int>::min();
    int maxY = std::numeric_limits<int>::min();
    for (const auto& sample : samples) {
        const size_t index = static_cast<size_t>(sample.y) * static_cast<size_t>(image.size().x()) + static_cast<size_t>(sample.x);
        if (index < result.selectionMask.size()) {
            result.selectionMask[index] = 255;
        }
        minX = std::min(minX, sample.x);
        minY = std::min(minY, sample.y);
        maxX = std::max(maxX, sample.x);
        maxY = std::max(maxY, sample.y);
    }
    if (minX <= maxX && minY <= maxY) {
        result.selectionBounds = tev::Box2i{{minX, minY}, {maxX + 1, maxY + 1}};
    }

    if (options.regionType == EImageStatisticsRegionType::Rectangle && result.rectangleRegion.has_value()) {
        const auto region = *result.rectangleRegion;
        result.regionSummary = std::format("{} x {} px", region.size().x(), region.size().y());
        if (result.hasAngularAxis) {
            const int midY = (region.min.y() + region.max.y() - 1) / 2;
            const int midX = (region.min.x() + region.max.x() - 1) / 2;
            const auto leftDir = directionForPixel(image, metadata, region.min.x(), midY);
            const auto rightDir = directionForPixel(image, metadata, region.max.x() - 1, midY);
            const auto topDir = directionForPixel(image, metadata, midX, region.min.y());
            const auto bottomDir = directionForPixel(image, metadata, midX, region.max.y() - 1);
            if (leftDir && rightDir && topDir && bottomDir) {
                result.regionSummary += " | " + formatAnglePair(
                    unsignedAngleDegrees(*leftDir, *rightDir),
                    unsignedAngleDegrees(*topDir, *bottomDir)
                );
            }
        }
    } else if (result.circleRegion.has_value()) {
        result.regionSummary = std::format(
            "center {} {} | radius {:.2f}°",
            result.circleRegion->center.x(),
            result.circleRegion->center.y(),
            result.circleRegion->radiusDegrees
        );
    } else {
        const std::string label = options.selectionLabel.empty() ? "Selection mask" : options.selectionLabel;
        if (result.selectionBounds.has_value()) {
            const auto bounds = *result.selectionBounds;
            result.regionSummary = std::format(
                "{} | {} px | bbox {} x {} px",
                label,
                result.pixelCount,
                bounds.size().x(),
                bounds.size().y()
            );
            if (result.hasAngularAxis) {
                const int midY = (bounds.min.y() + bounds.max.y() - 1) / 2;
                const int midX = (bounds.min.x() + bounds.max.x() - 1) / 2;
                const auto leftDir = directionForPixel(image, metadata, bounds.min.x(), midY);
                const auto rightDir = directionForPixel(image, metadata, bounds.max.x() - 1, midY);
                const auto topDir = directionForPixel(image, metadata, midX, bounds.min.y());
                const auto bottomDir = directionForPixel(image, metadata, midX, bounds.max.y() - 1);
                if (leftDir && rightDir && topDir && bottomDir) {
                    result.regionSummary += " | " + formatAnglePair(
                        unsignedAngleDegrees(*leftDir, *rightDir),
                        unsignedAngleDegrees(*topDir, *bottomDir)
                    );
                }
            }
        } else {
            result.regionSummary = std::format("{} | {} px", label, result.pixelCount);
        }
    }

    result.axisSummary = options.axisMode == EImageStatisticsAxisMode::AngleDegrees && result.hasAngularAxis
        ? "Axis: Angle (deg)"
        : "Axis: Pixels";

    if (options.mode == EImageStatisticsMode::Horizontal || options.mode == EImageStatisticsMode::Both) {
        auto horizontal = buildHorizontalProfile(samples, image, metadata, options);
        if (options.valueMode == EImageStatisticsValueMode::Luminance) {
            result.table.header = {"profile", "index", "axis", "omega", "cum_omega", "value", "cum_value"};
        } else {
            result.table.header = {"profile", "index", "axis", "omega", "cum_omega", "R", "G", "B", "cum_R", "cum_G", "cum_B"};
        }
        appendProfileRows(result.table, "rows", horizontal, options.valueMode);
        result.graphs.push_back(buildProfileGraph(
            "Average of Rows",
            result.regionSummary,
            options.axisMode == EImageStatisticsAxisMode::AngleDegrees ? "Angle (deg)" : "Row (px)",
            std::move(horizontal),
            options.valueMode
        ));
    }

    if (options.mode == EImageStatisticsMode::Vertical || options.mode == EImageStatisticsMode::Both) {
        auto vertical = buildVerticalProfile(samples, image, metadata, options);
        if (result.table.header.empty()) {
            if (options.valueMode == EImageStatisticsValueMode::Luminance) {
                result.table.header = {"profile", "index", "axis", "omega", "cum_omega", "value", "cum_value"};
            } else {
                result.table.header = {"profile", "index", "axis", "omega", "cum_omega", "R", "G", "B", "cum_R", "cum_G", "cum_B"};
            }
        }
        appendProfileRows(result.table, "columns", vertical, options.valueMode);
        result.graphs.push_back(buildProfileGraph(
            "Average of Columns",
            result.regionSummary,
            options.axisMode == EImageStatisticsAxisMode::AngleDegrees ? "Angle (deg)" : "Column (px)",
            std::move(vertical),
            options.valueMode
        ));
    }

    if (options.mode == EImageStatisticsMode::Ranked) {
        std::sort(samples.begin(), samples.end(), [](const SampleValue& a, const SampleValue& b) { return a.sortKey > b.sortKey; });
        if (options.valueMode == EImageStatisticsValueMode::Luminance) {
            result.table.header = {"rank", "x", "y", "omega", "cum_axis", "value", "cum_value"};
        } else {
            result.table.header = {"rank", "x", "y", "omega", "cum_axis", "R", "G", "B", "cum_R", "cum_G", "cum_B"};
        }
        const bool useCumulativeOmega = result.hasAngularAxis;
        appendRankedRows(result.table, samples, useCumulativeOmega, options.valueMode);
        result.graphs.push_back(buildRankedGraph(
            "Ranked pixels",
            result.regionSummary,
            useCumulativeOmega ? "Cumulative Ω (sr)" : "Rank",
            samples,
            useCumulativeOmega,
            options.valueMode
        ));
    }

    result.statusMessage = std::format(
        "{} pixels | Ω {:.6f} sr{}",
        result.pixelCount,
        result.solidAngle,
        result.hasAngularAxis ? "" : " | angular axis unavailable"
    );

    return result;
}

std::vector<uint8_t> renderImageStatisticsGraphsToRgba(
    const ImageStatisticsResult& result,
    EImageStatisticsGraphMode graphMode,
    int width,
    int panelHeight
) {
    if (result.graphs.empty() || width <= 0 || panelHeight <= 0) {
        return {};
    }

    return renderGraphPanelsToRgba(
        result.graphs,
        graphMode,
        width,
        panelHeight * static_cast<int>(result.graphs.size()),
        nanogui::Color(18, 22, 31, 255),
        nullptr
    );
}

std::vector<uint8_t> renderGraphPanelsToRgba(
    std::span<const ImageStatisticsGraph> graphs,
    EImageStatisticsGraphMode graphMode,
    int width,
    int height,
    nanogui::Color backgroundColor,
    const tev::ProfileGraphStyle* styleOverride
) {
    if (graphs.empty() || width <= 0 || height <= 0) {
        return {};
    }

    tev::ProfileGraphStyle style;
    if (styleOverride) {
        style = *styleOverride;
    } else {
        style.backgroundColor = backgroundColor;
        style.transparentBackground = backgroundColor.a() <= 0.001f;
    }
    style.backgroundColor = backgroundColor;
    if (backgroundColor.a() <= 0.001f) {
        style.transparentBackground = true;
    }
    const auto colors = deriveSoftGraphColors(style);

    const int graphSpacing = 12;
    const int graphCount = static_cast<int>(graphs.size());
    const int availableHeight = std::max(1, height - graphSpacing * std::max(0, graphCount - 1));
    const int panelHeight = std::max(1, availableHeight / std::max(1, graphCount));
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    for (size_t i = 0; i < rgba.size() / 4; ++i) {
        rgba[i * 4 + 0] = static_cast<uint8_t>(std::clamp(backgroundColor[0] * 255.0f, 0.0f, 255.0f));
        rgba[i * 4 + 1] = static_cast<uint8_t>(std::clamp(backgroundColor[1] * 255.0f, 0.0f, 255.0f));
        rgba[i * 4 + 2] = static_cast<uint8_t>(std::clamp(backgroundColor[2] * 255.0f, 0.0f, 255.0f));
        rgba[i * 4 + 3] = static_cast<uint8_t>(std::clamp(backgroundColor[3] * 255.0f, 0.0f, 255.0f));
    }

    for (size_t graphIndex = 0; graphIndex < graphs.size(); ++graphIndex) {
        const auto& graph = graphs[graphIndex];
        const auto bounds = computeSoftBounds(graph.series);
        if (!bounds.valid) {
            continue;
        }
        const auto xAxis = buildSoftAxisLayout(bounds.minX, bounds.maxX, style.xTickStep, 6, false, style.xMinOverride, style.xMaxOverride, false);
        const auto yAxis = buildSoftAxisLayout(
            bounds.minY,
            bounds.maxY,
            style.yTickStep,
            5,
            bounds.minY >= 0.0f,
            style.yMinOverride,
            style.yMaxOverride,
            style.yLogScale,
            bounds.minPositiveY
        );

        const int yOffset = static_cast<int>(graphIndex) * (panelHeight + graphSpacing);
        const int titleScale = style.titleFontSize >= 16.0f ? 2 : 1;
        const int subtitleScale = style.subtitleFontSize >= 14.0f ? 2 : 1;
        const int axisScale = style.xAxisFontSize >= 14.0f ? 2 : 1;

        const int maxYTickWidth = [&]() {
            int maxWidth = 0;
            for (float tick : yAxis.ticks) {
                maxWidth = std::max(maxWidth, graphTextWidth(formatProfileNumberSoft(tick), axisScale));
            }
            return maxWidth;
        }();

        const int top = 16 + titleScale * 8 + (!graph.subtitle.empty() ? (subtitleScale * 8 + 8) : 0);
        const int left = std::max(56, maxYTickWidth + (graph.yLabel.empty() ? 14 : (axisScale * 8 + 22)));
        const int right = 14;
        const int bottom = std::max(34, axisScale * 18 + (!graph.xLabel.empty() ? 16 : 8));
        const int x0 = left;
        const int y0 = yOffset + top;
        const int x1 = width - right - 1;
        const int y1 = yOffset + panelHeight - bottom - 1;
        const int plotWidth = std::max(1, x1 - x0);
        const int plotHeight = std::max(1, y1 - y0);

        if (!style.transparentBackground) {
            for (int y = yOffset + 1; y < yOffset + panelHeight - 1; ++y) {
                for (int x = 1; x < width - 1; ++x) {
                    blendPixel(rgba, width, height, x, y, {
                        static_cast<uint8_t>(std::clamp(colors.panelBackground.r() * 255.0f, 0.0f, 255.0f)),
                        static_cast<uint8_t>(std::clamp(colors.panelBackground.g() * 255.0f, 0.0f, 255.0f)),
                        static_cast<uint8_t>(std::clamp(colors.panelBackground.b() * 255.0f, 0.0f, 255.0f)),
                        static_cast<uint8_t>(std::clamp(colors.panelBackground.a() * 255.0f, 0.0f, 255.0f))
                    });
                }
            }
        }

        if (!style.transparentBackground) {
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    blendPixel(rgba, width, height, x, y, {
                        static_cast<uint8_t>(std::clamp(colors.plotBackground.r() * 255.0f, 0.0f, 255.0f)),
                        static_cast<uint8_t>(std::clamp(colors.plotBackground.g() * 255.0f, 0.0f, 255.0f)),
                        static_cast<uint8_t>(std::clamp(colors.plotBackground.b() * 255.0f, 0.0f, 255.0f)),
                        static_cast<uint8_t>(std::clamp(colors.plotBackground.a() * 255.0f, 0.0f, 255.0f))
                    });
                }
            }
        }

        for (size_t i = 0; i < yAxis.ticks.size(); ++i) {
            const float tick = yAxis.ticks[i];
            const int py = [&]() {
                if (style.yLogScale) {
                    if (tick <= 0.0f || yAxis.minValue <= 0.0f || yAxis.maxValue <= yAxis.minValue) {
                        return y1;
                    }
                    const float minLog = std::log10(yAxis.minValue);
                    const float maxLog = std::log10(yAxis.maxValue);
                    const float valueLog = std::log10(tick);
                    return y1 - static_cast<int>(std::round(((valueLog - minLog) / (maxLog - minLog)) * static_cast<float>(plotHeight)));
                }
                return y1 - static_cast<int>(std::round(((tick - yAxis.minValue) / (yAxis.maxValue - yAxis.minValue)) * static_cast<float>(plotHeight)));
            }();
            const auto c = (i == 0 || i + 1 == yAxis.ticks.size()) ? colors.gridMajor : colors.gridMinor;
            drawLine(rgba, width, height, x0, py, x1, py, {
                static_cast<uint8_t>(std::clamp(c.r() * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(c.g() * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(c.b() * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(c.a() * 255.0f, 0.0f, 255.0f))
            });
        }

        for (size_t i = 0; i < xAxis.ticks.size(); ++i) {
            const float tick = xAxis.ticks[i];
            const int px = x0 + static_cast<int>(std::round(((tick - xAxis.minValue) / (xAxis.maxValue - xAxis.minValue)) * static_cast<float>(plotWidth)));
            const auto c = (i == 0 || i + 1 == xAxis.ticks.size()) ? colors.gridMajor : colors.gridMinor;
            drawLine(rgba, width, height, px, y0, px, y1, {
                static_cast<uint8_t>(std::clamp(c.r() * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(c.g() * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(c.b() * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(c.a() * 255.0f, 0.0f, 255.0f))
            });
        }

        const auto border = std::array<uint8_t, 4>{
            static_cast<uint8_t>(std::clamp(colors.border.r() * 255.0f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(colors.border.g() * 255.0f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(colors.border.b() * 255.0f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(colors.border.a() * 255.0f, 0.0f, 255.0f))
        };
        drawLine(rgba, width, height, x0, y0, x1, y0, border);
        drawLine(rgba, width, height, x0, y1, x1, y1, border);
        drawLine(rgba, width, height, x0, y0, x0, y1, border);
        drawLine(rgba, width, height, x1, y0, x1, y1, border);

        const auto mapX = [&](float value) {
            return x0 + static_cast<int>(std::round(((value - xAxis.minValue) / (xAxis.maxValue - xAxis.minValue)) * static_cast<float>(plotWidth)));
        };
        const auto mapY = [&](float value) {
            if (style.yLogScale) {
                if (value <= 0.0f || yAxis.minValue <= 0.0f || yAxis.maxValue <= yAxis.minValue) {
                    return y1;
                }
                const float minLog = std::log10(yAxis.minValue);
                const float maxLog = std::log10(yAxis.maxValue);
                const float valueLog = std::log10(value);
                return y1 - static_cast<int>(std::round(((valueLog - minLog) / (maxLog - minLog)) * static_cast<float>(plotHeight)));
            }
            return y1 - static_cast<int>(std::round(((value - yAxis.minValue) / (yAxis.maxValue - yAxis.minValue)) * static_cast<float>(plotHeight)));
        };

        for (const auto& series : graph.series) {
            const auto rgb = paletteColorRgb(series.color);
            const std::array<uint8_t, 4> color = {
                static_cast<uint8_t>(std::clamp(rgb[0] * 255.0, 0.0, 255.0)),
                static_cast<uint8_t>(std::clamp(rgb[1] * 255.0, 0.0, 255.0)),
                static_cast<uint8_t>(std::clamp(rgb[2] * 255.0, 0.0, 255.0)),
                255,
            };
            const size_t count = std::min(series.x.size(), series.y.size());
            if (count == 0) {
                continue;
            }
            if (graphMode != EImageStatisticsGraphMode::Scatter) {
                for (size_t i = 1; i < count; ++i) {
                    drawLine(
                        rgba,
                        width,
                        height,
                        mapX(series.x[i - 1]),
                        mapY(series.y[i - 1]),
                        mapX(series.x[i]),
                        mapY(series.y[i]),
                        color
                    );
                }
            }
            if (graphMode != EImageStatisticsGraphMode::Line) {
                for (size_t i = 0; i < count; ++i) {
                    fillCircle(rgba, width, height, mapX(series.x[i]), mapY(series.y[i]), 2, {color[0], color[1], color[2], 230});
                }
            }
        }

        const auto primaryText = std::array<uint8_t, 4>{
            static_cast<uint8_t>(std::clamp(colors.primaryText.r() * 255.0f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(colors.primaryText.g() * 255.0f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(colors.primaryText.b() * 255.0f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(colors.primaryText.a() * 255.0f, 0.0f, 255.0f))
        };
        const auto secondaryText = std::array<uint8_t, 4>{
            static_cast<uint8_t>(std::clamp(colors.secondaryText.r() * 255.0f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(colors.secondaryText.g() * 255.0f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(colors.secondaryText.b() * 255.0f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(colors.secondaryText.a() * 255.0f, 0.0f, 255.0f))
        };
        drawGraphTextBitmap(rgba, width, height, 10, yOffset + 6, graph.title, primaryText, titleScale);
        if (!graph.subtitle.empty()) {
            drawGraphTextBitmap(rgba, width, height, 10, yOffset + 8 + titleScale * 8, graph.subtitle, secondaryText, subtitleScale);
        }
        if (!graph.xLabel.empty()) {
            const std::string label = sanitizeGraphText(graph.xLabel);
            drawGraphTextBitmap(
                rgba,
                width,
                height,
                x0 + std::max(0, (plotWidth - graphTextWidth(label, axisScale)) / 2),
                y1 + 10,
                label,
                secondaryText,
                axisScale
            );
        }
        if (!graph.yLabel.empty()) {
            const std::string label = sanitizeGraphText(graph.yLabel);
            const int labelHeight = static_cast<int>(label.size()) * 8 * axisScale;
            drawGraphVerticalTextBitmap(
                rgba,
                width,
                height,
                10,
                y0 + std::max(0, (plotHeight - labelHeight) / 2),
                label,
                secondaryText,
                axisScale
            );
        }
        for (float tick : xAxis.ticks) {
            const std::string label = formatProfileNumberSoft(tick);
            const int px = mapX(tick) - graphTextWidth(label, axisScale) / 2;
            drawGraphTextBitmap(rgba, width, height, px, y1 + 4, label, primaryText, axisScale);
        }
        for (float tick : yAxis.ticks) {
            const std::string label = formatProfileNumberSoft(tick);
            const int py = mapY(tick) - 3 * axisScale;
            drawGraphTextBitmap(rgba, width, height, x0 - 6 - graphTextWidth(label, axisScale), py, label, primaryText, axisScale);
        }
        if (graph.series.size() > 1) {
            int legendY = yOffset + 8;
            for (auto it = graph.series.rbegin(); it != graph.series.rend(); ++it) {
                const auto rgb = paletteColorRgb(it->color);
                const auto swatch = std::array<uint8_t, 4>{
                    static_cast<uint8_t>(std::clamp(rgb[0] * 255.0, 0.0, 255.0)),
                    static_cast<uint8_t>(std::clamp(rgb[1] * 255.0, 0.0, 255.0)),
                    static_cast<uint8_t>(std::clamp(rgb[2] * 255.0, 0.0, 255.0)),
                    255
                };
                const std::string label = sanitizeGraphText(it->label);
                const int labelWidth = graphTextWidth(label, 1);
                for (int yy = 0; yy < 7; ++yy) {
                    for (int xx = 0; xx < 10; ++xx) {
                        blendPixel(rgba, width, height, width - 12 - labelWidth - 14 + xx, legendY + yy, swatch);
                    }
                }
                drawGraphTextBitmap(rgba, width, height, width - 12 - labelWidth, legendY, label, secondaryText, 1);
                legendY += 12;
            }
        }
    }

    return rgba;
}

void saveImageStatisticsCsv(const std::filesystem::path& path, const ImageStatisticsResult& result) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Could not open statistics CSV for writing.");
    }

    output << "# " << result.statusMessage << "\n";
    output << "# " << result.regionSummary << "\n";
    output << "# " << result.axisSummary << "\n";
    for (size_t i = 0; i < result.table.header.size(); ++i) {
        if (i != 0) {
            output << ",";
        }
        output << csvEscape(result.table.header[i]);
    }
    output << "\n";

    for (const auto& row : result.table.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i != 0) {
                output << ",";
            }
            output << csvEscape(row[i]);
        }
        output << "\n";
    }
}

} // namespace native
