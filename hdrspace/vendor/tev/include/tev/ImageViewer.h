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

#include <tev/BackgroundImagesLoader.h>
#include <tev/HelpWindow.h>
#include <tev/Image.h>
#include <tev/ImageButton.h>
#include <tev/ImageCanvas.h>
#include <tev/ImageInfoWindow.h>
#include <tev/Ipc.h>
#include <tev/Lazy.h>
#include <tev/MultiGraph.h>
#include <tev/SharedQueue.h>
#include <tev/VectorGraphics.h>
#include <tev/imageio/Colors.h>

#include <nanogui/button.h>
#include <nanogui/colorwheel.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/slider.h>
#include <nanogui/textbox.h>

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

namespace tev {

class ImageViewer final : public nanogui::Screen {
public:
    ImageViewer(
        nanogui::Vector2i size,
        const std::shared_ptr<BackgroundImagesLoader>& imagesLoader,
        std::weak_ptr<Ipc> ipc,
        bool maximize,
        bool showUi,
        bool floatBuffer,
        bool sidebarOnRight = false
    );

    bool resize_event(const nanogui::Vector2i& size) override;

    using nanogui::Screen::perform_layout;
    void perform_layout(NVGcontext* ctx) override;

    bool mouse_button_event(const nanogui::Vector2i& p, int button, bool down, int modifiers) override;
    bool mouse_motion_event_f(const nanogui::Vector2f& p, const nanogui::Vector2f& rel, int button, int modifiers) override;

    bool drop_event(const std::vector<std::string>& filenames) override;

    bool keyboard_event(int key, int scancode, int action, int modifiers) override;

    void focusWindow();

    void draw_contents() override;

    void updateColorCapabilities();

    void insertImage(std::shared_ptr<Image> image, size_t index, bool shallSelect = false);
    void moveImageInList(size_t oldIndex, size_t newIndex);

    bool hasImageWithName(std::string_view imageName) { return !!imageByName(imageName); }
    std::shared_ptr<Image> imageByPath(const fs::path& imagePath);
    bool selectImageByPath(const fs::path& imagePath);

    void addImage(std::shared_ptr<Image> image, bool shallSelect = false) { insertImage(image, mImages.size(), shallSelect); }

    void removeImage(std::shared_ptr<Image> image);
    void removeImage(std::string_view imageName) { removeImage(imageByName(imageName)); }
    void removeAllImages();

    void replaceImage(std::shared_ptr<Image> image, std::shared_ptr<Image> replacement, bool shallSelect);
    void replaceImage(std::string_view imageName, std::shared_ptr<Image> replacement, bool shallSelect) {
        replaceImage(imageByName(imageName), replacement, shallSelect);
    }

    void reloadImage(std::shared_ptr<Image> image, bool shallSelect = false);
    void reloadImage(std::string_view imageName, bool shallSelect = false) { reloadImage(imageByName(imageName), shallSelect); }
    void reloadAllImages();
    void reloadImagesWhoseFileChanged();

    void updateImage(std::string_view imageName, bool shallSelect, std::string_view channel, Box2i bounds, std::span<const float> imageData);

    void updateImageVectorGraphics(std::string_view imageName, bool shallSelect, bool append, std::span<const VgCommand> commands);

    void selectImage(const std::shared_ptr<Image>& image, bool stopPlayback = true);

    void selectGroup(std::string_view name);

    void selectReference(const std::shared_ptr<Image>& image);

    float exposure() const { return mExposureSlider->value(); }

    void setExposure(float value);
    void setExposurePrecise(float value);

    float offset() const { return mOffsetSlider->value(); }

    void setOffset(float value);
    void updateDockedSliderWidths();

    float gamma() const { return mGammaSlider->value(); }

    void setGamma(float value);

    void normalizeExposureAndOffset();

    void resetImage();

    void ungroupCurrentChannelGroup();

    EInterpolationMode minFilter() const { return mImageCanvas->minFilter(); }
    void setMinFilter(EInterpolationMode value) { mImageCanvas->setMinFilter(value); }

    EInterpolationMode magFilter() const { return mImageCanvas->magFilter(); }
    void setMagFilter(EInterpolationMode value) { mImageCanvas->setMagFilter(value); }

    ETonemap tonemap() const { return mImageCanvas->tonemap(); }
    void setTonemap(ETonemap tonemap);

    EMetric metric() const { return mImageCanvas->metric(); }
    void setMetric(EMetric metric);

    bool areChannelsMasked(EChannelMask mask) const { return mImageCanvas->areChannelsMasked(mask); };
    EChannelMask channelMask() const { return mImageCanvas->channelMask(); }
    void setChannelMask(EChannelMask channel, bool state);

    void setBackgroundColorStraight(nanogui::Color color);

    float displayWhiteLevel() const;
    void setDisplayWhiteLevel(float value);
    void setDisplayWhiteLevelToImageMetadata();
    void setImageWhiteLevel(float value);

    enum class EDisplayWhiteLevelSetting {
        System = 0,
        Custom = 1,
        ImageMetadata = 2,
    };

    EDisplayWhiteLevelSetting displayWhiteLevelSetting() const;
    void setDisplayWhiteLevelSetting(EDisplayWhiteLevelSetting value);

    nanogui::Vector2f sizeToFitImage(const std::shared_ptr<Image>& image);
    nanogui::Vector2f sizeToFitAllImages();
    void resizeToFit(nanogui::Vector2f size);

    bool playingBack() const;
    void setPlayingBack(bool value);

    bool setFilter(std::string_view filter);

    void setFps(int value);

    bool useRegex() const;
    void setUseRegex(bool value);

    bool watchFilesForChanges() const;
    void setWatchFilesForChanges(bool value);

    bool autoFitToScreen() const;
    void setAutoFitToScreen(bool value);

    bool resizeWindowToFitImageOnLoad() const;
    void setResizeWindowToFitImageOnLoad(bool value);

    void maximize();
    bool isMaximized();
    void toggleMaximized();

    bool isUiVisible() { return mSidebar->visible(); }
    void setUiVisible(bool shouldBeVisible);
    // Reserve application chrome without changing image scale or pan. Repeating
    // the same insets is a no-op, including across inspector mode switches.
    void setWorkspaceInsets(int left, int top, int right, int bottom);
    // Use a separate inspector origin when a canvas-local toolbar is present.
    // A negative value restores the workspace top inset as the inspector origin.
    void setWorkspaceInspectorTop(int top);
    void focusCropControls();
    // Colour space of the current file's values, as the Pixel values list shows it:
    // "Unknown", "Rad (header)", "Luminance (assigned)" and so on.
    std::string sourceColorCaption() const;
    // -1 unknown, 0 Radiance RGB, 1 sRGB, 2 XYZ, 3 raw sensor, 4 luminance (cd/m²), 5 other primaries.
    int sourceColorKind() const;
    // True for a Radiance file without colour information (or with one hdrspace assigned):
    // the user may then assign a colour space, which is written into the file's header.
    bool canAssignSourceColor() const;
    // Asks for confirmation, adds the PRIMARIES / HDRSPACE_COLOR header lines and reloads.
    void requestSourceColorAssignment(int kind);
    // Perceptual maps (shown on the canvas in place of the file's values).
    // inputColor: 0 Radiance RGB, 1 sRGB, 2 XYZ (as View Visibility's input colour).
    void setPerceptualMapSettings(double pixelsPerDegree, double sensitivityCorrection,
        const std::string& spectralEmissionPath, int inputColor);
    // kind: 0 L, 1 M, 2 Rod, 3 L+M, 4 Adaptation, 5 Detectable contrast, 6 Eqv. luminance.
    bool showPerceptualMap(int kind);
    void showSourceValues();
    bool perceptualMapShown() const;
    // False colour for the canvas: on/off, log or linear ramp and its range.
    void setFalseColorDisplay(bool enabled, bool logScale, float minValue, float maxValue);
    // Open the value-mode menu (file values and conversions) in the inspector.
    void openInspectionModeMenu();
    void closeInspectionModeMenu();
    // Close every popup that hangs off the viewer inspector (colours, false colour,
    // HDR, value modes) when the workspace hides or replaces that panel.
    void closeSidebarPopups();
    // Application shortcuts are consulted before tev's own key bindings.
    void setAppShortcutHandler(std::function<bool(int, int)> handler) { mAppShortcutHandler = std::move(handler); }
    nanogui::Vector2i workspacePosition() const { return mImageCanvas->absolute_position(); }
    nanogui::Vector2i workspaceSize() const { return mImageCanvas->size(); }
    void setSidebarOverlayReveal(float reveal);
    float sidebarOverlayReveal() const { return mSidebarOverlayReveal; }
    nanogui::Vector2i sidebarPosition() const { return mSidebar->position(); }
    nanogui::Vector2i sidebarSize() const { return mSidebar->size(); }

    void toggleHelpWindow();

    void toggleImageInfoWindow();
    void updateImageInfoWindow();

    void openImageDialog();
    void saveImageDialog();
    void copyFalseColorBarToClipboard() const;

    void requestLayoutUpdate() { mRequiresLayoutUpdate = true; }
    void setCropUi(const std::optional<Box2i>& crop);
    void setCropSaveHandler(std::function<void(const fs::path&, const fs::path&, const Box2i&)> handler) { mCropSaveHandler = std::move(handler); }
    void setProjectionSaveHandler(std::function<void(const fs::path&, const fs::path&, const std::string&, const std::string&)> handler) {
        mProjectionSaveHandler = std::move(handler);
    }
    void setImageInsertedCallback(std::function<void(const std::shared_ptr<Image>&)> callback) { mImageInsertedCallback = std::move(callback); }
    void setImageSelectedCallback(std::function<void(const std::shared_ptr<Image>&)> callback) { mImageSelectedCallback = std::move(callback); }

    template <typename T> void scheduleToUiThread(const T& fun) {
        mTaskQueue.push(fun);
        redraw();
    }

    BackgroundImagesLoader& imagesLoader() const { return *mImagesLoader; }
    std::weak_ptr<Ipc> ipc() const { return mIpc; }

    void copyImageCanvasToClipboard() const;
    void copyImageNameToClipboard() const;
    void pasteImagesFromClipboard();

    void showErrorDialog(std::string_view message);

    ImageCanvas* imageCanvas() const { return mImageCanvas; }
    const std::shared_ptr<Image>& currentImage() const { return mCurrentImage; }
    const std::shared_ptr<Image>& currentReference() const { return mCurrentReference; }

    chroma_t inspectionChroma() const;
    void setInspectionChroma(const chroma_t& chroma);

    ituth273::ETransfer inspectionTransfer() const;
    void setInspectionTransfer(ituth273::ETransfer transfer);

    bool inspectionAdaptWhitePoint() const;
    void setInspectionAdaptWhitePoint(bool adapt);

    bool inspectionPremultipliedAlpha() const;
    void setInspectionPremultipliedAlpha(bool value);

    float inspectionScale() const { return mImageCanvas->inspectionScale(); }
    void setInspectionScale(float scale) { mImageCanvas->setInspectionScale(scale); }
    void setDisplayInspectionInPreview(bool value) { mImageCanvas->setDisplayInspectionInPreview(value); }
    bool displayInspectionInPreview() const { return mImageCanvas->displayInspectionInPreview(); }
    int inspectionPresetIndex() const { return mInspectionPreset; }
    void setInspectionPresetIndex(int presetIndex);
    std::string currentSourceValueLabel() const;
    std::string currentProjectionLabel() const;
    std::string currentFieldOfViewLabel() const;
    void setAnalysisOverlay(const std::shared_ptr<Image>& image, const std::shared_ptr<PixelBuffer>& pixels) { mImageCanvas->setAnalysisOverlay(image, pixels); }
    void clearAnalysisOverlay() { mImageCanvas->clearAnalysisOverlay(); }

    enum class EAiMaskPickMode {
        None,
        Positive,
        Negative,
        StatsCenter,
        ToneMappingGaze,
    };

    void setAiMaskPickMode(EAiMaskPickMode mode);
    EAiMaskPickMode aiMaskPickMode() const { return mAiMaskPickMode; }
    void setAiMaskPickHandler(std::function<void(const nanogui::Vector2i&, bool)> handler) { mAiMaskPickHandler = std::move(handler); }
    void setAiMaskPromptPoints(std::vector<nanogui::Vector2i> positivePoints, std::vector<nanogui::Vector2i> negativePoints) {
        mImageCanvas->setAiMaskPromptPoints(std::move(positivePoints), std::move(negativePoints));
    }
    void clearAiMaskPromptPoints() { mImageCanvas->clearAiMaskPromptPoints(); }

    bool radiance179Preview() const { return mRadiance179Preview; }
    void setRadiance179Preview(bool value);

private:
    void updateFilter();
    void updateLayout();
    void updateTitle();
    void syncCropUiFromCanvas();
    void refreshProjectionUi(bool resetSelection = true);
    void applyCropFromUi();
    void maybeOfferCropSave();
    fs::path suggestedCroppedImagePath() const;
    void saveCurrentCropAsHdr(const fs::path& path);
    void maybeOfferProjectionSave();
    fs::path suggestedProjectedImagePath(std::string_view targetProjection) const;
    void saveCurrentProjectionAsHdr(const fs::path& path, const std::string& fromProjection, const std::string& toProjection);
    void applyHighlightRangeFromUi();
    void applyInspectionPreset(int presetIndex);
#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
    bool inspectionPresetUsesExperimentalMap(int presetIndex) const;
    bool currentImageSupportsExperimentalPerceptualMaps() const;
    std::shared_ptr<Image> experimentalPerceptualPreviewImage(int presetIndex);
    std::shared_ptr<Image> makeExperimentalPerceptualPreviewImage(const std::shared_ptr<Image>& source, int presetIndex) const;
#endif
    float radiometricInspectionScale() const;
    std::optional<float> sourceExposureCompensationScale() const;
    bool currentGroupHasThreeColorChannels() const;
    bool groupHasThreeColorChannels(std::string_view group) const;
    std::optional<std::string> preferredInspectionGroup() const;
    std::optional<int> inspectionChannelSelectionForGroup(std::string_view group) const;
    bool canConvertSourceToXyz() const;
    bool inspectionPresetSupported(int presetIndex) const;
    std::optional<nanogui::Matrix3f> inspectionSourceToRawMatrix() const;
    nanogui::Matrix3f inspectionSourceToRadMatrix() const;
    nanogui::Matrix3f inspectionSourceToRec709Matrix() const;
    nanogui::Matrix3f inspectionSourceToXyzMatrix() const;
    std::string inspectionPresetLabel(int presetIndex) const;
    void refreshInspectionModeUi();
    void updateHighlightModeUi();
    EHighlightMode selectedHighlightMode() const;
    void applyFalseColorSettingsFromUi();
    void refreshFalseColorBarUi();
    std::string sourceValueLabel() const;
    bool currentImageUsesRadiometricMonochromeFallback() const;
    void invalidateSourceInterpretationCache();
    std::string effectiveHighlightGroup() const;
    void syncHighlightChannelGroup();
    int effectiveInspectionDisplayChannelSelection() const;
    void syncInspectionDisplayChannelSelection();
    std::string inspectionCombinedLabel() const;
    std::string inspectionChannelLabel(size_t index) const;
    std::string inspectionSingleChannelLabel() const;
    std::string inspectionCaptionForGroup(std::string_view group) const;
    void refreshGroupButtonCaptions();
    void updateRoiStatsLabel(const std::shared_ptr<CanvasStatistics>& statistics);
    std::string_view groupName(size_t index);

    std::optional<size_t> groupId(std::string_view groupName) const;
    std::optional<size_t> imageId(const std::shared_ptr<Image>& image) const;
    std::optional<size_t> imageId(std::string_view imageName) const;

    std::string_view nextGroup(std::string_view groupName, EDirection direction);
    std::string_view nthVisibleGroup(size_t n);

    std::shared_ptr<Image> nextImage(const std::shared_ptr<Image>& image, EDirection direction);
    std::shared_ptr<Image> nthVisibleImage(size_t n);
    std::shared_ptr<Image> imageByName(std::string_view imageName);

    bool canDragSidebarFrom(nanogui::Vector2i p) {
        if (!mSidebar->visible() || mHasWorkspaceInsets) {
            return false;
        }

        const int edge = mSidebarOnRight ? m_size.x() - mSidebar->fixed_width() : mSidebar->fixed_width();
        return p.x() - edge < 10 && p.x() - edge > -5;
    }

    int visibleSidebarWidth() { return mSidebar->visible() ? mSidebar->fixed_width() : 0; }
    float mSidebarOverlayReveal = 1.0f;

    int visibleFooterHeight() { return mFooter->visible() ? mFooter->fixed_height() : 0; }

    void updateCurrentMonitorSize();

    SharedQueue<std::function<void(void)>> mTaskQueue;
    std::function<void(const fs::path&, const fs::path&, const Box2i&)> mCropSaveHandler;
    std::function<void(const fs::path&, const fs::path&, const std::string&, const std::string&)> mProjectionSaveHandler;
    std::function<void(const std::shared_ptr<Image>&)> mImageInsertedCallback;
    std::function<void(const std::shared_ptr<Image>&)> mImageSelectedCallback;
    std::function<void(const nanogui::Vector2i&, bool)> mAiMaskPickHandler;
    EAiMaskPickMode mAiMaskPickMode = EAiMaskPickMode::None;

    bool mRequiresFilterUpdate = true;
    bool mRequiresLayoutUpdate = true;

    nanogui::Widget* mVerticalScreenSplit = nullptr;
    bool mSidebarOnRight = false;
    bool mHasWorkspaceInsets = false;
    int mWorkspaceInspectorTop = -1;
    std::array<int, 4> mWorkspaceInsets = {0, 0, 0, 0};
    bool mPlayingBack = false;

    nanogui::Widget* mSidebar = nullptr;
    nanogui::Widget* mSidebarRoot = nullptr;
    nanogui::Widget* mSidebarToolbar = nullptr;
    nanogui::Widget* mSidebarTitleHost = nullptr;
    nanogui::Widget* mFloatingToolbarCard = nullptr;
    nanogui::Button* mHeaderOpenImageButton = nullptr;
    nanogui::Button* mHelpButton = nullptr;
    nanogui::Widget* mSidebarLayout = nullptr;
    nanogui::Widget* mCropControlsPanel = nullptr;
    std::function<bool(int, int)> mAppShortcutHandler;

    nanogui::Widget* mFooter = nullptr;
    bool mShouldFooterBeVisible = false;

    nanogui::Label* mExposureLabel = nullptr;
    nanogui::Slider* mExposureSlider = nullptr;

    nanogui::Label* mOffsetLabel = nullptr;
    nanogui::Slider* mOffsetSlider = nullptr;

    nanogui::Label* mGammaLabel = nullptr;
    nanogui::Slider* mGammaSlider = nullptr;

    nanogui::Widget* mTonemapButtonContainer = nullptr;
    std::array<nanogui::Button*, static_cast<size_t>(ETonemap::Count)> mTonemapButtons = {};
    nanogui::Widget* mMetricButtonContainer = nullptr;
    nanogui::Widget* mChannelMaskButtonContainer = nullptr;

    std::shared_ptr<BackgroundImagesLoader> mImagesLoader;
    std::weak_ptr<Ipc> mIpc;

    std::shared_ptr<Image> mCurrentImage;
    std::shared_ptr<Image> mCurrentReference;

    std::vector<std::shared_ptr<Image>> mImages;
    std::vector<std::string> mPendingImageRemovals;
    std::vector<std::shared_ptr<Image>> mZombieImagesPending;
    std::vector<std::shared_ptr<Image>> mZombieImagesRetired;

    MultiGraph* mHistogram = nullptr;
    nanogui::FloatBox<float>* mHighlightMinBox = nullptr;
    nanogui::FloatBox<float>* mHighlightMaxBox = nullptr;
    nanogui::ComboBox* mHighlightModeComboBox = nullptr;
    nanogui::Button* mHighlightEnableButton = nullptr;
    int mHighlightModeSelection = 0;
    nanogui::Label* mRoiStatsLabel = nullptr;
    nanogui::Button* mRoiClearButton = nullptr;
    std::unordered_set<std::shared_ptr<Image>> mToBump;

    nanogui::TextBox* mFilter = nullptr;
    nanogui::Button* mRegexButton = nullptr;

    nanogui::Button* mWatchFilesForChangesButton = nullptr;
    std::chrono::steady_clock::time_point mLastFileChangesCheckTime = {};

    nanogui::Button* mAutoFitToScreenButton = nullptr;
    nanogui::Button* mResizeWindowToFitImageOnLoadButton = nullptr;

    // Buttons which require a current image to be meaningful.
    std::vector<nanogui::Button*> mCurrentImageButtons;
    nanogui::Button* mImageInfoButton = nullptr;
    ImageInfoWindow* mImageInfoWindow = nullptr;

    // Buttons which require at least one image to be meaningful
    std::vector<nanogui::Button*> mAnyImageButtons;

    nanogui::Button* mPlayButton = nullptr;
    nanogui::IntBox<int>* mFpsTextBox = nullptr;
    std::chrono::steady_clock::time_point mLastPlaybackFrameTime = {};

    nanogui::Widget* mImageButtonContainer = nullptr;
    nanogui::Widget* mScrollContent = nullptr;
    nanogui::VScrollPanel* mImageScrollContainer = nullptr;

    ImageCanvas* mImageCanvas = nullptr;

    nanogui::Widget* mGroupButtonContainer = nullptr;
    std::string mCurrentGroup;

    HelpWindow* mHelpWindow = nullptr;

    enum class EMouseDragType {
        None,
        ImageDrag,
        ImageCrop,
        ImageRoi,
        ImageButtonDrag,
        SidebarDrag,
    };

    nanogui::Vector2i mDraggingStartPosition;
    EMouseDragType mDragType = EMouseDragType::None;
    size_t mDraggedImageButtonId;

    size_t mClipboardIndex = 0;

    // HDR UI elements support
    struct ColorSpace {
        ituth273::ETransfer transfer;
        EWpPrimaries primaries;
        float maxLuminance;

        bool operator==(const ColorSpace& other) const {
            return transfer == other.transfer && primaries == other.primaries && maxLuminance == other.maxLuminance;
        }
    };

    std::optional<ColorSpace> mSystemColorSpace;

    nanogui::PopupButton* mColorsPopupButton = nullptr;
    nanogui::PopupButton* mFalseColorPopupButton = nullptr;
    nanogui::PopupButton* mFalseColorBarPopupButton = nullptr;
    nanogui::ComboBox* mFalseColorColormapComboBox = nullptr;
    nanogui::ComboBox* mFalseColorScaleComboBox = nullptr;
    nanogui::FloatBox<float>* mFalseColorMinBox = nullptr;
    nanogui::FloatBox<float>* mFalseColorMaxBox = nullptr;
    nanogui::ComboBox* mFalseColorBarTicksComboBox = nullptr;
    nanogui::TextBox* mFalseColorBarManualTicksBox = nullptr;
    nanogui::Widget* mFalseColorBarWidget = nullptr;

    nanogui::ComboBox* mInspectionPrimariesComboBox = nullptr;
    std::vector<nanogui::FloatBox<float>*> mInspectionPrimariesBoxes;
    nanogui::ComboBox* mInspectionTransferComboBox = nullptr;
    nanogui::Button* mInspectionAdaptWhitePointButton = nullptr;
    nanogui::Button* mInspectionPremultipliedAlphaButton = nullptr;

    nanogui::ColorWheel* mBackgroundColorWheel = nullptr;
    nanogui::Slider* mBackgroundAlphaSlider = nullptr;

    nanogui::PopupButton* mHdrPopupButton = nullptr;
    nanogui::PopupButton* mValueModePopupButton = nullptr;
    std::vector<nanogui::Button*> mInspectionPresetButtons;
    nanogui::Button* mClipToLdrButton = nullptr;
    nanogui::ComboBox* mProjectionComboBox = nullptr;
    nanogui::Button* mProjectionApplyButton = nullptr;
    int mProjectionSelection = 0;
    nanogui::IntBox<int>* mCropLeftBox = nullptr;
    nanogui::IntBox<int>* mCropTopBox = nullptr;
    nanogui::IntBox<int>* mCropWidthBox = nullptr;
    nanogui::IntBox<int>* mCropHeightBox = nullptr;

    nanogui::FloatBox<float>* mDisplayWhiteLevelBox = nullptr;
    nanogui::ComboBox* mDisplayWhiteLevelSettingComboBox = nullptr;

    nanogui::FloatBox<float>* mImageWhiteLevelBox = nullptr;

    // Misc state tracking variables
    int mDidFitToImage = 0;

    nanogui::Vector2f mMinWindowPos = {0, 0};
    nanogui::Vector2f mMaxWindowSize = {8192, 8192};

    bool mInitialized = false;
    bool mRadiance179Preview = false;
    mutable const Image* mCachedSourceInterpretationImage = nullptr;
    mutable int mCachedSourceInterpretationImageId = -1;
    mutable std::optional<bool> mCachedRadiometricMonochromeFallback = std::nullopt;
    int mInspectionPreset = 0;
#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
    std::shared_ptr<Image> mExperimentalInspectionPreviewImage;
    int mExperimentalInspectionPreviewPreset = -1;
    int mExperimentalInspectionPreviewSourceImageId = -1;
#endif
    double mPerceptualPpd = 30.0;
    double mPerceptualSensitivity = -1.0;
    std::string mPerceptualEmissionPath;
    int mPerceptualInputColor = 0;
    std::vector<nanogui::Button*> mAssignColorButtons;
    nanogui::Widget* mAssignColorHeading = nullptr;
    nanogui::Widget* mAssignColorSpacer = nullptr;
    std::optional<std::string> assignedSourceColor() const;
    bool currentFileIsRadiance() const;
    bool writeAssignedColorHeader(int kind, std::string& error);

    bool mMaximizedLaunch = false;
    bool mMaximizedUnreliable = false;

    std::unique_ptr<std::thread> mFileDialogThread;
};

} // namespace tev
