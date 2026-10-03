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

#include <tev/ImageButton.h>

#include <nanogui/layout.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/theme.h>

#include <cctype>
#include <tuple>
#include <utility>

using namespace nanogui;
using namespace std;

namespace tev {

namespace {
// Width kept free at the right end of a caption for the close mark (drawn 7..15 px
// from the right edge, clickable 4..18 px from it).
constexpr int kCloseReserve = 18;
} // namespace

ImageButton::ImageButton(Widget* parent, string_view caption, bool canBeReference) :
    Widget{parent}, mCaption{caption}, mCanBeReference{canBeReference} {
    this->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill});
    mCaptionTextBox = new TextBox{this, caption};
    mCaptionTextBox->set_visible(false);
    mCaptionTextBox->set_editable(true);
    mCaptionTextBox->set_alignment(TextBox::Alignment::Right);
    mCaptionTextBox->set_placeholder(caption);
    mCaptionTextBox->set_callback([this](const string&) {
        this->hideTextBox();
        return true;
    });
    mCaptionTextBox->set_corner_radius(0.0f);
    mCaptionTextBox->set_solid_color(IMAGE_COLOR);
}

ImageButton::~ImageButton() {
    if (mThumbnailImageHandle != 0 && screen() && screen()->nvg_context()) {
        nvgDeleteImage(screen()->nvg_context(), mThumbnailImageHandle);
        mThumbnailImageHandle = 0;
    }
}

Vector2i ImageButton::preferred_size_impl(NVGcontext* ctx) const {
    if (!mThumbnailRgba.empty() && mThumbnailSize.x() > 0 && mThumbnailSize.y() > 0) {
        return {96, 72};
    }

    if (m_preferred_size_cache != Vector2i(-1)) {
        return m_preferred_size_cache;
    }

    nvgFontSize(ctx, m_font_size);
    nvgFontFace(ctx, "sans-bold");
    string idString = to_string(mId);
    float idSize = nvgTextBounds(ctx, 0, 0, idString.data(), idString.data() + idString.size(), nullptr);

    nvgFontSize(ctx, m_font_size);
    nvgFontFace(ctx, "sans");
    float tw = nvgTextBounds(ctx, 0, 0, mCaption.data(), mCaption.data() + mCaption.size(), nullptr);

    m_preferred_size_cache = Vector2i(static_cast<int>(tw + idSize) + 28 + (mCloseCallback ? kCloseReserve : 0), m_font_size + 6);
    return m_preferred_size_cache;
}

bool ImageButton::mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) {
    if (Widget::mouse_button_event(p, button, down, modifiers)) {
        return true;
    }

    if (!m_enabled || !down) {
        return false;
    }

    const bool closeHit = button == GLFW_MOUSE_BUTTON_1 && p.x() >= m_size.x() - 18 && p.x() <= m_size.x() - 4 && p.y() >= 3 &&
        p.y() <= m_size.y() - 3;
    if (closeHit && mCloseCallback) {
        mCloseCallback();
        return true;
    }

    if (mCanBeReference && (button == GLFW_MOUSE_BUTTON_2 || (button == GLFW_MOUSE_BUTTON_1 && modifiers & GLFW_MOD_SHIFT))) {
        // If we already were the reference, then let's disable using us a reference.
        mIsReference = !mIsReference;

        // If we newly became the reference, then we need to disable the existing reference if it exists.
        if (mIsReference) {
            for (auto widget : parent()->children()) {
                ImageButton* b = dynamic_cast<ImageButton*>(widget);
                if (b && b != this) {
                    b->mIsReference = false;
                }
            }
        }

        // Invoke the callback in any case, such that the surrounding code can react to new references or a loss of a reference image.
        if (mReferenceCallback) {
            mReferenceCallback(mIsReference);
        }
        return true;
    } else if (button == GLFW_MOUSE_BUTTON_1) {
        if (!mIsSelected) {
            // Unselect the other, currently selected image.
            for (auto widget : parent()->children()) {
                ImageButton* b = dynamic_cast<ImageButton*>(widget);
                if (b && b != this) {
                    b->mIsSelected = false;
                }
            }

            mIsSelected = true;
            if (mSelectedCallback) {
                mSelectedCallback();
            }
        }
        return true;
    }

    return false;
}

void ImageButton::draw(NVGcontext* ctx) {
    Widget::draw(ctx);

    if (mCaptionTextBox->visible()) {
        return;
    }

    if (mIsReference) {
        nvgBeginPath(ctx);
        nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
        nvgFillColor(ctx, REFERENCE_COLOR);
        nvgFill(ctx);
    }

    // Fill the button with color.
    if (mIsSelected || m_mouse_focus) {
        nvgBeginPath(ctx);

        if (mIsReference) {
            nvgRect(ctx, m_pos.x() + 2, m_pos.y() + 2, m_size.x() - 4, m_size.y() - 4);
        } else {
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
        }

        nvgFillColor(ctx, mIsSelected ? IMAGE_COLOR : Color(1.0f, 0.1f));
        nvgFill(ctx);
    }

    if (!mThumbnailRgba.empty() && mThumbnailSize.x() > 0 && mThumbnailSize.y() > 0) {
        if (mThumbnailDirty) {
            if (mThumbnailImageHandle != 0) {
                nvgDeleteImage(ctx, mThumbnailImageHandle);
                mThumbnailImageHandle = 0;
            }
            mThumbnailImageHandle = nvgCreateImageRGBA(ctx, mThumbnailSize.x(), mThumbnailSize.y(), 0, mThumbnailRgba.data());
            mThumbnailDirty = false;
        }

        const float inset = mIsReference ? 3.0f : 1.0f;
        const float drawX = static_cast<float>(m_pos.x()) + inset;
        const float drawY = static_cast<float>(m_pos.y()) + inset;
        const float drawW = static_cast<float>(m_size.x()) - inset * 2.0f;
        const float drawH = static_cast<float>(m_size.y()) - inset * 2.0f;

        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, drawX, drawY, drawW, drawH, 7.0f);
        nvgFillColor(ctx, Color(24, 28, 38, 220));
        nvgFill(ctx);

        if (mThumbnailImageHandle != 0) {
            const float srcAspect = static_cast<float>(mThumbnailSize.x()) / static_cast<float>(mThumbnailSize.y());
            const float dstAspect = drawW / std::max(1.0f, drawH);
            float thumbW = drawW;
            float thumbH = drawH;
            float thumbX = drawX;
            float thumbY = drawY;
            if (srcAspect > dstAspect) {
                thumbH = drawW / srcAspect;
                thumbY += (drawH - thumbH) * 0.5f;
            } else {
                thumbW = drawH * srcAspect;
                thumbX += (drawW - thumbW) * 0.5f;
            }
            NVGpaint paint = nvgImagePattern(ctx, thumbX, thumbY, thumbW, thumbH, 0.0f, mThumbnailImageHandle, 1.0f);
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, drawX, drawY, drawW, drawH, 7.0f);
            nvgFillPaint(ctx, paint);
            nvgFill(ctx);
        }

        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, drawX + 0.5f, drawY + 0.5f, drawW - 1.0f, drawH - 1.0f, 7.0f);
        nvgStrokeColor(ctx, mIsSelected ? Color(255, 255, 255, 220) : (m_mouse_focus ? Color(255, 255, 255, 120) : Color(255, 255, 255, 28)));
        nvgStrokeWidth(ctx, mIsSelected ? 2.0f : 1.0f);
        nvgStroke(ctx);

        if (mIsReference) {
            nvgBeginPath(ctx);
            nvgCircle(ctx, drawX + 8.0f, drawY + 8.0f, 3.5f);
            nvgFillColor(ctx, REFERENCE_COLOR);
            nvgFill(ctx);
        }

        if (mCloseCallback) {
            const float crossRight = static_cast<float>(m_pos.x() + m_size.x() - 8);
            const float crossLeft = crossRight - 8.0f;
            const float crossTop = static_cast<float>(m_pos.y() + 6);
            const float crossBottom = crossTop + 8.0f;
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, crossLeft, crossTop);
            nvgLineTo(ctx, crossRight, crossBottom);
            nvgMoveTo(ctx, crossLeft, crossBottom);
            nvgLineTo(ctx, crossRight, crossTop);
            nvgStrokeWidth(ctx, 1.5f);
            nvgStrokeColor(ctx, m_mouse_focus ? Color(255, 255) : Color(210, 220));
            nvgStroke(ctx);
        }
        return;
    }

    const string idString = to_string(mId);
    const float closeReserve = mCloseCallback ? static_cast<float>(kCloseReserve) : 0.0f;
    // tev highlights the part of a name that differs from the other open images. With no
    // such part (nothing highlighted, or the whole name), shorten in the middle instead.
    const bool partialHighlight = mHighlightBegin < mHighlightEnd && (mHighlightBegin > 0 || mHighlightEnd < mCaption.size());
    const bool middleCut = !mCaptionLeftAligned && !partialHighlight;
    // A fully highlighted name is drawn bold, as tev does for names with no common part.
    const bool wholeHighlight = mHighlightBegin == 0 && mHighlightEnd >= mCaption.size() && !mCaption.empty();
    const char* middleCutFace = wholeHighlight ? "sans-bold" : "sans";
    if (m_size.x() == preferred_size_impl(ctx).x()) {
        mCutoff = 0;
        mHeadBegin = 0;
        mHeadEnd = mTailBegin = mCaption.size();
    } else if (m_size != mSizeForWhichCutoffWasComputed) {
        mCutoff = 0;
        mHeadBegin = 0;
        mHeadEnd = mTailBegin = mCaption.size();

        nvgFontSize(ctx, m_font_size + 2);
        nvgFontFace(ctx, "sans-bold");
        const float idSize = nvgTextBounds(ctx, 0, 0, idString.data(), idString.data() + idString.size(), nullptr);

        nvgFontSize(ctx, m_font_size);
        const float availableWidth = mCaptionLeftAligned ? m_size.x() - 30 - idSize : m_size.x() - 38 - idSize - closeReserve;
        if (mCaptionLeftAligned) {
            while (mCutoff < mCaption.size()) {
                const size_t visibleCount = mCaption.size() - mCutoff;
                const bool needsEllipsis = visibleCount < mCaption.size();
                const string_view visible = string_view{mCaption}.substr(0, visibleCount);
                float bounds = nvgTextBounds(ctx, 0, 0, visible.data(), visible.data() + visible.size(), nullptr);
                if (needsEllipsis) {
                    bounds += nvgTextBounds(ctx, 0, 0, "…", nullptr, nullptr);
                }
                if (bounds <= availableWidth) {
                    break;
                }

                mCutoff += codePointLength(mCaption[mCaption.size() - 1 - mCutoff]);
            }
        } else if (middleCut) {
            // hdrspace: a caption that does not fit drops its folder first and then the
            // middle of the file name, keeping the name's beginning (the scene) and end (the
            // version and extension): "office_south…final_review_v12.hdr".
            nvgFontFace(ctx, middleCutFace);
            const auto widthOf = [&](string_view text) { return nvgTextBounds(ctx, 0, 0, text.data(), text.data() + text.size(), nullptr); };
            if (widthOf(mCaption) > availableWidth) {
                const size_t slash = mCaption.find_last_of('/');
                const size_t nameBegin = slash == string::npos || slash + 1 >= mCaption.size() ? 0 : slash + 1;
                mHeadBegin = nameBegin;
                if (widthOf(string_view{mCaption}.substr(nameBegin)) > availableWidth) {
                    vector<size_t> starts;
                    for (size_t i = nameBegin; i < mCaption.size(); ++i) {
                        if ((static_cast<unsigned char>(mCaption[i]) & 0xC0) != 0x80) {
                            starts.push_back(i);
                        }
                    }
                    const size_t count = starts.size();
                    const auto cutFor = [&](size_t keep) {
                        const size_t head = (keep + 1) / 2;
                        const size_t tail = keep / 2;
                        return pair<size_t, size_t>{head < count ? starts[head] : mCaption.size(),
                                                    tail > 0 ? starts[count - tail] : mCaption.size()};
                    };
                    const auto widthFor = [&](size_t keep) {
                        const auto [headEnd, tailBegin] = cutFor(keep);
                        const string text = mCaption.substr(nameBegin, headEnd - nameBegin) + "…" + mCaption.substr(tailBegin);
                        return widthOf(text);
                    };
                    size_t lo = 0;
                    size_t hi = count > 0 ? count - 1 : 0;
                    while (lo < hi) {
                        const size_t mid = (lo + hi + 1) / 2;
                        if (widthFor(mid) <= availableWidth) {
                            lo = mid;
                        } else {
                            hi = mid - 1;
                        }
                    }
                    std::tie(mHeadEnd, mTailBegin) = cutFor(lo);
                }
            }
        } else {
            while (mCutoff < mCaption.size()) {
                float bounds = nvgTextBounds(ctx, 0, 0, mCaption.data() + mCutoff, mCaption.data() + mCaption.size(), nullptr);
                if (bounds <= availableWidth) {
                    break;
                }

                mCutoff += codePointLength(mCaption[mCutoff]);
            }
        }

        mSizeForWhichCutoffWasComputed = m_size;
    }

    // Image name
    if (!mShowCaption) {
        if (mCloseCallback) {
            const float crossRight = static_cast<float>(m_pos.x() + m_size.x() - 7);
            const float crossLeft = crossRight - 8.0f;
            const float crossTop = static_cast<float>(m_pos.y() + 5);
            const float crossBottom = static_cast<float>(m_pos.y() + m_size.y() - 5);
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, crossLeft, crossTop);
            nvgLineTo(ctx, crossRight, crossBottom);
            nvgMoveTo(ctx, crossLeft, crossBottom);
            nvgLineTo(ctx, crossRight, crossTop);
            nvgStrokeWidth(ctx, 1.5f);
            nvgStrokeColor(ctx, m_mouse_focus ? Color(255, 255) : Color(180, 220));
            nvgStroke(ctx);
        }
        return;
    }

    const string_view caption = mCaptionLeftAligned ? string_view{mCaption}.substr(0, mCaption.size() - mCutoff) :
                                                      string_view{mCaption}.substr(mCutoff);
    vector<string_view> pieces;
    if (mCaptionLeftAligned) {
        pieces.emplace_back(caption);
        if (mCutoff > 0 && mCutoff < mCaption.size()) {
            pieces.emplace_back("…");
        }
    } else {
        if (mHighlightBegin <= mCutoff) {
            if (mHighlightEnd <= mCutoff) {
                pieces.emplace_back(caption);
            } else {
                const size_t offset = mHighlightEnd - mCutoff;
                pieces.emplace_back(caption.substr(offset));
                pieces.emplace_back(caption.substr(0, offset));
            }
        } else {
            const size_t beginOffset = mHighlightBegin - mCutoff;
            const size_t endOffset = mHighlightEnd - mCutoff;
            pieces.emplace_back(caption.substr(endOffset));
            pieces.emplace_back(caption.substr(beginOffset, endOffset - beginOffset));
            pieces.emplace_back(caption.substr(0, beginOffset));
        }

        if (mCutoff > 0 && mCutoff < mCaption.size()) {
            pieces.emplace_back("…");
        }
    }

    const Vector2f center = Vector2f{m_pos} + Vector2f{m_size} * 0.5f;
    const Vector2f bottomRight = Vector2f{m_pos} + Vector2f{m_size};
    Vector2f textPos = mCaptionLeftAligned ? Vector2f{m_pos.x() + 19.0f + nvgTextBounds(ctx, 0, 0, idString.data(), idString.data() + idString.size(), nullptr), center.y() + 0.5f * (m_font_size + 1)}
                                           : Vector2f{bottomRight.x() - 5 - closeReserve, center.y() + 0.5f * (m_font_size + 1)};
    NVGcolor regularTextColor = mCanBeReference ? Color(150, 255) : Color(190, 255);
    NVGcolor hightlightedTextColor = Color(190, 255);
    if (mIsSelected || mIsReference || m_mouse_focus) {
        regularTextColor = hightlightedTextColor = Color(255, 255);
    }

    nvgFontSize(ctx, m_font_size);
    nvgTextAlign(ctx, (mCaptionLeftAligned ? NVG_ALIGN_LEFT : NVG_ALIGN_RIGHT) | NVG_ALIGN_BOTTOM);

    if (middleCut) {
        const string text = mHeadEnd < mTailBegin
            ? mCaption.substr(mHeadBegin, mHeadEnd - mHeadBegin) + "…" + mCaption.substr(mTailBegin)
            : mCaption.substr(mHeadBegin);
        nvgFontFace(ctx, middleCutFace);
        nvgFillColor(ctx, wholeHighlight ? hightlightedTextColor : regularTextColor);
        nvgText(ctx, textPos.x(), textPos.y(), text.data(), text.data() + text.size());
    } else if (mCaptionLeftAligned) {
        for (size_t i = 0; i < pieces.size(); ++i) {
            nvgFontFace(ctx, "sans");
            nvgFillColor(ctx, regularTextColor);
            nvgText(ctx, textPos.x(), textPos.y(), pieces[i].data(), pieces[i].data() + pieces[i].size());
            textPos.x() += nvgTextBounds(ctx, 0, 0, pieces[i].data(), pieces[i].data() + pieces[i].size(), nullptr);
        }
    } else {
        for (size_t i = 0; i < pieces.size(); ++i) {
            nvgFontFace(ctx, i == 1 ? "sans-bold" : "sans");
            nvgFillColor(ctx, i == 1 ? hightlightedTextColor : regularTextColor);
            nvgText(ctx, textPos.x(), textPos.y(), pieces[i].data(), pieces[i].data() + pieces[i].size());
            textPos.x() -= nvgTextBounds(ctx, 0, 0, pieces[i].data(), pieces[i].data() + pieces[i].size(), nullptr);
        }
    }

    // Image number
    NVGcolor idColor = Color(200, 255);
    if (mIsSelected || mIsReference || m_mouse_focus) {
        idColor = Color(255, 255);
    }

    nvgFontSize(ctx, m_font_size + 2);
    nvgFontFace(ctx, "sans-bold");
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
    nvgFillColor(ctx, idColor);
    nvgText(ctx, m_pos.x() + 5, textPos.y(), idString.data(), idString.data() + idString.size());

    if (mCloseCallback) {
        const float crossRight = static_cast<float>(m_pos.x() + m_size.x() - 7);
        const float crossLeft = crossRight - 8.0f;
        const float crossTop = static_cast<float>(m_pos.y() + 5);
        const float crossBottom = static_cast<float>(m_pos.y() + m_size.y() - 5);
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, crossLeft, crossTop);
        nvgLineTo(ctx, crossRight, crossBottom);
        nvgMoveTo(ctx, crossLeft, crossBottom);
        nvgLineTo(ctx, crossRight, crossTop);
        nvgStrokeWidth(ctx, 1.5f);
        nvgStrokeColor(ctx, m_mouse_focus ? Color(255, 255) : Color(180, 220));
        nvgStroke(ctx);
    }
}

void ImageButton::setHighlightRange(size_t begin, size_t end) {
    size_t beginIndex = begin;
    if (end > mCaption.size()) {
        throw invalid_argument{format("end ({}) must not be larger than mCaption.size() ({})", end, mCaption.size())};
    }

    mHighlightBegin = beginIndex;
    mHighlightEnd = std::max(mCaption.size() - end, beginIndex);
    mSizeForWhichCutoffWasComputed = {0};

    if (mHighlightBegin == mHighlightEnd || mCaption.empty()) {
        return;
    }

    // Extend beginning and ending of highlighted region to entire word/number
    if (isalnum(mCaption[mHighlightBegin])) {
        while (mHighlightBegin > 0 && isalnum(mCaption[mHighlightBegin - 1])) {
            --mHighlightBegin;
        }
    }

    if (isalnum(mCaption[mHighlightEnd - 1])) {
        while (mHighlightEnd < mCaption.size() && isalnum(mCaption[mHighlightEnd])) {
            ++mHighlightEnd;
        }
    }
}

void ImageButton::showTextBox() {
    mCaptionTextBox->set_visible(true);
    mCaptionTextBox->request_focus();
    mCaptionTextBox->select_all();
}

void ImageButton::hideTextBox() {
    if (!textBoxVisible()) {
        return;
    }

    mCaptionTextBox->set_focused(false);
    this->setCaption(mCaptionTextBox->value());
    mCaptionTextBox->set_visible(false);
}

void ImageButton::setThumbnail(vector<uint8_t> rgba, Vector2i size) {
    mThumbnailRgba = std::move(rgba);
    mThumbnailSize = size;
    mThumbnailDirty = true;
    preferred_size_changed();
}

void ImageButton::clearThumbnail() {
    mThumbnailRgba.clear();
    mThumbnailSize = {0, 0};
    if (mThumbnailImageHandle != 0 && screen() && screen()->nvg_context()) {
        nvgDeleteImage(screen()->nvg_context(), mThumbnailImageHandle);
        mThumbnailImageHandle = 0;
    }
    mThumbnailDirty = false;
    preferred_size_changed();
}

void ImageButton::setShowCaption(bool showCaption) {
    if (mShowCaption == showCaption) {
        return;
    }
    mShowCaption = showCaption;
    preferred_size_changed();
}

} // namespace tev
