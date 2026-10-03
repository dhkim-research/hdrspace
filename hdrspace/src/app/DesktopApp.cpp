#include "app/DesktopApp.h"

#include "platform/ProcessRunner.h"
#include "platform/TevIpcClient.h"
#include "webview/webview.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mach-o/dyld.h>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

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

struct MergeJob {
    int id = 0;
    pid_t pid = -1;
    fs::path logPath;
    fs::path outputPath;
    std::string command;
    bool finished = false;
    int exitCode = -1;
    std::chrono::steady_clock::time_point startedAt;
};

std::mutex gJobMutex;
std::map<int, MergeJob> gJobs;
int gNextJobId = 1;

std::string readFile(const fs::path &path) {
    std::ifstream input(path);
    if (!input)
        return std::string();
    std::ostringstream oss;
    oss << input.rdbuf();
    return oss.str();
}

std::string trim(const std::string &text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
        ++start;
    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(start, end - start);
}

std::vector<std::string> splitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        std::string cleaned = trim(line);
        if (!cleaned.empty())
            lines.push_back(cleaned);
    }
    return lines;
}

std::string shellQuote(const std::string &value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'')
            out += "'\\''";
        else
            out += ch;
    }
    out += "'";
    return out;
}

std::string jsonEscape(const std::string &value) {
    std::ostringstream oss;
    for (char ch : value) {
        switch (ch) {
        case '\\': oss << "\\\\"; break;
        case '"': oss << "\\\""; break;
        case '\n': oss << "\\n"; break;
        case '\r': oss << "\\r"; break;
        case '\t': oss << "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                oss << "\\u"
                    << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch))
                    << std::dec << std::setfill(' ');
            } else {
                oss << ch;
            }
        }
    }
    return oss.str();
}

std::map<std::string, std::string> parseQuery(const std::string &query, const std::function<std::string(const std::string &)> &decode) {
    std::map<std::string, std::string> out;
    std::stringstream ss(query);
    std::string part;
    while (std::getline(ss, part, '&')) {
        if (part.empty())
            continue;
        size_t eq = part.find('=');
        std::string key = part.substr(0, eq);
        std::string value = (eq == std::string::npos) ? std::string() : part.substr(eq + 1);
        out[decode(key)] = decode(value);
    }
    return out;
}

std::string boolString(bool value) {
    return value ? "true" : "false";
}

std::string sanitizeStem(const fs::path &path) {
    std::string stem = path.stem().string();
    if (stem.empty())
        stem = "item";
    for (char &ch : stem) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_'))
            ch = '_';
    }
    return stem;
}

std::string replaceCarriageReturns(std::string text) {
    std::replace(text.begin(), text.end(), '\r', '\n');
    return text;
}

bool sendOpenImageToTev(const std::string &path, std::string *errorMessage) {
    return tevipc::sendOpenImage("127.0.0.1", 14158, path, "", true, errorMessage);
}

std::pair<int, int> parseImageSize(const std::string &text) {
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        std::stringstream ls(line);
        std::string token;
        int width = 0;
        int height = 0;
        while (ls >> token) {
            if (token == "x")
                continue;
            if (width == 0) {
                try {
                    width = std::stoi(token);
                    continue;
                } catch (...) {
                }
            } else if (height == 0) {
                if (token == "x")
                    continue;
                try {
                    height = std::stoi(token);
                    return {width, height};
                } catch (...) {
                    width = 0;
                }
            }
        }
    }
    return {0, 0};
}

std::string appleEscape(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '"')
            out += "\\\"";
        else
            out += ch;
    }
    return out;
}

std::string buildAppleScriptCommand(const std::vector<std::string> &lines) {
    std::ostringstream cmd;
    cmd << "osascript";
    for (const std::string &line : lines)
        cmd << " -e " << shellQuote(line);
    return cmd.str();
}

std::map<std::string, std::string> readKeyValueFile(const fs::path &path) {
    std::map<std::string, std::string> values;
    if (!fs::exists(path))
        return values;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        values[key] = value;
    }
    return values;
}

void writeKeyValueFile(const fs::path &path, const std::map<std::string, std::string> &values) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    if (!output)
        throw std::runtime_error("Could not write settings file.");
    for (const auto &entry : values)
        output << entry.first << "=" << entry.second << "\n";
}

std::vector<std::string> listProfileNames(const fs::path &profilesDir) {
    std::vector<std::string> names;
    if (!fs::exists(profilesDir))
        return names;
    for (const auto &entry : fs::directory_iterator(profilesDir)) {
        if (!entry.is_regular_file())
            continue;
        if (entry.path().extension() != ".cfg")
            continue;
        names.push_back(entry.path().stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::map<std::string, std::string> parseProfileFile(const fs::path &profilePath) {
    std::map<std::string, std::string> values;
    if (!fs::exists(profilePath))
        return values;
    std::ifstream input(profilePath);
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line.front() == '[')
            continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return values;
}

std::string jsonArray(const std::vector<std::string> &values) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i)
            oss << ",";
        oss << "\"" << jsonEscape(values[i]) << "\"";
    }
    oss << "]";
    return oss.str();
}

std::string quoteInputFiles(const std::vector<std::string> &files) {
    std::ostringstream oss;
    for (size_t i = 0; i < files.size(); ++i) {
        if (i)
            oss << " ";
        oss << shellQuote(files[i]);
    }
    return oss.str();
}

pid_t spawnShellJob(const std::string &command, const fs::path &logPath) {
    pid_t pid = fork();
    if (pid < 0)
        throw std::runtime_error("Could not start merge job.");
    if (pid == 0) {
        int fd = open(logPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0)
            _exit(127);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        execl("/bin/zsh", "zsh", "-lc", command.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    return pid;
}

int lastPercentInText(const std::string &text) {
    int last = -1;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        size_t pos = 0;
        while ((pos = line.find('%', pos)) != std::string::npos) {
            size_t start = pos;
            while (start > 0 && std::isdigit(static_cast<unsigned char>(line[start - 1])))
                --start;
            if (start < pos) {
                try {
                    int value = std::stoi(line.substr(start, pos - start));
                    last = value;
                } catch (...) {
                }
            }
            ++pos;
        }
    }
    return last;
}

std::string lastStageInText(const std::string &text) {
    std::stringstream ss(text);
    std::string line;
    std::string last;
    while (std::getline(ss, line)) {
        std::string trimmed = trim(line);
        if (trimmed.rfind("stage:", 0) == 0)
            last = trimmed.substr(6);
    }
    return trim(last);
}

}  // namespace

DesktopApp::DesktopApp() : paths_(resolvePaths()) {
    if (!paths_.cacheDir.empty())
        fs::create_directories(paths_.cacheDir);
}

int DesktopApp::run() {
    webview::webview window(true, nullptr);
    window.set_title("hdrspace");
    window.set_size(1340, 900, WEBVIEW_HINT_NONE);

    window.bind("appInfo", [this](const std::string &) -> std::string {
        return appInfoJson();
    });

    window.bind("profileInfo", [this](const std::string &req) -> std::string {
        return profileInfoFromQuery(parseSingleStringArg(req));
    });

    window.bind("saveSettings", [this](const std::string &req) -> std::string {
        return saveSettingsFromQuery(parseSingleStringArg(req));
    });

    window.bind("chooseDialog", [this](const std::string &req) -> std::string {
        return chooseDialogFromQuery(parseSingleStringArg(req));
    });

    window.bind("startMerge", [this](const std::string &req) -> std::string {
        return startMergeFromQuery(parseSingleStringArg(req));
    });

    window.bind("pollMergeJob", [this](const std::string &req) -> std::string {
        return pollMergeJobFromQuery(parseSingleStringArg(req));
    });

    window.bind("makePreview", [this](const std::string &req) -> std::string {
        return makePreviewFromQuery(parseSingleStringArg(req));
    });

    window.bind("runChartCells", [this](const std::string &req) -> std::string {
        return runChartCellsFromQuery(parseSingleStringArg(req));
    });

    window.bind("runColorCalibrate", [this](const std::string &req) -> std::string {
        return runColorCalibrateFromQuery(parseSingleStringArg(req));
    });

    window.bind("runShutter", [this](const std::string &req) -> std::string {
        return runShutterFromQuery(parseSingleStringArg(req));
    });

    window.bind("runAperture", [this](const std::string &req) -> std::string {
        return runApertureFromQuery(parseSingleStringArg(req));
    });

    window.bind("openTev", [this](const std::string &req) -> std::string {
        return openTevForPath(parseSingleStringArg(req));
    });

    if (!fs::exists(paths_.uiIndex))
        throw std::runtime_error("UI entry file not found: " + paths_.uiIndex.string());

    window.navigate(fileUrl(paths_.uiIndex));
    window.run();
    return 0;
}

fs::path DesktopApp::executablePath() {
    std::vector<char> buffer(4096, '\0');
    uint32_t size = static_cast<uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size + 1, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
            throw std::runtime_error("Could not resolve executable path.");
    }
    return fs::weakly_canonical(fs::path(buffer.data()));
}

fs::path DesktopApp::findOnPath(const std::string &name) {
    const char *pathEnv = std::getenv("PATH");
    if (!pathEnv)
        return fs::path();
    std::stringstream ss(pathEnv);
    std::string entry;
    while (std::getline(ss, entry, ':')) {
        if (entry.empty())
            continue;
        fs::path candidate = fs::path(entry) / name;
        if (fs::exists(candidate))
            return fs::weakly_canonical(candidate);
    }
    return fs::path();
}

std::string DesktopApp::jsonEscape(const std::string &value) {
    return ::jsonEscape(value);
}

std::string DesktopApp::parseSingleStringArg(const std::string &req) {
    if (req.size() < 4 || req[0] != '[' || req[1] != '"' || req[req.size() - 2] != '"' || req[req.size() - 1] != ']')
        throw std::runtime_error("Unexpected bridge payload.");
    std::string raw = req.substr(2, req.size() - 4);
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        char ch = raw[i];
        if (ch == '\\' && i + 1 < raw.size()) {
            char next = raw[++i];
            switch (next) {
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case '/': out.push_back('/'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(next); break;
            }
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::string DesktopApp::urlDecode(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        char ch = value[i];
        if (ch == '%' && i + 2 < value.size()) {
            std::string hex = value.substr(i + 1, 2);
            char decoded = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            out.push_back(decoded);
            i += 2;
        } else if (ch == '+') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

DesktopApp::AppPaths DesktopApp::resolvePaths() const {
    AppPaths out;
    fs::path exe = executablePath();
    out.appRoot = fs::weakly_canonical(exe.parent_path().parent_path());
    out.uiIndex = out.appRoot / "build" / "ui" / "index.html";
    out.cacheDir = out.appRoot / "build" / "cache";
    out.settingsFile = out.cacheDir / "settings.conf";
    out.profilesDir = out.appRoot.parent_path() / "mergehdr" / "profiles";

    fs::path detectedMergehdr = out.appRoot.parent_path() / "mergehdr" / "bin" / "mergehdr";
    if (!fs::exists(detectedMergehdr))
        detectedMergehdr = findOnPath("mergehdr");
    out.mergehdr = detectedMergehdr;

    fs::path detectedTev = findOnPath("tev");
    if (detectedTev.empty()) {
        fs::path appTev("/Applications/tev.app/Contents/MacOS/tev");
        if (fs::exists(appTev))
            detectedTev = appTev;
    }
    out.tev = detectedTev;

    fs::path detectedOiiotool = findOnPath("oiiotool");
    if (detectedOiiotool.empty()) {
        fs::path brew("/opt/homebrew/bin/oiiotool");
        if (fs::exists(brew))
            detectedOiiotool = brew;
    }
    out.oiiotool = detectedOiiotool;

    std::map<std::string, std::string> settings = readKeyValueFile(out.settingsFile);
    if (!settings["mergehdrPath"].empty())
        out.mergehdr = fs::path(settings["mergehdrPath"]);
    if (!settings["tevPath"].empty())
        out.tev = fs::path(settings["tevPath"]);
    if (!settings["oiiotoolPath"].empty())
        out.oiiotool = fs::path(settings["oiiotoolPath"]);
    return out;
}

void DesktopApp::reloadPaths() {
    paths_ = resolvePaths();
}

std::string DesktopApp::fileUrl(const fs::path &path) const {
    auto escape = [](const std::string &text) {
        std::ostringstream oss;
        for (unsigned char ch : text) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '/' || ch == '-' || ch == '_' ||
                ch == '.' || ch == '~') {
                oss << static_cast<char>(ch);
            } else {
                oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(ch) << std::nouppercase << std::dec << std::setfill(' ');
            }
        }
        return oss.str();
    };
    return "file://" + escape(path.string());
}

std::string DesktopApp::appInfoJson() const {
    fs::path cwd = safeCurrentPath();
    std::map<std::string, std::string> settings = readKeyValueFile(paths_.settingsFile);
    std::vector<std::string> profiles = listProfileNames(paths_.profilesDir);
    std::ostringstream oss;
    oss << "{"
        << "\"mergehdrPath\":\"" << jsonEscape(paths_.mergehdr.string()) << "\","
        << "\"tevPath\":\"" << jsonEscape(paths_.tev.string()) << "\","
        << "\"oiiotoolPath\":\"" << jsonEscape(paths_.oiiotool.string()) << "\","
        << "\"savedMergehdrPath\":\"" << jsonEscape(settings["mergehdrPath"]) << "\","
        << "\"savedTevPath\":\"" << jsonEscape(settings["tevPath"]) << "\","
        << "\"savedOiiotoolPath\":\"" << jsonEscape(settings["oiiotoolPath"]) << "\","
        << "\"defaultWorkdir\":\"" << jsonEscape(cwd.string()) << "\","
        << "\"uiPath\":\"" << jsonEscape(paths_.uiIndex.string()) << "\","
        << "\"profiles\":" << jsonArray(profiles)
        << "}";
    return oss.str();
}

std::string DesktopApp::profileInfoFromQuery(const std::string &query) const {
    std::map<std::string, std::string> params = parseQuery(query, &DesktopApp::urlDecode);
    std::string profile = params["profile"];
    if (profile.empty())
        return "{\"ok\":false,\"error\":\"Profile name is required.\"}";
    fs::path profilePath = paths_.profilesDir / (profile + ".cfg");
    std::map<std::string, std::string> values = parseProfileFile(profilePath);
    if (values.empty())
        return "{\"ok\":false,\"error\":\"Profile not found.\"}";

    auto valueOr = [&](const char *key) {
        auto it = values.find(key);
        return it == values.end() ? std::string() : it->second;
    };

    std::ostringstream oss;
    oss << "{"
        << "\"ok\":true,"
        << "\"profile\":\"" << jsonEscape(profile) << "\","
        << "\"crop\":\"" << jsonEscape(valueOr("crop")) << "\","
        << "\"colorspace\":\"" << jsonEscape(valueOr("colorspace")) << "\","
        << "\"fisheye\":\"" << jsonEscape(valueOr("fisheye")) << "\","
        << "\"saturation\":\"" << jsonEscape(valueOr("saturation")) << "\","
        << "\"white\":\"" << jsonEscape(valueOr("white")) << "\","
        << "\"black\":\"" << jsonEscape(valueOr("black")) << "\","
        << "\"nd\":\"" << jsonEscape(valueOr("nd")) << "\""
        << "}";
    return oss.str();
}

std::string DesktopApp::saveSettingsFromQuery(const std::string &query) {
    std::map<std::string, std::string> params = parseQuery(query, &DesktopApp::urlDecode);
    std::map<std::string, std::string> settings;
    settings["mergehdrPath"] = trim(params["mergehdrPath"]);
    settings["tevPath"] = trim(params["tevPath"]);
    settings["oiiotoolPath"] = trim(params["oiiotoolPath"]);
    writeKeyValueFile(paths_.settingsFile, settings);
    reloadPaths();
    return appInfoJson();
}

std::string DesktopApp::chooseDialogFromQuery(const std::string &query) const {
    std::map<std::string, std::string> params = parseQuery(query, &DesktopApp::urlDecode);
    std::string mode = params["mode"];
    std::string prompt = params["prompt"];
    std::string defaultName = params["defaultName"];
    if (prompt.empty())
        prompt = "Choose a path";

    std::vector<std::string> lines;
    if (mode == "files") {
        lines = {
            "set chosenFiles to choose file with prompt \"" + appleEscape(prompt) + "\" with multiple selections allowed",
            "set outputText to \"\"",
            "repeat with f in chosenFiles",
            "set outputText to outputText & POSIX path of f & linefeed",
            "end repeat",
            "return outputText"
        };
    } else if (mode == "folder") {
        lines = {
            "return POSIX path of (choose folder with prompt \"" + appleEscape(prompt) + "\")"
        };
    } else if (mode == "save") {
        std::string clause = defaultName.empty() ? "" : " default name \"" + appleEscape(defaultName) + "\"";
        lines = {
            "return POSIX path of (choose file name with prompt \"" + appleEscape(prompt) + "\"" + clause + ")"
        };
    } else {
        lines = {
            "return POSIX path of (choose file with prompt \"" + appleEscape(prompt) + "\")"
        };
    }

    ProcessResult result = runShellCommandWithCapturedOutput(buildAppleScriptCommand(lines));
    std::string value = trim(result.stdoutText);
    std::ostringstream oss;
    oss << "{"
        << "\"ok\":" << boolString(result.exitCode == 0) << ","
        << "\"value\":\"" << jsonEscape(value) << "\","
        << "\"log\":\"" << jsonEscape(result.stderrText) << "\""
        << "}";
    return oss.str();
}

std::string DesktopApp::startMergeFromQuery(const std::string &query) const {
    if (paths_.mergehdr.empty())
        throw std::runtime_error("Could not find mergehdr. Set it in Settings.");

    std::map<std::string, std::string> params = parseQuery(query, &DesktopApp::urlDecode);
    std::string workdir = params["workdir"];
    std::string inputs = params["inputs"];
    std::vector<std::string> inputFiles = splitLines(params["inputFiles"]);
    std::string outputDir = params["outputDir"];
    std::string outputName = params["outputName"];
    std::string outputFormat = params["outputFormat"];
    std::string profile = params["profile"];
    std::string colorspace = params["colorspace"];
    std::string black = trim(params["black"]);
    std::string white = trim(params["white"]);
    std::string saturation = trim(params["saturation"]);
    std::string xyzcam = trim(params["xyzcam"]);
    std::string extra = params["extra"];
    std::string crop = params["crop"];
    bool bloom = params["bloom"] == "1";
    bool fisheye = params["fisheye"] != "0";
    bool rawgrid = params["rawgrid"] == "1";
    std::string demosaic = params["demosaic"];
    std::string mergeWeight = params["mergeWeight"];

    if (workdir.empty())
        throw std::runtime_error("Workdir is required.");
    if (outputName.empty())
        throw std::runtime_error("Output name is required.");
    if (inputs.empty() && inputFiles.empty())
        throw std::runtime_error("Choose RAW files or enter an input pattern.");
    if (fisheye && crop.empty())
        throw std::runtime_error("Fisheye correction requires a crop. Pick a crop in the preview first.");

    if (outputFormat.empty())
        outputFormat = "hdr";
    if (outputFormat != "hdr" && outputFormat != "exr")
        throw std::runtime_error("Output format must be hdr or exr.");

    fs::path baseDir = workdir.empty() ? safeCurrentPath() : fs::path(workdir);
    fs::path finalDir = outputDir.empty() ? baseDir : fs::path(outputDir);
    if (!finalDir.is_absolute())
        finalDir = fs::weakly_canonical(baseDir / finalDir);
    fs::create_directories(finalDir);

    std::string finalName = outputName;
    if (fs::path(finalName).extension().empty())
        finalName += "." + outputFormat;
    fs::path finalOutput = finalDir / finalName;

    fs::create_directories(paths_.cacheDir);
    std::string token = sanitizeStem(finalOutput);
    fs::path tempHdr = paths_.cacheDir / (token + "_render.hdr");
    fs::path jobLog = paths_.cacheDir / (token + "_merge.log");

    std::ostringstream cmd;
    cmd << "cd " << shellQuote(workdir) << " && ";
    cmd << shellQuote(paths_.mergehdr.string()) << " ";
    if (!profile.empty())
        cmd << "-profile " << shellQuote(profile) << " ";
    cmd << "run ";
    if (!colorspace.empty())
        cmd << "--colorspace " << shellQuote(colorspace) << " ";
    if (!black.empty())
        cmd << "--black " << shellQuote(black) << " ";
    if (!white.empty())
        cmd << "--white " << shellQuote(white) << " ";
    if (!saturation.empty())
        cmd << "--saturation " << shellQuote(saturation) << " ";
    if (!xyzcam.empty())
        cmd << "--xyzcam " << shellQuote(xyzcam) << " ";
    if (!crop.empty())
        cmd << "--crop " << crop << " ";
    if (!demosaic.empty())
        cmd << "--demosaic " << shellQuote(demosaic) << " ";
    if (!mergeWeight.empty())
        cmd << "--merge-weight " << shellQuote(mergeWeight) << " ";
    if (rawgrid)
        cmd << "--rawgrid ";
    if (bloom)
        cmd << "--bloom-prevent ";
    cmd << (fisheye ? "--fisheye " : "--no-fisheye ");
    if (!extra.empty())
        cmd << extra << " ";
    cmd << "-o " << shellQuote((outputFormat == "hdr" ? finalOutput : tempHdr).string()) << " ";
    if (!inputFiles.empty())
        cmd << quoteInputFiles(inputFiles);
    else
        cmd << inputs;
    if (outputFormat == "exr") {
        if (paths_.oiiotool.empty())
            throw std::runtime_error("Could not find oiiotool. Set it in Settings for EXR output.");
        cmd << " && " << shellQuote(paths_.oiiotool.string()) << " "
            << shellQuote(tempHdr.string()) << " -o " << shellQuote(finalOutput.string());
    }

    pid_t pid = spawnShellJob(cmd.str(), jobLog);
    MergeJob job;
    {
        std::lock_guard<std::mutex> lock(gJobMutex);
        job.id = gNextJobId++;
        job.pid = pid;
        job.logPath = jobLog;
        job.outputPath = finalOutput;
        job.command = cmd.str();
        job.startedAt = std::chrono::steady_clock::now();
        gJobs[job.id] = job;
    }

    std::ostringstream oss;
    oss << "{"
        << "\"ok\":true,"
        << "\"jobId\":" << job.id << ","
        << "\"command\":\"" << jsonEscape(job.command) << "\","
        << "\"output\":\"" << jsonEscape(job.outputPath.string()) << "\""
        << "}";
    return oss.str();
}

std::string DesktopApp::pollMergeJobFromQuery(const std::string &query) const {
    std::map<std::string, std::string> params = parseQuery(query, &DesktopApp::urlDecode);
    int jobId = params["jobId"].empty() ? -1 : std::stoi(params["jobId"]);
    if (jobId < 0)
        throw std::runtime_error("Job id is required.");

    MergeJob job;
    {
        std::lock_guard<std::mutex> lock(gJobMutex);
        auto it = gJobs.find(jobId);
        if (it == gJobs.end())
            throw std::runtime_error("Job not found.");

        if (!it->second.finished) {
            int status = 0;
            pid_t result = waitpid(it->second.pid, &status, WNOHANG);
            if (result == it->second.pid) {
                it->second.finished = true;
                it->second.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : status;
            }
        }
        job = it->second;
    }

    std::string log = replaceCarriageReturns(readFile(job.logPath));
    int progress = lastPercentInText(log);
    if (job.finished && job.exitCode == 0)
        progress = 100;
    if (progress < 0)
        progress = job.finished ? 100 : 0;

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - job.startedAt).count() / 1000.0;
    std::ostringstream oss;
    oss << "{"
        << "\"ok\":true,"
        << "\"finished\":" << boolString(job.finished) << ","
        << "\"exitCode\":" << job.exitCode << ","
        << "\"progress\":" << progress << ","
        << "\"stage\":\"" << jsonEscape(lastStageInText(log)) << "\","
        << "\"elapsed\":" << std::fixed << std::setprecision(2) << elapsed << ","
        << "\"command\":\"" << jsonEscape(job.command) << "\","
        << "\"log\":\"" << jsonEscape(log) << "\","
        << "\"output\":\"" << jsonEscape(job.outputPath.string()) << "\""
        << "}";
    return oss.str();
}

std::string DesktopApp::makePreviewFromQuery(const std::string &query) const {
    if (paths_.oiiotool.empty())
        throw std::runtime_error("Could not find oiiotool. Set it in Settings.");

    std::map<std::string, std::string> params = parseQuery(query, &DesktopApp::urlDecode);
    std::string workdir = params["workdir"];
    std::string sourceText = params["source"];
    int fitWidth = params["fitWidth"].empty() ? 1600 : std::stoi(params["fitWidth"]);
    int fitHeight = params["fitHeight"].empty() ? 1200 : std::stoi(params["fitHeight"]);
    if (sourceText.empty())
        throw std::runtime_error("Preview source is required.");

    fs::path source = fs::path(sourceText);
    if (!source.is_absolute())
        source = fs::weakly_canonical((workdir.empty() ? safeCurrentPath() : fs::path(workdir)) / source);
    if (!fs::exists(source))
        throw std::runtime_error("Preview source not found: " + source.string());

    fs::create_directories(paths_.cacheDir);
    std::string stem = sanitizeStem(source);
    fs::path preview = paths_.cacheDir / (stem + "_preview.png");

    std::ostringstream cmd;
    cmd << shellQuote(paths_.oiiotool.string()) << " "
        << shellQuote(source.string()) << " --fit " << fitWidth << "x" << fitHeight
        << " -o " << shellQuote(preview.string());
    ProcessResult run = runShellCommandWithCapturedOutput(cmd.str());
    if (run.exitCode != 0)
        throw std::runtime_error("Preview generation failed.\n" + run.stderrText);

    ProcessResult sourceInfo = runShellCommandWithCapturedOutput(
        shellQuote(paths_.oiiotool.string()) + " --info " + shellQuote(source.string()));
    ProcessResult previewInfo = runShellCommandWithCapturedOutput(
        shellQuote(paths_.oiiotool.string()) + " --info " + shellQuote(preview.string()));
    std::pair<int, int> sourceSize = parseImageSize(sourceInfo.stdoutText);
    std::pair<int, int> previewSize = parseImageSize(previewInfo.stdoutText);

    std::ostringstream oss;
    oss << "{"
        << "\"ok\":true,"
        << "\"command\":\"" << jsonEscape(cmd.str()) << "\","
        << "\"log\":\"" << jsonEscape(run.stderrText) << "\","
        << "\"source\":\"" << jsonEscape(source.string()) << "\","
        << "\"previewPath\":\"" << jsonEscape(preview.string()) << "\","
        << "\"previewUrl\":\"" << jsonEscape(fileUrl(preview)) << "\","
        << "\"sourceWidth\":" << sourceSize.first << ","
        << "\"sourceHeight\":" << sourceSize.second << ","
        << "\"previewWidth\":" << previewSize.first << ","
        << "\"previewHeight\":" << previewSize.second
        << "}";
    return oss.str();
}

std::string DesktopApp::runChartCellsFromQuery(const std::string &query) const {
    if (paths_.mergehdr.empty())
        throw std::runtime_error("Could not find mergehdr. Set it in Settings.");

    std::map<std::string, std::string> params = parseQuery(query, &DesktopApp::urlDecode);
    std::string workdir = params["workdir"];
    std::string imageText = params["image"];
    std::string cellsText = params["cellsPath"];
    std::string previewText = params["previewPath"];
    std::string patches = params["patches"];
    std::string cols = params["cols"];
    std::string rows = params["rows"];
    std::string inset = params["inset"];
    bool rowMajor = params["rowMajor"] == "1";

    if (workdir.empty())
        throw std::runtime_error("Workdir is required.");
    if (imageText.empty())
        throw std::runtime_error("Chart image is required.");

    fs::path image = fs::path(imageText);
    if (!image.is_absolute())
        image = fs::weakly_canonical(fs::path(workdir) / image);
    if (!fs::exists(image))
        throw std::runtime_error("Chart image not found: " + image.string());

    fs::create_directories(paths_.cacheDir);
    if (cellsText.empty())
        cellsText = (paths_.cacheDir / (sanitizeStem(image) + "_cells.txt")).string();
    if (previewText.empty())
        previewText = (paths_.cacheDir / (sanitizeStem(image) + "_cells.jpg")).string();

    fs::path cells = cellsText;
    if (!cells.is_absolute())
        cells = fs::weakly_canonical(fs::path(workdir) / cells);
    fs::path preview = previewText;
    if (!preview.is_absolute())
        preview = fs::weakly_canonical(fs::path(workdir) / preview);

    std::ostringstream cmd;
    cmd << "cd " << shellQuote(workdir) << " && "
        << shellQuote(paths_.mergehdr.string()) << " chartcells "
        << shellQuote(image.string()) << " -o " << shellQuote(cells.string())
        << " -preview " << shellQuote(preview.string()) << " ";
    if (!patches.empty())
        cmd << "-patches " << shellQuote(patches) << " ";
    if (!cols.empty())
        cmd << "-cols " << shellQuote(cols) << " ";
    if (!rows.empty())
        cmd << "-rows " << shellQuote(rows) << " ";
    if (!inset.empty())
        cmd << "-inset " << shellQuote(inset) << " ";
    cmd << (rowMajor ? "--row-major " : "--white-first ");

    ProcessResult result = runShellCommandWithCapturedOutput(cmd.str());
    std::ostringstream oss;
    oss << "{"
        << "\"ok\":" << boolString(result.exitCode == 0) << ","
        << "\"exitCode\":" << result.exitCode << ","
        << "\"command\":\"" << jsonEscape(cmd.str()) << "\","
        << "\"log\":\"" << jsonEscape(result.stdoutText + result.stderrText) << "\","
        << "\"cellsPath\":\"" << jsonEscape(cells.string()) << "\","
        << "\"previewPath\":\"" << jsonEscape(preview.string()) << "\","
        << "\"previewUrl\":\"" << jsonEscape(fileUrl(preview)) << "\","
        << "\"cellsText\":\"" << jsonEscape(readFile(cells)) << "\""
        << "}";
    return oss.str();
}

std::string DesktopApp::runColorCalibrateFromQuery(const std::string &query) const {
    if (paths_.mergehdr.empty())
        throw std::runtime_error("Could not find mergehdr. Set it in Settings.");

    std::map<std::string, std::string> params = parseQuery(query, &DesktopApp::urlDecode);
    std::ostringstream cmd;
    cmd << "cd " << shellQuote(params["workdir"]) << " && "
        << shellQuote(paths_.mergehdr.string()) << " colorcalibrate "
        << shellQuote(params["reference"]) << " "
        << shellQuote(params["test"]) << " ";
    if (!params["refCells"].empty())
        cmd << "-rc " << shellQuote(params["refCells"]) << " ";
    if (!params["testCells"].empty())
        cmd << "-tc " << shellQuote(params["testCells"]) << " ";
    if (!params["refcol"].empty())
        cmd << "-refcol " << shellQuote(params["refcol"]) << " ";
    if (!params["xyzcam"].empty())
        cmd << "-xyzcam " << shellQuote(params["xyzcam"]) << " ";
    if (!params["minimizer"].empty())
        cmd << "-minimizer " << shellQuote(params["minimizer"]) << " ";
    if (params["verbose"] == "1")
        cmd << "--verbose ";

    ProcessResult result = runShellCommandWithCapturedOutput(cmd.str());
    std::ostringstream oss;
    oss << "{"
        << "\"ok\":" << boolString(result.exitCode == 0) << ","
        << "\"exitCode\":" << result.exitCode << ","
        << "\"command\":\"" << jsonEscape(cmd.str()) << "\","
        << "\"result\":\"" << jsonEscape(result.stdoutText + result.stderrText) << "\""
        << "}";
    return oss.str();
}

std::string DesktopApp::runShutterFromQuery(const std::string &query) const {
    if (paths_.mergehdr.empty())
        throw std::runtime_error("Could not find mergehdr. Set it in Settings.");
    std::map<std::string, std::string> params = parseQuery(query, &DesktopApp::urlDecode);
    std::ostringstream cmd;
    cmd << "cd " << shellQuote(params["workdir"]) << " && "
        << shellQuote(paths_.mergehdr.string()) << " shutter ";
    for (const std::string &line : splitLines(params["sequences"]))
        cmd << "-seq " << shellQuote(line) << " ";
    for (const std::string &line : splitLines(params["crops"]))
        cmd << "-crop " << shellQuote(line) << " ";
    if (!params["channel"].empty())
        cmd << "-channel " << shellQuote(params["channel"]) << " ";
    if (!params["dataout"].empty())
        cmd << "-dataout " << shellQuote(params["dataout"]) << " ";

    ProcessResult result = runShellCommandWithCapturedOutput(cmd.str());
    std::ostringstream oss;
    oss << "{"
        << "\"ok\":" << boolString(result.exitCode == 0) << ","
        << "\"exitCode\":" << result.exitCode << ","
        << "\"command\":\"" << jsonEscape(cmd.str()) << "\","
        << "\"result\":\"" << jsonEscape(result.stdoutText + result.stderrText) << "\""
        << "}";
    return oss.str();
}

std::string DesktopApp::runApertureFromQuery(const std::string &query) const {
    if (paths_.mergehdr.empty())
        throw std::runtime_error("Could not find mergehdr. Set it in Settings.");
    std::map<std::string, std::string> params = parseQuery(query, &DesktopApp::urlDecode);
    std::ostringstream cmd;
    cmd << "cd " << shellQuote(params["workdir"]) << " && "
        << shellQuote(paths_.mergehdr.string()) << " aperture ";
    if (!params["shutterc"].empty())
        cmd << "-shutterc " << shellQuote(params["shutterc"]) << " ";
    if (!params["crop"].empty())
        cmd << "-crop " << shellQuote(params["crop"]) << " ";
    for (const std::string &line : splitLines(params["sequences"]))
        cmd << "-seq " << shellQuote(line) << " ";

    ProcessResult result = runShellCommandWithCapturedOutput(cmd.str());
    std::ostringstream oss;
    oss << "{"
        << "\"ok\":" << boolString(result.exitCode == 0) << ","
        << "\"exitCode\":" << result.exitCode << ","
        << "\"command\":\"" << jsonEscape(cmd.str()) << "\","
        << "\"result\":\"" << jsonEscape(result.stdoutText + result.stderrText) << "\""
        << "}";
    return oss.str();
}

std::string DesktopApp::openTevForPath(const std::string &pathText) const {
    if (paths_.tev.empty())
        return "{\"ok\":false,\"error\":\"Could not find tev. Set it in Settings.\"}";
    if (pathText.empty())
        return "{\"ok\":false,\"error\":\"No output file was given.\"}";

    std::string error;
    if (!sendOpenImageToTev(pathText, &error)) {
        std::string cmd = shellQuote(paths_.tev.string()) + " --host 127.0.0.1:14158 >/dev/null 2>&1 &";
        int launchCode = std::system(cmd.c_str());
        if (launchCode == 0) {
            for (int attempt = 0; attempt < 20; ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                error.clear();
                if (sendOpenImageToTev(pathText, &error)) {
                    std::ostringstream ok;
                    ok << "{"
                       << "\"ok\":true,"
                       << "\"exitCode\":0,"
                       << "\"mode\":\"ipc\""
                       << "}";
                    return ok.str();
                }
            }
        }

        std::string fallback = shellQuote(paths_.tev.string()) + " " + shellQuote(pathText) + " >/dev/null 2>&1 &";
        int fallbackCode = std::system(fallback.c_str());
        std::ostringstream fallbackOut;
        fallbackOut << "{"
                    << "\"ok\":" << boolString(fallbackCode == 0) << ","
                    << "\"exitCode\":" << fallbackCode << ","
                    << "\"mode\":\"spawn\","
                    << "\"error\":\"" << jsonEscape(error) << "\""
                    << "}";
        return fallbackOut.str();
    }

    std::ostringstream oss;
    oss << "{"
        << "\"ok\":true,"
        << "\"exitCode\":0,"
        << "\"mode\":\"ipc\""
        << "}";
    return oss.str();
}
