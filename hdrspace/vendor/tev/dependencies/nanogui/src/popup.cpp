/*
    src/popup.cpp -- Simple popup widget which is attached to another given
    window (can be nested)

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/popup.h>
#include <nanogui/theme.h>
#include <nanogui/opengl.h>

NAMESPACE_BEGIN(nanogui)

Popup::Popup(Widget *parent, Window *parent_window)
    : Window(parent, ""), m_parent_window(parent_window), m_anchor_pos(Vector2i(0)),
      m_anchor_offset(30), m_anchor_size(15), m_side(Side::Right) { }

void Popup::perform_layout(NVGcontext *ctx) {
    if (m_layout || m_children.size() != 1) {
        Widget::perform_layout(ctx);
    } else {
        m_children[0]->set_position(Vector2i(0));
        m_children[0]->set_size(m_size);
        m_children[0]->perform_layout(ctx);
    }
    if (m_side == Side::Left)
        m_anchor_pos[0] -= size()[0];
}

void Popup::refresh_relative_placement() {
    if (!m_parent_window)
        return;
    m_parent_window->refresh_relative_placement();
    m_visible &= m_parent_window->visible_recursive();
    m_pos = m_parent_window->position() + m_anchor_pos - Vector2i(0, m_anchor_offset);
}

void Popup::draw(NVGcontext* ctx) {
    refresh_relative_placement();

    if (!m_visible)
        return;

    // hdrspace: a flat card with a soft shadow and a hairline border; no anchor arrow.
    const int ds = 12;
    const float cr = 6.0f;
    nvgSave(ctx);
    nvgResetScissor(ctx);

    NVGpaint shadow_paint = nvgBoxGradient(
        ctx, m_pos.x(), m_pos.y() + 3, m_size.x(), m_size.y(), cr * 2, ds,
        Color(0, 0, 0, 120), Color(0, 0, 0, 0));
    nvgBeginPath(ctx);
    nvgRect(ctx, m_pos.x() - ds, m_pos.y() - ds, m_size.x() + 2 * ds, m_size.y() + 2 * ds);
    nvgRoundedRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y(), cr);
    nvgPathWinding(ctx, NVG_HOLE);
    nvgFillPaint(ctx, shadow_paint);
    nvgFill(ctx);

    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, m_pos.x() + 0.5f, m_pos.y() + 0.5f, m_size.x() - 1.0f, m_size.y() - 1.0f, cr);
    nvgFillColor(ctx, Color(42, 43, 47, 255));
    nvgFill(ctx);
    nvgStrokeColor(ctx, Color(255, 255, 255, 26));
    nvgStrokeWidth(ctx, 1.0f);
    nvgStroke(ctx);
    nvgRestore(ctx);

    Widget::draw(ctx);
}

NAMESPACE_END(nanogui)
