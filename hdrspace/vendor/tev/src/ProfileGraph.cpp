#include <tev/ProfileGraph.h>

#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/theme.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <iomanip>
#include <mutex>
#include <string_view>

using namespace nanogui;

namespace tev {

namespace {

bool graphPreviewDebugEnabled() {
    static const bool enabled = std::getenv("HDRSPACE_DEBUG_GRAPH_PREVIEW") != nullptr;
    return enabled;
}

void graphPreviewDebugLog(const std::string& message) {
    if (!graphPreviewDebugEnabled()) {
        return;
    }

    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    std::ofstream output("/tmp/hdrspace_graph_preview.log", std::ios::app);
    if (!output) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    output << std::put_time(std::localtime(&time), "%H:%M:%S") << " | " << message << "\n";
}

std::string formatProfileNumber(float value) {
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

struct PlotBounds {
    float minX = 0.0f;
    float maxX = 1.0f;
    float minY = 0.0f;
    float maxY = 1.0f;
    float minPositiveY = std::numeric_limits<float>::infinity();
    bool valid = false;
};

PlotBounds computeBounds(std::span<const ProfileGraphSeries> series) {
    PlotBounds bounds;
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

struct AxisLayout {
    float minValue = 0.0f;
    float maxValue = 1.0f;
    std::vector<float> ticks{0.0f, 0.5f, 1.0f};
};

double niceTickStep(double rawStep) {
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

std::vector<float> buildTicks(float minValue, float maxValue, float preferredStep, int targetCount = 5) {
    if (!std::isfinite(minValue) || !std::isfinite(maxValue)) {
        return {0.0f, 1.0f};
    }

    if (maxValue < minValue) {
        std::swap(minValue, maxValue);
    }

    double step = preferredStep > 0.0f
        ? static_cast<double>(preferredStep)
        : niceTickStep(static_cast<double>(maxValue - minValue) / std::max(1, targetCount - 1));
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

std::vector<float> buildLogTicks(float minValue, float maxValue) {
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

AxisLayout buildAxisLayout(
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
    AxisLayout layout;
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
        if (!(std::isfinite(minValue) && std::isfinite(maxValue))) {
            return layout;
        }
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
            ? buildTicks(axisMin, axisMax, preferredStep, targetCount)
            : buildLogTicks(axisMin, axisMax);
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
        : niceTickStep(static_cast<double>(maxValue - minValue) / std::max(1, targetCount - 1));
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
    layout.ticks = buildTicks(layout.minValue, layout.maxValue, static_cast<float>(step), targetCount);
    return layout;
}

float colorLuminance(const Color& color) {
    return 0.2126f * color.r() + 0.7152f * color.g() + 0.0722f * color.b();
}

struct DerivedGraphColors {
    Color panelBackground;
    Color plotBackground;
    Color border;
    Color gridMajor;
    Color gridMinor;
    Color primaryText;
    Color secondaryText;
};

DerivedGraphColors deriveGraphColors(const ProfileGraphStyle& style) {
    const bool lightBackground = !style.transparentBackground && colorLuminance(style.backgroundColor) > 0.6f;

    DerivedGraphColors out;
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

struct PlotGeometry {
    PlotBounds bounds;
    AxisLayout xAxis;
    AxisLayout yAxis;
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 1.0f;
    float y1 = 1.0f;
    float plotWidth = 1.0f;
    float plotHeight = 1.0f;
    bool valid = false;
};

float clampToRange(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

float normalizeZoomWheelDelta(float wheelDelta) {
    if (!std::isfinite(wheelDelta) || std::abs(wheelDelta) < 1e-4f) {
        return 0.0f;
    }

    const float absDelta = std::abs(wheelDelta);
    if (absDelta < 1.0f) {
        return std::copysign(std::max(1.0f, absDelta * 8.0f), wheelDelta);
    }
    return wheelDelta * 2.0f;
}

PlotGeometry buildPlotGeometry(
    const Vector2i& pos,
    const Vector2i& size,
    std::span<const ProfileGraphSeries> series,
    const ProfileGraphStyle& style,
    std::string_view subtitle,
    std::string_view xAxisLabel,
    std::string_view yAxisLabel,
    bool hasInteractiveView,
    float interactiveXMin,
    float interactiveXMax,
    float interactiveYMin,
    float interactiveYMax
) {
    PlotGeometry geometry;
    geometry.bounds = computeBounds(series);

    const std::optional<float> xMinOverride = hasInteractiveView ? std::optional<float>{interactiveXMin} : style.xMinOverride;
    const std::optional<float> xMaxOverride = hasInteractiveView ? std::optional<float>{interactiveXMax} : style.xMaxOverride;
    const std::optional<float> yMinOverride = hasInteractiveView ? std::optional<float>{interactiveYMin} : style.yMinOverride;
    const std::optional<float> yMaxOverride = hasInteractiveView ? std::optional<float>{interactiveYMax} : style.yMaxOverride;

    geometry.xAxis = buildAxisLayout(
        geometry.bounds.minX,
        geometry.bounds.maxX,
        style.xTickStep,
        6,
        false,
        xMinOverride,
        xMaxOverride,
        false
    );
    geometry.yAxis = buildAxisLayout(
        geometry.bounds.minY,
        geometry.bounds.maxY,
        style.yTickStep,
        5,
        geometry.bounds.minY >= 0.0f,
        yMinOverride,
        yMaxOverride,
        style.yLogScale,
        geometry.bounds.minPositiveY
    );

    const float topMargin = 8.0f + style.titleFontSize + (!subtitle.empty() ? (style.subtitleFontSize + 6.0f) : 0.0f);
    const float maxYTickWidth = style.yLogScale ? 46.0f : 40.0f;
    const float yAxisLabelReserve = yAxisLabel.empty()
        ? 8.0f
        : (yAxisLabel.size() <= 10 ? (style.yAxisFontSize + 10.0f) : (style.yAxisFontSize + 16.0f));
    const float leftMargin = std::max(44.0f, maxYTickWidth + yAxisLabelReserve);
    const float rightMargin = 10.0f;
    const float bottomMargin = std::max(28.0f, style.xAxisFontSize * 1.9f + (!xAxisLabel.empty() ? 12.0f : 6.0f));

    geometry.x0 = pos.x() + leftMargin;
    geometry.y0 = pos.y() + topMargin;
    geometry.x1 = pos.x() + size.x() - rightMargin;
    geometry.y1 = pos.y() + size.y() - bottomMargin;
    geometry.plotWidth = std::max(1.0f, geometry.x1 - geometry.x0);
    geometry.plotHeight = std::max(1.0f, geometry.y1 - geometry.y0);
    geometry.valid =
        geometry.bounds.valid &&
        geometry.xAxis.maxValue > geometry.xAxis.minValue &&
        geometry.yAxis.maxValue > geometry.yAxis.minValue &&
        geometry.plotWidth > 1.0f &&
        geometry.plotHeight > 1.0f;
    return geometry;
}

bool pointInsidePlot(const PlotGeometry& geometry, const Vector2i& p) {
    return geometry.valid &&
        p.x() >= geometry.x0 && p.x() <= geometry.x1 &&
        p.y() >= geometry.y0 && p.y() <= geometry.y1;
}

float mapPlotX(const PlotGeometry& geometry, float value) {
    return geometry.x0 + ((value - geometry.xAxis.minValue) / (geometry.xAxis.maxValue - geometry.xAxis.minValue)) * geometry.plotWidth;
}

float mapPlotY(const PlotGeometry& geometry, const ProfileGraphStyle& style, float value) {
    if (style.yLogScale) {
        if (value <= 0.0f || geometry.yAxis.minValue <= 0.0f || geometry.yAxis.maxValue <= geometry.yAxis.minValue) {
            return geometry.y1;
        }
        const float minLog = std::log10(geometry.yAxis.minValue);
        const float maxLog = std::log10(geometry.yAxis.maxValue);
        const float valueLog = std::log10(value);
        return geometry.y1 - ((valueLog - minLog) / (maxLog - minLog)) * geometry.plotHeight;
    }
    return geometry.y1 - ((value - geometry.yAxis.minValue) / (geometry.yAxis.maxValue - geometry.yAxis.minValue)) * geometry.plotHeight;
}

float unmapPlotX(const PlotGeometry& geometry, float px) {
    const float t = clampToRange((px - geometry.x0) / geometry.plotWidth, 0.0f, 1.0f);
    return geometry.xAxis.minValue + t * (geometry.xAxis.maxValue - geometry.xAxis.minValue);
}

float unmapPlotY(const PlotGeometry& geometry, const ProfileGraphStyle& style, float py) {
    const float t = clampToRange((geometry.y1 - py) / geometry.plotHeight, 0.0f, 1.0f);
    if (style.yLogScale) {
        const float minLog = std::log10(geometry.yAxis.minValue);
        const float maxLog = std::log10(geometry.yAxis.maxValue);
        return std::pow(10.0f, minLog + t * (maxLog - minLog));
    }
    return geometry.yAxis.minValue + t * (geometry.yAxis.maxValue - geometry.yAxis.minValue);
}

void clampRangeToBase(float& currentMin, float& currentMax, float baseMin, float baseMax) {
    const float currentRange = currentMax - currentMin;
    const float baseRange = baseMax - baseMin;
    if (!(std::isfinite(currentRange) && std::isfinite(baseRange)) || currentRange <= 0.0f || baseRange <= 0.0f) {
        currentMin = baseMin;
        currentMax = baseMax;
        return;
    }

    if (currentRange >= baseRange) {
        currentMin = baseMin;
        currentMax = baseMax;
        return;
    }

    if (currentMin < baseMin) {
        currentMax += (baseMin - currentMin);
        currentMin = baseMin;
    }
    if (currentMax > baseMax) {
        currentMin -= (currentMax - baseMax);
        currentMax = baseMax;
    }

    currentMin = std::max(currentMin, baseMin);
    currentMax = std::min(currentMax, baseMax);
}

void clampLogRangeToBase(float& currentMin, float& currentMax, float baseMin, float baseMax) {
    currentMin = std::max(currentMin, std::max(baseMin, 1e-6f));
    currentMax = std::max(currentMax, currentMin * 1.01f);
    clampRangeToBase(currentMin, currentMax, std::max(baseMin, 1e-6f), std::max(baseMax, currentMin * 1.01f));
}

void requestGraphRedraw(nanogui::Screen* screen) {
    if (!screen) {
        return;
    }
    screen->redraw();
    screen->draw_all();
}

} // namespace

ProfileGraph::ProfileGraph(Widget* parent, std::string_view title) : Widget{parent}, mTitle{title} {}

Vector2i ProfileGraph::preferred_size_impl(NVGcontext*) const {
    return {210, 165};
}

void ProfileGraph::setSeries(std::span<const ProfileGraphSeries> series) {
    mSeries.assign(series.begin(), series.end());
    graphPreviewDebugLog(std::format("ProfileGraph::setSeries title='{}' count={}", mTitle, mSeries.size()));
    resetInteractiveView();
}

void ProfileGraph::setStyle(const ProfileGraphStyle& style) {
    mStyle = style;
    graphPreviewDebugLog(std::format("ProfileGraph::setStyle title='{}'", mTitle));
    resetInteractiveView();
}

void ProfileGraph::resetInteractiveView() {
    graphPreviewDebugLog(std::format("ProfileGraph::resetInteractiveView title='{}'", mTitle));
    mHasInteractiveView = false;
    mDraggingView = false;
    mPotentialClick = false;
}

bool ProfileGraph::mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) {
    Widget::mouse_button_event(p, button, down, modifiers);
    const PlotGeometry geometry = buildPlotGeometry(
        m_pos, m_size, mSeries, mStyle, mSubtitle, mXAxisLabel, mYAxisLabel,
        mHasInteractiveView, mInteractiveXMin, mInteractiveXMax, mInteractiveYMin, mInteractiveYMax
    );

    if (button == GLFW_MOUSE_BUTTON_RIGHT && down) {
        resetInteractiveView();
        requestGraphRedraw(screen());
        return contains(p);
    }

    if (button != GLFW_MOUSE_BUTTON_LEFT) {
        return false;
    }

    if (down) {
        if (mExpandOnMouseDown && contains(p) && mClickCallback) {
            mClickCallback();
            mPotentialClick = false;
            mDraggingView = false;
            return true;
        }
        mPotentialClick = contains(p);
        mDraggingView = pointInsidePlot(geometry, p);
        mLastMousePos = p;
        return contains(p);
    }

    const bool shouldClick = mPotentialClick && contains(p) && mClickCallback;
    mDraggingView = false;
    mPotentialClick = false;
    if (shouldClick) {
        mClickCallback();
        return true;
    }
    return contains(p);
}

bool ProfileGraph::mouse_drag_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) {
    (void)modifiers;
    if (!mDraggingView || (button & (1 << GLFW_MOUSE_BUTTON_LEFT)) == 0) {
        return false;
    }

    if (std::abs(rel.x()) > 1 || std::abs(rel.y()) > 1) {
        mPotentialClick = false;
    }

    const PlotGeometry geometry = buildPlotGeometry(
        m_pos, m_size, mSeries, mStyle, mSubtitle, mXAxisLabel, mYAxisLabel,
        mHasInteractiveView, mInteractiveXMin, mInteractiveXMax, mInteractiveYMin, mInteractiveYMax
    );
    if (!geometry.valid) {
        return false;
    }

    if (!mHasInteractiveView) {
        mHasInteractiveView = true;
        mInteractiveXMin = geometry.xAxis.minValue;
        mInteractiveXMax = geometry.xAxis.maxValue;
        mInteractiveYMin = geometry.yAxis.minValue;
        mInteractiveYMax = geometry.yAxis.maxValue;
    }

    const PlotGeometry baseGeometry = buildPlotGeometry(
        m_pos, m_size, mSeries, mStyle, mSubtitle, mXAxisLabel, mYAxisLabel,
        false, 0.0f, 0.0f, 0.0f, 0.0f
    );

    const float xRange = mInteractiveXMax - mInteractiveXMin;
    mInteractiveXMin -= static_cast<float>(rel.x()) / geometry.plotWidth * xRange;
    mInteractiveXMax -= static_cast<float>(rel.x()) / geometry.plotWidth * xRange;
    clampRangeToBase(mInteractiveXMin, mInteractiveXMax, baseGeometry.xAxis.minValue, baseGeometry.xAxis.maxValue);

    if (mStyle.yLogScale) {
        const float logRange = std::log10(mInteractiveYMax) - std::log10(mInteractiveYMin);
        const float deltaLog = static_cast<float>(rel.y()) / geometry.plotHeight * logRange;
        mInteractiveYMin = std::pow(10.0f, std::log10(mInteractiveYMin) + deltaLog);
        mInteractiveYMax = std::pow(10.0f, std::log10(mInteractiveYMax) + deltaLog);
        clampLogRangeToBase(mInteractiveYMin, mInteractiveYMax, baseGeometry.yAxis.minValue, baseGeometry.yAxis.maxValue);
    } else {
        const float yRange = mInteractiveYMax - mInteractiveYMin;
        mInteractiveYMin += static_cast<float>(rel.y()) / geometry.plotHeight * yRange;
        mInteractiveYMax += static_cast<float>(rel.y()) / geometry.plotHeight * yRange;
        clampRangeToBase(mInteractiveYMin, mInteractiveYMax, baseGeometry.yAxis.minValue, baseGeometry.yAxis.maxValue);
    }

    requestGraphRedraw(screen());
    return true;
}

bool ProfileGraph::scroll_event(const Vector2i& p, const Vector2f& rel) {
    const PlotGeometry geometry = buildPlotGeometry(
        m_pos, m_size, mSeries, mStyle, mSubtitle, mXAxisLabel, mYAxisLabel,
        mHasInteractiveView, mInteractiveXMin, mInteractiveXMax, mInteractiveYMin, mInteractiveYMax
    );
    graphPreviewDebugLog(std::format(
        "ProfileGraph::scroll_event title='{}' p=({}, {}) rel=({:.3f}, {:.3f}) contains={} valid={} size=({}, {})",
        mTitle,
        p.x(), p.y(),
        rel.x(), rel.y(),
        contains(p),
        geometry.valid,
        m_size.x(), m_size.y()
    ));
    if (!geometry.valid || !contains(p)) {
        return false;
    }

    const float wheelDeltaRaw = std::abs(rel.y()) >= std::abs(rel.x()) ? rel.y() : rel.x();
    const float wheelDelta = normalizeZoomWheelDelta(wheelDeltaRaw);
    if (std::abs(wheelDelta) < 1e-4f) {
        return false;
    }

    if (!mHasInteractiveView) {
        mHasInteractiveView = true;
        mInteractiveXMin = geometry.xAxis.minValue;
        mInteractiveXMax = geometry.xAxis.maxValue;
        mInteractiveYMin = geometry.yAxis.minValue;
        mInteractiveYMax = geometry.yAxis.maxValue;
    }

    const PlotGeometry baseGeometry = buildPlotGeometry(
        m_pos, m_size, mSeries, mStyle, mSubtitle, mXAxisLabel, mYAxisLabel,
        false, 0.0f, 0.0f, 0.0f, 0.0f
    );

    const float anchorPx = clampToRange(static_cast<float>(p.x()), geometry.x0, geometry.x1);
    const float anchorPy = clampToRange(static_cast<float>(p.y()), geometry.y0, geometry.y1);
    const float zoomFactor = std::pow(0.82f, wheelDelta);
    const float minXRange = std::max((baseGeometry.xAxis.maxValue - baseGeometry.xAxis.minValue) * 0.0025f, 1e-4f);
    const float anchorX = unmapPlotX(geometry, anchorPx);
    const float nextXRange = std::max((mInteractiveXMax - mInteractiveXMin) * zoomFactor, minXRange);
    mInteractiveXMin = anchorX - (anchorX - mInteractiveXMin) * zoomFactor;
    mInteractiveXMax = mInteractiveXMin + nextXRange;
    clampRangeToBase(mInteractiveXMin, mInteractiveXMax, baseGeometry.xAxis.minValue, baseGeometry.xAxis.maxValue);

    if (mStyle.yLogScale) {
        const float safeBaseMin = std::max(baseGeometry.yAxis.minValue, 1e-6f);
        const float safeBaseMax = std::max(baseGeometry.yAxis.maxValue, safeBaseMin * 1.01f);
        const float anchorY = unmapPlotY(geometry, mStyle, anchorPy);
        const float minLogRange = std::max((std::log10(safeBaseMax) - std::log10(safeBaseMin)) * 0.02f, 0.02f);
        const float currentLogMin = std::log10(std::max(mInteractiveYMin, 1e-6f));
        const float currentLogMax = std::log10(std::max(mInteractiveYMax, mInteractiveYMin * 1.01f));
        const float anchorLog = std::log10(std::max(anchorY, 1e-6f));
        const float nextLogRange = std::max((currentLogMax - currentLogMin) + std::log10(zoomFactor), minLogRange);
        const float nextLogMin = anchorLog - (anchorLog - currentLogMin) * zoomFactor;
        mInteractiveYMin = std::pow(10.0f, nextLogMin);
        mInteractiveYMax = std::pow(10.0f, nextLogMin + nextLogRange);
        clampLogRangeToBase(mInteractiveYMin, mInteractiveYMax, safeBaseMin, safeBaseMax);
    } else {
        const float minYRange = std::max((baseGeometry.yAxis.maxValue - baseGeometry.yAxis.minValue) * 0.0025f, 1e-4f);
        const float anchorY = unmapPlotY(geometry, mStyle, anchorPy);
        const float nextYRange = std::max((mInteractiveYMax - mInteractiveYMin) * zoomFactor, minYRange);
        mInteractiveYMin = anchorY - (anchorY - mInteractiveYMin) * zoomFactor;
        mInteractiveYMax = mInteractiveYMin + nextYRange;
        clampRangeToBase(mInteractiveYMin, mInteractiveYMax, baseGeometry.yAxis.minValue, baseGeometry.yAxis.maxValue);
    }

    requestGraphRedraw(screen());
    graphPreviewDebugLog(std::format(
        "ProfileGraph::scroll_event applied title='{}' x=[{:.3f},{:.3f}] y=[{:.3f},{:.3f}]",
        mTitle,
        mInteractiveXMin, mInteractiveXMax,
        mInteractiveYMin, mInteractiveYMax
    ));
    return true;
}

void ProfileGraph::zoomBy(float wheelDelta) {
    graphPreviewDebugLog(std::format(
        "ProfileGraph::zoomBy begin title='{}' delta={:.3f} size=({}, {}) hasInteractive={}",
        mTitle,
        wheelDelta,
        m_size.x(), m_size.y(),
        mHasInteractiveView
    ));
    wheelDelta = normalizeZoomWheelDelta(wheelDelta);
    if (!std::isfinite(wheelDelta) || std::abs(wheelDelta) < 1e-4f) {
        graphPreviewDebugLog(std::format("ProfileGraph::zoomBy ignored title='{}' normalized={:.6f}", mTitle, wheelDelta));
        return;
    }

    const PlotGeometry geometry = buildPlotGeometry(
        m_pos, m_size, mSeries, mStyle, mSubtitle, mXAxisLabel, mYAxisLabel,
        mHasInteractiveView, mInteractiveXMin, mInteractiveXMax, mInteractiveYMin, mInteractiveYMax
    );
    if (!geometry.valid) {
        graphPreviewDebugLog(std::format("ProfileGraph::zoomBy invalid geometry title='{}'", mTitle));
        return;
    }

    if (!mHasInteractiveView) {
        mHasInteractiveView = true;
        mInteractiveXMin = geometry.xAxis.minValue;
        mInteractiveXMax = geometry.xAxis.maxValue;
        mInteractiveYMin = geometry.yAxis.minValue;
        mInteractiveYMax = geometry.yAxis.maxValue;
    }

    const PlotGeometry baseGeometry = buildPlotGeometry(
        m_pos, m_size, mSeries, mStyle, mSubtitle, mXAxisLabel, mYAxisLabel,
        false, 0.0f, 0.0f, 0.0f, 0.0f
    );

    const float anchorPx = geometry.x0 + geometry.plotWidth * 0.5f;
    const float anchorPy = geometry.y0 + geometry.plotHeight * 0.5f;
    const float zoomFactor = std::pow(0.82f, wheelDelta);
    const float minXRange = std::max((baseGeometry.xAxis.maxValue - baseGeometry.xAxis.minValue) * 0.0025f, 1e-4f);
    const float anchorX = unmapPlotX(geometry, anchorPx);
    const float nextXRange = std::max((mInteractiveXMax - mInteractiveXMin) * zoomFactor, minXRange);
    mInteractiveXMin = anchorX - (anchorX - mInteractiveXMin) * zoomFactor;
    mInteractiveXMax = mInteractiveXMin + nextXRange;
    clampRangeToBase(mInteractiveXMin, mInteractiveXMax, baseGeometry.xAxis.minValue, baseGeometry.xAxis.maxValue);

    if (mStyle.yLogScale) {
        const float safeBaseMin = std::max(baseGeometry.yAxis.minValue, 1e-6f);
        const float safeBaseMax = std::max(baseGeometry.yAxis.maxValue, safeBaseMin * 1.01f);
        const float anchorY = unmapPlotY(geometry, mStyle, anchorPy);
        const float minLogRange = std::max((std::log10(safeBaseMax) - std::log10(safeBaseMin)) * 0.02f, 0.02f);
        const float currentLogMin = std::log10(std::max(mInteractiveYMin, 1e-6f));
        const float currentLogMax = std::log10(std::max(mInteractiveYMax, mInteractiveYMin * 1.01f));
        const float anchorLog = std::log10(std::max(anchorY, 1e-6f));
        const float nextLogRange = std::max((currentLogMax - currentLogMin) + std::log10(zoomFactor), minLogRange);
        const float nextLogMin = anchorLog - (anchorLog - currentLogMin) * zoomFactor;
        mInteractiveYMin = std::pow(10.0f, nextLogMin);
        mInteractiveYMax = std::pow(10.0f, nextLogMin + nextLogRange);
        clampLogRangeToBase(mInteractiveYMin, mInteractiveYMax, safeBaseMin, safeBaseMax);
    } else {
        const float minYRange = std::max((baseGeometry.yAxis.maxValue - baseGeometry.yAxis.minValue) * 0.0025f, 1e-4f);
        const float anchorY = unmapPlotY(geometry, mStyle, anchorPy);
        const float nextYRange = std::max((mInteractiveYMax - mInteractiveYMin) * zoomFactor, minYRange);
        mInteractiveYMin = anchorY - (anchorY - mInteractiveYMin) * zoomFactor;
        mInteractiveYMax = mInteractiveYMin + nextYRange;
        clampRangeToBase(mInteractiveYMin, mInteractiveYMax, baseGeometry.yAxis.minValue, baseGeometry.yAxis.maxValue);
    }
    requestGraphRedraw(screen());
    graphPreviewDebugLog(std::format(
        "ProfileGraph::zoomBy applied title='{}' normalized={:.3f} x=[{:.3f},{:.3f}] y=[{:.3f},{:.3f}]",
        mTitle,
        wheelDelta,
        mInteractiveXMin, mInteractiveXMax,
        mInteractiveYMin, mInteractiveYMax
    ));
}

void ProfileGraph::zoomXBy(float wheelDelta) {
    graphPreviewDebugLog(std::format(
        "ProfileGraph::zoomXBy begin title='{}' delta={:.3f} size=({}, {}) hasInteractive={}",
        mTitle,
        wheelDelta,
        m_size.x(), m_size.y(),
        mHasInteractiveView
    ));
    wheelDelta = normalizeZoomWheelDelta(wheelDelta);
    if (!std::isfinite(wheelDelta) || std::abs(wheelDelta) < 1e-4f) {
        graphPreviewDebugLog(std::format("ProfileGraph::zoomXBy ignored title='{}' normalized={:.6f}", mTitle, wheelDelta));
        return;
    }

    const PlotGeometry geometry = buildPlotGeometry(
        m_pos, m_size, mSeries, mStyle, mSubtitle, mXAxisLabel, mYAxisLabel,
        mHasInteractiveView, mInteractiveXMin, mInteractiveXMax, mInteractiveYMin, mInteractiveYMax
    );
    if (!geometry.valid) {
        graphPreviewDebugLog(std::format("ProfileGraph::zoomXBy invalid geometry title='{}'", mTitle));
        return;
    }

    if (!mHasInteractiveView) {
        mHasInteractiveView = true;
        mInteractiveXMin = geometry.xAxis.minValue;
        mInteractiveXMax = geometry.xAxis.maxValue;
        mInteractiveYMin = geometry.yAxis.minValue;
        mInteractiveYMax = geometry.yAxis.maxValue;
    }

    const PlotGeometry baseGeometry = buildPlotGeometry(
        m_pos, m_size, mSeries, mStyle, mSubtitle, mXAxisLabel, mYAxisLabel,
        false, 0.0f, 0.0f, 0.0f, 0.0f
    );

    const float anchorPx = geometry.x0 + geometry.plotWidth * 0.5f;
    const float zoomFactor = std::pow(0.82f, wheelDelta);
    const float minXRange = std::max((baseGeometry.xAxis.maxValue - baseGeometry.xAxis.minValue) * 0.0025f, 1e-4f);
    const float anchorX = unmapPlotX(geometry, anchorPx);
    const float nextXRange = std::max((mInteractiveXMax - mInteractiveXMin) * zoomFactor, minXRange);
    mInteractiveXMin = anchorX - (anchorX - mInteractiveXMin) * zoomFactor;
    mInteractiveXMax = mInteractiveXMin + nextXRange;
    clampRangeToBase(mInteractiveXMin, mInteractiveXMax, baseGeometry.xAxis.minValue, baseGeometry.xAxis.maxValue);

    requestGraphRedraw(screen());
    graphPreviewDebugLog(std::format(
        "ProfileGraph::zoomXBy applied title='{}' normalized={:.3f} x=[{:.3f},{:.3f}] y=[{:.3f},{:.3f}]",
        mTitle,
        wheelDelta,
        mInteractiveXMin, mInteractiveXMax,
        mInteractiveYMin, mInteractiveYMax
    ));
}

void ProfileGraph::draw(NVGcontext* ctx) {
    Widget::draw(ctx);

    const PlotGeometry geometry = buildPlotGeometry(
        m_pos, m_size, mSeries, mStyle, mSubtitle, mXAxisLabel, mYAxisLabel,
        mHasInteractiveView, mInteractiveXMin, mInteractiveXMax, mInteractiveYMin, mInteractiveYMax
    );
    if (graphPreviewDebugEnabled() && mHasInteractiveView) {
        graphPreviewDebugLog(std::format(
            "ProfileGraph::draw title='{}' x=[{:.3f},{:.3f}] y=[{:.3f},{:.3f}] size=({}, {})",
            mTitle,
            mInteractiveXMin, mInteractiveXMax,
            mInteractiveYMin, mInteractiveYMax,
            m_size.x(), m_size.y()
        ));
    }
    const auto colors = deriveGraphColors(mStyle);
    const float x0 = geometry.x0;
    const float y0 = geometry.y0;
    const float x1 = geometry.x1;
    const float y1 = geometry.y1;
    const float plotWidth = geometry.plotWidth;
    const float plotHeight = geometry.plotHeight;

    if (!mStyle.transparentBackground) {
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, m_pos.x() + 1, m_pos.y() + 1, m_size.x() - 2, m_size.y() - 2, 8.0f);
        nvgFillColor(ctx, colors.panelBackground);
        nvgFill(ctx);
    }

    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, m_pos.x() + 0.5f, m_pos.y() + 0.5f, m_size.x() - 1, m_size.y() - 1, 8.0f);
    nvgStrokeColor(ctx, colors.border);
    nvgStroke(ctx);

    if (!mStyle.transparentBackground) {
        nvgBeginPath(ctx);
        nvgRect(ctx, x0, y0, plotWidth, plotHeight);
        nvgFillColor(ctx, colors.plotBackground);
        nvgFill(ctx);
    }

    for (size_t i = 0; i < geometry.yAxis.ticks.size(); ++i) {
        const float tick = geometry.yAxis.ticks[i];
        const float py = mapPlotY(geometry, mStyle, tick);
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, x0, py);
        nvgLineTo(ctx, x1, py);
        nvgStrokeColor(ctx, (i == 0 || i + 1 == geometry.yAxis.ticks.size()) ? colors.gridMajor : colors.gridMinor);
        nvgStrokeWidth(ctx, 1.0f);
        nvgStroke(ctx);
    }

    for (size_t i = 0; i < geometry.xAxis.ticks.size(); ++i) {
        const float tick = geometry.xAxis.ticks[i];
        const float px = mapPlotX(geometry, tick);
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, px, y0);
        nvgLineTo(ctx, px, y1);
        nvgStrokeColor(ctx, (i == 0 || i + 1 == geometry.xAxis.ticks.size()) ? colors.gridMajor : colors.gridMinor);
        nvgStrokeWidth(ctx, 1.0f);
        nvgStroke(ctx);
    }

    if (geometry.bounds.valid) {
        nvgSave(ctx);
        nvgIntersectScissor(ctx, x0, y0, plotWidth, plotHeight);
        for (const auto& item : mSeries) {
            const size_t count = std::min(item.x.size(), item.y.size());
            if (count == 0) {
                continue;
            }

            if (mDisplayMode != EProfileGraphDisplayMode::Scatter) {
                bool started = false;
                nvgBeginPath(ctx);
                for (size_t i = 0; i < count; ++i) {
                    const float x = item.x[i];
                    const float y = item.y[i];
                    if (!std::isfinite(x) || !std::isfinite(y)) {
                        continue;
                    }
                    if (mStyle.yLogScale && y <= 0.0f) {
                        continue;
                    }

                    const float px = mapPlotX(geometry, x);
                    const float py = mapPlotY(geometry, mStyle, y);
                    if (!started) {
                        nvgMoveTo(ctx, px, py);
                        started = true;
                    } else {
                        nvgLineTo(ctx, px, py);
                    }
                }
                nvgStrokeColor(ctx, item.color);
                nvgStrokeWidth(ctx, 1.75f);
                nvgStroke(ctx);
            }

            if (mDisplayMode != EProfileGraphDisplayMode::Line) {
                for (size_t i = 0; i < count; ++i) {
                    const float x = item.x[i];
                    const float y = item.y[i];
                    if (!std::isfinite(x) || !std::isfinite(y)) {
                        continue;
                    }
                    if (mStyle.yLogScale && y <= 0.0f) {
                        continue;
                    }

                    const float px = mapPlotX(geometry, x);
                    const float py = mapPlotY(geometry, mStyle, y);
                    nvgBeginPath(ctx);
                    nvgCircle(ctx, px, py, 2.3f);
                    nvgFillColor(ctx, item.color);
                    nvgFill(ctx);
                }
            }
        }
        nvgRestore(ctx);
    }

    nvgFontFace(ctx, "sans");
    nvgFillColor(ctx, colors.primaryText);
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFontSize(ctx, mStyle.titleFontSize);
    // Title and subtitle start at the plot's left edge, clear of the y-axis tick labels
    // (the top label sits half above the plot and used to run into the title).
    const float titleX = std::max(m_pos.x() + 10.0f, x0);
    nvgText(ctx, titleX, m_pos.y() + 6.0f, mTitle.c_str(), nullptr);

    if (!mSubtitle.empty()) {
        nvgFontSize(ctx, mStyle.subtitleFontSize);
        nvgFillColor(ctx, colors.secondaryText);
        nvgText(ctx, titleX, m_pos.y() + 8.0f + mStyle.titleFontSize, mSubtitle.c_str(), nullptr);
    }

    if (!mXAxisLabel.empty()) {
        nvgFontSize(ctx, mStyle.xAxisFontSize);
        nvgFillColor(ctx, colors.secondaryText);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
        nvgText(ctx, x0 + plotWidth * 0.5f, m_pos.y() + m_size.y() - 5.0f, mXAxisLabel.c_str(), nullptr);
    }

    if (!mYAxisLabel.empty()) {
        nvgFontSize(ctx, mStyle.yAxisFontSize);
        nvgFillColor(ctx, colors.secondaryText);
        nvgSave(ctx);
        nvgTranslate(ctx, m_pos.x() + 12.0f, y0 + plotHeight * 0.5f);
        nvgRotate(ctx, -1.57079632679f);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(ctx, 0.0f, 0.0f, mYAxisLabel.c_str(), nullptr);
        nvgRestore(ctx);
    }

    nvgFontSize(ctx, mStyle.xAxisFontSize);
    nvgFillColor(ctx, colors.primaryText);
    nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    for (float tick : geometry.xAxis.ticks) {
        nvgText(ctx, mapPlotX(geometry, tick), y1 + 3.0f, formatProfileNumber(tick).c_str(), nullptr);
    }

    nvgFontSize(ctx, mStyle.yAxisFontSize);
    nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    for (float tick : geometry.yAxis.ticks) {
        nvgText(ctx, x0 - 4.0f, mapPlotY(geometry, mStyle, tick), formatProfileNumber(tick).c_str(), nullptr);
    }

    if (mStyle.showInteractionHint && !mStyle.interactionHintText.empty()) {
        nvgFontSize(ctx, 9.5f);
        nvgFillColor(ctx, colors.secondaryText);
        nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(ctx, m_pos.x() + m_size.x() - 10.0f, m_pos.y() + 8.0f, mStyle.interactionHintText.c_str(), nullptr);
    }

    if (mHasInteractiveView && geometry.valid) {
        const std::string rangeText = mStyle.yLogScale
            ? std::format("Zoom {:.1f}-{:.1f} / {:.3g}-{:.3g}", mInteractiveXMin, mInteractiveXMax, mInteractiveYMin, mInteractiveYMax)
            : std::format("Zoom {:.1f}-{:.1f} / {:.1f}-{:.1f}", mInteractiveXMin, mInteractiveXMax, mInteractiveYMin, mInteractiveYMax);
        nvgFontSize(ctx, 9.5f);
        nvgFillColor(ctx, colors.secondaryText);
        nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        const float rangeY = (mStyle.showInteractionHint && !mStyle.interactionHintText.empty()) ? (m_pos.y() + 20.0f) : (m_pos.y() + 8.0f);
        nvgText(ctx, m_pos.x() + m_size.x() - 10.0f, rangeY, rangeText.c_str(), nullptr);
    }
}

} // namespace tev
