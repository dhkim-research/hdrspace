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

#include <tev/HelpWindow.h>
#include <tev/Ipc.h>

#include <nanogui/button.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/textbox.h>
#include <nanogui/theme.h>
#include <nanogui/vscrollpanel.h>
#include <nanogui/window.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

using namespace nanogui;
using namespace std;
namespace fs = std::filesystem;

namespace tev {

namespace {

constexpr const char* kHdrspaceAboutMarkAsset = "hdrspace_icon_1024.png";

fs::path hdrspaceExecutablePath() {
#if defined(__APPLE__)
    std::vector<char> buffer(4096, '\0');
    uint32_t size = static_cast<uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size + 1, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return {};
        }
    }
    std::error_code ec;
    return fs::weakly_canonical(fs::path(buffer.data()), ec);
#else
    return {};
#endif
}

fs::path hdrspaceBrandingResourcePath(std::string_view fileName) {
    std::error_code ec;
    const fs::path exe = hdrspaceExecutablePath();
    if (!exe.empty()) {
        const fs::path contents = fs::weakly_canonical(exe.parent_path().parent_path(), ec);
        if (!ec && !contents.empty()) {
            const fs::path bundled = contents / "Resources" / "branding" / std::string(fileName);
            if (fs::exists(bundled)) {
                return bundled;
            }
        }
    }

    const fs::path cwdRelative = fs::current_path(ec) / "assets" / "branding" / std::string(fileName);
    if (!ec && fs::exists(cwdRelative)) {
        return cwdRelative;
    }

    return {};
}

// Contents/Resources/licenses inside the bundle (assembled from tools/
// collect_hdrspace_licenses.py); falls back to assets/licenses for source runs.
fs::path hdrspaceLicensesPath() {
    std::error_code ec;
    const fs::path exe = hdrspaceExecutablePath();
    if (!exe.empty()) {
        const fs::path bundled = exe.parent_path().parent_path() / "Resources" / "licenses";
        if (fs::is_directory(bundled, ec)) {
            return bundled;
        }
    }
    const fs::path cwdRelative = fs::current_path(ec) / "assets" / "licenses";
    if (!ec && fs::is_directory(cwdRelative, ec)) {
        return cwdRelative;
    }
    return {};
}

void revealInFinder(const fs::path& path) {
#if defined(__APPLE__)
    if (path.empty()) {
        return;
    }
    const std::string target = path.string();
    std::thread([target] {
        pid_t pid = 0;
        const char* argv[] = {"/usr/bin/open", target.c_str(), nullptr};
        if (posix_spawn(&pid, "/usr/bin/open", nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0) {
            int status = 0;
            waitpid(pid, &status, 0);
        }
    }).detach();
#else
    (void)path;
#endif
}

class AboutBrandMark final : public Widget {
public:
    explicit AboutBrandMark(Widget* parent, fs::path imagePath) : Widget(parent), mImagePath(std::move(imagePath)) {
        set_fixed_size({72, 72});
    }

    void draw(NVGcontext* ctx) override {
        if (!mLoaded) {
            mLoaded = true;
            if (!mImagePath.empty() && fs::exists(mImagePath)) {
                mImageHandle = nvgCreateImage(ctx, mImagePath.string().c_str(), 0);
            }
        }

        const float x = static_cast<float>(m_pos.x());
        const float y = static_cast<float>(m_pos.y());
        const float w = static_cast<float>(m_size.x());
        const float h = static_cast<float>(m_size.y());
        if (mImageHandle != 0) {
            NVGpaint paint = nvgImagePattern(ctx, x, y, w, h, 0.0f, mImageHandle, 1.0f);
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x, y, w, h, 4.0f);
            nvgFillPaint(ctx, paint);
            nvgFill(ctx);
        }
    }

private:
    fs::path mImagePath;
    int mImageHandle = 0;
    bool mLoaded = false;
};

const Color kInk(236, 237, 238, 255);
const Color kMuted(170, 174, 179, 235);
const Color kAccent(244, 191, 79, 255);

void drawHairline(NVGcontext* ctx, float x0, float x1, float y) {
    nvgBeginPath(ctx);
    nvgMoveTo(ctx, x0, y);
    nvgLineTo(ctx, x1, y);
    nvgStrokeColor(ctx, Color(255, 255, 255, 16));
    nvgStrokeWidth(ctx, 1.0f);
    nvgStroke(ctx);
}

// Header bar: first child pinned left, the rest right-aligned; hairline at the bottom.
class HelpHeaderBar final : public Widget {
public:
    explicit HelpHeaderBar(Widget* parent) : Widget(parent) {}
    void perform_layout(NVGcontext* ctx) override {
        const int h = m_size.y();
        int left = 18;
        int right = m_size.x() - 12;
        // children: [title, pills, (spacer...), search, close]: title and pills go left, rest right.
        for (size_t i = 0; i < m_children.size(); ++i) {
            Widget* child = m_children[i];
            const Vector2i fix = child->fixed_size();
            const Vector2i pref = child->preferred_size(ctx);
            const Vector2i size{fix.x() ? fix.x() : pref.x(), fix.y() ? fix.y() : pref.y()};
            child->set_size(size);
            if (i < 2) {
                child->set_position({left, (h - size.y()) / 2});
                left += size.x() + 14;
            }
        }
        for (int i = static_cast<int>(m_children.size()) - 1; i >= 2; --i) {
            Widget* child = m_children[static_cast<size_t>(i)];
            if (!child->visible()) continue;
            right -= child->size().x();
            child->set_position({right, (h - child->size().y()) / 2});
            right -= 8;
        }
        for (auto* child : m_children) child->perform_layout(ctx);
    }
    void draw(NVGcontext* ctx) override {
        drawHairline(ctx, m_pos.x(), m_pos.x() + m_size.x(), m_pos.y() + m_size.y() - 0.5f);
        Widget::draw(ctx);
    }
};

class HelpTitle final : public Widget {
public:
    HelpTitle(Widget* parent, std::string text) : Widget(parent), mText(std::move(text)) {}
    Vector2i preferred_size_impl(NVGcontext* ctx) const override {
        nvgFontFace(ctx, "sans-bold");
        nvgFontSize(ctx, 16.0f);
        return {static_cast<int>(nvgTextBounds(ctx, 0, 0, mText.c_str(), nullptr, nullptr)) + 2, 22};
    }
    void draw(NVGcontext* ctx) override {
        nvgFontFace(ctx, "sans-bold");
        nvgFontSize(ctx, 16.0f);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, kInk);
        nvgText(ctx, m_pos.x(), m_pos.y() + m_size.y() * 0.5f, mText.c_str(), nullptr);
    }

private:
    std::string mText;
};

class HelpPillTrack final : public Widget {
public:
    explicit HelpPillTrack(Widget* parent) : Widget(parent) {
        set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 2, 4});
    }
    void draw(NVGcontext* ctx) override {
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, m_pos.x() + 0.5f, m_pos.y() + 0.5f, m_size.x() - 1.0f, m_size.y() - 1.0f, 6.0f);
        nvgFillColor(ctx, Color(30, 31, 34, 255));
        nvgFill(ctx);
        nvgStrokeColor(ctx, Color(255, 255, 255, 18));
        nvgStrokeWidth(ctx, 1.0f);
        nvgStroke(ctx);
        Widget::draw(ctx);
    }
};

class HelpPill final : public Button {
public:
    HelpPill(Widget* parent, const std::string& caption) : Button(parent, caption) { set_flags(Button::RadioButton); }
    Vector2i preferred_size_impl(NVGcontext* ctx) const override {
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 13.0f);
        return {static_cast<int>(nvgTextBounds(ctx, 0, 0, m_caption.c_str(), nullptr, nullptr)) + 24, 26};
    }
    void draw(NVGcontext* ctx) override {
        const float x = m_pos.x(), y = m_pos.y(), w = m_size.x(), h = m_size.y();
        if (m_pushed || m_mouse_focus) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x, y, w, h, 4.0f);
            nvgFillColor(ctx, m_pushed ? Color(52, 53, 58, 255) : Color(255, 255, 255, 10));
            nvgFill(ctx);
        }
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 13.0f);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, m_pushed ? kAccent : kMuted);
        nvgText(ctx, x + w * 0.5f, y + h * 0.5f, m_caption.c_str(), nullptr);
    }
};

class HelpCloseButton final : public Button {
public:
    explicit HelpCloseButton(Widget* parent) : Button(parent, "") {
        set_fixed_size({28, 28});
        set_tooltip("Close (Esc)");
    }
    void draw(NVGcontext* ctx) override {
        const float x = m_pos.x(), y = m_pos.y(), w = m_size.x(), h = m_size.y();
        if (m_mouse_focus || m_pushed) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x, y, w, h, 4.0f);
            nvgFillColor(ctx, Color(255, 255, 255, m_pushed ? 26 : 16));
            nvgFill(ctx);
        }
        const auto glyph = utf8(FA_TIMES);
        nvgFontFace(ctx, "icons");
        nvgFontSize(ctx, 14.0f);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, m_mouse_focus ? kInk : kMuted);
        nvgText(ctx, x + w * 0.5f, y + h * 0.5f + 1.0f, glyph.data(), nullptr);
    }
};

// Search field that reports every edit (NanoGUI's TextBox only reports on Enter / blur).
class HelpSearchBox final : public TextBox {
public:
    HelpSearchBox(Widget* parent, std::function<void(const std::string&)> onEdit)
        : TextBox(parent, ""), mOnEdit(std::move(onEdit)) {
        set_editable(true);
        set_alignment(TextBox::Alignment::Left);
        set_placeholder("Search shortcuts");
        set_font_size(13);
        set_fixed_size({190, 28});
    }
    bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        const bool handled = TextBox::keyboard_event(key, scancode, action, modifiers);
        notify();
        return handled;
    }
    bool keyboard_character_event(unsigned int codepoint) override {
        const bool handled = TextBox::keyboard_character_event(codepoint);
        notify();
        return handled;
    }

private:
    void notify() {
        const std::string current = focused() ? m_value_temp : m_value;
        if (current != mLast) {
            mLast = current;
            if (mOnEdit) mOnEdit(current);
        }
    }
    std::function<void(const std::string&)> mOnEdit;
    std::string mLast;
};

// 11 bold caps section heading.
class HelpSectionHeading final : public Widget {
public:
    HelpSectionHeading(Widget* parent, std::string text, int width) : Widget(parent), mText(std::move(text)) {
        std::transform(mText.begin(), mText.end(), mText.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        set_fixed_size({width, 34});
    }
    void draw(NVGcontext* ctx) override {
        nvgFontFace(ctx, "sans-bold");
        nvgFontSize(ctx, 11.0f);
        nvgTextLetterSpacing(ctx, 0.8f);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        nvgFillColor(ctx, Color(154, 151, 143, 255));
        nvgText(ctx, m_pos.x(), m_pos.y() + m_size.y() - 7.0f, mText.c_str(), nullptr);
        nvgTextLetterSpacing(ctx, 0.0f);
    }

private:
    std::string mText;
};

// One shortcut: description left, key caps right (⌘ ⇧ ⌥ drawn as symbols on macOS).
class ShortcutRow final : public Widget {
public:
    ShortcutRow(Widget* parent, const std::string& keys, const std::string& desc, int width)
        : Widget(parent), mDesc(desc), mSearchText(desc + " " + keys) {
        std::transform(mSearchText.begin(), mSearchText.end(), mSearchText.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        set_fixed_size({width, 30});
        parseKeys(keys);
    }
    bool matches(const std::string& lowerQuery) const { return lowerQuery.empty() || mSearchText.find(lowerQuery) != std::string::npos; }

    void draw(NVGcontext* ctx) override {
        const float x = m_pos.x(), y = m_pos.y(), w = m_size.x(), h = m_size.y();
        const float cy = y + h * 0.5f;
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 13.0f);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, kInk);
        nvgText(ctx, x, cy, mDesc.c_str(), nullptr);

        // Measure caps right-to-left.
        float right = x + w;
        for (int a = static_cast<int>(mAlternatives.size()) - 1; a >= 0; --a) {
            const auto& alt = mAlternatives[static_cast<size_t>(a)];
            for (int k = static_cast<int>(alt.caps.size()) - 1; k >= 0; --k) {
                const auto& cap = alt.caps[static_cast<size_t>(k)];
                const bool symbol = isSymbol(cap);
                nvgFontFace(ctx, symbol ? "mono" : "sans");
                nvgFontSize(ctx, symbol ? 14.0f : 11.5f);
                const float tw = nvgTextBounds(ctx, 0, 0, cap.c_str(), nullptr, nullptr);
                const float cw = std::max(20.0f, tw + 12.0f);
                const float cx = right - cw;
                nvgBeginPath(ctx);
                nvgRoundedRect(ctx, cx, cy - 10.0f, cw, 21.0f, 4.0f);
                nvgFillColor(ctx, Color(22, 23, 25, 255));
                nvgFill(ctx);
                nvgBeginPath(ctx);
                nvgRoundedRect(ctx, cx + 0.5f, cy - 10.5f, cw - 1.0f, 20.0f, 4.0f);
                nvgFillColor(ctx, Color(33, 34, 38, 255));
                nvgFill(ctx);
                nvgStrokeColor(ctx, Color(255, 255, 255, 26));
                nvgStrokeWidth(ctx, 1.0f);
                nvgStroke(ctx);
                nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                nvgFillColor(ctx, Color(217, 214, 207, 255));
                nvgText(ctx, cx + cw * 0.5f, cy - 0.5f, cap.c_str(), nullptr);
                right = cx - 3.0f;
            }
            if (a > 0) {
                const char* sep = alt.joinWithSlash ? "/" : "or";
                nvgFontFace(ctx, "sans");
                nvgFontSize(ctx, 11.0f);
                nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                nvgFillColor(ctx, Color(130, 133, 138, 255));
                right -= 5.0f;
                nvgText(ctx, right, cy, sep, nullptr);
                right -= nvgTextBounds(ctx, 0, 0, sep, nullptr, nullptr) + 6.0f;
            }
        }
        drawHairline(ctx, x, x + w, y + h - 0.5f);
    }

private:
    struct Alternative {
        std::vector<std::string> caps;
        bool joinWithSlash = false; // separator drawn before this alternative
    };

    static bool isSymbol(const std::string& cap) {
        return cap == "⌘" || cap == "⇧" || cap == "⌥";
    }

    static std::string capFor(std::string token) {
#ifdef __APPLE__
        if (token == "Cmd") return "⌘";
        if (token == "Shift") return "⇧";
        if (token == "Opt") return "⌥";
#endif
        return token;
    }

    static std::vector<std::string> split(const std::string& text, const std::string& sep) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            const size_t pos = text.find(sep, start);
            if (pos == std::string::npos) {
                parts.push_back(text.substr(start));
                break;
            }
            parts.push_back(text.substr(start, pos - start));
            start = pos + sep.size();
        }
        return parts;
    }

    void addAlternative(const std::string& alt, bool slash) {
        Alternative entry;
        entry.joinWithSlash = slash;
        if (alt.empty() || alt[0] == '+' || alt.find("(+") != std::string::npos) {
            entry.caps.push_back(alt);
        } else {
            for (const auto& token : split(alt, "+")) {
                if (!token.empty()) entry.caps.push_back(capFor(token));
            }
        }
        mAlternatives.push_back(std::move(entry));
    }

    void parseKeys(const std::string& keys) {
        for (const auto& alt : split(keys, " or ")) {
            // "E/Shift+E" or "Ctrl+Tab/Ctrl+Shift+Tab": a slash between whole combos.
            const auto slashParts = split(alt, "/");
            bool comboSlash = slashParts.size() > 1 && alt[0] != '+' && alt.find("(+") == std::string::npos;
            if (comboSlash) {
                comboSlash = false;
                for (size_t i = 1; i < slashParts.size(); ++i) {
                    if (slashParts[i].find('+') != std::string::npos) comboSlash = true;
                }
            }
            if (comboSlash) {
                for (size_t i = 0; i < slashParts.size(); ++i) addAlternative(slashParts[i], i > 0);
            } else {
                addAlternative(alt, false);
            }
        }
    }

    std::string mDesc;
    std::string mSearchText;
    std::vector<Alternative> mAlternatives;
};

// One credited work: name + licence badge on the first line, authors under it.
class CreditRow final : public Widget {
public:
    CreditRow(Widget* parent, std::string name, std::string authors, std::string license, int width)
        : Widget(parent), mName(std::move(name)), mAuthors(std::move(authors)), mLicense(std::move(license)) {
        set_fixed_size({width, 42});
    }
    void draw(NVGcontext* ctx) override {
        const float x = m_pos.x(), y = m_pos.y(), w = m_size.x();
        drawHairline(ctx, x, x + w, y + 0.5f);
        nvgFontFace(ctx, "sans-bold");
        nvgFontSize(ctx, 13.0f);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, kInk);
        nvgText(ctx, x, y + 14.0f, mName.c_str(), nullptr);
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 12.0f);
        nvgFillColor(ctx, kMuted);
        nvgText(ctx, x, y + 31.0f, mAuthors.c_str(), nullptr);

        nvgFontSize(ctx, 10.5f);
        const float tw = nvgTextBounds(ctx, 0, 0, mLicense.c_str(), nullptr, nullptr);
        const float bw = tw + 14.0f;
        const float bx = x + w - bw;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, bx + 0.5f, y + 5.5f, bw - 1.0f, 17.0f, 8.5f);
        nvgFillColor(ctx, Color(30, 31, 34, 255));
        nvgFill(ctx);
        nvgStrokeColor(ctx, Color(255, 255, 255, 26));
        nvgStrokeWidth(ctx, 1.0f);
        nvgStroke(ctx);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, Color(201, 198, 191, 255));
        nvgText(ctx, bx + bw * 0.5f, y + 14.0f, mLicense.c_str(), nullptr);
    }

private:
    std::string mName;
    std::string mAuthors;
    std::string mLicense;
};

} // namespace

#ifdef __APPLE__
string HelpWindow::COMMAND = "Cmd";
#else
string HelpWindow::COMMAND = "Ctrl";
#endif

#ifdef __APPLE__
string HelpWindow::ALT = "Opt";
#else
string HelpWindow::ALT = "Alt";
#endif

HelpWindow::HelpWindow(Widget* parent, weak_ptr<Ipc> weakIpc, function<void()> closeCallback) :
    Window{parent, ""}, mCloseCallback{closeCallback} {
    (void)weakIpc;

    static constexpr int WINDOW_WIDTH = 720;
    static constexpr int HEADER_HEIGHT = 52;
    // Fit the screen: header + body + a margin above and below.
    const int parentHeight = parent && parent->height() > 0 ? parent->height() : 720;
    const int BODY_HEIGHT = std::clamp(parentHeight - HEADER_HEIGHT - 80, 260, 560);
    static constexpr int PAD = 20;
    const int contentWidth = WINDOW_WIDTH - PAD * 2 - 12;

    set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});
    set_fixed_width(WINDOW_WIDTH);

    // Header: title, Shortcuts | About pills, search, close.
    auto* header = new HelpHeaderBar{this};
    header->set_fixed_size({WINDOW_WIDTH, HEADER_HEIGHT});
    new HelpTitle{header, "Help"};
    auto* pills = new HelpPillTrack{header};
    mKeysTab = new HelpPill{pills, "Shortcuts"};
    mAboutTab = new HelpPill{pills, "About"};
    mKeysTab->set_callback([this] { selectTab(0); });
    mAboutTab->set_callback([this] { selectTab(1); });
    mSearch = new HelpSearchBox{header, [this](const std::string& query) { applyShortcutFilter(query); }};
    auto* closeButton = new HelpCloseButton{header};
    closeButton->set_callback([this] { mCloseCallback(); });

    auto* body = new Widget{this};
    body->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    // ---- Shortcuts --------------------------------------------------------------------------
    mScrollPanel = new VScrollPanel{body};
    Widget* shortcuts = new Widget(mScrollPanel);
    shortcuts->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Minimum, PAD, 0});

    const auto section = [&](const string& title) {
        mShortcutSections.push_back({new HelpSectionHeading{shortcuts, title, contentWidth}, {}});
    };
    const auto addRow = [&](string keys, string desc) {
        mShortcutSections.back().second.push_back(new ShortcutRow{shortcuts, keys, desc, contentWidth});
    };

    section("Workspace");
    addRow(ALT + "+" + COMMAND + "+1…5", "Viewer … View Visibility (main tools)");
    addRow("Ctrl+" + COMMAND + "+S", "Show / hide the tool list");
    addRow(COMMAND + "+B", "Show / hide the inspector");
    addRow("Shift+" + COMMAND + "+F", "Focus on the image (hide all panels)");

    section("Image loading");
    addRow(COMMAND + "+O", "Open image");
    addRow(COMMAND + "+S", "Save view as image");
    addRow(COMMAND + "+R or F5", "Reload image");
    addRow(COMMAND + "+Shift+R or " + COMMAND + "+F5", "Reload all images");
    addRow(COMMAND + "+W", "Close image");
    addRow(COMMAND + "+Shift+W", "Close all images");
    addRow(COMMAND + "+C", "Copy image to clipboard");
    addRow(COMMAND + "+Shift+C", "Copy image name to clipboard");
    addRow(COMMAND + "+V", "Paste image from clipboard");

    section("Image options");
    addRow("Left Click", "Select hovered image");
    addRow("1…9", "Select N-th image");
    addRow("Down/Up or S/W or Ctrl+Tab/Ctrl+Shift+Tab", "Select next/previous image");
    addRow("Home/End", "Select first/last image");
    addRow("Space", "Toggle playback of images as video");
    addRow("Click & Drag or H/J/K/L (+Shift/Ctrl)", "Translate image");
    addRow("Click & Drag+C (hold)", "Crop image");
    addRow("+/- or Scroll (+Shift/Ctrl)", "Zoom in/out of image");
    addRow(COMMAND + "+0", "Zoom to actual size");
    addRow(COMMAND + "+9/F", "Zoom to fit");
    addRow("N", "Normalize image to [0, 1]");
    addRow("R", "Reset image parameters");
    addRow("U", "Toggle clipping to [0, 1] (LDR mode)");
    addRow("Shift+Right/Left or Shift+D/A", "Select next/previous tonemap");
    addRow("E/Shift+E", "Increase/decrease exposure by 0.5");
    addRow("O/Shift+O", "Increase/decrease offset by 0.1");
    addRow("B (hold)", "Draw a border around the image");
    addRow("Shift+Ctrl (hold)", "Display sRGB bytes when zoomed-in");
#ifdef __APPLE__
    addRow("Enter", "Rename the image");
#else
    addRow("F2", "Rename the image");
#endif

    section("Reference options");
    addRow("Shift (hold)", "View currently selected reference");
    addRow("Shift+Left Click or Right Click", "Select hovered image as reference");
    addRow("Shift+1…9", "Select N-th image as reference");
    addRow("Shift+Down/Up or Shift+S/W", "Select next/previous image as reference");
    addRow("Ctrl (hold)", "View selected image if reference is selected");
    addRow("Ctrl+Right/Left or Ctrl+D/A", "Select next/previous error metric");

    section("Channel group options");
    addRow("Left Click", "Select hovered channel group");
    addRow("Ctrl+1…9", "Select N-th channel group");
    addRow("Right/Left or D/A or ]/[", "Select next/previous channel group");
    addRow("X", "Ungroup current channel group");

    section("Interface");
    addRow(ALT + "+Enter", "Maximize");
    addRow("?", "Show help (this window)");
    addRow("I", "Show image info and metadata");
    addRow(COMMAND + "+F", "Find image or channel group");
    addRow("Escape", "Reset find string");
    addRow(COMMAND + "+Q", "Quit");

    // ---- About ------------------------------------------------------------------------------
    // Product, authorship, the original works hdrspace builds on, what was changed, and the
    // notices their licenses require.
    mAboutScrollPanel = new VScrollPanel{body};
    Widget* about = new Widget(mAboutScrollPanel);
    about->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Minimum, PAD, 6});

    static constexpr auto addSpacer = [](Widget* current, int space) {
        auto row = new Widget{current};
        row->set_fixed_height(space);
    };
    auto addParagraph = [&](Widget* current, const string& text, int fontSize = 13, Color color = kMuted) {
        auto* label = new Label{current, text, "sans", fontSize};
        label->set_fixed_width(contentWidth);
        label->set_color(color);
        return label;
    };
    auto addHeading = [&](Widget* current, const string& text) {
        return new HelpSectionHeading{current, text, contentWidth};
    };
    // One credited work: name, authors and license badge, then what hdrspace uses or changed.
    auto addWork = [&](Widget* current, const string& name, const string& authors, const string& license,
                       const vector<string>& lines) {
        new CreditRow{current, name, authors, license, contentWidth};
        for (const auto& line : lines) {
            addParagraph(current, line, 12, Color(190, 193, 197, 235));
        }
        addSpacer(current, 6);
    };

    auto* brandRow = new Widget{about};
    brandRow->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 0, 14});
    auto* brandMark = new AboutBrandMark{brandRow, hdrspaceBrandingResourcePath(kHdrspaceAboutMarkAsset)};
    brandMark->set_fixed_size({52, 52});
    auto* brandText = new Widget{brandRow};
    brandText->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Minimum, 0, 3});
    new Label{brandText, "hdrspace", "sans-bold", 20};
#ifdef HDRSPACE_VERSION
    auto* versionLabel = new Label{brandText, string("Version ") + HDRSPACE_VERSION + "  ·  © 2026 Dong Hyun Kim  ·  GNU GPL v3.0", "sans", 12};
#else
    auto* versionLabel = new Label{brandText, "© 2026 Dong Hyun Kim  ·  GNU GPL v3.0", "sans", 12};
#endif
    versionLabel->set_color(kMuted);

    addSpacer(about, 4);
    addParagraph(about, "HDR imaging, glare and visibility analysis in one native workspace.", 14, kInk);
    addParagraph(about,
        "hdrspace and its HDR engine mergehdr are developed by Dong Hyun Kim. hdrspace is free software: you can "
        "redistribute it and/or modify it under the terms of the GNU General Public License, version 3. It is "
        "distributed WITHOUT ANY WARRANTY; see the license texts below.", 12);

    addHeading(about, "Built on");
    addParagraph(about, "Each entry names the original authors and license, then what hdrspace uses or changed.", 12);
    addSpacer(about, 2);

    addWork(about, "tev", "Thomas Müller", "GPL v3.0",
        {"hdrspace is a modified version of tev. Its viewer, image loading, display and comparison are kept; hdrspace "
         "adds the workspace around it (tool list, inspectors and analysis panels) and crop, projection and "
         "perceptual value modes."});
    addWork(about, "linearhdr and pylinearhdr", "Stephen Wasilewski, EPFL", "LGPL v3.0 / GPL v3.0",
        {"mergehdr re-implements the linearhdr RAW-to-HDR pipeline in C++ (Dong Hyun Kim): exposure calibration, "
         "linearhdr merge weighting, DHT demosaicing and color conversion. It adds fisheye projection, vignetting "
         "correction and color, shutter and aperture calibration tools."});
    addWork(about, "hdrmerge", "Wenzel Jakob", "GPL v3.0",
        {"The default fast merge path of mergehdr is based on the hdrmerge merging code."});
    addWork(about, "evalglare", "J. Wienold, Fraunhofer ISE and EPFL", "Evalglare Software License 3.0",
        {"This product includes the evalglare software, developed at Fraunhofer ISE and EPFL by J. Wienold.",
         "This software uses a modified version of the source code of evalglare.",
         "Glare analysis in hdrspace is a C++ re-implementation of evalglare by Dong Hyun Kim. Angles are entered in "
         "degrees, and equivalent-luminance and local-only evaluation modes are added."});
    addWork(about, "HDR-VDP 3", "Rafał K. Mantiuk", "BSD 3-Clause",
        {"© 2010–2020 Rafal Mantiuk, © 2020 Displays and Graphics Ltd.",
         "View Visibility ports the parts of HDR-VDP 3.0.7 that compute visibility and detectable contrast from "
         "MATLAB to C++ (Dong Hyun Kim). The HDR-VDP spectral data files are included unchanged."});
    addWork(about, "matlabPyrTools", "Laboratory for Computational Vision, NYU", "MIT",
        {"The steerable-pyramid convolution code (convolve.c, edges.c) is used unchanged by View Visibility."});
    addWork(about, "Radiance", "Lawrence Berkeley National Laboratory", "Radiance Software License 2.0",
        {"This product includes Radiance software (http://radsite.lbl.gov/) developed by the Lawrence Berkeley "
         "National Laboratory (http://www.lbl.gov/).",
         "The Radiance command-line tools are bundled for Create View rendering."});
    addWork(about, "NanoGUI and NanoVG", "Wenzel Jakob; Mikko Mononen", "BSD 3-Clause · zlib",
        {"The native user interface toolkit and its vector drawing."});
    addWork(about, "Image, color and math libraries", "Various authors", "Various",
        {"OpenEXR, Imath, LibRaw, libjpeg-turbo, libpng, LibTIFF, libwebp, libjxl, OpenJPEG, Little-CMS, Exiv2, "
         "FFTW, Boost, Eigen, libomp and others, as listed in the license folder."});

    addHeading(about, "Downloaded on request");
    addParagraph(about,
        "The AI segmentation (SAM 3) and depth (Depth Anything 3) models, and the SpectralDB material list used by "
        "Create View, are not part of hdrspace. They are downloaded only when you ask for them and remain under "
        "their own licenses.", 12);

    addHeading(about, "License texts");
    auto* licenseRow = new Widget{about};
    licenseRow->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 0, 12});
    auto* licenseNote = new Label{licenseRow, "Full license texts ship in Contents/Resources/licenses.", "sans", 12};
    licenseNote->set_color(kMuted);
    const fs::path licensesPath = hdrspaceLicensesPath();
    auto* openLicenses = new Button{licenseRow, licensesPath.empty() ? "Not found in this build" : "Open license folder"};
    openLicenses->set_font_size(13);
    openLicenses->set_fixed_height(28);
    openLicenses->set_enabled(!licensesPath.empty());
    openLicenses->set_callback([licensesPath] { revealInFinder(licensesPath); });
    addSpacer(about, 8);

    perform_layout(screen()->nvg_context());
    for (auto* panel : {mScrollPanel, mAboutScrollPanel}) {
        panel->set_fixed_size({WINDOW_WIDTH - 4, BODY_HEIGHT});
    }
    body->set_fixed_size({WINDOW_WIDTH, BODY_HEIGHT + 8});
    selectTab(0);
}

void HelpWindow::draw(NVGcontext* ctx) {
    const float x = m_pos.x(), y = m_pos.y(), w = m_size.x(), h = m_size.y();
    nvgSave(ctx);
    NVGpaint shadow = nvgBoxGradient(ctx, x, y + 8.0f, w, h, 10.0f, 30.0f, Color(0, 0, 0, 150), Color(0, 0, 0, 0));
    nvgBeginPath(ctx);
    nvgRect(ctx, x - 40.0f, y - 30.0f, w + 80.0f, h + 80.0f);
    nvgRoundedRect(ctx, x, y, w, h, 8.0f);
    nvgPathWinding(ctx, NVG_HOLE);
    nvgFillPaint(ctx, shadow);
    nvgFill(ctx);
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, 8.0f);
    nvgFillColor(ctx, Color(38, 39, 43, 255));
    nvgFill(ctx);
    nvgStrokeColor(ctx, Color(255, 255, 255, 24));
    nvgStrokeWidth(ctx, 1.0f);
    nvgStroke(ctx);
    nvgRestore(ctx);
    Widget::draw(ctx);
}

void HelpWindow::selectTab(int tab) {
    mTab = tab;
    if (mKeysTab) mKeysTab->set_pushed(tab == 0);
    if (mAboutTab) mAboutTab->set_pushed(tab == 1);
    if (mScrollPanel) mScrollPanel->set_visible(tab == 0);
    if (mAboutScrollPanel) mAboutScrollPanel->set_visible(tab == 1);
    if (mSearch) mSearch->set_visible(tab == 0);
    if (screen()) {
        screen()->perform_layout();
        screen()->redraw();
    }
}

void HelpWindow::applyShortcutFilter(const std::string& query) {
    std::string lower = query;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (auto& [heading, rows] : mShortcutSections) {
        bool any = false;
        for (auto* row : rows) {
            const bool match = static_cast<ShortcutRow*>(row)->matches(lower);
            row->set_visible(match);
            any = any || match;
        }
        heading->set_visible(any);
    }
    if (mScrollPanel) {
        mScrollPanel->set_scroll(0.0f);
    }
    if (screen()) {
        screen()->perform_layout();
        screen()->redraw();
    }
}

bool HelpWindow::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (Window::keyboard_event(key, scancode, action, modifiers)) {
        return true;
    }

    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_Q) {
            mCloseCallback();
            return true;
        } else if (key == GLFW_KEY_TAB && (modifiers & GLFW_MOD_CONTROL)) {
            selectTab(mTab == 0 ? 1 : 0);
            return true;
        } else if (key == GLFW_KEY_J) {
            auto* activeScrollPanel = mTab == 0 ? mScrollPanel : mAboutScrollPanel;
            if (activeScrollPanel) {
                activeScrollPanel->scroll_absolute(48.0f);
            }
            return true;
        } else if (key == GLFW_KEY_K) {
            auto* activeScrollPanel = mTab == 0 ? mScrollPanel : mAboutScrollPanel;
            if (activeScrollPanel) {
                activeScrollPanel->scroll_absolute(-48.0f);
            }
            return true;
        }
    }

    return false;
}

} // namespace tev
