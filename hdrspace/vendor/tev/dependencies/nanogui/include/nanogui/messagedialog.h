/*
    nanogui/messagedialog.h -- Simple "OK" or "Yes/No"-style modal dialogs

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/
/** \file */

#pragma once

#include <nanogui/window.h>

NAMESPACE_BEGIN(nanogui)

/**
 * \class MessageDialog messagedialog.h nanogui/messagedialog.h
 *
 * \brief Simple "OK" or "Yes/No"-style modal dialogs.
 */
class NANOGUI_EXPORT MessageDialog : public Window {
public:
    /// Classification of the type of message this MessageDialog represents.
    enum class Type {
        Information,
        Question,
        Warning
    };

    MessageDialog(Widget *parent, Type type, std::string_view title = "Untitled",
                  std::string_view message = "Message",
                  std::string_view button_text = "OK",
                  std::string_view alt_button_text = "Cancel", bool alt_button = false);

    Label *message_label() { return m_message_label; }
    const Label *message_label() const { return m_message_label; }

    const std::function<void(int)> &callback() const { return m_callback; }
    void set_callback(const std::function<void(int)> &callback) { m_callback = callback; }

    /// hdrspace: card-style drawing and Enter / Esc handling
    virtual void draw(NVGcontext *ctx) override;
    virtual void perform_layout(NVGcontext *ctx) override;
    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override;
    /// Close the dialog the way the Cancel / alternative button does (Esc, the x button)
    void cancel();
    /// Accept the dialog the way the primary button does (Enter)
    void accept();
protected:
    std::function<void(int)> m_callback;
    Label *m_message_label;
    bool m_has_alt = false;
};

NAMESPACE_END(nanogui)
