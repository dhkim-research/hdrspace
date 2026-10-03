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

#include <tev/Image.h>
#include <tev/ImageViewer.h>
#include <tev/FalseColor.h>
#include <tev/WaylandClipboard.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/imageio/ImageSaver.h>
#include <tev/imageio/PngImageSaver.h>

#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
#include <glareeval.h>
#endif

#include <clip.h>

#include <nanogui/button.h>
#include <nanogui/colorwheel.h>
#include <nanogui/combobox.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/menuitem.h>
#include <nanogui/messagedialog.h>
#include <nanogui/popupbutton.h>
#include <nanogui/screen.h>
#include <nanogui/textbox.h>
#include <nanogui/theme.h>
#include <nanogui/vscrollpanel.h>

#include <cctype>
#include <fstream>
#include <iterator>
#ifdef __APPLE__
#    include <nanogui/metal.h>
#endif

#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

using namespace nanogui;
using namespace std;

namespace tev {

namespace {

vector<uint8_t> createNormalizedImageButtonThumbnail(const shared_ptr<Image>& image, Vector2i targetSize) {
    if (!image || targetSize.x() <= 0 || targetSize.y() <= 0 || image->channelGroups().empty()) {
        return {};
    }

    const auto& primaryGroup = image->channelGroups().front();
    const auto groupChannels = image->channelsInGroup(primaryGroup.name);
    if (groupChannels.empty()) {
        return {};
    }

    const Channel* red = nullptr;
    const Channel* green = nullptr;
    const Channel* blue = nullptr;
    const Channel* fallback = nullptr;
    for (const auto& channelName : groupChannels) {
        const Channel* channel = image->channel(channelName);
        if (!channel) {
            continue;
        }
        const string tail = toLower(string{Channel::tail(channel->name())});
        if (tail == "a") {
            continue;
        }
        if (!fallback) {
            fallback = channel;
        }
        if (tail == "r") {
            red = channel;
        } else if (tail == "g") {
            green = channel;
        } else if (tail == "b") {
            blue = channel;
        }
    }

    if (!fallback) {
        return {};
    }

    if (!red) red = fallback;
    if (!green) green = fallback;
    if (!blue) blue = fallback;

    const Vector2i size = image->size();
    if (size.x() <= 0 || size.y() <= 0) {
        return {};
    }

    vector<float> luminanceSamples;
    const size_t pixelCount = static_cast<size_t>(size.x()) * static_cast<size_t>(size.y());
    const size_t sampleStep = std::max<size_t>(1, pixelCount / 120000);
    luminanceSamples.reserve(pixelCount / sampleStep + 1);
    for (size_t i = 0; i < pixelCount; i += sampleStep) {
        const float r = red->dynamicAt(i);
        const float g = green->dynamicAt(i);
        const float b = blue->dynamicAt(i);
        luminanceSamples.push_back(0.2126f * r + 0.7152f * g + 0.0722f * b);
    }

    float whitePoint = 1.0f;
    if (!luminanceSamples.empty()) {
        const size_t percentileIndex = std::min(
            luminanceSamples.size() - 1,
            static_cast<size_t>(floor(static_cast<double>(luminanceSamples.size() - 1) * 0.99))
        );
        nth_element(luminanceSamples.begin(), luminanceSamples.begin() + static_cast<ptrdiff_t>(percentileIndex), luminanceSamples.end());
        whitePoint = std::max(1e-5f, luminanceSamples[percentileIndex]);
    }
    const float exposureScale = 0.85f / whitePoint;

    const float srcAspect = static_cast<float>(size.x()) / static_cast<float>(size.y());
    const float dstAspect = static_cast<float>(targetSize.x()) / static_cast<float>(targetSize.y());
    float drawWidth = static_cast<float>(targetSize.x());
    float drawHeight = static_cast<float>(targetSize.y());
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    if (srcAspect > dstAspect) {
        drawHeight = drawWidth / srcAspect;
        offsetY = (static_cast<float>(targetSize.y()) - drawHeight) * 0.5f;
    } else {
        drawWidth = drawHeight * srcAspect;
        offsetX = (static_cast<float>(targetSize.x()) - drawWidth) * 0.5f;
    }

    auto sample = [&](const Channel* channel, float x, float y) -> float {
        x = clamp(x, 0.0f, static_cast<float>(size.x() - 1));
        y = clamp(y, 0.0f, static_cast<float>(size.y() - 1));
        const int x0 = static_cast<int>(floor(x));
        const int y0 = static_cast<int>(floor(y));
        const int x1 = std::min(x0 + 1, size.x() - 1);
        const int y1 = std::min(y0 + 1, size.y() - 1);
        const float tx = x - static_cast<float>(x0);
        const float ty = y - static_cast<float>(y0);
        const float a = channel->dynamicAt({x0, y0});
        const float b = channel->dynamicAt({x1, y0});
        const float c = channel->dynamicAt({x0, y1});
        const float d = channel->dynamicAt({x1, y1});
        const float top = a + (b - a) * tx;
        const float bottom = c + (d - c) * tx;
        return top + (bottom - top) * ty;
    };

    vector<uint8_t> rgba(static_cast<size_t>(targetSize.x()) * static_cast<size_t>(targetSize.y()) * 4, uint8_t{0});
    for (int y = 0; y < targetSize.y(); ++y) {
        for (int x = 0; x < targetSize.x(); ++x) {
            const size_t dst = (static_cast<size_t>(y) * static_cast<size_t>(targetSize.x()) + static_cast<size_t>(x)) * 4;
            rgba[dst + 0] = 18;
            rgba[dst + 1] = 22;
            rgba[dst + 2] = 29;
            rgba[dst + 3] = 255;

            const float fx = static_cast<float>(x) + 0.5f;
            const float fy = static_cast<float>(y) + 0.5f;
            if (fx < offsetX || fx > offsetX + drawWidth || fy < offsetY || fy > offsetY + drawHeight) {
                continue;
            }

            const float u = clamp((fx - offsetX) / std::max(1.0f, drawWidth), 0.0f, 1.0f);
            const float v = clamp((fy - offsetY) / std::max(1.0f, drawHeight), 0.0f, 1.0f);
            const float srcX = u * static_cast<float>(size.x() - 1);
            const float srcY = v * static_cast<float>(size.y() - 1);
            const array<const Channel*, 3> channels{red, green, blue};
            for (size_t c = 0; c < 3; ++c) {
                const float mapped = 1.0f - exp(-std::max(0.0f, sample(channels[c], srcX, srcY)) * exposureScale);
                const float gamma = pow(clamp(mapped, 0.0f, 1.0f), 1.0f / 2.2f);
                rgba[dst + c] = static_cast<uint8_t>(lround(gamma * 255.0f));
            }
        }
    }

    return rgba;
}

class OverlayScrollPanel final : public VScrollPanel {
public:
    using VScrollPanel::VScrollPanel;

    void draw(NVGcontext* ctx) override {
        nvgSave(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
        nvgFillColor(ctx, Color{29, 31, 33, 255});
        nvgFill(ctx);
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, m_pos.x() + 0.5f, m_pos.y());
        nvgLineTo(ctx, m_pos.x() + 0.5f, m_pos.y() + m_size.y());
        nvgStrokeColor(ctx, Color{57, 60, 63, 255});
        nvgStrokeWidth(ctx, 1.0f);
        nvgStroke(ctx);
        nvgRestore(ctx);
        VScrollPanel::draw(ctx);
    }
};

// The toolbar belongs to the docked inspector's single opaque surface.
class FloatingToolbarCard final : public Widget {
public:
    using Widget::Widget;
};

// hdrspace inspector section heading: 11 bold grey capitals under a hairline, like the
// tool-rail group titles and the other inspector panels.
class SidebarSectionHeader final : public Widget {
public:
    SidebarSectionHeader(Widget* parent, string title) : Widget(parent) {
        for (auto& c : title) c = (char)std::toupper((unsigned char)c);
        mTitle = std::move(title);
        set_fixed_height(30);
    }

    void draw(NVGcontext* ctx) override {
        const float x = (float)m_pos.x(), y = (float)m_pos.y(), w = (float)m_size.x(), h = (float)m_size.y();
        nvgSave(ctx);
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, x, y + 4.5f);
        nvgLineTo(ctx, x + w, y + 4.5f);
        nvgStrokeColor(ctx, Color(255, 255, 255, 22));
        nvgStrokeWidth(ctx, 1.0f);
        nvgStroke(ctx);
        nvgFontFace(ctx, "sans-bold");
        nvgFontSize(ctx, 11.0f);
        nvgTextLetterSpacing(ctx, 0.8f);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        nvgFillColor(ctx, Color(170, 174, 179, 210));
        nvgText(ctx, x, y + h - 2.0f, mTitle.c_str(), nullptr);
        nvgTextLetterSpacing(ctx, 0.0f);
        nvgRestore(ctx);
    }

private:
    string mTitle;
};

class HairlineDivider final : public Widget {
public:
    using Widget::Widget;

    void draw(NVGcontext* ctx) override {
        const float x = static_cast<float>(m_pos.x());
        const float y = static_cast<float>(m_pos.y()) + 0.5f;
        const float w = static_cast<float>(m_size.x());

        nvgSave(ctx);
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, x + 10.0f, y);
        nvgLineTo(ctx, x + std::max(10.0f, w - 10.0f), y);
        nvgStrokeColor(ctx, Color(255, 255, 255, 18));
        nvgStrokeWidth(ctx, 1.0f);
        nvgStroke(ctx);
        nvgRestore(ctx);
    }
};

float falseColorCoordForUi(float value, EFalseColorScaleMode scaleMode, Vector2f range) {
    const float minValue = std::max(range.x(), 0.0f);
    const float maxValue = std::max(range.y(), minValue + 1e-6f);

    if (scaleMode == EFalseColorScaleMode::Linear) {
        return clamp((value - minValue) / (maxValue - minValue), 0.0f, 1.0f);
    }

    const float safeMin = std::max(minValue, 1e-6f);
    const float safeValue = std::max(value, safeMin);
    const float base = static_cast<float>(static_cast<int>(scaleMode) + 1);
    const float invLogBase = 1.0f / std::log(base);
    const float minLog = std::log(safeMin) * invLogBase;
    const float maxLog = std::log(maxValue) * invLogBase;
    const float valueLog = std::log(safeValue) * invLogBase;
    return clamp((valueLog - minLog) / std::max(maxLog - minLog, 1e-6f), 0.0f, 1.0f);
}

std::vector<float> computeFalseColorBarTicks(int tickMode, std::string_view manualValues, EFalseColorScaleMode scaleMode, Vector2f range) {
    std::vector<float> ticks;

    if (tickMode == 3) {
        std::string manual{manualValues};
        for (char& c : manual) {
            if (c == ',' || c == ';') {
                c = ' ';
            }
        }

        std::stringstream stream{manual};
        float value = 0.0f;
        while (stream >> value) {
            ticks.push_back(value);
        }
        return ticks;
    }

    const int tickCount = tickMode == 0 ? 3 : tickMode == 2 ? 10 : 5;
    ticks.reserve((size_t)tickCount);
    for (int i = 0; i < tickCount; ++i) {
        const float t = tickCount == 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(tickCount - 1);
        if (scaleMode == EFalseColorScaleMode::Linear) {
            ticks.push_back(range.x() + t * (range.y() - range.x()));
        } else {
            const float safeMin = std::max(range.x(), 1e-6f);
            const float base = static_cast<float>(static_cast<int>(scaleMode) + 1);
            const float invLogBase = 1.0f / std::log(base);
            const float minLog = std::log(safeMin) * invLogBase;
            const float maxLog = std::log(std::max(range.y(), safeMin + 1e-6f)) * invLogBase;
            const float valueLog = minLog + t * (maxLog - minLog);
            ticks.push_back(std::pow(base, valueLog));
        }
    }

    return ticks;
}

class FalseColorBarWidget final : public Widget {
public:
    using Widget::Widget;

    Vector2i preferred_size_impl(NVGcontext*) const override { return {292, 74}; }

    void setColorMap(EFalseColorMap map) { mMap = map; }
    void setScaleMode(EFalseColorScaleMode mode) { mScaleMode = mode; }
    void setRange(Vector2f range) { mRange = range; }
    void setTicks(std::vector<float> ticks) { mTicks = std::move(ticks); }
    EFalseColorMap colorMap() const { return mMap; }
    EFalseColorScaleMode scaleMode() const { return mScaleMode; }
    Vector2f range() const { return mRange; }
    const std::vector<float>& ticks() const { return mTicks; }

    void draw(NVGcontext* ctx) override {
        Widget::draw(ctx);

        const auto frame = Box2f{
            {m_pos.x() + 2.0f, m_pos.y() + 2.0f},
            {m_pos.x() + m_size.x() - 2.0f, m_pos.y() + 30.0f},
        };

        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, frame.min.x(), frame.min.y(), frame.size().x(), frame.size().y(), 4.0f);
        nvgFillColor(ctx, Color{18, 220});
        nvgFill(ctx);

        const auto colormap = mMap == EFalseColorMap::Viridis ? colormap::viridis() : colormap::turbo();
        static constexpr int segments = 128;
        const float segmentWidth = frame.size().x() / static_cast<float>(segments);
        for (int i = 0; i < segments; ++i) {
            const float t0 = static_cast<float>(i) / static_cast<float>(segments);
            const int index = std::clamp((int)(t0 * static_cast<float>(colormap.size() / 4)), 0, (int)colormap.size() / 4 - 1) * 4;
            nvgBeginPath(ctx);
            nvgRect(ctx, frame.min.x() + segmentWidth * i, frame.min.y(), segmentWidth + 1.0f, frame.size().y());
            nvgFillColor(ctx, Color{colormap[index], colormap[index + 1], colormap[index + 2], 1.0f});
            nvgFill(ctx);
        }

        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, frame.min.x() + 0.5f, frame.min.y() + 0.5f, frame.size().x() - 1.0f, frame.size().y() - 1.0f, 4.0f);
        nvgStrokeColor(ctx, Color{255, 40});
        nvgStrokeWidth(ctx, 1.0f);
        nvgStroke(ctx);

        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 12.0f);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgFillColor(ctx, Color{235, 220});

        for (float tick : mTicks) {
            const float t = falseColorCoordForUi(tick, mScaleMode, mRange);
            const float x = frame.min.x() + t * (frame.size().x() - 1.0f);

            nvgBeginPath(ctx);
            nvgMoveTo(ctx, x, frame.max.y());
            nvgLineTo(ctx, x, frame.max.y() + 6.0f);
            nvgStrokeColor(ctx, Color{255, 90});
            nvgStrokeWidth(ctx, 1.0f);
            nvgStroke(ctx);

            const std::string label = std::format("{:.5g}", tick);
            float bounds[4];
            nvgTextBounds(ctx, 0.0f, 0.0f, label.c_str(), nullptr, bounds);
            const float textWidth = bounds[2] - bounds[0];
            const float padding = 8.0f;
            float drawX = clamp(x, frame.min.x() + textWidth * 0.5f + padding, frame.max.x() - textWidth * 0.5f - padding);
            drawTextWithShadow(ctx, drawX, frame.max.y() + 8.0f, label);
        }
    }

private:
    EFalseColorMap mMap = EFalseColorMap::Turbo;
    EFalseColorScaleMode mScaleMode = EFalseColorScaleMode::Log2;
    Vector2f mRange = {0.03125f, 32.0f};
    std::vector<float> mTicks;
};

} // namespace

namespace {

enum InspectionPresetIndex {
    InspectionSource = 0,
    InspectionRaw = 1,
    InspectionRad = 2,
    InspectionSRgb = 3,
    InspectionXyz = 4,
    InspectionLuminance = 5,
#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
    InspectionL = 6,
    InspectionM = 7,
    InspectionRod = 8,
    InspectionLPlusM = 9,
    InspectionAdaptation = 10,
    InspectionDetectableContrast = 11,
    InspectionEqvLuminance = 12,
#endif
};

int tonemapIcon(ETonemap tonemap) {
    switch (tonemap) {
        case ETonemap::SRGB: return FA_DESKTOP;
        case ETonemap::Gamma: return FA_MAGIC;
        case ETonemap::FalseColor: return FA_CHART_BAR;
        case ETonemap::PositiveNegative: return FA_CHART_AREA;
        case ETonemap::Reinhard: return FA_ADJUST;
        default: return 0;
    }
}

Color viewerSidebarInk(int alpha = 232) {
    return Color(238, 239, 240, alpha);
}

Color viewerSidebarButtonFill(bool /*strong*/ = false) {
    return Color{0, 0}; // Use the shared theme for every interaction state.
}

Color viewerToolbarButtonFill(bool /*strong*/ = false) {
    return Color{0, 0}; // Use the shared theme for every interaction state.
}

void styleViewerSidebarButton(Button* button, int icon = 0, bool compact = false, bool strong = false) {
    if (!button) {
        return;
    }

    if (icon != 0) {
        button->set_icon(icon);
        button->set_icon_position(Button::IconPosition::LeftCentered);
    }

    button->set_font_size(compact ? 13 : 14);
    button->set_padding(compact ? Vector2i{8, 6} : Vector2i{10, 7});
    button->set_background_color(viewerSidebarButtonFill(strong));
    button->set_text_color(viewerSidebarInk());
}

void styleViewerToolbarButton(Button* button, int icon = 0, bool strong = false) {
    if (!button) {
        return;
    }

    if (icon != 0) {
        button->set_icon(icon);
        button->set_icon_position(Button::IconPosition::LeftCentered);
    }

    button->set_font_size(13);
    button->set_padding(Vector2i{7, 5});
    button->set_background_color(viewerToolbarButtonFill(strong));
    button->set_text_color(viewerSidebarInk(224));
}

chroma_t radChroma() {
    chroma_t result;
    result[0] = Vector2f{0.6400f, 0.3300f};
    result[1] = Vector2f{0.2900f, 0.6000f};
    result[2] = Vector2f{0.1500f, 0.0600f};
    result[3] = Vector2f{0.3333f, 0.3333f};
    return result;
}

bool approxEqual(float a, float b, float eps = 5e-3f) {
    return std::abs(a - b) <= eps;
}

std::string sanitizedStem(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char ch : text) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') {
            result.push_back(ch);
        } else {
            result.push_back('_');
        }
    }

    if (result.empty()) {
        result = "image";
    }

    return result;
}

bool sameChroma(const chroma_t& a, const chroma_t& b, float eps = 5e-3f) {
    for (size_t i = 0; i < a.size(); ++i) {
        if (!approxEqual(a[i].x(), b[i].x(), eps) || !approxEqual(a[i].y(), b[i].y(), eps)) {
            return false;
        }
    }
    return true;
}

string channelInspectionLabelForPreset(int presetIndex, size_t index) {
    static constexpr array<string_view, 3> rgbNames = {"R", "G", "B"};
    static constexpr array<string_view, 3> xyzNames = {"X", "Y", "Z"};

    if (presetIndex == InspectionLuminance) {
        return "Luminance";
    }

#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
    switch (presetIndex) {
        case InspectionL: return "L";
        case InspectionM: return "M";
        case InspectionRod: return "Rod";
        case InspectionLPlusM: return "L+M";
        case InspectionAdaptation: return "Adaptation";
        case InspectionDetectableContrast: return "Detectable contrast";
        case InspectionEqvLuminance: return "Eqv_Luminance";
        default: break;
    }
#endif

    if (index >= 3) {
        return "C";
    }

    return presetIndex == InspectionXyz ? std::string{xyzNames[index]} : std::string{rgbNames[index]};
}

vector<string> highlightItemsForPreset(int presetIndex) {
    switch (presetIndex) {
        case InspectionXyz: return {"XYZ", "X", "Y", "Z"};
        case InspectionLuminance: return {"Luminance"};
#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
        case InspectionL: return {"L"};
        case InspectionM: return {"M"};
        case InspectionRod: return {"Rod"};
        case InspectionLPlusM: return {"L+M"};
        case InspectionAdaptation: return {"Adaptation"};
        case InspectionDetectableContrast: return {"Detectable contrast"};
        case InspectionEqvLuminance: return {"Eqv_Luminance"};
#endif
        default: return {"RGB", "R", "G", "B"};
    }
}

Matrix3f primariesToXyzMatrix(const chroma_t& chroma) {
    Matrix3f pxyz = Matrix3f{1.0f};
    for (int c = 0; c < 3; ++c) {
        const float x = chroma[(size_t)c].x();
        const float y = chroma[(size_t)c].y();
        const float z = 1.0f - x - y;
        pxyz.m[(size_t)c][0] = x;
        pxyz.m[(size_t)c][1] = y;
        pxyz.m[(size_t)c][2] = z;
    }

    Vector3f wxyz;
    wxyz[0] = chroma[3].x() / chroma[3].y();
    wxyz[1] = 1.0f;
    wxyz[2] = (1.0f - chroma[3].x() - chroma[3].y()) / chroma[3].y();

    const Vector3f scales = inverse(pxyz) * wxyz;
    Matrix3f rgbToXyz = pxyz;
    for (int c = 0; c < 3; ++c) {
        rgbToXyz.m[(size_t)c][0] *= scales[c];
        rgbToXyz.m[(size_t)c][1] *= scales[c];
        rgbToXyz.m[(size_t)c][2] *= scales[c];
    }

    return rgbToXyz;
}

optional<string_view> findAttributeValue(std::span<const AttributeNode> nodes, std::string_view name) {
    for (const auto& node : nodes) {
        if (node.name == name) {
            return node.value;
        }

        if (const auto child = findAttributeValue(node.children, name)) {
            return child;
        }
    }

    return nullopt;
}

void findAttributeValues(std::span<const AttributeNode> nodes, std::string_view name, std::vector<std::string_view>& result) {
    for (const auto& node : nodes) {
        if (node.name == name) {
            result.push_back(node.value);
        }

        findAttributeValues(node.children, name, result);
    }
}

bool imageChannelsAreMonochrome(const Image& image, std::span<const std::string> channels) {
    if (channels.size() < 3) {
        return false;
    }

    const Channel* c0 = image.channel(channels[0]);
    const Channel* c1 = image.channel(channels[1]);
    const Channel* c2 = image.channel(channels[2]);
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

vector<float> parseAttributeFloatList(string_view value) {
    vector<float> result;
    for (const auto token : splitWhitespace(trim(value))) {
        float parsed = 0.0f;
        if (fromChars(token, parsed)) {
            result.push_back(parsed);
        }
    }
    return result;
}

optional<chroma_t> parseTargetChroma(std::span<const AttributeNode> nodes) {
    const auto primariesValue = findAttributeValue(nodes, "TargetPrimaries");
    const auto whiteValue = findAttributeValue(nodes, "TargetWhitePoint");
    if (primariesValue && whiteValue) {
        const auto primaries = parseAttributeFloatList(*primariesValue);
        const auto white = parseAttributeFloatList(*whiteValue);
        if (primaries.size() == 6 && white.size() == 2) {
            chroma_t result;
            result[0] = Vector2f{primaries[0], primaries[1]};
            result[1] = Vector2f{primaries[2], primaries[3]};
            result[2] = Vector2f{primaries[4], primaries[5]};
            result[3] = Vector2f{white[0], white[1]};
            return result;
        }
    }

    const auto radiancePrimariesValue = findAttributeValue(nodes, "PRIMARIES");
    if (!radiancePrimariesValue) {
        return nullopt;
    }

    const auto values = parseAttributeFloatList(*radiancePrimariesValue);
    if (values.size() != 8) {
        return nullopt;
    }

    chroma_t result;
    result[0] = Vector2f{values[0], values[1]};
    result[1] = Vector2f{values[2], values[3]};
    result[2] = Vector2f{values[4], values[5]};
    result[3] = Vector2f{values[6], values[7]};
    return result;
}

optional<float> parseExposureCompensationScale(std::span<const AttributeNode> nodes) {
    const auto formatValue = findAttributeValue(nodes, "FORMAT");
    if (!formatValue) {
        return nullopt;
    }

    std::string format = std::string{*formatValue};
    std::ranges::transform(format, format.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (format.find("rgbe") == std::string::npos && format.find("xyze") == std::string::npos) {
        return nullopt;
    }

    std::vector<std::string_view> values;
    findAttributeValues(nodes, "EXPOSURE", values);
    if (values.empty()) {
        return nullopt;
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
        return nullopt;
    }

    return scale;
}

optional<Matrix3f> parseSensorToXyz(std::span<const AttributeNode> nodes) {
    if (const auto sensorToXyzValue = findAttributeValue(nodes, "SENSOR2XYZ")) {
        const auto sensorToXyzValues = parseAttributeFloatList(*sensorToXyzValue);
        if (sensorToXyzValues.size() == 9) {
            Matrix3f sensorToXyz = Matrix3f{1.0f};
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    sensorToXyz.m[col][row] = sensorToXyzValues[(size_t)(row * 3 + col)];
                }
            }
            return sensorToXyz;
        }
    }

    const auto xyzcamValue = findAttributeValue(nodes, "XYZCAM");
    if (!xyzcamValue) {
        return nullopt;
    }

    const auto xyzcamValues = parseAttributeFloatList(*xyzcamValue);
    if (xyzcamValues.size() != 9) {
        return nullopt;
    }

    Matrix3f xyzCam = Matrix3f{1.0f};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            xyzCam.m[col][row] = xyzcamValues[(size_t)(row * 3 + col)];
        }
    }

    if (const auto premultValue = findAttributeValue(nodes, "CAM_PREMULTIPLIERS")) {
        const auto premults = parseAttributeFloatList(*premultValue);
        if (premults.size() >= 3) {
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    xyzCam.m[col][row] *= premults[(size_t)row];
                }
            }
        }
    }

    return inverse(xyzCam);
}

enum class EDetectedProjection {
    Unknown,
    Equisolid,
    Equidistant,
};

struct ProjectionMetadata {
    EDetectedProjection projection = EDetectedProjection::Unknown;
    bool cropped = false;
    bool square = false;
};

struct ViewAngleMetadata {
    bool hasView = false;
    double horizontalDegrees = 0.0;
    double verticalDegrees = 0.0;
};

EDetectedProjection parseProjectionName(std::string_view value) {
    std::string token{trim(value)};
    std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (token == "equisolid")
        return EDetectedProjection::Equisolid;
    if (token == "equidistant" || token == "equiangular")
        return EDetectedProjection::Equidistant;
    return EDetectedProjection::Unknown;
}

std::string projectionName(EDetectedProjection projection) {
    switch (projection) {
        case EDetectedProjection::Equisolid: return "equisolid";
        case EDetectedProjection::Equidistant: return "equidistant";
        default: return "unknown";
    }
}

int projectionSelectionIndex(EDetectedProjection projection) {
    switch (projection) {
        case EDetectedProjection::Equisolid: return 1;
        case EDetectedProjection::Equidistant: return 2;
        default: return 0;
    }
}

EDetectedProjection projectionFromSelectionIndex(int index) {
    switch (index) {
        case 1: return EDetectedProjection::Equisolid;
        case 2: return EDetectedProjection::Equidistant;
        default: return EDetectedProjection::Unknown;
    }
}

void findProjectionCommandValues(std::span<const AttributeNode> nodes, std::vector<std::string_view>& values) {
    for (const auto& node : nodes) {
        if (node.name == "MERGEHDR_COMMAND" || node.name.rfind("MERGEHDR_STEP_", 0) == 0) {
            values.push_back(node.value);
        }
        findProjectionCommandValues(node.children, values);
    }
}

ProjectionMetadata detectProjectionMetadata(const std::shared_ptr<Image>& image) {
    ProjectionMetadata metadata;
    if (!image) {
        return metadata;
    }

    const auto attributes = image->attributes();
    metadata.cropped = findAttributeValue(attributes, "MERGEHDR_CROP").has_value();
    metadata.square = image->size().x() == image->size().y();

    if (const auto explicitProjection = findAttributeValue(attributes, "MERGEHDR_PROJECTION")) {
        metadata.projection = parseProjectionName(*explicitProjection);
        if (metadata.projection != EDetectedProjection::Unknown) {
            return metadata;
        }
    }

    if (const auto view = findAttributeValue(attributes, "VIEW")) {
        if (view->find("-vta") != string_view::npos) {
            metadata.projection = EDetectedProjection::Equidistant;
            return metadata;
        }
    }

    std::vector<std::string_view> commands;
    findProjectionCommandValues(attributes, commands);
    for (const auto value : commands) {
        if (value.find("--solid2ang") != string_view::npos ||
                (value.find("--fisheye") != string_view::npos && value.find("--no-fisheye") == string_view::npos)) {
            metadata.projection = EDetectedProjection::Equidistant;
            return metadata;
        }
    }

    if (metadata.cropped && metadata.square) {
        for (const auto value : commands) {
            if (value.find("--no-fisheye") != string_view::npos) {
                metadata.projection = EDetectedProjection::Equisolid;
                return metadata;
            }
        }
    }

    return metadata;
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

ViewAngleMetadata detectViewAngleMetadata(const std::shared_ptr<Image>& image) {
    ViewAngleMetadata metadata;
    if (!image) {
        return metadata;
    }

    if (const auto view = findAttributeValue(image->attributes(), "VIEW")) {
        parseViewAngles(*view, metadata.horizontalDegrees, metadata.verticalDegrees);
        metadata.hasView = metadata.horizontalDegrees > 0.0 || metadata.verticalDegrees > 0.0;
    }

    return metadata;
}

std::string formatDegrees(double degrees) {
    if (degrees <= 0.0) {
        return "none";
    }

    const double rounded = std::round(degrees);
    if (std::abs(degrees - rounded) < 0.05) {
        return std::format("{}\u00B0", static_cast<int>(rounded));
    }

    return std::format("{:.1f}\u00B0", degrees);
}

} // namespace

static constexpr int SIDEBAR_MIN_WIDTH = 304;
static constexpr float CROP_MIN_SIZE = 3;

static constexpr array<pair<EWpPrimaries, string_view>, 11> PRIMARIES = {
    {
     {EWpPrimaries::SRGB, "sRGB"},
     {EWpPrimaries::BT2020, "BT.2020"},
     {EWpPrimaries::DCIP3, "DCI P3"},
     {EWpPrimaries::DisplayP3, "Display P3"},
     {EWpPrimaries::AdobeRGB, "Adobe RGB"},
     {EWpPrimaries::ProPhotoRGB, "ProPhoto RGB"},
     {EWpPrimaries::NTSC, "NTSC"},
     {EWpPrimaries::PAL, "PAL"},
     {EWpPrimaries::PALM, "PAL-M"},
     {EWpPrimaries::Film, "Generic Film"},
     {EWpPrimaries::CIE1931XYZ, "CIE 1931 XYZ"},
     }
};

static constexpr array<pair<ituth273::ETransfer, string_view>, 13> TRANSFERS = {
    {
     {ituth273::ETransfer::Linear, "Linear"},
     {ituth273::ETransfer::SRGB, "sRGB"},
     {ituth273::ETransfer::PQ, "PQ"},
     {ituth273::ETransfer::HLG, "HLG"},
     {ituth273::ETransfer::Gamma22, "Gamma 2.2"},
     {ituth273::ETransfer::Gamma28, "Gamma 2.8"},
     {ituth273::ETransfer::Log100, "Log100"},
     {ituth273::ETransfer::Log100Sqrt10, "Log100 Sqrt10"},
     {ituth273::ETransfer::BT709, "BT.709/601/2020"},
     // Same as above
    // {ituth273::ETransfer::BT601,          "BT.601"          },
    // {ituth273::ETransfer::BT202010bit,    "BT.2020 10-bit"  },
    // {ituth273::ETransfer::BT202012bit,    "BT.2020"         },
        {ituth273::ETransfer::BT1361Extended, "BT.1361 Ext."},
     {ituth273::ETransfer::SMPTE240, "SMPTE 240M"},
     {ituth273::ETransfer::SMPTE428, "SMPTE ST 428-1"},
     {ituth273::ETransfer::IEC61966_2_4, "IEC 61966-2-4"},
     }
};

ImageViewer::ImageViewer(
    Vector2i size,
    const shared_ptr<BackgroundImagesLoader>& imagesLoader,
    weak_ptr<Ipc> ipc,
    bool maximize,
    bool showUi,
    bool floatBuffer,
    bool sidebarOnRight
) :
    Screen{size, "hdrspace", true, maximize, false, true, true, floatBuffer},
    mSidebarOnRight{sidebarOnRight},
    mImagesLoader{imagesLoader},
    mIpc{ipc},
    mMaximizedLaunch{maximize} {

    // At this point we no longer need the standalone console (if it exists).
    toggleConsole();

    // Get monitor configuration to figure out how large the tev window may maximally become. This will later get overwritten once
    // glfwGetWindowCurrentMonitor() works (it does not while the window is still getting constructed).
    {
        int monitorCount;
        auto** monitors = glfwGetMonitors(&monitorCount);
        if (monitors && monitorCount > 0) {
            Vector2i monitorMin{numeric_limits<int>::max(), numeric_limits<int>::max()},
                monitorMax{numeric_limits<int>::min(), numeric_limits<int>::min()};

            for (int i = 0; i < monitorCount; ++i) {
                Vector2i monitorPos, monitorSize;
                glfwGetMonitorWorkarea(monitors[i], &monitorPos.x(), &monitorPos.y(), &monitorSize.x(), &monitorSize.y());
                monitorMin = min(monitorMin, monitorPos);
                monitorMax = max(monitorMax, monitorPos + monitorSize);
            }

            mMinWindowPos = monitorMin;
            mMaxWindowSize = min(mMaxWindowSize, Vector2f{max(monitorMax - monitorMin, Vector2i{1024, 800})});
        }
    }

    // Try to get the current monitor size right away. Better to have it early. The function will get called again before every draw to
    // handle monitor changes as well as the case where glfwGetWindowCurrentMonitor() did not work yet.
    updateCurrentMonitorSize();

    m_background = Color{22, 24, 26, 255};

    mVerticalScreenSplit = new Widget{this};
    mVerticalScreenSplit->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill});

    auto horizontalScreenSplit = new Widget(mVerticalScreenSplit);
    horizontalScreenSplit->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill});

    if (mSidebarOnRight) {
        mImageCanvas = new ImageCanvas{horizontalScreenSplit};
        mImageCanvas->setPixelRatio(pixel_ratio());
        mSidebar = new OverlayScrollPanel{horizontalScreenSplit};
    } else {
        mSidebar = new VScrollPanel{horizontalScreenSplit};
        mImageCanvas = new ImageCanvas{horizontalScreenSplit};
        mImageCanvas->setPixelRatio(pixel_ratio());
    }

    mSidebar->set_fixed_width(SIDEBAR_MIN_WIDTH);
    mSidebar->set_visible(showUi);

    mSidebarRoot = new Widget{mSidebar};
    mSidebarRoot->set_layout(new BoxLayout{
        Orientation::Vertical,
        Alignment::Fill,
        mSidebarOnRight ? 4 : 0,
        0
    });

    if (mSidebarOnRight) {
        auto* topInset = new Widget{mSidebarRoot};
        topInset->set_fixed_height(4);
    }

    mSidebarToolbar = new Widget{mSidebarRoot};
    mSidebarToolbar->set_fixed_height(mSidebarOnRight ? 44 : 0);

    mHeaderOpenImageButton = new Button{mSidebarToolbar, ""};
    mHeaderOpenImageButton->set_callback([this] { openImageDialog(); });
    mHeaderOpenImageButton->set_font_size(14);
    mHeaderOpenImageButton->set_icon(FA_FOLDER_OPEN);
    mHeaderOpenImageButton->set_icon_position(Button::IconPosition::LeftCentered);
    mHeaderOpenImageButton->set_padding({10, 7});
    mHeaderOpenImageButton->set_tooltip(format("Open ({}+O)", HelpWindow::COMMAND));
    mHeaderOpenImageButton->set_visible(mSidebarOnRight);
    mHelpButton = new Button{mSidebarToolbar, "", FA_QUESTION};
    mHelpButton->set_change_callback([this](bool) { toggleHelpWindow(); });
    mHelpButton->set_font_size(14);
    mHelpButton->set_tooltip("Keybindings and an overview of the app.");
    mHelpButton->set_flags(Button::ToggleButton);

    if (mSidebarOnRight) {
        mSidebarTitleHost = new Widget{mSidebarToolbar};
        mSidebarTitleHost->set_layout(new GridLayout{Orientation::Horizontal, 1, Alignment::Fill, 6, 0});
    }

    mSidebarLayout = new Widget{mSidebarRoot};
    mSidebarLayout->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    if (mSidebarOnRight) {
        mFloatingToolbarCard = new FloatingToolbarCard{mSidebarLayout};
        mFloatingToolbarCard->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 4, 0});

        auto* toolbarInset = new Widget{mFloatingToolbarCard};
        toolbarInset->set_fixed_height(2);
    }

    // Tonemapping sectionim
    {
        auto panel = new Widget{mSidebarLayout};
        panel->set_tooltip("Various tonemapping options. Hover the individual controls to learn more!");

        if (mSidebarOnRight) {
            panel->set_visible(false);
            panel->set_fixed_height(0);

            auto* titlePanel = new Widget{mSidebarTitleHost};
            titlePanel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 1, 0});

            auto* topSpacer = new Widget{titlePanel};
            topSpacer->set_fixed_height(4);

            auto* sectionLabel = new Label{titlePanel, "Viewer", "sans-bold", 18};
            sectionLabel->set_color(viewerSidebarInk(255));
        } else {
            panel->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 5});
            new Label{panel, "Tonemapping", "sans-bold", 25};
        }

        // Exposure label and slider
        {
            panel = new Widget{mSidebarLayout};
            panel->set_layout(mSidebarOnRight ? (Layout*)new BoxLayout{Orientation::Horizontal, Alignment::Middle, 5, 8}
                                              : (Layout*)new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

            mExposureLabel = new Label{panel, "", "sans", mSidebarOnRight ? 13 : 14};
            if (mSidebarOnRight) {
                mExposureLabel->set_color(Color(170, 174, 179, 215));
                mExposureLabel->set_fixed_width(104);
            }

            mExposureSlider = new Slider{panel};
            mExposureSlider->set_range({-5.0f, 5.0f});
            mExposureSlider->set_callback([this](float value) { setExposure(value); });
            setExposure(0);

            panel->set_tooltip(
                "Exposure scales the brightness of an image prior to tonemapping by 2^Exposure.\n\n"
                "Keyboard shortcuts: E and Shift+E"
            );
        }

        // Offset/Gamma label and slider
        {
            panel = new Widget{mSidebarLayout};
            panel->set_layout(mSidebarOnRight ? (Layout*)new BoxLayout{Orientation::Vertical, Alignment::Fill, 5, 4}
                                              : (Layout*)new GridLayout{Orientation::Vertical, 2, Alignment::Fill, 5, 0});
            Widget* offsetRow = panel;
            Widget* gammaRow = panel;
            if (mSidebarOnRight) {
                offsetRow = new Widget{panel};
                offsetRow->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 0, 8});
                gammaRow = new Widget{panel};
                gammaRow->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 0, 8});
            }

            mOffsetLabel = new Label{offsetRow, "", "sans", mSidebarOnRight ? 13 : 14};
            if (mSidebarOnRight) {
                mOffsetLabel->set_color(Color(170, 174, 179, 215));
                mOffsetLabel->set_fixed_width(104);
            }

            mOffsetSlider = new Slider{offsetRow};
            mOffsetSlider->set_range({-1.0f, 1.0f});
            mOffsetSlider->set_callback([this](float value) { setOffset(value); });
            setOffset(0);

            mGammaLabel = new Label{gammaRow, "", "sans", mSidebarOnRight ? 13 : 14};
            if (mSidebarOnRight) {
                mGammaLabel->set_color(Color(170, 174, 179, 215));
                mGammaLabel->set_fixed_width(104);
            }

            mGammaSlider = new Slider{gammaRow};
            mGammaSlider->set_range({0.01f, 5.0f});
            mGammaSlider->set_callback([this](float value) { setGamma(value); });
            setGamma(2.2f);

            panel->set_tooltip(
                "The offset is added to the image after exposure has been applied.\n"
                "Keyboard shortcuts: O and Shift+O\n\n"
                "Gamma is the exponent used when gamma-tonemapping.\n"
                "Keyboard shortcuts: G and Shift+G\n\n"
            );
        }
    }

    // Exposure/offset buttons
    {
        Widget* quickToolsHost = mSidebarLayout;
        if (mSidebarOnRight) {
            quickToolsHost = new Widget{mFloatingToolbarCard};
            quickToolsHost->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});
        }

        auto buttonContainer = new Widget{quickToolsHost};
        buttonContainer->set_layout(new GridLayout{Orientation::Horizontal, mSidebarOnRight ? 2 : 5, Alignment::Fill, mSidebarOnRight ? 4 : 5, 2});
        Widget* valuesRow = buttonContainer;
        if (mSidebarOnRight) {
            valuesRow = new Widget{quickToolsHost};
            valuesRow->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 4, 8});
            auto* valuesLabel = new Label{valuesRow, "Pixel values", "sans", 13};
            valuesLabel->set_color(Color(170, 174, 179, 215));
            valuesLabel->set_fixed_width(104);
            valuesLabel->set_tooltip("How hover values, the histogram and ROI statistics are shown.");
        }

        const auto makeButton = [&](string_view name, function<void()> callback, int icon = 0, string_view tooltip = "") {
            auto button = new Button{buttonContainer, name, icon};
            button->set_font_size(14);
            button->set_callback(callback);
            button->set_tooltip(tooltip);
            if (mSidebarOnRight) {
                styleViewerToolbarButton(button, icon, false);
            }
            return button;
        };

        mCurrentImageButtons.push_back(makeButton(
            "Normalize",
            [this]() { normalizeExposureAndOffset(); },
            0,
            "Normalize image such that the smallest pixel value is displayed as 0 and the largest as 1.\n\n"
            "Shortcut: N"
        ));
        makeButton("Reset", [this]() { resetImage(); }, 0, "Shortcut: R");
        mValueModePopupButton = new PopupButton{valuesRow, "Color"};
        if (mSidebarOnRight) {
            styleViewerToolbarButton(mValueModePopupButton, 0, true);
        } else {
            styleViewerSidebarButton(mValueModePopupButton, 0, false, true);
        }
        mValueModePopupButton->set_chevron_icon(0);
        mValueModePopupButton->set_tooltip("Choose how hover values, histogram, and ROI statistics are shown.");
        if (mSidebarOnRight) {
            mValueModePopupButton->set_side(Popup::Left);
            mValueModePopupButton->set_chevron_icon(FA_CHEVRON_DOWN);
            mValueModePopupButton->set_fixed_height(28);
            mValueModePopupButton->set_background_color(Color(255, 255, 255, 13));
        }
        {
            auto* popup = mValueModePopupButton->popup();
            popup->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 4, 0});
            mInspectionPresetButtons.clear();
            const auto makeModeButton = [&](string_view label, int presetIndex, string_view tooltip) {
                auto* button = new MenuItemButton{popup, label};
                button->set_flags(Button::RadioButton);
                button->set_tooltip(tooltip);
                button->set_callback([this, presetIndex]() { applyInspectionPreset(presetIndex); });
                mInspectionPresetButtons.push_back(button);
                return button;
            };

            makeModeButton("RGB", InspectionSource, "Source pixel values from the current file.");
            makeModeButton("Raw", InspectionRaw, "Sensor-space values using mergeHDR's XYZCAM metadata when available.");
            makeModeButton("Rad", InspectionRad, "Linear Radiance RGB values using mergeHDR target primaries.");
            makeModeButton("sRGB", InspectionSRgb, "Linear sRGB-primary values derived from source metadata.");
            makeModeButton("XYZ", InspectionXyz, "Linear XYZ values. Radiance files use 179x scaling here.");
            makeModeButton("Luminance", InspectionLuminance, "Single-channel Y from XYZ. Radiance files use 179x scaling here.");
            // Files without colour information: let the user say what the values are. The choice
            // is written into the file's header (two lines), so every tool reads it the same way.
            {
                mAssignColorSpacer = new Widget{popup};
                mAssignColorSpacer->set_fixed_height(9);
                auto* heading = new Label{popup, "INTERPRET FILE AS", "sans-bold", 10};
                heading->set_color(Color(154, 151, 143, 255));
                heading->set_fixed_height(18);
                mAssignColorHeading = heading;
                mAssignColorButtons.clear();
                const std::pair<const char*, const char*> assignItems[] = {
                    {"Radiance RGB", "Radiance primaries (equal-energy white). Writes PRIMARIES and HDRSPACE_COLOR into the header."},
                    {"sRGB / Rec.709", "Linear sRGB primaries, D65 white. Writes PRIMARIES and HDRSPACE_COLOR into the header."},
                    {"XYZ", "CIE 1931 XYZ in Radiance units (cd/m\u00b2 \u00f7 179), as mergehdr and Radiance write it. Writes PRIMARIES and HDRSPACE_COLOR into the header."},
                    {"Raw (camera)", "Camera sensor values without a colour matrix; conversions stay off. Writes HDRSPACE_COLOR."},
                    {"Luminance (cd/m\u00b2)", "Monochrome luminance in cd/m\u00b2 (all channels equal). Writes HDRSPACE_COLOR."},
                    {"XYZ (cd/m\u00b2)", "CIE 1931 XYZ already in cd/m\u00b2 (not Radiance units). Writes PRIMARIES and HDRSPACE_COLOR into the header."},
                };
                mAssignColorButtonKinds.clear();
                for (const int kind : {0, 1, 2, 5, 3, 4}) {
                    auto* button = new MenuItemButton{popup, assignItems[kind].first};
                    button->set_tooltip(assignItems[kind].second);
                    button->set_callback([this, kind]() { requestSourceColorAssignment(kind); });
                    mAssignColorButtons.push_back(button);
                    mAssignColorButtonKinds.push_back(kind);
                }
            }
            refreshInspectionModeUi();
        }

        static constexpr auto addSpacer = [](Widget* current, int space) {
            auto row = new Widget{current};
            row->set_height(space);
        };

        const auto buildHdrControls = [&](Widget* popup) {
            new Label{popup, "Display & HDR", "sans-bold", 14};
            addSpacer(popup, 10);

            mClipToLdrButton = new Button{popup, "Clip to LDR", 0};
            mClipToLdrButton->set_font_size(14);
            mClipToLdrButton->set_change_callback([this](bool value) { mImageCanvas->setClipToLdr(value); });
            mClipToLdrButton->set_tooltip(
                "Clips the image to [0,1] as if displayed on a low dynamic range (LDR) screen.\n\n"
                "Shortcut: U"
            );
            mClipToLdrButton->set_flags(Button::ToggleButton);

            addSpacer(popup, 10);

            new Label{popup, "Display white level", "sans", 14};

            addSpacer(popup, 5);

            auto whiteLevelContainer = new Widget{popup};
            whiteLevelContainer->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 0, 2});

            mDisplayWhiteLevelBox = new FloatBox<float>{whiteLevelContainer};
            mDisplayWhiteLevelBox->set_alignment(TextBox::Alignment::Right);
            mDisplayWhiteLevelBox->set_min_max_values(0.0f, 10000.0f);
            mDisplayWhiteLevelBox->set_fixed_width(90);
            mDisplayWhiteLevelBox->set_value(glfwGetWindowSdrWhiteLevel(m_glfw_window));
            mDisplayWhiteLevelBox->set_default_value(to_string(DEFAULT_IMAGE_WHITE_LEVEL));
            mDisplayWhiteLevelBox->set_units("nits");

            mDisplayWhiteLevelSettingComboBox = new ComboBox{
                whiteLevelContainer, {"System", "Custom", "Image"}
            };
            mDisplayWhiteLevelSettingComboBox->set_font_size(14);
            mDisplayWhiteLevelSettingComboBox->set_fixed_width(80);
            if (mSidebarOnRight) {
                mDisplayWhiteLevelSettingComboBox->set_side(Popup::Left);
            }
            mDisplayWhiteLevelSettingComboBox->set_callback([this](int value) {
                setDisplayWhiteLevelSetting(static_cast<EDisplayWhiteLevelSetting>(value));
            });

            mDisplayWhiteLevelBox->set_callback([this](float value) {
                setDisplayWhiteLevelSetting(EDisplayWhiteLevelSetting::Custom);
                setDisplayWhiteLevel(value);
            });

            addSpacer(popup, 10);

            new Label{popup, "Best guess image white level", "sans", 14};

            addSpacer(popup, 5);

            mImageWhiteLevelBox = new FloatBox<float>{popup};
            mImageWhiteLevelBox->set_alignment(TextBox::Alignment::Right);
            mImageWhiteLevelBox->set_min_max_values(0.0, 10000.0f);
            mImageWhiteLevelBox->set_fixed_width(90);
            mImageWhiteLevelBox->set_value(DEFAULT_IMAGE_WHITE_LEVEL);
            mImageWhiteLevelBox->set_default_value(to_string(DEFAULT_IMAGE_WHITE_LEVEL));
            mImageWhiteLevelBox->set_units("nits");
            mImageWhiteLevelBox->set_tooltip(
                "tev's best guess of the image's reference white level (aka. paper white) in nits (cd/m²). "
                "This value represents the brightness a pixel value of 1.0 is meant to represent.\n\n"

                "tev usually has to guess this value for multiple reasons. "
                "Many image formats are display-referred and, as such, have no white level in (absolute) nits. "
                "Other formats are scene-referred and thus do have an absolute white level, but this information is often not stored in the file. "
                "Sometimes, it is not even clear whether a given image format is display- or scene-referred.\n\n"

                "However, when an image has unambiguous metadata, e.g. uses the PQ transfer function, "
                "tev can determine the white level reliably."
            );

            mImageWhiteLevelBox->set_editable(false);
            mImageWhiteLevelBox->set_enabled(false);
        };

        if (!mSidebarOnRight) {
            mHdrPopupButton = new PopupButton{buttonContainer, "HDR", 0};
            styleViewerSidebarButton(mHdrPopupButton, FA_INFO, false, true);
            mHdrPopupButton->set_chevron_icon(0);
        } else {
            mHdrPopupButton = nullptr;
        }

        {
            if (!mSidebarOnRight) {
                auto popup = mHdrPopupButton->popup();
                popup->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 10});
                buildHdrControls(popup);
            }
        }

        if (mSidebarOnRight) {
            mColorsPopupButton = nullptr;
            mFalseColorPopupButton = new PopupButton{buttonContainer, "False"};
            styleViewerToolbarButton(mFalseColorPopupButton, 0, true);
            mFalseColorPopupButton->set_chevron_icon(0);
            mFalseColorPopupButton->set_tooltip("False-color controls");
            mFalseColorPopupButton->set_side(Popup::Left);
            mFalseColorPopupButton->popup()->set_size({320, 280});
            mFalseColorPopupButton->set_change_callback([this](bool value) {
                if (value) {
                    setTonemap(ETonemap::FalseColor);
                }
            });

            auto* popup = mFalseColorPopupButton->popup();
            popup->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 10});
            new Label{popup, "False color", "sans-bold", 14};

            auto* mappingRow = new Widget{popup};
            mappingRow->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 5, 4});
            auto* mapLabel = new Label{mappingRow, "Map", "sans", 14};
            mapLabel->set_fixed_width(60);
            mFalseColorColormapComboBox = new ComboBox{mappingRow, {"Turbo", "Viridis"}};
            mFalseColorColormapComboBox->set_font_size(14);
            mFalseColorColormapComboBox->set_side(Popup::Left);
            mFalseColorColormapComboBox->set_selected_index(0);
            mFalseColorColormapComboBox->set_callback([this](int) { applyFalseColorSettingsFromUi(); });

            auto* scaleLabel = new Label{mappingRow, "Scale", "sans", 14};
            scaleLabel->set_fixed_width(60);
            mFalseColorScaleComboBox = new ComboBox{mappingRow, {"Linear", "Log2", "Log3", "Log4", "Log5"}};
            mFalseColorScaleComboBox->set_font_size(14);
            mFalseColorScaleComboBox->set_side(Popup::Left);
            mFalseColorScaleComboBox->set_selected_index(1);
            mFalseColorScaleComboBox->set_callback([this](int) { applyFalseColorSettingsFromUi(); });

            new Label{popup, "Range", "sans-bold", 14};
            auto* rangeRow = new Widget{popup};
            rangeRow->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 6, 0});
            mFalseColorMinBox = new FloatBox<float>{rangeRow};
            mFalseColorMinBox->set_editable(true);
            mFalseColorMinBox->set_enabled(true);
            mFalseColorMinBox->set_font_size(14);
            mFalseColorMinBox->set_alignment(TextBox::Alignment::Right);
            mFalseColorMinBox->number_format("%.5g");
            mFalseColorMinBox->set_value(0.03125f);
            mFalseColorMinBox->set_default_value("0.03125");
            mFalseColorMinBox->set_tooltip("Minimum value for the false-color ramp.");
            mFalseColorMaxBox = new FloatBox<float>{rangeRow};
            mFalseColorMaxBox->set_editable(true);
            mFalseColorMaxBox->set_enabled(true);
            mFalseColorMaxBox->set_font_size(14);
            mFalseColorMaxBox->set_alignment(TextBox::Alignment::Right);
            mFalseColorMaxBox->number_format("%.5g");
            mFalseColorMaxBox->set_value(32.0f);
            mFalseColorMaxBox->set_default_value("32");
            mFalseColorMaxBox->set_tooltip("Maximum value for the false-color ramp.");

            const auto applyRange = [this](float) {
                applyFalseColorSettingsFromUi();
                return true;
            };
            mFalseColorMinBox->set_callback(applyRange);
            mFalseColorMaxBox->set_callback(applyRange);

            auto* hint = new Label{popup, "Manual min/max with linear or log ramps.", "sans", 12};
            hint->set_color(Color{0.7f, 1.0f});
        } else {
            mFalseColorPopupButton = nullptr;
            mColorsPopupButton = new PopupButton{buttonContainer, "Colors"};
            styleViewerSidebarButton(mColorsPopupButton, FA_MAGIC, false, true);
            mColorsPopupButton->set_chevron_icon(0);
            mColorsPopupButton->set_tooltip("Color settings");
            auto popup = mColorsPopupButton->popup();
            popup->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 10});

            auto label = new Label{popup, "Inspection color space", "sans-bold", 14};
            label->set_tooltip(
                "The color space used for pixel inspection, i.e. the pixel values shown on hover, when zooming in, and in the histogram.\n\n"
                "IMPORTANT: this setting does NOT affect the appearance of the image shown on screen. "
                "The image is always displayed in correct colors; tev negotiates the correct display color space with the operating system automatically."
            );

            addSpacer(popup, 10);

            auto xy = new Widget{popup};
            xy->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 0, 2});

            label = new Label{xy, "Transfer", "sans", 14};
            label->set_fixed_width(100);
            label = new Label{xy, "Primaries", "sans", 14};
            label->set_fixed_width(100);

            vector<string> transferNames;
            for (const auto& t : TRANSFERS) {
                transferNames.emplace_back(t.second);
            }

            mInspectionTransferComboBox = new ComboBox{xy, transferNames};
            mInspectionTransferComboBox->set_font_size(14);
            mInspectionTransferComboBox->set_callback([this](int value) {
                TEV_ASSERT(value >= 0 && (size_t)value < TRANSFERS.size(), "Invalid transfer function index");
                setInspectionTransfer(ituth273::ETransfer(TRANSFERS.at(value).first));
            });

            vector<string> primariesNames;
            for (const auto& p : PRIMARIES) {
                primariesNames.emplace_back(p.second);
            }

            primariesNames.emplace_back("Custom");

            mInspectionPrimariesComboBox = new ComboBox{xy, primariesNames};
            mInspectionPrimariesComboBox->set_font_size(14);

            const auto makeChromaBox = [this](Widget* parent, size_t idx) {
                auto box = new FloatBox<float>{parent};
                box->set_editable(true);
                box->set_enabled(true);
                box->set_value_increment(0.0001f);
                box->number_format("%.05f");

                box->set_callback([this, idx](float val) {
                    TEV_ASSERT(idx < 8, "Invalid chromaticity index");

                    chroma_t chr = inspectionChroma();
                    chr[idx / 2][idx % 2] = val;
                    setInspectionChroma(chr);
                });

                return box;
            };

            addSpacer(xy, 6);
            addSpacer(xy, 6);

            const array<string_view, 4> labels = {"Red", "Green", "Blue", "White"};
            for (size_t i = 0; i < labels.size(); ++i) {
                new Label{xy, format("{} X", labels[i]), "sans", 14};
                new Label{xy, format("{} Y", labels[i]), "sans", 14};
                mInspectionPrimariesBoxes.emplace_back(makeChromaBox(xy, i * 2 + 0));
                mInspectionPrimariesBoxes.emplace_back(makeChromaBox(xy, i * 2 + 1));
                addSpacer(xy, 1);
                addSpacer(xy, 1);
            }

            mInspectionPrimariesComboBox->set_callback([this](int value) {
                TEV_ASSERT(value >= 0 && (size_t)value < PRIMARIES.size() + 1, "Invalid primaries index");

                if ((size_t)value >= PRIMARIES.size()) {
                    return;
                }

                setInspectionChroma(chroma(PRIMARIES.at(value).first));
            });

            addSpacer(xy, 1);
            addSpacer(xy, 1);

            mInspectionAdaptWhitePointButton = new Button{xy, "Adapt white"};
            mInspectionAdaptWhitePointButton->set_font_size(14);
            mInspectionAdaptWhitePointButton->set_flags(Button::ToggleButton);
            mInspectionAdaptWhitePointButton->set_tooltip(
                "Adapt from tev's internal D65 illuminant to the white point of the inspection color space using Bradford's algorithm. "
                "Enabling this feature is equivalent to a \"relative colorimetric\" color space conversion. Disabled is \"absolute colorimetric\"."
            );
            mInspectionAdaptWhitePointButton->set_change_callback([this](bool value) { setInspectionAdaptWhitePoint(value); });

            mInspectionPremultipliedAlphaButton = new Button{xy, "Premult. alpha"};
            mInspectionPremultipliedAlphaButton->set_font_size(14);
            mInspectionPremultipliedAlphaButton->set_flags(Button::ToggleButton);
            mInspectionPremultipliedAlphaButton->set_tooltip("Whether the inspected pixel values should have alpha premultiplied or not.");
            mInspectionPremultipliedAlphaButton->set_change_callback([this](bool value) { setInspectionPremultipliedAlpha(value); });

            setInspectionChroma(mImageCanvas->inspectionChroma());
            setInspectionTransfer(mImageCanvas->inspectionTransfer());
            setInspectionAdaptWhitePoint(mImageCanvas->inspectionAdaptWhitePoint());
            setInspectionPremultipliedAlpha(mImageCanvas->inspectionPremultipliedAlpha());

            addSpacer(popup, 20);

            applyInspectionPreset(0);

            new Label{popup, "Background color", "sans-bold", 14};
            mBackgroundColorWheel = new ColorWheel{popup};
            mBackgroundColorWheel->set_callback([this](Color value) {
                const float a = mBackgroundAlphaSlider->value();
                mImageCanvas->setBackgroundColor(Color{value.r() * a, value.g() * a, value.b() * a, a});
            });

            new Label{popup, "Background alpha", "sans", 14};
            mBackgroundAlphaSlider = new Slider{popup};
            mBackgroundAlphaSlider->set_range({0.0f, 1.0f});
            mBackgroundAlphaSlider->set_callback([this](float a) {
                const auto col = mBackgroundColorWheel->color();
                mImageCanvas->setBackgroundColor(Color{col.r() * a, col.g() * a, col.b() * a, a});
            });

            setBackgroundColorStraight(Color{0, 0, 0, 0});
        }
    }

    // Tonemap options
    {
        Widget* displayModeHost = mSidebarLayout;
        if (mSidebarOnRight) {
            auto* divider = new HairlineDivider{mFloatingToolbarCard};
            divider->set_fixed_height(1);

            displayModeHost = new Widget{mFloatingToolbarCard};
            displayModeHost->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});
        }

        mTonemapButtonContainer = new Widget{displayModeHost};
        mTonemapButtonContainer->set_layout(new GridLayout{Orientation::Horizontal, mSidebarOnRight ? 4 : 4, Alignment::Fill, mSidebarOnRight ? 4 : 5, 2});

        const auto makeTonemapButton = [&](string_view name, ETonemap tonemapValue, function<void()> callback) {
            auto button = new Button{mTonemapButtonContainer, name};
            button->set_flags(Button::RadioButton);
            if (mSidebarOnRight) {
                styleViewerToolbarButton(button, tonemapIcon(tonemapValue), false);
            } else {
                styleViewerSidebarButton(button, tonemapIcon(tonemapValue), true);
            }
            button->set_callback(callback);
            mTonemapButtons[(size_t)tonemapValue] = button;
            return button;
        };

        makeTonemapButton(mSidebarOnRight ? "sRGB" : "sRGB Display", ETonemap::SRGB, [this]() { setTonemap(ETonemap::SRGB); });
        makeTonemapButton("Gamma", ETonemap::Gamma, [this]() { setTonemap(ETonemap::Gamma); });
        auto* reinhardButton = makeTonemapButton("Reinhard", ETonemap::Reinhard, [this]() { setTonemap(ETonemap::Reinhard); });
        if (mSidebarOnRight) {
            // hdrspace: tone mapping operators live in the Tonemapping tool, so the docked
            // Viewer offers only the display curves (sRGB, Gamma) and false colour.
            reinhardButton->set_visible(false);
        }

        if (mSidebarOnRight) {
            mFalseColorBarPopupButton = new PopupButton{mTonemapButtonContainer, "Legend"};
            styleViewerToolbarButton(mFalseColorBarPopupButton, 0, true);
            mFalseColorBarPopupButton->set_chevron_icon(0);
            mFalseColorBarPopupButton->set_side(Popup::Left);
            mFalseColorBarPopupButton->set_tooltip("False-color legend and tick controls");
            mFalseColorBarPopupButton->set_change_callback([this](bool value) {
                if (value) {
                    setTonemap(ETonemap::FalseColor);
                }
            });
            auto* popup = mFalseColorBarPopupButton->popup();
            popup->set_size({344, 264});
            popup->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 10});

            new Label{popup, "Falsecolor bar", "sans-bold", 14};
            mFalseColorBarWidget = new FalseColorBarWidget{popup};

            auto* ticksRow = new Widget{popup};
            ticksRow->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 5, 4});
            auto* ticksLabel = new Label{ticksRow, "Ticks", "sans", 14};
            ticksLabel->set_fixed_width(60);
            mFalseColorBarTicksComboBox = new ComboBox{ticksRow, {"3", "5", "10", "Manual"}};
            mFalseColorBarTicksComboBox->set_font_size(14);
            mFalseColorBarTicksComboBox->set_side(Popup::Left);
            mFalseColorBarTicksComboBox->set_selected_index(1);

            auto* manualLabel = new Label{ticksRow, "Values", "sans", 14};
            manualLabel->set_fixed_width(60);
            mFalseColorBarManualTicksBox = new TextBox{ticksRow, ""};
            mFalseColorBarManualTicksBox->set_font_size(14);
            mFalseColorBarManualTicksBox->set_editable(true);
            mFalseColorBarManualTicksBox->set_alignment(TextBox::Alignment::Left);
            mFalseColorBarManualTicksBox->set_placeholder("100, 250, 500");
            mFalseColorBarManualTicksBox->set_enabled(false);

            mFalseColorBarTicksComboBox->set_callback([this](int index) {
                if (mFalseColorBarManualTicksBox) {
                    mFalseColorBarManualTicksBox->set_enabled(index == 3);
                }
                refreshFalseColorBarUi();
            });
            mFalseColorBarManualTicksBox->set_callback([this](string_view) {
                refreshFalseColorBarUi();
                return true;
            });

            auto* copyButton = new Button{popup, "Copy"};
            styleViewerSidebarButton(copyButton, FA_COPY, true);
            copyButton->set_callback([this]() {
                try {
                    copyFalseColorBarToClipboard();
                } catch (const runtime_error& e) { showErrorDialog(format("Failed to copy falsecolor bar: {}", e.what())); }
            });

            auto* bottomInset = new Widget{mFloatingToolbarCard};
            bottomInset->set_fixed_height(6);

            // hdrspace inspector order: "Display" heading, then the display curves with
            // false colour beside them, then Normalize / Reset and the pixel-value choice.
            const auto moveChild = [](Widget* child, Widget* newParent, int index) {
                if (!child || !newParent || !child->parent()) {
                    return;
                }
                child->inc_ref();
                child->parent()->remove_child(child);
                newParent->add_child(std::min(index, newParent->child_count()), child);
                child->dec_ref();
            };
            moveChild(mFalseColorPopupButton, mTonemapButtonContainer, 3);
            if (mFalseColorPopupButton) {
                mFalseColorPopupButton->set_caption("False");
            }
            moveChild(displayModeHost, mFloatingToolbarCard, 1);
            auto* displayHeader = new SidebarSectionHeader{mFloatingToolbarCard, "Display"};
            moveChild(displayHeader, mFloatingToolbarCard, 1);
        }
        else {
            mFalseColorBarPopupButton = nullptr;
            makeTonemapButton("FC", ETonemap::FalseColor, [this]() { setTonemap(ETonemap::FalseColor); });
            makeTonemapButton("+/-", ETonemap::PositiveNegative, [this]() { setTonemap(ETonemap::PositiveNegative); });
        }

        setTonemap(ETonemap::SRGB);
        refreshFalseColorBarUi();

        mTonemapButtonContainer->set_tooltip(
            "Tonemap selection:\n\n"

            "None\n"
            "No tonemapping\n\n"

            "Gamma\n"
            "Gamma correction + inverse sRGB\n"
            "Needed when displaying SDR to\n"
            "gamma-encoded displays.\n\n"

            "FC\n"
            "False-color visualization\n\n"

            "+/-\n"
            "Positive=Green, Negative=Red\n\n"
            "Reinhard\n"
            "Luminance-preserving HDR compression"
        );
    }

    // Error metrics
    {
        mMetricButtonContainer = new Widget{mSidebarLayout};
        mMetricButtonContainer->set_layout(new GridLayout{Orientation::Horizontal, 5, Alignment::Fill, 5, 2});

        const auto makeMetricButton = [&](string_view name, function<void()> callback) {
            auto button = new Button{mMetricButtonContainer, name};
            button->set_flags(Button::RadioButton);
            styleViewerSidebarButton(button, 0, true);
            button->set_callback(callback);
            return button;
        };

        makeMetricButton("E", [this]() { setMetric(EMetric::Error); });
        makeMetricButton("AE", [this]() { setMetric(EMetric::AbsoluteError); });
        makeMetricButton("SE", [this]() { setMetric(EMetric::SquaredError); });
        makeMetricButton("RAE", [this]() { setMetric(EMetric::RelativeAbsoluteError); });
        makeMetricButton("RSE", [this]() { setMetric(EMetric::RelativeSquaredError); });

        setMetric(EMetric::AbsoluteError);

        mMetricButtonContainer->set_tooltip(
            "Error metric selection. Given a reference image r and the selected image i, "
            "the following operators are available:\n\n"

            "E (Error)\n"
            "i - r\n\n"

            "AE (Absolute Error)\n"
            "|i - r|\n\n"

            "SE (Squared Error)\n"
            "(i - r)²\n\n"

            "RAE (Relative Absolute Error)\n"
            "|i - r| / (r + 0.01)\n\n"

            "RSE (Relative Squared Error)\n"
            "(i - r)² / (r² + 0.01)"
        );

        if (mSidebarOnRight) {
            mMetricButtonContainer->set_visible(false);
        }
    }

    // Image channel mask
    {
        if (mSidebarOnRight) {
            new SidebarSectionHeader{mSidebarLayout, "Channels"};
        }
        mChannelMaskButtonContainer = new Widget{mSidebarLayout};
        mChannelMaskButtonContainer->set_layout(new GridLayout{Orientation::Horizontal, 5, Alignment::Fill, 5, 2});

        const auto makeChannelMaskBtn = [&](string_view name, string_view humanReadable, int c) {
            auto button = new Button{mChannelMaskButtonContainer, name};
            button->set_flags(Button::ToggleButton);
            button->set_pushed(false);
            button->set_font_size(14);
            button->set_text_color(Channel::color(name, true));
            button->set_change_callback(
                [this, button, mask = static_cast<EChannelMask>(1 << c), activeColor = Channel::color(name, true)](bool state) {
                    setChannelMask(mask, !state);
                    button->set_text_color(state ? Color{0.6f} : activeColor);
                }
            );
            button->set_tooltip(format(
                "Disable the {} channel.{}",
                humanReadable,
                c < 3 ? "\n\nIn terms of rec.709 primaries, regardless of the image's original color space." : ""
            ));
            return button;
        };

        makeChannelMaskBtn("R", "red", 0);
        makeChannelMaskBtn("G", "green", 1);
        makeChannelMaskBtn("B", "blue", 2);
        makeChannelMaskBtn("A", "alpha", 3);

        auto button = new Button{mChannelMaskButtonContainer, "Ungroup"};
        button->set_font_size(14);
        button->set_callback([this]() { ungroupCurrentChannelGroup(); });
        button->set_tooltip("Ungroup current channel group into individual channels.\n\nKeyboard shortcut: Ctrl+U");
    }

    // Crop controls
    {
        auto spacer = new Widget{mSidebarLayout};
        spacer->set_height(6);

        if (mSidebarOnRight) {
            auto* panel = new Widget{mSidebarLayout};
            mCropControlsPanel = panel;
            panel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

            auto* cropLabel = new SidebarSectionHeader{panel, "Crop & projection"};
            cropLabel->set_tooltip("Set crop here, or drag a crop in the viewer while holding C.");

            auto* fieldRow = new Widget{panel};
            fieldRow->set_layout(new GridLayout{Orientation::Horizontal, 4, Alignment::Fill, 4, 0});

            auto makeCropBox = [&](string_view shortLabel, string_view tooltip, nanogui::IntBox<int>*& box) {
                auto* field = new Widget{fieldRow};
                field->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 2});
                auto* label = new Label{field, shortLabel, "sans", 12};
                label->set_color(Color{170, 174, 179, 255});
                box = new IntBox<int>{field};
                box->set_tooltip(tooltip);
                box->set_editable(true);
                box->set_alignment(TextBox::Alignment::Right);
                box->set_default_value("0");
                box->set_min_max_values(0, 1000000);
                box->set_spinnable(false);
                box->set_font_size(14);
            };

            makeCropBox("L", "Left edge in image pixels", mCropLeftBox);
            makeCropBox("T", "Top edge in image pixels", mCropTopBox);
            makeCropBox("W", "Crop width in image pixels", mCropWidthBox);
            makeCropBox("H", "Crop height in image pixels", mCropHeightBox);

            auto* buttonRow = new Widget{panel};
            buttonRow->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 6, 0});
            auto* apply = new Button{buttonRow, "Crop"};
            apply->set_font_size(14);
            apply->set_callback([this]() {
                applyCropFromUi();
                maybeOfferCropSave();
            });
            auto* clear = new Button{buttonRow, "Clear"};
            clear->set_font_size(14);
            clear->set_callback([this]() {
                mImageCanvas->setCrop(nullopt);
                syncCropUiFromCanvas();
            });
        } else {
            Widget* row = nullptr;
            row = new Widget{mSidebarLayout};
            mCropControlsPanel = row;
            row->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 3, 0});
            auto* cropLabel = new Label{row, "Crop", "sans-bold", 16};
            cropLabel->set_fixed_width(34);
            cropLabel->set_tooltip("Set crop here, or drag a crop in the viewer while holding C.");

            auto makeCropBox = [&](string_view shortLabel, nanogui::IntBox<int>*& box) {
                auto* l = new Label{row, shortLabel, "sans", 12};
                l->set_fixed_width(8);
                box = new IntBox<int>{row};
                box->set_editable(true);
                box->set_alignment(TextBox::Alignment::Right);
                box->set_default_value("0");
                box->set_min_max_values(0, 1000000);
                box->set_spinnable(false);
                box->set_fixed_width(40);
                box->set_font_size(12);
            };

            makeCropBox("L", mCropLeftBox);
            makeCropBox("T", mCropTopBox);
            makeCropBox("W", mCropWidthBox);
            makeCropBox("H", mCropHeightBox);

            auto* apply = new Button{row, "Apply"};
            apply->set_font_size(12);
            apply->set_fixed_width(42);
            apply->set_callback([this]() {
                applyCropFromUi();
                maybeOfferCropSave();
            });
            auto* clear = new Button{row, "Clear"};
            clear->set_font_size(12);
            clear->set_fixed_width(40);
            clear->set_callback([this]() {
                mImageCanvas->setCrop(nullopt);
                syncCropUiFromCanvas();
            });
        }
    }

    // Projection controls
    {
        auto spacer = new Widget{mSidebarLayout};
        spacer->set_height(6);

        if (mSidebarOnRight) {
            auto* panel = new Widget{mSidebarLayout};
            panel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

            auto* projectionLabel = new Label{panel, "Projection", "sans", 13};
            projectionLabel->set_color(Color(170, 174, 179, 215));
            projectionLabel->set_tooltip("Detected from HDR metadata. Convert and save with mergehdr when supported.");

            auto* row = new Widget{panel};
            row->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 6, 0});

            mProjectionComboBox = new ComboBox{row, {"Unknown", "Equisolid", "Equidistant"}};
            mProjectionComboBox->set_font_size(14);
            mProjectionComboBox->set_selected_index(0);
            mProjectionComboBox->set_side(Popup::Left);
            mProjectionComboBox->set_chevron_icon(FA_CHEVRON_DOWN);
            mProjectionComboBox->set_callback([this](int index) {
                mProjectionSelection = index;
                refreshProjectionUi(false);
            });

            mProjectionApplyButton = new Button{row, "Convert"};
            mProjectionApplyButton->set_font_size(14);
            mProjectionApplyButton->set_callback([this]() { maybeOfferProjectionSave(); });
        }
    }

    // Image selection
    {
        auto spacer = new Widget{mSidebarLayout};
        spacer->set_height(10);

        {
            auto panel = new Widget{mSidebarLayout};
            panel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
            Widget* label = mSidebarOnRight ? (Widget*)new SidebarSectionHeader{panel, "Histogram"} : (Widget*)new Label{panel, "Images", "sans-bold", 25};
            label->set_tooltip(
                "Select images either by left-clicking on them or by pressing arrow/number keys on your keyboard.\n"
                "Right-clicking an image marks it as the 'reference' image. "
                "While a reference image is set, the currently selected image is not simply displayed, but compared to the reference image."
            );
        }

        // Histogram of selected image
        {
            auto controls = new Widget{mSidebarLayout};
            controls->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
            auto* label = new Label{controls, "Highlight range", "sans", mSidebarOnRight ? 13 : 14};
            if (mSidebarOnRight) label->set_color(Color(170, 174, 179, 215));
            label->set_tooltip("Temporarily overlay pixels in red whose selected channel value falls inside the selected range.");

            auto* modeRow = new Widget{controls};
            modeRow->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 5, 0});
            auto* modeLabel = new Label{modeRow, "Channel", "sans", mSidebarOnRight ? 13 : 14};
            if (mSidebarOnRight) modeLabel->set_color(Color(170, 174, 179, 215));
            modeLabel->set_tooltip("Choose which value the highlight range should test.");
            mHighlightModeComboBox = new ComboBox{modeRow, {"RGB", "R", "G", "B"}};
            mHighlightModeComboBox->set_font_size(14);
            mHighlightModeComboBox->set_selected_index(0);
            mHighlightModeSelection = 0;
            if (mSidebarOnRight) {
                mHighlightModeComboBox->set_side(Popup::Left);
                mHighlightModeComboBox->set_chevron_icon(FA_CHEVRON_DOWN);
            }
            mHighlightModeComboBox->set_callback([this](int index) {
                mHighlightModeSelection = index;
                mImageCanvas->setHighlightMode(selectedHighlightMode());
                applyHighlightRangeFromUi();
            });

            auto* row = new Widget{controls};
            row->set_layout(new GridLayout{Orientation::Horizontal, 4, Alignment::Fill, 5, 0});
            mHighlightMinBox = new FloatBox<float>{row};
            mHighlightMinBox->set_font_size(14);
            mHighlightMinBox->set_editable(true);
            mHighlightMinBox->set_enabled(true);
            mHighlightMinBox->set_alignment(TextBox::Alignment::Right);
            mHighlightMinBox->set_default_value("0");
            mHighlightMinBox->set_value(0.0f);
            mHighlightMinBox->number_format("%.5g");
            mHighlightMinBox->set_tooltip("Minimum value");
            mHighlightMaxBox = new FloatBox<float>{row};
            mHighlightMaxBox->set_font_size(14);
            mHighlightMaxBox->set_editable(true);
            mHighlightMaxBox->set_enabled(true);
            mHighlightMaxBox->set_alignment(TextBox::Alignment::Right);
            mHighlightMaxBox->set_default_value("1");
            mHighlightMaxBox->set_value(1.0f);
            mHighlightMaxBox->number_format("%.5g");
            mHighlightMaxBox->set_tooltip("Maximum value");
            mHighlightEnableButton = new Button{row, "Show"};
            mHighlightEnableButton->set_font_size(14);
            mHighlightEnableButton->set_flags(Button::ToggleButton);
            mHighlightEnableButton->set_change_callback([this](bool) { applyHighlightRangeFromUi(); });
            auto* clear = new Button{row, "Clear"};
            clear->set_font_size(14);
            clear->set_callback([this]() {
                if (mHighlightEnableButton) {
                    mHighlightEnableButton->set_pushed(false);
                }
                if (mHighlightMinBox) {
                    mHighlightMinBox->set_value(0.0f);
                }
                if (mHighlightMaxBox) {
                    mHighlightMaxBox->set_value(1.0f);
                }
                mImageCanvas->setHighlightValueRange(std::nullopt);
                redraw();
            });

            mHighlightMinBox->set_callback([this](float) {
                applyHighlightRangeFromUi();
                return true;
            });
            mHighlightMaxBox->set_callback([this](float) {
                applyHighlightRangeFromUi();
                return true;
            });

            auto panel = new Widget{mSidebarLayout};
            panel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

            mHistogram = new MultiGraph{panel, ""};

            if (mSidebarOnRight) {
                new SidebarSectionHeader{panel, "Region (ROI)"};
            }
            auto* roiHeader = new Widget{panel};
            roiHeader->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 5, 0});
            auto* roiLabel = mSidebarOnRight ? new Label{roiHeader, "Right-drag on image", "sans", 12}
                                             : new Label{roiHeader, "ROI", "sans-bold", 14};
            if (mSidebarOnRight) roiLabel->set_color(Color(170, 174, 179, 200));
            roiLabel->set_tooltip("Right-drag in the viewer to define a histogram/statistics ROI.");
            mRoiClearButton = new Button{roiHeader, "Clear"};
            mRoiClearButton->set_font_size(14);
            mRoiClearButton->set_callback([this]() {
                mImageCanvas->setRoi(std::nullopt);
                redraw();
            });

            mRoiStatsLabel = new Label{panel, "Right-drag in the viewer to inspect a region.", "sans", 12};
        }

        // Fuzzy filter of open images
        if (!mSidebarOnRight) {
            auto panel = new Widget{mSidebarLayout};
            panel->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 5, 2});

            mFilter = new TextBox{panel, ""};
            mFilter->set_editable(true);
            mFilter->set_alignment(TextBox::Alignment::Left);
            mFilter->set_callback([this](string_view filter) { return setFilter(filter); });

            mFilter->set_placeholder("Find");
            mFilter->set_tooltip(format(
                "Filters visible images and channel groups according to a supplied string. "
                "The string must have the format 'image:group'. "
                "Only images whose name contains 'image' and groups whose name contains 'group' will be visible.\n\n"
                "Keyboard shortcut: {}+F",
                HelpWindow::COMMAND
            ));

            mRegexButton = new Button{panel, "", FA_SEARCH};
            mRegexButton->set_font_size(14);
            mRegexButton->set_tooltip("Treat filter as regular expression");
            mRegexButton->set_pushed(false);
            mRegexButton->set_flags(Button::ToggleButton);
            mRegexButton->set_change_callback([this](bool value) { setUseRegex(value); });
        } else {
            mFilter = nullptr;
            mRegexButton = nullptr;
        }

        // Playback controls
        if (!mSidebarOnRight) {
            auto playback = new Widget{mSidebarLayout};
            playback->set_layout(new GridLayout{Orientation::Horizontal, 6, Alignment::Fill, 5, 2});

            const auto makePlaybackButton =
                [&](string_view name, bool enabled, function<void()> callback, int icon = 0, string_view tooltip = "") {
                    auto button = new Button{playback, name, icon};
                    button->set_callback(callback);
                    button->set_tooltip(tooltip);
                    button->set_font_size(14);
                    button->set_enabled(enabled);
                    button->set_padding({10, 10});
                    return button;
                };

            mPlayButton = makePlaybackButton("", true, [] {}, FA_PLAY, "Play (Space)");
            mPlayButton->set_flags(Button::ToggleButton);
            mPlayButton->set_change_callback([this](bool value) { setPlayingBack(value); });

            mAnyImageButtons.push_back(
                makePlaybackButton("", false, [this] { selectImage(nthVisibleImage(0)); }, FA_FAST_BACKWARD, "Front (Home)")
            );

            mAnyImageButtons.push_back(
                makePlaybackButton("", false, [this] { selectImage(nthVisibleImage(mImages.size())); }, FA_FAST_FORWARD, "Back (End)")
            );

            mFpsTextBox = new IntBox<int>{playback, 24};
            mFpsTextBox->set_default_value("24");
            mFpsTextBox->set_units("fps");
            mFpsTextBox->set_editable(true);
            mFpsTextBox->set_alignment(TextBox::Alignment::Right);
            mFpsTextBox->set_min_max_values(1, 1000);
            mFpsTextBox->set_spinnable(true);

            mAutoFitToScreenButton =
                makePlaybackButton("", true, {}, FA_EXPAND_ARROWS_ALT, "Automatically fit image to screen upon selection.");
            mAutoFitToScreenButton->set_flags(Button::Flags::ToggleButton);
            mAutoFitToScreenButton->set_change_callback([this](bool value) { setAutoFitToScreen(value); });

            mResizeWindowToFitImageOnLoadButton =
                makePlaybackButton("", true, {}, FA_WINDOW_RESTORE, "Automatically resize tev's window to fit image on load.");
            mResizeWindowToFitImageOnLoadButton->set_flags(Button::Flags::ToggleButton);
            mResizeWindowToFitImageOnLoadButton->set_change_callback([this](bool value) { setResizeWindowToFitImageOnLoad(value); });
            mResizeWindowToFitImageOnLoadButton->set_pushed(true);
        }

        // Save, refresh, load, close
        if (!mSidebarOnRight) {
            auto tools = new Widget{mSidebarLayout};
            tools->set_layout(new GridLayout{Orientation::Horizontal, 7, Alignment::Fill, 5, 1});

            const auto makeImageButton =
                [&](string_view name, bool enabled, function<void()> callback, int icon = 0, string_view tooltip = "") {
                    auto button = new Button{tools, name, icon};
                    button->set_callback(callback);
                    button->set_tooltip(tooltip);
                    button->set_font_size(14);
                    button->set_enabled(enabled);
                    button->set_padding({10, 10});
                    return button;
                };

            makeImageButton("", true, [this] { openImageDialog(); }, FA_FOLDER, format("Open ({}+O)", HelpWindow::COMMAND));

            mCurrentImageButtons.push_back(
                makeImageButton("", false, [this] { saveImageDialog(); }, FA_SAVE, format("Save ({}+S)", HelpWindow::COMMAND))
            );

            mCurrentImageButtons.push_back(makeImageButton(
                "",
                false,
                [this] { reloadImage(mCurrentImage); },
                FA_RECYCLE,
                format("Reload ({}+R or F5)", HelpWindow::COMMAND)
            ));

            mCurrentImageButtons.push_back(makeImageButton(
                "",
                false,
                [this] {
                    auto* glfwWindow = screen()->glfw_window();
                    if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) {
                        removeAllImages();
                    } else {
                        removeImage(mCurrentImage);
                    }
                },
                FA_TIMES,
                format("Close ({}+W); Close all ({}+Shift+W)", HelpWindow::COMMAND, HelpWindow::COMMAND)
            ));

            mAnyImageButtons.push_back(makeImageButton(
                "A",
                false,
                [this] { reloadAllImages(); },
                0,
                format("Reload all ({}+Shift+R or {}+F5)", HelpWindow::COMMAND, HelpWindow::COMMAND)
            ));

            mWatchFilesForChangesButton =
                makeImageButton("W", true, {}, 0, "Watch image files and directories for changes and reload them automatically.");
            mWatchFilesForChangesButton->set_flags(Button::Flags::ToggleButton);
            mWatchFilesForChangesButton->set_change_callback([this](bool value) { setWatchFilesForChanges(value); });

            mImageInfoButton = makeImageButton("", false, {}, FA_INFO, "Show image info and metadata (I)");
            mImageInfoButton->set_flags(Button::ToggleButton);
            mImageInfoButton->set_change_callback([this](bool) { toggleImageInfoWindow(); });
            mAnyImageButtons.push_back(mImageInfoButton);

            spacer = new Widget{mSidebarLayout};
            spacer->set_height(3);
        }

        // List of open images
        {
            if (mSidebarOnRight) {
                new SidebarSectionHeader{mSidebarLayout, "Images"};
            }
            mImageScrollContainer = new VScrollPanel{mSidebarLayout};
            mImageScrollContainer->set_fixed_width(mSidebarLayout->fixed_width());

            mScrollContent = new Widget{mImageScrollContainer};
            mScrollContent->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill});

            mImageButtonContainer = new Widget{mScrollContent};
            mImageButtonContainer->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill});
        }
    }

    // Group selection
    {
        mFooter = new Widget{mVerticalScreenSplit};

        mGroupButtonContainer = new Widget{mFooter};
        mGroupButtonContainer->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill});
        mGroupButtonContainer->set_fixed_height(25);
        mFooter->set_fixed_height(25);
        mFooter->set_visible(false);
    }

    set_resize_callback([this](Vector2i) { requestLayoutUpdate(); });
    resize_callback_event(m_size.x(), m_size.y()); // Required on some OSs to get up-to-date pixel ratio

    selectImage(nullptr);
    selectReference(nullptr);

    if (!maximize) {
        // mDidFitToImage is only used when starting out maximized and wanting to fit the window to the image size after *unmaximizing*.
        mDidFitToImage = 3;
    }

    updateColorCapabilities();
    updateLayout();

    mInitialized = true;
}

bool ImageViewer::resize_event(const Vector2i& size) {
    mImageCanvas->setPixelRatio(pixel_ratio());
    requestLayoutUpdate();

    return Screen::resize_event(size);
}

void ImageViewer::setAiMaskPickMode(EAiMaskPickMode mode) {
    if (mAiMaskPickMode == mode) {
        return;
    }

    mAiMaskPickMode = mode;
    mImageCanvas->set_cursor(mode == EAiMaskPickMode::None ? Cursor::Arrow : Cursor::Crosshair);
    redraw();
}

bool ImageViewer::mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) {
    // Check if the user performed mousedown on an imagebutton so we can mark it as being dragged. This has to occur before
    // Screen::mouse_button_event as the button would absorb the event.
    if (down) {
        if (mImageScrollContainer->contains(p - mSidebarLayout->parent()->position())) {
            auto& buttons = mImageButtonContainer->children();

            Vector2i relMousePos = (absolute_position() + p) - mImageButtonContainer->absolute_position();

            for (size_t i = 0; i < buttons.size(); ++i) {
                const auto* imgButton = dynamic_cast<ImageButton*>(buttons[i]);
                if (imgButton->visible() && imgButton->contains(relMousePos) && !imgButton->textBoxVisible()) {
                    mDraggingStartPosition = relMousePos - imgButton->position();
                    mDragType = EMouseDragType::ImageButtonDrag;
                    mDraggedImageButtonId = i;
                    break;
                }
            }
        }
    }

    if (down && button == GLFW_MOUSE_BUTTON_LEFT && mAiMaskPickMode != EAiMaskPickMode::None && mImageCanvas->contains(p) && mCurrentImage) {
        const Vector2i rel = p - mImageCanvas->position();
        const Vector2i imageCoords = mImageCanvas->getImageCoords(mCurrentImage.get(), {rel.x(), rel.y()});
        if (mCurrentImage->contains(imageCoords) && mAiMaskPickHandler) {
            mAiMaskPickHandler(imageCoords, mAiMaskPickMode != EAiMaskPickMode::Negative);
            return true;
        }
    }

    if (Screen::mouse_button_event(p, button, down, modifiers)) {
        return true;
    }

    // Hide caption textbox when the user performed mousedown on any other component
    if (down) {
        for (auto& b : mImageButtonContainer->children()) {
            dynamic_cast<ImageButton*>(b)->hideTextBox();
        }
    }

    auto* glfwWindow = screen()->glfw_window();
    if (down) {
        if (mDragType != EMouseDragType::ImageButtonDrag) {
            mDraggingStartPosition = p;
            if (canDragSidebarFrom(p)) {
                mDragType = EMouseDragType::SidebarDrag;
                return true;
            } else if (mImageCanvas->contains(p) && mCurrentImage) {
                if (button == GLFW_MOUSE_BUTTON_RIGHT) {
                    mDragType = EMouseDragType::ImageRoi;
                } else {
                    mDragType = glfwGetKey(glfwWindow, GLFW_KEY_C) ? EMouseDragType::ImageCrop : EMouseDragType::ImageDrag;
                }
                return true;
            }
        }
    } else {
        if (mDragType == EMouseDragType::ImageButtonDrag) {
            requestLayoutUpdate();
        } else if (mDragType == EMouseDragType::ImageCrop) {
            if (norm(mDraggingStartPosition - p) < CROP_MIN_SIZE) {
                // If the user did not drag the mouse far enough, we assume that they wanted to reset the crop rather than create a new one.
                mImageCanvas->setCrop(nullopt);
            }
            syncCropUiFromCanvas();
        } else if (mDragType == EMouseDragType::ImageRoi) {
            if (norm(mDraggingStartPosition - p) < CROP_MIN_SIZE) {
                mImageCanvas->setRoi(nullopt);
            }
        }

        mDragType = EMouseDragType::None;
    }

    return true;
}

bool ImageViewer::mouse_motion_event_f(const Vector2f& p, const Vector2f& rel, int button, int modifiers) {
    if (Screen::mouse_motion_event_f(p, rel, button, modifiers)) {
        return true;
    }

    bool shouldShowResizeCursor = mDragType == EMouseDragType::SidebarDrag || canDragSidebarFrom(p);
    Cursor cursorType = shouldShowResizeCursor ? Cursor::HResize :
        (mAiMaskPickMode != EAiMaskPickMode::None ? Cursor::Crosshair : Cursor::Arrow);

    mSidebarLayout->set_cursor(cursorType);
    mImageCanvas->set_cursor(cursorType);

    switch (mDragType) {
        case EMouseDragType::SidebarDrag:
            if (mSidebarOnRight) {
                mSidebar->set_fixed_width(clamp((float)m_size.x() - p.x(), (float)SIDEBAR_MIN_WIDTH, (float)m_size.x() - 10.0f));
            } else {
                mSidebar->set_fixed_width(clamp(p.x(), (float)SIDEBAR_MIN_WIDTH, (float)m_size.x() - 10.0f));
            }
            requestLayoutUpdate();
            break;

        case EMouseDragType::ImageDrag: {
            Vector2f relativeMovement = rel;
            auto* glfwWindow = screen()->glfw_window();
            // There is no explicit access to the currently pressed modifier keys here, so we need to directly ask GLFW.
            if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) {
                relativeMovement /= 8;
            } else if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_CONTROL) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_CONTROL)) {
                relativeMovement *= 8;
            }

            // If left mouse button is held, move the image with mouse movement
            if ((button & 1) != 0) {
                mImageCanvas->translate(relativeMovement);
            }

            // If middle mouse button is held, zoom in-out with up-down mouse movement
            if ((button & 4) != 0) {
                mImageCanvas->scale(relativeMovement.y() / 8.0f, Vector2f{mDraggingStartPosition});
            }

            break;
        }

        case EMouseDragType::ImageCrop: {
            Vector2i relStartMousePos = (absolute_position() + mDraggingStartPosition) - mImageCanvas->absolute_position();
            Vector2i relMousePos = (absolute_position() + Vector2i{p}) - mImageCanvas->absolute_position();

            // Require a minimum movement to start cropping. Since this is measured in nanogui / screen space and not image space, this does
            // not prevent the cropping of smaller image regions. Just zoom in before cropping smaller regions.
            if (norm(relStartMousePos - relMousePos) < CROP_MIN_SIZE) {
                return false;
            }

            auto startImageCoords = mImageCanvas->getDisplayWindowCoords(mCurrentImage.get(), relStartMousePos);
            auto imageCoords = mImageCanvas->getDisplayWindowCoords(mCurrentImage.get(), relMousePos);

            // sanitize the input crop
            Box2i crop = {{{startImageCoords, imageCoords}}};
            crop.max += Vector2i{1};

            // we do not need to worry about min/max ordering here, as setCrop sanitizes the input for us
            mImageCanvas->setCrop(crop);

            break;
        }

        case EMouseDragType::ImageRoi: {
            Vector2i relStartMousePos = (absolute_position() + mDraggingStartPosition) - mImageCanvas->absolute_position();
            Vector2i relMousePos = (absolute_position() + Vector2i{p}) - mImageCanvas->absolute_position();

            if (norm(relStartMousePos - relMousePos) < CROP_MIN_SIZE) {
                return false;
            }

            auto startImageCoords = mImageCanvas->getDisplayWindowCoords(mCurrentImage.get(), relStartMousePos);
            auto imageCoords = mImageCanvas->getDisplayWindowCoords(mCurrentImage.get(), relMousePos);

            Box2i roi = {{{startImageCoords, imageCoords}}};
            roi.max += Vector2i{1};
            mImageCanvas->setRoi(roi);
            break;
        }

        case EMouseDragType::ImageButtonDrag: {
            auto& buttons = mImageButtonContainer->children();
            Vector2i relMousePos = (absolute_position() + Vector2i{p}) - mImageButtonContainer->absolute_position();

            TEV_ASSERT(mDraggedImageButtonId < buttons.size(), "Dragged image button id is out of bounds.");
            auto* draggedImgButton = dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId]);
            for (size_t i = 0; i < buttons.size(); ++i) {
                if (i == mDraggedImageButtonId) {
                    continue;
                }

                auto* imgButton = dynamic_cast<ImageButton*>(buttons[i]);
                if (imgButton->visible() && imgButton->contains(relMousePos)) {
                    Vector2i pos = imgButton->position();
                    pos.y() += ((int)draggedImgButton->id() - (int)imgButton->id()) * imgButton->size().y();
                    imgButton->set_position(pos);
                    imgButton->mouse_enter_event(relMousePos, false);

                    moveImageInList(mDraggedImageButtonId, i);
                    mDraggedImageButtonId = i;
                    break;
                }
            }

            dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId])->set_position(relMousePos - mDraggingStartPosition);

            break;
        }

        case EMouseDragType::None: break;
    }

    return focused();
}

bool ImageViewer::drop_event(const vector<string>& filenames) {
    if (Screen::drop_event(filenames)) {
        return true;
    }

    for (size_t i = 0; i < filenames.size(); ++i) {
        mImagesLoader->enqueue(toPath(filenames[i]), "", i == filenames.size() - 1);
    }

    // Make sure we gain focus after dragging files into here.
    focusWindow();
    return true;
}

bool ImageViewer::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (Screen::keyboard_event(key, scancode, action, modifiers)) {
        return true;
    }

    if (action == GLFW_PRESS && mAppShortcutHandler && mAppShortcutHandler(key, modifiers)) {
        return true;
    }

    int numGroups = mGroupButtonContainer->child_count();

    // Keybindings which should _not_ respond to repeats
    if (action == GLFW_PRESS) {
        // The checks for mod + GLFW_KEY_0 and GLFW_KEY_9 need to happen prior to checking for generic number keys as they should take
        // priority over group switching on Windows/Linux. No conflics on macOS.
        if (key == GLFW_KEY_0 && (modifiers & SYSTEM_COMMAND_MOD)) {
            mImageCanvas->resetTransform();
            return true;
        } else if (key == GLFW_KEY_F && (modifiers & SYSTEM_COMMAND_MOD) && mFilter) {
            mFilter->request_focus();
            mFilter->select_all();
            return true;
        } else if (key == GLFW_KEY_F || (key == GLFW_KEY_9 && (modifiers & SYSTEM_COMMAND_MOD))) {
            if (mCurrentImage) {
                mImageCanvas->fitImageToScreen(*mCurrentImage);
            }
            return true;
        } else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
            int idx = (key - GLFW_KEY_1 + 10) % 10;
            if (modifiers & GLFW_MOD_SHIFT) {
                const auto& image = nthVisibleImage(idx);
                if (image) {
                    if (mCurrentReference == image) {
                        selectReference(nullptr);
                    } else {
                        selectReference(image);
                    }
                }
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (idx >= 0 && idx < numGroups) {
                    selectGroup(nthVisibleGroup(idx));
                }
            } else {
                const auto& image = nthVisibleImage(idx);
                if (image) {
                    selectImage(image);
                }
            }
            return true;
        } else if (key == GLFW_KEY_HOME || key == GLFW_KEY_END) {
            const auto& image = nthVisibleImage(key == GLFW_KEY_HOME ? 0 : mImages.size());
            if (modifiers & GLFW_MOD_SHIFT) {
                if (mCurrentReference == image) {
                    selectReference(nullptr);
                } else {
                    selectReference(image);
                }
            } else {
                selectImage(image);
            }
            return true;
#ifdef __APPLE__
        } else if (key == GLFW_KEY_ENTER) {
#else
        } else if (key == GLFW_KEY_F2) {
#endif
            const auto id = imageId(mCurrentImage);
            if (id.has_value()) {
                dynamic_cast<ImageButton*>(mImageButtonContainer->child_at(*id))->showTextBox();
                requestLayoutUpdate();
            }

            return true;
        } else if (key == GLFW_KEY_N) {
            normalizeExposureAndOffset();
            return true;
        } else if (key == GLFW_KEY_U) {
            const bool clipToLdr = !mImageCanvas->clipToLdr();
            mImageCanvas->setClipToLdr(clipToLdr);
            if (mClipToLdrButton) {
                mClipToLdrButton->set_pushed(clipToLdr);
            }
            return true;
        } else if (key == GLFW_KEY_I) {
            toggleImageInfoWindow();
            return true;
        } else if (key == GLFW_KEY_R) {
            if (modifiers & SYSTEM_COMMAND_MOD) {
                if (modifiers & GLFW_MOD_SHIFT) {
                    reloadAllImages();
                } else {
                    reloadImage(mCurrentImage);
                }
            } else {
                resetImage();
            }
            return true;
        } else if (key == GLFW_KEY_X) {
            ungroupCurrentChannelGroup();
        } else if (key == GLFW_KEY_B && (modifiers & SYSTEM_COMMAND_MOD)) {
            setUiVisible(!isUiVisible());
            return true;
        } else if (key == GLFW_KEY_O && (modifiers & SYSTEM_COMMAND_MOD)) {
            openImageDialog();
            return true;
        } else if (key == GLFW_KEY_S && (modifiers & SYSTEM_COMMAND_MOD)) {
            saveImageDialog();
            return true;
        } else if (
            // question mark on US layout
            key == GLFW_KEY_SLASH && (modifiers & GLFW_MOD_SHIFT)
        ) {
            toggleHelpWindow();
            return true;
        } else if (key == GLFW_KEY_ENTER && modifiers & GLFW_MOD_ALT) {
            toggleMaximized();
            return true;
        } else if (key == GLFW_KEY_F5) {
            if (modifiers & SYSTEM_COMMAND_MOD) {
                reloadAllImages();
            } else {
                reloadImage(mCurrentImage);
            }
            return true;
        } else if (key == GLFW_KEY_F12) {
            // For debugging purposes.
            toggleConsole();
            return true;
        } else if (key == GLFW_KEY_SPACE) {
            setPlayingBack(!playingBack());
            return true;
        } else if (key == GLFW_KEY_ESCAPE) {
            setFilter("");
            return true;
        } else if (key == GLFW_KEY_Q && (modifiers & SYSTEM_COMMAND_MOD)) {
            set_visible(false);
            return true;
        } else if (key == GLFW_KEY_C && (modifiers & SYSTEM_COMMAND_MOD)) {
            if (modifiers & GLFW_MOD_SHIFT) {
                try {
                    copyImageNameToClipboard();
                } catch (const runtime_error& e) { showErrorDialog(format("Failed to copy image name to clipboard: {}", e.what())); }
            } else {
                try {
                    if (mFalseColorBarPopupButton && mFalseColorBarPopupButton->pushed()) {
                        copyFalseColorBarToClipboard();
                    } else {
                        copyImageCanvasToClipboard();
                    }
                } catch (const runtime_error& e) { showErrorDialog(format("Failed to copy image to clipboard: {}", e.what())); }
            }

            return true;
        } else if (key == GLFW_KEY_V && (modifiers & SYSTEM_COMMAND_MOD)) {
            if (modifiers & GLFW_MOD_SHIFT) {
                const char* clipboardString = glfwGetClipboardString(m_glfw_window);
                if (clipboardString) {
                    tlog::warning("Pasted string \"{}\" from clipboard, but tev can only paste images from clipboard.", clipboardString);
                }
            } else {
                try {
                    pasteImagesFromClipboard();
                } catch (const runtime_error& e) { showErrorDialog(format("Failed to paste image from clipboard: {}", e.what())); }
            }

            return true;
        }
    }

    // Keybindings which should respond to repeats
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (key == GLFW_KEY_KP_ADD || key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_SUBTRACT || key == GLFW_KEY_MINUS) {
            float scaleAmount = 1.0f;
            if (modifiers & GLFW_MOD_SHIFT) {
                scaleAmount /= 8;
            } else if (modifiers & GLFW_MOD_CONTROL) {
                scaleAmount *= 8;
            }

            if (key == GLFW_KEY_KP_SUBTRACT || key == GLFW_KEY_MINUS) {
                scaleAmount = -scaleAmount;
            }

            Vector2f origin = Vector2f{mImageCanvas->position()} + Vector2f{mImageCanvas->size()} * 0.5f;

            mImageCanvas->scale(scaleAmount, {origin.x(), origin.y()});
            return true;
        }

        if (key == GLFW_KEY_E) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setExposure(exposure() - 0.5f);
            } else {
                setExposure(exposure() + 0.5f);
            }

            return true;
        }

        if (key == GLFW_KEY_O) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setOffset(offset() - 0.1f);
            } else {
                setOffset(offset() + 0.1f);
            }

            return true;
        }

        if (key == GLFW_KEY_G) {
            if (mGammaSlider->enabled()) {
                if (modifiers & GLFW_MOD_SHIFT) {
                    setGamma(gamma() - 0.1f);
                } else {
                    setGamma(gamma() + 0.1f);
                }
            }

            return true;
        }

        if (key == GLFW_KEY_W && (modifiers & SYSTEM_COMMAND_MOD)) {
            if (modifiers & GLFW_MOD_SHIFT) {
                removeAllImages();
            } else {
                removeImage(mCurrentImage);
            }

            return true;
        } else if (key == GLFW_KEY_UP || key == GLFW_KEY_W || key == GLFW_KEY_PAGE_UP ||
                   (key == GLFW_KEY_TAB && (modifiers & GLFW_MOD_CONTROL) && (modifiers & GLFW_MOD_SHIFT))) {
            if (key != GLFW_KEY_TAB && (modifiers & GLFW_MOD_SHIFT)) {
                selectReference(nextImage(mCurrentReference, Backward));
            } else {
                selectImage(nextImage(mCurrentImage, Backward));
            }

            return true;
        } else if (key == GLFW_KEY_DOWN || key == GLFW_KEY_S || key == GLFW_KEY_PAGE_DOWN ||
                   (key == GLFW_KEY_TAB && (modifiers & GLFW_MOD_CONTROL) && !(modifiers & GLFW_MOD_SHIFT))) {
            if (key != GLFW_KEY_TAB && (modifiers & GLFW_MOD_SHIFT)) {
                selectReference(nextImage(mCurrentReference, Forward));
            } else {
                selectImage(nextImage(mCurrentImage, Forward));
            }

            return true;
        }

        if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_D || key == GLFW_KEY_RIGHT_BRACKET) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setTonemap(static_cast<ETonemap>(((int)tonemap() + 1) % (int)ETonemap::Count));
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (mCurrentReference) {
                    setMetric(static_cast<EMetric>(((int)metric() + 1) % (int)EMetric::Count));
                }
            } else {
                selectGroup(nextGroup(mCurrentGroup, Forward));
            }

            return true;
        } else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_A || key == GLFW_KEY_LEFT_BRACKET) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setTonemap(static_cast<ETonemap>(((int)tonemap() - 1 + (int)ETonemap::Count) % (int)ETonemap::Count));
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (mCurrentReference) {
                    setMetric(static_cast<EMetric>(((int)metric() - 1 + (int)EMetric::Count) % (int)EMetric::Count));
                }
            } else {
                selectGroup(nextGroup(mCurrentGroup, Backward));
            }

            return true;
        }

        float translationAmount = 64.0f;
        if (modifiers & GLFW_MOD_SHIFT) {
            translationAmount /= 8.0f;
            if (modifiers & GLFW_MOD_CONTROL) {
                translationAmount /= 8.0f;
            }
        } else if (modifiers & GLFW_MOD_CONTROL) {
            translationAmount *= 8.0f;
        }

        if (key == GLFW_KEY_H) {
            mImageCanvas->translate({translationAmount, 0});
            return true;
        } else if (key == GLFW_KEY_L) {
            mImageCanvas->translate({-translationAmount, 0});
            return true;
        } else if (key == GLFW_KEY_J) {
            mImageCanvas->translate({0, -translationAmount});
            return true;
        } else if (key == GLFW_KEY_K) {
            mImageCanvas->translate({0, translationAmount});
            return true;
        }
    }

    return true;
}

void ImageViewer::focusWindow() { glfwFocusWindow(m_glfw_window); }

void ImageViewer::draw_contents() {
    if (!mInitialized) {
        return;
    }

    if (!mZombieImagesRetired.empty()) {
        mZombieImagesRetired.clear();
    }
    mZombieImagesRetired.swap(mZombieImagesPending);

    updateColorCapabilities();

    // Update SDR white level from system settings if not overridden by the user
    if (mDisplayWhiteLevelBox && mDisplayWhiteLevelSettingComboBox &&
        displayWhiteLevelSetting() == EDisplayWhiteLevelSetting::System) {
        mDisplayWhiteLevelBox->set_value(glfwGetWindowSdrWhiteLevel(m_glfw_window));
    }

    updateCurrentMonitorSize();

    // HACK HACK HACK: on Windows, when restoring a window from maximization, the old window size is restored _several times_, necessitating
    // a repeated resize to the actually desired window size.
    if (mDidFitToImage < 3 && !isMaximized()) {
        ++mDidFitToImage;

        if (resizeWindowToFitImageOnLoad()) {
            resizeToFit(sizeToFitAllImages());
        }
    }

    clear();

    // If playing back, ensure correct frame pacing
    if (playingBack() && mTaskQueue.empty()) {
        auto fps = clamp(mFpsTextBox ? mFpsTextBox->value() : 24, 1, 1000);
        auto seconds_per_frame = chrono::duration<float>{1.0f / fps};
        auto now = chrono::steady_clock::now();

        if (now - mLastPlaybackFrameTime > 500s) {
            // If lagging behind too far, drop the frames, but otherwise...
            mLastPlaybackFrameTime = now;
            selectImage(nextImage(mCurrentImage, Forward), false);
        } else {
            // ...advance by as many frames as the user-specified FPS would demand, given the elapsed time since the last render.
            while (now - mLastPlaybackFrameTime >= seconds_per_frame) {
                mLastPlaybackFrameTime += chrono::duration_cast<chrono::steady_clock::duration>(seconds_per_frame);
                selectImage(nextImage(mCurrentImage, Forward), false);
            }
        }
    }

    // If watching files for changes, do so every 100ms
    if (watchFilesForChanges()) {
        auto now = chrono::steady_clock::now();
        if (now - mLastFileChangesCheckTime >= 100ms) {
            reloadImagesWhoseFileChanged();
            mImagesLoader->checkDirectoriesForNewFilesAndLoadThose();
            mLastFileChangesCheckTime = now;
        }
    }

    // In case any images got loaded in the background, they sit around in mImagesLoader. Here is the place where we actually add them to
    // the GUI. Focus the application in case one of the new images is meant to override the current selection.
    bool newFocus = false;
    while (auto addition = mImagesLoader->tryPop()) {
        newFocus |= addition->shallSelect;

        bool first = true;
        for (auto& image : addition->images) {
            // If the loaded file consists of multiple images (such as multi-part EXRs), select the first part if selection is desired.
            bool shallSelect = first ? addition->shallSelect : false;
            if (addition->toReplace) {
                replaceImage(addition->toReplace, image, shallSelect);
            } else {
                addImage(image, shallSelect);
            }
            first = false;
        }
    }

    if (newFocus) {
        focusWindow();
    }

    // mTaskQueue contains jobs that should be executed on the main thread. It is useful for handling callbacks from background threads
    while (auto task = mTaskQueue.tryPop()) {
        (*task)();
    }

    if (!mPendingImageRemovals.empty()) {
        auto pending = std::move(mPendingImageRemovals);
        mPendingImageRemovals.clear();
        for (auto& imageName : pending) {
            removeImage(imageName);
        }
    }

    for (auto it = begin(mToBump); it != end(mToBump);) {
        auto& image = *it;
        bool isShown = image == mCurrentImage || image == mCurrentReference;

        // If the image is no longer shown, bump ID immediately. Otherwise, wait until canvas statistics were ready for over 200 ms.
        if (!isShown || chrono::steady_clock::now() - mImageCanvas->canvasStatistics()->becameReadyAt() > 200ms) {
            image->bumpId();
            auto localIt = it;
            ++it;
            mToBump.erase(localIt);
        } else {
            ++it;
        }
    }

    if (mRequiresFilterUpdate) {
        updateFilter();
        mRequiresFilterUpdate = false;
    }

    const bool anyImageVisible = mCurrentImage || mCurrentReference ||
        any_of(begin(mImageButtonContainer->children()), end(mImageButtonContainer->children()), [](const auto& c) { return c->visible(); });

    for (auto button : mAnyImageButtons) {
        button->set_enabled(anyImageVisible);
    }

    if (mRequiresLayoutUpdate) {
        Vector2i oldDraggedImageButtonPos{0, 0};
        auto& buttons = mImageButtonContainer->children();
        if (mDragType == EMouseDragType::ImageButtonDrag) {
            oldDraggedImageButtonPos = dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId])->position();
        }

        updateLayout();
        mRequiresLayoutUpdate = false;

        if (mDragType == EMouseDragType::ImageButtonDrag) {
            dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId])->set_position(oldDraggedImageButtonPos);
        }
    }

    updateTitle();

    static constexpr string_view histogramTooltipBase =
        "Histogram of color values with logarithmic x-axis. Adapts to the currently chosen channel group, error metric, and inspection color space.";
    if (auto lazyCanvasStatistics = mImageCanvas->canvasStatistics()) {
        if (lazyCanvasStatistics->isReady()) {
            auto statistics = lazyCanvasStatistics->get();
            int histogramChannels = statistics->nChannels;
            vector<Color> histogramColors = statistics->histogramColors;
            vector<float> histogramValues = statistics->histogram;
            float histogramMinimum = statistics->minimum;
            float histogramMean = statistics->mean;
            float histogramMaximum = statistics->maximum;
            float histogramStdDev = statistics->stddev;
            string histogramGroupName = inspectionCombinedLabel();
            int displayChannelIndex = 0;

            if (const auto selection = inspectionChannelSelectionForGroup(mCurrentGroup)) {
                displayChannelIndex = *selection;
            }

            if (mHighlightModeComboBox && statistics->nChannels >= 3) {
                const int channelIndex = mHighlightModeSelection;
                if (channelIndex >= 1 && channelIndex <= 3) {
                    const size_t nBins = histogramValues.size() / (size_t)statistics->nChannels;
                    const size_t offset = (size_t)(channelIndex - 1) * nBins;
                    histogramChannels = 1;
                    histogramColors = {statistics->histogramColors[(size_t)(channelIndex - 1)]};
                    histogramValues = vector<float>{histogramValues.begin() + offset, histogramValues.begin() + offset + nBins};
                    histogramMinimum = statistics->channelMinimum[(size_t)(channelIndex - 1)];
                    histogramMean = statistics->channelMean[(size_t)(channelIndex - 1)];
                    histogramMaximum = statistics->channelMaximum[(size_t)(channelIndex - 1)];
                    histogramStdDev = statistics->channelStdDev[(size_t)(channelIndex - 1)];
                    histogramGroupName = inspectionChannelLabel((size_t)(channelIndex - 1));
                } else if (displayChannelIndex >= 1 && displayChannelIndex <= 3) {
                    const size_t nBins = histogramValues.size() / (size_t)statistics->nChannels;
                    const size_t offset = (size_t)(displayChannelIndex - 1) * nBins;
                    histogramChannels = 1;
                    histogramColors = {statistics->histogramColors[(size_t)(displayChannelIndex - 1)]};
                    histogramValues = vector<float>{histogramValues.begin() + offset, histogramValues.begin() + offset + nBins};
                    histogramMinimum = statistics->channelMinimum[(size_t)(displayChannelIndex - 1)];
                    histogramMean = statistics->channelMean[(size_t)(displayChannelIndex - 1)];
                    histogramMaximum = statistics->channelMaximum[(size_t)(displayChannelIndex - 1)];
                    histogramStdDev = statistics->channelStdDev[(size_t)(displayChannelIndex - 1)];
                    histogramGroupName = inspectionChannelLabel((size_t)(displayChannelIndex - 1));
                }
            } else if (statistics->nChannels == 1) {
                histogramGroupName = inspectionSingleChannelLabel();
            }

            mHistogram->setNChannels(histogramChannels);
            mHistogram->setColors(histogramColors);
            mHistogram->setValues(histogramValues);
            mHistogram->setMinimum(histogramMinimum);
            mHistogram->setMean(histogramMean);
            mHistogram->setMaximum(histogramMaximum);
            mHistogram->setZero(statistics->histogramZero);
            mHistogram->setHeader("");
            mHistogram->set_tooltip(format(
                "{}\n\n"
                "Current group: {}\n"
                "Region: {},{} {}x{}\n"
                "Minimum: {:.3f}\n"
                "Mean: {:.3f}\n"
                "Maximum: {:.3f}\n"
                "Std. dev.: {:.3f}",
                histogramTooltipBase,
                histogramGroupName.empty() ? "default" : histogramGroupName,
                statistics->region.min.x(),
                statistics->region.min.y(),
                statistics->region.size().x(),
                statistics->region.size().y(),
                histogramMinimum,
                histogramMean,
                histogramMaximum,
                histogramStdDev
            ));
            updateRoiStatsLabel(statistics);
        }
    } else {
        mHistogram->setNChannels(1);
        mHistogram->setColors({
            {1.0f, 1.0f, 1.0f}
        });
        mHistogram->setValues({{0.0f}});
        mHistogram->setMinimum(0);
        mHistogram->setMean(0);
        mHistogram->setMaximum(0);
        mHistogram->setZero(0);
        mHistogram->setHeader("");
        mHistogram->set_tooltip(histogramTooltipBase);
        updateRoiStatsLabel(nullptr);
    }
}

void ImageViewer::updateColorCapabilities() {
    const auto prevColorSpace = mSystemColorSpace;
    mSystemColorSpace = ColorSpace{
        .transfer = ituth273::fromWpTransfer(glfwGetWindowTransfer(m_glfw_window)),
        .primaries = static_cast<EWpPrimaries>(glfwGetWindowPrimaries(m_glfw_window)),
        .maxLuminance = glfwGetWindowMaxLuminance(m_glfw_window),
    };

    if (mSystemColorSpace == prevColorSpace) {
        return;
    }

    const auto& cs = *mSystemColorSpace;

#if defined(__APPLE__)
    const auto [supportsWideGamut, supportsHdr] = test_10bit_edr_support();
    const bool supportsAbsoluteBrightness = supportsHdr;
#else // Linux and Windows
    const bool supportsExtendedRange = m_float_buffer || cs.transfer == ituth273::ETransfer::PQ || cs.transfer == ituth273::ETransfer::HLG;
    const bool supportsHdr = supportsExtendedRange &&
        (cs.maxLuminance > 80.0f || cs.maxLuminance == 0.0f); // Some systems don't report max luminance (value of 0.0). Assume HDR then.
    const bool supportsWideGamut = supportsExtendedRange ||
        cs.primaries != EWpPrimaries::SRGB; // Non-sRGB primaries imply wide color support.
    const bool supportsAbsoluteBrightness = supportsHdr;
#endif

    tlog::info(
        "{} {} bit {} point frame buffer with primaries={} transfer={} range={}",
        prevColorSpace ? "Switched to" : "Initialized",
        this->bits_per_sample(),
        m_float_buffer ? "floating" : "fixed",
        toString(cs.primaries),
        ituth273::toString(cs.transfer),
        supportsHdr           ? "hdr" :
            supportsWideGamut ? "wide_gamut_sdr" :
                                "sdr"
    );

    // Update UI elements accordingly
    if (mHdrPopupButton) {
        mHdrPopupButton->set_enabled(supportsHdr);
        if (supportsHdr) {
            mHdrPopupButton->set_tooltip("HDR Settings");
        } else {
            mHdrPopupButton->set_tooltip(
                "Your system does not support HDR colors. "
                "Make sure that your OS, GPU, and display support HDR and that it is enabled in your system and display settings."
            );
        }
    }

    if (mClipToLdrButton) {
        mClipToLdrButton->set_enabled(supportsHdr);
    }

    if (mDisplayWhiteLevelBox) {
        if (supportsAbsoluteBrightness) {
            mDisplayWhiteLevelBox->set_tooltip(
                "The display reference white level (aka. paper white) in nits (cd/m²). "
                "This value determines how bright a pixel value of 1.0 appears on the display. "
                "It follows your system settings by default.\n\n"
                "You can customize this value to change the brightness at which images are displayed. "
                "Or you can link this value to the image white level (if known) to display images at their absolute brightness "
                "rather than relative to your system's brightness setting."
            );
        } else {
            mDisplayWhiteLevelBox->set_tooltip(
                "Your system or display does not support absolute brightness rendering. "
                "White level override is disabled."
            );
        }

        mDisplayWhiteLevelBox->set_editable(supportsAbsoluteBrightness);
        mDisplayWhiteLevelBox->set_enabled(supportsAbsoluteBrightness);
    }

    if (mDisplayWhiteLevelSettingComboBox) {
        mDisplayWhiteLevelSettingComboBox->set_enabled(supportsAbsoluteBrightness);
    }
}

void ImageViewer::insertImage(shared_ptr<Image> image, size_t index, bool shallSelect) {
    if (!image) {
        throw invalid_argument{"Image may not be null."};
    }

    if (mDragType == EMouseDragType::ImageButtonDrag && index <= mDraggedImageButtonId) {
        ++mDraggedImageButtonId;
    }

    auto button = new ImageButton{nullptr, image->name(), true};
    button->set_font_size(14);
    button->setId(index + 1);
    button->set_tooltip(image->toString());

    const string imageName{image->name()};
    button->setSelectedCallback([this, imageName]() { selectImage(imageByName(imageName)); });
    button->setCloseCallback([this, imageName]() { mPendingImageRemovals.push_back(imageName); });

    button->setReferenceCallback([this, imageName](bool isReference) {
        auto image = imageByName(imageName);
        if (!isReference) {
            selectReference(nullptr);
        } else {
            selectReference(image);
        }
    });

    button->setCaptionChangeCallback([this]() { mRequiresFilterUpdate = true; });

    mImageButtonContainer->add_child((int)index, button);
    mImages.insert(begin(mImages) + index, image);

    mShouldFooterBeVisible |= image->channelGroups().size() > 1;
    // The following call will make sure the footer becomes visible if the previous line enabled it.
    setUiVisible(isUiVisible());

    // Ensure the new image button will have the correct visibility state.
    setFilter(mFilter ? mFilter->value() : "");

    requestLayoutUpdate();

    if (mImageInsertedCallback) {
        mImageInsertedCallback(image);
    }

    // First image got added, let's select it.
    if ((index == 0 && mImages.size() == 1) || shallSelect) {
        selectImage(image);
        if (!isMaximized() && resizeWindowToFitImageOnLoad()) {
            resizeToFit(sizeToFitImage(image));
        }
    }
}

void ImageViewer::moveImageInList(size_t oldIndex, size_t newIndex) {
    if (oldIndex == newIndex) {
        return;
    }

    TEV_ASSERT(oldIndex < mImages.size(), "oldIndex must be smaller than the number of images.");
    TEV_ASSERT(newIndex < mImages.size(), "newIndex must be smaller than the number of images.");

    auto* button = dynamic_cast<ImageButton*>(mImageButtonContainer->child_at((int)oldIndex));
    TEV_ASSERT(button, "Image button must exist.");

    button->inc_ref();
    mImageButtonContainer->remove_child_at((int)oldIndex);
    mImageButtonContainer->add_child((int)newIndex, button);
    button->dec_ref();

    int change = newIndex > oldIndex ? 1 : -1;
    for (size_t i = oldIndex; i != newIndex; i += change) {
        auto* curButton = dynamic_cast<ImageButton*>(mImageButtonContainer->child_at((int)i));
        if (curButton->visible()) {
            curButton->setId(curButton->id() - change);
            button->setId(button->id() + change);
        }
    }

    auto img = mImages[oldIndex];
    mImages.erase(mImages.begin() + oldIndex);
    mImages.insert(mImages.begin() + newIndex, img);

    requestLayoutUpdate();
}

void ImageViewer::removeImage(shared_ptr<Image> image) {
    if (!image) {
        return;
    }

    const auto id = imageId(image);
    if (!id) {
        return;
    }

    if (mDragType == EMouseDragType::ImageButtonDrag) {
        // If we're currently dragging the to-be-removed image, stop.
        if (id == mDraggedImageButtonId) {
            requestLayoutUpdate();
            mDragType = EMouseDragType::None;
        } else if (id < mDraggedImageButtonId) {
            --mDraggedImageButtonId;
        }
    }

    auto nextCandidate = nextImage(image, Forward);
    // If we rolled over, let's rather use the previous image. We don't want to jumpt to the beginning when deleting the last image in our
    // list.
    if (imageId(nextCandidate) < id) {
        nextCandidate = nextImage(image, Backward);
    }

    // If `nextImage` produced the same image again, this means that `image` is the only (visible) image and hence, after removal, should be
    // replaced by no selection at all.
    if (nextCandidate == image) {
        nextCandidate = nullptr;
    }

    // Reset all focus as a workaround a crash caused by nanogui.
    // TODO: Remove once a fix exists.
    request_focus();

    mImages.erase(begin(mImages) + *id);
    mImageButtonContainer->remove_child_at((int)*id);
    mZombieImagesPending.push_back(image);

    if (mImages.empty()) {
        selectImage(nullptr);
        selectReference(nullptr);
        return;
    }

    if (mCurrentImage == image) {
        selectImage(nextCandidate);
    }

    if (mCurrentReference == image) {
        selectReference(nextCandidate);
    }
}

void ImageViewer::removeAllImages() {
    if (mImages.empty()) {
        return;
    }

    // Reset all focus as a workaround a crash caused by nanogui.
    // TODO: Remove once a fix exists.
    request_focus();

    for (int i = (int)mImages.size() - 1; i >= 0; --i) {
        if (mImageButtonContainer->child_at(i)->visible()) {
            mZombieImagesPending.push_back(mImages[(size_t)i]);
            mImages.erase(begin(mImages) + i);
            mImageButtonContainer->remove_child_at(i);
        }
    }

    // No images left to select
    selectImage(nullptr);
    selectReference(nullptr);
}

void ImageViewer::replaceImage(shared_ptr<Image> image, shared_ptr<Image> replacement, bool shallSelect) {
    if (replacement == nullptr) {
        throw runtime_error{"Must not replace image with nullptr."};
    }

    const auto currentId = imageId(mCurrentImage);
    const auto id = imageId(image);
    if (!id) {
        addImage(replacement, shallSelect);
        return;
    }

    // Preserve image button caption when replacing an image
    ImageButton* ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children().at(*id));
    const string caption{ib->caption()};

    // If we already have the image selected, we must re-select it regardless of the `shallSelect` parameter.
    shallSelect |= currentId == id;

    const auto referenceId = imageId(mCurrentReference);

    removeImage(image);
    insertImage(replacement, *id, shallSelect);

    ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children().at(*id));
    ib->setCaption(caption);

    if (referenceId) {
        selectReference(mImages.at(*referenceId));
    }
}

void ImageViewer::reloadImage(shared_ptr<Image> image, bool shallSelect) {
    if (imageId(image)) {
        mImagesLoader->enqueue(image->path(), image->channelSelector(), shallSelect, image);
    }
}

void ImageViewer::reloadAllImages() {
    for (size_t i = 0; i < mImages.size(); ++i) {
        reloadImage(mImages[i]);
    }
}

void ImageViewer::reloadImagesWhoseFileChanged() {
    for (size_t i = 0; i < mImages.size(); ++i) {
        auto& image = mImages[i];
        if (!fs::exists(image->path())) {
            continue;
        }

        fs::file_time_type fileLastModified;

        // Unlikely, but the file could have been deleted, moved, or something else could have happened to it that makes obtaining its last
        // modified time impossible. Ignore such errors.
        try {
            fileLastModified = fs::last_write_time(image->path());
        } catch (...) { continue; }

        if (fileLastModified != image->fileLastModified()) {
            // Updating the last-modified date prevents double-scheduled reloads if the load take a lot of time or fails.
            image->setFileLastModified(fileLastModified);
            reloadImage(image);
        }
    }
}

void ImageViewer::updateImage(string_view imageName, bool shallSelect, string_view channel, Box2i bounds, span<const float> imageData) {
    auto image = imageByName(imageName);
    if (!image) {
        tlog::warning("Image {} could not be updated, because it does not exist.", imageName);
        return;
    }

    image->updateChannel(channel, bounds, imageData);
    if (shallSelect) {
        selectImage(image);
    }

    // This image needs newly computed statistics... so give it a new ID. However, if the image is currently shown, we don't want to
    // overwhelm the CPU, so we only launch new statistics computations every so often. These computations are scheduled from `drawContents`
    // via the `mToBump` set.
    if (image != mCurrentImage && image != mCurrentReference) {
        image->bumpId();
    } else {
        mToBump.insert(image);
    }
}

void ImageViewer::updateImageVectorGraphics(string_view imageName, bool shallSelect, bool append, span<const VgCommand> commands) {
    auto image = imageByName(imageName);
    if (!image) {
        tlog::warning("Vector graphics of image {} could not be updated, because it does not exist.", imageName);
        return;
    }

    image->updateVectorGraphics(append, commands);
    if (shallSelect) {
        selectImage(image);
    }
}

void ImageViewer::selectImage(const shared_ptr<Image>& image, bool stopPlayback) {
    // Once the selected image has been updated, reflect that in the image info window.
    const auto imageInfoGuard = ScopeGuard{[this]() {
        if (mImageInfoWindow) {
            updateImageInfoWindow();
        }
    }};

    if (stopPlayback && playingBack()) {
        setPlayingBack(false);
    }

    for (auto button : mCurrentImageButtons) {
        button->set_enabled(image != nullptr);
    }

    if (!image) {
        auto& buttons = mImageButtonContainer->children();
        for (size_t i = 0; i < buttons.size(); ++i) {
            dynamic_cast<ImageButton*>(buttons[i])->setIsSelected(false);
        }

        mCurrentImage = nullptr;
        invalidateSourceInterpretationCache();
        mImageCanvas->setImage(nullptr);

        // Clear group buttons
        while (mGroupButtonContainer->child_count() > 0) {
            mGroupButtonContainer->remove_child_at(mGroupButtonContainer->child_count() - 1);
        }

        setImageWhiteLevel(DEFAULT_IMAGE_WHITE_LEVEL);
        refreshProjectionUi();

        requestLayoutUpdate();
        return;
    }

    if (image != mCurrentImage) {
    }

    const auto id = imageId(image).value_or(0);

    // When the sidebar is hidden, image buttons are not visible even though the
    // image should still be selectable programmatically.
    if (mSidebar->visible() && !mImageButtonContainer->child_at((int)id)->visible()) {
        return;
    }

    auto& buttons = mImageButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        dynamic_cast<ImageButton*>(buttons[i])->setIsSelected(i == id);
    }

    mCurrentImage = image;
    invalidateSourceInterpretationCache();
    mImageCanvas->setImage(mCurrentImage);
    if (mImageSelectedCallback) {
        mImageSelectedCallback(mCurrentImage);
    }
    if (mImageCanvas->crop().has_value() && !mImageCanvas->cropInImageCoords().isValid()) {
        mImageCanvas->setCrop(nullopt);
    }
    syncCropUiFromCanvas();
    refreshProjectionUi();

    setImageWhiteLevel(mCurrentImage->whiteLevel());
    refreshInspectionModeUi();
    applyInspectionPreset(mInspectionPreset);

    // Clear group buttons
    while (mGroupButtonContainer->child_count() > 0) {
        mGroupButtonContainer->remove_child_at(mGroupButtonContainer->child_count() - 1);
    }

    const size_t numGroups = mCurrentImage->channelGroups().size();
    for (size_t i = 0; i < numGroups; ++i) {
        const std::string group = std::string{groupName(i)};
        const auto button = new ImageButton{mGroupButtonContainer, group, false};
        button->set_font_size(14);
        button->setId(i + 1);

        button->setSelectedCallback([this, group]() { selectGroup(group); });
    }

    mShouldFooterBeVisible |= image->channelGroups().size() > 1;
    // The following call will make sure the footer becomes visible if the previous line enabled it.
    setUiVisible(isUiVisible());

    // Setting the filter again makes sure, that groups are correctly filtered.
    setFilter(mFilter ? mFilter->value() : "");
    requestLayoutUpdate();

    // This will automatically fall back to the root group if the current group isn't found.
    selectGroup(mCurrentGroup);

    // Ensure the currently active image button is always fully on-screen
    Widget* activeImageButton = nullptr;
    for (Widget* widget : mImageButtonContainer->children()) {
        if (dynamic_cast<ImageButton*>(widget)->isSelected()) {
            activeImageButton = widget;
            break;
        }
    }

    if (activeImageButton) {
        float divisor = mScrollContent->height() - mImageScrollContainer->height();
        if (divisor > 0) {
            mImageScrollContainer->set_scroll(clamp(
                mImageScrollContainer->scroll(),
                (activeImageButton->position().y() + activeImageButton->height() - mImageScrollContainer->height()) / divisor,
                activeImageButton->position().y() / divisor
            ));
        }
    }

    if (autoFitToScreen()) {
        mImageCanvas->fitImageToScreen(*mCurrentImage);
    }
}

void ImageViewer::selectGroup(string_view group) {
    // If the group does not exist, select the first group.
    const auto id = groupId(group).value_or(0);

    auto& buttons = mGroupButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        dynamic_cast<ImageButton*>(buttons[i])->setIsSelected(i == id);
    }

    mCurrentGroup = groupName(id);
    if (mInspectionPreset != InspectionSource) {
        if (const auto preferredGroup = preferredInspectionGroup()) {
            mImageCanvas->setRequestedChannelGroup(*preferredGroup);
        } else {
            mImageCanvas->setRequestedChannelGroup(mCurrentGroup);
        }
    } else {
        mImageCanvas->setRequestedChannelGroup(mCurrentGroup);
    }
    syncHighlightChannelGroup();
    syncInspectionDisplayChannelSelection();
    refreshInspectionModeUi();
    applyInspectionPreset(mInspectionPreset);

    // Ensure the currently active group button is always fully on-screen
    Widget* activeGroupButton = nullptr;
    for (Widget* widget : mGroupButtonContainer->children()) {
        if (dynamic_cast<ImageButton*>(widget)->isSelected()) {
            activeGroupButton = widget;
            break;
        }
    }

    // Ensure the currently active group button is always fully on-screen
    if (activeGroupButton) {
        mGroupButtonContainer->set_position(
            Vector2i{
                clamp(
                    mGroupButtonContainer->position().x(),
                    -activeGroupButton->position().x(),
                    m_size.x() - activeGroupButton->position().x() - activeGroupButton->width()
                ),
                0
            }
        );
    }

    redraw();
}

void ImageViewer::selectReference(const shared_ptr<Image>& image) {
    if (!image) {
        auto& buttons = mImageButtonContainer->children();
        for (size_t i = 0; i < buttons.size(); ++i) {
            dynamic_cast<ImageButton*>(buttons[i])->setIsReference(false);
        }

        auto& metricButtons = mMetricButtonContainer->children();
        for (size_t i = 0; i < metricButtons.size(); ++i) {
            dynamic_cast<Button*>(metricButtons[i])->set_enabled(false);
        }

        mCurrentReference = nullptr;
        mImageCanvas->setReference(nullptr);
        refreshInspectionModeUi();
        applyInspectionPreset(mInspectionPreset);
        return;
    }

    const auto id = imageId(image).value_or(0);

    auto& buttons = mImageButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        dynamic_cast<ImageButton*>(buttons[i])->setIsReference(i == id);
    }

    auto& metricButtons = mMetricButtonContainer->children();
    for (size_t i = 0; i < metricButtons.size(); ++i) {
        dynamic_cast<Button*>(metricButtons[i])->set_enabled(true);
    }

    mCurrentReference = image;
    mImageCanvas->setReference(mCurrentReference);
    refreshInspectionModeUi();
    applyInspectionPreset(mInspectionPreset);

    // Ensure the currently active reference button is always fully on-screen
    Widget* activeReferenceButton = nullptr;
    for (Widget* widget : mImageButtonContainer->children()) {
        if (dynamic_cast<ImageButton*>(widget)->isReference()) {
            activeReferenceButton = widget;
            break;
        }
    }

    if (activeReferenceButton) {
        float divisor = mScrollContent->height() - mImageScrollContainer->height();
        if (divisor > 0) {
            mImageScrollContainer->set_scroll(clamp(
                mImageScrollContainer->scroll(),
                (activeReferenceButton->position().y() + activeReferenceButton->height() - mImageScrollContainer->height()) / divisor,
                activeReferenceButton->position().y() / divisor
            ));
        }
    }
}

void ImageViewer::setExposure(float value) {
    value = round(value, 1.0f);
    mExposureSlider->set_value(value);
    mExposureLabel->set_caption(format("Exposure: {:+.1f}", value));

    mImageCanvas->setExposure(value);
}

void ImageViewer::setExposurePrecise(float value) {
    mExposureSlider->set_value(value);
    mExposureLabel->set_caption(format("Exposure: {:+.2f}", value));
    mImageCanvas->setExposure(value);
}

void ImageViewer::setRadiance179Preview(bool value) {
    mRadiance179Preview = value;
    // Perceptual maps are already in the engine's output units; the source file's EXPOSURE
    // compensation is not applied to them again for display and readouts.
    mImageCanvas->setInspectionScale(perceptualMapShown() ? 1.0f : radiometricInspectionScale());
}

void ImageViewer::setInspectionPresetIndex(int presetIndex) {
    applyInspectionPreset(presetIndex);
}

bool ImageViewer::currentGroupHasThreeColorChannels() const {
    return groupHasThreeColorChannels(mCurrentGroup);
}

bool ImageViewer::groupHasThreeColorChannels(string_view group) const {
    if (!mCurrentImage) {
        return false;
    }

    const auto channelsSpan = mCurrentImage->channelsInGroup(group);
    vector<string_view> channels{begin(channelsSpan), end(channelsSpan)};
    channels.erase(unique(begin(channels), end(channels)), end(channels));
    const bool hasAlpha = channels.size() > 1 && Channel::isAlpha(channels.back());
    const size_t nColorChannels = channels.size() - (hasAlpha ? 1 : 0);
    return nColorChannels >= 3;
}

optional<string> ImageViewer::preferredInspectionGroup() const {
    if (!mCurrentImage) {
        return nullopt;
    }

    if (groupHasThreeColorChannels(mCurrentGroup)) {
        return string{mCurrentGroup};
    }

    for (const auto& group : mCurrentImage->channelGroups()) {
        if (groupHasThreeColorChannels(group.name)) {
            return group.name;
        }
    }

    return nullopt;
}

optional<Matrix3f> parseCameraToRgb(std::span<const AttributeNode> nodes) {
    if (const auto cameraToRgbValue = findAttributeValue(nodes, "Camera2RGB")) {
        const auto cameraToRgbValues = parseAttributeFloatList(*cameraToRgbValue);
        if (cameraToRgbValues.size() == 9) {
            Matrix3f cameraToRgb = Matrix3f{1.0f};
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    cameraToRgb.m[(size_t)col][(size_t)row] = cameraToRgbValues[(size_t)(row * 3 + col)];
                }
            }
            return cameraToRgb;
        }
    }

    return nullopt;
}

optional<int> ImageViewer::inspectionChannelSelectionForGroup(string_view group) const {
    if (!mCurrentImage) {
        return nullopt;
    }

    if (groupHasThreeColorChannels(group)) {
        return 0;
    }

    const auto tail = Channel::tail(group);
    if (tail.size() != 1) {
        return nullopt;
    }

    switch (std::toupper((unsigned char)tail.front())) {
        case 'R':
        case 'X': return 1;
        case 'G':
        case 'Y': return 2;
        case 'B':
        case 'Z': return 3;
        default: return nullopt;
    }
}

string ImageViewer::effectiveHighlightGroup() const {
    if (!mCurrentImage) {
        return {};
    }

    if (const auto preferredGroup = preferredInspectionGroup()) {
        return *preferredGroup;
    }

    return string{mCurrentGroup};
}

void ImageViewer::syncHighlightChannelGroup() {
    if (!mImageCanvas) {
        return;
    }

    mImageCanvas->setHighlightChannelGroup(effectiveHighlightGroup());
}

int ImageViewer::effectiveInspectionDisplayChannelSelection() const {
    if (!mCurrentImage) {
        return 0;
    }

    if (const auto selection = inspectionChannelSelectionForGroup(mCurrentGroup)) {
        return *selection;
    }

    return 0;
}

void ImageViewer::syncInspectionDisplayChannelSelection() {
    if (!mImageCanvas) {
        return;
    }

    const int selection = mInspectionPreset == InspectionSource ? 0 : effectiveInspectionDisplayChannelSelection();
    mImageCanvas->setInspectionDisplayChannelSelection(selection);
}

bool ImageViewer::canConvertSourceToXyz() const {
    if (!mCurrentImage || !preferredInspectionGroup().has_value()) {
        return false;
    }

    if (currentImageUsesRadiometricMonochromeFallback()) {
        return true;
    }

    const auto source = sourceValueLabel();
    if (source == "Raw") {
        return parseSensorToXyz(mCurrentImage->attributes()).has_value();
    }

    if (source == "XYZ") {
        return true;
    }

    return parseTargetChroma(mCurrentImage->attributes()).has_value();
}

optional<Matrix3f> ImageViewer::inspectionSourceToRawMatrix() const {
    if (!mCurrentImage) {
        return nullopt;
    }

    if (const auto cameraToRgb = parseCameraToRgb(mCurrentImage->attributes())) {
        const auto source = sourceValueLabel();
        if (source == "Rad" || source == "sRGB" || source == "RGB") {
            return inverse(*cameraToRgb);
        }
    }

    const auto sensorToXyz = parseSensorToXyz(mCurrentImage->attributes());
    if (!sensorToXyz) {
        return nullopt;
    }

    return inverse(*sensorToXyz) * inspectionSourceToXyzMatrix();
}

Matrix3f ImageViewer::inspectionSourceToRadMatrix() const {
    return inverse(primariesToXyzMatrix(radChroma())) * inspectionSourceToXyzMatrix();
}

Matrix3f ImageViewer::inspectionSourceToRec709Matrix() const {
    return inverse(primariesToXyzMatrix(rec709Chroma())) * inspectionSourceToXyzMatrix();
}

Matrix3f ImageViewer::inspectionSourceToXyzMatrix() const {
    if (!mCurrentImage) {
        return Matrix3f{1.0f};
    }

    if (currentImageUsesRadiometricMonochromeFallback()) {
        return Matrix3f{1.0f};
    }

    const auto source = sourceValueLabel();
    if (source == "Raw") {
        if (const auto sensorToXyz = parseSensorToXyz(mCurrentImage->attributes())) {
            return *sensorToXyz;
        }
    }

    if (source == "XYZ") {
        return Matrix3f{1.0f};
    }

    if (const auto targetChroma = parseTargetChroma(mCurrentImage->attributes())) {
        return primariesToXyzMatrix(*targetChroma);
    }

    return Matrix3f{1.0f};
}

bool ImageViewer::inspectionPresetSupported(int presetIndex) const {
    if (presetIndex == InspectionSource) {
        return true;
    }

#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
    if (inspectionPresetUsesExperimentalMap(presetIndex)) {
        if (!currentImageSupportsExperimentalPerceptualMaps()) {
            return false;
        }
        const int colorKind = sourceColorKind();
        if (colorKind != 0 && colorKind != 1 && colorKind != 2 && colorKind != 4) {
            return false;
        }
        if (presetIndex == InspectionEqvLuminance) {
            const auto source = sourceValueLabel();
            return source == "Rad" || source == "sRGB" || source == "XYZ";
        }
        return true;
    }
#endif

    if (!canConvertSourceToXyz()) {
        return false;
    }
    if (currentImageUsesRadiometricMonochromeFallback()) {
        // Assigned luminance (cd/m²): the values already are luminance.
        return presetIndex == InspectionLuminance;
    }

    const auto source = sourceValueLabel();
    switch (presetIndex) {
        case InspectionRaw: return inspectionSourceToRawMatrix().has_value() && source != "Raw";
        case InspectionRad: return source != "Rad";
        case InspectionSRgb: return source != "sRGB";
        case InspectionXyz: return source != "XYZ";
        case InspectionLuminance: return true;
        default: return false;
    }
}

string ImageViewer::inspectionPresetLabel(int presetIndex) const {
    switch (presetIndex) {
        case InspectionSource: return sourceValueLabel();
        case InspectionRaw: return "Raw";
        case InspectionRad: return "Rad";
        case InspectionSRgb: return "sRGB";
        case InspectionXyz: return "XYZ";
        case InspectionLuminance: return "Luminance";
#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
        case InspectionL: return "L";
        case InspectionM: return "M";
        case InspectionRod: return "Rod";
        case InspectionLPlusM: return "L+M";
        case InspectionAdaptation: return "Adaptation";
        case InspectionDetectableContrast: return "Detectable contrast";
        case InspectionEqvLuminance: return "Eqv_Luminance";
#endif
        default: return "Values";
    }
}

void ImageViewer::refreshInspectionModeUi() {
    for (size_t i = 0; i < mInspectionPresetButtons.size(); ++i) {
        // The first entry shows the file's own values; mark it so it is not mistaken for
        // the conversion of the same name further down the list.
        const string label = inspectionPresetLabel((int)i);
        mInspectionPresetButtons[i]->set_caption((int)i == InspectionSource ? sourceColorCaption() : label);
        mInspectionPresetButtons[i]->set_enabled(inspectionPresetSupported((int)i));
    }

    if (!inspectionPresetSupported(mInspectionPreset)) {
        mInspectionPreset = InspectionSource;
    }
    for (size_t i = 0; i < mInspectionPresetButtons.size(); ++i) {
        mInspectionPresetButtons[i]->set_pushed((int)i == mInspectionPreset);
    }

    if (mValueModePopupButton) {
        if (mSidebarOnRight) {
            mValueModePopupButton->set_caption(mInspectionPreset == InspectionSource
                ? sourceColorCaption()
                : inspectionPresetLabel(mInspectionPreset));
            mValueModePopupButton->set_tooltip(format(
                "Choose how hover values, histogram, and ROI statistics are shown. Current: {}.",
                mInspectionPreset == InspectionSource ? sourceColorCaption() : inspectionPresetLabel(mInspectionPreset)
            ));
        } else {
            mValueModePopupButton->set_caption(inspectionPresetLabel(mInspectionPreset));
        }
    }

    {
        const bool assignable = canAssignSourceColor();
        const int assignedKind = currentAssignedColorKind();
        if (mAssignColorSpacer) mAssignColorSpacer->set_visible(assignable);
        if (mAssignColorHeading) mAssignColorHeading->set_visible(assignable);
        for (size_t i = 0; i < mAssignColorButtons.size(); ++i) {
            mAssignColorButtons[i]->set_visible(assignable);
            mAssignColorButtons[i]->set_pushed(i < mAssignColorButtonKinds.size() && mAssignColorButtonKinds[i] == assignedKind);
        }
        if (mValueModePopupButton && m_nvg_context) {
            auto* popup = mValueModePopupButton->popup();
            popup->set_size(popup->preferred_size(m_nvg_context));
            popup->perform_layout(m_nvg_context);
        }
    }

    refreshGroupButtonCaptions();
}

void ImageViewer::applyInspectionPreset(int presetIndex) {
    if (!inspectionPresetSupported(presetIndex)) {
        presetIndex = InspectionSource;
    }

    if (mInspectionPreset != presetIndex) {
        mHighlightModeSelection = 0;
    }

    mInspectionPreset = presetIndex;
    for (size_t i = 0; i < mInspectionPresetButtons.size(); ++i) {
        mInspectionPresetButtons[i]->set_pushed((int)i == mInspectionPreset);
    }
    if (mInspectionPreset != InspectionSource) {
        if (const auto preferredGroup = preferredInspectionGroup()) {
            mImageCanvas->setRequestedChannelGroup(*preferredGroup);
        } else {
            mImageCanvas->setRequestedChannelGroup(mCurrentGroup);
        }
    } else {
        mImageCanvas->setRequestedChannelGroup(mCurrentGroup);
    }
    syncHighlightChannelGroup();
    syncInspectionDisplayChannelSelection();
    // Radiance pictures store Rad, sRGB and XYZ values in Radiance units (cd/m² / 179), so XYZ
    // and luminance read-outs are scaled by 179, unless the values are marked as cd/m².
    const std::string sourceLabel = sourceValueLabel();
    const bool useRadianceScale = currentFileIsRadiance() && !sourceValuesInCdm2() &&
        (sourceLabel == "Rad" || sourceLabel == "sRGB" || sourceLabel == "XYZ") &&
        (presetIndex == InspectionXyz || presetIndex == InspectionLuminance);
    mImageCanvas->clearInspectionPreviewImage();
    mImageCanvas->setInspectionMatrixOverride(Matrix3f{1.0f});
    mImageCanvas->setInspectionValueMode(EInspectionValueMode::Channels);
    mImageCanvas->setDisplayInspectionInPreview(presetIndex != InspectionSource);
    setInspectionTransfer(ituth273::ETransfer::Linear);
    setInspectionAdaptWhitePoint(false);
    setRadiance179Preview(useRadianceScale);

    switch (presetIndex) {
        case InspectionSource: break;
        case InspectionRaw:
            if (const auto rawMatrix = inspectionSourceToRawMatrix()) {
                mImageCanvas->setInspectionMatrixOverride(*rawMatrix);
            }
            break;
        case InspectionRad:
            mImageCanvas->setInspectionMatrixOverride(inspectionSourceToRadMatrix());
            break;
        case InspectionSRgb:
            mImageCanvas->setInspectionMatrixOverride(inspectionSourceToRec709Matrix());
            break;
        case InspectionXyz:
            mImageCanvas->setInspectionMatrixOverride(inspectionSourceToXyzMatrix());
            break;
        case InspectionLuminance:
            mImageCanvas->setInspectionMatrixOverride(inspectionSourceToXyzMatrix());
            mImageCanvas->setInspectionValueMode(EInspectionValueMode::YChannel);
            break;
#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
        case InspectionL:
        case InspectionM:
        case InspectionRod:
        case InspectionLPlusM:
        case InspectionAdaptation:
        case InspectionDetectableContrast:
        case InspectionEqvLuminance:
            if (const auto previewImage = experimentalPerceptualPreviewImage(presetIndex)) {
                mImageCanvas->setInspectionPreviewImage(previewImage);
                // Preview images are synthesized as tev RGB channels, whose grouped
                // channel name is "R,G,B" rather than the UI label "RGB".
                mImageCanvas->setRequestedChannelGroup("R,G,B");
                mImageCanvas->setInspectionValueMode(EInspectionValueMode::Luminance);
                setRadiance179Preview(false);
            } else {
                showErrorDialog("Experimental perceptual map could not be generated for the current image.");
                applyInspectionPreset(InspectionSource);
                return;
            }
            break;
#endif
        default: break;
    }

    if (mValueModePopupButton) {
        if (mSidebarOnRight) {
            mValueModePopupButton->set_caption(presetIndex == InspectionSource
                ? sourceColorCaption()
                : inspectionPresetLabel(presetIndex));
            mValueModePopupButton->set_tooltip(format(
                "Choose how hover values, histogram, and ROI statistics are shown. Current: {}.",
                presetIndex == InspectionSource ? sourceColorCaption() : inspectionPresetLabel(presetIndex)
            ));
        } else {
            mValueModePopupButton->set_caption(inspectionPresetLabel(presetIndex));
        }
    }

    updateHighlightModeUi();
    refreshGroupButtonCaptions();
    applyHighlightRangeFromUi();
    redraw();
}

float ImageViewer::radiometricInspectionScale() const {
    float scale = sourceExposureCompensationScale().value_or(1.0f);
    if (mRadiance179Preview) {
        scale *= 179.0f;
    }
    return scale;
}

optional<float> ImageViewer::sourceExposureCompensationScale() const {
    if (!mCurrentImage) {
        return nullopt;
    }

    return parseExposureCompensationScale(mCurrentImage->attributes());
}

void ImageViewer::syncCropUiFromCanvas() {
    if (!mCropLeftBox || !mCropTopBox || !mCropWidthBox || !mCropHeightBox) {
        return;
    }

    const auto& crop = mImageCanvas->crop();
    if (!crop.has_value()) {
        mCropLeftBox->set_value(0);
        mCropTopBox->set_value(0);
        mCropWidthBox->set_value(0);
        mCropHeightBox->set_value(0);
        return;
    }

    const auto size = crop->size();
    mCropLeftBox->set_value(crop->min.x());
    mCropTopBox->set_value(crop->min.y());
    mCropWidthBox->set_value(size.x());
    mCropHeightBox->set_value(size.y());
}

void ImageViewer::setCropUi(const std::optional<Box2i>& crop) {
    if (!mCropLeftBox || !mCropTopBox || !mCropWidthBox || !mCropHeightBox) {
        return;
    }

    if (!crop.has_value()) {
        mCropLeftBox->set_value(0);
        mCropTopBox->set_value(0);
        mCropWidthBox->set_value(0);
        mCropHeightBox->set_value(0);
        return;
    }

    const auto size = crop->size();
    mCropLeftBox->set_value(crop->min.x());
    mCropTopBox->set_value(crop->min.y());
    mCropWidthBox->set_value(size.x());
    mCropHeightBox->set_value(size.y());
}

void ImageViewer::refreshProjectionUi(bool resetSelection) {
    if (!mProjectionComboBox) {
        return;
    }

    const ProjectionMetadata metadata = detectProjectionMetadata(mCurrentImage);
    if (resetSelection) {
        mProjectionSelection = projectionSelectionIndex(metadata.projection);
    }

    mProjectionSelection = clamp(mProjectionSelection, 0, 2);
    mProjectionComboBox->set_selected_index(mProjectionSelection);
    mProjectionComboBox->set_enabled(mCurrentImage != nullptr);
    mProjectionComboBox->set_tooltip(format(
        "Detected: {}{}{}",
        projectionName(metadata.projection),
        metadata.cropped ? ", cropped" : "",
        metadata.square ? ", square" : ""
    ));

    if (!mProjectionApplyButton) {
        return;
    }

    const EDetectedProjection target = projectionFromSelectionIndex(mProjectionSelection);
    const bool canConvert = mCurrentImage &&
        !mCurrentImage->path().empty() &&
        metadata.cropped &&
        metadata.square &&
        metadata.projection == EDetectedProjection::Equisolid &&
        target == EDetectedProjection::Equidistant;
    mProjectionApplyButton->set_enabled(canConvert);
}

void ImageViewer::applyCropFromUi() {
    if (!mCropLeftBox || !mCropTopBox || !mCropWidthBox || !mCropHeightBox) {
        return;
    }

    const int left = mCropLeftBox->value();
    const int top = mCropTopBox->value();
    const int width = mCropWidthBox->value();
    const int height = mCropHeightBox->value();
    if (width <= 0 || height <= 0) {
        mImageCanvas->setCrop(nullopt);
        syncCropUiFromCanvas();
        return;
    }

    mImageCanvas->setCrop(Box2i{{left, top}, {left + width, top + height}});
    redraw();
}

fs::path ImageViewer::suggestedCroppedImagePath() const {
    if (!mCurrentImage) {
        return {};
    }

    const fs::path imagePath = mCurrentImage->path();
    const fs::path directory = imagePath.empty() ? fs::current_path() : imagePath.parent_path();
    const std::string stem = imagePath.empty() ? sanitizedStem(mCurrentImage->name()) : sanitizedStem(imagePath.stem().string());

    fs::path candidate = directory / (stem + "_cropped.hdr");
    int suffix = 2;
    while (fs::exists(candidate)) {
        candidate = directory / (stem + "_cropped_" + std::to_string(suffix) + ".hdr");
        ++suffix;
    }

    return candidate;
}

void ImageViewer::saveCurrentCropAsHdr(const fs::path& path) {
    if (!mCurrentImage) {
        throw runtime_error{"No image selected to crop."};
    }

    if (!mImageCanvas->crop().has_value() || !mImageCanvas->cropInImageCoords().isValid()) {
        throw runtime_error{"Set a valid crop before saving."};
    }

    const auto crop = mImageCanvas->cropInImageCoords();
    if (mCropSaveHandler && !mCurrentImage->path().empty()) {
        mCropSaveHandler(mCurrentImage->path(), path, crop);
    } else {
        mImageCanvas->saveImage(path);
    }

    mImageCanvas->setCrop(nullopt);
    syncCropUiFromCanvas();
    mImagesLoader->enqueue(path, "", true);
}

fs::path ImageViewer::suggestedProjectedImagePath(std::string_view targetProjection) const {
    if (!mCurrentImage) {
        return {};
    }

    const fs::path imagePath = mCurrentImage->path();
    const fs::path directory = imagePath.empty() ? fs::current_path() : imagePath.parent_path();
    const std::string stem = imagePath.empty() ? sanitizedStem(mCurrentImage->name()) : sanitizedStem(imagePath.stem().string());
    const std::string suffix = targetProjection.empty() ? "projected" : std::string{targetProjection};

    fs::path candidate = directory / (stem + "_" + suffix + ".hdr");
    int index = 2;
    while (fs::exists(candidate)) {
        candidate = directory / (stem + "_" + suffix + "_" + std::to_string(index) + ".hdr");
        ++index;
    }

    return candidate;
}

void ImageViewer::saveCurrentProjectionAsHdr(const fs::path& path, const std::string& fromProjection, const std::string& toProjection) {
    if (!mCurrentImage) {
        throw runtime_error{"No image selected to reproject."};
    }
    if (mCurrentImage->path().empty()) {
        throw runtime_error{"Current image does not have a file path."};
    }
    if (!mProjectionSaveHandler) {
        throw runtime_error{"Projection save handler is not configured."};
    }

    mProjectionSaveHandler(mCurrentImage->path(), path, fromProjection, toProjection);
    mImagesLoader->enqueue(path, "", true);
}

void ImageViewer::maybeOfferProjectionSave() {
    if (!mCurrentImage || !mProjectionComboBox) {
        return;
    }

    const ProjectionMetadata metadata = detectProjectionMetadata(mCurrentImage);
    const EDetectedProjection target = projectionFromSelectionIndex(mProjectionSelection);
    if (metadata.projection == target) {
        return;
    }

    if (!(metadata.cropped && metadata.square && metadata.projection == EDetectedProjection::Equisolid &&
            target == EDetectedProjection::Equidistant)) {
        showErrorDialog("Only cropped square equisolid HDR images can be converted to equidistant right now.");
        refreshProjectionUi();
        return;
    }

    const std::string fromProjection = projectionName(metadata.projection);
    const std::string toProjection = projectionName(target);
    const fs::path outputPath = suggestedProjectedImagePath(toProjection);
    auto* dialog = new MessageDialog(
        this,
        MessageDialog::Type::Question,
        "Save projected HDR",
        format("Write projected HDR to\n{}\nand add it to the image list?", outputPath.string()),
        "Yes",
        "No",
        true
    );
    dialog->set_callback([this, outputPath, fromProjection, toProjection](int result) {
        if (result != 0) {
            refreshProjectionUi();
            return;
        }

        try {
            saveCurrentProjectionAsHdr(outputPath, fromProjection, toProjection);
        } catch (const std::exception& e) {
            showErrorDialog(format("Failed to save projected HDR: {}", e.what()));
        }
        refreshProjectionUi();
    });
}

void ImageViewer::maybeOfferCropSave() {
    if (!mCurrentImage) {
        return;
    }

    if (!mImageCanvas->crop().has_value() || !mImageCanvas->cropInImageCoords().isValid()) {
        return;
    }

    const fs::path outputPath = suggestedCroppedImagePath();
    auto* dialog = new MessageDialog(
        this,
        MessageDialog::Type::Question,
        "Save cropped HDR",
        format("Write cropped HDR to\n{}\nand add it to the image list?", outputPath.string()),
        "Yes",
        "No",
        true
    );
    dialog->set_callback([this, outputPath](int result) {
        if (result != 0) {
            return;
        }

        try {
            saveCurrentCropAsHdr(outputPath);
        } catch (const ImageSaveError& e) {
            showErrorDialog(format("Failed to save cropped HDR: {}", e.what()));
        } catch (const std::exception& e) {
            showErrorDialog(format("Failed to save cropped HDR: {}", e.what()));
        }
    });
}

void ImageViewer::applyHighlightRangeFromUi() {
    if (!mHighlightEnableButton || !mHighlightMinBox || !mHighlightMaxBox) {
        return;
    }

    syncHighlightChannelGroup();
    mImageCanvas->setHighlightMode(selectedHighlightMode());

    if (!mHighlightEnableButton->pushed()) {
        mImageCanvas->setHighlightValueRange(std::nullopt);
        redraw();
        return;
    }

    float minValue = mHighlightMinBox->value();
    float maxValue = mHighlightMaxBox->value();
    if (maxValue < minValue) {
        std::swap(minValue, maxValue);
        mHighlightMinBox->set_value(minValue);
        mHighlightMaxBox->set_value(maxValue);
    }

    mImageCanvas->setHighlightValueRange(Vector2f{minValue, maxValue});
    redraw();
}

void ImageViewer::applyFalseColorSettingsFromUi() {
    if (!mFalseColorColormapComboBox || !mFalseColorScaleComboBox || !mFalseColorMinBox || !mFalseColorMaxBox) {
        return;
    }

    float minValue = mFalseColorMinBox->value();
    float maxValue = mFalseColorMaxBox->value();
    if (maxValue < minValue) {
        std::swap(minValue, maxValue);
        mFalseColorMinBox->set_value(minValue);
        mFalseColorMaxBox->set_value(maxValue);
    }

    mImageCanvas->setFalseColorMap(mFalseColorColormapComboBox->selected_index() == 1 ? EFalseColorMap::Viridis : EFalseColorMap::Turbo);
    mImageCanvas->setFalseColorScaleMode(static_cast<EFalseColorScaleMode>(clamp(mFalseColorScaleComboBox->selected_index(), 0, 4)));
    mImageCanvas->setFalseColorRange(Vector2f{minValue, maxValue});
    refreshFalseColorBarUi();
    setTonemap(ETonemap::FalseColor);
    redraw();
}

void ImageViewer::refreshFalseColorBarUi() {
    auto* bar = dynamic_cast<FalseColorBarWidget*>(mFalseColorBarWidget);
    if (!bar || !mFalseColorColormapComboBox || !mFalseColorScaleComboBox || !mFalseColorMinBox || !mFalseColorMaxBox) {
        return;
    }

    const auto map = mFalseColorColormapComboBox->selected_index() == 1 ? EFalseColorMap::Viridis : EFalseColorMap::Turbo;
    const auto scaleMode = static_cast<EFalseColorScaleMode>(clamp(mFalseColorScaleComboBox->selected_index(), 0, 4));
    const Vector2f range{
        std::min(mFalseColorMinBox->value(), mFalseColorMaxBox->value()),
        std::max(mFalseColorMinBox->value(), mFalseColorMaxBox->value()),
    };

    const int tickMode = mFalseColorBarTicksComboBox ? mFalseColorBarTicksComboBox->selected_index() : 1;
    const std::string manualValues = mFalseColorBarManualTicksBox ? mFalseColorBarManualTicksBox->value() : "";
    std::vector<float> ticks = computeFalseColorBarTicks(tickMode, manualValues, scaleMode, range);

    bar->setColorMap(map);
    bar->setScaleMode(scaleMode);
    bar->setRange(range);
    bar->setTicks(std::move(ticks));
}

void ImageViewer::updateHighlightModeUi() {
    if (!mHighlightModeComboBox) {
        return;
    }

    auto items = highlightItemsForPreset(mInspectionPreset);
    if (!items.empty()) {
        items[0] = inspectionPresetLabel(mInspectionPreset);
    }
    const int newIndex = clamp(mHighlightModeSelection, 0, (int)items.size() - 1);
    mHighlightModeSelection = newIndex;
    mHighlightModeComboBox->set_items(items);
    mHighlightModeComboBox->set_selected_index(newIndex);
    mImageCanvas->setHighlightMode(selectedHighlightMode());
}

EHighlightMode ImageViewer::selectedHighlightMode() const {
    if (!mHighlightModeComboBox) {
        return EHighlightMode::Rgb;
    }

    if (mInspectionPreset == InspectionLuminance) {
        return EHighlightMode::Single;
    }

#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
    if (inspectionPresetUsesExperimentalMap(mInspectionPreset)) {
        return EHighlightMode::Single;
    }
#endif

    switch (mHighlightModeSelection) {
        case 1: return EHighlightMode::Red;
        case 2: return EHighlightMode::Green;
        case 3: return EHighlightMode::Blue;
        default: return EHighlightMode::Rgb;
    }
}

string ImageViewer::inspectionCombinedLabel() const {
    return inspectionPresetLabel(mInspectionPreset);
}

string ImageViewer::inspectionChannelLabel(size_t index) const {
    return channelInspectionLabelForPreset(mInspectionPreset, index);
}

string ImageViewer::inspectionSingleChannelLabel() const {
    if (mInspectionPreset == InspectionLuminance) {
        return "Luminance";
    }

#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
    if (inspectionPresetUsesExperimentalMap(mInspectionPreset)) {
        return inspectionPresetLabel(mInspectionPreset);
    }
#endif

    if (mInspectionPreset == InspectionSource && !mCurrentGroup.empty() && mCurrentGroup != "RGB") {
        return std::string{mCurrentGroup};
    }

    return inspectionChannelLabel(0);
}

string ImageViewer::inspectionCaptionForGroup(string_view group) const {
    if (mInspectionPreset == InspectionSource) {
        return string{group};
    }

    if (groupHasThreeColorChannels(group)) {
        return inspectionCombinedLabel();
    }

    if (const auto selection = inspectionChannelSelectionForGroup(group)) {
        if (*selection == 0) {
            return inspectionCombinedLabel();
        }

        return inspectionChannelLabel((size_t)(*selection - 1));
    }

    return string{group};
}

void ImageViewer::refreshGroupButtonCaptions() {
    if (!mCurrentImage) {
        return;
    }

    auto& buttons = mGroupButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        if (auto* button = dynamic_cast<ImageButton*>(buttons[i])) {
            button->setCaption(inspectionCaptionForGroup(groupName(i)));
        }
    }
}

string ImageViewer::sourceValueLabel() const {
    if (!mCurrentImage) {
        return "RGB";
    }

    if (const auto targetChroma = parseTargetChroma(mCurrentImage->attributes())) {
        if (sameChroma(*targetChroma, radChroma())) {
            return "Rad";
        }

        if (sameChroma(*targetChroma, rec709Chroma())) {
            return "sRGB";
        }

        if (sameChroma(*targetChroma, chroma(EWpPrimaries::CIE1931XYZ))) {
            return "XYZ";
        }

        return "RGB";
    }

    if (parseSensorToXyz(mCurrentImage->attributes()).has_value()) {
        return "Raw";
    }

    if (currentImageUsesRadiometricMonochromeFallback()) {
        return "Luminance";
    }

    return "RGB";
}

std::string ImageViewer::currentSourceValueLabel() const {
    if (currentImageUsesRadiometricMonochromeFallback()) {
        return "Luminance";
    }
    return sourceValueLabel();
}

bool ImageViewer::currentImageUsesRadiometricMonochromeFallback() const {
    if (!mCurrentImage) {
        return false;
    }

    if (mCachedSourceInterpretationImage == mCurrentImage.get() &&
        mCachedSourceInterpretationImageId == mCurrentImage->id() &&
        mCachedRadiometricMonochromeFallback.has_value()) {
        return *mCachedRadiometricMonochromeFallback;
    }

    bool result = false;
    const auto attributes = mCurrentImage->attributes();
    // A monochrome file is read as luminance only when the user assigned it (HDRSPACE_COLOR=
    // luminance); otherwise its colour space is unknown and conversions stay off.
    const auto assigned = assignedSourceColor();
    if (assigned.has_value() && *assigned == "luminance" &&
        !parseTargetChroma(attributes).has_value() &&
        !parseSensorToXyz(attributes).has_value()) {
        if (const auto preferredGroup = preferredInspectionGroup()) {
            const auto channelsSpan = mCurrentImage->channelsInGroup(*preferredGroup);
            vector<string> colorChannels;
            colorChannels.reserve(3);
            for (const auto& channelName : channelsSpan) {
                if (Channel::isAlpha(channelName)) {
                    continue;
                }

                colorChannels.push_back(channelName);
                if (colorChannels.size() == 3) {
                    break;
                }
            }

            result = imageChannelsAreMonochrome(*mCurrentImage, colorChannels);
        }
    }

    mCachedSourceInterpretationImage = mCurrentImage.get();
    mCachedSourceInterpretationImageId = mCurrentImage->id();
    mCachedRadiometricMonochromeFallback = result;
    return result;
}

void ImageViewer::invalidateSourceInterpretationCache() {
    mCachedSourceInterpretationImage = nullptr;
    mCachedSourceInterpretationImageId = -1;
    mCachedRadiometricMonochromeFallback = nullopt;
}

#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
bool ImageViewer::inspectionPresetUsesExperimentalMap(int presetIndex) const {
    switch (presetIndex) {
        case InspectionL:
        case InspectionM:
        case InspectionRod:
        case InspectionLPlusM:
        case InspectionAdaptation:
        case InspectionDetectableContrast:
        case InspectionEqvLuminance:
            return true;
        default:
            return false;
    }
}

bool ImageViewer::currentImageSupportsExperimentalPerceptualMaps() const {
    if (!mCurrentImage || mCurrentReference || !preferredInspectionGroup().has_value()) {
        return false;
    }

    std::string extension = mCurrentImage->path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    return fs::exists(mCurrentImage->path()) && (extension == ".hdr" || extension == ".pic" || extension == ".rgbe");
}

std::shared_ptr<Image> ImageViewer::experimentalPerceptualPreviewImage(int presetIndex) {
    if (!inspectionPresetUsesExperimentalMap(presetIndex) || !currentImageSupportsExperimentalPerceptualMaps()) {
        return nullptr;
    }

    if (mExperimentalInspectionPreviewImage && mExperimentalInspectionPreviewPreset == presetIndex &&
        mCurrentImage && mExperimentalInspectionPreviewSourceImageId == mCurrentImage->id()) {
        return mExperimentalInspectionPreviewImage;
    }

    mExperimentalInspectionPreviewImage = makeExperimentalPerceptualPreviewImage(mCurrentImage, presetIndex);
    if (!mExperimentalInspectionPreviewImage) {
        mExperimentalInspectionPreviewPreset = -1;
        mExperimentalInspectionPreviewSourceImageId = -1;
        return nullptr;
    }
    mExperimentalInspectionPreviewPreset = presetIndex;
    mExperimentalInspectionPreviewSourceImageId = mCurrentImage ? mCurrentImage->id() : -1;
    return mExperimentalInspectionPreviewImage;
}

std::shared_ptr<Image> ImageViewer::makeExperimentalPerceptualPreviewImage(const std::shared_ptr<Image>& source, int presetIndex) const {
    if (!source) {
        return nullptr;
    }

    PerceptualMapKind kind = PerceptualMapKind::L;
    switch (presetIndex) {
        case InspectionL: kind = PerceptualMapKind::L; break;
        case InspectionM: kind = PerceptualMapKind::M; break;
        case InspectionRod: kind = PerceptualMapKind::Rod; break;
        case InspectionLPlusM: kind = PerceptualMapKind::LPlusM; break;
        case InspectionAdaptation: kind = PerceptualMapKind::Adaptation; break;
        case InspectionDetectableContrast: kind = PerceptualMapKind::DetectableContrast; break;
        case InspectionEqvLuminance: kind = PerceptualMapKind::EqvLuminance; break;
        default: return nullptr;
    }

    PerceptualMapOptions opts;
    opts.inputPath = source->path().string();
    opts.pixelsPerDegree = mPerceptualPpd;
    opts.sensitivityCorrection = mPerceptualSensitivity;
    opts.spectralEmissionPath = mPerceptualEmissionPath;
    opts.inputColor = mPerceptualInputColor == 1 ? ViewVisibilitySummaryOptions::InputColor::Srgb
        : mPerceptualInputColor == 2 ? ViewVisibilitySummaryOptions::InputColor::Xyz
        : mPerceptualInputColor == 3 ? ViewVisibilitySummaryOptions::InputColor::XyzCdm2
                                     : ViewVisibilitySummaryOptions::InputColor::Rad;
    PerceptualMapResult result;
    try {
        result = analyzePerceptualMap(opts, kind);
    } catch (const std::exception&) {
        return nullptr;
    }
    const Vector2i size{(int)result.width, (int)result.height};
    auto interleaved = std::make_shared<PixelBuffer>(PixelBuffer::alloc<float>(result.values.size() * 3, EPixelFormat::F32));
    auto* pixels = interleaved->data<float>();
    for (size_t i = 0; i < result.values.size(); ++i) {
        const float value = result.values[i];
        const size_t base = i * 3;
        pixels[base + 0] = value;
        pixels[base + 1] = value;
        pixels[base + 2] = value;
    }

    ImageData data;
    data.channels.emplace_back("R", size, EPixelFormat::F32, EPixelFormat::F32, interleaved, 0, 3);
    data.channels.emplace_back("G", size, EPixelFormat::F32, EPixelFormat::F32, interleaved, 1, 3);
    data.channels.emplace_back("B", size, EPixelFormat::F32, EPixelFormat::F32, interleaved, 2, 3);
    data.dataWindow = Box2i{size};
    data.displayWindow = Box2i{size};
    data.updateLayers();

    fs::path previewPath = source->path().parent_path() /
        (sanitizedStem(source->path().stem().string()) + "_" + sanitizedStem(result.label) + "_perceptual_preview.hdr");
    return std::make_shared<Image>(previewPath, fs::file_time_type{}, std::move(data), "", true);
}
#endif

std::string ImageViewer::currentProjectionLabel() const {
    const ProjectionMetadata metadata = detectProjectionMetadata(mCurrentImage);
    return projectionName(metadata.projection);
}

std::string ImageViewer::currentFieldOfViewLabel() const {
    const ViewAngleMetadata metadata = detectViewAngleMetadata(mCurrentImage);
    if (!metadata.hasView) {
        return "none";
    }

    if (metadata.horizontalDegrees > 0.0 && metadata.verticalDegrees > 0.0 &&
            std::abs(metadata.horizontalDegrees - metadata.verticalDegrees) > 0.05) {
        return std::format("{} x {}", formatDegrees(metadata.horizontalDegrees), formatDegrees(metadata.verticalDegrees));
    }

    if (metadata.horizontalDegrees > 0.0) {
        return formatDegrees(metadata.horizontalDegrees);
    }

    return formatDegrees(metadata.verticalDegrees);
}

void ImageViewer::updateRoiStatsLabel(const std::shared_ptr<CanvasStatistics>& statistics) {
    if (!mRoiStatsLabel) {
        return;
    }

    if (mRoiClearButton) {
        mRoiClearButton->set_enabled(mImageCanvas && mImageCanvas->roi().has_value());
    }

    if (!statistics) {
        mRoiStatsLabel->set_caption("Right-drag in the viewer to inspect a region.");
        return;
    }

    const auto& region = statistics->region;
    const bool hasRoi = mImageCanvas && mImageCanvas->roi().has_value();
    const string regionLabel = hasRoi
        ? format("ROI {},{} {}x{}", region.min.x(), region.min.y(), region.size().x(), region.size().y())
        : format("Image {},{} {}x{}", region.min.x(), region.min.y(), region.size().x(), region.size().y());

    ostringstream stream;
    stream << regionLabel << '\n';

    if (statistics->channelMean.empty()) {
        stream << format(
            "Mean {:.5g}  Min {:.5g}  Max {:.5g}  Std {:.5g}",
            statistics->mean,
            statistics->minimum,
            statistics->maximum,
            statistics->stddev
        );
    } else if (const auto selection = inspectionChannelSelectionForGroup(mCurrentGroup);
               selection.has_value() && *selection >= 1 && statistics->channelMean.size() >= (size_t)*selection) {
        const size_t i = (size_t)(*selection - 1);
        const auto label = inspectionChannelLabel(i);
        stream << format(
            "{}  Mean {:.5g}  Min {:.5g}  Max {:.5g}  Std {:.5g}",
            label,
            statistics->channelMean[i],
            statistics->channelMinimum[i],
            statistics->channelMaximum[i],
            statistics->channelStdDev[i]
        );
    } else if (statistics->channelMean.size() == 1) {
        const auto label = inspectionSingleChannelLabel();
        stream << format(
            "{}  Mean {:.5g}  Min {:.5g}  Max {:.5g}  Std {:.5g}",
            label,
            statistics->channelMean[0],
            statistics->channelMinimum[0],
            statistics->channelMaximum[0],
            statistics->channelStdDev[0]
        );
    } else {
        for (size_t i = 0; i < statistics->channelMean.size(); ++i) {
            const auto label = inspectionChannelLabel(i);
            stream << format(
                "{} {:.5g} / {:.5g} / {:.5g} / {:.5g}",
                label,
                statistics->channelMean[i],
                statistics->channelMinimum[i],
                statistics->channelMaximum[i],
                statistics->channelStdDev[i]
            );

            if (i + 1 < statistics->channelMean.size()) {
                stream << (mSidebarOnRight ? "\n" : "  |  ");
            }
        }
    }

    mRoiStatsLabel->set_caption(stream.str());
}

void ImageViewer::updateDockedSliderWidths() {
    // Docked inspector: grey label (104) + slider on one line; the value field is the
    // label itself ("Exposure: +0.0").
    if (!mSidebarOnRight || !mSidebarLayout) {
        return;
    }
    const int sliderWidth = std::max(60, mSidebarLayout->fixed_width() - 2 * 5 - 104 - 8);
    for (Slider* slider : {mExposureSlider, mOffsetSlider, mGammaSlider}) {
        if (slider) {
            slider->set_fixed_width(sliderWidth);
        }
    }
    if (mValueModePopupButton) {
        mValueModePopupButton->set_fixed_width(std::max(60, mSidebarLayout->fixed_width() - 2 * 4 - 104 - 8));
    }
    if (mRoiStatsLabel) {
        // A fixed width makes the label wrap and honour the per-channel line breaks.
        mRoiStatsLabel->set_fixed_width(std::max(60, mSidebarLayout->fixed_width() - 2 * 5));
    }
}

void ImageViewer::setOffset(float value) {
    value = round(value, 2.0f);
    mOffsetSlider->set_value(value);
    mOffsetLabel->set_caption(format("Offset: {:+.2f}", value));

    mImageCanvas->setOffset(value);
}

void ImageViewer::setGamma(float value) {
    value = round(value, 2.0f);
    mGammaSlider->set_value(value);
    mGammaLabel->set_caption(format("Gamma: {:+.2f}", value));

    mImageCanvas->setGamma(value);
}

void ImageViewer::normalizeExposureAndOffset() {
    if (!mCurrentImage) {
        return;
    }

    const auto channels = mCurrentImage->channelsInGroup(mCurrentGroup);

    float minimum = numeric_limits<float>::max();
    float maximum = numeric_limits<float>::min();
    for (const auto& channelName : channels) {
        const auto& channel = mCurrentImage->channel(channelName);
        auto [cmin, cmax, cmean] = channel->minMaxMean();
        maximum = std::max(maximum, cmax);
        minimum = std::min(minimum, cmin);
    }

    const float factor = 1.0f / (maximum - minimum);
    setExposure(log2(factor));
    setOffset(-minimum * factor);
}

void ImageViewer::resetImage() {
    setExposure(0);
    setOffset(0);
    setGamma(2.2f);
    mImageCanvas->resetTransform();
}

void ImageViewer::ungroupCurrentChannelGroup() {
    if (mCurrentImage) {
        mCurrentImage->ungroup(mCurrentGroup);
        selectImage(mCurrentImage);
    }

    if (mCurrentReference) {
        mCurrentReference->ungroup(mCurrentGroup);
        selectReference(mCurrentReference);
    }
}

void ImageViewer::setTonemap(ETonemap tonemap) {
    mImageCanvas->setTonemap(tonemap);
    for (size_t i = 0; i < mTonemapButtons.size(); ++i) {
        if (mTonemapButtons[i]) {
            mTonemapButtons[i]->set_pushed((ETonemap)i == tonemap);
        }
    }

    if (mFalseColorPopupButton) {
        mFalseColorPopupButton->set_pushed(tonemap == ETonemap::FalseColor);
    }

    if (mFalseColorBarPopupButton) {
        mFalseColorBarPopupButton->set_pushed(tonemap == ETonemap::FalseColor);
    }

    if (mGammaSlider) {
        mGammaSlider->set_enabled(tonemap == ETonemap::Gamma);
    }

    if (mGammaLabel) {
        mGammaLabel->set_color(tonemap == ETonemap::Gamma ? mGammaLabel->theme()->m_text_color : Color{0.5f, 1.0f});
    }
}

void ImageViewer::setMetric(EMetric metric) {
    mImageCanvas->setMetric(metric);
    auto& buttons = mMetricButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        Button* b = dynamic_cast<Button*>(buttons[i]);
        b->set_pushed((EMetric)i == metric);
    }
}

void ImageViewer::setChannelMask(EChannelMask channel, bool state) {
    EChannelMask mask = mImageCanvas->channelMask();

    if (state) {
        mask |= channel;
    } else {
        mask &= ~channel;
    }

    mImageCanvas->setChannelMask(mask);
    requestLayoutUpdate();
}

void ImageViewer::setBackgroundColorStraight(Color color) {
    if (mBackgroundColorWheel) {
        mBackgroundColorWheel->set_color(color);
    }
    if (mBackgroundAlphaSlider) {
        mBackgroundAlphaSlider->set_value(color.a());
    }

    const Color premul = Color{color.r() * color.a(), color.g() * color.a(), color.b() * color.a(), color.a()};
    if (mImageCanvas) {
        mImageCanvas->setBackgroundColor(premul);
    }
}

float ImageViewer::displayWhiteLevel() const {
    return mDisplayWhiteLevelBox ? mDisplayWhiteLevelBox->value() : glfwGetWindowSdrWhiteLevel(m_glfw_window);
}

void ImageViewer::setDisplayWhiteLevel(float value) {
    if (mDisplayWhiteLevelBox) {
        mDisplayWhiteLevelBox->set_value(value);
    }

    const auto setting =
        mDisplayWhiteLevelSettingComboBox ? displayWhiteLevelSetting() : EDisplayWhiteLevelSetting::System;
    mImageCanvas->setWhiteLevelOverride(
        setting != EDisplayWhiteLevelSetting::System && value > 0.0f ? optional<float>{value} : nullopt
    );
}

void ImageViewer::setDisplayWhiteLevelToImageMetadata() {
    setDisplayWhiteLevel(mCurrentImage ? mCurrentImage->whiteLevel() : DEFAULT_IMAGE_WHITE_LEVEL);
}

void ImageViewer::setImageWhiteLevel(float value) {
    if (mImageWhiteLevelBox) {
        mImageWhiteLevelBox->set_value(value);
    }

    if (mDisplayWhiteLevelSettingComboBox && displayWhiteLevelSetting() == EDisplayWhiteLevelSetting::ImageMetadata) {
        setDisplayWhiteLevelToImageMetadata();
    }
}

ImageViewer::EDisplayWhiteLevelSetting ImageViewer::displayWhiteLevelSetting() const {
    if (!mDisplayWhiteLevelSettingComboBox) {
        return EDisplayWhiteLevelSetting::System;
    }

    return static_cast<EDisplayWhiteLevelSetting>(mDisplayWhiteLevelSettingComboBox->selected_index());
}

void ImageViewer::setDisplayWhiteLevelSetting(EDisplayWhiteLevelSetting setting) {
    if (!mDisplayWhiteLevelSettingComboBox) {
        return;
    }

    mDisplayWhiteLevelSettingComboBox->set_selected_index(static_cast<int>(setting));

    switch (displayWhiteLevelSetting()) {
        case EDisplayWhiteLevelSetting::System: setDisplayWhiteLevel(glfwGetWindowSdrWhiteLevel(m_glfw_window)); break;
        case EDisplayWhiteLevelSetting::Custom: break;
        case EDisplayWhiteLevelSetting::ImageMetadata: setDisplayWhiteLevelToImageMetadata();
    }
}

Vector2f ImageViewer::sizeToFitImage(const shared_ptr<Image>& image) {
    if (!image) {
        return m_size;
    }

    // Convert from image pixel coordinates to nanogui coordinates.
    auto requiredSize = Vector2f{image->displaySize()} / pixel_ratio();

    // Take into account the size of the UI.
    if (mHasWorkspaceInsets) {
        requiredSize += Vector2f{float(mWorkspaceInsets[0] + mWorkspaceInsets[2]),
                                 float(mWorkspaceInsets[1] + mWorkspaceInsets[3])};
    } else if (mSidebar->visible() && !mSidebarOnRight) {
        requiredSize.x() += mSidebar->fixed_width();
    }

    if (mFooter->visible()) {
        requiredSize.y() += mFooter->fixed_height();
    }

    return requiredSize;
}

Vector2f ImageViewer::sizeToFitAllImages() {
    Vector2f result = m_size;
    for (const auto& image : mImages) {
        result = max(result, sizeToFitImage(image));
    }

    return result;
}

void ImageViewer::resizeToFit(Vector2f targetSize) {
    // On Wayland, some information like the current monitor or fractional DPI scaling is not available until some time has passed.
    // Potentially a few frames have been rendered. Hence postpone resizing until we have a valid monitor.
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND && !glfwGetWindowCurrentMonitor(m_glfw_window)) {
        mDidFitToImage = 2;
        return;
    }

    // Only increase our current size if we are larger than the current size of the window.
    targetSize = max(Vector2f{m_size}, targetSize);
    // For sanity, don't make us larger than 8192x8192 to ensure that we don't break any texture size limitations of the user's GPU.

    auto maxSize = mMaxWindowSize;

    const Vector2f padding = {
#ifdef _WIN32
        2
#else
        0
#endif
    };

    maxSize -= 2 * padding;

    targetSize = min(targetSize, maxSize);
    if (targetSize == m_size) {
        return;
    }

    tlog::debug("Resizing window to {}", targetSize);

    const auto sizeDiff = targetSize - Vector2f{m_size};

    set_size(targetSize);
    move_window(-sizeDiff / 2);

    // Ensure the window does not go off-screen by clamping its position. This does not work on Wayland, because Wayland does not allow
    // windows to control their own position. On Windows, we add additional padding because, otherwise, moving the mouse to the edge of the
    // screen does not allow the user to resize the window anymore.
    if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) {
        const auto minWindowPos = Vector2i{mMinWindowPos + padding};
        const auto maxWindowPos = Vector2i{mMinWindowPos + maxSize - targetSize + padding};

        Vector2i pos;
        glfwGetWindowPos(m_glfw_window, &pos.x(), &pos.y());
        pos = min(max(pos, minWindowPos), maxWindowPos);
        glfwSetWindowPos(m_glfw_window, pos.x(), pos.y());
    }

    if (autoFitToScreen() && mCurrentImage) {
        mImageCanvas->fitImageToScreen(*mCurrentImage);
    }
}

bool ImageViewer::playingBack() const { return mPlayingBack; }

void ImageViewer::setPlayingBack(bool value) {
    mPlayingBack = value;
    if (mPlayButton) {
        mPlayButton->set_pushed(value);
    }
    mLastPlaybackFrameTime = chrono::steady_clock::now();
    set_run_mode(value ? RunMode::VSync : RunMode::Lazy);
}

bool ImageViewer::setFilter(string_view filter) {
    if (mFilter) {
        mFilter->set_value(filter);
    }
    mRequiresFilterUpdate = true;
    return true;
}

void ImageViewer::setFps(int value) {
    if (mFpsTextBox) {
        mFpsTextBox->set_value(value);
    }
}

bool ImageViewer::useRegex() const { return mRegexButton ? mRegexButton->pushed() : false; }

void ImageViewer::setUseRegex(bool value) {
    if (mRegexButton) {
        mRegexButton->set_pushed(value);
    }
    mRequiresFilterUpdate = true;
}

bool ImageViewer::watchFilesForChanges() const { return mWatchFilesForChangesButton ? mWatchFilesForChangesButton->pushed() : false; }

void ImageViewer::setWatchFilesForChanges(bool value) {
    if (mWatchFilesForChangesButton) {
        mWatchFilesForChangesButton->set_pushed(value);
    }
}

bool ImageViewer::autoFitToScreen() const { return mAutoFitToScreenButton ? mAutoFitToScreenButton->pushed() : true; }

void ImageViewer::setAutoFitToScreen(bool value) {
    if (mAutoFitToScreenButton) {
        mAutoFitToScreenButton->set_pushed(value);
    }
    if (value && mCurrentImage) {
        mImageCanvas->fitImageToScreen(*mCurrentImage);
    }
}

bool ImageViewer::resizeWindowToFitImageOnLoad() const {
    return mResizeWindowToFitImageOnLoadButton ? mResizeWindowToFitImageOnLoadButton->pushed() : true;
}

void ImageViewer::setResizeWindowToFitImageOnLoad(bool value) {
    if (mResizeWindowToFitImageOnLoadButton) {
        mResizeWindowToFitImageOnLoadButton->set_pushed(value);
    }
    if (value && mCurrentImage) {
        resizeToFit(sizeToFitImage(mCurrentImage));
    }
}

void ImageViewer::maximize() {
    glfwMaximizeWindow(m_glfw_window);
    if (autoFitToScreen() && mCurrentImage) {
        mImageCanvas->fitImageToScreen(*mCurrentImage);
    }
}

bool ImageViewer::isMaximized() { return !mMaximizedUnreliable && glfwGetWindowAttrib(m_glfw_window, GLFW_MAXIMIZED) != 0; }

void ImageViewer::toggleMaximized() {
    if (isMaximized()) {
        glfwRestoreWindow(m_glfw_window);
    } else {
        maximize();
    }
}

void ImageViewer::setUiVisible(bool shouldBeVisible) {
    if (!shouldBeVisible && mDragType == EMouseDragType::SidebarDrag) {
        mDragType = EMouseDragType::None;
    }

    mSidebar->set_visible(shouldBeVisible);
    mFooter->set_visible(mShouldFooterBeVisible && (shouldBeVisible || mHasWorkspaceInsets));

    requestLayoutUpdate();
}

void ImageViewer::setWorkspaceInsets(int left, int top, int right, int bottom) {
    const array<int, 4> insets = {std::max(0, left), std::max(0, top), std::max(0, right), std::max(0, bottom)};
    if (insets == mWorkspaceInsets) {
        return;
    }
    mWorkspaceInsets = insets;
    mHasWorkspaceInsets = std::ranges::any_of(insets, [](int value) { return value != 0; });
    mFooter->set_visible(mShouldFooterBeVisible && (mSidebar->visible() || mHasWorkspaceInsets));
    // ImageCanvas keeps its scale and center-relative pan transform. Only the
    // widget viewport changes; image coordinates, ROI and sampling stay intact.
    updateLayout();
    redraw();
}

void ImageViewer::focusCropControls() {
    if (!mCropControlsPanel || !mSidebarRoot) {
        return;
    }
    setUiVisible(true);
    updateLayout();
    if (auto* scroll = dynamic_cast<VScrollPanel*>(mSidebar)) {
        const int scrollRange = std::max(0, mSidebarRoot->preferred_size(m_nvg_context).y() - scroll->height());
        const int cropOffset = std::max(0, mCropControlsPanel->absolute_position().y() - mSidebarRoot->absolute_position().y() - 8);
        scroll->set_scroll(scrollRange > 0 ? std::clamp(float(cropOffset) / float(scrollRange), 0.0f, 1.0f) : 0.0f);
        scroll->perform_layout(m_nvg_context);
    }
    if (mCropLeftBox) {
        mCropLeftBox->request_focus();
    }
    redraw();
}

void ImageViewer::openInspectionModeMenu() {
    if (!mValueModePopupButton) {
        return;
    }
    setUiVisible(true);
    updateLayout();
    if (auto* scroll = dynamic_cast<VScrollPanel*>(mSidebar)) {
        scroll->set_scroll(0.0f);
        scroll->perform_layout(m_nvg_context);
    }
    mValueModePopupButton->set_pushed(true);
    mValueModePopupButton->perform_layout(m_nvg_context);
    mValueModePopupButton->popup()->request_focus();
    redraw();
}

void ImageViewer::closeInspectionModeMenu() {
    if (!mValueModePopupButton || !mValueModePopupButton->pushed()) {
        return;
    }
    mValueModePopupButton->set_pushed(false);
    mValueModePopupButton->popup()->set_visible(false);
    redraw();
}

void ImageViewer::closeSidebarPopups() {
    bool changed = false;
    for (PopupButton* button : {mColorsPopupButton, mFalseColorPopupButton, mFalseColorBarPopupButton, mHdrPopupButton, mValueModePopupButton}) {
        if (!button || !button->pushed()) {
            continue;
        }
        button->set_pushed(false);
        if (button->popup()) {
            button->popup()->set_visible(false);
        }
        changed = true;
    }
    if (changed) {
        redraw();
    }
}

void ImageViewer::setWorkspaceInspectorTop(int top) {
    const int inspectorTop = std::max(-1, top);
    if (mWorkspaceInspectorTop == inspectorTop) {
        return;
    }
    mWorkspaceInspectorTop = inspectorTop;
    if (mHasWorkspaceInsets) {
        updateLayout();
        redraw();
    }
}

void ImageViewer::setSidebarOverlayReveal(float reveal) {
    const float clamped = std::clamp(reveal, 0.0f, 1.0f);
    if (std::abs(mSidebarOverlayReveal - clamped) < 1e-4f) {
        return;
    }
    mSidebarOverlayReveal = clamped;
    requestLayoutUpdate();
}

void ImageViewer::toggleHelpWindow() {
    if (mHelpWindow) {
        mHelpWindow->dispose();
        mHelpWindow = nullptr;
        mHelpButton->set_pushed(false);
    } else {
        mHelpWindow = new HelpWindow{this, ipc(), [this] { toggleHelpWindow(); }};
        mHelpWindow->center();
        mHelpWindow->request_focus();
        mHelpButton->set_pushed(true);
    }

    requestLayoutUpdate();
}

void ImageViewer::toggleImageInfoWindow() {
    if (mImageInfoWindow) {
        mImageInfoWindow->dispose();
        mImageInfoWindow = nullptr;

        if (mImageInfoButton) {
            mImageInfoButton->set_pushed(false);
        }
    } else {
        if (mCurrentImage) {
            mImageInfoWindow = new ImageInfoWindow{this, mCurrentImage, [this] { toggleImageInfoWindow(); }};
            mImageInfoWindow->center();
            mImageInfoWindow->request_focus();

            if (mImageInfoButton) {
                mImageInfoButton->set_pushed(true);
            }
        }
    }

    requestLayoutUpdate();
}

void ImageViewer::updateImageInfoWindow() {
    if (mImageInfoWindow) {
        const auto pos = mImageInfoWindow->position();
        const auto size = mImageInfoWindow->size();
        const string tabName = string{mImageInfoWindow->currentTabName()};
        const float scroll = mImageInfoWindow->currentScroll();
        mImageInfoWindow->dispose();

        if (mCurrentImage) {
            mImageInfoWindow = new ImageInfoWindow{this, mCurrentImage, [this] { toggleImageInfoWindow(); }};
            mImageInfoWindow->set_position(pos);
            mImageInfoWindow->set_size(size);
            if (mImageInfoWindow->selectTabWithName(tabName)) {
                mImageInfoWindow->setScroll(scroll);
            }

            mImageInfoWindow->request_focus();

            if (mImageInfoButton) {
                mImageInfoButton->set_pushed(true);
            }
        } else {
            mImageInfoWindow = nullptr;
            if (mImageInfoButton) {
                mImageInfoButton->set_pushed(false);
            }
        }
    }
}

void ImageViewer::openImageDialog() {
    if (mFileDialogThread) {
        tlog::warning("File dialog already running.");
        return;
    }

    const auto runDialog = [this]() {
        const auto threadGuard = ScopeGuard{[this]() {
            scheduleToUiThread([this]() {
                focusWindow();
                if (mFileDialogThread && mFileDialogThread->joinable()) {
                    mFileDialogThread->join();
                }

                mFileDialogThread = nullptr;
            });
        }};

        try {
            vector<pair<string, string>> filters = {
                {"apng",                    "Animated PNG image"               },
#ifdef TEV_SUPPORT_AVIF
                {"avif",                    "AV1 Image File"                   },
#endif
                {"bmp",                     "Bitmap image"                     },
                {"cur",                     "Microsoft cursor image"           },
#ifdef _WIN32
                {"dds",                     "DirectDraw Surface image"         },
#endif
                {"dng",                     "Digital Negative image"           },
                {"exr",                     "OpenEXR image"                    },
                {"gif",                     "Graphics Interchange Format image"},
                {"hdr",                     "HDR image"                        },
#ifdef TEV_SUPPORT_HEIC
                {"heic",                    "High Efficiency Image Container"  },
#endif
                {"ico",                     "Microsoft icon image"             },
                {"jpeg,jpg",                "JPEG image"                       },
                {"jxl",                     "JPEG-XL image"                    },
                {"pam,pbm,pfm,pgm,pnm,ppm", "Portable *Map image"              },
                {"pic",                     "PIC image"                        },
                {"png",                     "Portable Network Graphics image"  },
                {"psd",                     "PSD image"                        },
                {"qoi",                     "Quite OK Image format"            },
                {"tga",                     "Truevision TGA image"             },
                {"tiff,tif",                "Tag Image File Format image"      },
                {"webp",                    "WebP image"                       },
            };

            vector<string_view> allImages;
            for (const auto& filter : filters) {
                allImages.push_back(filter.first);
            }

            filters.emplace(filters.begin(), pair<string, string>{join(allImages, ","), "All images"});
            const auto paths = file_dialog(this, FileDialogType::OpenMultiple, filters);

            for (size_t i = 0; i < paths.size(); ++i) {
                const bool shallSelect = i == paths.size() - 1;
                mImagesLoader->enqueue(paths[i], "", shallSelect);
            }
        } catch (const runtime_error& e) {
            const auto error = format("File dialog: {}", e.what());
            scheduleToUiThread([this, error]() { showErrorDialog(error); });
        }
    };

#if defined(__APPLE__) || defined(_WIN32)
    runDialog();
#else
    mFileDialogThread = make_unique<thread>(runDialog);
#endif
}

void ImageViewer::saveImageDialog() {
    if (!mCurrentImage) {
        return;
    }

    if (mFileDialogThread) {
        tlog::warning("File dialog already running.");
        return;
    }

    const auto runDialog = [this]() {
        const auto threadGuard = ScopeGuard{[this]() {
            scheduleToUiThread([this]() {
                focusWindow();
                if (mFileDialogThread && mFileDialogThread->joinable()) {
                    mFileDialogThread->join();
                }

                mFileDialogThread = nullptr;
            });
        }};

        try {
            const auto paths = file_dialog(
                this,
                FileDialogType::Save,
                {
                    {"exr",      "OpenEXR image"                  },
                    {"hdr",      "HDR image"                      },
                    {"bmp",      "Bitmap Image File"              },
                    {"jpg,jpeg", "JPEG image"                     },
                    {"jxl",      "JPEG-XL image"                  },
                    {"png",      "Portable Network Graphics image"},
                    {"qoi",      "Quite OK Image format"          },
                    {"tga",      "Truevision TGA image"           },
            }
            );

            if (paths.empty() || paths.front().empty()) {
                return;
            }

            scheduleToUiThread([this, path = paths.front()]() {
                try {
                    mImageCanvas->saveImage(path);
                } catch (const ImageSaveError& e) { showErrorDialog(format("Failed to save image: {}", e.what())); }
            });
        } catch (const runtime_error& e) {
            const auto error = format("Save dialog: {}", e.what());
            scheduleToUiThread([this, error]() { showErrorDialog(error); });
        }
    };

#if defined(__APPLE__) || defined(_WIN32)
    runDialog();
#else
    mFileDialogThread = make_unique<thread>(runDialog);
#endif
}

void ImageViewer::copyImageCanvasToClipboard() const {
    if (!mCurrentImage) {
        throw runtime_error{"No image selected for copy."};
    }

    const auto imageSize = mImageCanvas->imageDataSize();
    if (imageSize.x() == 0 || imageSize.y() == 0) {
        throw runtime_error{"Image canvas has no image data to copy to clipboard."};
    }

    const auto start = chrono::steady_clock::now();

    const auto imageData = mImageCanvas->getRgbaLdrImageData(true, numeric_limits<int>::max()).get();

#if defined(__APPLE__) or defined(_WIN32)
    const clip::image image(
        imageData.data(),
        clip::image_spec{
            .width = (unsigned long)imageSize.x(),
            .height = (unsigned long)imageSize.y(),
            .bits_per_pixel = 32,
            .bytes_per_row = 4 * (unsigned long)imageSize.x(),
            .red_mask = 0x000000ff,
            .green_mask = 0x0000ff00,
            .blue_mask = 0x00ff0000,
            .alpha_mask = 0xff000000,
            .red_shift = 0,
            .green_shift = 8,
            .blue_shift = 16,
            .alpha_shift = 24
        }
    );

    if (!clip::set_image(image)) {
        throw runtime_error{"clip::set_image failed."};
    }
#else
    const auto pngImageSaver = make_unique<PngImageSaver>();

    ostringstream pngData;
    try {
        pngImageSaver->save(pngData, "clipboard.png", imageData, imageSize, 4).get();
    } catch (const ImageSaveError& e) { throw runtime_error{format("Failed to save image data to clipboard as PNG: {}", e.what())}; }

    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        waylandSetClipboardPngImage(pngData.view());
    } else if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
        clip::lock l;
        if (!l.locked()) {
            throw runtime_error{"Failed to lock clipboard."};
        }

        l.clear();
        if (!l.set_data(clip::image_format(), pngData.view().data(), pngData.view().size())) {
            throw runtime_error{"Failed to set image data to clipboard."};
        }
    }
#endif
    const auto end = chrono::steady_clock::now();
    const auto duration = chrono::duration<float>(end - start).count();

    tlog::success("Image copied to clipboard after {:.3f} seconds.", duration);
}

void ImageViewer::copyFalseColorBarToClipboard() const {
    const auto* bar = dynamic_cast<FalseColorBarWidget*>(mFalseColorBarWidget);
    if (!bar) {
        throw runtime_error{"Falsecolor bar is not available."};
    }

    constexpr int width = 760;
    constexpr int height = 124;
    constexpr int x0 = 28;
    constexpr int x1 = width - 28;
    constexpr int y0 = 18;
    constexpr int y1 = 48;

    std::vector<uint8_t> imageData((size_t)width * (size_t)height * 4, 0);
    auto writePixel = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return;
        }

        const size_t index = ((size_t)y * (size_t)width + (size_t)x) * 4;
        imageData[index + 0] = r;
        imageData[index + 1] = g;
        imageData[index + 2] = b;
        imageData[index + 3] = a;
    };

    auto glyphRows = [](char c) -> std::array<std::string_view, 7> {
        switch (c) {
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
            case 'e': return {"     ", " ### ", "#   #", "#####", "#    ", "#   #", " ### "};
            case 'E': return {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"};
            default: return {"     ", "     ", "     ", "     ", "     ", "     ", "     "};
        }
    };

    auto textWidth = [&](std::string_view text) {
        if (text.empty()) {
            return 0;
        }
        return (int)text.size() * 6 - 1;
    };

    auto drawTextBitmap = [&](int x, int y, std::string_view text, uint8_t r, uint8_t g, uint8_t b) {
        int penX = x;
        for (char c : text) {
            const auto rows = glyphRows(c);
            for (int row = 0; row < (int)rows.size(); ++row) {
                for (int col = 0; col < (int)rows[row].size(); ++col) {
                    if (rows[row][col] != ' ') {
                        writePixel(penX + col, y + row, r, g, b, 255);
                    }
                }
            }
            penX += 6;
        }
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            writePixel(x, y, 18, 18, 18, 255);
        }
    }

    const auto colormap = bar->colorMap() == EFalseColorMap::Viridis ? colormap::viridis() : colormap::turbo();
    for (int x = x0; x < x1; ++x) {
        const float t = (float)(x - x0) / (float)std::max(1, x1 - x0 - 1);
        const int colorIndex = std::clamp((int)(t * (float)(colormap.size() / 4)), 0, (int)colormap.size() / 4 - 1) * 4;
        const uint8_t r = (uint8_t)std::clamp((int)std::lround(colormap[colorIndex + 0] * 255.0f), 0, 255);
        const uint8_t g = (uint8_t)std::clamp((int)std::lround(colormap[colorIndex + 1] * 255.0f), 0, 255);
        const uint8_t b = (uint8_t)std::clamp((int)std::lround(colormap[colorIndex + 2] * 255.0f), 0, 255);
        for (int y = y0; y < y1; ++y) {
            writePixel(x, y, r, g, b, 255);
        }
    }

    for (int x = x0; x < x1; ++x) {
        writePixel(x, y0, 255, 255, 255, 255);
        writePixel(x, y1 - 1, 255, 255, 255, 255);
    }
    for (int y = y0; y < y1; ++y) {
        writePixel(x0, y, 255, 255, 255, 255);
        writePixel(x1 - 1, y, 255, 255, 255, 255);
    }

    for (float tick : bar->ticks()) {
        const float t = falseColorCoordForUi(tick, bar->scaleMode(), bar->range());
        const int x = (int)std::lround((float)x0 + t * (float)(x1 - x0 - 1));
        for (int y = y1; y < y1 + 10; ++y) {
            writePixel(x, y, 255, 255, 255, 255);
            writePixel(x - 1, y, 120, 120, 120, 255);
            writePixel(x + 1, y, 120, 120, 120, 255);
        }

        const std::string label = std::format("{:.5g}", tick);
        const int labelWidth = textWidth(label);
        const int labelX = clamp(x - labelWidth / 2, x0, x1 - labelWidth);
        drawTextBitmap(labelX, y1 + 16, label, 235, 235, 235);
    }

#if defined(__APPLE__) || defined(_WIN32)
    const clip::image image(
        imageData.data(),
        clip::image_spec{
            .width = (unsigned long)width,
            .height = (unsigned long)height,
            .bits_per_pixel = 32,
            .bytes_per_row = 4UL * (unsigned long)width,
            .red_mask = 0x000000ff,
            .green_mask = 0x0000ff00,
            .blue_mask = 0x00ff0000,
            .alpha_mask = 0xff000000,
            .red_shift = 0,
            .green_shift = 8,
            .blue_shift = 16,
            .alpha_shift = 24
        }
    );

    if (!clip::set_image(image)) {
        throw runtime_error{"clip::set_image failed."};
    }
#else
    const auto pngImageSaver = make_unique<PngImageSaver>();
    ostringstream pngData;
    try {
        pngImageSaver->save(pngData, "falsecolor-bar.png", imageData, Vector2i{width, height}, 4).get();
    } catch (const ImageSaveError& e) { throw runtime_error{format("Failed to save falsecolor bar to clipboard as PNG: {}", e.what())}; }

    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        waylandSetClipboardPngImage(pngData.view());
    } else if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
        clip::lock l;
        if (!l.locked()) {
            throw runtime_error{"Failed to lock clipboard."};
        }

        l.clear();
        if (!l.set_data(clip::image_format(), pngData.view().data(), pngData.view().size())) {
            throw runtime_error{"Failed to set image data to clipboard."};
        }
    }
#endif

    tlog::success("Falsecolor bar copied to clipboard.");
}

void ImageViewer::copyImageNameToClipboard() const {
    if (!mCurrentImage) {
        throw runtime_error{"No image selected for copy."};
    }

    glfwSetClipboardString(m_glfw_window, string{mCurrentImage->name()}.c_str());
    tlog::success("Image name copied to clipboard.");
}

void ImageViewer::pasteImagesFromClipboard() {
    stringstream imageStream;
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        const auto data = waylandGetClipboardPngImage();
        if (data.empty()) {
            throw runtime_error{"No image data found in clipboard."};
        }

        // TODO: use spanstream once it is available in C++23 to avoid the copy
        imageStream = stringstream{string{data}, ios::in};
    } else if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
        clip::lock l;
        if (!l.locked()) {
            throw runtime_error{"Failed to lock clipboard."};
        }

        clip::format f = clip::image_format();
        if (!l.is_convertible(f)) {
            throw runtime_error{"Clipboard does not contain image data."};
        }

        const size_t len = l.get_data_length(f);
        string data(len, '\0');
        l.get_data(f, data.data(), len);

        imageStream = stringstream{std::move(data), ios::in};
    } else {
        clip::image clipImage;
        if (!clip::get_image(clipImage)) {
            throw runtime_error{"No image data found in clipboard."};
        }

        imageStream << "clip";
        imageStream.write(reinterpret_cast<const char*>(&clipImage.spec()), sizeof(clip::image_spec));
        imageStream.write(clipImage.data(), clipImage.spec().bytes_per_row * clipImage.spec().height);
    }

    tlog::info("Loading image from clipboard...");

    const auto name = format("clipboard ({})", ++mClipboardIndex);
    const auto images = tryLoadImage(name, imageStream, "", mImagesLoader->imageLoaderSettings(), mImagesLoader->groupChannels()).get();

    if (images.empty()) {
        throw runtime_error{"Failed to load image from clipboard data."};
    } else {
        for (auto& image : images) {
            addImage(image, true);
        }
    }
}

void ImageViewer::showErrorDialog(string_view message) {
    tlog::error(message);
    new MessageDialog(this, MessageDialog::Type::Warning, "Error", message);
}

chroma_t ImageViewer::inspectionChroma() const {
    if (!mInspectionPrimariesComboBox || mInspectionPrimariesBoxes.size() != 8) {
        return mImageCanvas->inspectionChroma();
    }

    chroma_t chr;
    for (size_t i = 0; i < chr.size(); ++i) {
        for (size_t c = 0; c < 2; ++c) {
            chr[i][c] = mInspectionPrimariesBoxes.at(i * 2 + c)->value();
        }
    }

    return chr;
}

void ImageViewer::setInspectionChroma(const chroma_t& chr) {
    mImageCanvas->setInspectionChroma(chr);

    if (!mInspectionPrimariesComboBox || mInspectionPrimariesBoxes.size() != 8) {
        return;
    }

    for (size_t i = 0; i < chr.size(); ++i) {
        for (size_t c = 0; c < 2; ++c) {
            mInspectionPrimariesBoxes.at(i * 2 + c)->set_value(chr[i][c]);
        }
    }

    for (size_t i = 0; i < PRIMARIES.size(); ++i) {
        if (chr == chroma(PRIMARIES.at(i).first)) {
            mInspectionPrimariesComboBox->set_selected_index((int)i);
            return;
        }
    }

    // "Custom"
    mInspectionPrimariesComboBox->set_selected_index(PRIMARIES.size());
}

ituth273::ETransfer ImageViewer::inspectionTransfer() const {
    if (!mInspectionTransferComboBox) {
        return mImageCanvas->inspectionTransfer();
    }

    const size_t index = (size_t)mInspectionTransferComboBox->selected_index();
    TEV_ASSERT(index <= TRANSFERS.size(), "Invalid transfer function index selected for inspection.");

    return TRANSFERS.at(index).first;
}

void ImageViewer::setInspectionTransfer(const ituth273::ETransfer transfer) {
    mImageCanvas->setInspectionTransfer(transfer);

    if (!mInspectionTransferComboBox) {
        return;
    }

    for (size_t i = 0; i < TRANSFERS.size(); ++i) {
        if (transfer == TRANSFERS.at(i).first) {
            mInspectionTransferComboBox->set_selected_index((int)i);
            return;
        }
    }

    TEV_ASSERT(false, "Invalid transfer function specified for inspection.");
}

bool ImageViewer::inspectionAdaptWhitePoint() const {
    return mInspectionAdaptWhitePointButton ? mInspectionAdaptWhitePointButton->pushed() : mImageCanvas->inspectionAdaptWhitePoint();
}

void ImageViewer::setInspectionAdaptWhitePoint(bool value) {
    mImageCanvas->setInspectionAdaptWhitePoint(value);
    if (mInspectionAdaptWhitePointButton) {
        mInspectionAdaptWhitePointButton->set_pushed(value);
    }
}

bool ImageViewer::inspectionPremultipliedAlpha() const {
    return mInspectionPremultipliedAlphaButton ? mInspectionPremultipliedAlphaButton->pushed() : mImageCanvas->inspectionPremultipliedAlpha();
}

void ImageViewer::setInspectionPremultipliedAlpha(bool value) {
    mImageCanvas->setInspectionPremultipliedAlpha(value);
    if (mInspectionPremultipliedAlphaButton) {
        mInspectionPremultipliedAlphaButton->set_pushed(value);
    }
}

void ImageViewer::updateFilter() {
    if (!mFilter) {
        size_t id = 1;
        for (size_t i = 0; i < mImages.size(); ++i) {
            auto* ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children()[i]);
            ib->set_visible(true);
            ib->setId(id++);
            ib->setHighlightRange(0, 0);
        }

        if (mCurrentImage) {
            size_t groupId = 1;
            for (Widget* button : mGroupButtonContainer->children()) {
                auto* ib = dynamic_cast<ImageButton*>(button);
                ib->set_visible(true);
                ib->setId(groupId++);
            }
        }

        requestLayoutUpdate();
        return;
    }

    string filter = mFilter->value();
    string imagePart = filter;
    string groupPart = "";

    auto colonPos = filter.find_last_of(':');
    if (colonPos != string::npos) {
        imagePart = filter.substr(0, colonPos);
        groupPart = filter.substr(colonPos + 1);
    }

    // Image filtering
    {
        // Checks whether an image matches the filter. This is the case if the image name matches the image part and at least one of the
        // image's groups matches the group part.
        const auto doesImageMatch = [&](const auto& name, const auto& channelGroups) {
            bool doesMatch = matchesFuzzyOrRegex(name, imagePart, useRegex());
            if (doesMatch) {
                bool anyGroupsMatch = false;
                for (const auto& group : channelGroups) {
                    if (matchesFuzzyOrRegex(group.name, groupPart, useRegex())) {
                        anyGroupsMatch = true;
                        break;
                    }
                }

                if (!anyGroupsMatch) {
                    doesMatch = false;
                }
            }

            return doesMatch;
        };

        vector<string> activeImageNames;
        size_t id = 1;
        for (size_t i = 0; i < mImages.size(); ++i) {
            ImageButton* ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children()[i]);
            ib->set_visible(doesImageMatch(ib->caption(), mImages[i]->channelGroups()));
            if (ib->visible()) {
                ib->setId(id++);
                activeImageNames.emplace_back(ib->caption());
            }
        }

        int beginOffset = 0, endOffset = 0;
        if (!activeImageNames.empty()) {
            string first = activeImageNames.front();
            int firstSize = (int)first.size();
            if (firstSize > 0) {
                bool allStartWithSameChar;
                do {
                    int len = codePointLength(first[beginOffset]);

                    allStartWithSameChar = all_of(begin(activeImageNames), end(activeImageNames), [&first, beginOffset, len](string_view name) {
                        if (beginOffset + len > (int)name.size()) {
                            return false;
                        }
                        for (int i = beginOffset; i < beginOffset + len; ++i) {
                            if (name[i] != first[i]) {
                                return false;
                            }
                        }
                        return true;
                    });

                    if (allStartWithSameChar) {
                        beginOffset += len;
                    }
                } while (allStartWithSameChar && beginOffset < firstSize);

                bool allEndWithSameChar;
                do {
                    char lastChar = first[firstSize - endOffset - 1];
                    allEndWithSameChar = all_of(begin(activeImageNames), end(activeImageNames), [lastChar, endOffset](string_view name) {
                        int index = (int)name.size() - endOffset - 1;
                        return index >= 0 && name[index] == lastChar;
                    });

                    if (allEndWithSameChar) {
                        ++endOffset;
                    }
                } while (allEndWithSameChar && endOffset < firstSize);
            }
        }

        bool currentImageMatchesFilter = false;
        for (size_t i = 0; i < mImages.size(); ++i) {
            ImageButton* ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children()[i]);
            if (ib->visible()) {
                currentImageMatchesFilter |= mImages[i] == mCurrentImage;
                ib->setHighlightRange(beginOffset, endOffset);
            }
        }

        if (!currentImageMatchesFilter) {
            selectImage(nthVisibleImage(0));
        }

        if (mCurrentReference && !matchesFuzzyOrRegex(mCurrentReference->name(), imagePart, useRegex())) {
            selectReference(nullptr);
        }
    }

    // Group filtering
    if (mCurrentImage) {
        size_t id = 1;
        const auto& buttons = mGroupButtonContainer->children();
        for (Widget* button : buttons) {
            ImageButton* ib = dynamic_cast<ImageButton*>(button);
            ib->set_visible(matchesFuzzyOrRegex(ib->caption(), groupPart, useRegex()));
            if (ib->visible()) {
                ib->setId(id++);
            }
        }

        if (!matchesFuzzyOrRegex(mCurrentGroup, groupPart, useRegex())) {
            selectGroup(nthVisibleGroup(0));
        }
    }

    requestLayoutUpdate();
}

void ImageViewer::perform_layout(NVGcontext* ctx) {
    Screen::perform_layout(ctx);
    if (!mHasWorkspaceInsets) {
        return;
    }

    // The application also requests ordinary NanoGUI layouts. Reapply the
    // reserved viewport after BoxLayout places the viewer's children.
    const int footerHeight = visibleFooterHeight();
    const Vector2i origin = min(Vector2i{mWorkspaceInsets[0], mWorkspaceInsets[1]}, max(Vector2i{0}, m_size - Vector2i{1}));
    const Vector2i extent = max(Vector2i{1}, m_size - origin - Vector2i{mWorkspaceInsets[2], mWorkspaceInsets[3] + footerHeight});
    mImageCanvas->parent()->set_position({0, 0});
    mImageCanvas->parent()->set_size(m_size);
    mImageCanvas->set_fixed_size(extent);
    mImageCanvas->set_position(origin);
    mImageCanvas->set_size(extent);
    mFooter->set_position({origin.x(), origin.y() + extent.y()});
    mFooter->set_fixed_width(extent.x());
    mFooter->set_size({extent.x(), footerHeight});
    mFooter->perform_layout(ctx);

    if (mSidebarOnRight) {
        const int top = mWorkspaceInspectorTop >= 0 ? mWorkspaceInspectorTop : mWorkspaceInsets[1];
        const int width = std::max(1, mWorkspaceInsets[2]);
        const int height = std::max(1, m_size.y() - top - 26);
        const int x = m_size.x() - static_cast<int>(std::lround(float(width) * std::clamp(mSidebarOverlayReveal, 0.0f, 1.0f)));
        mSidebar->set_position({x, top});
        mSidebar->set_fixed_size({width, height});
        mSidebar->set_size({width, height});
        mSidebar->perform_layout(ctx);
    }
}

void ImageViewer::updateLayout() {
    int sidebarWidth = mSidebarOnRight ? 0 : visibleSidebarWidth();
    int footerHeight = visibleFooterHeight();
    const Vector2i workspaceOrigin = mHasWorkspaceInsets
        ? min(Vector2i{mWorkspaceInsets[0], mWorkspaceInsets[1]}, max(Vector2i{0}, m_size - Vector2i{1}))
        : Vector2i{sidebarWidth, 0};
    const Vector2i workspaceExtent = mHasWorkspaceInsets
        ? max(Vector2i{1}, m_size - workspaceOrigin - Vector2i{mWorkspaceInsets[2], mWorkspaceInsets[3] + footerHeight})
        : m_size - Vector2i{sidebarWidth, footerHeight};
    mImageCanvas->set_fixed_size(workspaceExtent);
    mSidebar->set_fixed_height(m_size.y() - footerHeight);

    mVerticalScreenSplit->set_fixed_size(m_size);
    mImageScrollContainer->set_fixed_height(m_size.y() - mImageScrollContainer->position().y() - footerHeight);

    if (mImageScrollContainer->fixed_height() < 100) {
        // Stop scrolling the image button container and instead scroll the entire sidebar
        mImageScrollContainer->set_fixed_height(0);
    }

    if (mSidebarRoot) {
        mSidebarRoot->set_height(mSidebarRoot->preferred_size(m_nvg_context).y());
    }
    perform_layout();

    const int sidebarContentWidth = mSidebarRoot ? mSidebarRoot->width() : mSidebarLayout->parent()->width();
    if (mSidebarToolbar) {
        mSidebarToolbar->set_fixed_width(sidebarContentWidth);
        mSidebarToolbar->set_size({sidebarContentWidth, mSidebarToolbar->fixed_height()});
    }
    if (mSidebarTitleHost) {
        mSidebarTitleHost->set_fixed_width(sidebarContentWidth);
        mSidebarTitleHost->set_size({sidebarContentWidth, mSidebarTitleHost->preferred_size(m_nvg_context).y()});
    }
    if (mFloatingToolbarCard) {
        mFloatingToolbarCard->set_fixed_width(sidebarContentWidth);
        mFloatingToolbarCard->set_size({sidebarContentWidth, mFloatingToolbarCard->preferred_size(m_nvg_context).y()});
    }
    mSidebarLayout->set_fixed_width(sidebarContentWidth);
    updateDockedSliderWidths();
    const int helpWidth = 28;
    const int openWidth = mHeaderOpenImageButton ? 64 : 0;
    const int headerControlY = mSidebarOnRight ? 11 : 14;
    if (mHeaderOpenImageButton) {
        mHeaderOpenImageButton->set_fixed_size(Vector2i{openWidth, 26});
        const int headerWidth = mSidebarToolbar ? mSidebarToolbar->width() : mSidebarLayout->fixed_width();
        mHeaderOpenImageButton->set_position(Vector2i{headerWidth - helpWidth - openWidth - 12, headerControlY});
    }
    mHelpButton->set_fixed_size(Vector2i{helpWidth, 26});
    {
        const int headerWidth = mSidebarToolbar ? mSidebarToolbar->width() : mSidebarLayout->fixed_width();
        mHelpButton->set_position(Vector2i{headerWidth - helpWidth - 6, headerControlY});
    }
    if (mFilter) {
        mFilter->set_fixed_width(mSidebarLayout->fixed_width() - 42);
    }
    perform_layout();

    if (mSidebarOnRight && mSidebar->visible()) {
        constexpr int kContextStripHeight = 112;
        constexpr int kContextGap = 18;
        const int overlayMargin = 18;
        constexpr int kContextLift = 30;
        constexpr int kSidebarPeekWidth = 18;
        const int contextStripY = std::max(overlayMargin + 240, m_size.y() - kContextStripHeight - overlayMargin - kContextLift);
        const int overlayTop = mHasWorkspaceInsets
            ? (mWorkspaceInspectorTop >= 0 ? mWorkspaceInspectorTop : mWorkspaceInsets[1])
            : overlayMargin;
        const int overlayHeight = mHasWorkspaceInsets ? std::max(1, m_size.y() - overlayTop - 26)
            : std::max(420, contextStripY - overlayMargin - kContextGap);
        const int overlayWidth = mHasWorkspaceInsets ? std::max(1, mWorkspaceInsets[2])
            : std::max(SIDEBAR_MIN_WIDTH, mSidebar->fixed_width());
        const int expandedX = m_size.x() - overlayWidth - (mHasWorkspaceInsets ? 0 : overlayMargin);
        const int collapsedX = m_size.x() - (mHasWorkspaceInsets ? 0 : kSidebarPeekWidth);
        const int overlayX = static_cast<int>(std::lround(
            static_cast<float>(collapsedX) +
            (static_cast<float>(expandedX - collapsedX) * std::clamp(mSidebarOverlayReveal, 0.0f, 1.0f))
        ));
        mSidebar->set_position(Vector2i{overlayX, overlayTop});
        mSidebar->set_size(Vector2i{overlayWidth, overlayHeight});
        mSidebar->set_fixed_width(overlayWidth);
        mSidebar->set_fixed_height(overlayHeight);
        mSidebar->perform_layout(m_nvg_context);
        const int overlayContentWidth = mSidebarRoot ? mSidebarRoot->width() : mSidebarLayout->parent()->width();
        if (mSidebarToolbar) {
            mSidebarToolbar->set_fixed_width(overlayContentWidth);
            mSidebarToolbar->set_size({overlayContentWidth, mSidebarToolbar->fixed_height()});
        }
        if (mFloatingToolbarCard) {
            mFloatingToolbarCard->set_fixed_width(overlayContentWidth);
            mFloatingToolbarCard->set_size({overlayContentWidth, mFloatingToolbarCard->preferred_size(m_nvg_context).y()});
        }
        mSidebarLayout->set_fixed_width(overlayContentWidth);
        updateDockedSliderWidths();
        if (mHeaderOpenImageButton) {
            const int headerWidth = mSidebarToolbar ? mSidebarToolbar->width() : mSidebarLayout->fixed_width();
            mHeaderOpenImageButton->set_position(Vector2i{headerWidth - helpWidth - openWidth - 12, headerControlY});
        }
        {
            const int headerWidth = mSidebarToolbar ? mSidebarToolbar->width() : mSidebarLayout->fixed_width();
            mHelpButton->set_position(Vector2i{headerWidth - helpWidth - 6, headerControlY});
        }
        if (mFilter) {
            mFilter->set_fixed_width(mSidebarLayout->fixed_width() - 42);
        }
        mSidebar->perform_layout(m_nvg_context);
    }

    // With a changed layout the relative position of the mouse
    // within children changes and therefore should get updated.
    // nanogui does not handle this for us.
    double x, y;
    glfwGetCursorPos(m_glfw_window, &x, &y);
    cursor_pos_callback_event(x, y);
}

void ImageViewer::updateTitle() {
    if (!mCurrentImage) {
        set_caption("");
        return;
    }

    ostringstream caption;

    const string valueGroup = (mInspectionPreset != InspectionSource && preferredInspectionGroup().has_value())
        ? *preferredInspectionGroup()
        : string{mCurrentGroup};
    const auto channelsSpan = mCurrentImage->channelsInGroup(valueGroup);
    vector<string_view> channels = {begin(channelsSpan), end(channelsSpan)};

    // Remove duplicates
    channels.erase(unique(begin(channels), end(channels)), end(channels));
    // Only treat alpha specially if it is not the only channel.
    const bool hasAlpha = channels.size() > 1 && Channel::isAlpha(channels.back());

    auto channelTails = channels;
    transform(begin(channelTails), end(channelTails), begin(channelTails), Channel::tail);

    caption << format(
        "{} – {} – {}%",
        mCurrentImage->shortName(),
        inspectionCaptionForGroup(mCurrentGroup),
        (int)std::round(mImageCanvas->scale() * 100)
    );

    const auto rel = mouse_pos() - mImageCanvas->position();
    const vector<float> values = mImageCanvas->getValuesAtNanoPos({rel.x(), rel.y()}, channels);
    const Vector2i imageCoords = mImageCanvas->getImageCoords(mCurrentImage.get(), {rel.x(), rel.y()});
    TEV_ASSERT(values.size() >= channelTails.size(), "Should obtain a value for every existing channel.");

    caption << format(
        " – @{},{} ({:.3f},{:.3f}) / {}x{}: ",
        imageCoords.x(),
        imageCoords.y(),
        imageCoords.x() / (double)mCurrentImage->size().x(),
        imageCoords.y() / (double)mCurrentImage->size().y(),
        mCurrentImage->size().x(),
        mCurrentImage->size().y()
    );

    auto transformedValues = mImageCanvas->getDisplayedValuesAtNanoPos({rel.x(), rel.y()}, channels);
    if (const auto selection = inspectionChannelSelectionForGroup(mCurrentGroup)) {
        if (*selection >= 1 && transformedValues.size() >= (size_t)*selection) {
            transformedValues = {transformedValues[(size_t)(*selection - 1)]};
        }
    }
    for (size_t i = 0; i < transformedValues.size(); ++i) {
        caption << format("{:.2f},", transformedValues[i]);
    }

    caption.seekp(-1, ios_base::cur); // Remove last comma
    caption << " / 0x";
    for (size_t i = 0; i < values.size(); ++i) {
        const float srgbValue = hasAlpha && i == values.size() - 1 ? values[i] : toSRGB(values[i]);
        unsigned char discretizedValue = (char)(clamp(srgbValue, 0.0f, 1.0f) * 255 + 0.5f);
        caption << format("{:02X}", discretizedValue);
    }

    set_caption(caption.view());
}

string_view ImageViewer::groupName(size_t index) {
    if (!mCurrentImage) {
        return "";
    }

    const auto groups = mCurrentImage->channelGroups();
    TEV_ASSERT(index < groups.size(), "Group index out of bounds.");
    return groups[index].name;
}

optional<size_t> ImageViewer::groupId(string_view groupName) const {
    if (!mCurrentImage) {
        return 0;
    }

    const auto groups = mCurrentImage->channelGroups();
    const auto pos = (size_t)distance(begin(groups), ranges::find(groups, groupName, [](const auto& g) -> string_view { return g.name; }));
    return pos >= groups.size() ? nullopt : optional{pos};
}

optional<size_t> ImageViewer::imageId(const shared_ptr<Image>& image) const {
    const auto pos = (size_t)distance(begin(mImages), ranges::find(mImages, image));
    return pos >= mImages.size() ? nullopt : optional{pos};
}

optional<size_t> ImageViewer::imageId(string_view imageName) const {
    const auto pos = (size_t)distance(begin(mImages), ranges::find(mImages, imageName, [](const auto& i) { return i->name(); }));
    return pos >= mImages.size() ? nullopt : optional{pos};
}

string_view ImageViewer::nextGroup(string_view group, EDirection direction) {
    if (mGroupButtonContainer->child_count() == 0) {
        return mCurrentGroup;
    }

    const auto dir = direction == Forward ? 1 : -1;

    // If the group does not exist, start at index 0.
    const auto startId = (int)groupId(group).value_or(0);

    auto id = startId;
    do {
        id = (id + mGroupButtonContainer->child_count() + dir) % mGroupButtonContainer->child_count();
    } while (!mGroupButtonContainer->child_at(id)->visible() && id != startId);

    return groupName(id);
}

string_view ImageViewer::nthVisibleGroup(size_t n) {
    auto visibleGroups = views::iota(0, mGroupButtonContainer->child_count()) |
        views::filter([&](int i) { return mGroupButtonContainer->child_at(i)->visible(); }) |
        views::transform([&](int i) { return groupName((size_t)i); }) | views::take(n + 1);

    string_view lastVisible = mCurrentGroup;
    for (auto group : visibleGroups) {
        lastVisible = group;
    }

    return lastVisible;
}

shared_ptr<Image> ImageViewer::nextImage(const shared_ptr<Image>& image, EDirection direction) {
    if (mImages.empty()) {
        return nullptr;
    }

    const auto dir = direction == Forward ? 1 : -1;

    // If the image does not exist, start at image 0.
    const auto startId = (int)imageId(image).value_or(0);

    auto id = startId;
    do {
        id = (id + mImageButtonContainer->child_count() + dir) % mImageButtonContainer->child_count();
    } while (!mImageButtonContainer->child_at(id)->visible() && id != startId);

    return mImages.at(id);
}

shared_ptr<Image> ImageViewer::nthVisibleImage(size_t n) {
    shared_ptr<Image> lastVisible = nullptr;
    for (size_t i = 0; i < mImages.size(); ++i) {
        if (mImageButtonContainer->children()[i]->visible()) {
            lastVisible = mImages[i];
            if (n == 0) {
                break;
            }
            --n;
        }
    }

    return lastVisible;
}

shared_ptr<Image> ImageViewer::imageByName(string_view imageName) {
    const auto id = imageId(imageName);
    return id ? mImages.at(*id) : nullptr;
}

shared_ptr<Image> ImageViewer::imageByPath(const fs::path& imagePath) {
    auto normalized = fs::weakly_canonical(imagePath);
    for (const auto& image : mImages) {
        if (!image) {
            continue;
        }

        if (fs::weakly_canonical(image->path()) == normalized) {
            return image;
        }
    }

    return nullptr;
}

bool ImageViewer::selectImageByPath(const fs::path& imagePath) {
    if (auto image = imageByPath(imagePath)) {
        selectImage(image);
        return true;
    }

    return false;
}

void ImageViewer::updateCurrentMonitorSize() {
    if (GLFWmonitor* monitor = glfwGetWindowCurrentMonitor(m_glfw_window)) {
        Vector2i pos, size;
        glfwGetMonitorWorkarea(monitor, &pos.x(), &pos.y(), &size.x(), &size.y());
        if (size == Vector2i{0, 0}) {
            return;
        }

        // On some systems (notably Hyprland and some other tiling window managers / compositors), windows are always flagged as
        // maximized, even if they are technically not, to get them to play nicely with decorations. In the following, we detect
        // such cases (only after a current monitor was detected to give enough time for the compositor to set up the window) and
        // treat them as non-maximized always.
        if (isMaximized() && !mMaximizedLaunch) {
            tlog::debug("Detected unreliable maximized state; disabling maximized detection.");
            mMaximizedUnreliable = true;
        }

        auto posf = Vector2f{pos};
        auto sizef = Vector2f{size};

        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            posf = posf / pixel_ratio();
            sizef = sizef / pixel_ratio();
        }

        if (posf == mMinWindowPos && sizef == mMaxWindowSize) {
            return;
        }

        mMinWindowPos = posf;
        mMaxWindowSize = sizef;

        tlog::debug("Current monitor: pos={} size={}", mMinWindowPos, mMaxWindowSize);
    }
}

// ---- Colour space of the file's values ---------------------------------------------------

std::optional<std::string> ImageViewer::assignedSourceColor() const {
    if (!mCurrentImage) {
        return std::nullopt;
    }
    const auto value = findAttributeValue(mCurrentImage->attributes(), "HDRSPACE_COLOR");
    if (!value) {
        return std::nullopt;
    }
    std::string token;
    for (const char c : *value) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!token.empty()) break;
            continue;
        }
        token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (token.empty()) {
        return std::nullopt;
    }
    return token;
}

bool ImageViewer::currentFileIsRadiance() const {
    if (!mCurrentImage) {
        return false;
    }
    std::string extension = mCurrentImage->path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    return extension == ".hdr" || extension == ".pic" || extension == ".rgbe";
}

int ImageViewer::sourceColorKind() const {
    if (!mCurrentImage) {
        return -1;
    }
    const auto attributes = mCurrentImage->attributes();
    if (const auto chromaValue = parseTargetChroma(attributes)) {
        if (sameChroma(*chromaValue, radChroma())) return 0;
        if (sameChroma(*chromaValue, rec709Chroma())) return 1;
        if (sameChroma(*chromaValue, chroma(EWpPrimaries::CIE1931XYZ))) return 2;
        return 5;
    }
    if (parseSensorToXyz(attributes).has_value()) {
        return 3;
    }
    if (const auto assigned = assignedSourceColor()) {
        if (*assigned == "raw") return 3;
        if (*assigned == "luminance") return 4;
    }
    return -1;
}

std::string ImageViewer::sourceColorCaption() const {
    const int kind = sourceColorKind();
    if (kind < 0) {
        return "Unknown";
    }
    static const char* kNames[] = {"Rad", "sRGB", "XYZ", "Raw", "Luminance", "RGB"};
    const std::string name = std::string{kNames[kind]} + (kind == 2 && sourceValuesInCdm2() ? " cd/m\u00b2" : "");
    return name + (assignedSourceColor().has_value() ? " (assigned)" : " (header)");
}

bool ImageViewer::sourceValuesInCdm2() const {
    if (!mCurrentImage) {
        return false;
    }
    const auto value = findAttributeValue(mCurrentImage->attributes(), "HDRSPACE_COLOR");
    if (!value) {
        return false;
    }
    std::string text{*value};
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text.find("cd/m2") != std::string::npos;
}

int ImageViewer::currentAssignedColorKind() const {
    const auto assigned = assignedSourceColor();
    if (!assigned) return -1;
    if (*assigned == "rad") return 0;
    if (*assigned == "srgb") return 1;
    if (*assigned == "xyz") return sourceValuesInCdm2() ? 5 : 2;
    if (*assigned == "raw") return 3;
    if (*assigned == "luminance") return 4;
    return -1;
}

bool ImageViewer::canAssignSourceColor() const {
    if (!currentFileIsRadiance() || mCurrentImage->path().empty() || !fs::exists(mCurrentImage->path())) {
        return false;
    }
    return sourceColorKind() < 0 || assignedSourceColor().has_value();
}

// Adds the colour lines at the end of the information header: the bytes of every other header
// line and of the pixel data are kept. Lines hdrspace wrote before (HDRSPACE_COLOR and the
// top-level PRIMARIES that came with it) are replaced.
bool ImageViewer::writeAssignedColorHeader(int kind, std::string& error) {
    static const char* kKeys[] = {"rad", "srgb", "xyz", "raw", "luminance", "xyz"};
    static const char* kPrimaries[] = {
        "PRIMARIES= 0.6400 0.3300 0.2900 0.6000 0.1500 0.0600 0.3333 0.3333",
        "PRIMARIES= 0.6400 0.3300 0.3000 0.6000 0.1500 0.0600 0.3127 0.3290",
        "PRIMARIES= 1.0000 0.0000 0.0000 1.0000 0.0000 0.0000 0.3333 0.3333",
        nullptr,
        nullptr,
        "PRIMARIES= 1.0000 0.0000 0.0000 1.0000 0.0000 0.0000 0.3333 0.3333",
    };
    if (kind < 0 || kind > 5 || !mCurrentImage) {
        error = "No colour space chosen.";
        return false;
    }
    const fs::path path = mCurrentImage->path();
    std::string data;
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "Could not read the file.";
            return false;
        }
        data.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    if (data.rfind("#?", 0) != 0) {
        error = "Not a Radiance picture header.";
        return false;
    }
    const size_t headerEnd = data.find("\n\n");
    if (headerEnd == std::string::npos) {
        error = "The header has no end (blank line).";
        return false;
    }

    std::vector<std::string> lines;
    {
        size_t start = 0;
        const std::string header = data.substr(0, headerEnd);
        while (start <= header.size()) {
            const size_t next = header.find('\n', start);
            lines.push_back(header.substr(start, next == std::string::npos ? std::string::npos : next - start));
            if (next == std::string::npos) break;
            start = next + 1;
        }
    }
    const auto startsWith = [](const std::string& line, std::string_view prefix) { return line.rfind(prefix, 0) == 0; };
    bool hadAssigned = false;
    for (const auto& line : lines) {
        if (startsWith(line, "HDRSPACE_COLOR=")) hadAssigned = true;
    }
    if (!hadAssigned) {
        for (const auto& line : lines) {
            if (startsWith(line, "PRIMARIES=") || startsWith(line, "TargetPrimaries=")) {
                error = "The header already defines colour primaries; it was not changed.";
                return false;
            }
        }
    }
    std::vector<std::string> kept;
    for (auto& line : lines) {
        if (startsWith(line, "HDRSPACE_COLOR=")) continue;
        if (hadAssigned && startsWith(line, "PRIMARIES=")) continue;
        kept.push_back(std::move(line));
    }
    if (kPrimaries[kind]) {
        kept.push_back(kPrimaries[kind]);
    }
    kept.push_back(std::string{"HDRSPACE_COLOR= "} + kKeys[kind] + (kind == 4 || kind == 5 ? " cd/m2" : ""));

    std::string output;
    for (size_t i = 0; i < kept.size(); ++i) {
        if (i) output.push_back('\n');
        output += kept[i];
    }
    output.append(data, headerEnd, std::string::npos);

    const fs::path temp = fs::path{path.string() + ".hdrspace-tmp"};
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Could not write next to the file (is the folder read-only?).";
            return false;
        }
        out.write(output.data(), static_cast<std::streamsize>(output.size()));
        if (!out) {
            error = "Writing the file failed.";
            std::error_code ec;
            fs::remove(temp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::permissions(temp, fs::status(path, ec).permissions(), ec);
    fs::rename(temp, path, ec);
    if (ec) {
        error = "Could not replace the file: " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

void ImageViewer::requestSourceColorAssignment(int kind) {
    if (!canAssignSourceColor() || kind < 0 || kind > 5) {
        return;
    }
    static const char* kNames[] = {"Radiance RGB", "sRGB / Rec.709", "XYZ", "Raw (camera)", "Luminance (cd/m²)", "XYZ (cd/m²)"};
    static const char* kLines[] = {
        "PRIMARIES= 0.6400 0.3300 0.2900 0.6000 0.1500 0.0600 0.3333 0.3333\nHDRSPACE_COLOR= rad",
        "PRIMARIES= 0.6400 0.3300 0.3000 0.6000 0.1500 0.0600 0.3127 0.3290\nHDRSPACE_COLOR= srgb",
        "PRIMARIES= 1.0000 0.0000 0.0000 1.0000 0.0000 0.0000 0.3333 0.3333\nHDRSPACE_COLOR= xyz",
        "HDRSPACE_COLOR= raw",
        "HDRSPACE_COLOR= luminance cd/m2",
        "PRIMARIES= 1.0000 0.0000 0.0000 1.0000 0.0000 0.0000 0.3333 0.3333\nHDRSPACE_COLOR= xyz cd/m2",
    };
    closeInspectionModeMenu();
    const std::string fileName = mCurrentImage->path().filename().string();
    auto* dialog = new MessageDialog(
        this,
        MessageDialog::Type::Question,
        std::string{"Read values as "} + kNames[kind] + "?",
        "Adds to the header of " + fileName + ":\n" + kLines[kind] +
            "\nPixel values and the other header lines stay as they are.",
        "Write header",
        "Cancel",
        true
    );
    dialog->set_callback([this, kind](int result) {
        if (result != 0) {
            refreshInspectionModeUi();
            return;
        }
        std::string error;
        if (!writeAssignedColorHeader(kind, error)) {
            showErrorDialog("The colour space was not written. " + error);
            refreshInspectionModeUi();
            return;
        }
        invalidateSourceInterpretationCache();
        reloadImage(mCurrentImage, true);
    });
}

// ---- Perceptual maps on the canvas -------------------------------------------------------

void ImageViewer::setPerceptualMapSettings(double pixelsPerDegree, double sensitivityCorrection,
    const std::string& spectralEmissionPath, int inputColor) {
    const bool changed = pixelsPerDegree != mPerceptualPpd || sensitivityCorrection != mPerceptualSensitivity ||
        spectralEmissionPath != mPerceptualEmissionPath || inputColor != mPerceptualInputColor;
    mPerceptualPpd = pixelsPerDegree;
    mPerceptualSensitivity = sensitivityCorrection;
    mPerceptualEmissionPath = spectralEmissionPath;
    mPerceptualInputColor = inputColor;
#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
    if (changed) {
        mExperimentalInspectionPreviewImage.reset();
        mExperimentalInspectionPreviewPreset = -1;
        mExperimentalInspectionPreviewSourceImageId = -1;
    }
#else
    (void)changed;
#endif
}

bool ImageViewer::showPerceptualMap(int kind) {
#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
    if (kind < 0 || kind > 6) {
        return false;
    }
    const int preset = InspectionL + kind;
    if (!inspectionPresetSupported(preset)) {
        return false;
    }
    applyInspectionPreset(preset);
    return mInspectionPreset == preset;
#else
    (void)kind;
    return false;
#endif
}

void ImageViewer::showSourceValues() {
    if (mInspectionPreset != InspectionSource) {
        applyInspectionPreset(InspectionSource);
    }
}

bool ImageViewer::perceptualMapShown() const {
#ifdef MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
    return inspectionPresetUsesExperimentalMap(mInspectionPreset);
#else
    return false;
#endif
}

void ImageViewer::setFalseColorDisplay(bool enabled, bool logScale, float minValue, float maxValue) {
    if (!enabled) {
        setTonemap(ETonemap::SRGB);
        redraw();
        return;
    }
    if (maxValue < minValue) {
        std::swap(minValue, maxValue);
    }
    if (mFalseColorScaleComboBox && mFalseColorMinBox && mFalseColorMaxBox && mFalseColorColormapComboBox) {
        mFalseColorScaleComboBox->set_selected_index(logScale ? 1 : 0);
        mFalseColorMinBox->set_value(minValue);
        mFalseColorMaxBox->set_value(maxValue);
        applyFalseColorSettingsFromUi();
        return;
    }
    mImageCanvas->setFalseColorScaleMode(static_cast<EFalseColorScaleMode>(logScale ? 1 : 0));
    mImageCanvas->setFalseColorRange(Vector2f{minValue, maxValue});
    setTonemap(ETonemap::FalseColor);
    redraw();
}

} // namespace tev
