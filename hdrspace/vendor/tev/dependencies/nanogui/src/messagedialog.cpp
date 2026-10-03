/*
    src/messagedialog.cpp -- Simple "OK" or "Yes/No"-style modal dialogs

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.

    hdrspace: restyled as a card (small type badge, title + grey body, text-only buttons on
    the right with one amber default, a small close button, Enter / Esc).
*/

#include <nanogui/messagedialog.h>
#include <nanogui/layout.h>
#include <nanogui/button.h>
#include <nanogui/label.h>
#include <nanogui/opengl.h>
#include <algorithm>

NAMESPACE_BEGIN(nanogui)

namespace {

constexpr int kDialogTextWidth = 320;

class DialogBadge : public Widget {
public:
    DialogBadge(Widget *parent, MessageDialog::Type type) : Widget(parent), m_type(type) {
        set_fixed_size(Vector2i(22, 22));
    }
    void draw(NVGcontext *ctx) override {
        Color bg(51, 65, 90, 255), fg(159, 192, 240, 255);
        const char *glyph = "i";
        if (m_type == MessageDialog::Type::Question) {
            bg = Color(74, 63, 34, 255); fg = Color(244, 191, 79, 255); glyph = "?";
        } else if (m_type == MessageDialog::Type::Warning) {
            bg = Color(79, 42, 39, 255); fg = Color(240, 140, 128, 255); glyph = "!";
        }
        const float cx = m_pos.x() + 11.0f, cy = m_pos.y() + 11.0f;
        nvgBeginPath(ctx);
        nvgCircle(ctx, cx, cy, 11.0f);
        nvgFillColor(ctx, bg);
        nvgFill(ctx);
        nvgFontFace(ctx, "sans-bold");
        nvgFontSize(ctx, 13.0f);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, fg);
        nvgText(ctx, cx, cy + 0.5f, glyph, nullptr);
    }
private:
    MessageDialog::Type m_type;
};

class DialogButton : public Button {
public:
    DialogButton(Widget *parent, std::string_view caption, bool primary) : Button(parent, caption), m_primary(primary) {
        const int width = std::max(72, 28 + (int) caption.size() * 8);
        set_fixed_size(Vector2i(width, 30));
    }
    void draw(NVGcontext *ctx) override {
        const float x = m_pos.x(), y = m_pos.y(), w = m_size.x(), h = m_size.y();
        const bool hot = m_mouse_focus && m_enabled;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, 4.0f);
        if (m_primary) {
            nvgFillColor(ctx, hot ? Color(250, 204, 112, 255) : Color(244, 191, 79, 255));
            nvgFill(ctx);
        } else {
            nvgFillColor(ctx, Color(255, 255, 255, hot ? 24 : 14));
            nvgFill(ctx);
            nvgStrokeColor(ctx, Color(255, 255, 255, 30));
            nvgStrokeWidth(ctx, 1.0f);
            nvgStroke(ctx);
        }
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 13.0f);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, m_primary ? Color(27, 23, 16, 255) : Color(236, 237, 238, 255));
        nvgText(ctx, x + w * 0.5f, y + h * 0.5f, m_caption.c_str(), nullptr);
    }
private:
    bool m_primary;
};

class DialogCloseButton : public Button {
public:
    explicit DialogCloseButton(Widget *parent) : Button(parent, "") {
        set_fixed_size(Vector2i(24, 24));
        set_tooltip("Close (Esc)");
    }
    void draw(NVGcontext *ctx) override {
        const float x = m_pos.x(), y = m_pos.y(), w = m_size.x(), h = m_size.y();
        if (m_mouse_focus) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x, y, w, h, 4.0f);
            nvgFillColor(ctx, Color(255, 255, 255, 18));
            nvgFill(ctx);
        }
        const float c = 4.5f, cx = x + w * 0.5f, cy = y + h * 0.5f;
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, cx - c, cy - c);
        nvgLineTo(ctx, cx + c, cy + c);
        nvgMoveTo(ctx, cx + c, cy - c);
        nvgLineTo(ctx, cx - c, cy + c);
        nvgStrokeColor(ctx, m_mouse_focus ? Color(236, 237, 238, 255) : Color(170, 174, 179, 220));
        nvgStrokeWidth(ctx, 1.5f);
        nvgStroke(ctx);
    }
};

} // namespace

MessageDialog::MessageDialog(Widget *parent, Type type, std::string_view title,
              std::string_view message,
              std::string_view button_text,
              std::string_view alt_button_text, bool alt_button) : Window(parent, "") {
    set_layout(new BoxLayout(Orientation::Vertical, Alignment::Minimum, 18, 14));
    set_modal(true);
    m_has_alt = alt_button;

    // A generic title (the app name, "Untitled") adds nothing: the message becomes the heading.
    const bool generic_title = title.empty() || title == "Untitled" || title == "hdrspace";

    Widget *top = new Widget(this);
    top->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Minimum, 0, 12));
    new DialogBadge(top, type);
    Widget *text = new Widget(top);
    text->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Minimum, 0, 6));
    if (!generic_title) {
        Label *heading = new Label(text, title, "sans-bold", 15);
        heading->set_color(Color(236, 237, 238, 255));
        heading->set_fixed_width(kDialogTextWidth);
    }
    m_message_label = new Label(text, message, "sans", generic_title ? 14 : 13);
    m_message_label->set_color(generic_title ? Color(226, 228, 230, 255) : Color(170, 174, 179, 235));
    m_message_label->set_fixed_width(kDialogTextWidth);
    auto *close = new DialogCloseButton(top);
    close->set_callback([this] { cancel(); });

    Widget *buttons = new Widget(this);
    buttons->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 8));
    if (alt_button) {
        Button *button = new DialogButton(buttons, alt_button_text, false);
        button->set_callback([this] { if (m_callback) m_callback(1); dispose(); });
    }
    Button *button = new DialogButton(buttons, button_text, true);
    button->set_callback([this] { if (m_callback) m_callback(0); dispose(); });
    center();
    request_focus();
}

void MessageDialog::cancel() {
    if (m_callback) m_callback(m_has_alt ? 1 : 0);
    dispose();
}

void MessageDialog::accept() {
    if (m_callback) m_callback(0);
    dispose();
}

// Message row on the left margin, button row against the right margin.
void MessageDialog::perform_layout(NVGcontext *ctx) {
    Window::perform_layout(ctx);
    if (m_children.size() >= 2) {
        Widget *buttons = m_children.back();
        buttons->set_position(Vector2i(m_size.x() - 18 - buttons->size().x(), buttons->position().y()));
    }
}

bool MessageDialog::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
            accept();
            return true;
        }
        if (key == GLFW_KEY_ESCAPE) {
            cancel();
            return true;
        }
    }
    return Window::keyboard_event(key, scancode, action, modifiers);
}

void MessageDialog::draw(NVGcontext *ctx) {
    const float x = m_pos.x(), y = m_pos.y(), w = m_size.x(), h = m_size.y();
    const float r = 8.0f;
    nvgSave(ctx);
    NVGpaint shadow = nvgBoxGradient(ctx, x, y + 6.0f, w, h, r * 2.0f, 24.0f, Color(0, 0, 0, 140), Color(0, 0, 0, 0));
    nvgBeginPath(ctx);
    nvgRect(ctx, x - 30.0f, y - 30.0f, w + 60.0f, h + 70.0f);
    nvgRoundedRect(ctx, x, y, w, h, r);
    nvgPathWinding(ctx, NVG_HOLE);
    nvgFillPaint(ctx, shadow);
    nvgFill(ctx);
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, r);
    nvgFillColor(ctx, Color(38, 39, 43, 255));
    nvgFill(ctx);
    nvgStrokeColor(ctx, Color(255, 255, 255, 26));
    nvgStrokeWidth(ctx, 1.0f);
    nvgStroke(ctx);
    nvgRestore(ctx);
    Widget::draw(ctx);
}

NAMESPACE_END(nanogui)
