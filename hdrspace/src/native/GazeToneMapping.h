#pragma once

#include <tev/Image.h>

#include <nanogui/vector.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace native {

enum class EGazeMeteringMode {
    UniformAverage,
    CenterWeighted,
    GaussianGazeSpot,
};

enum class EGazeProjection {
    Auto,
    AngularFisheye,
    HemisphericalFisheye,
    Perspective,
};

enum class EGazeInputEncoding {
    Auto,
    RadianceRgb,
    RadianceXyze,
    LinearRec709Rgb,
    SrgbEncodedRgb,
    CieXyz,
    LuminanceY,
};

enum class EGazeOutputMapping {
    Clipping,
    Reinhard,
};

struct GazeProjectionInfo {
    EGazeProjection projection = EGazeProjection::Perspective;
    double horizontalDegrees = 180.0;
    double verticalDegrees = 180.0;
    std::string source;
};

struct GazeLuminanceSettings {
    EGazeInputEncoding encoding = EGazeInputEncoding::Auto;
    // Final multiplier from decoded Y to cd/m². No value uses the mode/header default.
    std::optional<double> scaleOverride;
};

struct GazeLuminanceInfo {
    EGazeInputEncoding requestedEncoding = EGazeInputEncoding::Auto;
    EGazeInputEncoding resolvedEncoding = EGazeInputEncoding::Auto;
    double luminanceScale = 1.0;
    bool absoluteLuminance = false;
    bool usedSourceChannels = false;
    std::string description;
    std::string scaleSource;
};

struct GazeDisplayGeometry {
    bool enabled = false;
    int pixelWidth = 0;
    int pixelHeight = 0;
    double pixelsPerInch = 0.0;
    double viewingDistanceMm = 0.0;
};

struct GazeAdaptationOptions {
    EGazeMeteringMode meteringMode = EGazeMeteringMode::GaussianGazeSpot;
    nanogui::Vector2i gazePoint = {0, 0};
    double gaussianSigmaDegrees = 2.0;
    double centerSigmaDegrees = 45.0;
    double targetGrey = 0.18;
    double displayScale = 1.0;
    bool protectGazeHighlights = false;
    double highlightHeadroom = 0.95;
    double highlightRadiusSigmas = 3.0;
    EGazeOutputMapping outputMapping = EGazeOutputMapping::Clipping;
    // When enabled, degree-based weighting is measured at the viewer's eye
    // after fitting the image to the physical display. Otherwise, source-image
    // projection angles are used.
    GazeDisplayGeometry displayGeometry;
};

struct GazeAdaptationResult {
    bool ok = false;
    double adaptationLuminance = 0.0;
    double adaptationSignal = 0.0;
    double gazeHighlightLuminance = 0.0;
    double gazeHighlightPeakSignal = 0.0;
    double unconstrainedExposureEv = 0.0;
    double highlightExposureLimitEv = 0.0;
    double targetExposureEv = 0.0;
    double weightSum = 0.0;
    size_t sampleCount = 0;
    size_t highlightSampleCount = 0;
    bool highlightProtectionRequested = false;
    bool highlightProtectionActive = false;
    nanogui::Vector2i gazePoint = {0, 0};
    GazeProjectionInfo projection;
    GazeLuminanceInfo luminance;
    std::string channelDescription;
    std::string statusMessage;
};

struct LuminanceDistributionThreshold {
    double luminance = 0.0;
    size_t pixelCountAtOrAbove = 0;
    double fractionAtOrAbove = 0.0;
};

struct LuminanceDistributionTail {
    double topFraction = 0.0;
    double thresholdLuminance = 0.0;
    double luminanceSumFraction = 0.0;
};

struct LuminanceDistributionOptions {
    bool autoCropLeftEye = true;
    bool excludeOutsideFisheye = true;
    size_t graphPointCount = 4097;
    std::vector<double> thresholds = {
        0.01, 0.1, 1.0, 10.0, 50.0, 100.0, 200.0,
        300.0, 500.0, 750.0, 1000.0, 3000.0, 5000.0, 10000.0,
    };
    std::vector<double> tailFractions = {
        0.0001, 0.0005, 0.001, 0.002, 0.005, 0.01, 0.02,
    };
};

struct LuminanceDistributionResult {
    bool ok = false;
    bool croppedLeftEye = false;
    bool excludedOutsideFisheye = false;
    size_t pixelCount = 0;
    double minimumLuminance = 0.0;
    double maximumLuminance = 0.0;
    double meanLuminance = 0.0;
    GazeProjectionInfo projection;
    GazeLuminanceInfo luminance;
    std::string channelDescription;
    std::string statusMessage;
    std::vector<float> quantile;
    std::vector<float> luminanceAtQuantile;
    std::vector<LuminanceDistributionThreshold> thresholds;
    std::vector<LuminanceDistributionTail> tails;
};

class GazeAdaptationSampler {
public:
    bool rebuild(
        const tev::Image& image,
        std::string_view channelGroup,
        EGazeProjection projectionOverride = EGazeProjection::Auto,
        const GazeLuminanceSettings& luminanceSettings = {},
        int maximumSamplesPerAxis = 512
    );

    GazeAdaptationResult evaluate(const GazeAdaptationOptions& options) const;

    bool valid() const { return !samples_.empty(); }
    nanogui::Vector2i imageSize() const { return imageSize_; }
    size_t sampleCount() const { return samples_.size(); }
    const GazeProjectionInfo& projectionInfo() const { return projectionInfo_; }
    const GazeLuminanceInfo& luminanceInfo() const { return luminanceInfo_; }
    std::string_view channelDescription() const { return channelDescription_; }
    std::string_view statusMessage() const { return statusMessage_; }

private:
    struct Sample {
        double signal = 0.0;
        double peakSignal = 0.0;
        double luminance = 0.0;
        double logSignal = 0.0;
        double logLuminance = 0.0;
        nanogui::Vector3f direction = {0.0f, 1.0f, 0.0f};
        nanogui::Vector2i pixel = {0, 0};
    };

    bool directionForPixel(nanogui::Vector2i pixel, nanogui::Vector3f& direction) const;

    nanogui::Vector2i imageSize_ = {0, 0};
    GazeProjectionInfo projectionInfo_;
    GazeLuminanceInfo luminanceInfo_;
    std::string channelDescription_;
    std::string statusMessage_;
    std::vector<Sample> samples_;
};

LuminanceDistributionResult computeLuminanceDistribution(
    const tev::Image& image,
    std::string_view channelGroup,
    EGazeProjection projectionOverride = EGazeProjection::Auto,
    const GazeLuminanceSettings& luminanceSettings = {},
    const LuminanceDistributionOptions& options = {}
);

std::string_view gazeMeteringModeLabel(EGazeMeteringMode mode);
std::string_view gazeProjectionLabel(EGazeProjection projection);
std::string_view gazeInputEncodingLabel(EGazeInputEncoding encoding);

} // namespace native
