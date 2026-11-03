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

// This file was adapted from the nanogui::Graph class, which was developed
// by Wenzel Jakob <wenzel.jakob@epfl.ch> and based on the NanoVG demo application
// by Mikko Mononen. Modifications were developed by Thomas Müller <contact@tom94.net>.

#include <tev/MultiGraph.h>

#include <nanogui/opengl.h>
#include <nanogui/theme.h>

#include <algorithm>
#include <array>
#include <cmath>

using namespace nanogui;
using namespace std;

namespace tev {

static string formatNumber(float v) {
    bool needsScientificNotation = v != 0 && (abs(v) < 0.01f || abs(v) >= 1000);
    return needsScientificNotation ? format("{:.2e}", v) : format("{:.3f}", v);
}

MultiGraph::MultiGraph(Widget* parent, string_view caption) : Widget{parent}, mCaption{caption} {
    mBackgroundColor = Color(20, 128);
    mForegroundColor = Color(255, 192, 0, 128);
    mTextColor = Color(240, 192);
}

Vector2i MultiGraph::preferred_size_impl(NVGcontext*) const { return mQuiet ? Vector2i(180, 72) : Vector2i(180, 80); }

// hdrspace: the docked Viewer draws the histogram as quiet lines. Channel hues match the
// region statistics table; anything that is not clearly R, G or B is drawn in grey.
static Color quietChannelColor(const Color& c) {
    const float r = c.r(), g = c.g(), b = c.b();
    if (r > g + 0.2f && r > b + 0.2f) {
        return Color(226, 119, 109, 204);
    }
    if (g > r + 0.2f && g > b + 0.2f) {
        return Color(134, 194, 127, 204);
    }
    if (b > r + 0.2f && b > g + 0.2f) {
        return Color(127, 159, 224, 204);
    }
    return Color(170, 174, 179, 204);
}

void MultiGraph::draw(NVGcontext* ctx) {
    Widget::draw(ctx);

    if (mQuiet) {
        constexpr float kAxisHeight = 16.0f;
        const float px = (float)m_pos.x(), py = (float)m_pos.y(), pw = (float)m_size.x();
        const float ph = std::max(12.0f, (float)m_size.y() - kAxisHeight);
        nvgSave(ctx);
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, px, py, pw, ph, 4.0f);
        nvgFillColor(ctx, Color(20, 22, 24, 255));
        nvgFill(ctx);

        const size_t nBins = mNChannels > 0 ? mValues.size() / (size_t)mNChannels : 0;
        if (mValues.size() >= 2 && nBins >= 2) {
            const float ix = px + 8.0f, iy = py + 6.0f, iw = pw - 16.0f, ih = ph - 12.0f;
            if (mZeroBin > 0) {
                const float zx = std::round(ix + mZeroBin * iw / (float)(nBins - 1)) + 0.5f;
                nvgBeginPath(ctx);
                nvgMoveTo(ctx, zx, iy);
                nvgLineTo(ctx, zx, iy + ih);
                nvgStrokeColor(ctx, Color(170, 174, 179, 90));
                nvgStrokeWidth(ctx, 1.0f);
                nvgStroke(ctx);
            }
            for (size_t i = 0; i < (size_t)mNChannels; i++) {
                nvgBeginPath(ctx);
                for (size_t j = 0; j < nBins; j++) {
                    const float value = std::clamp(mValues[j + i * nBins], 0.0f, 1.0f);
                    const float vx = ix + j * iw / (float)(nBins - 1);
                    const float vy = iy + (1.0f - value) * ih;
                    if (j == 0) {
                        nvgMoveTo(ctx, vx, vy);
                    } else {
                        nvgLineTo(ctx, vx, vy);
                    }
                }
                nvgStrokeColor(ctx, quietChannelColor(i < mColors.size() ? mColors[i] : mForegroundColor));
                nvgStrokeWidth(ctx, 1.0f);
                nvgLineJoin(ctx, NVG_ROUND);
                nvgStroke(ctx);
            }

            nvgFontFace(ctx, "sans");
            nvgFontSize(ctx, 11.0f);
            nvgFillColor(ctx, Color(170, 174, 179, 168));
            const float ty = py + ph + 4.0f;
            const string minimum = formatNumber(mMinimum), mean = formatNumber(mMean), maximum = formatNumber(mMaximum);
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgText(ctx, px + 8.0f, ty, minimum.c_str(), nullptr);
            nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            nvgText(ctx, px + pw * 0.5f, ty, mean.c_str(), nullptr);
            nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
            nvgText(ctx, px + pw - 8.0f, ty, maximum.c_str(), nullptr);
        }
        nvgRestore(ctx);
        return;
    }

    NVGpaint bg = nvgBoxGradient(ctx, m_pos.x() + 1, m_pos.y() + 1 + 1.0f, m_size.x() - 2, m_size.y() - 2, 3, 4, Color(120, 32), Color(32, 32));

    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, m_pos.x() + 1, m_pos.y() + 1 + 1.0f, m_size.x() - 2, m_size.y() - 2, 3);

    nvgFillPaint(ctx, bg);

    nvgFill(ctx);

    if (mValues.size() >= 2) {
        nvgSave(ctx);

        // Additive blending
        nvgGlobalCompositeBlendFunc(ctx, NVGblendFactor::NVG_SRC_ALPHA, NVGblendFactor::NVG_ONE);

        size_t nBins = mValues.size() / mNChannels;

        for (size_t i = 0; i < (size_t)mNChannels; i++) {
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, m_pos.x(), m_pos.y() + m_size.y());

            for (size_t j = 0; j < (size_t)nBins; j++) {
                float value = mValues[j + i * nBins];
                float vx = m_pos.x() + 2 + j * (m_size.x() - 4) / (float)(nBins - 1);
                float vy = m_pos.y() + (1 - value) * m_size.y();
                nvgLineTo(ctx, vx, vy);
            }

            auto color = i < mColors.size() ? mColors[i] : mForegroundColor;
            nvgLineTo(ctx, m_pos.x() + m_size.x(), m_pos.y() + m_size.y());
            nvgFillColor(ctx, color);
            nvgFill(ctx);
        }

        nvgRestore(ctx);

        if (mZeroBin > 0) {
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x() + 1 + mZeroBin * (m_size.x() - 4) / (float)(nBins - 1), m_pos.y() + 15, 4, m_size.y() - 15);
            nvgFillColor(ctx, Color(0, 128));
            nvgFill(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x() + 2 + mZeroBin * (m_size.x() - 4) / (float)(nBins - 1), m_pos.y() + 15, 2, m_size.y() - 15);
            nvgFillColor(ctx, Color(200, 255));
            nvgFill(ctx);
        }

        nvgFontFace(ctx, "sans");

        nvgFontSize(ctx, 15.0f);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(ctx, mTextColor);
        drawTextWithShadow(ctx, m_pos.x() + 3, m_pos.y() + 1, formatNumber(mMinimum));

        nvgTextAlign(ctx, NVG_ALIGN_MIDDLE | NVG_ALIGN_TOP);
        nvgFillColor(ctx, mTextColor);
        string meanString = formatNumber(mMean);
        float textWidth = nvgTextBounds(ctx, 0, 0, meanString.c_str(), nullptr, nullptr);
        drawTextWithShadow(ctx, m_pos.x() + (float)m_size.x() / 2 - textWidth / 2, m_pos.y() + 1, meanString);

        nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgFillColor(ctx, mTextColor);
        drawTextWithShadow(ctx, m_pos.x() + m_size.x() - 3, m_pos.y() + 1, formatNumber(mMaximum));

        if (!mCaption.empty()) {
            nvgFontSize(ctx, 14.0f);
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(ctx, mTextColor);
            nvgText(ctx, m_pos.x() + 3, m_pos.y() + 1, mCaption.data(), mCaption.data() + mCaption.size());
        }

        if (!mHeader.empty()) {
            nvgFontSize(ctx, 18.0f);
            nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
            nvgFillColor(ctx, mTextColor);
            nvgText(ctx, m_pos.x() + m_size.x() - 3, m_pos.y() + 1, mHeader.data(), mHeader.data() + mHeader.size());
        }

        if (!mFooter.empty()) {
            nvgFontSize(ctx, 15.0f);
            nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
            nvgFillColor(ctx, mTextColor);
            nvgText(ctx, m_pos.x() + m_size.x() - 3, m_pos.y() + m_size.y() - 1, mFooter.data(), mFooter.data() + mFooter.size());
        }
    }

    nvgBeginPath(ctx);
    nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
    nvgRoundedRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y(), 2.5f);
    nvgPathWinding(ctx, NVG_HOLE);
    nvgFillColor(ctx, Color(0.23f, 1.0f));
    nvgFill(ctx);

    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, m_pos.x() + 0.5f, m_pos.y() + 0.5f, m_size.x() - 1, m_size.y() - 1, 2.5f);
    nvgStrokeColor(ctx, Color(0, 48));
    nvgStroke(ctx);
}

} // namespace tev
