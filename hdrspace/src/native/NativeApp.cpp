#include "native/NativeApp.h"

#include "platform/ProcessRunner.h"

#include <tev/BackgroundImagesLoader.h>
#include <tev/Box.h>
#include <tev/Common.h>
#include <tev/Image.h>
#include <tev/ImageCanvas.h>
#include <tev/ThreadPool.h>

#include <nanogui/button.h>
#include <nanogui/checkbox.h>
#include <nanogui/combobox.h>
#include <nanogui/common.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/messagedialog.h>
#include <nanogui/opengl.h>
#include <nanogui/progressbar.h>
#include <nanogui/screen.h>
#include <nanogui/slider.h>
#include <nanogui/tabwidget.h>
#include <nanogui/textarea.h>
#include <nanogui/textbox.h>
#include <nanogui/theme.h>
#include <nanogui/vscrollpanel.h>
#include <nanogui/widget.h>
#include <nanogui/window.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mach-o/dyld.h>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace nanogui;
using namespace std::chrono_literals;

namespace {

fs::path safeCurrentPath() {
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (!ec && !cwd.empty()) {
        return cwd;
    }
    if (const char* home = std::getenv("HOME")) {
        fs::path homePath{home};
        if (!homePath.empty()) {
            return homePath;
        }
    }
    fs::path tempDir = fs::temp_directory_path(ec);
    if (!ec && !tempDir.empty()) {
        return tempDir;
    }
    return "/";
}

std::string trim(const std::string& text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return text.substr(start, end - start);
}

std::string normalizedTextBoxValue(std::string text) {
    text = trim(text);
    if (text.empty()) {
        return {};
    }

    const std::string lowered = tev::toLower(text);
    static const std::set<std::string> sentinels = {
        "untitled",
        "default",
        "optional",
        "optional override",
        "advanced options",
        "output folder",
        "reference hdr",
        "test hdr",
        "ref_cells.txt",
        "test_cells.txt",
        "shutterc",
        "x y w h",
        "r g b [g2]",
        "mergehdr binary",
        "external tev (optional)",
        "oiiotool",
        "default (profile / libraw)",
        "default (profile / libraw defaults)",
    };

    if (sentinels.contains(lowered)) {
        return {};
    }

    return text;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream ss{text};
    std::string line;
    while (std::getline(ss, line)) {
        line = trim(line);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }

    return lines;
}

std::string readFile(const fs::path& path) {
    std::ifstream input{path};
    if (!input) {
        return {};
    }

    std::ostringstream oss;
    oss << input.rdbuf();
    return oss.str();
}

std::map<std::string, std::string> readKeyValueFile(const fs::path& path) {
    std::map<std::string, std::string> values;
    if (!fs::exists(path)) {
        return values;
    }

    std::ifstream input{path};
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }

    return values;
}

void writeKeyValueFile(const fs::path& path, const std::map<std::string, std::string>& values) {
    fs::create_directories(path.parent_path());
    std::ofstream output{path};
    if (!output) {
        throw std::runtime_error("Could not write settings file.");
    }

    for (const auto& [key, value] : values) {
        output << key << "=" << value << "\n";
    }
}

std::map<std::string, std::string> parseProfileFile(const fs::path& path) {
    std::map<std::string, std::string> values;
    if (!fs::exists(path)) {
        return values;
    }

    std::ifstream input{path};
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == '[') {
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }

    return values;
}

std::vector<std::string> listProfileNames(const fs::path& profilesDir) {
    std::vector<std::string> profiles;
    if (!fs::exists(profilesDir)) {
        return profiles;
    }

    for (const auto& entry : fs::directory_iterator{profilesDir}) {
        if (entry.is_regular_file() && entry.path().extension() == ".cfg") {
            profiles.push_back(entry.path().stem().string());
        }
    }

    std::sort(profiles.begin(), profiles.end());
    return profiles;
}

std::string shellQuote(const std::string& value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

std::string quoteInputFiles(const std::vector<std::string>& files) {
    std::ostringstream oss;
    for (size_t i = 0; i < files.size(); ++i) {
        if (i) {
            oss << ' ';
        }
        oss << shellQuote(files[i]);
    }
    return oss.str();
}

pid_t spawnShellJob(const std::string& command, const fs::path& logPath) {
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("Could not start merge job.");
    }

    if (pid == 0) {
        int fd = open(logPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            _exit(127);
        }

        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        execl("/bin/zsh", "zsh", "-lc", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    return pid;
}

int lastPercentInText(const std::string& text) {
    int last = -1;
    std::stringstream ss{text};
    std::string line;
    while (std::getline(ss, line)) {
        size_t pos = 0;
        while ((pos = line.find('%', pos)) != std::string::npos) {
            size_t start = pos;
            while (start > 0 && std::isdigit(static_cast<unsigned char>(line[start - 1]))) {
                --start;
            }

            if (start < pos) {
                try {
                    last = std::stoi(line.substr(start, pos - start));
                } catch (...) {
                }
            }

            ++pos;
        }
    }

    return last;
}

std::string lastStageInText(const std::string& text) {
    std::stringstream ss{text};
    std::string line;
    std::string stage;
    while (std::getline(ss, line)) {
        std::string cleaned = trim(line);
        if (cleaned.rfind("stage:", 0) == 0) {
            stage = trim(cleaned.substr(6));
        }
    }

    return stage;
}

std::string summarizeRangeFromLog(const std::string& text) {
    std::stringstream ss{text};
    std::string line;
    while (std::getline(ss, line)) {
        auto cleaned = trim(line);
        auto marker = cleaned.find("range min:");
        if (marker != std::string::npos) {
            return cleaned.substr(marker);
        }
    }

    return "No luminance summary yet.";
}

std::string collectWarnings(const std::string& text) {
    std::stringstream ss{text};
    std::string line;
    std::vector<std::string> warnings;
    while (std::getline(ss, line)) {
        auto cleaned = trim(line);
        auto lowered = tev::toLower(cleaned);
        if (lowered.find("warning") != std::string::npos || lowered.find("fatal") != std::string::npos ||
            lowered.find("error") != std::string::npos) {
            warnings.push_back(cleaned);
        }
    }

    if (warnings.empty()) {
        return {};
    }

    std::ostringstream oss;
    for (size_t i = 0; i < warnings.size(); ++i) {
        if (i) {
            oss << '\n';
        }
        oss << warnings[i];
    }

    return oss.str();
}

std::string sanitizeStem(const fs::path& path) {
    std::string stem = path.stem().string();
    if (stem.empty()) {
        stem = "item";
    }

    for (char& ch : stem) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_')) {
            ch = '_';
        }
    }

    return stem;
}

std::optional<tev::Box2i> parseCropString(const std::string& text) {
    std::stringstream ss{text};
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    if (!(ss >> x >> y >> w >> h)) {
        return std::nullopt;
    }

    if (w <= 0 || h <= 0) {
        return std::nullopt;
    }

    return tev::Box2i{{x, y}, {x + w, y + h}};
}

std::string cropToString(const std::optional<tev::Box2i>& crop) {
    if (!crop.has_value()) {
        return {};
    }

    const auto size = crop->size();
    std::ostringstream oss;
    oss << crop->min.x() << " " << crop->min.y() << " " << size.x() << " " << size.y();
    return oss.str();
}

std::optional<std::array<float, 9>> parseRecommendedMatrix(const std::string& text) {
    auto lines = splitLines(text);
    for (const auto& line : lines) {
        if (line.rfind("Color Matrix SLSQP Minimization", 0) == 0 || line.rfind("Color Matrix Minimization", 0) == 0 ||
            line.rfind("recommended:", 0) == 0) {
            std::string numbers = line.substr(line.find(':') + 1);
            std::stringstream ss{numbers};
            std::array<float, 9> matrix{};
            for (float& value : matrix) {
                if (!(ss >> value)) {
                    return std::nullopt;
                }
            }
            return matrix;
        }
    }
    return std::nullopt;
}

class NativeApp;
NativeApp* gApp = nullptr;
void queueUiTask(const std::function<void()>& fun);
void queueViewerRedraw();

} // namespace

namespace tev {

void scheduleToMainThread(const std::function<void()>& fun) {
    queueUiTask(fun);
}

void redrawWindow() {
    queueViewerRedraw();
}

} // namespace tev

namespace {

class PlainTextArea final : public TextArea {
public:
    explicit PlainTextArea(Widget* parent) : TextArea(parent) {}

    void append(std::string_view text) {
        plainText_ += text;
        TextArea::append(text);
    }

    void append_line(std::string_view text) {
        append(std::string(text) + "\n");
    }

    void clear() {
        plainText_.clear();
        TextArea::clear();
    }

    void setText(std::string_view text) {
        plainText_ = std::string{text};
        TextArea::clear();
        if (!plainText_.empty()) {
            TextArea::append(plainText_);
        }
    }

    const std::string& text() const {
        return plainText_;
    }

private:
    std::string plainText_;
};

class NativeApp final : public Screen {
public:
    NativeApp() :
        Screen{{1560, 980}, "hdrspace", true, false, false, true, true, true},
        paths_{resolvePaths()},
        imagesLoader_{std::make_shared<tev::BackgroundImagesLoader>()} {
        gApp = this;
        theme()->m_standard_font_size = 12;
        theme()->m_button_font_size = 14;
        theme()->m_text_box_font_size = 13;
        theme()->m_window_header_height = 22;
        theme()->m_window_corner_radius = 3;
        theme()->m_tab_button_horizontal_padding = 6;
        theme()->m_tab_button_vertical_padding = 1;
        theme()->m_window_fill_focused = Color{28, 230};
        theme()->m_window_fill_unfocused = Color{24, 225};
        set_background(Color{24, 24, 28, 255});

        tev::toggleConsole();
        buildUi();
        refreshProfiles();
        loadSettingsIntoUi();
        updateViewerControls();
        updateCropUi();
    }

    ~NativeApp() override { gApp = nullptr; }

    void scheduleToUiThread(const std::function<void()>& fun) {
        std::lock_guard<std::mutex> lock{uiTaskMutex_};
        uiTasks_.push_back(fun);
        redraw();
    }

    void requestRedrawFromWorker() {
        redraw();
    }

    bool resize_event(const Vector2i& size) override {
        bool result = Screen::resize_event(size);
        if (sidebar_ && sidebarScroll_ && viewerToolbar_ && canvas_ && statusBar_) {
            layoutChrome();
        }
        return result;
    }

    bool mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) override {
        bool handled = Screen::mouse_button_event(p, button, down, modifiers);

        if (button == GLFW_MOUSE_BUTTON_LEFT && !down) {
            viewerDragging_ = false;
            if (cropDragging_) {
                cropDragging_ = false;
                updateCropUi();
            }
        }

        if (!currentImage_) {
            return handled;
        }

        const Vector2i canvasPos = p - canvas_->absolute_position();

        if (button == GLFW_MOUSE_BUTTON_LEFT && cropMode_ && pointInCanvas(p)) {
            if (down) {
                cropDragging_ = true;
                cropStart_ = canvas_->getDisplayWindowCoords(currentImage_.get(), canvasPos);
                canvas_->setCrop(tev::Box2i{cropStart_, cropStart_});
            } else if (cropDragging_) {
                cropDragging_ = false;
                updateCropUi();
            }
            redraw();
            return true;
        }

        if (button == GLFW_MOUSE_BUTTON_LEFT && !cropMode_ && pointInCanvas(p)) {
            viewerDragging_ = down;
            return true;
        }

        return handled;
    }

    bool mouse_motion_event_f(const Vector2f& p, const Vector2f& rel, int button, int modifiers) override {
        bool handled = Screen::mouse_motion_event_f(p, rel, button, modifiers);
        Vector2i ip = p;
        Vector2i canvasPos = ip - canvas_->absolute_position();

        if (currentImage_ && pointInCanvas(ip)) {
            updatePixelReadout(canvasPos);
        } else {
            pixelInfoLabel_->set_caption("pixel: -");
        }

        if (cropDragging_ && currentImage_) {
            Vector2i current = canvas_->getDisplayWindowCoords(currentImage_.get(), canvasPos);
            tev::Box2i crop{{std::min(cropStart_.x(), current.x()), std::min(cropStart_.y(), current.y())},
                            {std::max(cropStart_.x(), current.x()) + 1, std::max(cropStart_.y(), current.y()) + 1}};
            canvas_->setCrop(crop);
            updateCropUi();
            redraw();
            return true;
        }

        if (viewerDragging_ && currentImage_ && pointInCanvas(ip) && (button & 1) != 0) {
            Vector2f relativeMovement = rel;
            auto* glfwWindow = screen()->glfw_window();
            if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) {
                relativeMovement /= 8.0f;
            } else if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_CONTROL) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_CONTROL)) {
                relativeMovement *= 8.0f;
            }
            canvas_->translate(relativeMovement);
            redraw();
            return true;
        }

        return handled;
    }

    bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (Screen::keyboard_event(key, scancode, action, modifiers)) {
            return true;
        }

        if (action != GLFW_PRESS && action != GLFW_REPEAT) {
            return false;
        }

        if (key == GLFW_KEY_F && currentImage_) {
            fitCurrentImage();
            return true;
        }

        if (key == GLFW_KEY_1 && currentImage_) {
            resetZoom();
            return true;
        }

        if (key == GLFW_KEY_C && action == GLFW_PRESS) {
            toggleCropMode();
            return true;
        }

        if (key == GLFW_KEY_E) {
            float step = (modifiers & GLFW_MOD_SHIFT) ? -0.1f : 0.1f;
            exposureSlider_->set_value(std::clamp(exposureSlider_->value() + step, -5.0f, 5.0f));
            applyExposureFromSlider();
            return true;
        }

        if (key == GLFW_KEY_G) {
            float delta = (modifiers & GLFW_MOD_SHIFT) ? -0.1f : 0.1f;
            gammaSlider_->set_value(std::clamp(gammaSlider_->value() + delta, 0.01f, 5.0f));
            applyGammaFromSlider();
            return true;
        }

        return false;
    }

    void draw_contents() override {
        Screen::draw_contents();
        flushUiTasks();
        processLoadedImages();
        pollMergeJob();
        pollWorkerTask(colorTask_, colorResultArea_);
        pollWorkerTask(shutterTask_, shutterResultArea_);
        pollWorkerTask(apertureTask_, apertureResultArea_);
        if (hasActiveBackgroundWork()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastBackgroundRedrawAt_ >= 50ms) {
                lastBackgroundRedrawAt_ = now;
                redraw();
            }
        }
    }

private:
    struct AppPaths {
        fs::path appRoot;
        fs::path settingsFile;
        fs::path cacheDir;
        fs::path mergehdr;
        fs::path tev;
        fs::path oiiotool;
        fs::path profilesDir;
    };

    struct MergeJobState {
        bool active = false;
        bool finished = false;
        pid_t pid = -1;
        int exitCode = -1;
        fs::path logPath;
        fs::path outputPath;
        std::string command;
        std::chrono::steady_clock::time_point startedAt = {};
    };

    struct WorkerTask {
        std::mutex mutex;
        bool active = false;
        bool finished = false;
        int exitCode = -1;
        std::string command;
        std::string output;
    };

    AppPaths paths_;
    std::shared_ptr<tev::BackgroundImagesLoader> imagesLoader_;
    std::vector<std::shared_ptr<tev::Image>> images_;
    std::shared_ptr<tev::Image> currentImage_;
    std::string currentGroup_;
    bool clearImagesOnNextLoad_ = false;

    std::mutex uiTaskMutex_;
    std::vector<std::function<void()>> uiTasks_;

    Widget* sidebar_ = nullptr;
    Widget* viewerToolbar_ = nullptr;
    Widget* statusBar_ = nullptr;
    tev::ImageCanvas* canvas_ = nullptr;
    VScrollPanel* sidebarScroll_ = nullptr;
    TabWidget* tabs_ = nullptr;

    Label* viewerSourceLabel_ = nullptr;
    Label* cropModeLabel_ = nullptr;
    Label* pixelInfoLabel_ = nullptr;
    Label* stageLabel_ = nullptr;
    Label* rangeLabel_ = nullptr;
    Label* issuesLabel_ = nullptr;
    ProgressBar* progressBar_ = nullptr;

    Slider* exposureSlider_ = nullptr;
    Slider* gammaSlider_ = nullptr;
    ComboBox* tonemapCombo_ = nullptr;
    ComboBox* groupCombo_ = nullptr;
    CheckBox* radiance179Box_ = nullptr;
    CheckBox* redMask_ = nullptr;
    CheckBox* greenMask_ = nullptr;
    CheckBox* blueMask_ = nullptr;
    CheckBox* alphaMask_ = nullptr;

    PlainTextArea* inputListArea_ = nullptr;
    TextBox* outputFolderBox_ = nullptr;
    TextBox* outputNameBox_ = nullptr;
    ComboBox* profileCombo_ = nullptr;
    ComboBox* colorspaceCombo_ = nullptr;
    ComboBox* outputFormatCombo_ = nullptr;
    ComboBox* demosaicCombo_ = nullptr;
    ComboBox* mergeWeightCombo_ = nullptr;
    CheckBox* bloomPreventBox_ = nullptr;
    CheckBox* fisheyeBox_ = nullptr;
    CheckBox* rawgridBox_ = nullptr;
    TextBox* blackBox_ = nullptr;
    TextBox* whiteBox_ = nullptr;
    TextBox* saturationBox_ = nullptr;
    TextBox* ndBox_ = nullptr;
    TextBox* scaleBox_ = nullptr;
    TextBox* rawMultipliersBox_ = nullptr;
    TextBox* xyzcamBox_ = nullptr;
    TextBox* cropBox_ = nullptr;
    TextBox* extraArgsBox_ = nullptr;
    Button* runMergeButton_ = nullptr;
    Button* copyCommandButton_ = nullptr;

    TextBox* refFileBox_ = nullptr;
    TextBox* testFileBox_ = nullptr;
    TextBox* refCellsBox_ = nullptr;
    TextBox* testCellsBox_ = nullptr;
    ComboBox* refColorCombo_ = nullptr;
    ComboBox* minimizerCombo_ = nullptr;
    TextBox* colorXyzcamBox_ = nullptr;
    PlainTextArea* colorResultArea_ = nullptr;
    Button* copyRecommendedMatrixButton_ = nullptr;

    PlainTextArea* shutterSequencesArea_ = nullptr;
    PlainTextArea* shutterCropsArea_ = nullptr;
    ComboBox* shutterChannelCombo_ = nullptr;
    PlainTextArea* shutterResultArea_ = nullptr;

    PlainTextArea* apertureSequencesArea_ = nullptr;
    TextBox* apertureCropBox_ = nullptr;
    TextBox* apertureShuttercBox_ = nullptr;
    PlainTextArea* apertureResultArea_ = nullptr;

    TextBox* mergehdrPathBox_ = nullptr;
    TextBox* tevPathBox_ = nullptr;
    TextBox* oiiotoolPathBox_ = nullptr;

    std::vector<std::string> profileNames_;
    std::vector<std::string> inputFiles_;
    fs::path workingDir_ = safeCurrentPath();
    std::string lastMergeCommand_;
    MergeJobState mergeJob_;
    WorkerTask colorTask_;
    WorkerTask shutterTask_;
    WorkerTask apertureTask_;

    bool cropMode_ = false;
    bool cropDragging_ = false;
    bool viewerDragging_ = false;
    Vector2i cropStart_ = {0, 0};
    bool lastOutputUsesRadiance179_ = false;
    std::chrono::steady_clock::time_point lastBackgroundRedrawAt_{};

    bool workerTaskIsActive(WorkerTask& task) {
        std::lock_guard<std::mutex> lock{task.mutex};
        return task.active;
    }

    bool hasActiveBackgroundWork() {
        return mergeJob_.active || imagesLoader_->hasPendingLoads() ||
            workerTaskIsActive(colorTask_) || workerTaskIsActive(shutterTask_) ||
            workerTaskIsActive(apertureTask_);
    }

    static fs::path executablePath() {
        std::vector<char> buffer(4096, '\0');
        uint32_t size = static_cast<uint32_t>(buffer.size());
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            buffer.resize(size + 1, '\0');
            if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
                throw std::runtime_error("Could not resolve executable path.");
            }
        }

        return fs::weakly_canonical(fs::path(buffer.data()));
    }

    static fs::path findOnPath(const std::string& name) {
        const char* pathEnv = std::getenv("PATH");
        if (!pathEnv) {
            return {};
        }

        std::stringstream ss{pathEnv};
        std::string entry;
        while (std::getline(ss, entry, ':')) {
            if (entry.empty()) {
                continue;
            }

            fs::path candidate = fs::path(entry) / name;
            if (fs::exists(candidate)) {
                return fs::weakly_canonical(candidate);
            }
        }

        return {};
    }

    static AppPaths resolvePaths() {
        AppPaths out;
        fs::path exe = executablePath();
        out.appRoot = fs::weakly_canonical(exe.parent_path().parent_path());
        out.cacheDir = out.appRoot / "build" / "cache";
        out.settingsFile = out.cacheDir / "settings.conf";
        out.profilesDir = out.appRoot.parent_path() / "mergehdr" / "profiles";
        out.mergehdr = out.appRoot.parent_path() / "mergehdr" / "bin" / "mergehdr";
        if (!fs::exists(out.mergehdr)) {
            out.mergehdr = findOnPath("mergehdr");
        }

        out.tev = findOnPath("tev");
        if (out.tev.empty()) {
            fs::path appTev{"/Applications/tev.app/Contents/MacOS/tev"};
            if (fs::exists(appTev)) {
                out.tev = appTev;
            }
        }

        out.oiiotool = findOnPath("oiiotool");
        if (out.oiiotool.empty()) {
            fs::path brew{"/opt/homebrew/bin/oiiotool"};
            if (fs::exists(brew)) {
                out.oiiotool = brew;
            }
        }

        auto settings = readKeyValueFile(out.settingsFile);
        if (!settings["mergehdrPath"].empty()) {
            out.mergehdr = settings["mergehdrPath"];
        }
        if (!settings["tevPath"].empty()) {
            out.tev = settings["tevPath"];
        }
        if (!settings["oiiotoolPath"].empty()) {
            out.oiiotool = settings["oiiotoolPath"];
        }

        fs::create_directories(out.cacheDir);
        return out;
    }

    void refreshProfiles() {
        profileNames_ = listProfileNames(paths_.profilesDir);
        std::vector<std::string> items = {"Auto (profile / LibRaw defaults)"};
        items.insert(items.end(), profileNames_.begin(), profileNames_.end());
        profileCombo_->set_items(items);
        profileCombo_->set_selected_index(0);
    }

    void loadSettingsIntoUi() {
        mergehdrPathBox_->set_value(paths_.mergehdr.string());
        tevPathBox_->set_value(paths_.tev.string());
        oiiotoolPathBox_->set_value(paths_.oiiotool.string());
    }

    void saveSettings() {
        std::map<std::string, std::string> values;
        values["mergehdrPath"] = normalizedTextBoxValue(mergehdrPathBox_->value());
        values["tevPath"] = normalizedTextBoxValue(tevPathBox_->value());
        values["oiiotoolPath"] = normalizedTextBoxValue(oiiotoolPathBox_->value());
        writeKeyValueFile(paths_.settingsFile, values);
        paths_ = resolvePaths();
        loadSettingsIntoUi();
        showMessage("Settings saved.");
    }

    void buildUi() {
        sidebar_ = new Widget(this);
        sidebar_->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 10, 12});

        auto* title = new Label(sidebar_, "mergehdr", "sans-bold", 18);
        title->set_color(Color(240, 255));

        sidebarScroll_ = new VScrollPanel(sidebar_);
        auto* scrollContent = new Widget(sidebarScroll_);
        scrollContent->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

        tabs_ = new TabWidget(scrollContent);
        tabs_->set_callback([this](int) {
            layoutChrome();
            redraw();
        });

        auto* mergeTab = new Widget(tabs_);
        mergeTab->set_layout(new GroupLayout{12, 6, 14, 6});
        tabs_->append_tab("Merge", mergeTab);
        buildMergeTab(mergeTab);

        auto* colorTab = new Widget(tabs_);
        colorTab->set_layout(new GroupLayout{12, 6, 14, 6});
        tabs_->append_tab("Color Calibration", colorTab);
        buildColorTab(colorTab);

        auto* shutterTab = new Widget(tabs_);
        shutterTab->set_layout(new GroupLayout{12, 6, 14, 6});
        tabs_->append_tab("Shutter Calibration", shutterTab);
        buildShutterTab(shutterTab);

        auto* apertureTab = new Widget(tabs_);
        apertureTab->set_layout(new GroupLayout{12, 6, 14, 6});
        tabs_->append_tab("Aperture Calibration", apertureTab);
        buildApertureTab(apertureTab);

        auto* settingsTab = new Widget(tabs_);
        settingsTab->set_layout(new GroupLayout{12, 6, 14, 6});
        tabs_->append_tab("Settings", settingsTab);
        buildSettingsTab(settingsTab);

        viewerToolbar_ = new Widget(this);
        viewerToolbar_->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 8, 8});

        new Label(viewerToolbar_, "Viewer", "sans-bold", 15);
        viewerSourceLabel_ = new Label(viewerToolbar_, "No image loaded", "sans", 13);
        auto* showOutputButton = new Button(viewerToolbar_, "Result");
        showOutputButton->set_font_size(14);
        showOutputButton->set_callback([this] {
            if (!mergeJob_.outputPath.empty() && fs::exists(mergeJob_.outputPath)) {
                loadImageIntoViewer(mergeJob_.outputPath);
            }
        });
        auto* fitButton = new Button(viewerToolbar_, "Fit");
        fitButton->set_font_size(14);
        fitButton->set_callback([this] { fitCurrentImage(); });
        auto* oneToOneButton = new Button(viewerToolbar_, "100%");
        oneToOneButton->set_font_size(14);
        oneToOneButton->set_callback([this] { resetZoom(); });
        auto* cropButton = new Button(viewerToolbar_, "Crop");
        cropButton->set_font_size(14);
        cropButton->set_flags(Button::ToggleButton);
        cropButton->set_change_callback([this](bool value) {
            cropMode_ = value;
            updateCropUi();
        });
        cropModeLabel_ = new Label(viewerToolbar_, "Crop off", "sans", 13);

        new Label(viewerToolbar_, "Tone", "sans", 13);
        tonemapCombo_ = new ComboBox(viewerToolbar_, {"sRGB", "Gamma", "False color", "Positive/Negative"});
        tonemapCombo_->set_selected_index(0);
        tonemapCombo_->set_callback([this](int index) {
            switch (index) {
            case 0: canvas_->setTonemap(tev::ETonemap::SRGB); break;
            case 1: canvas_->setTonemap(tev::ETonemap::Gamma); break;
            case 2: canvas_->setTonemap(tev::ETonemap::FalseColor); break;
            default: canvas_->setTonemap(tev::ETonemap::PositiveNegative); break;
            }
            redraw();
        });

        new Label(viewerToolbar_, "Exp", "sans", 13);
        exposureSlider_ = new Slider(viewerToolbar_);
        exposureSlider_->set_fixed_width(96);
        exposureSlider_->set_range({-5.0f, 5.0f});
        exposureSlider_->set_value(0.0f);
        exposureSlider_->set_callback([this](float) { applyExposureFromSlider(); });

        new Label(viewerToolbar_, "Gamma", "sans", 13);
        gammaSlider_ = new Slider(viewerToolbar_);
        gammaSlider_->set_fixed_width(92);
        gammaSlider_->set_range({0.01f, 5.0f});
        gammaSlider_->set_value(2.2f);
        gammaSlider_->set_callback([this](float) { applyGammaFromSlider(); });

        radiance179Box_ = new CheckBox(viewerToolbar_, "x179");
        radiance179Box_->set_checked(false);
        radiance179Box_->set_callback([this](bool) { applyExposureFromSlider(); });

        redMask_ = new CheckBox(viewerToolbar_, "R");
        redMask_->set_checked(true);
        redMask_->set_callback([this](bool) { applyChannelMask(); });
        greenMask_ = new CheckBox(viewerToolbar_, "G");
        greenMask_->set_checked(true);
        greenMask_->set_callback([this](bool) { applyChannelMask(); });
        blueMask_ = new CheckBox(viewerToolbar_, "B");
        blueMask_->set_checked(true);
        blueMask_->set_callback([this](bool) { applyChannelMask(); });
        alphaMask_ = new CheckBox(viewerToolbar_, "A");
        alphaMask_->set_checked(true);
        alphaMask_->set_callback([this](bool) { applyChannelMask(); });

        groupCombo_ = new ComboBox(viewerToolbar_, {"default"});
        groupCombo_->set_fixed_width(180);
        groupCombo_->set_callback([this](int index) { selectCurrentGroup(index); });
        auto* ungroupButton = new Button(viewerToolbar_, "Ungroup");
        ungroupButton->set_font_size(14);
        ungroupButton->set_callback([this] {
            if (currentImage_ && !currentGroup_.empty()) {
                currentImage_->ungroup(currentGroup_);
                refreshGroupChoices();
            }
        });

        canvas_ = new tev::ImageCanvas(this);
        canvas_->setPixelRatio(pixel_ratio());
        canvas_->setBackgroundColor(Color{18, 255});
        canvas_->setTonemap(tev::ETonemap::SRGB);

        statusBar_ = new Widget(this);
        statusBar_->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 10, 8});
        stageLabel_ = new Label(statusBar_, "Ready", "sans-bold", 13);
        progressBar_ = new ProgressBar(statusBar_);
        progressBar_->set_fixed_width(180);
        progressBar_->set_value(0.0f);
        rangeLabel_ = new Label(statusBar_, "No luminance summary yet.", "sans", 12);
        issuesLabel_ = new Label(statusBar_, "", "sans", 12);
        issuesLabel_->set_color(Color(255, 110, 110, 255));
        pixelInfoLabel_ = new Label(statusBar_, "pixel: -", "mono", 12);

        applyExposureFromSlider();
        applyGammaFromSlider();
    }

    TextBox* smallTextBox(Widget* parent, std::string_view placeholder = {}) {
        auto* box = new TextBox(parent, "");
        box->set_font_size(14);
        box->set_editable(true);
        box->set_value("");
        box->set_default_value("");
        if (!placeholder.empty()) {
            box->set_placeholder(placeholder);
        }
        return box;
    }

    void buildMergeTab(Widget* parent) {
        auto* inputRow = new Widget(parent);
        inputRow->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 8, 0});
        auto* chooseInputs = new Button(inputRow, "Choose input files");
        chooseInputs->set_font_size(14);
        chooseInputs->set_callback([this] { chooseInputFiles(); });
        auto* clearInputs = new Button(inputRow, "Clear");
        clearInputs->set_font_size(14);
        clearInputs->set_callback([this] {
            inputFiles_.clear();
            inputListArea_->clear();
            viewerSourceLabel_->set_caption("No image loaded");
        });

        inputListArea_ = new PlainTextArea(parent);
        inputListArea_->set_fixed_height(90);
        inputListArea_->set_padding(8);
        inputListArea_->set_selectable(true);
        inputListArea_->set_background_color(Color{16, 180});

        new Label(parent, "Output", "sans-bold", 14);
        auto* outputRow = new Widget(parent);
        outputRow->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 8, 0});
        outputFolderBox_ = smallTextBox(outputRow, "Output folder");
        auto* chooseOutputFolderButton = new Button(outputRow, "Folder");
        chooseOutputFolderButton->set_font_size(14);
        chooseOutputFolderButton->set_callback([this] { chooseOutputFolder(); });

        auto* outputMetaRow = new Widget(parent);
        outputMetaRow->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 8, 0});
        outputNameBox_ = smallTextBox(outputMetaRow, "default");
        outputFormatCombo_ = new ComboBox(outputMetaRow, {"Radiance HDR (.hdr)", "OpenEXR (.exr)"});
        outputFormatCombo_->set_selected_index(0);

        new Label(parent, "Profile", "sans-bold", 14);
        profileCombo_ = new ComboBox(parent, {"Auto (profile / LibRaw defaults)"});
        profileCombo_->set_callback([this](int index) { applyProfileFromSelection(index); });

        auto* csHeader = new Widget(parent);
        csHeader->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 8, 0});
        new Label(csHeader, "Color space", "sans-bold", 14);
        auto* infoButton = new Button(csHeader, "Info");
        infoButton->set_font_size(13);
        infoButton->set_callback([this] {
            new MessageDialog(
                this,
                MessageDialog::Type::Information,
                "Color spaces",
                "rad keeps Radiance primaries.\n"
                "srgb targets standard display primaries.\n"
                "xyz writes CIE XYZ.\n"
                "raw keeps demosaiced sensor RGB.\n\n"
                "Radiance output uses the standard 179 scale by default."
            );
        });

        colorspaceCombo_ = new ComboBox(parent, {"rad", "srgb", "xyz", "raw"});
        colorspaceCombo_->set_selected_index(0);

        auto* qualityRow = new Widget(parent);
        qualityRow->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 8, 6});
        new Label(qualityRow, "Interpolation", "sans", 13);
        demosaicCombo_ = new ComboBox(qualityRow, {"AHD (default)", "DHT (slow)"});
        demosaicCombo_->set_selected_index(0);
        new Label(qualityRow, "Merge weight", "sans", 13);
        mergeWeightCombo_ = new ComboBox(qualityRow, {"hdrmerge (default)", "linearhdr (slow)"});
        mergeWeightCombo_->set_selected_index(0);

        auto* modeRow = new Widget(parent);
        modeRow->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 8, 4});
        bloomPreventBox_ = new CheckBox(modeRow, "Bloom prevent");
        bloomPreventBox_->set_callback([this](bool value) {
            if (value) {
                demosaicCombo_->set_selected_index(1);
                mergeWeightCombo_->set_selected_index(1);
            } else {
                demosaicCombo_->set_selected_index(0);
                mergeWeightCombo_->set_selected_index(0);
            }
        });
        fisheyeBox_ = new CheckBox(modeRow, "Fisheye projection correction");
        fisheyeBox_->set_checked(false);
        fisheyeBox_->set_callback([this](bool value) {
            if (value && cropBox_->value().empty()) {
                fisheyeBox_->set_checked(false);
                showMessage("Fisheye correction needs a crop first.");
            }
        });
        rawgridBox_ = new CheckBox(modeRow, "Raw grid output");

        auto* cropHeader = new Widget(parent);
        cropHeader->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 8, 0});
        new Label(cropHeader, "Crop", "sans-bold", 14);
        auto* setCropButton = new Button(cropHeader, "Set in viewer");
        setCropButton->set_font_size(13);
        setCropButton->set_callback([this] { toggleCropMode(); });
        auto* clearCropButton = new Button(cropHeader, "Clear");
        clearCropButton->set_font_size(13);
        clearCropButton->set_callback([this] {
            canvas_->setCrop(std::nullopt);
            cropMode_ = false;
            updateCropUi();
        });
        cropBox_ = smallTextBox(parent, "x y w h");
        cropBox_->set_callback([this](const std::string& value) {
            auto crop = parseCropString(value);
            canvas_->setCrop(crop);
            updateCropUi();
            return true;
        });

        auto* numericGrid = new Widget(parent);
        numericGrid->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 8, 4});
        new Label(numericGrid, "Black", "sans", 13);
        blackBox_ = smallTextBox(numericGrid, "default");
        new Label(numericGrid, "White", "sans", 13);
        whiteBox_ = smallTextBox(numericGrid, "default");
        new Label(numericGrid, "Saturation", "sans", 13);
        saturationBox_ = smallTextBox(numericGrid, "default");
        new Label(numericGrid, "ND", "sans", 13);
        ndBox_ = smallTextBox(numericGrid, "default");
        new Label(numericGrid, "Scale", "sans", 13);
        scaleBox_ = smallTextBox(numericGrid, "default");
        new Label(numericGrid, "Premultipliers", "sans", 13);
        rawMultipliersBox_ = smallTextBox(numericGrid, "default");
        new Label(numericGrid, "XYZCAM", "sans", 13);
        xyzcamBox_ = smallTextBox(numericGrid, "default (profile / LibRaw)");

        new Label(parent, "Extra run args", "sans-bold", 14);
        extraArgsBox_ = smallTextBox(parent, "advanced options");

        auto* actionRow = new Widget(parent);
        actionRow->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 8, 0});
        runMergeButton_ = new Button(actionRow, "Run merge");
        runMergeButton_->set_font_size(15);
        runMergeButton_->set_callback([this] { startMerge(); });
        copyCommandButton_ = new Button(actionRow, "Copy CLI");
        copyCommandButton_->set_font_size(14);
        copyCommandButton_->set_callback([this] {
            if (!lastMergeCommand_.empty()) {
                glfwSetClipboardString(glfw_window(), lastMergeCommand_.c_str());
                showMessage("Copied merge command.");
            }
        });
    }

    void buildColorTab(Widget* parent) {
        auto* refRow = new Widget(parent);
        refRow->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 8, 0});
        auto* chooseRef = new Button(refRow, "Reference HDR");
        chooseRef->set_font_size(14);
        chooseRef->set_callback([this] {
            auto paths = file_dialog(this, FileDialogType::Open);
            if (!paths.empty()) {
                refFileBox_->set_value(paths.front());
                loadImageIntoViewer(paths.front());
            }
        });
        auto* viewRef = new Button(refRow, "View");
        viewRef->set_font_size(14);
        viewRef->set_callback([this] {
            if (!trim(refFileBox_->value()).empty()) {
                loadImageIntoViewer(refFileBox_->value());
            }
        });
        refFileBox_ = smallTextBox(parent, "Reference HDR");

        auto* testRow = new Widget(parent);
        testRow->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 8, 0});
        auto* chooseTest = new Button(testRow, "Camera HDR");
        chooseTest->set_font_size(14);
        chooseTest->set_callback([this] {
            auto paths = file_dialog(this, FileDialogType::Open);
            if (!paths.empty()) {
                testFileBox_->set_value(paths.front());
                loadImageIntoViewer(paths.front());
            }
        });
        auto* viewTest = new Button(testRow, "View");
        viewTest->set_font_size(14);
        viewTest->set_callback([this] {
            if (!trim(testFileBox_->value()).empty()) {
                loadImageIntoViewer(testFileBox_->value());
            }
        });
        testFileBox_ = smallTextBox(parent, "Test HDR");

        auto* cellsRow = new Widget(parent);
        cellsRow->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 8, 0});
        auto* refCellsAuto = new Button(cellsRow, "Auto cells for reference");
        refCellsAuto->set_font_size(14);
        refCellsAuto->set_callback([this] { runChartCells(true); });
        auto* testCellsAuto = new Button(cellsRow, "Auto cells for camera");
        testCellsAuto->set_font_size(14);
        testCellsAuto->set_callback([this] { runChartCells(false); });
        refCellsBox_ = smallTextBox(parent, "ref_cells.txt");
        testCellsBox_ = smallTextBox(parent, "test_cells.txt");

        auto* options = new Widget(parent);
        options->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 8, 4});
        new Label(options, "Reference space", "sans", 13);
        refColorCombo_ = new ComboBox(options, {"rad", "srgb", "xyz", "raw"});
        refColorCombo_->set_selected_index(0);
        new Label(options, "Minimizer", "sans", 13);
        minimizerCombo_ = new ComboBox(options, {"luv", "lab"});
        minimizerCombo_->set_selected_index(0);
        new Label(options, "XYZCAM override", "sans", 13);
        colorXyzcamBox_ = smallTextBox(options, "optional");

        auto* actionRow = new Widget(parent);
        actionRow->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 8, 0});
        auto* runButton = new Button(actionRow, "Run color calibration");
        runButton->set_font_size(15);
        runButton->set_callback([this] { startColorCalibration(); });
        copyRecommendedMatrixButton_ = new Button(actionRow, "Copy recommended xyzcam");
        copyRecommendedMatrixButton_->set_font_size(14);
        copyRecommendedMatrixButton_->set_callback([this] { copyRecommendedMatrix(); });

        colorResultArea_ = new PlainTextArea(parent);
        colorResultArea_->set_fixed_height(240);
        colorResultArea_->set_padding(8);
        colorResultArea_->set_selectable(true);
        colorResultArea_->set_background_color(Color{14, 180});
    }

    void buildShutterTab(Widget* parent) {
        auto* actions = new Widget(parent);
        actions->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 8, 0});
        auto* useMergeInputs = new Button(actions, "Use merge inputs");
        useMergeInputs->set_font_size(14);
        useMergeInputs->set_callback([this] {
            shutterSequencesArea_->clear();
            for (const auto& path : inputFiles_) {
                shutterSequencesArea_->append_line(path);
            }
        });
        auto* useCurrentCrop = new Button(actions, "Use current crop");
        useCurrentCrop->set_font_size(14);
        useCurrentCrop->set_callback([this] {
            shutterCropsArea_->clear();
            if (!cropBox_->value().empty()) {
                shutterCropsArea_->append_line(cropBox_->value());
            }
        });

        new Label(parent, "Sequences", "sans-bold", 14);
        shutterSequencesArea_ = new PlainTextArea(parent);
        shutterSequencesArea_->set_fixed_height(110);
        shutterSequencesArea_->set_padding(8);
        shutterSequencesArea_->set_selectable(true);
        shutterSequencesArea_->set_background_color(Color{14, 180});

        new Label(parent, "Crop list", "sans-bold", 14);
        shutterCropsArea_ = new PlainTextArea(parent);
        shutterCropsArea_->set_fixed_height(80);
        shutterCropsArea_->set_padding(8);
        shutterCropsArea_->set_selectable(true);
        shutterCropsArea_->set_background_color(Color{14, 180});

        auto* row = new Widget(parent);
        row->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 8, 4});
        new Label(row, "Channel", "sans", 13);
        shutterChannelCombo_ = new ComboBox(row, {"g", "r", "b"});
        shutterChannelCombo_->set_selected_index(0);

        auto* run = new Button(parent, "Run shutter calibration");
        run->set_font_size(15);
        run->set_callback([this] { startShutterCalibration(); });

        shutterResultArea_ = new PlainTextArea(parent);
        shutterResultArea_->set_fixed_height(220);
        shutterResultArea_->set_padding(8);
        shutterResultArea_->set_selectable(true);
        shutterResultArea_->set_background_color(Color{14, 180});
    }

    void buildApertureTab(Widget* parent) {
        auto* actions = new Widget(parent);
        actions->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 8, 0});
        auto* useCrop = new Button(actions, "Use current crop");
        useCrop->set_font_size(14);
        useCrop->set_callback([this] { apertureCropBox_->set_value(cropBox_->value()); });

        apertureCropBox_ = smallTextBox(parent, "x y w h");
        apertureShuttercBox_ = smallTextBox(parent, "shutterc");

        new Label(parent, "Sequences", "sans-bold", 14);
        apertureSequencesArea_ = new PlainTextArea(parent);
        apertureSequencesArea_->set_fixed_height(130);
        apertureSequencesArea_->set_padding(8);
        apertureSequencesArea_->set_selectable(true);
        apertureSequencesArea_->set_background_color(Color{14, 180});

        auto* run = new Button(parent, "Run aperture calibration");
        run->set_font_size(15);
        run->set_callback([this] { startApertureCalibration(); });

        apertureResultArea_ = new PlainTextArea(parent);
        apertureResultArea_->set_fixed_height(220);
        apertureResultArea_->set_padding(8);
        apertureResultArea_->set_selectable(true);
        apertureResultArea_->set_background_color(Color{14, 180});
    }

    void buildSettingsTab(Widget* parent) {
        new Label(parent, "Paths", "sans-bold", 14);
        mergehdrPathBox_ = smallTextBox(parent, "mergehdr binary");
        tevPathBox_ = smallTextBox(parent, "external tev (optional)");
        oiiotoolPathBox_ = smallTextBox(parent, "oiiotool");

        auto* save = new Button(parent, "Save settings");
        save->set_font_size(15);
        save->set_callback([this] { saveSettings(); });
    }

    void layoutChrome() {
        if (!m_nvg_context || !glfw_window() || !sidebar_ || !sidebarScroll_ || !viewerToolbar_ || !canvas_ || !statusBar_) {
            return;
        }

        const int sidebarWidth = 258;
        const int toolbarHeight = 40;
        const int statusHeight = 34;
        const int margin = 10;

        sidebar_->set_position({0, 0});
        sidebar_->set_fixed_size({sidebarWidth, m_size.y()});
        sidebarScroll_->set_fixed_size({sidebarWidth - 20, m_size.y() - 60});

        viewerToolbar_->set_position({sidebarWidth + margin, margin});
        viewerToolbar_->set_fixed_size({m_size.x() - sidebarWidth - margin * 2, toolbarHeight});

        const Vector2i canvasPos{sidebarWidth + margin, toolbarHeight + margin * 2};
        const Vector2i canvasSize{
            m_size.x() - sidebarWidth - margin * 2,
            m_size.y() - toolbarHeight - statusHeight - margin * 3,
        };
        canvas_->set_position(canvasPos);
        canvas_->set_size(canvasSize);

        statusBar_->set_position({sidebarWidth + margin, m_size.y() - statusHeight - margin});
        statusBar_->set_fixed_size({m_size.x() - sidebarWidth - margin * 2, statusHeight});

        perform_layout(m_nvg_context);
    }

    void flushUiTasks() {
        std::vector<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock{uiTaskMutex_};
            tasks.swap(uiTasks_);
        }

        for (auto& task : tasks) {
            task();
        }
    }

    void processLoadedImages() {
        bool loaded = false;
        while (auto addition = imagesLoader_->tryPop()) {
            if (clearImagesOnNextLoad_) {
                images_.clear();
                clearImagesOnNextLoad_ = false;
            }

            for (auto& image : addition->images) {
                images_.push_back(image);
                if (addition->shallSelect || !currentImage_) {
                    selectImage(image);
                }
                loaded = true;
            }
        }

        if (loaded) {
            redraw();
        }
    }

    void selectImage(const std::shared_ptr<tev::Image>& image) {
        currentImage_ = image;
        canvas_->setImage(image);
        canvas_->setWhiteLevelOverride(std::nullopt);
        if (!image->channelGroups().empty()) {
            currentGroup_ = image->channelGroups().front().name;
        } else {
            currentGroup_.clear();
        }
        if (radiance179Box_) {
            bool isMergeOutput = false;
            if (!mergeJob_.outputPath.empty()) {
                std::error_code ec;
                const auto lhs = fs::weakly_canonical(image->path(), ec);
                const auto rhs = fs::weakly_canonical(mergeJob_.outputPath, ec);
                isMergeOutput = !ec && lhs == rhs;
            }
            radiance179Box_->set_checked(isMergeOutput && lastOutputUsesRadiance179_);
        }
        refreshGroupChoices();
        viewerSourceLabel_->set_caption(image->path().filename().string());
        fitCurrentImage();
        applyExposureFromSlider();
    }

    void refreshGroupChoices() {
        std::vector<std::string> names;
        if (currentImage_) {
            for (const auto& group : currentImage_->channelGroups()) {
                names.push_back(group.name);
            }
        }
        if (names.empty()) {
            names = {"default"};
            currentGroup_.clear();
        }

        groupCombo_->set_items(names);
        int selected = 0;
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == currentGroup_) {
                selected = static_cast<int>(i);
                break;
            }
        }
        groupCombo_->set_selected_index(selected);
        if (currentImage_ && !currentGroup_.empty()) {
            canvas_->setRequestedChannelGroup(currentGroup_);
        }
    }

    void selectCurrentGroup(int index) {
        if (!currentImage_ || index < 0 || index >= static_cast<int>(currentImage_->channelGroups().size())) {
            return;
        }

        currentGroup_ = currentImage_->channelGroups()[static_cast<size_t>(index)].name;
        canvas_->setRequestedChannelGroup(currentGroup_);
        redraw();
    }

    void applyExposureFromSlider() {
        const float value = exposureSlider_->value();
        const float radianceBoost = (radiance179Box_ && radiance179Box_->checked()) ? std::log2(179.0f) : 0.0f;
        canvas_->setExposure(value + radianceBoost);
        redraw();
    }

    void applyGammaFromSlider() {
        const float gamma = gammaSlider_->value();
        canvas_->setGamma(gamma);
        redraw();
    }

    void applyChannelMask() {
        tev::EChannelMask mask = tev::EChannelMask{};
        if (redMask_->checked()) {
            mask |= tev::EChannelMask::Red;
        }
        if (greenMask_->checked()) {
            mask |= tev::EChannelMask::Green;
        }
        if (blueMask_->checked()) {
            mask |= tev::EChannelMask::Blue;
        }
        if (alphaMask_->checked()) {
            mask |= tev::EChannelMask::Alpha;
        }
        canvas_->setChannelMask(mask);
        redraw();
    }

    void fitCurrentImage() {
        if (currentImage_) {
            canvas_->fitImageToScreen(*currentImage_);
            redraw();
        }
    }

    void resetZoom() {
        canvas_->resetTransform();
        redraw();
    }

    bool pointInCanvas(const Vector2i& p) const {
        const Vector2i pos = canvas_->absolute_position();
        const Vector2i size = canvas_->size();
        return p.x() >= pos.x() && p.y() >= pos.y() && p.x() < pos.x() + size.x() && p.y() < pos.y() + size.y();
    }

    void updatePixelReadout(const Vector2i& canvasPos) {
        if (!currentImage_) {
            pixelInfoLabel_->set_caption("pixel: -");
            return;
        }

        std::vector<std::string_view> channels;
        if (!currentGroup_.empty()) {
            auto group = currentImage_->channelsInGroup(currentGroup_);
            channels.assign(group.begin(), group.end());
        }
        if (channels.empty()) {
            pixelInfoLabel_->set_caption("pixel: -");
            return;
        }

        std::vector<float> values = canvas_->getValuesAtNanoPos(canvasPos, channels);
        Vector2i coord = canvas_->getDisplayWindowCoords(currentImage_.get(), canvasPos);
        std::ostringstream oss;
        oss << coord.x() << "," << coord.y() << "  ";
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) {
                oss << "  ";
            }
            oss << channels[i] << ":" << std::fixed << std::setprecision(4) << values[i];
        }
        pixelInfoLabel_->set_caption(oss.str());
    }

    void toggleCropMode() {
        cropMode_ = !cropMode_;
        updateCropUi();
    }

    void updateCropUi() {
        cropBox_->set_value(cropToString(canvas_->crop()));
        cropModeLabel_->set_caption(cropMode_ ? "Crop on" : "Crop off");
        bool hasCrop = !cropBox_->value().empty();
        fisheyeBox_->set_enabled(hasCrop);
        if (!hasCrop) {
            fisheyeBox_->set_checked(false);
        }
        redraw();
    }

    void loadImageIntoViewer(const fs::path& path) {
        if (!fs::exists(path)) {
            showMessage("Preview file does not exist.");
            return;
        }
        currentImage_.reset();
        currentGroup_.clear();
        canvas_->setImage(nullptr);
        refreshGroupChoices();
        clearImagesOnNextLoad_ = true;
        viewerSourceLabel_->set_caption("Loading " + path.filename().string() + "...");
        imagesLoader_->enqueue(path, "", true);
        lastBackgroundRedrawAt_ = std::chrono::steady_clock::now() - 50ms;
        redraw();
    }

    void loadInputPreview() {
        showMessage("RAW source preview is disabled here. Run merge to inspect the HDR result.");
    }

    void chooseInputFiles() {
        auto result = file_dialog(this, FileDialogType::OpenMultiple, {}, workingDir_.string());
        if (result.empty()) {
            return;
        }

        inputFiles_.assign(result.begin(), result.end());
        workingDir_ = fs::path(result.front()).parent_path();
        if (outputFolderBox_->value().empty()) {
            outputFolderBox_->set_value(workingDir_.string());
        }
        if (outputNameBox_->value().empty()) {
            outputNameBox_->set_value(fs::path(result.front()).stem().string() + "_merge.hdr");
        }

        inputListArea_->clear();
        for (const auto& file : inputFiles_) {
            inputListArea_->append_line(file);
        }

    }

    void chooseOutputFolder() {
        auto result = file_dialog(this, FileDialogType::PickFolder, {}, workingDir_.string());
        if (!result.empty()) {
            outputFolderBox_->set_value(result.front());
        }
    }

    void applyProfileFromSelection(int index) {
        if (index <= 0 || index > static_cast<int>(profileNames_.size())) {
            return;
        }

        fs::path profilePath = paths_.profilesDir / (profileNames_[static_cast<size_t>(index - 1)] + ".cfg");
        auto values = parseProfileFile(profilePath);
        if (values.contains("colorspace")) {
            auto items = colorspaceCombo_->items();
            auto it = std::find(items.begin(), items.end(), values["colorspace"]);
            if (it != items.end()) {
                colorspaceCombo_->set_selected_index(static_cast<int>(std::distance(items.begin(), it)));
            }
        }
        if (values.contains("black")) {
            blackBox_->set_value(values["black"]);
        }
        if (values.contains("white")) {
            whiteBox_->set_value(values["white"]);
        }
        if (values.contains("saturation")) {
            saturationBox_->set_value(values["saturation"]);
        }
        if (values.contains("nd")) {
            ndBox_->set_value(values["nd"]);
        }
        if (values.contains("xyzcam")) {
            xyzcamBox_->set_value(values["xyzcam"]);
        }
        if (values.contains("crop")) {
            cropBox_->set_value(values["crop"]);
            canvas_->setCrop(parseCropString(values["crop"]));
        }
        if (values.contains("fisheye")) {
            std::string lowered = tev::toLower(values["fisheye"]);
            fisheyeBox_->set_checked(lowered == "true" || lowered == "1" || lowered == "yes");
        }
        updateCropUi();
    }

    std::string selectedProfileName() const {
        int index = profileCombo_->selected_index();
        if (index <= 0 || index > static_cast<int>(profileNames_.size())) {
            return {};
        }
        return profileNames_[static_cast<size_t>(index - 1)];
    }

    std::string selectedColorspace() const {
        return colorspaceCombo_->items()[static_cast<size_t>(colorspaceCombo_->selected_index())];
    }

    std::string selectedDemosaic() const {
        return demosaicCombo_->selected_index() == 0 ? "ahd" : "dht";
    }

    std::string selectedMergeWeight() const {
        return mergeWeightCombo_->selected_index() == 0 ? "hdrmerge" : "linearhdr";
    }

    std::string selectedOutputFormat() const {
        return outputFormatCombo_->selected_index() == 0 ? "hdr" : "exr";
    }

    void startMerge() {
        if (mergeJob_.active) {
            showMessage("A merge is already running.");
            return;
        }

        if (paths_.mergehdr.empty() || !fs::exists(paths_.mergehdr)) {
            showMessage("Set a valid mergehdr path in Settings.");
            return;
        }

        if (inputFiles_.empty()) {
            showMessage("Choose RAW files first.");
            return;
        }

        if (fisheyeBox_->checked() && cropBox_->value().empty()) {
            showMessage("Fisheye correction needs a crop first.");
            return;
        }

        std::string outputName = normalizedTextBoxValue(outputNameBox_->value());
        if (outputName.empty()) {
            outputName = inputFiles_.empty() ? "mergehdr_output" : fs::path(inputFiles_.front()).stem().string() + "_merge";
        }

        fs::path outputDir = outputFolderBox_->value().empty() ? workingDir_ : fs::path(outputFolderBox_->value());
        if (!outputDir.is_absolute()) {
            outputDir = fs::absolute(workingDir_ / outputDir);
        }
        fs::create_directories(outputDir);

        std::string outputFormat = selectedOutputFormat();
        if (fs::path(outputName).extension().empty()) {
            outputName += "." + outputFormat;
        }
        fs::path finalOutput = outputDir / outputName;

        fs::path tempHdr = paths_.cacheDir / (sanitizeStem(finalOutput) + "_render.hdr");
        fs::path jobLog = paths_.cacheDir / (sanitizeStem(finalOutput) + "_merge.log");

        std::ostringstream cmd;
        cmd << "cd " << shellQuote(workingDir_.string()) << " && ";
        cmd << shellQuote(paths_.mergehdr.string()) << " ";
        const std::string profile = selectedProfileName();
        if (!profile.empty()) {
            cmd << "-profile " << shellQuote(profile) << " ";
        }
        cmd << "run ";
        cmd << "--colorspace " << shellQuote(selectedColorspace()) << " ";
        if (!normalizedTextBoxValue(blackBox_->value()).empty()) {
            cmd << "--black " << shellQuote(normalizedTextBoxValue(blackBox_->value())) << " ";
        }
        if (!normalizedTextBoxValue(whiteBox_->value()).empty()) {
            cmd << "--white " << shellQuote(normalizedTextBoxValue(whiteBox_->value())) << " ";
        }
        if (!normalizedTextBoxValue(saturationBox_->value()).empty()) {
            cmd << "--saturation " << shellQuote(normalizedTextBoxValue(saturationBox_->value())) << " ";
        }
        if (!normalizedTextBoxValue(ndBox_->value()).empty()) {
            cmd << "--nd " << shellQuote(normalizedTextBoxValue(ndBox_->value())) << " ";
        }
        if (!normalizedTextBoxValue(scaleBox_->value()).empty()) {
            cmd << "--scale " << shellQuote(normalizedTextBoxValue(scaleBox_->value())) << " ";
        }
        if (!normalizedTextBoxValue(rawMultipliersBox_->value()).empty()) {
            cmd << "--rawmultipliers " << shellQuote(normalizedTextBoxValue(rawMultipliersBox_->value())) << " ";
        }
        if (!normalizedTextBoxValue(xyzcamBox_->value()).empty()) {
            cmd << "--xyzcam " << shellQuote(normalizedTextBoxValue(xyzcamBox_->value())) << " ";
        }
        if (!normalizedTextBoxValue(cropBox_->value()).empty()) {
            cmd << "--crop " << normalizedTextBoxValue(cropBox_->value()) << " ";
        }
        cmd << "--demosaic " << selectedDemosaic() << " ";
        cmd << "--merge-weight " << selectedMergeWeight() << " ";
        if (rawgridBox_->checked()) {
            cmd << "--rawgrid ";
        }
        if (bloomPreventBox_->checked()) {
            cmd << "--bloom-prevent ";
        }
        cmd << (fisheyeBox_->checked() ? "--fisheye " : "--no-fisheye ");
        if (!normalizedTextBoxValue(extraArgsBox_->value()).empty()) {
            cmd << normalizedTextBoxValue(extraArgsBox_->value()) << " ";
        }
        cmd << "-o " << shellQuote((outputFormat == "hdr" ? finalOutput : tempHdr).string()) << " ";
        cmd << quoteInputFiles(inputFiles_);
        if (outputFormat == "exr") {
            if (paths_.oiiotool.empty() || !fs::exists(paths_.oiiotool)) {
                showMessage("EXR output needs oiiotool. Set it in Settings.");
                return;
            }
            cmd << " && " << shellQuote(paths_.oiiotool.string()) << " "
                << shellQuote(tempHdr.string()) << " -o " << shellQuote(finalOutput.string());
        }

        mergeJob_ = {};
        mergeJob_.active = true;
        mergeJob_.pid = spawnShellJob(cmd.str(), jobLog);
        mergeJob_.logPath = jobLog;
        mergeJob_.outputPath = finalOutput;
        mergeJob_.command = cmd.str();
        mergeJob_.startedAt = std::chrono::steady_clock::now();
        lastOutputUsesRadiance179_ = selectedColorspace() == "rad" && outputFormat == "hdr";
        lastMergeCommand_ = cmd.str();
        lastBackgroundRedrawAt_ = std::chrono::steady_clock::now() - 50ms;
        progressBar_->set_value(0.02f);
        stageLabel_->set_caption("Running merge");
        rangeLabel_->set_caption("Waiting for luminance summary...");
        issuesLabel_->set_caption("");
        redraw();
    }

    void pollMergeJob() {
        if (!mergeJob_.active) {
            return;
        }

        if (!mergeJob_.finished) {
            int status = 0;
            pid_t result = waitpid(mergeJob_.pid, &status, WNOHANG);
            if (result == mergeJob_.pid) {
                mergeJob_.finished = true;
                mergeJob_.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : status;
            }
        }

        const std::string log = readFile(mergeJob_.logPath);
        int percent = lastPercentInText(log);
        if (mergeJob_.finished && mergeJob_.exitCode == 0) {
            percent = 100;
        }
        if (percent < 0) {
            percent = mergeJob_.finished ? 100 : 5;
        }
        progressBar_->set_value(std::clamp(percent / 100.0f, 0.0f, 1.0f));

        const std::string stage = lastStageInText(log);
        stageLabel_->set_caption(stage.empty() ? (mergeJob_.finished ? "Finished" : "Merging") : stage);
        rangeLabel_->set_caption(summarizeRangeFromLog(log));
        issuesLabel_->set_caption(collectWarnings(log));

        if (mergeJob_.finished) {
            mergeJob_.active = false;
            if (mergeJob_.exitCode == 0 && fs::exists(mergeJob_.outputPath)) {
                stageLabel_->set_caption("Loading result");
                loadImageIntoViewer(mergeJob_.outputPath);
                stageLabel_->set_caption("Merge complete");
            } else {
                stageLabel_->set_caption("Merge failed");
                if (issuesLabel_->caption().empty()) {
                    issuesLabel_->set_caption("Check merge settings.");
                }
            }
        }
    }

    template <typename Builder>
    void startWorkerTask(WorkerTask& task, Builder&& builder) {
        std::lock_guard<std::mutex> lock{task.mutex};
        if (task.active) {
            showMessage("A task is already running in this tab.");
            return;
        }

        const auto [command, error] = builder();
        if (!error.empty()) {
            showMessage(error);
            return;
        }

        task.active = true;
        task.finished = false;
        task.exitCode = -1;
        task.command = command;
        task.output.clear();
        lastBackgroundRedrawAt_ = std::chrono::steady_clock::now() - 50ms;

        auto state = &task;
        std::thread([this, state, command]() {
            ProcessResult result = runShellCommandWithCapturedOutput(command);
            scheduleToUiThread([state, command, result]() {
                std::lock_guard<std::mutex> taskLock{state->mutex};
                state->active = false;
                state->finished = true;
                state->exitCode = result.exitCode;
                state->command = command;
                state->output = result.stdoutText + result.stderrText;
            });
        }).detach();
    }

    void pollWorkerTask(WorkerTask& task, PlainTextArea* outputArea) {
        std::lock_guard<std::mutex> lock{task.mutex};
        if (!task.finished) {
            return;
        }

        outputArea->setText(task.output);
        task.finished = false;
    }

    std::pair<std::string, std::string> buildColorCommand() const {
        if (paths_.mergehdr.empty() || !fs::exists(paths_.mergehdr)) {
            return {"", "Set a valid mergehdr path in Settings."};
        }
        if (trim(refFileBox_->value()).empty() || trim(testFileBox_->value()).empty()) {
            return {"", "Choose both reference and camera HDR files first."};
        }

        std::ostringstream cmd;
        cmd << "cd " << shellQuote(workingDir_.string()) << " && ";
        cmd << shellQuote(paths_.mergehdr.string()) << " colorcalibrate ";
        cmd << shellQuote(refFileBox_->value()) << " " << shellQuote(testFileBox_->value()) << " ";
        if (!trim(refCellsBox_->value()).empty()) {
            cmd << "-rc " << shellQuote(refCellsBox_->value()) << " ";
        }
        if (!trim(testCellsBox_->value()).empty()) {
            cmd << "-tc " << shellQuote(testCellsBox_->value()) << " ";
        }
        cmd << "-refcol " << shellQuote(refColorCombo_->items()[static_cast<size_t>(refColorCombo_->selected_index())]) << " ";
        cmd << "-minimizer " << shellQuote(minimizerCombo_->items()[static_cast<size_t>(minimizerCombo_->selected_index())]) << " ";
        if (!trim(colorXyzcamBox_->value()).empty()) {
            cmd << "-xyzcam " << shellQuote(trim(colorXyzcamBox_->value())) << " ";
        }
        return {cmd.str(), {}};
    }

    void startColorCalibration() {
        startWorkerTask(colorTask_, [this] { return buildColorCommand(); });
    }

    void copyRecommendedMatrix() {
        auto matrix = parseRecommendedMatrix(colorResultAreaText());
        if (!matrix.has_value()) {
            showMessage("No recommended xyzcam found yet.");
            return;
        }

        std::ostringstream oss;
        for (size_t i = 0; i < matrix->size(); ++i) {
            if (i) {
                oss << ' ';
            }
            oss << std::fixed << std::setprecision(6) << (*matrix)[i];
        }
        glfwSetClipboardString(glfw_window(), oss.str().c_str());
        showMessage("Copied recommended xyzcam.");
    }

    std::string colorResultAreaText() const {
        return textAreaPlainText(colorResultArea_);
    }

    void runChartCells(bool reference) {
        if (paths_.mergehdr.empty() || !fs::exists(paths_.mergehdr)) {
            showMessage("Set a valid mergehdr path in Settings.");
            return;
        }

        const std::string imagePath = reference ? trim(refFileBox_->value()) : trim(testFileBox_->value());
        if (imagePath.empty()) {
            showMessage("Choose an HDR image first.");
            return;
        }

        fs::path image = imagePath;
        fs::path cells = paths_.cacheDir / (sanitizeStem(image) + (reference ? "_ref_cells.txt" : "_test_cells.txt"));
        fs::path preview = paths_.cacheDir / (sanitizeStem(image) + (reference ? "_ref_cells.jpg" : "_test_cells.jpg"));

        std::ostringstream cmd;
        cmd << "cd " << shellQuote(image.parent_path().string()) << " && ";
        cmd << shellQuote(paths_.mergehdr.string()) << " chartcells "
            << shellQuote(image.string()) << " -o " << shellQuote(cells.string())
            << " -preview " << shellQuote(preview.string()) << " --white-first";

        ProcessResult result = runShellCommandWithCapturedOutput(cmd.str());
        if (result.exitCode != 0) {
            showMessage("Chart cell detection failed.");
            if (reference) {
                colorResultArea_->setText(result.stdoutText + result.stderrText);
            } else {
                colorResultArea_->setText(result.stdoutText + result.stderrText);
            }
            return;
        }

        if (reference) {
            refCellsBox_->set_value(cells.string());
        } else {
            testCellsBox_->set_value(cells.string());
        }
        loadImageIntoViewer(preview);
    }

    std::pair<std::string, std::string> buildShutterCommand() const {
        if (paths_.mergehdr.empty() || !fs::exists(paths_.mergehdr)) {
            return {"", "Set a valid mergehdr path in Settings."};
        }

        std::vector<std::string> sequences = splitLines(textAreaPlainText(shutterSequencesArea_));
        if (sequences.empty()) {
            return {"", "Add at least one shutter sequence."};
        }

        std::ostringstream cmd;
        cmd << "cd " << shellQuote(workingDir_.string()) << " && "
            << shellQuote(paths_.mergehdr.string()) << " shutter ";
        for (const auto& seq : sequences) {
            cmd << "-seq " << shellQuote(seq) << " ";
        }
        for (const auto& crop : splitLines(textAreaPlainText(shutterCropsArea_))) {
            cmd << "-crop " << shellQuote(crop) << " ";
        }
        cmd << "-channel " << shellQuote(shutterChannelCombo_->items()[static_cast<size_t>(shutterChannelCombo_->selected_index())]) << " ";
        return {cmd.str(), {}};
    }

    void startShutterCalibration() {
        startWorkerTask(shutterTask_, [this] { return buildShutterCommand(); });
    }

    std::pair<std::string, std::string> buildApertureCommand() const {
        if (paths_.mergehdr.empty() || !fs::exists(paths_.mergehdr)) {
            return {"", "Set a valid mergehdr path in Settings."};
        }

        std::vector<std::string> sequences = splitLines(textAreaPlainText(apertureSequencesArea_));
        if (sequences.empty()) {
            return {"", "Add at least one aperture sequence."};
        }

        std::ostringstream cmd;
        cmd << "cd " << shellQuote(workingDir_.string()) << " && "
            << shellQuote(paths_.mergehdr.string()) << " aperture ";
        if (!trim(apertureShuttercBox_->value()).empty()) {
            cmd << "-shutterc " << shellQuote(trim(apertureShuttercBox_->value())) << " ";
        }
        if (!trim(apertureCropBox_->value()).empty()) {
            cmd << "-crop " << shellQuote(trim(apertureCropBox_->value())) << " ";
        }
        for (const auto& seq : sequences) {
            cmd << "-seq " << shellQuote(seq) << " ";
        }
        return {cmd.str(), {}};
    }

    void startApertureCalibration() {
        startWorkerTask(apertureTask_, [this] { return buildApertureCommand(); });
    }

    static std::string textAreaPlainText(const PlainTextArea* area) {
        return area ? area->text() : std::string{};
    }

    void updateViewerControls() {
        applyChannelMask();
    }

    void showMessage(const std::string& message) {
        auto* dialog = new MessageDialog(this, MessageDialog::Type::Information, "hdrspace", message);
        dialog->set_callback([](int) {});
    }
};

void queueUiTask(const std::function<void()>& fun) {
    if (gApp) {
        gApp->scheduleToUiThread(fun);
    }
}

void queueViewerRedraw() {
    if (gApp) {
        gApp->requestRedrawFromWorker();
    }
}

} // namespace

int runHdrspaceNativeApp(int, char**) {
    glfwSetErrorCallback([](int error, const char* description) { tlog::warning("GLFW error {}: {}", error, description); });
    nanogui::init(true);

    auto* app = new NativeApp{};
    app->draw_all();
    app->set_visible(true);
    app->redraw();
    nanogui::run(nanogui::RunMode::Lazy);
    return 0;
}
