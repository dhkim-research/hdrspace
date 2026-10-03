/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <tev/Box.h>
#include <tev/Common.h>
#include <tev/Image.h>
#include <tev/Lazy.h>
#include <tev/UberShader.h>

#include <nanogui/canvas.h>

#include <memory>
#include <optional>
#include <unordered_map>

namespace tev {

enum class EHighlightMode {
    Rgb = 0,
    Red = 1,
    Green = 2,
    Blue = 3,
    Single = 4,
};

enum class EInspectionValueMode {
    Channels = 0,
    Luminance = 1,
    YChannel = 2,
};

enum class EFalseColorMap {
    Turbo = 0,
    Viridis = 1,
};

enum class EFalseColorScaleMode {
    Linear = 0,
    Log2 = 1,
    Log3 = 2,
    Log4 = 3,
    Log5 = 4,
};

struct CanvasStatistics {
    float mean = 0.0f;
    float maximum = 0.0f;
    float minimum = 0.0f;
    float stddev = 0.0f;
    std::vector<float> histogram;
    std::vector<nanogui::Color> histogramColors;
    std::vector<float> channelMean;
    std::vector<float> channelMaximum;
    std::vector<float> channelMinimum;
    std::vector<float> channelStdDev;
    int nChannels = 0;
    int histogramZero = 0;
    Box2i region = Box2i{0};
};

class ImageCanvas final : public nanogui::Canvas {
public:
    ImageCanvas(nanogui::Widget* parent);

    bool scroll_event(const nanogui::Vector2i& p, const nanogui::Vector2f& rel) override;

    void draw_contents() override;

    void draw(NVGcontext* ctx) override;

    void translate(nanogui::Vector2f amount);
    void scale(float amount, nanogui::Vector2f origin);
    float scale() const { return extractScale(mTransform); }

    void setExposure(float exposure) { mExposure = exposure; }
    void setOffset(float offset) { mOffset = offset; }
    void setGamma(float gamma) { mGamma = gamma; }

    void setImage(std::shared_ptr<Image> image);
    void setReference(std::shared_ptr<Image> reference);
    void setRequestedChannelGroup(std::string_view groupName);
    std::string_view requestedChannelGroup() const { return mRequestedChannelGroup; }
    void setHighlightChannelGroup(std::string_view groupName);

    nanogui::Vector2i getImageCoords(const Image* image, nanogui::Vector2i mousePos);
    nanogui::Vector2i getDisplayWindowCoords(const Image* image, nanogui::Vector2i mousePos);

    void getValuesAtNanoPos(nanogui::Vector2i nanoPos, std::vector<float>& result, std::span<std::string_view> channels);
    std::vector<float> getValuesAtNanoPos(nanogui::Vector2i nanoPos, std::span<std::string_view> channels) {
        std::vector<float> result;
        getValuesAtNanoPos(nanoPos, result, channels);
        return result;
    }
    void getDisplayedValuesAtNanoPos(nanogui::Vector2i nanoPos, std::vector<float>& result, std::span<std::string_view> channels);
    std::vector<float> getDisplayedValuesAtNanoPos(nanogui::Vector2i nanoPos, std::span<std::string_view> channels) {
        std::vector<float> result;
        getDisplayedValuesAtNanoPos(nanoPos, result, channels);
        return result;
    }

    ETonemap tonemap() const { return mTonemap; }
    void setTonemap(ETonemap tonemap) { mTonemap = tonemap; }

    EMetric metric() const { return mMetric; }
    void setMetric(EMetric metric) { mMetric = metric; }

    bool areChannelsMasked(EChannelMask mask) const { return hasFlag(mChannelMask, mask); }
    EChannelMask channelMask() const { return mChannelMask; }
    void setChannelMask(EChannelMask mask) { mChannelMask = mask; }

    const std::optional<Box2i>& crop() const & { return mCrop; }
    void setCrop(const std::optional<Box2i>& crop);
    Box2i cropInImageCoords() const;
    const std::optional<Box2i>& roi() const & { return mRoi; }
    void setRoi(const std::optional<Box2i>& roi);
    Box2i roiInImageCoords() const;
    Box2i statisticsRegionInImageCoords() const;

    void fitImageToScreen(const Image& image);
    void resetTransform();

    std::optional<float> whiteLevelOverride() const { return mWhiteLevelOverride; }
    void setWhiteLevelOverride(std::optional<float> value) { mWhiteLevelOverride = value; }

    bool clipToLdr() const { return mClipToLdr; }
    void setClipToLdr(bool value) { mClipToLdr = value; }

    auto backgroundColor() { return mBackgroundColor; }
    auto backgroundColor() const { return mBackgroundColor; }
    void setBackgroundColor(const nanogui::Color& color) { mBackgroundColor = color; }

    EInterpolationMode minFilter() const { return mMinFilter; }
    void setMinFilter(EInterpolationMode value) { mMinFilter = value; }

    EInterpolationMode magFilter() const { return mMagFilter; }
    void setMagFilter(EInterpolationMode value) { mMagFilter = value; }

    // The following functions return four values per pixel in RGBA order. The number of pixels is given by `imageDataSize()`. If the canvas
    // does not currently hold an image, or no channels are displayed, then zero pixels are returned.
    nanogui::Vector2i imageDataSize() const { return cropInImageCoords().size(); }
    Task<HeapArray<float>> getRgbaHdrImageData(bool divideAlpha, int priority) const;
    Task<HeapArray<uint8_t>> getRgbaLdrImageData(bool divideAlpha, int priority) const;
    Task<HeapArray<uint8_t>> getFullImageRgbaLdrImageData(bool divideAlpha, int priority) const;
    Task<HeapArray<uint8_t>> getFullImageRgbaLdrImageDataWithOverrides(
        bool divideAlpha,
        ETonemap tonemap,
        float gamma,
        float exposure,
        float offset,
        int priority
    ) const;

    void saveImage(const fs::path& filename) const;

    std::shared_ptr<Lazy<std::shared_ptr<CanvasStatistics>>> canvasStatistics();

    void purgeCanvasStatistics(size_t imageId);

    float pixelRatio() const { return mPixelRatio; }
    void setPixelRatio(float ratio) { mPixelRatio = ratio; }

    chroma_t inspectionChroma() const { return mInspectionChroma; }
    void setInspectionChroma(const chroma_t& chroma);

    ituth273::ETransfer inspectionTransfer() const { return mInspectionTransfer; }
    void setInspectionTransfer(const ituth273::ETransfer transfer);

    bool inspectionAdaptWhitePoint() const { return mInspectionAdaptWhitePoint; }
    void setInspectionAdaptWhitePoint(bool adapt);

    bool inspectionPremultipliedAlpha() const { return mInspectionPremultipliedAlpha; }
    void setInspectionPremultipliedAlpha(bool premultipied);

    float inspectionScale() const { return mInspectionScale; }
    void setInspectionScale(float scale);
    EInspectionValueMode inspectionValueMode() const { return mInspectionValueMode; }
    void setInspectionValueMode(EInspectionValueMode mode);
    int inspectionDisplayChannelSelection() const { return mInspectionDisplayChannelSelection; }
    void setInspectionDisplayChannelSelection(int selection);
    void setInspectionMatrixOverride(const std::optional<nanogui::Matrix3f>& matrix);
    void setInspectionPreviewImage(const std::shared_ptr<Image>& image);
    void clearInspectionPreviewImage();
    bool displayInspectionInPreview() const { return mDisplayInspectionInPreview; }
    void setDisplayInspectionInPreview(bool value);
    EFalseColorMap falseColorMap() const { return mFalseColorMap; }
    void setFalseColorMap(EFalseColorMap map);
    EFalseColorScaleMode falseColorScaleMode() const { return mFalseColorScaleMode; }
    void setFalseColorScaleMode(EFalseColorScaleMode mode);
    nanogui::Vector2f falseColorRange() const { return mFalseColorRange; }
    void setFalseColorRange(nanogui::Vector2f range);

    const std::optional<nanogui::Vector2f>& highlightValueRange() const { return mHighlightValueRange; }
    void setHighlightValueRange(const std::optional<nanogui::Vector2f>& range);
    EHighlightMode highlightMode() const { return mHighlightMode; }
    void setHighlightMode(EHighlightMode mode);
    void setAnalysisOverlay(std::shared_ptr<Image> image, std::shared_ptr<PixelBuffer> pixels);
    void clearAnalysisOverlay();
    void setAiMaskPromptPoints(std::vector<nanogui::Vector2i> positivePoints, std::vector<nanogui::Vector2i> negativePoints);
    void clearAiMaskPromptPoints();

    // Assumes the alpha channel is the last one, if present
    void applyInspectionParameters(std::vector<float>& values, bool hasAlpha);

private:
    static Task<std::shared_ptr<CanvasStatistics>> computeCanvasStatistics(
        std::shared_ptr<Image> image,
        std::shared_ptr<Image> reference,
        std::string_view requestedChannelGroup,
        EMetric metric,
        Box2i region,
        const nanogui::Matrix3f& inspectionMatrix,
        ituth273::ETransfer transfer,
        bool premultipliedAlpha,
        float inspectionScale,
        EInspectionValueMode inspectionValueMode,
        int priority
    );

    void invalidateCanvasStatistics();
    void invalidateStatisticsOnly();
    void invalidateInspectionPreviewCache();
    void invalidateHighlightMaskCache();

    void drawPixelValuesAsText(NVGcontext* ctx);
    void drawCoordinateSystem(NVGcontext* ctx);
    void drawAiMaskPromptPoints(NVGcontext* ctx);
    void drawEdgeShadows(NVGcontext* ctx);
    nanogui::Matrix3f inspectionMatrix() const;
    nanogui::Texture* inspectionPreviewTexture(const std::shared_ptr<Image>& image);
    bool sampleInspectionPreviewAtImageCoords(const std::shared_ptr<Image>& image, nanogui::Vector2i imageCoords, nanogui::Vector4f& result);
    nanogui::Texture* highlightMaskTexture();
    std::shared_ptr<PixelBuffer> createInspectionPreviewPixels(const std::shared_ptr<Image>& image, std::string_view requestedGroup, int displayChannelSelection);
    nanogui::Texture* analysisOverlayTexture(const std::shared_ptr<Image>& image);

    nanogui::Vector2f pixelOffset(nanogui::Vector2i size) const;

    // Assembles the transform from canonical space to the [-1, 1] square for the current image.
    nanogui::Matrix3f transform(const Image* image);
    nanogui::Matrix3f textureToNanogui(const Image* image);
    nanogui::Matrix3f displayWindowToNanogui(const Image* image);

    float mPixelRatio = 1;
    float mExposure = 0;
    float mOffset = 0;
    float mGamma = 2.2f;

    std::optional<float> mWhiteLevelOverride = std::nullopt;
    bool mClipToLdr = false;
    nanogui::Color mBackgroundColor = nanogui::Color(0, 0, 0, 0);

    EInterpolationMode mMinFilter = EInterpolationMode::Trilinear;
    EInterpolationMode mMagFilter = EInterpolationMode::Nearest;

    std::shared_ptr<Image> mImage;
    std::shared_ptr<Image> mReference;

    std::string mRequestedChannelGroup = "";
    std::string mHighlightChannelGroup = "";

    nanogui::Matrix3f mTransform = nanogui::Matrix3f::scale(nanogui::Vector3f(1.0f));

    std::unique_ptr<UberShader> mShader;

    EChannelMask mChannelMask = EChannelMask::Red | EChannelMask::Green | EChannelMask::Blue | EChannelMask::Alpha;
    ETonemap mTonemap = ETonemap::None;
    EMetric mMetric = EMetric::Error;
    std::optional<Box2i> mCrop;
    std::optional<Box2i> mRoi;
    std::unordered_map<std::string, std::shared_ptr<Lazy<std::shared_ptr<CanvasStatistics>>>> mCanvasStatistics;
    std::unordered_map<int, std::vector<std::string>> mImageIdToCanvasStatisticsKey;

    chroma_t mInspectionChroma = rec709Chroma();
    ituth273::ETransfer mInspectionTransfer = ituth273::ETransfer::Linear;
    bool mInspectionAdaptWhitePoint = false;
    bool mInspectionPremultipliedAlpha = true;
    float mInspectionScale = 1.0f;
    EInspectionValueMode mInspectionValueMode = EInspectionValueMode::Channels;
    int mInspectionDisplayChannelSelection = 0;
    std::optional<nanogui::Matrix3f> mInspectionMatrixOverride;
    std::shared_ptr<Image> mInspectionPreviewImage;
    bool mDisplayInspectionInPreview = false;
    EFalseColorMap mFalseColorMap = EFalseColorMap::Turbo;
    EFalseColorScaleMode mFalseColorScaleMode = EFalseColorScaleMode::Log2;
    nanogui::Vector2f mFalseColorRange = {0.03125f, 32.0f};
    std::optional<nanogui::Vector2f> mHighlightValueRange;
    EHighlightMode mHighlightMode = EHighlightMode::Rgb;

    struct InspectionPreviewCache {
        std::shared_ptr<Image> image;
        std::string requestedGroup;
        nanogui::Matrix3f matrix = nanogui::Matrix3f{1.0f};
        ituth273::ETransfer transfer = ituth273::ETransfer::Linear;
        bool premultipliedAlpha = true;
        float scale = 1.0f;
        EInspectionValueMode valueMode = EInspectionValueMode::Channels;
        int displayChannelSelection = 0;
        std::shared_ptr<PixelBuffer> pixels;
        nanogui::ref<nanogui::Texture> texture;
    };

    InspectionPreviewCache mInspectionPreviewCache;
    InspectionPreviewCache mInspectionReferencePreviewCache;

    struct HighlightMaskCache {
        std::shared_ptr<Image> image;
        std::shared_ptr<Image> reference;
        std::string requestedGroup;
        std::string highlightGroup;
        EMetric metric = EMetric::Error;
        std::optional<nanogui::Vector2f> range;
        EHighlightMode mode = EHighlightMode::Rgb;
        nanogui::Matrix3f matrix = nanogui::Matrix3f{1.0f};
        ituth273::ETransfer transfer = ituth273::ETransfer::Linear;
        bool premultipliedAlpha = true;
        float scale = 1.0f;
        EInspectionValueMode valueMode = EInspectionValueMode::Channels;
        int displayChannelSelection = 0;
        nanogui::ref<nanogui::Texture> texture;
    };

    HighlightMaskCache mHighlightMaskCache;

    struct AnalysisOverlayCache {
        std::shared_ptr<Image> image;
        std::shared_ptr<PixelBuffer> pixels;
        nanogui::ref<nanogui::Texture> texture;
    };

    AnalysisOverlayCache mAnalysisOverlayCache;

    struct AiMaskPromptPoint {
        nanogui::Vector2i imageCoords = {0, 0};
        bool positive = true;

        bool operator==(const AiMaskPromptPoint& other) const {
            return imageCoords == other.imageCoords && positive == other.positive;
        }
    };

    std::vector<AiMaskPromptPoint> mAiMaskPromptPoints;
};

} // namespace tev
