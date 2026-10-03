#pragma once

#include <tev/Common.h>

#include <nanogui/widget.h>

#include <functional>
#include <optional>
#include <span>

namespace tev {

enum class EProfileGraphDisplayMode {
    Line,
    Scatter,
    LineScatter,
};

struct ProfileGraphSeries {
    std::string label;
    nanogui::Color color;
    std::vector<float> x;
    std::vector<float> y;
};

struct ProfileGraphStyle {
    float xAxisFontSize = 10.5f;
    float yAxisFontSize = 10.5f;
    float titleFontSize = 14.5f;
    float subtitleFontSize = 11.5f;
    float xTickStep = 0.0f;
    float yTickStep = 0.0f;
    std::optional<float> xMinOverride;
    std::optional<float> xMaxOverride;
    std::optional<float> yMinOverride;
    std::optional<float> yMaxOverride;
    bool yLogScale = false;
    nanogui::Color backgroundColor = nanogui::Color(19, 23, 31, 238);
    bool transparentBackground = false;
    bool showInteractionHint = false;
    std::string interactionHintText;
};

class ProfileGraph final : public nanogui::Widget {
public:
    ProfileGraph(nanogui::Widget* parent, std::string_view title = "Profile");

    void setTitle(std::string_view title) { mTitle = title; }
    void setSubtitle(std::string_view subtitle) { mSubtitle = subtitle; }
    void setXAxisLabel(std::string_view label) { mXAxisLabel = label; }
    void setYAxisLabel(std::string_view label) { mYAxisLabel = label; }
    void setSeries(std::span<const ProfileGraphSeries> series);
    void clearSeries() {
        mSeries.clear();
        resetInteractiveView();
    }
    void setDisplayMode(EProfileGraphDisplayMode mode) { mDisplayMode = mode; }
    void setStyle(const ProfileGraphStyle& style);
    const ProfileGraphStyle& style() const { return mStyle; }
    void setClickCallback(std::function<void()> callback) { mClickCallback = std::move(callback); }
    void setExpandOnMouseDown(bool value) { mExpandOnMouseDown = value; }
    void zoomBy(float wheelDelta);
    void zoomXBy(float wheelDelta);
    void resetInteractiveView();

    std::span<const ProfileGraphSeries> series() const { return mSeries; }

    nanogui::Vector2i preferred_size_impl(NVGcontext* ctx) const override;
    bool mouse_button_event(const nanogui::Vector2i& p, int button, bool down, int modifiers) override;
    bool mouse_drag_event(const nanogui::Vector2i& p, const nanogui::Vector2i& rel, int button, int modifiers) override;
    bool scroll_event(const nanogui::Vector2i& p, const nanogui::Vector2f& rel) override;
    void draw(NVGcontext* ctx) override;

private:
    std::string mTitle;
    std::string mSubtitle;
    std::string mXAxisLabel;
    std::string mYAxisLabel;
    std::vector<ProfileGraphSeries> mSeries;
    EProfileGraphDisplayMode mDisplayMode = EProfileGraphDisplayMode::LineScatter;
    ProfileGraphStyle mStyle;
    std::function<void()> mClickCallback;
    bool mHasInteractiveView = false;
    float mInteractiveXMin = 0.0f;
    float mInteractiveXMax = 1.0f;
    float mInteractiveYMin = 0.0f;
    float mInteractiveYMax = 1.0f;
    bool mDraggingView = false;
    bool mPotentialClick = false;
    nanogui::Vector2i mLastMousePos{0, 0};
    bool mExpandOnMouseDown = false;
};

} // namespace tev
