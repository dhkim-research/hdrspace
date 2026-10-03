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

#include <tev/Common.h>
#include <tev/FalseColor.h>
#include <tev/ImageCanvas.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>

#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/theme.h>
#include <nanogui/vector.h>

#include <chrono>
#include <cctype>
#include <limits>
#include <set>
#include <span>

using namespace nanogui;
using namespace std;

namespace tev {

namespace {

Vector3f clampToHdrDomain(const Vector3f& value) {
    return {
        std::max(0.0f, value.x()),
        std::max(0.0f, value.y()),
        std::max(0.0f, value.z()),
    };
}

Matrix3f currentInspectionMatrix(const optional<Matrix3f>& overrideMatrix, const chroma_t& chroma, bool adaptWhitePoint) {
    if (overrideMatrix.has_value()) {
        return *overrideMatrix;
    }

    return convertColorspaceMatrix(
        rec709Chroma(), chroma, adaptWhitePoint ? ERenderingIntent::RelativeColorimetric : ERenderingIntent::AbsoluteColorimetric
    );
}

shared_ptr<Image> inspectionPreviewSourceImage(
    const shared_ptr<Image>& image,
    const shared_ptr<Image>& currentImage,
    const shared_ptr<Image>& previewImage
) {
    if (image == currentImage && previewImage) {
        return previewImage;
    }

    return image;
}

} // namespace

ImageCanvas::ImageCanvas(Widget* parent) : Canvas{parent, 1, false, false, false} {
    // If we are rendering to a float buffer (which is the case if the screen has a float back buffer, or if the screen performs color
    // management), we don't need to dither here. The screen will do it. Otherwise, we are rendering directly to an integer buffer and we
    // *should* dither to avoid banding artifacts.
    auto* screen = this->screen();
    const float ditherScale = (screen->has_float_buffer() || screen->applies_color_management()) ?
        0.0f :
        (1.0f / (1u << screen->bits_per_sample()));

    mShader.reset(new UberShader{render_pass(), ditherScale});
    set_draw_border(false);
}

bool ImageCanvas::scroll_event(const Vector2i& p, const Vector2f& rel) {
    if (Canvas::scroll_event(p, rel)) {
        return true;
    }

    float scaleAmount = rel.y();
    auto* glfwWindow = screen()->glfw_window();
    // There is no explicit access to the currently pressed modifier keys here, so we need to directly ask GLFW.
    if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) {
        scaleAmount /= 8;
    } else if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_CONTROL) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_CONTROL)) {
        scaleAmount *= 8;
    }

    scale(scaleAmount, Vector2f{p});
    return true;
}

void ImageCanvas::draw_contents() {
    auto* glfwWindow = screen()->glfw_window();
    bool viewReferenceOnly = glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT);
    bool viewImageOnly = glfwGetKey(glfwWindow, GLFW_KEY_LEFT_CONTROL) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_CONTROL);
    if (viewReferenceOnly && viewImageOnly) {
        // If both modifiers are pressed at the same time, we want entirely different behavior from modifying which image is shown. Do
        // nothing here.
        viewReferenceOnly = viewImageOnly = false;
    }

    Image* image = (mReference && viewReferenceOnly) ? mReference.get() : mImage.get();

    optional<Box2i> imageSpaceCrop = nullopt;
    if (image && mCrop.has_value()) {
        imageSpaceCrop = mCrop.value().translate(image->displayWindow().min - image->dataWindow().min);
    }

    viewImageOnly |= !mReference || image == mReference.get();

    Image* reference = (viewImageOnly || !mReference || image == mReference.get()) ? nullptr : mReference.get();
    const auto displayedImage = image == mReference.get() ? mReference : mImage;
    const auto displayedInspectionImage = inspectionPreviewSourceImage(displayedImage, mImage, mInspectionPreviewImage);
    Texture* inspectionImageTexture =
        (mDisplayInspectionInPreview && displayedInspectionImage) ? inspectionPreviewTexture(displayedInspectionImage) : nullptr;
    Texture* inspectionReferenceTexture =
        (mDisplayInspectionInPreview && mReference && reference) ? inspectionPreviewTexture(mReference) : nullptr;
    const bool useInspectionTextures = inspectionImageTexture != nullptr;
    Texture* analysisOverlay = analysisOverlayTexture(displayedImage);
    Texture* cpuHighlightMaskTexture = analysisOverlay ? analysisOverlay : highlightMaskTexture();

    mShader->draw(
        2.0f * inverse(Vector2f{m_size}) / mPixelRatio,
        Vector2f{20.0f},
        image,
        // The uber shader operates in [-1, 1] coordinates and requires the _inserve_ image transform to obtain texture coordinates in [0,
        // 1]-space.
        inverse(transform(mImage.get())),
        reference,
        inverse(transform(mReference.get())),
        mRequestedChannelGroup,
        mMinFilter,
        mMagFilter,
        mExposure,
        mOffset,
        mGamma,
        mWhiteLevelOverride ? (*mWhiteLevelOverride / glfwGetWindowSdrWhiteLevel(glfwWindow)) : 1.0f,
        mClipToLdr,
        mBackgroundColor,
        mTonemap,
        static_cast<int>(mFalseColorMap),
        static_cast<int>(mFalseColorScaleMode),
        mFalseColorRange,
        mMetric,
        mChannelMask,
        mHighlightValueRange,
        static_cast<int>(mHighlightMode),
        useInspectionTextures ? 1.0f : mInspectionScale,
        useInspectionTextures ? Matrix3f{1.0f} : inspectionMatrix(),
        useInspectionTextures ? static_cast<int>(ituth273::ETransfer::Linear) : static_cast<int>(mInspectionTransfer),
        useInspectionTextures ? static_cast<int>(EInspectionValueMode::Channels) : static_cast<int>(mInspectionValueMode),
        useInspectionTextures ? false : mDisplayInspectionInPreview,
        imageSpaceCrop,
        inspectionImageTexture,
        inspectionReferenceTexture,
        useInspectionTextures ? 3 : 0,
        cpuHighlightMaskTexture
    );
}

void ImageCanvas::drawPixelValuesAsText(NVGcontext* ctx) {
    TEV_ASSERT(mImage, "Can only draw pixel values if there exists an image.");

    auto texToNano = textureToNanogui(mImage.get());
    auto nanoToTex = inverse(texToNano);

    Vector2f pixelSize = texToNano * Vector2f{1.0f} - texToNano * Vector2f{0.0f};

    Vector2f topLeft = (nanoToTex * Vector2f{0.0f});
    Vector2f bottomRight = (nanoToTex * Vector2f{m_size});

    Vector2i startIndices = Vector2i{
        static_cast<int>(floor(topLeft.x())),
        static_cast<int>(floor(topLeft.y())),
    };

    Vector2i endIndices = Vector2i{
        static_cast<int>(ceil(bottomRight.x())),
        static_cast<int>(ceil(bottomRight.y())),
    };

    if (pixelSize.x() > 50 && pixelSize.x() < 1024) {
        const auto channelsSpan = mImage->channelsInGroup(mRequestedChannelGroup);
        vector<string_view> channels(begin(channelsSpan), end(channelsSpan));

        // Remove duplicates
        channels.erase(unique(begin(channels), end(channels)), end(channels));
        if (channels.empty()) {
            return;
        }

        // Only treat alpha specially if it's not the only channel
        const bool hasAlpha = channels.size() > 1 && Channel::isAlpha(channels.back());

        vector<Color> baseColors;
        for (const auto& channel : channels) {
            baseColors.emplace_back(Channel::color(channel, true));
        }

        float fontSize = pixelSize.x() / 6;
        if (baseColors.size() > 4) {
            fontSize *= 4.0f / baseColors.size();
        }

        const float fontAlpha = std::min(std::min(1.0f, (pixelSize.x() - 50) / 30), (1024 - pixelSize.x()) / 256);

        nvgFontSize(ctx, fontSize);
        nvgFontFace(ctx, "sans");
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

        auto* glfwWindow = screen()->glfw_window();
        bool shiftAndControlHeld = (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) &&
            (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_CONTROL) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_CONTROL));

        Vector2i cur;
        vector<float> values;
        for (cur.y() = startIndices.y(); cur.y() < endIndices.y(); ++cur.y()) {
            for (cur.x() = startIndices.x(); cur.x() < endIndices.x(); ++cur.x()) {
                Vector2i nano = Vector2i{texToNano * (Vector2f{cur} + Vector2f{0.5f})};
                if (shiftAndControlHeld) {
                    getValuesAtNanoPos(nano, values, channels);
                } else {
                    getDisplayedValuesAtNanoPos(nano, values, channels);
                }

                auto colors = baseColors;

                // If shift and control are held, we display sRGB hex values
                if (shiftAndControlHeld) {
                    TEV_ASSERT(values.size() == baseColors.size(), "Can not have more values than channels.");
                    for (size_t i = 0; i < colors.size(); ++i) {
                        values[i] = hasAlpha && i == colors.size() - 1 ? values[i] : toSRGB(values[i]);
                    }
                } else {
                    if (mInspectionValueMode == EInspectionValueMode::Luminance && colors.size() >= 3) {
                        values = {values.front()};
                        colors = {Color(255, 255, 255, 255)};
                    } else if (mInspectionValueMode == EInspectionValueMode::YChannel && colors.size() >= 3) {
                        values = {values.size() > 1 ? values[1] : values.front()};
                        colors = {Color(255, 255, 255, 255)};
                    }
                }

                for (size_t i = 0; i < colors.size(); ++i) {
                    string str;
                    Vector2f pos;

                    if (shiftAndControlHeld) {
                        const unsigned char discretizedValue = (char)(clamp(values[i], 0.0f, 1.0f) * 255 + 0.5f);
                        str = format("{:02X}", discretizedValue);
                        pos = Vector2f{
                            m_pos.x() + nano.x() + (i - 0.5f * (colors.size() - 1)) * fontSize * 0.88f,
                            (float)m_pos.y() + nano.y(),
                        };
                    } else {
                        str = abs(values[i]) > 100000 ? format("{:6g}", values[i]) : format("{:.5f}", values[i]);
                        pos = Vector2f{
                            (float)m_pos.x() + nano.x(),
                            m_pos.y() + nano.y() + (i - 0.5f * (colors.size() - 1)) * fontSize,
                        };
                    }

                    const Color col = colors[i];
                    nvgFillColor(ctx, Color(col.r(), col.g(), col.b(), fontAlpha));
                    drawTextWithShadow(ctx, pos.x(), pos.y(), str, fontAlpha);
                }
            }
        }
    }
}

void ImageCanvas::drawCoordinateSystem(NVGcontext* ctx) {
    TEV_ASSERT(mImage, "Can only draw coordinate system if there exists an image.");

    const auto displayWindowToNano = displayWindowToNanogui(mImage.get());

    enum DrawFlags {
        Label = 1,
        Region = 2,
    };

    const auto drawWindow = [&](Box2f window, Color color, bool top, bool right, string_view name, DrawFlags flags) {
        float fontSize = 20;
        float strokeWidth = 3.0f;

        Vector2i topLeft = m_pos +
            Vector2i{
                displayWindowToNano * Vector2f{window.min.x(), window.min.y()}
        };
        Vector2i topRight = m_pos +
            Vector2i{
                displayWindowToNano * Vector2f{window.max.x(), window.min.y()}
        };
        Vector2i bottomLeft = m_pos +
            Vector2i{
                displayWindowToNano * Vector2f{window.min.x(), window.max.y()}
        };
        Vector2i bottomRight = m_pos +
            Vector2i{
                displayWindowToNano * Vector2f{window.max.x(), window.max.y()}
        };

        nvgSave(ctx);

        nvgFontFace(ctx, "sans-bold");
        nvgFontSize(ctx, fontSize);
        nvgTextAlign(ctx, (right ? NVG_ALIGN_RIGHT : NVG_ALIGN_LEFT) | (top ? NVG_ALIGN_BOTTOM : NVG_ALIGN_TOP));
        float textWidth = nvgTextBounds(ctx, 0, 0, name.data(), name.data() + name.size(), nullptr);
        float textAlpha = std::max(std::min(1.0f, (((topRight.x() - topLeft.x() - textWidth - 5) / 30))), 0.0f);
        float regionAlpha = std::max(std::min(1.0f, (((topRight.x() - topLeft.x() - textWidth - 5) / 30))), 0.0f);

        Color textColor = Color(190, 255);
        textColor.a() = textAlpha;

        if (flags & Region) {
            color.a() = regionAlpha;

            nvgBeginPath(ctx);
            nvgMoveTo(ctx, bottomLeft.x(), bottomLeft.y());
            nvgLineTo(ctx, topLeft.x(), topLeft.y());
            nvgLineTo(ctx, topRight.x(), topRight.y());
            nvgLineTo(ctx, bottomRight.x(), bottomRight.y());
            nvgLineTo(ctx, bottomLeft.x(), bottomLeft.y());
            nvgStrokeWidth(ctx, strokeWidth);
            nvgStrokeColor(ctx, color);
            nvgStroke(ctx);
        }

        if (!name.empty() && (flags & Label)) {
            color.a() = textAlpha;

            nvgBeginPath(ctx);
            nvgFillColor(ctx, color);

            float cornerRadius = fontSize / 3;
            float topLeftCornerRadius = top && right ? cornerRadius : 0;
            float topRightCornerRadius = top && !right ? cornerRadius : 0;
            float bottomLeftCornerRadius = !top && right ? cornerRadius : 0;
            float bottomRightCornerRadius = !top && !right ? cornerRadius : 0;

            nvgRoundedRectVarying(
                ctx,
                right ? (topRight.x() - textWidth - 4 * strokeWidth) : topLeft.x() - strokeWidth / 2,
                topLeft.y() - (top ? fontSize : 0),
                textWidth + 4 * strokeWidth,
                fontSize,
                topLeftCornerRadius,
                topRightCornerRadius,
                bottomRightCornerRadius,
                bottomLeftCornerRadius
            );
            nvgFill(ctx);

            nvgFillColor(ctx, textColor);
            nvgText(
                ctx,
                right ? (topRight.x() - 2 * strokeWidth) : (topLeft.x() + 2 * strokeWidth) - strokeWidth / 2,
                topLeft.y(),
                name.data(),
                name.data() + name.size()
            );
        }

        nvgRestore(ctx);
    };

    const auto draw = [&](DrawFlags flags) {
        if (mReference) {
            if (mReference->dataWindow() != mImage->dataWindow()) {
                drawWindow(
                    mReference->dataWindow(),
                    REFERENCE_COLOR,
                    mReference->displayWindow().min.y() > mReference->dataWindow().min.y(),
                    true,
                    "Reference data window",
                    flags
                );
            }

            if (mReference->displayWindow() != mImage->displayWindow()) {
                drawWindow(
                    mReference->displayWindow(),
                    REFERENCE_COLOR,
                    mReference->displayWindow().min.y() <= mReference->dataWindow().min.y(),
                    true,
                    "Reference display window",
                    flags
                );
            }
        }

        if (mImage->dataWindow() != mImage->displayWindow()) {
            drawWindow(
                mImage->dataWindow(), IMAGE_COLOR, mImage->displayWindow().min.y() > mImage->dataWindow().min.y(), false, "Data window", flags
            );
            drawWindow(
                mImage->displayWindow(),
                Color(0.3f, 1.0f),
                mImage->displayWindow().min.y() <= mImage->dataWindow().min.y(),
                false,
                "Display window",
                flags
            );
        } else {
            drawWindow(
                mImage->displayWindow(), Color(0.3f, 1.0f), mImage->displayWindow().min.y() <= mImage->dataWindow().min.y(), false, "", flags
            );
        }

        if (mCrop.has_value()) {
            drawWindow(mCrop.value(), CROP_COLOR, false, false, "Crop", flags);
        }

        if (mRoi.has_value()) {
            drawWindow(mRoi.value(), Color{0.08f, 0.85f, 0.95f, 1.0f}, true, false, "ROI", flags);
        }

    };

    // Draw all labels after the regions to ensure no occlusion
    draw(Region);
    draw(Label);
}

void ImageCanvas::drawEdgeShadows(NVGcontext* ctx) {
    const int ds = m_theme->m_window_drop_shadow_size, cr = m_theme->m_window_corner_radius;
    NVGpaint shadowPaint =
        nvgBoxGradient(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y(), cr * 2, ds * 2, m_theme->m_transparent, m_theme->m_drop_shadow);

    nvgSave(ctx);
    nvgResetScissor(ctx);
    nvgBeginPath(ctx);
    nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
    nvgRoundedRect(ctx, m_pos.x() + ds, m_pos.y() + ds, m_size.x() - 2 * ds, m_size.y() - 2 * ds, cr);
    nvgPathWinding(ctx, NVG_HOLE);
    nvgFillPaint(ctx, shadowPaint);
    nvgFill(ctx);
    nvgRestore(ctx);
}

void ImageCanvas::draw(NVGcontext* ctx) {
    Canvas::draw(ctx);

    if (mImage) {
        drawPixelValuesAsText(ctx);

        const auto displayWindowToNano = displayWindowToNanogui(mImage.get());

        const auto vgToNano = [&](const Vector2f p) { return Vector2f{m_pos} + displayWindowToNano * p; };
        const auto applyVgCommand = [&](const VgCommand& command) {
            const float* f = command.data.data();
            switch (command.type) {
                // State
                case VgCommand::EType::Save: nvgSave(ctx); return;
                case VgCommand::EType::Restore: nvgRestore(ctx); return;
                // Draw calls
                case VgCommand::EType::FillColor: nvgFillColor(ctx, {{{f[0], f[1], f[2], f[3]}}}); return;
                case VgCommand::EType::Fill: nvgFill(ctx); return;
                case VgCommand::EType::StrokeColor: nvgStrokeColor(ctx, {{{f[0], f[1], f[2], f[3]}}}); return;
                case VgCommand::EType::Stroke: nvgStroke(ctx); return;
                // Path control
                case VgCommand::EType::BeginPath: nvgBeginPath(ctx); return;
                case VgCommand::EType::ClosePath: nvgClosePath(ctx); return;
                case VgCommand::EType::PathWinding: nvgPathWinding(ctx, (int)f[0]); return;
                case VgCommand::EType::DebugDumpPathCache: nvgDebugDumpPathCache(ctx); return;
                // Path construction
                case VgCommand::EType::MoveTo: {
                    Vector2f p = vgToNano({f[0], f[1]});
                    nvgMoveTo(ctx, p.x(), p.y());
                }
                    return;
                case VgCommand::EType::LineTo: {
                    Vector2f p = vgToNano({f[0], f[1]});
                    nvgLineTo(ctx, p.x(), p.y());
                }
                    return;
                case VgCommand::EType::ArcTo: {
                    Vector2f p1 = vgToNano({f[0], f[1]});
                    Vector2f p2 = vgToNano({f[2], f[3]});
                    float radius = f[4] * extractScale(displayWindowToNano);
                    nvgArcTo(ctx, p1.x(), p1.y(), p2.x(), p2.y(), radius);
                }
                    return;
                case VgCommand::EType::Arc: {
                    Vector2f c = vgToNano({f[0], f[1]});
                    float radius = f[2] * extractScale(displayWindowToNano);
                    nvgArc(ctx, c.x(), c.y(), radius, f[3], f[4], (int)f[5]);
                }
                    return;
                case VgCommand::EType::BezierTo: {
                    Vector2f c1 = vgToNano({f[0], f[1]});
                    Vector2f c2 = vgToNano({f[2], f[3]});
                    Vector2f p = vgToNano({f[4], f[5]});
                    nvgBezierTo(ctx, c1.x(), c1.y(), c2.x(), c2.y(), p.x(), p.y());
                }
                    return;
                case VgCommand::EType::Circle: {
                    Vector2f c = vgToNano({f[0], f[1]});
                    float radius = f[2] * extractScale(displayWindowToNano);
                    nvgCircle(ctx, c.x(), c.y(), radius);
                }
                    return;
                case VgCommand::EType::Ellipse: {
                    Vector2f c = vgToNano({f[0], f[1]});
                    Vector2f r = extract2x2(displayWindowToNano) * Vector2f{f[2], f[3]};
                    nvgEllipse(ctx, c.x(), c.y(), r.x(), r.y());
                }
                    return;
                case VgCommand::EType::QuadTo:

                {
                    Vector2f c = vgToNano({f[0], f[1]});
                    Vector2f p = vgToNano({f[2], f[3]});
                    nvgQuadTo(ctx, c.x(), c.y(), p.x(), p.y());
                }

                    return;
                case VgCommand::EType::Rect: {
                    Vector2f p = vgToNano({f[0], f[1]});
                    Vector2f size = extract2x2(displayWindowToNano) * Vector2f{f[2], f[3]};
                    nvgRect(ctx, p.x(), p.y(), size.x(), size.y());
                }
                    return;
                case VgCommand::EType::RoundedRect: {
                    Vector2f p = vgToNano({f[0], f[1]});
                    Vector2f size = extract2x2(displayWindowToNano) * Vector2f{f[2], f[3]};
                    float radius = f[4] * extractScale(displayWindowToNano);
                    nvgRoundedRect(ctx, p.x(), p.y(), size.x(), size.y(), radius);
                }
                    return;
                case VgCommand::EType::RoundedRectVarying: {
                    Vector2f p = vgToNano({f[0], f[1]});
                    Vector2f size = extract2x2(displayWindowToNano) * Vector2f{f[2], f[3]};
                    float scale = extractScale(displayWindowToNano);
                    nvgRoundedRectVarying(ctx, p.x(), p.y(), size.x(), size.y(), f[4] * scale, f[5] * scale, f[6] * scale, f[7] * scale);
                }
                    return;
                // TODO: text rendering
                default: throw runtime_error{"Invalid VgCommand type."};
            }
        };

        // Draw image-specific vector graphics overlay for both the currently selected image as well as the reference.
        const auto applyVgCommandsSandboxed = [&](const Color& defaultColor, span<const VgCommand> commands) {
            nvgSave(ctx);

            nvgFillColor(ctx, defaultColor);
            nvgStrokeColor(ctx, defaultColor);
            nvgStrokeWidth(ctx, 3.0f);

            size_t saveCounter = 0;
            for (const auto& command : commands) {
                if (command.type == VgCommand::EType::Save) {
                    ++saveCounter;
                } else if (command.type == VgCommand::EType::Restore) {
                    if (saveCounter == 0) {
                        tlog::warning("Malformed vector graphics commands: restore before save");
                        continue;
                    }

                    --saveCounter;
                }

                applyVgCommand(command);
            }

            if (saveCounter > 0) {
                tlog::warning("Malformed vector graphics commands: missing restore after save");
                for (size_t i = 0; i < saveCounter; ++i) {
                    nvgRestore(ctx);
                }
            }

            nvgRestore(ctx);
        };

        if (mReference && !mReference->vgCommands().empty()) {
            applyVgCommandsSandboxed(REFERENCE_COLOR, mReference->vgCommands());
        }

        if (!mImage->vgCommands().empty()) {
            applyVgCommandsSandboxed(IMAGE_COLOR, mImage->vgCommands());
        }

        // If the coordinate system is in any sort of way non-trivial, or if a hotkey is held, draw it!
        if (glfwGetKey(screen()->glfw_window(), GLFW_KEY_B) || mCrop.has_value() || mRoi.has_value() ||
            mImage->dataWindow() != mImage->displayWindow() ||
            mImage->displayWindow().min != Vector2i{0} ||
            (mReference && (mReference->dataWindow() != mImage->dataWindow() || mReference->displayWindow() != mImage->displayWindow()))) {
            drawCoordinateSystem(ctx);
        }

        if (!mAiMaskPromptPoints.empty()) {
            drawAiMaskPromptPoints(ctx);
        }
    }

    // If we're not in fullscreen mode draw an inner drop shadow. (adapted from Window)
    if (m_pos.x() != 0) {
        drawEdgeShadows(ctx);
    }
}

void ImageCanvas::translate(Vector2f amount) { mTransform = Matrix3f::translate(amount) * mTransform; }

void ImageCanvas::scale(float amount, Vector2f origin) {
    static const double BASE_SCALE = sqrt(sqrt(sqrt(2.0)));
    const float scaleFactor = (float)pow(BASE_SCALE, (double)amount);

    // Use the current cursor position as the origin to scale around.
    Vector2f offset = -(origin - Vector2f{position()}) + 0.5f * Vector2f{m_size};
    auto scaleTransform = Matrix3f::translate(-offset) * Matrix3f::scale(Vector2f{scaleFactor}) * Matrix3f::translate(offset);

    mTransform = scaleTransform * mTransform;
}

Vector2i ImageCanvas::getImageCoords(const Image* image, Vector2i nanoPos) {
    Vector2f imagePos = inverse(textureToNanogui(image)) * Vector2f{nanoPos};
    return {
        static_cast<int>(floor(imagePos.x())),
        static_cast<int>(floor(imagePos.y())),
    };
}

Vector2i ImageCanvas::getDisplayWindowCoords(const Image* image, Vector2i nanoPos) {
    Vector2f imageCoords = getImageCoords(image, nanoPos);
    if (image) {
        imageCoords += Vector2f(image->dataWindow().min - image->displayWindow().min);
    }

    return imageCoords;
}

void ImageCanvas::getValuesAtNanoPos(Vector2i nanoPos, vector<float>& result, span<string_view> channels) {
    result.clear();
    if (!mImage) {
        return;
    }

    const auto imageCoords = getImageCoords(mImage.get(), nanoPos);
    for (const auto& channel : channels) {
        const Channel* c = mImage->hasSourceChannels() ? mImage->sourceChannel(channel) : mImage->channel(channel);
        TEV_ASSERT(c, "Requested channel must exist.");
        result.push_back(c->evalOrZero(imageCoords));
    }

    // Subtract reference if it exists.
    if (mReference) {
        const auto referenceCoords = getImageCoords(mReference.get(), nanoPos);
        for (size_t i = 0; i < result.size(); ++i) {
            const bool isAlpha = Channel::isAlpha(channels[i]);
            const float defaultVal = isAlpha && mReference->contains(referenceCoords) ? 1.0f : 0.0f;

            const Channel* c = mReference->hasSourceChannels() ? mReference->sourceChannel(channels[i]) : mReference->channel(channels[i]);
            const float reference = c ? c->evalOrZero(referenceCoords) : defaultVal;

            result[i] = isAlpha ? 0.5f * (result[i] + reference) : applyMetric(result[i], reference, mMetric);
        }
    }
}

void ImageCanvas::getDisplayedValuesAtNanoPos(Vector2i nanoPos, vector<float>& result, span<string_view> channels) {
    result.clear();

    if (mImage) {
        Vector4f previewValues{0.0f};
        const auto imageCoords = getImageCoords(mImage.get(), nanoPos);
        if (sampleInspectionPreviewAtImageCoords(inspectionPreviewSourceImage(mImage, mImage, mInspectionPreviewImage), imageCoords, previewValues)) {
            if (mReference) {
                Vector4f referenceValues{0.0f};
                const auto referenceCoords = getImageCoords(mReference.get(), nanoPos);
                if (sampleInspectionPreviewAtImageCoords(mReference, referenceCoords, referenceValues)) {
                    previewValues[0] = applyMetric(previewValues[0], referenceValues[0], mMetric);
                    previewValues[1] = applyMetric(previewValues[1], referenceValues[1], mMetric);
                    previewValues[2] = applyMetric(previewValues[2], referenceValues[2], mMetric);
                    previewValues[3] = 0.5f * (previewValues[3] + referenceValues[3]);
                }
            }

            for (const auto& channel : channels) {
                if (Channel::isAlpha(channel)) {
                    result.push_back(previewValues[3]);
                    continue;
                }

                const auto tail = Channel::tail(channel);
                const char name = tail.empty() ? 'R' : (char)std::toupper((unsigned char)tail.front());
                size_t idx = 0;
                switch (name) {
                    case 'G':
                    case 'Y': idx = 1; break;
                    case 'B':
                    case 'Z': idx = 2; break;
                    default: idx = 0; break;
                }
                result.push_back(previewValues[idx]);
            }

            if (mInspectionValueMode == EInspectionValueMode::Luminance) {
                result.assign(1, result.empty() ? 0.0f : result.front());
            } else if (mInspectionValueMode == EInspectionValueMode::YChannel) {
                const float y = result.size() > 1 ? result[1] : (result.empty() ? 0.0f : result.front());
                result.assign(1, y);
            }
            return;
        }
    }

    getValuesAtNanoPos(nanoPos, result, channels);
    const bool hasAlpha = result.size() > 1 && !channels.empty() && Channel::isAlpha(channels.back());
    applyInspectionParameters(result, hasAlpha);
}

Box2i ImageCanvas::cropInImageCoords() const {
    if (!mImage) {
        return Box2i{0};
    }

    const auto region = mImage->toImageCoords(mCrop.has_value() ? *mCrop : mImage->displayWindow());
    if (!region.isValid()) {
        return Box2i{0};
    }

    return region;
}

Box2i ImageCanvas::roiInImageCoords() const {
    if (!mImage) {
        return Box2i{0};
    }

    const auto region = mImage->toImageCoords(mRoi.has_value() ? *mRoi : Box2i{0});
    if (!mRoi.has_value() || !region.isValid()) {
        return Box2i{0};
    }

    return region;
}

Box2i ImageCanvas::statisticsRegionInImageCoords() const {
    if (mRoi.has_value()) {
        const auto roiRegion = roiInImageCoords();
        if (roiRegion.isValid()) {
            return roiRegion;
        }
    }

    return cropInImageCoords();
}

void ImageCanvas::fitImageToScreen(const Image& image) {
    Vector2f nanoguiImageSize = Vector2f{image.displayWindow().size()} / mPixelRatio;
    mTransform = Matrix3f::scale(Vector2f{std::min(m_size.x() / nanoguiImageSize.x(), m_size.y() / nanoguiImageSize.y())});
}

void ImageCanvas::resetTransform() { mTransform = Matrix3f::scale(Vector2f{1.0f}); }

void ImageCanvas::invalidateCanvasStatistics() {
    invalidateStatisticsOnly();
    invalidateInspectionPreviewCache();
    redrawWindow();
}

void ImageCanvas::invalidateStatisticsOnly() {
    mCanvasStatistics.clear();
    mImageIdToCanvasStatisticsKey.clear();
}

void ImageCanvas::invalidateInspectionPreviewCache() {
    mInspectionPreviewCache = {};
    mInspectionReferencePreviewCache = {};
    invalidateHighlightMaskCache();
}

void ImageCanvas::invalidateHighlightMaskCache() {
    mHighlightMaskCache = {};
}

Texture* ImageCanvas::inspectionPreviewTexture(const shared_ptr<Image>& image) {
    if (!image) {
        return nullptr;
    }

    auto& cache = (image == mImage) ? mInspectionPreviewCache : mInspectionReferencePreviewCache;
    const auto matrix = inspectionMatrix();
    const auto transfer = mInspectionTransfer;
    const auto requestedGroup = mRequestedChannelGroup;
    const bool premultipliedAlpha = mInspectionPremultipliedAlpha;
    const float scale = mInspectionScale;
    const auto valueMode = mInspectionValueMode;
    const int displayChannelSelection = mInspectionDisplayChannelSelection;

    if (cache.image == image && cache.texture && cache.requestedGroup == requestedGroup && cache.matrix == matrix &&
        cache.transfer == transfer && cache.premultipliedAlpha == premultipliedAlpha && cache.scale == scale &&
        cache.valueMode == valueMode && cache.displayChannelSelection == displayChannelSelection) {
        return cache.texture.get();
    }

    auto dataPtr = createInspectionPreviewPixels(image, requestedGroup, displayChannelSelection);
    if (!dataPtr) {
        return nullptr;
    }
    const auto size = image->size();

    cache = {};
    cache.image = image;
    cache.requestedGroup = requestedGroup;
    cache.matrix = matrix;
    cache.transfer = transfer;
    cache.premultipliedAlpha = premultipliedAlpha;
    cache.scale = scale;
    cache.valueMode = valueMode;
    cache.displayChannelSelection = displayChannelSelection;
    cache.pixels = dataPtr;
    cache.texture = new Texture{
        Texture::PixelFormat::RGBA,
        Texture::ComponentFormat::Float32,
        {size.x(), size.y()},
        Texture::InterpolationMode::Nearest,
        Texture::InterpolationMode::Nearest,
        Texture::WrapMode::ClampToEdge,
        1,
        Texture::TextureFlags::ShaderRead,
        true,
    };
    cache.texture->upload(dataPtr->dataBytes());
    return cache.texture.get();
}

shared_ptr<PixelBuffer> ImageCanvas::createInspectionPreviewPixels(
    const shared_ptr<Image>& image,
    string_view requestedGroup,
    int displayChannelSelection
) {
    if (!image) {
        return nullptr;
    }

    const auto matrix = inspectionMatrix();
    const auto transfer = mInspectionTransfer;
    const bool premultipliedAlpha = mInspectionPremultipliedAlpha;
    const float scale = mInspectionScale;
    const auto valueMode = mInspectionValueMode;

    const auto flattened =
        image->getHdrImageData(nullptr, requestedGroup, EMetric::Error, numeric_limits<int>::max(), image->hasSourceChannels()).get();
    if (flattened.empty()) {
        return nullptr;
    }

    const auto views = flattened | views::transform([](const Channel& c) { return c.view<const float>(); }) | toVector;

    const ChannelView<const float>* alphaChannel = nullptr;
    if (!all_of(begin(flattened), end(flattened), [](const Channel& c) { return c.isAlpha(); })) {
        if (flattened.back().isAlpha()) {
            alphaChannel = &views.back();
        }
    }

    const size_t nInputColorChannels = alphaChannel ? (flattened.size() - 1) : flattened.size();
    if (nInputColorChannels == 0) {
        return nullptr;
    }

    std::array<size_t, 3> colorIndices = {0, 1, 2};
    if (nInputColorChannels >= 3) {
        std::array<std::optional<size_t>, 3> resolved;
        for (size_t i = 0; i < nInputColorChannels; ++i) {
            const auto tail = Channel::tail(flattened[i].name());
            if (tail.empty()) {
                continue;
            }

            switch (std::toupper((unsigned char)tail.front())) {
                case 'R':
                case 'X':
                    if (!resolved[0]) resolved[0] = i;
                    break;
                case 'G':
                case 'Y':
                    if (!resolved[1]) resolved[1] = i;
                    break;
                case 'B':
                case 'Z':
                    if (!resolved[2]) resolved[2] = i;
                    break;
                default:
                    break;
            }
        }

        for (size_t c = 0; c < 3; ++c) {
            colorIndices[c] = resolved[c].value_or(c);
        }
    }

    const auto size = image->size();
    auto dataPtr = make_shared<PixelBuffer>(PixelBuffer::alloc(image->numPixels() * 4, EPixelFormat::F32));
    auto* out = dataPtr->data<float>();

    for (int y = 0; y < size.y(); ++y) {
        for (int x = 0; x < size.x(); ++x) {
            const float alpha = alphaChannel && !premultipliedAlpha ? (*alphaChannel)[x, y] : 1.0f;
            const float alphaFactor = alpha == 0 ? 0.0f : 1.0f / alpha;

            Vector3f rgb{0.0f};
            if (nInputColorChannels >= 3) {
                for (size_t c = 0; c < 3; ++c) {
                    rgb[c] = views[colorIndices[c]][x, y] * alphaFactor * scale;
                }
                rgb = clampToHdrDomain(ituth273::transfer(transfer, matrix * rgb));

                if (valueMode == EInspectionValueMode::YChannel) {
                    rgb = Vector3f{rgb[1], rgb[1], rgb[1]};
                } else if (valueMode == EInspectionValueMode::Luminance) {
                    const float luminance = dot(rgb, Vector3f{0.2126f, 0.7152f, 0.0722f});
                    rgb = Vector3f{luminance, luminance, luminance};
                } else if (displayChannelSelection >= 1 && displayChannelSelection <= 3) {
                    const float channelValue = rgb[(size_t)(displayChannelSelection - 1)];
                    rgb = Vector3f{channelValue, channelValue, channelValue};
                }
            } else {
                const float value = ituth273::transferComponent(transfer, views[0][x, y] * alphaFactor * scale);
                rgb = Vector3f{value, value, value};
            }

            const size_t idx = ((size_t)y * (size_t)size.x() + (size_t)x) * 4;
            out[idx + 0] = rgb[0];
            out[idx + 1] = rgb[1];
            out[idx + 2] = rgb[2];
            out[idx + 3] = 1.0f;
        }
    }

    return dataPtr;
}

bool ImageCanvas::sampleInspectionPreviewAtImageCoords(const shared_ptr<Image>& image, Vector2i imageCoords, Vector4f& result) {
    auto* texture = inspectionPreviewTexture(image);
    if (!texture || !image || !image->contains(imageCoords)) {
        return false;
    }

    const auto& cache = (image == mImage) ? mInspectionPreviewCache : mInspectionReferencePreviewCache;
    if (!cache.pixels) {
        return false;
    }

    const auto size = image->size();
    const size_t idx = ((size_t)imageCoords.y() * (size_t)size.x() + (size_t)imageCoords.x()) * 4;
    const auto* values = cache.pixels->data<float>();
    result = Vector4f{values[idx + 0], values[idx + 1], values[idx + 2], values[idx + 3]};
    return true;
}

Texture* ImageCanvas::highlightMaskTexture() {
    if (!mImage || !mHighlightValueRange.has_value()) {
        return nullptr;
    }

    if (mReference) {
        return nullptr;
    }

    const auto highlightGroup = mHighlightChannelGroup.empty() ? mRequestedChannelGroup : mHighlightChannelGroup;
    const auto range = *mHighlightValueRange;
    const auto matrix = inspectionMatrix();
    const auto transfer = mInspectionTransfer;
    const auto requestedGroup = mRequestedChannelGroup;
    const bool premultipliedAlpha = mInspectionPremultipliedAlpha;
    const float scale = mInspectionScale;
    const auto valueMode = mInspectionValueMode;

    if (mHighlightMaskCache.image == mImage && mHighlightMaskCache.reference == mReference &&
        mHighlightMaskCache.requestedGroup == requestedGroup && mHighlightMaskCache.metric == mMetric &&
        mHighlightMaskCache.highlightGroup == highlightGroup &&
        mHighlightMaskCache.range == mHighlightValueRange && mHighlightMaskCache.mode == mHighlightMode &&
        mHighlightMaskCache.matrix == matrix && mHighlightMaskCache.transfer == transfer &&
        mHighlightMaskCache.premultipliedAlpha == premultipliedAlpha && mHighlightMaskCache.scale == scale &&
        mHighlightMaskCache.valueMode == valueMode && mHighlightMaskCache.displayChannelSelection == 0 && mHighlightMaskCache.texture) {
        return mHighlightMaskCache.texture.get();
    }

    shared_ptr<PixelBuffer> previewPixels;
    if (highlightGroup == requestedGroup && mInspectionPreviewCache.image == mImage && mInspectionPreviewCache.pixels &&
        mInspectionPreviewCache.requestedGroup == requestedGroup && mInspectionPreviewCache.matrix == matrix &&
        mInspectionPreviewCache.transfer == transfer && mInspectionPreviewCache.premultipliedAlpha == premultipliedAlpha &&
        mInspectionPreviewCache.scale == scale && mInspectionPreviewCache.valueMode == valueMode &&
        mInspectionPreviewCache.displayChannelSelection == 0) {
        previewPixels = mInspectionPreviewCache.pixels;
    } else {
        previewPixels = createInspectionPreviewPixels(mImage, highlightGroup, 0);
    }

    if (!previewPixels) {
        return nullptr;
    }

    const auto size = mImage->size();
    auto data = PixelBuffer::alloc(mImage->numPixels() * 4, EPixelFormat::F32);
    auto* out = data.data<float>();
    const auto* values = previewPixels->data<float>();

    const auto inRange = [&](float v) { return v >= range.x() && v <= range.y(); };

    for (int y = 0; y < size.y(); ++y) {
        for (int x = 0; x < size.x(); ++x) {
            const size_t idx = ((size_t)y * (size_t)size.x() + (size_t)x) * 4;
            const float r = values[idx + 0];
            const float g = values[idx + 1];
            const float b = values[idx + 2];

            Vector3f mask{0.0f};
            switch (mHighlightMode) {
                case EHighlightMode::Red:
                    if (inRange(r)) {
                        mask = Vector3f{1.0f, 0.12f, 0.12f};
                    }
                    break;
                case EHighlightMode::Green:
                    if (inRange(g)) {
                        mask = Vector3f{0.15f, 0.95f, 0.18f};
                    }
                    break;
                case EHighlightMode::Blue:
                    if (inRange(b)) {
                        mask = Vector3f{0.22f, 0.50f, 1.0f};
                    }
                    break;
                case EHighlightMode::Single:
                    if (inRange(r)) {
                        mask = Vector3f{1.0f, 0.92f, 0.18f};
                    }
                    break;
                case EHighlightMode::Rgb:
                default:
                    if (inRange(r) && inRange(g) && inRange(b)) {
                        mask = Vector3f{1.0f, 0.80f, 0.16f};
                    }
                    break;
            }

            out[idx + 0] = mask.x();
            out[idx + 1] = mask.y();
            out[idx + 2] = mask.z();
            out[idx + 3] = (mask.x() > 0.0f || mask.y() > 0.0f || mask.z() > 0.0f) ? 1.0f : 0.0f;
        }
    }

    mHighlightMaskCache = {};
    mHighlightMaskCache.image = mImage;
    mHighlightMaskCache.reference = mReference;
    mHighlightMaskCache.requestedGroup = requestedGroup;
    mHighlightMaskCache.highlightGroup = highlightGroup;
    mHighlightMaskCache.metric = mMetric;
    mHighlightMaskCache.range = mHighlightValueRange;
    mHighlightMaskCache.mode = mHighlightMode;
    mHighlightMaskCache.matrix = matrix;
    mHighlightMaskCache.transfer = transfer;
    mHighlightMaskCache.premultipliedAlpha = premultipliedAlpha;
    mHighlightMaskCache.scale = scale;
    mHighlightMaskCache.valueMode = valueMode;
    mHighlightMaskCache.displayChannelSelection = 0;
    mHighlightMaskCache.texture = new Texture{
        Texture::PixelFormat::RGBA,
        Texture::ComponentFormat::Float32,
        {size.x(), size.y()},
        Texture::InterpolationMode::Nearest,
        Texture::InterpolationMode::Nearest,
        Texture::WrapMode::ClampToEdge,
        1,
        Texture::TextureFlags::ShaderRead,
        true,
    };
    mHighlightMaskCache.texture->upload(data.dataBytes());
    return mHighlightMaskCache.texture.get();
}

Texture* ImageCanvas::analysisOverlayTexture(const shared_ptr<Image>& image) {
    if (!image || mAnalysisOverlayCache.image != image || !mAnalysisOverlayCache.pixels) {
        return nullptr;
    }

    if (mAnalysisOverlayCache.texture) {
        return mAnalysisOverlayCache.texture.get();
    }

    const auto size = image->size();
    mAnalysisOverlayCache.texture = new Texture{
        Texture::PixelFormat::RGBA,
        Texture::ComponentFormat::Float32,
        {size.x(), size.y()},
        Texture::InterpolationMode::Nearest,
        Texture::InterpolationMode::Nearest,
        Texture::WrapMode::ClampToEdge,
        1,
        Texture::TextureFlags::ShaderRead,
        true,
    };
    mAnalysisOverlayCache.texture->upload(mAnalysisOverlayCache.pixels->dataBytes());
    return mAnalysisOverlayCache.texture.get();
}

void ImageCanvas::setImage(shared_ptr<Image> image) {
    if (mImage == image) {
        return;
    }

    mImage = std::move(image);
    mInspectionPreviewImage.reset();
    invalidateCanvasStatistics();
}

void ImageCanvas::setReference(shared_ptr<Image> reference) {
    if (mReference == reference) {
        return;
    }

    mReference = std::move(reference);
    if (mReference) {
        mInspectionPreviewImage.reset();
    }
    invalidateCanvasStatistics();
}

void ImageCanvas::setRequestedChannelGroup(string_view groupName) {
    if (mRequestedChannelGroup == groupName) {
        return;
    }

    mRequestedChannelGroup = string{groupName};
    invalidateCanvasStatistics();
}

void ImageCanvas::setInspectionPreviewImage(const shared_ptr<Image>& image) {
    if (mInspectionPreviewImage == image) {
        return;
    }

    mInspectionPreviewImage = image;
    invalidateCanvasStatistics();
}

void ImageCanvas::clearInspectionPreviewImage() {
    setInspectionPreviewImage(nullptr);
}

void ImageCanvas::setHighlightChannelGroup(string_view groupName) {
    if (mHighlightChannelGroup == groupName) {
        return;
    }

    mHighlightChannelGroup = string{groupName};
    invalidateHighlightMaskCache();
    redrawWindow();
}

void ImageCanvas::setAnalysisOverlay(shared_ptr<Image> image, shared_ptr<PixelBuffer> pixels) {
    if (mAnalysisOverlayCache.image == image && mAnalysisOverlayCache.pixels == pixels) {
        return;
    }

    mAnalysisOverlayCache = {};
    mAnalysisOverlayCache.image = std::move(image);
    mAnalysisOverlayCache.pixels = std::move(pixels);
    redrawWindow();
}

void ImageCanvas::clearAnalysisOverlay() {
    if (!mAnalysisOverlayCache.image && !mAnalysisOverlayCache.pixels && !mAnalysisOverlayCache.texture) {
        return;
    }
    mAnalysisOverlayCache = {};
    redrawWindow();
}

void ImageCanvas::setAiMaskPromptPoints(vector<Vector2i> positivePoints, vector<Vector2i> negativePoints) {
    vector<AiMaskPromptPoint> combined;
    combined.reserve(positivePoints.size() + negativePoints.size());

    for (const auto& point : positivePoints) {
        combined.push_back({point, true});
    }

    for (const auto& point : negativePoints) {
        combined.push_back({point, false});
    }

    if (mAiMaskPromptPoints == combined) {
        return;
    }

    mAiMaskPromptPoints = std::move(combined);
    redrawWindow();
}

void ImageCanvas::clearAiMaskPromptPoints() {
    if (mAiMaskPromptPoints.empty()) {
        return;
    }

    mAiMaskPromptPoints.clear();
    redrawWindow();
}

void ImageCanvas::drawAiMaskPromptPoints(NVGcontext* ctx) {
    if (!mImage || mAiMaskPromptPoints.empty()) {
        return;
    }

    const auto imageToNano = textureToNanogui(mImage.get());
    const auto toCanvasPoint = [&](const Vector2i& imageCoords) {
        return Vector2f{m_pos} + imageToNano * Vector2f{
            (float)imageCoords.x() + 0.5f,
            (float)imageCoords.y() + 0.5f,
        };
    };

    nvgSave(ctx);
    nvgStrokeWidth(ctx, 2.0f);

    for (const auto& point : mAiMaskPromptPoints) {
        if (!mImage->contains(point.imageCoords)) {
            continue;
        }

        const Vector2f center = toCanvasPoint(point.imageCoords);
        const Color fill = point.positive ? Color(72, 220, 255, 220) : Color(255, 96, 192, 220);
        const Color inner = point.positive ? Color(255, 255, 255, 255) : Color(255, 255, 255, 245);
        constexpr float kOuterRadius = 7.0f;
        constexpr float kInnerRadius = 4.0f;
        constexpr float kMarkRadius = 3.5f;

        nvgBeginPath(ctx);
        nvgCircle(ctx, center.x(), center.y(), kOuterRadius);
        nvgFillColor(ctx, fill);
        nvgFill(ctx);

        nvgBeginPath(ctx);
        nvgCircle(ctx, center.x(), center.y(), kInnerRadius);
        nvgStrokeColor(ctx, inner);
        nvgStroke(ctx);

        nvgBeginPath(ctx);
        if (point.positive) {
            nvgMoveTo(ctx, center.x() - kMarkRadius, center.y());
            nvgLineTo(ctx, center.x() + kMarkRadius, center.y());
            nvgMoveTo(ctx, center.x(), center.y() - kMarkRadius);
            nvgLineTo(ctx, center.x(), center.y() + kMarkRadius);
        } else {
            nvgMoveTo(ctx, center.x() - kMarkRadius, center.y());
            nvgLineTo(ctx, center.x() + kMarkRadius, center.y());
        }
        nvgStrokeColor(ctx, inner);
        nvgStroke(ctx);
    }

    nvgRestore(ctx);
}

void ImageCanvas::setInspectionDisplayChannelSelection(int selection) {
    selection = clamp(selection, 0, 3);
    if (mInspectionDisplayChannelSelection == selection) {
        return;
    }

    mInspectionDisplayChannelSelection = selection;
    invalidateInspectionPreviewCache();
    redrawWindow();
}

void ImageCanvas::setCrop(const optional<Box2i>& crop) {
    if (mCrop == crop) {
        return;
    }

    mCrop = crop;
    invalidateStatisticsOnly();
    redrawWindow();
}

void ImageCanvas::setRoi(const optional<Box2i>& roi) {
    if (mRoi == roi) {
        return;
    }

    mRoi = roi;
    invalidateStatisticsOnly();
    redrawWindow();
}

void ImageCanvas::setInspectionChroma(const chroma_t& chroma) {
    if (mInspectionChroma == chroma) {
        return;
    }

    mInspectionChroma = chroma;
    invalidateCanvasStatistics();
}

void ImageCanvas::setInspectionTransfer(const ituth273::ETransfer transfer) {
    if (mInspectionTransfer == transfer) {
        return;
    }

    mInspectionTransfer = transfer;
    invalidateCanvasStatistics();
}

void ImageCanvas::setInspectionAdaptWhitePoint(bool adapt) {
    if (mInspectionAdaptWhitePoint == adapt) {
        return;
    }

    mInspectionAdaptWhitePoint = adapt;
    invalidateCanvasStatistics();
}

void ImageCanvas::setInspectionPremultipliedAlpha(bool premultipied) {
    if (mInspectionPremultipliedAlpha == premultipied) {
        return;
    }

    mInspectionPremultipliedAlpha = premultipied;
    invalidateCanvasStatistics();
}

void ImageCanvas::setInspectionScale(float scale) {
    if (mInspectionScale == scale) {
        return;
    }

    mInspectionScale = scale;
    invalidateCanvasStatistics();
}

void ImageCanvas::setInspectionValueMode(EInspectionValueMode mode) {
    if (mInspectionValueMode == mode) {
        return;
    }

    mInspectionValueMode = mode;
    invalidateCanvasStatistics();
}

void ImageCanvas::setInspectionMatrixOverride(const optional<Matrix3f>& matrix) {
    if (mInspectionMatrixOverride == matrix) {
        return;
    }

    mInspectionMatrixOverride = matrix;
    invalidateCanvasStatistics();
}

void ImageCanvas::setDisplayInspectionInPreview(bool value) {
    if (mDisplayInspectionInPreview == value) {
        return;
    }

    mDisplayInspectionInPreview = value;
    invalidateInspectionPreviewCache();
    redrawWindow();
}

void ImageCanvas::setFalseColorMap(EFalseColorMap map) {
    if (mFalseColorMap == map) {
        return;
    }

    mFalseColorMap = map;
    redrawWindow();
}

void ImageCanvas::setFalseColorScaleMode(EFalseColorScaleMode mode) {
    if (mFalseColorScaleMode == mode) {
        return;
    }

    mFalseColorScaleMode = mode;
    redrawWindow();
}

void ImageCanvas::setFalseColorRange(Vector2f range) {
    if (range.x() > range.y()) {
        std::swap(range.x(), range.y());
    }

    range.x() = std::max(range.x(), 0.0f);
    range.y() = std::max(range.y(), range.x() + 1e-6f);

    if (mFalseColorRange == range) {
        return;
    }

    mFalseColorRange = range;
    redrawWindow();
}

void ImageCanvas::setHighlightValueRange(const optional<Vector2f>& range) {
    if (mHighlightValueRange == range) {
        return;
    }

    mHighlightValueRange = range;
    invalidateHighlightMaskCache();
    redrawWindow();
}

void ImageCanvas::setHighlightMode(EHighlightMode mode) {
    if (mHighlightMode == mode) {
        return;
    }

    mHighlightMode = mode;
    invalidateHighlightMaskCache();
    redrawWindow();
}

Matrix3f ImageCanvas::inspectionMatrix() const {
    return currentInspectionMatrix(mInspectionMatrixOverride, mInspectionChroma, mInspectionAdaptWhitePoint);
}

Task<HeapArray<float>> ImageCanvas::getRgbaHdrImageData(bool divideAlpha, int priority) const {
    if (!mImage) {
        co_return {};
    }

    co_return co_await mImage->getRgbaHdrImageData(
        mReference, cropInImageCoords(), mRequestedChannelGroup, mMetric, mChannelMask, mBackgroundColor, divideAlpha, priority
    );
}

Task<HeapArray<uint8_t>> ImageCanvas::getRgbaLdrImageData(bool divideAlpha, int priority) const {
    if (!mImage) {
        co_return {};
    }

    co_return co_await mImage->getRgbaLdrImageData(
        mReference,
        cropInImageCoords(),
        mRequestedChannelGroup,
        mMetric,
        mChannelMask,
        mBackgroundColor,
        divideAlpha,
        mTonemap,
        mGamma,
        mExposure,
        mOffset,
        priority
    );
}

Task<HeapArray<uint8_t>> ImageCanvas::getFullImageRgbaLdrImageData(bool divideAlpha, int priority) const {
    if (!mImage) {
        co_return {};
    }

    const Box2i fullRegion{{0, 0}, mImage->size()};
    co_return co_await mImage->getRgbaLdrImageData(
        mReference,
        fullRegion,
        mRequestedChannelGroup,
        mMetric,
        mChannelMask,
        mBackgroundColor,
        divideAlpha,
        mTonemap,
        mGamma,
        mExposure,
        mOffset,
        priority
    );
}

Task<HeapArray<uint8_t>> ImageCanvas::getFullImageRgbaLdrImageDataWithOverrides(
    bool divideAlpha,
    ETonemap tonemap,
    float gamma,
    float exposure,
    float offset,
    int priority
) const {
    if (!mImage) {
        co_return {};
    }

    const Box2i fullRegion{{0, 0}, mImage->size()};
    co_return co_await mImage->getRgbaLdrImageData(
        mReference,
        fullRegion,
        mRequestedChannelGroup,
        mMetric,
        mChannelMask,
        mBackgroundColor,
        divideAlpha,
        tonemap,
        gamma,
        exposure,
        offset,
        priority
    );
}

void ImageCanvas::saveImage(const fs::path& path) const {
    if (!mImage) {
        throw ImageSaveError{"There is no image to save."};
    }

    tlog::info("Saving currently displayed image as {}.", toString(path));

    const auto start = chrono::steady_clock::now();

    mImage
        ->save(
            path,
            mReference,
            cropInImageCoords(),
            mRequestedChannelGroup,
            mMetric,
            mChannelMask,
            mBackgroundColor,
            mTonemap,
            mGamma,
            mExposure,
            mOffset,
            numeric_limits<int>::max() // Use maximum priority for saving images.
        )
        .get();

    const auto elapsedSeconds = chrono::duration<double>{chrono::steady_clock::now() - start};
    tlog::success("Saved {} after {:.3f} seconds.", path, elapsedSeconds.count());
}

shared_ptr<Lazy<shared_ptr<CanvasStatistics>>> ImageCanvas::canvasStatistics() {
    if (!mImage) {
        return nullptr;
    }

    const auto buildKey = [this]() {
        const auto statisticsImage = inspectionPreviewSourceImage(mImage, mImage, mInspectionPreviewImage);
        const string channels = join(statisticsImage->channelsInGroup(mRequestedChannelGroup), ",");

        ostringstream keyStream;
        keyStream << format(
            "{}-{}-{}-{}-{}-{}-{}-{}",
            (int)mInspectionTransfer,
            mInspectionChroma,
            mInspectionAdaptWhitePoint,
            mInspectionPremultipliedAlpha,
            mInspectionScale,
            (int)mInspectionValueMode,
            statisticsImage->id(),
            channels
        );

        if (mReference) {
            keyStream << format("-{}-{}", mReference->id(), (int)mMetric);
        }

        if (mCrop.has_value()) {
            keyStream << format("-crop-{}-{}", mCrop->min, mCrop->max);
        }

        if (mRoi.has_value()) {
            keyStream << format("-roi-{}-{}", mRoi->min, mRoi->max);
        }

        return keyStream;
    };

    // Indirection through lambda to ensure str()&& overload moves out of stringstream
    const string key = buildKey().str();
    const auto iter = mCanvasStatistics.find(key);
    if (iter != end(mCanvasStatistics)) {
        return iter->second;
    }

    promise<shared_ptr<CanvasStatistics>> promise;
    mCanvasStatistics.insert(make_pair(key, make_shared<Lazy<shared_ptr<CanvasStatistics>>>(promise.get_future())));

    // Remember the keys associateed with the participating images. Such that their canvas statistics can be retrieved and deleted when
    // either of the images is closed or mutated.
    mImageIdToCanvasStatisticsKey[mImage->id()].emplace_back(key);
    mImage->setStaleIdCallback([this](int id) { purgeCanvasStatistics(id); });

    if (mReference) {
        mImageIdToCanvasStatisticsKey[mReference->id()].emplace_back(key);
        mReference->setStaleIdCallback([this](int id) { purgeCanvasStatistics(id); });
    }

    // Later requests must have higher priority than previous ones.
    static atomic<int> sId{0};
    invokeTaskDetached(
        [image = inspectionPreviewSourceImage(mImage, mImage, mInspectionPreviewImage),
         reference = mReference,
         requestedChannelGroup = mRequestedChannelGroup,
         metric = mMetric,
         region = statisticsRegionInImageCoords(),
         inspectionMatrixValue = inspectionMatrix(),
         transfer = mInspectionTransfer,
         premultipliedAlpha = mInspectionPremultipliedAlpha,
         inspectionScale = mInspectionScale,
         inspectionValueMode = mInspectionValueMode,
         priority = ++sId,
         p = std::move(promise)]() mutable -> Task<void> {
            co_await ThreadPool::global().enqueueCoroutine(priority);
            p.set_value(
                co_await computeCanvasStatistics(
                    image,
                    reference,
                    requestedChannelGroup,
                    metric,
                    region,
                    inspectionMatrixValue,
                    transfer,
                    premultipliedAlpha,
                    inspectionScale,
                    inspectionValueMode,
                    priority
                )
            );
            redrawWindow();
        }
    );

    return mCanvasStatistics.at(key);
}

void ImageCanvas::purgeCanvasStatistics(size_t imageId) {
    const auto it = mImageIdToCanvasStatisticsKey.find(imageId);
    if (it == mImageIdToCanvasStatisticsKey.end()) {
        return;
    }

    for (const auto& key : it->second) {
        mCanvasStatistics.erase(key);
    }

    mImageIdToCanvasStatisticsKey.erase(it);
}

void ImageCanvas::applyInspectionParameters(vector<float>& values, bool hasAlpha) {
    if (values.empty()) {
        return;
    }

    // If we have 3 color channels, apply alpha, the inspection chroma, and transfer function
    const size_t nColorChannels = values.size() - (hasAlpha ? 1 : 0);

    const float alpha = hasAlpha && !mInspectionPremultipliedAlpha ? values.back() : 1.0f;
    const float alphaFactor = alpha == 0 ? 0.0f : 1.0f / alpha;

    if (nColorChannels >= 3) {
        const auto mat = inspectionMatrix();

        Vector3f rgb;
        for (size_t c = 0; c < 3; ++c) {
            rgb[c] = values[c] * alphaFactor * mInspectionScale;
        }

        rgb = clampToHdrDomain(ituth273::transfer(mInspectionTransfer, mat * rgb));
        for (size_t c = 0; c < 3; ++c) {
            values[c] = rgb[c];
        }

        if (mInspectionValueMode == EInspectionValueMode::YChannel) {
            values.assign({rgb[1]});
        } else if (mInspectionValueMode == EInspectionValueMode::Luminance) {
            const float luminance = dot(rgb, Vector3f{0.2126f, 0.7152f, 0.0722f});
            values.assign({luminance});
        }
    } else {
        // Otherwise, apply just alpha and transfer function
        for (size_t c = 0; c < nColorChannels; ++c) {
            values[c] = ituth273::transferComponent(mInspectionTransfer, values[c] * alphaFactor * mInspectionScale);
        }
    }
}

Task<shared_ptr<CanvasStatistics>> ImageCanvas::computeCanvasStatistics(
    shared_ptr<Image> image,
    shared_ptr<Image> reference,
    string_view requestedChannelGroup,
    EMetric metric,
    Box2i region,
    const Matrix3f& inspectionMatrix,
    ituth273::ETransfer transfer,
    bool premultipliedAlpha,
    float inspectionScale,
    EInspectionValueMode inspectionValueMode,
    int priority
) {
    TEV_ASSERT(image, "Image must be valid.");

    // If the crop region is outside the image, intersect. If no intersection, use the empty region about the origin.
    region = region.intersect(Box2i{image->size()});
    if (!region.isValid()) {
        region = Box2i{0};
    }

    const auto start = chrono::steady_clock::now();
    const auto scopeGuard = ScopeGuard([&]() {
        const auto end = chrono::steady_clock::now();
        const chrono::duration<double> elapsedSeconds = end - start;
        tlog::debug("Computed canvas statistics for {} in {:.4f} seconds.", image->name(), elapsedSeconds.count());
    });

    auto flattened = co_await image->getHdrImageData(reference, requestedChannelGroup, metric, priority, image->hasSourceChannels());
    const auto views = flattened | views::transform([](Channel& c) { return c.view<float>(); }) | toVector;

    const ChannelView<float>* alphaChannel = nullptr;

    // Only treat the alpha channel specially if it is not the only channel of the image.
    if (!all_of(begin(flattened), end(flattened), [](const Channel& c) { return c.isAlpha(); })) {
        if (flattened.back().isAlpha()) {
            alphaChannel = &views.back();
        }
    }

    const auto result = make_shared<CanvasStatistics>();
    const auto nInputColorChannels = alphaChannel ? (flattened.size() - 1) : flattened.size();
    const bool useLuminanceMode = inspectionValueMode == EInspectionValueMode::Luminance && nInputColorChannels >= 3;
    const bool useYChannelMode = inspectionValueMode == EInspectionValueMode::YChannel && nInputColorChannels >= 3;
    const size_t nColorChannels = (useLuminanceMode || useYChannelMode) ? 1 : nInputColorChannels;
    result->nChannels = (int)nColorChannels;

    result->histogramColors.resize(nColorChannels);
    for (size_t i = 0; i < nColorChannels; ++i) {
        string rgba[] = {"R", "G", "B", "A"};
        string colorName = useLuminanceMode ? "L" : (useYChannelMode ? "Y" : (nColorChannels == 1 ? "L" : rgba[std::min(i, 3uz)]));
        result->histogramColors[i] = Channel::color(colorName, false);
    }

    const auto regionSize = region.size();
    const auto numPixels = region.area();
    const auto numSamples = nColorChannels * numPixels;
    result->region = region;

    // If we have 3 color channels, apply alpha, the inspection chroma, and transfer function
    if (nInputColorChannels >= 3) {
        const auto mat = inspectionMatrix;

        co_await ThreadPool::global().parallelFor(
            region.min.y(),
            region.max.y(),
            numSamples,
            [&](int y) {
                for (int x = region.min.x(); x < region.max.x(); ++x) {
                    const float alpha = alphaChannel && !premultipliedAlpha ? (*alphaChannel)[x, y] : 1.0f;
                    const float alphaFactor = alpha == 0 ? 0.0f : 1.0f / alpha;

                    Vector3f rgb;
                    for (size_t c = 0; c < 3; ++c) {
                        rgb[c] = views[c][x, y] * alphaFactor * inspectionScale;
                    }

                    rgb = clampToHdrDomain(ituth273::transfer(transfer, mat * rgb));
                    for (size_t c = 0; c < 3; ++c) {
                        views[c][x, y] = rgb[c];
                    }
                }
            },
            priority
        );
    } else {
        // Otherwise, apply just alpha and transfer function
        if (transfer != ituth273::ETransfer::Linear || (alphaChannel && !premultipliedAlpha)) {
            co_await ThreadPool::global().parallelFor(
                region.min.y(),
                region.max.y(),
                numSamples,
                [&](int y) {
                    for (int x = region.min.x(); x < region.max.x(); ++x) {
                        const float alpha = alphaChannel && !premultipliedAlpha ? (*alphaChannel)[x, y] : 1.0f;
                        const float alphaFactor = alpha == 0 ? 0.0f : 1.0f / alpha;

                        for (size_t c = 0; c < nInputColorChannels; ++c) {
                            const float val = views[c][x, y] * alphaFactor * inspectionScale;
                            views[c][x, y] = ituth273::transferComponent(transfer, val);
                        }
                    }
                },
                priority
            );
        }
    }

    struct Stats {
        double sum = 0;
        double sumSq = 0;
        float maximum = -numeric_limits<float>::infinity();
        float minimum = numeric_limits<float>::infinity();
        size_t count = 0;
    };

    auto accumulateStat = [](Stats& stats, float value) {
        stats.sum += value;
        stats.sumSq += (double)value * value;
        stats.maximum = std::max(stats.maximum, value);
        stats.minimum = std::min(stats.minimum, value);
        ++stats.count;
    };

    auto mergeStats = [](Stats& dst, const Stats& src) {
        dst.sum += src.sum;
        dst.sumSq += src.sumSq;
        dst.maximum = std::max(dst.maximum, src.maximum);
        dst.minimum = std::min(dst.minimum, src.minimum);
        dst.count += src.count;
    };

    const size_t totalStatsPerLine = nColorChannels + 1;
    vector<Stats> channelStats(nColorChannels);
    Stats overallStats;

    // Parallel stats computation: every task computes its own stats, which are combined at the end.
    {
        vector<Stats> perLineStats((size_t)regionSize.y() * totalStatsPerLine);

        co_await ThreadPool::global().parallelFor(
            region.min.y(),
            region.max.y(),
            numSamples,
            [&](int y) {
                Stats* const lineStats = perLineStats.data() + (size_t)(y - region.min.y()) * totalStatsPerLine;
                Stats& lineOverall = lineStats[nColorChannels];

                for (int x = region.min.x(); x < region.max.x(); ++x) {
                    if (useLuminanceMode || useYChannelMode) {
                        const Vector3f triplet{views[0][x, y], views[1][x, y], views[2][x, y]};
                        const float v = useYChannelMode ? triplet[1] : dot(triplet, Vector3f{0.2126f, 0.7152f, 0.0722f});
                        if (isfinite(v)) {
                            accumulateStat(lineStats[0], v);
                            accumulateStat(lineOverall, v);
                        }
                    } else {
                        for (size_t c = 0; c < nColorChannels; ++c) {
                            auto v = views[c][x, y];
                            if (!isfinite(v)) {
                                continue;
                            }

                            accumulateStat(lineStats[c], v);
                            accumulateStat(lineOverall, v);
                        }
                    }
                }
            },
            priority
        );

        for (int y = 0; y < regionSize.y(); ++y) {
            const Stats* const lineStats = perLineStats.data() + (size_t)y * totalStatsPerLine;
            for (size_t c = 0; c < nColorChannels; ++c) {
                mergeStats(channelStats[c], lineStats[c]);
            }

            mergeStats(overallStats, lineStats[nColorChannels]);
        }
    }

    result->channelMean.resize(nColorChannels, 0.0f);
    result->channelMaximum.resize(nColorChannels, 0.0f);
    result->channelMinimum.resize(nColorChannels, 0.0f);
    result->channelStdDev.resize(nColorChannels, 0.0f);

    const auto finalizeMean = [](const Stats& stats) { return stats.count > 0 ? (float)(stats.sum / (double)stats.count) : 0.0f; };
    const auto finalizeStdDev = [&](const Stats& stats, float mean) {
        if (stats.count == 0) {
            return 0.0f;
        }

        return (float)std::sqrt(std::max(0.0, stats.sumSq / (double)stats.count - (double)mean * mean));
    };

    result->mean = finalizeMean(overallStats);
    result->maximum = overallStats.count > 0 ? overallStats.maximum : 0.0f;
    result->minimum = overallStats.count > 0 ? overallStats.minimum : 0.0f;
    result->stddev = finalizeStdDev(overallStats, result->mean);

    for (size_t c = 0; c < nColorChannels; ++c) {
        result->channelMean[c] = finalizeMean(channelStats[c]);
        result->channelMaximum[c] = channelStats[c].count > 0 ? channelStats[c].maximum : 0.0f;
        result->channelMinimum[c] = channelStats[c].count > 0 ? channelStats[c].minimum : 0.0f;
        result->channelStdDev[c] = finalizeStdDev(channelStats[c], result->channelMean[c]);
    }

    // The more pixels we have, the finer we can make the histogram without becoming noisy
    // const size_t numBins = clamp(numPixels / 512, 16uz, 512uz);
    const size_t numBins = 400;
    result->histogram.resize(numBins * nColorChannels);

    // We're going to draw our histogram in log space.
    static constexpr float addition = 0.001f;
    static const float smallest = log(addition);

    static constexpr auto symmetricLog = [](const float val) {
        return val > 0 ? (log(val + addition) - smallest) : -(log(-val + addition) - smallest);
    };
    static constexpr auto symmetricLogInverse = [](const float val) {
        return val > 0 ? (exp(val + smallest) - addition) : -(exp(-val + smallest) - addition);
    };

    const float minLog = symmetricLog(result->minimum);
    const float diffLog = std::max(symmetricLog(result->maximum) - minLog, 1e-6f);

    const auto valToBin = [minLog, diffLog](const float val) {
        return clamp((int)(numBins * (symmetricLog(val) - minLog) / diffLog), 0, (int)numBins - 1);
    };

    result->histogramZero = valToBin(0);

    const auto binToVal = [minLog, diffLog](const float val) { return symmetricLogInverse((diffLog * val / numBins) + minLog); };

    // In the strange case that we have 0 channels, early return, because the histogram makes no sense.
    if (nColorChannels == 0) {
        co_return result;
    }

    // Parallel histogram computation: every task computes its own histogram, which are combined at the end.
    {
        const size_t approxCost = numSamples *
            8; // constant factor to represent the increased workload of log/exp and somewhat random memory writes
        const size_t numTasks = nextMultiple(ThreadPool::global().nTasks<size_t>(0, numPixels, approxCost), nColorChannels);
        const size_t numTasksPerChannel = numTasks / nColorChannels;

        vector<float> perTaskHistograms(numBins * nColorChannels * numTasks);

        co_await ThreadPool::global().parallelFor(
            0uz,
            numTasks,
            approxCost,
            [&](size_t i) {
                const size_t c = i % nColorChannels;
                const size_t taskPerChannelIndex = i / nColorChannels;

                const size_t taskStart = numPixels * taskPerChannelIndex / numTasksPerChannel;
                const size_t taskEnd = numPixels * (taskPerChannelIndex + 1) / numTasksPerChannel;

                float* const histogram = perTaskHistograms.data() + numBins * i;
                for (size_t j = taskStart; j < taskEnd; ++j) {
                    const int x = (int)(j % regionSize.x()) + region.min.x();
                    const int y = (int)(j / regionSize.x()) + region.min.y();
                    const float value = useLuminanceMode
                        ? dot(Vector3f{views[0][x, y], views[1][x, y], views[2][x, y]}, Vector3f{0.2126f, 0.7152f, 0.0722f})
                        : views[c][x, y];
                    histogram[valToBin(value)] += alphaChannel ? (*alphaChannel)[x, y] : 1;
                }
            },
            priority
        );

        co_await ThreadPool::global().parallelFor(
            0uz,
            numBins * nColorChannels,
            numBins * nColorChannels * numTasks,
            [&](size_t i) {
                const size_t stride = numBins * nColorChannels;
                for (size_t j = 0; j < numTasks; ++j) {
                    result->histogram[i] += perTaskHistograms[i + j * stride];
                }
            },
            priority
        );
    }

    for (size_t i = 0; i < nColorChannels; ++i) {
        for (size_t j = 0; j < numBins; ++j) {
            result->histogram[j + i * numBins] /= binToVal(j + 1) - binToVal(j);
        }
    }

    // Normalize the histogram according to the n-th largest element to avoid outliers dominating the histogram display.
    // - Ensure at least one element per color channel is excluded (typically a spike at zero)
    // - Additionally, scale by number of bins to make it a percentile-like normalization.
    auto tmp = result->histogram;
    const size_t idx = tmp.size() - 1 - (1 + numBins / 128) * nColorChannels;
    nth_element(tmp.data(), tmp.data() + idx, tmp.data() + tmp.size());

    const float norm = 1.0f / (std::max(tmp[idx], 0.1f) * 1.3f);
    for (size_t i = 0; i < nColorChannels; ++i) {
        for (size_t j = 0; j < numBins; ++j) {
            result->histogram[j + i * numBins] *= norm;
        }
    }

    co_return result;
}

Vector2f ImageCanvas::pixelOffset(Vector2i /*size*/) const {
    // Translate by half of a pixel to avoid pixel boundaries aligning perfectly with texels. The translation only needs to happen for axes
    // with even resolution. Odd-resolution axes are implicitly shifted by half a pixel due to the centering operation. Additionally, add
    // 0.1111111 such that our final position is almost never 0 modulo our pixel ratio, which again avoids aligned pixel boundaries with
    // texels.
    // return Vector2f{
    //     size.x() % 2 == 0 ?  0.5f : 0.0f,
    //     size.y() % 2 == 0 ? -0.5f : 0.0f,
    // } + Vector2f{0.1111111f};
    return Vector2f{-0.1111111f};
}

Matrix3f ImageCanvas::transform(const Image* image) {
    if (!image) {
        return Matrix3f::scale(Vector2f{1.0f});
    }

    TEV_ASSERT(mImage, "Coordinates are relative to the currently selected image's display window. So must have an image selected.");

    // Center image, scale to pixel space, translate to desired position, then rescale to the [-1, 1] square for drawing.
    return Matrix3f::scale(Vector2f{2.0f / m_size.x(), -2.0f / m_size.y()}) * mTransform * Matrix3f::scale(Vector2f{1.0f / mPixelRatio}) *
        Matrix3f::translate(image->centerDisplayOffset(mImage->displayWindow()) + pixelOffset(image->size())) *
        Matrix3f::scale(Vector2f{image->size()}) * Matrix3f::translate(Vector2f{-0.5f});
}

Matrix3f ImageCanvas::textureToNanogui(const Image* image) {
    if (!image) {
        return Matrix3f::scale(Vector2f{1.0f});
    }

    TEV_ASSERT(mImage, "Coordinates are relative to the currently selected image's display window. So must have an image selected.");

    // Move origin to centre of image, scale pixels, apply our transform, move origin back to top-left.
    return Matrix3f::translate(0.5f * Vector2f{m_size}) * mTransform * Matrix3f::scale(Vector2f{1.0f / mPixelRatio}) *
        Matrix3f::translate(-0.5f * Vector2f{image->size()} + image->centerDisplayOffset(mImage->displayWindow()) + pixelOffset(image->size()));
}

Matrix3f ImageCanvas::displayWindowToNanogui(const Image* image) {
    if (!image) {
        return Matrix3f::scale(Vector2f{1.0f});
    }

    // Shift texture coordinates by the data coordinate offset. It's that simple.
    return textureToNanogui(image) * Matrix3f::translate(-image->dataWindow().min);
}

} // namespace tev
