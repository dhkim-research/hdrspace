/*
    nanogui/menuitem.h -- hdrspace: a list row for drop-down menus (replaces the stacked
    bordered buttons NanoGUI uses inside combo-box and popup-button lists).
*/
#pragma once

#include <nanogui/button.h>
#include <nanogui/icons.h>
#include <nanogui/opengl.h>

NAMESPACE_BEGIN(nanogui)

class MenuItemButton : public Button {
public:
    MenuItemButton(Widget *parent, std::string_view caption) : Button(parent, caption) {
        set_fixed_height(26);
    }

    Vector2i preferred_size_impl(NVGcontext *ctx) const override {
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 13.0f);
        const float tw = nvgTextBounds(ctx, 0, 0, m_caption.c_str(), nullptr, nullptr);
        return Vector2i((int) tw + 44, 26);
    }

    void draw(NVGcontext *ctx) override {
        const float x = m_pos.x(), y = m_pos.y(), w = m_size.x(), h = m_size.y();
        const bool hot = m_mouse_focus && m_enabled;
        nvgSave(ctx);
        if (m_pushed || hot) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x, y, w, h, 4.0f);
            nvgFillColor(ctx, Color(255, 255, 255, m_pushed ? 20 : 12));
            nvgFill(ctx);
        }
        if (m_pushed) {
            const auto glyph = utf8(FA_CHECK);
            nvgFontFace(ctx, "icons");
            nvgFontSize(ctx, 11.0f);
            nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(ctx, Color(244, 191, 79, 255));
            nvgText(ctx, x + 14.0f, y + h * 0.5f, glyph.data(), nullptr);
        }
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 13.0f);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, m_enabled ? Color(236, 237, 238, 255) : Color(150, 154, 159, 120));
        nvgText(ctx, x + 28.0f, y + h * 0.5f, m_caption.c_str(), nullptr);
        nvgRestore(ctx);
    }
};

NAMESPACE_END(nanogui)
