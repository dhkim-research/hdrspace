#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/program_options.hpp>
#include <exiv2/easyaccess.hpp>
#include <exiv2/image.hpp>

#include "camera_detect.h"
#include "chartcells.h"
#include "colorcalibrate.h"
#include "glareeval.h"
#include "hdrimage.h"
#include "hdrmerge.h"
#include "shadowband_native.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#if !defined(_WIN32)
#if !defined(_WIN32)
#include <glob.h>
#endif
#endif
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <chrono>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
// Declared here instead of including <windows.h>, whose macros clash with ordinary names.
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void *, wchar_t *, unsigned long);
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <vector>

#include <Eigen/LU>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace po = boost::program_options;
namespace fs = boost::filesystem;

namespace {

const char *MERGEHDR_VERSION = "1.0";
std::string g_invoked_argv0;

struct GlobalOptions {
    bool help = false;
    bool version = false;
    std::string configPath;
    std::string profileName;
};

struct RunOptions {
    bool help = false;
    bool verbose = false;
    bool correct = true;
    bool fisheye = false;
    bool bloomPrevent = false;
    bool rawgrid = false;
    bool fisheyeOverride = false;
    bool colorspaceOverride = false;
    bool demosaicOverride = false;
    bool mergeWeightOverride = false;
    bool xyzcamOverride = false;
    bool rawMultipliersOverride = false;
    bool badpixelsOverride = false;
    bool cscaleOverride = false;
    bool cropOverride = false;
    bool blackOverride = false;
    bool whiteOverride = false;
    bool scaleOverride = false;
    bool ndOverride = false;
    bool saturationOverride = false;
    bool rangeOverride = false;
    bool shuttercOverride = false;
    bool foOverride = false;
    bool outputOverride = false;
    bool vfileOverride = false;
    bool dualFisheye = false;

    std::string colorspace = "rad";
    std::string demosaic = "ahd";
    std::string mergeWeight = "hdrmerge";
    std::string xyzcamText;
    std::string rawMultipliersText;
    std::string badpixelsText;
    std::string cscaleText;
    std::string foText;
    std::string outputPath;
    std::string vfilePath;
    std::string singleFisheyeProjection;
    std::string projectionReferenceCal;
    std::string projectionView = "-vta -vp 0 0 0 -vd 0 0 1 -vu 0 1 0 -vh 180 -vv 180";
    std::vector<int> crop;
    std::vector<int> cropLeft;
    std::vector<int> cropRight;
    std::vector<double> projectionCoefficients;
    int blacklevel = -1;
    int whitepoint = -1;
    double projectionSourceRadius = 0.0;
    double projectionTargetRadius = 0.0;
    double scale = 1.0;
    double nd = 0.0;
    double saturation = 0.01;
    double rangeValue = 0.0;
    double shutterc = 0.0;
    std::vector<std::string> inputs;
};

struct CalibrationCommonOptions {
    bool blackOverride = false;
    bool whiteOverride = false;
    bool scaleOverride = false;
    bool ndOverride = false;
    bool saturationOverride = false;
    bool rangeOverride = false;
    bool shuttercOverride = false;
    bool foOverride = false;
    bool cropOverride = false;
    bool badpixelsOverride = false;

    int blacklevel = -1;
    int whitepoint = -1;
    double scale = 1.0;
    double nd = 0.0;
    double saturation = 0.01;
    double rangeValue = 0.0;
    double shutterc = 0.0;
    std::string foText;
    std::string badpixelsText;
    std::vector<int> crop;
};

struct ShutterOptions {
    bool help = false;
    std::vector<std::string> sequenceTexts;
    std::vector<std::string> cropTexts;
    std::string channel = "g";
    std::string dataOut;
    CalibrationCommonOptions common;
};

struct ApertureOptions {
    bool help = false;
    std::vector<std::string> sequenceTexts;
    std::string cropText;
    CalibrationCommonOptions common;
};

struct ChartCellsCliOptions {
    bool help = false;
    std::string imagePath;
    std::string outputPath;
    std::string previewPath;
    int patchCount = 0;
    int columns = 6;
    int rows = 4;
    double inset = 0.15;
    bool whiteFirst = true;
    bool layoutExplicit = false;
};

struct HdrCropOptions {
    bool help = false;
    std::string inputPath;
    std::string outputPath;
    std::vector<int> crop;
};

struct HdrRotateOptions {
    bool help = false;
    std::string inputPath;
    std::string outputPath;
    double angleDegrees = 0.0;
    bool angleSet = false;
};

struct HdrProjectOptions {
    bool help = false;
    std::string inputPath;
    std::string outputPath;
    bool equisolid = false;
    bool hemispherical = false;
    bool equidistant = false;
    std::vector<double> polynomialCoefficients;
    double sourceRadius = 0.0;
    double targetRadius = 0.0;
    std::string projectionView = "-vta -vp 0 0 0 -vd 0 0 1 -vu 0 1 0 -vh 180 -vv 180";
};

struct EvalGlareCliOptions {
    bool help = false;
    EvalGlareOptions native;
    EvalGlareCliParseResult parsed;  // -v/-a texts, usage errors, --version
};

struct ViewVisibilityCliOptions {
    bool help = false;
    bool writeDebugDump = false;
    std::string outputPath;
    std::string debugDir;
    ViewVisibilitySummaryOptions native;
};

struct ViewVosCalculationOptions {
    bool help = false;
    bool windowDistanceSet = false;
    bool estimateWindowDistance = false;
    std::string interiorPath;
    std::string exteriorPath;
    std::string windowMaskPath;
    std::string outputDir;
    double windowDistance = 0.0;
};

struct PerceptualMapCliOptions {
    bool help = false;
    std::string outputPath;
    std::string mapName;
    PerceptualMapOptions native;
};

struct ShadowbandCliOptions {
    bool help = false;
    std::string horizontalPath;
    std::string verticalPath;
    std::string noShadowPath;
    std::string outputPath = "blended.hdr";
    std::string rawExt = "CR2";
    std::string bandCfg;
    std::string ndCfg;
    ShadowbandMergeOptions native;
};

struct ExtractOptions {
    enum Mode {
        None,
        AverageOfRows,
        AverageOfColumns,
        RankedRectangle,
        RankedCircle
    };

    bool help = false;
    bool imageOutput = false;
    bool rgbOutput = false;
    Mode mode = None;
    int left = 0;
    int bottom = 0;
    int width = 0;
    int height = 0;
    int centerX = 0;
    int centerY = 0;
    double radiusDegrees = 0.0;
    std::string inputPath;
    std::string outputPath;
};

struct FrameInfo {
    std::string filename;
    std::string basename;
    std::string captureTime;
    double shutterRecip = 0.0;
    double exposureTime = 0.0;
    double shownExposure = 0.0;
    double aperture = 0.0;
    double iso = 0.0;
    double effectiveExposure = 0.0;
};

struct ColorHeaders {
    Eigen::Matrix3f cam2rgb = Eigen::Matrix3f::Identity();
    Eigen::Matrix3f sensor2xyz = Eigen::Matrix3f::Identity();
    std::array<float, 6> primaries{};
    std::array<float, 2> white{};
    std::array<float, 3> luminanceRgb{};
    std::array<float, 3> whiteSaturation{};
    bool raw = false;
    bool xyz = false;
    bool haveXYZCAM = false;
};

struct ResolvedRawLevels {
    int headerBlacklevel = -1;
    int headerWhitepoint = -1;
    int coreBlacklevel = -1;
    int coreWhitepoint = -1;
};

struct CalibrationSampleResult {
    double average = 0.0;
    double fraction = 0.0;
    size_t validSamples = 0;
    size_t totalSamples = 0;
};

struct ConfigValues {
    std::map<std::string, std::string> values;
};

struct VignettingTable {
    std::vector<double> angles;
    std::vector<std::array<double, 3>> factors;
    bool perChannel = false;

    bool empty() const {
        return angles.empty();
    }
};

std::string trimCopy(const std::string &value) {
    return boost::trim_copy(value);
}

std::string lowerCopy(const std::string &value) {
    return boost::to_lower_copy(value);
}

std::string canonicalMergeWeight(const std::string &value) {
    std::string token = lowerCopy(value);
    if (token == "pylinear")
        return "linearhdr";
    return token;
}

std::string canonicalDemosaic(const std::string &value) {
    return lowerCopy(value);
}

bool fileExists(const std::string &path) {
    struct stat sb;
    return stat(path.c_str(), &sb) == 0;
}

std::string lowerExtension(const std::string &path) {
    return lowerCopy(fs::path(path).extension().string());
}

bool isDirectoryPath(const std::string &path) {
    boost::system::error_code ec;
    return fs::is_directory(fs::path(path), ec);
}

bool isRadianceHdrPath(const std::string &path) {
    const std::string ext = lowerExtension(path);
    return ext == ".hdr" || ext == ".pic";
}

bool shadowbandConfigSpecLooksLikePath(const std::string &value) {
    return value.find('/') != std::string::npos ||
           value.find('\\') != std::string::npos ||
           lowerExtension(value) == ".cfg" ||
           fileExists(value);
}

std::string trimTrailingSlashes(const std::string &path) {
    std::string trimmed = path;
    while (trimmed.size() > 1 && (trimmed.back() == '/' || trimmed.back() == '\\'))
        trimmed.pop_back();
    return trimmed;
}

std::string shellQuote(const std::string &value) {
    std::string result = "'";
    for (char ch : value) {
        if (ch == '\'')
            result += "'\\''";
        else
            result += ch;
    }
    result += "'";
    return result;
}

// Quoting for the command lines that are actually run (popen). On POSIX this is shellQuote();
// cmd.exe on Windows needs double quotes with the CommandLineToArgvW escaping rules.
std::string processQuote(const std::string &value) {
#if defined(_WIN32)
    std::string result = "\"";
    size_t backslashes = 0;
    for (char ch : value) {
        if (ch == '\\') {
            ++backslashes;
        } else if (ch == '"') {
            result.append(backslashes * 2 + 1, '\\');
            result += '"';
            backslashes = 0;
            continue;
        } else {
            backslashes = 0;
        }
        result += ch;
    }
    result.append(backslashes, '\\');
    result += '"';
    return result;
#else
    return shellQuote(value);
#endif
}

#if defined(_WIN32)
// cmd /c strips the outer quotes of a command line that starts with a quote, so wrap it once more.
FILE *openProcessPipe(const std::string &command) {
    return _popen(("\"" + command + " 2>&1\"").c_str(), "rb");
}
int closeProcessPipe(FILE *pipe) {
    return _pclose(pipe);
}
#else
FILE *openProcessPipe(const std::string &command) {
    return popen((command + " 2>&1").c_str(), "r");
}
int closeProcessPipe(FILE *pipe) {
    const int status = pclose(pipe);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}
#endif

// Quoting for the command lines that are actually run (popen). On POSIX this is shellQuote();
// cmd.exe on Windows needs double quotes with the CommandLineToArgvW escaping rules.
std::string processQuote(const std::string &value) {
#if defined(_WIN32)
    std::string result = "\"";
    size_t backslashes = 0;
    for (char ch : value) {
        if (ch == '\\') {
            ++backslashes;
        } else if (ch == '"') {
            result.append(backslashes * 2 + 1, '\\');
            result += '"';
            backslashes = 0;
            continue;
        } else {
            backslashes = 0;
        }
        result += ch;
    }
    result.append(backslashes, '\\');
    result += '"';
    return result;
#else
    return shellQuote(value);
#endif
}

#if defined(_WIN32)
// cmd /c strips the outer quotes of a command line that starts with a quote, so wrap it once more.
FILE *openProcessPipe(const std::string &command) {
    return _popen(("\"" + command + " 2>&1\"").c_str(), "rb");
}
int closeProcessPipe(FILE *pipe) {
    return _pclose(pipe);
}
#else
FILE *openProcessPipe(const std::string &command) {
    return popen((command + " 2>&1").c_str(), "r");
}
int closeProcessPipe(FILE *pipe) {
    const int status = pclose(pipe);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}
#endif

std::string getExecutablePath() {
#if defined(__APPLE__)
    uint32_t len = PATH_MAX;
    std::vector<char> buffer(len + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &len) != 0) {
        buffer.assign(len + 1, '\0');
        if (_NSGetExecutablePath(buffer.data(), &len) != 0)
            throw std::runtime_error("Unable to determine executable path.");
    }
    char *resolved = realpath(buffer.data(), nullptr);
    if (resolved == nullptr)
        return std::string(buffer.data());
    std::string path(resolved);
    free(resolved);
    return path;
#elif defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const unsigned long count = GetModuleFileNameW(nullptr, &buffer[0], static_cast<unsigned long>(buffer.size()));
    if (count == 0 || count >= buffer.size())
        throw std::runtime_error("Unable to determine executable path.");
    buffer.resize(count);
    return fs::path(buffer).string();
#elif defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const unsigned long count = GetModuleFileNameW(nullptr, &buffer[0], static_cast<unsigned long>(buffer.size()));
    if (count == 0 || count >= buffer.size())
        throw std::runtime_error("Unable to determine executable path.");
    buffer.resize(count);
    return fs::path(buffer).string();
#else
    char buffer[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", buffer, PATH_MAX);
    if (count <= 0)
        throw std::runtime_error("Unable to determine executable path.");
    buffer[count] = '\0';
    return std::string(buffer);
#endif
}

std::string profileDirectory() {
    const char *env = std::getenv("MERGEHDR_PROFILE_DIR");
    if (env && fileExists(env))
        return std::string(env);

    fs::path exe = fs::canonical(fs::path(getExecutablePath()));
    std::vector<fs::path> candidates = {
        exe.parent_path().parent_path() / "profiles",
        exe.parent_path().parent_path().parent_path() / "mergehdr" / "profiles"
    };

    for (size_t i = 0; i < candidates.size(); ++i) {
        if (fs::exists(candidates[i]))
            return candidates[i].string();
    }

    throw std::runtime_error("Unable to locate mergehdr profiles.");
}

std::string resolveVignettingFilePath(const std::string &value,
        const std::string &configPath = std::string()) {
    if (value.empty())
        return std::string();

    const fs::path requested(value);
    std::set<std::string> seen;
    auto resolveCandidate = [&](const fs::path &candidate) -> std::string {
        boost::system::error_code ec;
        const fs::path absolute = fs::absolute(candidate, ec);
        const std::string key = ec ? candidate.string() : absolute.string();
        if (!seen.insert(key).second)
            return std::string();
        if (!fs::is_regular_file(candidate, ec) || ec)
            return std::string();
        return fs::canonical(candidate).string();
    };

    std::string resolved = resolveCandidate(requested);
    if (!resolved.empty())
        return resolved;

    if (requested.is_relative() && !configPath.empty()) {
        resolved = resolveCandidate(fs::path(configPath).parent_path() / requested);
        if (!resolved.empty())
            return resolved;
    }

    if (requested.is_relative()) {
        try {
            resolved = resolveCandidate(fs::path(profileDirectory()) / requested);
            if (!resolved.empty())
                return resolved;
        } catch (const std::exception &) {
            // The explicitly supplied path can still be diagnosed without a profiles directory.
        }
    }

    throw std::runtime_error(
        "Vignetting file does not exist: " + value +
        " (checked the given path, the config directory, and the mergehdr profiles directory).");
}

VignettingTable loadVignettingTable(const std::string &path) {
    std::ifstream input(path.c_str());
    if (!input)
        throw std::runtime_error("Unable to open vignetting file: " + path);

    VignettingTable table;
    size_t expectedColumns = 0;
    size_t lineNumber = 0;
    std::string line;
    while (std::getline(input, line)) {
        ++lineNumber;
        const size_t comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);
        line = trimCopy(line);
        if (line.empty())
            continue;

        std::istringstream valuesStream(line);
        std::vector<double> values;
        double value = 0.0;
        while (valuesStream >> value)
            values.push_back(value);
        if (!valuesStream.eof())
            throw std::runtime_error(
                "Invalid numeric value in vignetting file " + path +
                " at line " + std::to_string(lineNumber) + ".");
        if (values.size() != 2 && values.size() != 4)
            throw std::runtime_error(
                "Vignetting file rows must contain angle+factor or angle+R+G+B; " +
                path + " line " + std::to_string(lineNumber) + " has " +
                std::to_string(values.size()) + " columns.");
        if (expectedColumns == 0) {
            expectedColumns = values.size();
            table.perChannel = expectedColumns == 4;
        } else if (values.size() != expectedColumns) {
            throw std::runtime_error(
                "Vignetting file must use a consistent number of columns: " + path + ".");
        }
        for (double entry : values) {
            if (!std::isfinite(entry))
                throw std::runtime_error(
                    "Vignetting file contains a non-finite value: " + path +
                    " line " + std::to_string(lineNumber) + ".");
        }
        if (!table.angles.empty() && !(values[0] > table.angles.back()))
            throw std::runtime_error(
                "Vignetting angles must be strictly increasing: " + path +
                " line " + std::to_string(lineNumber) + ".");

        table.angles.push_back(values[0]);
        if (values.size() == 4) {
            table.factors.push_back({{values[1], values[2], values[3]}});
        } else {
            table.factors.push_back({{values[1], values[1], values[1]}});
        }
    }

    if (table.angles.size() < 2)
        throw std::runtime_error(
            "Vignetting file must contain at least two data rows: " + path);
    return table;
}

std::string coreBinaryPath() {
    fs::path exe = fs::canonical(fs::path(getExecutablePath()));
#if defined(_WIN32)
    fs::path candidate = exe.parent_path() / "mergehdrcore.exe";
#else
#if defined(_WIN32)
    fs::path candidate = exe.parent_path() / "mergehdrcore.exe";
#else
    fs::path candidate = exe.parent_path() / "mergehdrcore";
#endif
#endif
    if (!fs::exists(candidate))
        throw std::runtime_error("Unable to locate mergehdrcore.");
    return candidate.string();
}

std::vector<std::string> availableProfiles() {
    std::vector<std::string> result;
    fs::path dir(profileDirectory());
    if (!fs::exists(dir))
        return result;
    for (fs::directory_iterator it(dir); it != fs::directory_iterator(); ++it) {
        if (!fs::is_regular_file(it->path()) || it->path().extension() != ".cfg")
            continue;
        result.push_back(it->path().stem().string());
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::string joinStrings(const std::vector<std::string> &values, const std::string &sep) {
    std::ostringstream oss;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i)
            oss << sep;
        oss << values[i];
    }
    return oss.str();
}

std::vector<std::string> splitWhitespace(const std::string &value) {
    std::vector<std::string> tokens;
    std::istringstream iss(value);
    std::string token;
    while (iss >> token)
        tokens.push_back(token);
    return tokens;
}

bool tryParseSolarSourceDirectionFromHdrHeader(const std::string &path, std::array<double, 3> &direction) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
        return false;

    std::string line;
    if (!std::getline(input, line))
        return false;
    boost::trim_right_if(line, boost::is_any_of("\r\n"));
    if (line != "#?RADIANCE")
        return false;

    std::string combined;
    while (std::getline(input, line)) {
        boost::trim_right_if(line, boost::is_any_of("\r\n"));
        if (line.empty())
            break;
        if (line[0] == '#')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = trimCopy(line.substr(0, eq));
        if (key != "SOLARSOURCE")
            continue;
        if (!combined.empty())
            combined.push_back(' ');
        combined += trimCopy(line.substr(eq + 1));
    }
    if (combined.empty())
        return false;

    std::vector<double> numbers;
    const char *ptr = combined.c_str();
    while (*ptr != '\0') {
        char *end = nullptr;
        const double value = std::strtod(ptr, &end);
        if (end != ptr) {
            numbers.push_back(value);
            ptr = end;
            continue;
        }
        ++ptr;
    }
    if (numbers.size() < 4)
        return false;

    const size_t n = numbers.size();
    const double outX = numbers[n - 4];
    const double outY = numbers[n - 3];
    const double outZ = numbers[n - 2];
    const double length = std::sqrt(outX * outX + outY * outY + outZ * outZ);
    if (!(length > 0.0))
        return false;

    direction = {{-outX / length, -outY / length, outZ / length}};
    return true;
}

std::vector<std::string> normalizeOptionSpellings(const std::vector<std::string> &args) {
    std::vector<std::string> normalized;
    normalized.reserve(args.size() + 4);
    for (size_t i = 0; i < args.size(); ++i) {
        std::string arg = args[i];
        if (arg == "-ds") {
            normalized.push_back("-d");
            normalized.push_back("-S");
            continue;
        }
        if (arg == "-lf") {
            normalized.push_back("--local-only");
            continue;
        }
        if (arg.size() > 2 && arg[0] == '-' && arg[1] == 'G') {
            normalized.push_back("-G");
            normalized.push_back(arg.substr(2));
            continue;
        }
        if (arg.size() > 2 && arg[0] == '-' && arg[1] == 'Y') {
            normalized.push_back("-Y");
            normalized.push_back(arg.substr(2));
            continue;
        }
        if (arg.size() > 2 && arg[0] == '-' && arg[1] != '-' && std::isalpha(static_cast<unsigned char>(arg[1])))
            arg = "--" + arg.substr(1);

        if ((arg == "--fisheye" || arg == "--correct") && i + 1 < args.size()) {
            std::string next = lowerCopy(args[i + 1]);
            if (next == "true" || next == "1" || next == "yes" || next == "on") {
                normalized.push_back(arg);
                ++i;
                continue;
            }
            if (next == "false" || next == "0" || next == "no" || next == "off") {
                normalized.push_back(arg == "--fisheye" ? "--no-fisheye" : "--no-correct");
                ++i;
                continue;
            }
        }

        normalized.push_back(arg);
    }
    return normalized;
}

std::vector<int> parseIntList(const std::string &value) {
    std::vector<int> result;
    for (const std::string &token : splitWhitespace(value))
        result.push_back(std::stoi(token));
    return result;
}

std::vector<float> parseFloatList(const std::string &value) {
    std::vector<float> result;
    for (const std::string &token : splitWhitespace(value))
        result.push_back(std::stof(token));
    return result;
}

std::vector<double> parseDoubleList(const std::string &value) {
    std::vector<double> result;
    for (const std::string &token : splitWhitespace(value))
        result.push_back(std::stod(token));
    return result;
}

std::vector<std::pair<float, float>> parseFoPairs(const std::string &value) {
    std::vector<std::pair<float, float>> pairs;
    for (const std::string &token : splitWhitespace(value)) {
        size_t comma = token.find(',');
        if (comma == std::string::npos)
            throw std::runtime_error("Expected aperture override pairs in the form nominal,exact.");
        pairs.push_back(std::make_pair(
            std::stof(token.substr(0, comma)),
            std::stof(token.substr(comma + 1))));
    }
    return pairs;
}

bool parseBoolValue(const std::string &value) {
    std::string lowered = lowerCopy(trimCopy(value));
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on")
        return true;
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off")
        return false;
    throw std::runtime_error("Could not parse boolean value: " + value);
}

ConfigValues loadConfigValues(const std::string &path) {
    ConfigValues cfg;
    if (path.empty())
        return cfg;

    std::ifstream input(path.c_str());
    if (!input)
        throw std::runtime_error("Unable to open config file: " + path);

    bool inGlobals = true;
    std::string line;
    while (std::getline(input, line)) {
        std::string trimmed = trimCopy(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            std::string section = lowerCopy(trimmed.substr(1, trimmed.size() - 2));
            inGlobals = (section == "globals");
            continue;
        }
        if (!inGlobals)
            continue;
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = lowerCopy(trimCopy(trimmed.substr(0, eq)));
        std::string value = trimCopy(trimmed.substr(eq + 1));
        cfg.values[key] = value;
    }
    return cfg;
}

std::string configPathFromGlobal(const GlobalOptions &global) {
    if (!global.configPath.empty())
        return global.configPath;
    if (global.profileName.empty())
        return std::string();
    fs::path path = fs::path(profileDirectory()) / (global.profileName + ".cfg");
    if (!fs::exists(path))
        throw std::runtime_error("Unknown profile: " + global.profileName);
    return path.string();
}

bool hasWildcard(const std::string &value) {
    return value.find('*') != std::string::npos ||
           value.find('?') != std::string::npos ||
           value.find('[') != std::string::npos;
}

std::vector<std::string> expandPrintfPattern(const std::string &pattern) {
    std::vector<std::string> results;
    char buffer[4096];
    for (int index = 0; ; ++index) {
        std::snprintf(buffer, sizeof(buffer), pattern.c_str(), index);
        if (!fileExists(buffer))
            break;
        results.push_back(buffer);
        if (pattern.find('%') == std::string::npos)
            break;
    }
    if (results.empty()) {
        for (int index = 1; ; ++index) {
            std::snprintf(buffer, sizeof(buffer), pattern.c_str(), index);
            if (!fileExists(buffer))
                break;
            results.push_back(buffer);
        }
    }
    return results;
}

#if defined(_WIN32)
// fnmatch-style match of * and ? (case-insensitive, as Windows file names are).
bool wildcardMatch(const std::string &pattern, const std::string &name) {
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    const auto same = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || same(pattern[p], name[n]))) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}
#endif

#if defined(_WIN32)
// fnmatch-style match of * and ? (case-insensitive, as Windows file names are).
bool wildcardMatch(const std::string &pattern, const std::string &name) {
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    const auto same = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || same(pattern[p], name[n]))) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}
#endif

std::vector<std::string> expandInputArg(const std::string &arg) {
    if (arg.find('%') != std::string::npos) {
        std::vector<std::string> expanded = expandPrintfPattern(arg);
        if (!expanded.empty())
            return expanded;
    }

#if defined(_WIN32)
    if (hasWildcard(arg)) {
        // No glob() on Windows: match * and ? in the file-name part, sorted like glob().
        const fs::path pattern(arg);
        const fs::path directory = pattern.has_parent_path() ? pattern.parent_path() : fs::path(".");
        const std::string namePattern = pattern.filename().string();
        std::vector<std::string> expanded;
        if (fs::is_directory(directory)) {
            for (const fs::directory_entry &entry : fs::directory_iterator(directory)) {
                const std::string name = entry.path().filename().string();
                if (wildcardMatch(namePattern, name))
                    expanded.push_back(pattern.has_parent_path() ? (directory / name).string() : name);
            }
        }
        if (expanded.empty())
            throw std::runtime_error("Pattern did not match any files: " + arg);
        std::sort(expanded.begin(), expanded.end());
        return expanded;
    }
#else
#if defined(_WIN32)
    if (hasWildcard(arg)) {
        // No glob() on Windows: match * and ? in the file-name part, sorted like glob().
        const fs::path pattern(arg);
        const fs::path directory = pattern.has_parent_path() ? pattern.parent_path() : fs::path(".");
        const std::string namePattern = pattern.filename().string();
        std::vector<std::string> expanded;
        if (fs::is_directory(directory)) {
            for (const fs::directory_entry &entry : fs::directory_iterator(directory)) {
                const std::string name = entry.path().filename().string();
                if (wildcardMatch(namePattern, name))
                    expanded.push_back(pattern.has_parent_path() ? (directory / name).string() : name);
            }
        }
        if (expanded.empty())
            throw std::runtime_error("Pattern did not match any files: " + arg);
        std::sort(expanded.begin(), expanded.end());
        return expanded;
    }
#else
    if (hasWildcard(arg)) {
        glob_t matches;
        std::memset(&matches, 0, sizeof(matches));
        int rc = glob(arg.c_str(), 0, nullptr, &matches);
        if (rc != 0) {
            globfree(&matches);
            throw std::runtime_error("Pattern did not match any files: " + arg);
        }
        std::vector<std::string> expanded;
        for (size_t i = 0; i < matches.gl_pathc; ++i)
            expanded.push_back(matches.gl_pathv[i]);
        globfree(&matches);
        return expanded;
    }
#endif

    return std::vector<std::string>(1, arg);
}

std::vector<std::string> expandInputs(const std::vector<std::string> &inputs) {
    std::vector<std::string> expanded;
    for (const std::string &arg : inputs) {
        std::vector<std::string> part = expandInputArg(arg);
        expanded.insert(expanded.end(), part.begin(), part.end());
    }
    if (expanded.empty())
        throw std::runtime_error("No input files found.");
    return expanded;
}

double roundToNearestThirdStop(double seconds) {
    if (!(seconds > 0.0))
        return seconds;
    return std::pow(2.0, std::round(std::log2(seconds) * 3.0) / 3.0);
}

bool approxEqual(double a, double b) {
    double denom = std::max(std::abs(a), 1e-6);
    return std::abs(a - b) / denom < 1e-5;
}

double correctedAperture(double aperture, const std::vector<std::pair<float, float>> &overrides) {
    for (const auto &pair : overrides) {
        if (approxEqual(pair.first, aperture))
            return pair.second;
    }
    if (!std::isfinite(aperture) || aperture <= 0.0)
        return 1.0;
    return std::pow(2.0, std::round(std::log2(aperture * aperture) * 3.0) / 6.0);
}

std::string exifTimeString(const Exiv2::ExifData &exifData) {
    const char *keys[] = {
        "Exif.Photo.DateTimeOriginal",
        "Exif.Photo.DateTimeDigitized",
        "Exif.Image.DateTime"
    };
    for (const char *key : keys) {
        Exiv2::ExifData::const_iterator it = exifData.findKey(Exiv2::ExifKey(key));
        if (it != exifData.end())
            return trimCopy(it->toString());
    }
    return std::string();
}

FrameInfo readFrameInfo(const std::string &path, bool correct, double shutterc,
                        const std::vector<std::pair<float, float>> &fo) {
    Exiv2::Image::UniquePtr image = Exiv2::ImageFactory::open(path);
    if (image.get() == nullptr)
        throw std::runtime_error("Could not open RAW file: " + path);
    image->readMetadata();
    const Exiv2::ExifData &exif = image->exifData();

    Exiv2::ExifData::const_iterator exposureIt = Exiv2::exposureTime(exif);
    if (exposureIt == exif.end())
        throw std::runtime_error("Could not read exposure time from: " + path);
    Exiv2::ExifData::const_iterator isoIt = Exiv2::isoSpeed(exif);
    if (isoIt == exif.end())
        throw std::runtime_error("Could not read ISO from: " + path);
    Exiv2::ExifData::const_iterator apertureIt = Exiv2::fNumber(exif);
    if (apertureIt == exif.end())
        throw std::runtime_error("Could not read aperture from: " + path);

    FrameInfo frame;
    frame.filename = path;
    frame.basename = fs::path(path).filename().string();
    frame.captureTime = exifTimeString(exif);
    frame.shownExposure = exposureIt->toFloat();
    frame.exposureTime = frame.shownExposure;
    frame.iso = isoIt->toFloat();
    frame.aperture = apertureIt->toFloat();

    if (correct) {
        frame.exposureTime = roundToNearestThirdStop(frame.exposureTime);
        if (shutterc != 0.0 && frame.exposureTime > 0.0) {
            double shutterRecip = 1.0 / frame.exposureTime;
            shutterRecip *= std::exp(shutterc * shutterRecip);
            frame.exposureTime = 1.0 / shutterRecip;
        }
        frame.aperture = correctedAperture(frame.aperture, fo);
    } else if (!std::isfinite(frame.aperture) || frame.aperture <= 0.0) {
        frame.aperture = 1.0;
    }

    frame.shutterRecip = 1.0 / frame.exposureTime;
    return frame;
}

struct ColorSpaceSpec {
    bool raw = false;
    bool xyz = false;
    std::array<float, 6> primaries{};
    std::array<float, 2> white{};
};

ColorSpaceSpec parseColorSpaceSpec(const std::string &value) {
    std::string lowered = lowerCopy(value);
    ColorSpaceSpec spec;
    if (lowered == "raw" || lowered == "native") {
        spec.raw = true;
        return spec;
    }
    if (lowered == "xyz") {
        spec.xyz = true;
        spec.primaries = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
        spec.white = { 0.33333333f, 0.33333333f };
        return spec;
    }
    if (lowered == "rad") {
        spec.primaries = { 0.640f, 0.330f, 0.290f, 0.600f, 0.150f, 0.060f };
        spec.white = { 0.3333f, 0.3333f };
        return spec;
    }
    if (lowered == "srgb") {
        spec.primaries = { 0.6400f, 0.3300f, 0.3000f, 0.6000f, 0.1500f, 0.0600f };
        spec.white = { 0.3127f, 0.3290f };
        return spec;
    }
    throw std::runtime_error("Unsupported colorspace: " + value);
}

Eigen::Matrix3f primariesToXYZ(const ColorSpaceSpec &spec) {
    Eigen::Matrix3f pxyz;
    for (int c = 0; c < 3; ++c) {
        float x = spec.primaries[2 * c + 0];
        float y = spec.primaries[2 * c + 1];
        float z = 1.0f - x - y;
        pxyz(0, c) = x;
        pxyz(1, c) = y;
        pxyz(2, c) = z;
    }

    Eigen::Vector3f wxyz;
    wxyz(0) = spec.white[0] / spec.white[1];
    wxyz(1) = 1.0f;
    wxyz(2) = (1.0f - spec.white[0] - spec.white[1]) / spec.white[1];

    Eigen::Vector3f scales = pxyz.inverse() * wxyz;
    Eigen::Matrix3f rgbXYZ = pxyz;
    for (int c = 0; c < 3; ++c)
        rgbXYZ.col(c) *= scales(c);
    return rgbXYZ;
}

ColorHeaders buildColorHeaders(const std::string &colorspace, const std::vector<float> &xyzcamInput,
                               const std::vector<float> &rawMultipliers,
                               const std::vector<float> &cscale) {
    ColorHeaders out;
    ColorSpaceSpec spec = parseColorSpaceSpec(colorspace);
    out.raw = spec.raw;
    out.xyz = spec.xyz;

    Eigen::Matrix3f xyzcam = Eigen::Matrix3f::Identity();
    if (xyzcamInput.size() >= 9) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                xyzcam(i, j) = xyzcamInput[3 * i + j];
        out.haveXYZCAM = true;
    }

    if (out.haveXYZCAM && !rawMultipliers.empty()) {
        for (int i = 0; i < 3 && i < static_cast<int>(rawMultipliers.size()); ++i)
            xyzcam.row(i) *= rawMultipliers[i];
    }

    if (out.haveXYZCAM)
        out.sensor2xyz = xyzcam.inverse();

    if (spec.raw) {
        out.cam2rgb = Eigen::Matrix3f::Identity();
        return out;
    }

    out.primaries = spec.primaries;
    out.white = spec.white;

    if (spec.xyz) {
        out.cam2rgb = out.sensor2xyz;
        out.luminanceRgb = { 0.0f, 1.0f, 0.0f };
        out.whiteSaturation = { 1.0f, 1.0f, 1.0f };
    } else {
        Eigen::Matrix3f rgbXYZ = primariesToXYZ(spec);
        Eigen::Matrix3f rgbCam = xyzcam * rgbXYZ;
        out.cam2rgb = rgbCam.inverse();
        for (int i = 0; i < 3; ++i)
            out.luminanceRgb[i] = rgbXYZ(1, i);
        Eigen::Vector3f sums = rgbCam.rowwise().sum();
        float maxSum = sums.maxCoeff();
        for (int i = 0; i < 3; ++i)
            out.whiteSaturation[i] = maxSum > 0.0f ? sums(i) / maxSum : 1.0f;
    }

    if (!cscale.empty()) {
        Eigen::Matrix3f rgbcal = Eigen::Matrix3f::Identity();
        for (int i = 0; i < 3 && i < static_cast<int>(cscale.size()); ++i)
            rgbcal(i, i) = cscale[i];
        out.cam2rgb = rgbcal * out.cam2rgb;
    }

    return out;
}

std::string formatMatrixHeader(const std::string &name, const Eigen::Matrix3f &matrix) {
    std::ostringstream oss;
    oss << name << "= ";
    oss << std::fixed << std::setprecision(8);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (i != 0 || j != 0)
                oss << ' ';
            oss << matrix(i, j);
        }
    }
    return oss.str();
}

std::string formatTripletHeader(const std::string &name, const std::array<float, 3> &values, int precision = 8) {
    std::ostringstream oss;
    oss << name << "= " << std::fixed << std::setprecision(precision)
        << values[0] << ' ' << values[1] << ' ' << values[2];
    return oss.str();
}

std::string formatPairHeader(const std::string &name, const std::array<float, 2> &values, int precision = 4) {
    std::ostringstream oss;
    oss << name << "= " << std::fixed << std::setprecision(precision)
        << values[0] << ' ' << values[1];
    return oss.str();
}

std::string formatPrimariesHeader(const std::array<float, 6> &primaries) {
    std::ostringstream oss;
    oss << "TargetPrimaries= " << std::fixed << std::setprecision(4);
    for (int i = 0; i < 6; ++i) {
        if (i)
            oss << ' ';
        oss << primaries[i];
    }
    return oss.str();
}

std::string currentCapDate() {
    std::time_t now = std::time(nullptr);
    std::tm tmNow;
#if defined(_WIN32)
    localtime_s(&tmNow, &now);
#else
    localtime_r(&now, &tmNow);
#endif
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y:%m:%d %H:%M:%S", &tmNow);
    return buffer;
}

std::string sequenceCapDate(const std::vector<FrameInfo> &frames) {
    std::vector<std::string> times;
    for (const FrameInfo &frame : frames) {
        if (!frame.captureTime.empty())
            times.push_back(frame.captureTime);
    }
    if (times.empty())
        return std::string();
    std::sort(times.begin(), times.end());
    std::string stop = times.back();
    size_t space = stop.rfind(' ');
    if (space != std::string::npos)
        stop = stop.substr(space + 1);
    return times.front() + "-" + stop;
}

std::vector<std::string> rawImageLines(const std::vector<FrameInfo> &frames, size_t perLine = 10) {
    std::vector<std::string> lines;
    for (size_t start = 0; start < frames.size(); start += perLine) {
        std::ostringstream oss;
        oss << "RAW_IMAGES= ";
        for (size_t i = start; i < std::min(start + perLine, frames.size()); ++i) {
            if (i != start)
                oss << ' ';
            oss << frames[i].basename;
        }
        lines.push_back(oss.str());
    }
    return lines;
}

std::vector<std::string> dedupeLines(const std::vector<std::string> &lines) {
    std::set<std::string> seen;
    std::vector<std::string> result;
    for (const std::string &line : lines) {
        std::string trimmed = trimCopy(line);
        if (trimmed.empty() || seen.count(trimmed))
            continue;
        seen.insert(trimmed);
        result.push_back(trimmed);
    }
    return result;
}

std::vector<std::string> headerLinesForWrite(const HdrImage &image) {
    if (!image.headerLines.empty()) {
        std::vector<std::string> lines;
        for (const std::string &line : image.headerLines) {
            const std::string trimmed = trimCopy(line);
            const size_t eq = trimmed.find('=');
            const std::string key = lowerCopy(trimCopy(trimmed.substr(0, eq)));
            if (key == "format")
                continue;
            lines.push_back(line);
        }
        return lines;
    }

    std::vector<std::string> lines;
    for (const auto &entry : image.header) {
        if (boost::iequals(entry.first, "FORMAT"))
            continue;
        lines.push_back(entry.first + "= " + entry.second);
    }
    return lines;
}

int nextMergeHdrStepIndex(const std::vector<std::string> &headerLines) {
    std::regex stepPattern(R"(^\s*MERGEHDR_STEP_(\d+)\s*=)");
    int maxStep = 1;
    for (const std::string &line : headerLines) {
        std::smatch match;
        if (!std::regex_search(line, match, stepPattern))
            continue;
        maxStep = std::max(maxStep, std::stoi(match[1].str()));
    }
    return maxStep + 1;
}

HdrImage flipHdrRows(const HdrImage &image) {
    HdrImage flipped = image;
    flipped.rgb.assign(image.rgb.size(), 0.0f);
    for (size_t y = 0; y < image.height; ++y) {
        const size_t srcY = image.height - 1 - y;
        const size_t srcIndex = (srcY * image.width) * 3;
        const size_t dstIndex = (y * image.width) * 3;
        std::copy_n(image.rgb.begin() + static_cast<std::ptrdiff_t>(srcIndex),
                    static_cast<std::ptrdiff_t>(image.width * 3),
                    flipped.rgb.begin() + static_cast<std::ptrdiff_t>(dstIndex));
    }
    return flipped;
}

HdrImage readRadianceHDRTopDown(const std::string &path) {
    return flipHdrRows(readRadianceHDR(path));
}

HdrImage cropHdrImageTopLeftTopDown(const HdrImage &image, int left, int top, int width, int height) {
    if (left < 0 || top < 0 || width <= 0 || height <= 0)
        throw std::runtime_error("Crop expects positive bounds inside the HDR image.");
    if (static_cast<size_t>(left + width) > image.width || static_cast<size_t>(top + height) > image.height)
        throw std::runtime_error("Crop falls outside the HDR image.");

    HdrImage cropped;
    cropped.width = static_cast<size_t>(width);
    cropped.height = static_cast<size_t>(height);
    cropped.header = image.header;
    cropped.headerLines = image.headerLines;
    cropped.rgb.assign(cropped.width * cropped.height * 3, 0.0f);

    for (int y = 0; y < height; ++y) {
        const size_t srcY = static_cast<size_t>(top + y);
        const size_t dstY = static_cast<size_t>(y);
        const size_t srcIndex = (srcY * image.width + static_cast<size_t>(left)) * 3;
        const size_t dstIndex = (dstY * cropped.width) * 3;
        std::copy_n(image.rgb.begin() + static_cast<std::ptrdiff_t>(srcIndex),
                    static_cast<std::ptrdiff_t>(cropped.width * 3),
                    cropped.rgb.begin() + static_cast<std::ptrdiff_t>(dstIndex));
    }

    return cropped;
}

HdrImage rotateHdrImageTopDown(const HdrImage &image, double angleDegrees) {
    HdrImage rotated;
    rotated.width = image.width;
    rotated.height = image.height;
    rotated.header = image.header;
    rotated.headerLines = image.headerLines;
    rotated.rgb.assign(image.rgb.size(), 0.0f);

    if (image.width == 0 || image.height == 0)
        return rotated;

    const double radians = angleDegrees * std::acos(-1.0) / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const double centerX = 0.5 * static_cast<double>(image.width - 1);
    const double centerY = 0.5 * static_cast<double>(image.height - 1);

    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(image.height); ++y) {
        for (size_t x = 0; x < image.width; ++x) {
            const double outputX = static_cast<double>(x) - centerX;
            const double outputY = static_cast<double>(y) - centerY;

            // Inverse-map the output pixel into the top-down input image. In
            // image coordinates (Y points down), positive angles remain
            // counter-clockwise and negative angles rotate clockwise.
            const double sourceX = cosine * outputX - sine * outputY + centerX;
            const double sourceY = sine * outputX + cosine * outputY + centerY;
            if (sourceX < 0.0 || sourceY < 0.0 ||
                    sourceX > static_cast<double>(image.width - 1) ||
                    sourceY > static_cast<double>(image.height - 1)) {
                continue;
            }

            const size_t x0 = static_cast<size_t>(std::floor(sourceX));
            const size_t y0 = static_cast<size_t>(std::floor(sourceY));
            const size_t x1 = std::min(x0 + 1, image.width - 1);
            const size_t y1 = std::min(y0 + 1, image.height - 1);
            const double weightX = sourceX - static_cast<double>(x0);
            const double weightY = sourceY - static_cast<double>(y0);
            const size_t outputIndex = (static_cast<size_t>(y) * image.width + x) * 3;

            for (size_t channel = 0; channel < 3; ++channel) {
                const double topLeft = image.rgb[(y0 * image.width + x0) * 3 + channel];
                const double topRight = image.rgb[(y0 * image.width + x1) * 3 + channel];
                const double bottomLeft = image.rgb[(y1 * image.width + x0) * 3 + channel];
                const double bottomRight = image.rgb[(y1 * image.width + x1) * 3 + channel];
                const double top = topLeft + (topRight - topLeft) * weightX;
                const double bottom = bottomLeft + (bottomRight - bottomLeft) * weightX;
                rotated.rgb[outputIndex + channel] =
                    static_cast<float>(top + (bottom - top) * weightY);
            }
        }
    }

    return rotated;
}

std::string headerKeyLower(const std::string &line) {
    const std::string trimmed = trimCopy(line);
    const size_t eq = trimmed.find('=');
    if (eq == std::string::npos)
        return "";
    return lowerCopy(trimCopy(trimmed.substr(0, eq)));
}

std::array<double, 3> interpolateVignettingFactors(
        const VignettingTable &table, double angleDegrees) {
    size_t upperIndex = 1;
    if (angleDegrees <= table.angles.front()) {
        upperIndex = 1;
    } else if (angleDegrees >= table.angles.back()) {
        upperIndex = table.angles.size() - 1;
    } else {
        upperIndex = static_cast<size_t>(
            std::upper_bound(table.angles.begin(), table.angles.end(), angleDegrees) -
            table.angles.begin());
    }
    const size_t lowerIndex = upperIndex - 1;
    const double lowerAngle = table.angles[lowerIndex];
    const double upperAngle = table.angles[upperIndex];
    const double fraction = (angleDegrees - lowerAngle) / (upperAngle - lowerAngle);

    std::array<double, 3> result{};
    for (size_t channel = 0; channel < 3; ++channel) {
        result[channel] =
            table.factors[lowerIndex][channel] * (1.0 - fraction) +
            table.factors[upperIndex][channel] * fraction;
    }
    return result;
}

HdrImage applyVignettingCorrectionPylinearhdr(
        const HdrImage &image, const VignettingTable &table) {
    if (image.width == 0 || image.height == 0)
        throw std::runtime_error("Vignetting correction requires a non-empty HDR image.");
    if (image.width != image.height)
        throw std::runtime_error(
            "pylinearhdr-compatible vignetting correction requires a square final HDR image.");
    if (table.empty())
        throw std::runtime_error("Vignetting correction table is empty.");

    HdrImage corrected = image;
    corrected.rgb.resize(image.rgb.size());
    const double resolution = static_cast<double>(image.width);
    const double center = 0.5 * resolution;
    const double pi = std::acos(-1.0);

    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(image.height); ++y) {
        for (size_t x = 0; x < image.width; ++x) {
            const double dx = static_cast<double>(x) + 0.5 - center;
            const double dy = static_cast<double>(y) + 0.5 - center;
            const double normalizedRadius = std::sqrt(dx * dx + dy * dy) / resolution;
            const double cosine = std::max(
                -1.0, std::min(1.0, std::cos(pi * normalizedRadius)));
            const double angleDegrees = std::acos(cosine) * 180.0 / pi;
            const std::array<double, 3> factors =
                interpolateVignettingFactors(table, angleDegrees);

            const size_t index =
                (static_cast<size_t>(y) * image.width + x) * 3;
            for (size_t channel = 0; channel < 3; ++channel) {
                corrected.rgb[index + channel] = static_cast<float>(
                    static_cast<double>(image.rgb[index + channel]) * factors[channel]);
            }
        }
    }
    return corrected;
}

std::vector<std::string> appendVignettingHeaderLines(
        const std::vector<std::string> &baseLines, const std::string &vfilePath) {
    std::vector<std::string> lines;
    for (const std::string &line : baseLines) {
        const std::string key = headerKeyLower(line);
        if (key == "vignetting_correction" || key == "mergehdr_vignetting_method")
            continue;
        lines.push_back(line);
    }
    const int stepIndex = nextMergeHdrStepIndex(lines);
    lines.push_back("MERGEHDR_STEP_" + std::to_string(stepIndex) +
                    "= pylinearhdr-compatible angle-table vignetting; "
                    "piecewise-linear with endpoint extrapolation; "
                    "destination RGB after lens projection");
    lines.push_back("VIGNETTING_CORRECTION= " + vfilePath);
    lines.push_back("MERGEHDR_VIGNETTING_METHOD= pylinearhdr-compatible 180-degree "
                    "equiangular pixel-center angle; RGB multiplication");
    return dedupeLines(lines);
}

std::vector<std::string> headerLinesReplacing(const HdrImage &image, const std::set<std::string> &replacedKeys) {
    std::vector<std::string> lines;
    for (const std::string &line : headerLinesForWrite(image)) {
        const std::string key = headerKeyLower(line);
        if (replacedKeys.find(key) != replacedKeys.end())
            continue;
        lines.push_back(line);
    }
    return lines;
}

inline void sampleBilinearHdr(const HdrImage &image, double xCenter, double yCenter, float *out) {
    xCenter = std::max(0.5, std::min((double) image.width - 0.5, xCenter));
    yCenter = std::max(0.5, std::min((double) image.height - 0.5, yCenter));

    const double xi = xCenter - 0.5;
    const double yi = yCenter - 0.5;

    const int x0 = (int) std::floor(xi);
    const int y0 = (int) std::floor(yi);
    const int x1 = std::min<int>(x0 + 1, (int) image.width - 1);
    const int y1 = std::min<int>(y0 + 1, (int) image.height - 1);

    const double tx = xi - x0;
    const double ty = yi - y0;

    const float *p00 = image.rgb.data() + (static_cast<size_t>(y0) * image.width + static_cast<size_t>(x0)) * 3;
    const float *p10 = image.rgb.data() + (static_cast<size_t>(y0) * image.width + static_cast<size_t>(x1)) * 3;
    const float *p01 = image.rgb.data() + (static_cast<size_t>(y1) * image.width + static_cast<size_t>(x0)) * 3;
    const float *p11 = image.rgb.data() + (static_cast<size_t>(y1) * image.width + static_cast<size_t>(x1)) * 3;

    for (int channel = 0; channel < 3; ++channel) {
        const double a = p00[channel] * (1.0 - tx) + p10[channel] * tx;
        const double b = p01[channel] * (1.0 - tx) + p11[channel] * tx;
        out[channel] = (float) (a * (1.0 - ty) + b * ty);
    }
}

HdrImage projectHdrImageEquisolidToEquidistant(const HdrImage &image) {
    if (image.width == 0 || image.height == 0)
        throw std::runtime_error("Projection conversion requires a non-empty HDR image.");
    if (image.width != image.height)
        throw std::runtime_error("Projection conversion currently requires a square HDR image.");

    HdrImage projected = image;
    projected.rgb.assign(image.width * image.height * 3, 0.0f);

    const double sqrt2 = std::sqrt(2.0);
    const double pi = std::acos(-1.0);

    #pragma omp parallel for
    for (int y = 0; y < (int) image.height; ++y) {
        for (size_t x = 0; x < image.width; ++x) {
            float *dst = projected.rgb.data() + (static_cast<size_t>(y) * image.width + x) * 3;
            dst[0] = dst[1] = dst[2] = 0.0f;

            const double dx = ((double) x + 0.5) / (double) image.width - 0.5;
            const double dy = ((double) y + 0.5) / (double) image.height - 0.5;
            const double radius = std::sqrt(dx * dx + dy * dy);
            if (radius > 0.5)
                continue;

            double sourceRadius = 0.0;
            if (radius > 1e-12)
                sourceRadius = std::sin(0.5 * pi * radius) / sqrt2;

            const double scale = radius > 1e-12 ? sourceRadius / radius : 1.0;
            const double sourceU = 0.5 + dx * scale;
            const double sourceV = 0.5 + dy * scale;
            sampleBilinearHdr(image, sourceU * image.width, sourceV * image.height, dst);
        }
    }

    return projected;
}

HdrImage projectHdrImageHemisphericalToEquidistant(const HdrImage &image) {
    if (image.width == 0 || image.height == 0)
        throw std::runtime_error("Projection conversion requires a non-empty HDR image.");
    if (image.width != image.height)
        throw std::runtime_error("Projection conversion currently requires a square HDR image.");

    HdrImage projected = image;
    projected.rgb.assign(image.width * image.height * 3, 0.0f);

    const double pi = std::acos(-1.0);

    #pragma omp parallel for
    for (int y = 0; y < (int) image.height; ++y) {
        for (size_t x = 0; x < image.width; ++x) {
            float *dst = projected.rgb.data() + (static_cast<size_t>(y) * image.width + x) * 3;
            dst[0] = dst[1] = dst[2] = 0.0f;

            const double dx = ((double) x + 0.5) / (double) image.width - 0.5;
            const double dy = ((double) y + 0.5) / (double) image.height - 0.5;
            const double radius = std::sqrt(dx * dx + dy * dy);
            if (radius > 0.5)
                continue;

            double sourceRadius = 0.0;
            if (radius > 1e-12)
                sourceRadius = 0.5 * std::sin(pi * radius);

            const double scale = radius > 1e-12 ? sourceRadius / radius : 1.0;
            const double sourceU = 0.5 + dx * scale;
            const double sourceV = 0.5 + dy * scale;
            sampleBilinearHdr(image, sourceU * image.width, sourceV * image.height, dst);
        }
    }

    return projected;
}

void evaluateRadialPolynomial(const std::vector<double> &coefficients, double u,
        double &value, double &derivative) {
    value = 0.0;
    derivative = 0.0;
    double valuePower = u;
    double derivativePower = 1.0;
    for (size_t index = 0; index < coefficients.size(); ++index) {
        value += coefficients[index] * valuePower;
        derivative += static_cast<double>(index + 1) * coefficients[index] * derivativePower;
        valuePower *= u;
        derivativePower *= u;
    }
}

double invertRadialPolynomial4(const std::vector<double> &coefficients, double target) {
    double u = std::max(0.0, std::min(1.0, target));
    for (int iteration = 0; iteration < 4; ++iteration) {
        double value = 0.0;
        double derivative = 0.0;
        evaluateRadialPolynomial(coefficients, u, value, derivative);
        if (std::fabs(derivative) > 1e-14)
            u -= (value - target) / derivative;
        u = std::max(0.0, std::min(1.0, u));
    }
    return u;
}

HdrImage projectHdrImageCalibratedToEquidistant(const HdrImage &image,
        const std::vector<double> &coefficients, double sourceRadius, double targetRadius) {
    if (image.width == 0 || image.height == 0)
        throw std::runtime_error("Calibrated projection requires a non-empty HDR image.");
    if (image.width != image.height)
        throw std::runtime_error("Calibrated projection requires a square HDR image.");
    if (coefficients.size() != 5 || !(sourceRadius > 0.0) || !(targetRadius > 0.0))
        throw std::runtime_error("Calibrated projection parameters are incomplete.");

    HdrImage projected = image;
    projected.rgb.assign(image.width * image.height * 3, 0.0f);
    const double centerX = 0.5 * static_cast<double>(image.width);
    const double centerY = 0.5 * static_cast<double>(image.height);

    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(image.height); ++y) {
        for (size_t x = 0; x < image.width; ++x) {
            float *dst = projected.rgb.data() + (static_cast<size_t>(y) * image.width + x) * 3;
            dst[0] = dst[1] = dst[2] = 0.0f;

            const double dx = static_cast<double>(x) + 0.5 - centerX;
            const double dy = static_cast<double>(y) + 0.5 - centerY;
            const double outputRadius = std::sqrt(dx * dx + dy * dy);
            if (outputRadius > targetRadius)
                continue;

            if (outputRadius <= 1e-12) {
                sampleBilinearHdr(image, centerX, centerY, dst);
                continue;
            }

            const double q = outputRadius / targetRadius;
            const double u = invertRadialPolynomial4(coefficients, q);
            const double inputRadius = sourceRadius * u;
            const double scale = inputRadius / outputRadius;
            sampleBilinearHdr(image, centerX + dx * scale, centerY + dy * scale, dst);
        }
    }

    return projected;
}

double anglePolynomialRadius(const std::vector<double> &coefficients, double angleRadians) {
    double radius = 0.0;
    double derivative = 0.0;
    evaluateRadialPolynomial(coefficients, angleRadians, radius, derivative);
    return radius;
}

HdrImage projectHdrImageAnglePolynomialToEquidistant(const HdrImage &image,
        const std::vector<double> &coefficients, double sourceRadius, double targetRadius) {
    if (image.width == 0 || image.height == 0)
        throw std::runtime_error("Single-fisheye projection requires a non-empty HDR image.");
    if (image.width != image.height)
        throw std::runtime_error("Single-fisheye projection requires a square HDR image.");
    if (coefficients.empty() || !(sourceRadius > 0.0) || !(targetRadius > 0.0))
        throw std::runtime_error("Single-fisheye projection parameters are incomplete.");

    HdrImage projected = image;
    projected.rgb.assign(image.width * image.height * 3, 0.0f);
    const double centerX = 0.5 * static_cast<double>(image.width);
    const double centerY = 0.5 * static_cast<double>(image.height);
    const double halfPi = 0.5 * std::acos(-1.0);

    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(image.height); ++y) {
        for (size_t x = 0; x < image.width; ++x) {
            float *dst = projected.rgb.data() + (static_cast<size_t>(y) * image.width + x) * 3;
            dst[0] = dst[1] = dst[2] = 0.0f;

            const double dx = static_cast<double>(x) + 0.5 - centerX;
            const double dy = static_cast<double>(y) + 0.5 - centerY;
            const double outputRadius = std::sqrt(dx * dx + dy * dy);
            if (outputRadius >= targetRadius)
                continue;

            if (outputRadius <= 1e-12) {
                sampleBilinearHdr(image, centerX, centerY, dst);
                continue;
            }

            const double fieldAngle = halfPi * outputRadius / targetRadius;
            const double normalizedSourceRadius = anglePolynomialRadius(coefficients, fieldAngle);
            if (!(normalizedSourceRadius >= 0.0) || normalizedSourceRadius >= 1.0)
                continue;

            const double inputRadius = sourceRadius * normalizedSourceRadius;
            const double scale = inputRadius / outputRadius;
            sampleBilinearHdr(image, centerX + dx * scale, centerY + dy * scale, dst);
        }
    }

    return projected;
}

double anglePolynomialValidHalfAngle(const std::vector<double> &coefficients) {
    const double halfPi = 0.5 * std::acos(-1.0);
    if (anglePolynomialRadius(coefficients, halfPi) <= 1.0)
        return halfPi;

    double low = 0.0;
    double high = halfPi;
    for (int iteration = 0; iteration < 80; ++iteration) {
        const double mid = 0.5 * (low + high);
        if (anglePolynomialRadius(coefficients, mid) < 1.0)
            low = mid;
        else
            high = mid;
    }
    return 0.5 * (low + high);
}

std::string formatDoubleList(const std::vector<double> &values) {
    std::ostringstream oss;
    oss << std::setprecision(15);
    for (size_t index = 0; index < values.size(); ++index) {
        if (index)
            oss << ' ';
        if (values[index] >= 0.0)
            oss << '+';
        oss << values[index];
    }
    return oss.str();
}

std::vector<std::string> buildDualFisheyeHeaderLines(const HdrImage &fullImage,
        const RunOptions &opts, const std::vector<int> &crop, const std::string &eye) {
    std::vector<std::string> lines = headerLinesReplacing(fullImage, {
        "mergehdr_crop",
        "mergehdr_crop_source",
        "mergehdr_crop_source_size",
        "mergehdr_dual_fisheye",
        "mergehdr_dual_fisheye_eye",
        "mergehdr_projection",
        "mergehdr_projection_source",
        "mergehdr_projection_target",
        "mergehdr_projection_input",
        "mergehdr_projection_source_size",
        "mergehdr_projection_method",
        "mergehdr_projection_source_radius_px",
        "mergehdr_projection_target_radius_px",
        "mergehdr_projection_forward_variable",
        "mergehdr_projection_forward_coefficients",
        "mergehdr_projection_forward_order",
        "mergehdr_projection_inverse",
        "mergehdr_projection_resampling",
        "mergehdr_projection_outside_180",
        "view"
    });

    const int stepIndex = nextMergeHdrStepIndex(lines);
    std::ostringstream step;
    step << "calibrated dual-fisheye " << eye << " crop "
         << crop[0] << ' ' << crop[1] << ' ' << crop[2] << ' ' << crop[3]
         << "; inverse polynomial; one-pass bilinear";
    lines.push_back("MERGEHDR_STEP_" + std::to_string(stepIndex) + "= " + step.str());
    lines.push_back("MERGEHDR_DUAL_FISHEYE= True");
    lines.push_back("MERGEHDR_DUAL_FISHEYE_EYE= " + eye);
    lines.push_back("MERGEHDR_CROP= " + std::to_string(crop[0]) + " " + std::to_string(crop[1]) +
                    " " + std::to_string(crop[2]) + " " + std::to_string(crop[3]));
    lines.push_back("MERGEHDR_CROP_SOURCE= full_merged_dual_fisheye");
    lines.push_back("MERGEHDR_CROP_SOURCE_SIZE= " + std::to_string(fullImage.width) + " " +
                    std::to_string(fullImage.height));
    lines.push_back("MERGEHDR_PROJECTION= equidistant");
    lines.push_back("MERGEHDR_PROJECTION_SOURCE= measured_radial_fisheye_polynomial");
    lines.push_back("MERGEHDR_PROJECTION_TARGET= equidistant");
    lines.push_back("MERGEHDR_PROJECTION_INPUT= full_merged_dual_fisheye");
    lines.push_back("MERGEHDR_PROJECTION_SOURCE_SIZE= " + std::to_string(crop[2]) + " " +
                    std::to_string(crop[3]));
    lines.push_back("MERGEHDR_PROJECTION_METHOD= calibrated inverse radial map; Newton-Raphson 4 iterations; one-pass bilinear");

    std::ostringstream sourceRadius;
    sourceRadius << std::setprecision(15) << opts.projectionSourceRadius;
    std::ostringstream targetRadius;
    targetRadius << std::setprecision(15) << opts.projectionTargetRadius;
    lines.push_back("MERGEHDR_PROJECTION_SOURCE_RADIUS_PX= " + sourceRadius.str());
    lines.push_back("MERGEHDR_PROJECTION_TARGET_RADIUS_PX= " + targetRadius.str());
    lines.push_back("MERGEHDR_PROJECTION_FORWARD_VARIABLE= u=r_lens/source_radius; q=r_equidistant/target_radius");
    lines.push_back("MERGEHDR_PROJECTION_FORWARD_COEFFICIENTS= " +
                    formatDoubleList(opts.projectionCoefficients));
    lines.push_back("MERGEHDR_PROJECTION_FORWARD_ORDER= q=c1*u+c2*u^2+c3*u^3+c4*u^4+c5*u^5");
    lines.push_back("MERGEHDR_PROJECTION_INVERSE= Newton-Raphson 4 iterations; monotonic P(u)");
    lines.push_back("MERGEHDR_PROJECTION_RESAMPLING= bilinear; one pass");
    lines.push_back("MERGEHDR_PROJECTION_OUTSIDE_180= RGB 0 0 0");
    const std::string viewDirection = eye == "right" ? "0.0 -1.0 0.0" : "0.0 1.0 0.0";
    lines.push_back("VIEW= -vta -vv 180 -vh 180 -vd " + viewDirection +
                    " -vp 0 0 0 -vu 0 0 1");
    return dedupeLines(lines);
}

std::vector<std::string> buildSingleFisheyeHeaderLines(const HdrImage &source,
        const RunOptions &opts) {
    std::vector<std::string> lines = headerLinesReplacing(source, {
        "mergehdr_projection",
        "mergehdr_projection_source",
        "mergehdr_projection_target",
        "mergehdr_projection_input",
        "mergehdr_projection_source_size",
        "mergehdr_projection_method",
        "mergehdr_projection_reference_cal",
        "mergehdr_projection_source_radius_px",
        "mergehdr_projection_target_radius_px",
        "mergehdr_projection_forward_variable",
        "mergehdr_projection_forward_coefficients",
        "mergehdr_projection_forward_order",
        "mergehdr_projection_endpoint_p_pi_over_2",
        "mergehdr_projection_literal_valid_half_angle_deg",
        "mergehdr_projection_literal_valid_fov_deg",
        "mergehdr_projection_literal_valid_output_radius_px",
        "mergehdr_projection_resampling",
        "mergehdr_projection_outside_valid_source",
        "view"
    });

    const int stepIndex = nextMergeHdrStepIndex(lines);
    lines.push_back("MERGEHDR_STEP_" + std::to_string(stepIndex) +
                    "= native Paul Bourke angle-polynomial fisheye to 180-degree equidistant; one-pass bilinear");
    lines.push_back("MERGEHDR_PROJECTION= equidistant");
    lines.push_back("MERGEHDR_PROJECTION_SOURCE= Paul Bourke empirical Canon EF8-15mm fisheye at 8mm");
    lines.push_back("MERGEHDR_PROJECTION_TARGET= equidistant 180 degree");
    lines.push_back("MERGEHDR_PROJECTION_INPUT= cropped merged RAW bracket");
    lines.push_back("MERGEHDR_PROJECTION_SOURCE_SIZE= " + std::to_string(source.width) + " " +
                    std::to_string(source.height));
    lines.push_back("MERGEHDR_PROJECTION_METHOD= native angle polynomial; direct radial map; one-pass bilinear");
    if (!opts.projectionReferenceCal.empty())
        lines.push_back("MERGEHDR_PROJECTION_REFERENCE_CAL= " + opts.projectionReferenceCal);

    std::ostringstream sourceRadius;
    sourceRadius << std::setprecision(15) << opts.projectionSourceRadius;
    std::ostringstream targetRadius;
    targetRadius << std::setprecision(15) << opts.projectionTargetRadius;
    lines.push_back("MERGEHDR_PROJECTION_SOURCE_RADIUS_PX= " + sourceRadius.str());
    lines.push_back("MERGEHDR_PROJECTION_TARGET_RADIUS_PX= " + targetRadius.str());
    lines.push_back("MERGEHDR_PROJECTION_FORWARD_VARIABLE= phi=(pi/2)*(r_out/target_radius); rho=r_source/source_radius");
    lines.push_back("MERGEHDR_PROJECTION_FORWARD_COEFFICIENTS= " +
                    formatDoubleList(opts.projectionCoefficients));
    lines.push_back("MERGEHDR_PROJECTION_FORWARD_ORDER= rho=c1*phi+c2*phi^2+c3*phi^3+c4*phi^4; phi in radians");

    const double pi = std::acos(-1.0);
    const double endpoint = anglePolynomialRadius(opts.projectionCoefficients, 0.5 * pi);
    const double validHalfAngle = anglePolynomialValidHalfAngle(opts.projectionCoefficients);
    const double validHalfDegrees = validHalfAngle * 180.0 / pi;
    const double validOutputRadius = opts.projectionTargetRadius * validHalfAngle / (0.5 * pi);
    std::ostringstream endpointText;
    endpointText << std::setprecision(15) << endpoint;
    std::ostringstream validHalfText;
    validHalfText << std::setprecision(15) << validHalfDegrees;
    std::ostringstream validFovText;
    validFovText << std::setprecision(15) << 2.0 * validHalfDegrees;
    std::ostringstream validRadiusText;
    validRadiusText << std::setprecision(15) << validOutputRadius;
    lines.push_back("MERGEHDR_PROJECTION_ENDPOINT_P_PI_OVER_2= " + endpointText.str());
    lines.push_back("MERGEHDR_PROJECTION_LITERAL_VALID_HALF_ANGLE_DEG= " + validHalfText.str());
    lines.push_back("MERGEHDR_PROJECTION_LITERAL_VALID_FOV_DEG= " + validFovText.str());
    lines.push_back("MERGEHDR_PROJECTION_LITERAL_VALID_OUTPUT_RADIUS_PX= " + validRadiusText.str());
    lines.push_back("MERGEHDR_PROJECTION_RESAMPLING= native one-pass bilinear");
    lines.push_back("MERGEHDR_PROJECTION_OUTSIDE_VALID_SOURCE= RGB 0 0 0");
    lines.push_back("VIEW= " + opts.projectionView);
    return dedupeLines(lines);
}

std::pair<fs::path, fs::path> dualFisheyeOutputPaths(const RunOptions &opts,
        const std::string &firstInputPath) {
    fs::path parent;
    std::string stem;
    if (opts.outputOverride) {
        const fs::path requested(opts.outputPath);
        parent = requested.has_parent_path() ? requested.parent_path() : fs::current_path();
        stem = requested.has_extension() ? requested.stem().string() : requested.filename().string();
    } else {
        parent = fs::current_path();
        stem = firstInputPath.empty() ? std::string("merged") : fs::path(firstInputPath).stem().string();
    }
    if (stem.empty())
        stem = "merged";
    if (!fs::exists(parent))
        throw std::runtime_error("dual_fisheye output directory does not exist: " + parent.string());
    if (!fs::is_directory(parent))
        throw std::runtime_error("dual_fisheye output parent is not a directory: " + parent.string());
    return std::make_pair(parent / (stem + "_left.hdr"), parent / (stem + "_right.hdr"));
}

std::string buildHdrCropCommandLine(const GlobalOptions &global, const HdrCropOptions &opts) {
    std::vector<std::string> parts;
    parts.push_back("mergehdr");
    if (!global.profileName.empty()) {
        parts.push_back("-profile");
        parts.push_back(global.profileName);
    } else if (!global.configPath.empty()) {
        parts.push_back("-config");
        parts.push_back(shellQuote(global.configPath));
    }
    parts.push_back("hdrcrop");
    parts.push_back("-crop");
    for (int value : opts.crop)
        parts.push_back(std::to_string(value));
    parts.push_back("-output");
    parts.push_back(shellQuote(opts.outputPath));
    parts.push_back(shellQuote(opts.inputPath));
    return joinStrings(parts, " ");
}

std::vector<std::string> buildHdrCropHeaderLines(const HdrImage &source, const HdrCropOptions &opts,
        const std::string &commandLine) {
    std::vector<std::string> lines = headerLinesForWrite(source);
    const int stepIndex = nextMergeHdrStepIndex(lines);
    lines.push_back("MERGEHDR_STEP_" + std::to_string(stepIndex) + "= " + commandLine);
    lines.push_back("MERGEHDR_CROP= " + std::to_string(opts.crop[0]) + " " + std::to_string(opts.crop[1]) +
                    " " + std::to_string(opts.crop[2]) + " " + std::to_string(opts.crop[3]));
    lines.push_back("MERGEHDR_CROP_SOURCE= " + fs::path(opts.inputPath).filename().string());
    lines.push_back("MERGEHDR_CROP_SOURCE_SIZE= " + std::to_string(source.width) + " " + std::to_string(source.height));
    return lines;
}

std::string buildHdrRotateCommandLine(const GlobalOptions &global, const HdrRotateOptions &opts) {
    std::vector<std::string> parts;
    parts.push_back("mergehdr");
    if (!global.profileName.empty()) {
        parts.push_back("-profile");
        parts.push_back(global.profileName);
    } else if (!global.configPath.empty()) {
        parts.push_back("-config");
        parts.push_back(shellQuote(global.configPath));
    }
    parts.push_back("rotate");
    parts.push_back("-angle");
    std::ostringstream angle;
    angle << std::setprecision(15) << opts.angleDegrees;
    parts.push_back(angle.str());
    parts.push_back("-output");
    parts.push_back(shellQuote(opts.outputPath));
    parts.push_back(shellQuote(opts.inputPath));
    return joinStrings(parts, " ");
}

std::vector<std::string> buildHdrRotateHeaderLines(const HdrImage &source,
        const HdrRotateOptions &opts, const std::string &commandLine) {
    std::vector<std::string> lines = headerLinesForWrite(source);
    const int stepIndex = nextMergeHdrStepIndex(lines);
    lines.push_back("MERGEHDR_STEP_" + std::to_string(stepIndex) + "= " + commandLine);
    std::ostringstream angle;
    angle << std::setprecision(15) << opts.angleDegrees;
    lines.push_back("MERGEHDR_ROTATION_DEGREES= " + angle.str());
    lines.push_back("MERGEHDR_ROTATION_DIRECTION= positive counter-clockwise; negative clockwise");
    lines.push_back("MERGEHDR_ROTATION_RESAMPLING= native one-pass bilinear");
    lines.push_back("MERGEHDR_ROTATION_CANVAS= fixed " + std::to_string(source.width) + " " +
                    std::to_string(source.height));
    lines.push_back("MERGEHDR_ROTATION_OUTSIDE_SOURCE= RGB 0 0 0");
    return lines;
}

std::string buildHdrProjectCommandLine(const GlobalOptions &global, const HdrProjectOptions &opts) {
    std::vector<std::string> parts;
    parts.push_back("mergehdr");
    if (!global.profileName.empty()) {
        parts.push_back("-profile");
        parts.push_back(global.profileName);
    } else if (!global.configPath.empty()) {
        parts.push_back("-config");
        parts.push_back(shellQuote(global.configPath));
    }
    parts.push_back("convertprojection");
    if (opts.equisolid)
        parts.push_back("--equisolid");
    if (opts.hemispherical)
        parts.push_back("--hemispherical");
    if (opts.equidistant)
        parts.push_back("--equidistant");
    if (!opts.polynomialCoefficients.empty()) {
        parts.push_back("-p");
        for (double coefficient : opts.polynomialCoefficients) {
            std::ostringstream value;
            value << std::setprecision(15) << coefficient;
            parts.push_back(value.str());
        }
        parts.push_back("--source-radius");
        parts.push_back((boost::format("%1$.15g") % opts.sourceRadius).str());
        parts.push_back("--target-radius");
        parts.push_back((boost::format("%1$.15g") % opts.targetRadius).str());
        parts.push_back("--view");
        parts.push_back(shellQuote(opts.projectionView));
    }
    parts.push_back("-output");
    parts.push_back(shellQuote(opts.outputPath));
    parts.push_back(shellQuote(opts.inputPath));
    return joinStrings(parts, " ");
}

std::vector<std::string> buildHdrProjectHeaderLines(const HdrImage &source, const HdrProjectOptions &opts,
        const std::string &commandLine) {
    std::vector<std::string> lines = headerLinesReplacing(source, {
        "mergehdr_projection",
        "mergehdr_projection_source",
        "mergehdr_projection_target",
        "mergehdr_projection_input",
        "mergehdr_projection_source_size",
        "mergehdr_projection_method",
        "mergehdr_projection_source_radius_px",
        "mergehdr_projection_target_radius_px",
        "mergehdr_projection_forward_variable",
        "mergehdr_projection_forward_coefficients",
        "mergehdr_projection_forward_order",
        "mergehdr_projection_endpoint_p_pi_over_2",
        "mergehdr_projection_literal_valid_half_angle_deg",
        "mergehdr_projection_literal_valid_fov_deg",
        "mergehdr_projection_literal_valid_output_radius_px",
        "mergehdr_projection_resampling",
        "mergehdr_projection_outside_valid_source",
        "view"
    });
    const int stepIndex = nextMergeHdrStepIndex(lines);
    lines.push_back("MERGEHDR_STEP_" + std::to_string(stepIndex) + "= " + commandLine);
    lines.push_back("MERGEHDR_PROJECTION= equidistant");
    lines.push_back("MERGEHDR_PROJECTION_TARGET= equidistant");
    lines.push_back("MERGEHDR_PROJECTION_INPUT= " + fs::path(opts.inputPath).filename().string());
    lines.push_back("MERGEHDR_PROJECTION_SOURCE_SIZE= " + std::to_string(source.width) + " " + std::to_string(source.height));
    if (opts.polynomialCoefficients.empty()) {
        if (opts.hemispherical) {
            lines.push_back("MERGEHDR_PROJECTION_SOURCE= Radiance hemispherical fisheye (-vth)");
            lines.push_back("MERGEHDR_PROJECTION_METHOD= vth2ang; native inverse radial map; one-pass bilinear");
            lines.push_back("MERGEHDR_PROJECTION_FORWARD_ORDER= r_source=0.5*sin(pi*r_output)");
        } else {
            lines.push_back("MERGEHDR_PROJECTION_SOURCE= equisolid");
            lines.push_back("MERGEHDR_PROJECTION_METHOD= solid2ang");
        }
        if (opts.equidistant)
            lines.push_back("VIEW= -vta -vv 180 -vh 180 -vd 0.0 1.0 0.0 -vp 0 0 0 -vu 0 0 1");
        return lines;
    }

    lines.push_back("MERGEHDR_PROJECTION_SOURCE= empirical angle-polynomial fisheye");
    lines.push_back("MERGEHDR_PROJECTION_METHOD= native angle polynomial; direct radial map; one-pass bilinear");
    lines.push_back("MERGEHDR_PROJECTION_SOURCE_RADIUS_PX= " +
                    (boost::format("%1$.15g") % opts.sourceRadius).str());
    lines.push_back("MERGEHDR_PROJECTION_TARGET_RADIUS_PX= " +
                    (boost::format("%1$.15g") % opts.targetRadius).str());
    lines.push_back("MERGEHDR_PROJECTION_FORWARD_VARIABLE= phi=(pi/2)*(r_out/target_radius); rho=r_source/source_radius");
    lines.push_back("MERGEHDR_PROJECTION_FORWARD_COEFFICIENTS= " +
                    formatDoubleList(opts.polynomialCoefficients));
    lines.push_back("MERGEHDR_PROJECTION_FORWARD_ORDER= rho=c1*phi+c2*phi^2+c3*phi^3+c4*phi^4; phi in radians");

    const double pi = std::acos(-1.0);
    const double endpoint = anglePolynomialRadius(opts.polynomialCoefficients, 0.5 * pi);
    const double validHalfAngle = anglePolynomialValidHalfAngle(opts.polynomialCoefficients);
    const double validHalfDegrees = validHalfAngle * 180.0 / pi;
    const double validOutputRadius = opts.targetRadius * validHalfAngle / (0.5 * pi);
    lines.push_back("MERGEHDR_PROJECTION_ENDPOINT_P_PI_OVER_2= " +
                    (boost::format("%1$.15g") % endpoint).str());
    lines.push_back("MERGEHDR_PROJECTION_LITERAL_VALID_HALF_ANGLE_DEG= " +
                    (boost::format("%1$.15g") % validHalfDegrees).str());
    lines.push_back("MERGEHDR_PROJECTION_LITERAL_VALID_FOV_DEG= " +
                    (boost::format("%1$.15g") % (2.0 * validHalfDegrees)).str());
    lines.push_back("MERGEHDR_PROJECTION_LITERAL_VALID_OUTPUT_RADIUS_PX= " +
                    (boost::format("%1$.15g") % validOutputRadius).str());
    lines.push_back("MERGEHDR_PROJECTION_RESAMPLING= native one-pass bilinear");
    lines.push_back("MERGEHDR_PROJECTION_OUTSIDE_VALID_SOURCE= RGB 0 0 0");
    lines.push_back("VIEW= " + opts.projectionView);
    return lines;
}

double effectiveExposure(const FrameInfo &frame, double scaleTotal) {
    return (frame.exposureTime * frame.iso) / (100.0 * frame.aperture * frame.aperture * scaleTotal);
}

void emitRangeStats(const std::vector<FrameInfo> &frames, double saturationOffset, double rangeValue,
                    const ColorHeaders *colorHeaders, const std::vector<float> &cscale) {
    if (colorHeaders == nullptr || colorHeaders->raw)
        return;

    Eigen::Vector3f rgbScale = Eigen::Vector3f::Ones();
    for (int i = 0; i < 3 && i < static_cast<int>(cscale.size()); ++i)
        rgbScale(i) = cscale[i];

    double calcfac = 0.0;
    for (int idx = 0; idx < 3; ++idx)
        calcfac += colorHeaders->luminanceRgb[idx] * rgbScale(idx) * colorHeaders->cam2rgb.row(idx).sum();

    double floor = std::max(rangeValue, 0.005);
    double globalMin = std::numeric_limits<double>::infinity();
    double globalMax = 0.0;

    for (size_t index = 0; index < frames.size(); ++index) {
        const FrameInfo &frame = frames[index];
        if (!(std::isfinite(frame.effectiveExposure) && frame.effectiveExposure > 0.0))
            throw std::runtime_error(
                "Cannot report a calibrated range for a non-positive effective exposure.");
        double factor = 1.0 / frame.effectiveExposure;
        double fmax = factor * (1.0 - saturationOffset) * calcfac;
        double fmin = fmax * floor;
        globalMin = std::min(globalMin, fmin);
        globalMax = std::max(globalMax, fmax);
        std::cerr << "[" << (index + 1) << "/" << frames.size() << "] frame #" << (index + 1)
                  << ", min:" << std::setprecision(6) << fmin
                  << ", max:" << std::setprecision(6) << fmax << std::endl;
    }
    std::cerr << "using " << frames.size() << " frames, range min:" << std::setprecision(5)
              << globalMin << ", max:" << std::setprecision(5) << globalMax << std::endl;
}

std::vector<std::string> buildHeaderLines(const GlobalOptions &global, const RunOptions &opts,
                                          const std::vector<FrameInfo> &frames,
                                          const ColorHeaders *colorHeaders,
                                          const std::vector<float> &rawMultipliers,
                                          const std::vector<float> &cscale,
                                          int effectiveBlacklevel,
                                          int effectiveWhitepoint,
                                          const std::string &coreStep,
                                          const std::string &commandLine) {
    std::vector<std::string> lines;
    lines.push_back(std::string("MERGEHDR_VERSION= ") + MERGEHDR_VERSION);
    if (!global.profileName.empty())
        lines.push_back("MERGEHDR_PROFILE= " + global.profileName);
    lines.push_back("MERGEHDR_COMMAND= " + commandLine);
    lines.push_back("MERGEHDR_STEP_1= " + coreStep);

    std::ostringstream exposures;
    exposures << "MERGEHDR_EXPTIMES= ";
    for (size_t i = 0; i < frames.size(); ++i) {
        if (i)
            exposures << ',';
        exposures << std::setprecision(12) << frames[i].effectiveExposure;
    }
    lines.push_back(exposures.str());
    if (effectiveBlacklevel >= 0)
        lines.push_back("MERGEHDR_BLACKLEVEL= " + std::to_string(effectiveBlacklevel));
    if (effectiveWhitepoint >= 0)
        lines.push_back("MERGEHDR_WHITEPOINT= " + std::to_string(effectiveWhitepoint));

    if (colorHeaders && colorHeaders->haveXYZCAM) {
        lines.push_back(formatMatrixHeader("SENSOR2XYZ", colorHeaders->sensor2xyz));
        Eigen::Matrix3f xyzcam = colorHeaders->sensor2xyz.inverse();
        lines.push_back(formatMatrixHeader("XYZCAM", xyzcam));
    }

    std::array<float, 3> premul = { 1.0f, 1.0f, 1.0f };
    for (int i = 0; i < 3 && i < static_cast<int>(rawMultipliers.size()); ++i)
        premul[i] = rawMultipliers[i];
    lines.push_back(formatTripletHeader("CAM_PREMULTIPLIERS", premul));
    lines.push_back("CAPDATE= " + currentCapDate());

    std::vector<std::string> rawLines = rawImageLines(frames);
    lines.insert(lines.end(), rawLines.begin(), rawLines.end());

    if (colorHeaders) {
        lines.push_back(formatMatrixHeader("Camera2RGB", colorHeaders->cam2rgb));
        if (!colorHeaders->raw) {
            lines.push_back(formatPrimariesHeader(colorHeaders->primaries));
            lines.push_back(formatPairHeader("TargetWhitePoint", colorHeaders->white));
            lines.push_back(formatTripletHeader("LuminanceRGB", colorHeaders->luminanceRgb));
            lines.push_back(formatTripletHeader("WhiteSaturation", colorHeaders->whiteSaturation));
        }
    }

    std::string seq = sequenceCapDate(frames);
    if (!seq.empty())
        lines.push_back("SEQUENCECAPDATE= " + seq);
    lines.push_back("HDR_SEQUENCE_COUNT= " + std::to_string(frames.size()));

    if (colorHeaders && !colorHeaders->raw) {
        Eigen::Vector3f rgbScale = Eigen::Vector3f::Ones();
        for (int i = 0; i < 3 && i < static_cast<int>(cscale.size()); ++i)
            rgbScale(i) = cscale[i];
        double calcfac = 0.0;
        for (int idx = 0; idx < 3; ++idx)
            calcfac += colorHeaders->luminanceRgb[idx] * rgbScale(idx) * colorHeaders->cam2rgb.row(idx).sum();
        double floor = std::max(opts.rangeValue, 0.005);
        double globalMin = std::numeric_limits<double>::infinity();
        double globalMax = 0.0;
        for (const FrameInfo &frame : frames) {
            double factor = opts.scale * std::pow(10.0, opts.nd) * 100.0 * frame.aperture * frame.aperture * frame.shutterRecip / frame.iso;
            double fmax = factor * (1.0 - opts.saturation) * calcfac;
            double fmin = fmax * floor;
            globalMin = std::min(globalMin, fmin);
            globalMax = std::max(globalMax, fmax);
        }
        std::ostringstream rangeLine;
        rangeLine << "HDR_VALID_RANGE= " << std::setprecision(5) << globalMin << "-" << globalMax << " cd/m^2";
        lines.push_back(rangeLine.str());
    }

    if (opts.fisheye)
        lines.push_back("VIEW= -vta -vv 180 -vh 180 -vd 0.0 1.0 0.0 -vp 0 0 0 -vu 0 0 1");

    return dedupeLines(lines);
}

std::string buildCommandLine(const GlobalOptions &global, const RunOptions &opts) {
    std::vector<std::string> parts;
    parts.push_back("mergehdr");
    if (!global.profileName.empty()) {
        parts.push_back("-profile");
        parts.push_back(global.profileName);
    } else if (!global.configPath.empty()) {
        parts.push_back("-config");
        parts.push_back(shellQuote(global.configPath));
    }
    parts.push_back("run");
    parts.push_back("-colorspace");
    parts.push_back(opts.colorspace);
    if (opts.rawgrid) {
        parts.push_back("--rawgrid");
    } else {
        parts.push_back("--demosaic");
        parts.push_back(canonicalDemosaic(opts.demosaic));
    }
    parts.push_back("--merge-weight");
    parts.push_back(canonicalMergeWeight(opts.mergeWeight));
    parts.push_back("-saturation");
    parts.push_back((boost::format("%1$g") % opts.saturation).str());
    parts.push_back("-range");
    parts.push_back((boost::format("%1$g") % opts.rangeValue).str());
    parts.push_back("-scale");
    parts.push_back((boost::format("%1$g") % opts.scale).str());
    parts.push_back("-nd");
    parts.push_back((boost::format("%1$g") % opts.nd).str());
    parts.push_back(opts.fisheye ? "--fisheye" : "--no-fisheye");
    if (!opts.correct)
        parts.push_back("--no-correct");
    if (!opts.crop.empty()) {
        parts.push_back("-crop");
        for (int value : opts.crop)
            parts.push_back(std::to_string(value));
    }
    if (opts.blackOverride) {
        parts.push_back("-black");
        parts.push_back(std::to_string(opts.blacklevel));
    }
    if (opts.whiteOverride) {
        parts.push_back("-white");
        parts.push_back(std::to_string(opts.whitepoint));
    }
    if (opts.xyzcamOverride) {
        parts.push_back("-xyzcam");
        parts.push_back(shellQuote(opts.xyzcamText));
    }
    if (opts.badpixelsOverride) {
        parts.push_back("-badpixels");
        parts.push_back(shellQuote(opts.badpixelsText));
    }
    if (opts.shuttercOverride) {
        parts.push_back("-shutterc");
        parts.push_back((boost::format("%1$.16g") % opts.shutterc).str());
    }
    if (opts.foOverride) {
        parts.push_back("-fo");
        parts.push_back(shellQuote(opts.foText));
    }
    if (opts.cscaleOverride) {
        parts.push_back("-cscale");
        parts.push_back(shellQuote(opts.cscaleText));
    }
    if (opts.rawMultipliersOverride) {
        parts.push_back("-rawmultipliers");
        parts.push_back(shellQuote(opts.rawMultipliersText));
    }
    if (!opts.vfilePath.empty()) {
        parts.push_back("-vfile");
        parts.push_back(shellQuote(opts.vfilePath));
    }
    parts.push_back("<RAW_IMAGES> > output.hdr");
    return joinStrings(parts, " ");
}

std::string buildCoreStep(const RunOptions &opts, const std::vector<float> &xyzcamValues,
                          const std::vector<float> &rawMultipliers, const std::vector<float> &cscale,
                          const ResolvedRawLevels &levels) {
    std::vector<std::string> parts;
    parts.push_back("mergehdrcore");
    if (opts.rawgrid) {
        parts.push_back("--rawgrid");
    } else {
        parts.push_back("--demosaic");
        parts.push_back(canonicalDemosaic(opts.demosaic));
    }
    parts.push_back("--mergestyle");
    parts.push_back(canonicalMergeWeight(opts.mergeWeight));
    parts.push_back("--format");
    parts.push_back("hdr");
    parts.push_back("--colormode");
    parts.push_back("native");
    parts.push_back("--colorspace");
    parts.push_back(opts.colorspace);
    if (opts.fisheye)
        parts.push_back("--solid2ang");
    parts.push_back("--linearhdrexposure");
    parts.push_back("--exposurescale");
    parts.push_back((boost::format("%1$.12g") % (opts.scale * std::pow(10.0, opts.nd))).str());
    parts.push_back("--saturation");
    parts.push_back((boost::format("%1$.12g") % std::max(0.5, std::min(0.999999, 1.0 - opts.saturation))).str());
    parts.push_back("--range");
    parts.push_back((boost::format("%1$.12g") % opts.rangeValue).str());
    if (!opts.correct)
        parts.push_back("--nominal");
    if (!opts.crop.empty()) {
        parts.push_back("--mergecrop");
        parts.push_back((boost::format("%1%,%2%,%3%,%4%") % opts.crop[0] % opts.crop[1] % opts.crop[2] % opts.crop[3]).str());
    }
    if (levels.coreBlacklevel >= 0) {
        parts.push_back("--blacklevel");
        parts.push_back(std::to_string(levels.coreBlacklevel));
    }
    if (levels.coreWhitepoint >= 0) {
        parts.push_back("--whitepoint");
        parts.push_back(std::to_string(levels.coreWhitepoint));
    }
    if (!xyzcamValues.empty()) {
        std::ostringstream xyzText;
        xyzText << std::setprecision(12);
        for (size_t i = 0; i < xyzcamValues.size(); ++i) {
            if (i)
                xyzText << ' ';
            xyzText << xyzcamValues[i];
        }
        parts.push_back("--xyzcam");
        parts.push_back(xyzText.str());
    }
    if (opts.badpixelsOverride) {
        parts.push_back("--badpixels");
        parts.push_back(opts.badpixelsText);
    }
    if (!rawMultipliers.empty()) {
        std::ostringstream multText;
        multText << std::setprecision(12);
        for (size_t i = 0; i < rawMultipliers.size(); ++i) {
            if (i)
                multText << ' ';
            multText << rawMultipliers[i];
        }
        parts.push_back("--rawmultipliers");
        parts.push_back(multText.str());
    }
    if (!cscale.empty()) {
        std::ostringstream rgbText;
        rgbText << std::setprecision(12);
        for (size_t i = 0; i < cscale.size(); ++i) {
            if (i)
                rgbText << ' ';
            rgbText << cscale[i];
        }
        parts.push_back("--rgbcal");
        parts.push_back(rgbText.str());
    }
    if (opts.shuttercOverride) {
        parts.push_back("--shutterc");
        parts.push_back((boost::format("%1$.12g") % opts.shutterc).str());
    }
    if (opts.foOverride) {
        parts.push_back("--fo");
        parts.push_back(opts.foText);
    }
    parts.push_back("<RAW_IMAGES>");
    return joinStrings(parts, " ");
}

void copyBinaryFileToStream(const std::string &path, std::ostream &out) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
        throw std::runtime_error("Unable to read output file: " + path);
    const size_t chunkSize = 1 << 20;
    std::vector<char> buffer(chunkSize);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize got = input.gcount();
        if (got > 0)
            out.write(buffer.data(), got);
    }
}

bool shouldSuppressCoreLine(const std::string &line) {
    static const std::vector<std::regex> patterns = {
        std::regex(R"(^Found \d+ images)"),
        std::regex(R"(^Collected \d+ metadata entries)"),
        std::regex(R"(^Loading raw image data )"),
        std::regex(R"(^Pre-cropping RAW mosaic )"),
        std::regex(R"(^Merging \d+ exposures )"),
        std::regex(R"(^AHD demosaicing )"),
        std::regex(R"(^DHT demosaicing )"),
        std::regex(R"(^Transforming to .* color space )"),
        std::regex(R"(^Transforming to target RGB color space )"),
        std::regex(R"(^Cropping to )"),
        std::regex(R"(^Writing /)"),
        std::regex(R"(^Converting equisolid fisheye to equiangular )"),
        std::regex(R"(^Warning: image ".*" was \*not\* taken in manual )"),
        std::regex(R"(^Overriding black level:)"),
        std::regex(R"(^Overriding white point:)"),
        std::regex(R"(^Overriding exposure times:)")
    };
    for (const std::regex &pattern : patterns) {
        if (std::regex_search(line, pattern))
            return true;
    }
    return false;
}

std::vector<std::string> plannedCoreStages(const RunOptions &opts) {
    std::vector<std::string> stages;
    stages.push_back("load raw");
    if (!opts.crop.empty())
        stages.push_back("crop raw");
    stages.push_back("merge");
    stages.push_back("demosaic + color");
    if (opts.fisheye)
        stages.push_back("fisheye");
    stages.push_back("write hdr");
    return stages;
}

std::string formatProgressBar(size_t done, size_t total) {
    const size_t width = 28;
    size_t filled = total ? (done * width) / total : 0;
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < width; ++i)
        oss << (i < filled ? "#" : "-");
    oss << "]";
    return oss.str();
}

void printStageProgress(size_t stageIndex, size_t totalStages, const std::string &stageName,
        const std::chrono::steady_clock::time_point &startTime, bool interactive, bool &initialized) {
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
    int percent = totalStages ? (int) std::lround(100.0 * (stageIndex + 1) / totalStages) : 0;
    std::ostringstream barLine;
    barLine << formatProgressBar(stageIndex + 1, totalStages) << " " << percent << "%  "
            << std::fixed << std::setprecision(1) << elapsed << "s";
    std::string stageLine = "stage: " + stageName;

    if (interactive) {
        if (initialized)
            std::cerr << "\033[2A";
        std::cerr << "\r\033[2K" << barLine.str() << "\n";
        std::cerr << "\r\033[2K" << stageLine << "\n";
        std::cerr.flush();
        initialized = true;
        return;
    }

    std::cerr << barLine.str() << "  " << stageLine << std::endl;
}

int matchCoreStage(const std::string &line, const RunOptions &opts) {
    if (boost::starts_with(line, "Loading raw image data "))
        return 0;

    int stage = 1;
    if (!opts.crop.empty()) {
        if (boost::starts_with(line, "Pre-cropping RAW mosaic "))
            return stage;
        ++stage;
    }

    if (boost::starts_with(line, "Merging "))
        return stage;
    ++stage;

    if (boost::starts_with(line, "AHD demosaicing ") ||
        boost::starts_with(line, "DHT demosaicing ") ||
        boost::starts_with(line, "Transforming to "))
        return stage;
    ++stage;

    if (opts.fisheye) {
        if (boost::starts_with(line, "Converting equisolid fisheye to equiangular "))
            return stage;
        ++stage;
    }

    if (boost::starts_with(line, "Writing "))
        return stage;

    return -1;
}

int runCoreCommand(const std::string &command, const RunOptions &opts, std::vector<std::string> &captured) {
    FILE *pipe = openProcessPipe(command);
    if (!pipe)
        throw std::runtime_error("Unable to launch mergehdrcore.");

    std::vector<std::string> stages = plannedCoreStages(opts);
    std::set<int> emittedStages;
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
#if defined(_WIN32)
    bool interactive = _isatty(_fileno(stderr)) != 0;
#else
#if defined(_WIN32)
    bool interactive = _isatty(_fileno(stderr)) != 0;
#else
    bool interactive = isatty(STDERR_FILENO);
#endif
#endif
    bool progressInitialized = false;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        captured.push_back(line);

        int stageIndex = matchCoreStage(line, opts);
        if (stageIndex >= 0 && stageIndex < static_cast<int>(stages.size()) &&
                emittedStages.insert(stageIndex).second) {
            printStageProgress(stageIndex, stages.size(), stages[stageIndex],
                startTime, interactive, progressInitialized);
        }
    }
    const int rc = closeProcessPipe(pipe);
    if (interactive && progressInitialized)
        std::cerr.flush();
    return rc;
}

std::string temporaryDirectory() {
#if defined(_WIN32)
    for (int attempt = 0; attempt < 100; ++attempt) {
        const fs::path candidate = fs::temp_directory_path() / fs::unique_path("mergehdr-%%%%-%%%%-%%%%");
        boost::system::error_code ec;
        if (fs::create_directory(candidate, ec) && !ec)
            return candidate.string();
    }
    throw std::runtime_error("Unable to create a temporary directory.");
#else
    std::string templ = "/tmp/mergehdrXXXXXX";
    std::vector<char> buffer(templ.begin(), templ.end());
    buffer.push_back('\0');
    char *dir = mkdtemp(buffer.data());
    if (!dir)
        throw std::runtime_error("Unable to create a temporary directory.");
    return dir;
#endif
}

void removeTree(const fs::path &path) {
    if (fs::exists(path))
        fs::remove_all(path);
}

std::string defaultViewVisibilityOutputPath(const std::string &testPath) {
    fs::path path(testPath);
    const std::string stem = path.stem().string().empty() ? std::string("view_visibility_result") : path.stem().string();
    const fs::path dir = path.has_parent_path() ? path.parent_path() : fs::current_path();
    return (dir / (stem + "_view_visibility_result.hdr")).string();
}

std::string defaultPerceptualMapOutputPath(const std::string &inputPath, const std::string &mapName) {
    fs::path path(inputPath);
    const std::string stem = path.stem().string().empty() ? std::string("perceptual_map") : path.stem().string();
    const fs::path dir = path.has_parent_path() ? path.parent_path() : fs::current_path();
    return (dir / (stem + "_" + lowerCopy(mapName) + ".hdr")).string();
}

std::string makeImplicitWhiteMask(const std::string &referencePath) {
    const HdrImage reference = readRadianceHDR(referencePath, false, false);
    if (reference.width == 0 || reference.height == 0)
        throw std::runtime_error("Reference HDR is empty, so an implicit mask can not be created.");

    const std::string tmpdir = temporaryDirectory();
    const fs::path tmpPath(tmpdir);
    const fs::path maskPath = tmpPath / "implicit_view_visibility_mask.hdr";

    HdrImage mask;
    mask.width = reference.width;
    mask.height = reference.height;
    mask.rgb.assign(mask.width * mask.height * 3, 1.0f);
    writeRadianceHDR(maskPath.string(), mask, std::vector<std::string>(), false, true);
    return maskPath.string();
}

struct ExtractVec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct ExtractViewBasis {
    ExtractVec3d vdir{0.0, 1.0, 0.0};
    ExtractVec3d vup{0.0, 0.0, 1.0};
    ExtractVec3d hv;
    ExtractVec3d vv;
};

struct ExtractProjectionMetadata {
    bool available = false;
    double horizontalViewDegrees = 180.0;
    double verticalViewDegrees = 180.0;
};

struct ExtractLuminanceInfo {
    enum Mode {
        Unavailable,
        Weighted,
        Monochrome
    };

    Mode mode = Unavailable;
    std::array<double, 3> weights{{0.0, 0.0, 0.0}};
    double exposureScale = 1.0;
};

struct ExtractMat3d {
    double m[3][3] = {
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
        {0.0, 0.0, 1.0},
    };
};

struct ExtractGeometrySample {
    ExtractVec3d dir;
    double omega = 0.0;
    bool valid = false;
};

struct ExtractPixelSample {
    int x = 0;
    int y = 0;
    double omega = 0.0;
    double luminance = 0.0;
    std::array<double, 3> rgb{{0.0, 0.0, 0.0}};
};

struct ExtractSeriesAggregate {
    double omegaSum = 0.0;
    double luminanceSum = 0.0;
    std::array<double, 3> rgbSum{{0.0, 0.0, 0.0}};
    size_t count = 0;
};

double extractClampDot(double value) {
    return std::max(-1.0, std::min(1.0, value));
}

ExtractVec3d extractVecAdd(const ExtractVec3d &a, const ExtractVec3d &b) {
    ExtractVec3d result;
    result.x = a.x + b.x;
    result.y = a.y + b.y;
    result.z = a.z + b.z;
    return result;
}

ExtractVec3d extractVecMul(const ExtractVec3d &value, double scalar) {
    ExtractVec3d result;
    result.x = value.x * scalar;
    result.y = value.y * scalar;
    result.z = value.z * scalar;
    return result;
}

double extractDot(const ExtractVec3d &a, const ExtractVec3d &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

ExtractVec3d extractCross(const ExtractVec3d &a, const ExtractVec3d &b) {
    ExtractVec3d result;
    result.x = a.y * b.z - a.z * b.y;
    result.y = a.z * b.x - a.x * b.z;
    result.z = a.x * b.y - a.y * b.x;
    return result;
}

double extractNorm(const ExtractVec3d &value) {
    return std::sqrt(extractDot(value, value));
}

ExtractVec3d extractNormalize(const ExtractVec3d &value) {
    const double length = extractNorm(value);
    if (length <= 0.0)
        return ExtractVec3d();
    return extractVecMul(value, 1.0 / length);
}

void extractFinalizeViewBasis(ExtractViewBasis &view) {
    view.vdir = extractNormalize(view.vdir);
    view.vup = extractNormalize(view.vup);
    view.hv = extractNormalize(extractCross(view.vdir, view.vup));
    if (extractNorm(view.hv) <= 0.0)
        view.hv = ExtractVec3d{1.0, 0.0, 0.0};
    view.vv = extractNormalize(extractCross(view.hv, view.vdir));
    if (extractNorm(view.vv) <= 0.0)
        view.vv = ExtractVec3d{0.0, 0.0, 1.0};
}

std::vector<double> extractParseDoubleList(const std::string &value) {
    std::vector<double> result;
    std::istringstream iss(value);
    double parsed = 0.0;
    while (iss >> parsed)
        result.push_back(parsed);
    return result;
}

bool extractParseHeaderTriplet(const HdrImage &image, const std::string &key, std::array<double, 3> &values) {
    std::map<std::string, std::string>::const_iterator it = image.header.find(key);
    if (it == image.header.end())
        return false;
    const std::vector<double> parsed = extractParseDoubleList(it->second);
    if (parsed.size() != 3)
        return false;
    values[0] = parsed[0];
    values[1] = parsed[1];
    values[2] = parsed[2];
    return true;
}

bool extractParseHeaderMatrix3x3(const HdrImage &image, const std::string &key, ExtractMat3d &matrix) {
    std::map<std::string, std::string>::const_iterator it = image.header.find(key);
    if (it == image.header.end())
        return false;
    const std::vector<double> parsed = extractParseDoubleList(it->second);
    if (parsed.size() != 9)
        return false;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            matrix.m[row][col] = parsed[static_cast<size_t>(row * 3 + col)];
    }
    return true;
}

bool extractParseTargetPrimariesAndWhite(const HdrImage &image, std::array<double, 8> &values) {
    std::map<std::string, std::string>::const_iterator primariesIt = image.header.find("TargetPrimaries");
    std::map<std::string, std::string>::const_iterator whiteIt = image.header.find("TargetWhitePoint");
    if (primariesIt == image.header.end() || whiteIt == image.header.end())
        return false;
    const std::vector<double> primaries = extractParseDoubleList(primariesIt->second);
    const std::vector<double> white = extractParseDoubleList(whiteIt->second);
    if (primaries.size() != 6 || white.size() != 2)
        return false;
    for (size_t i = 0; i < 6; ++i)
        values[i] = primaries[i];
    values[6] = white[0];
    values[7] = white[1];
    return true;
}

bool extractParseRadiancePrimaries(const HdrImage &image, std::array<double, 8> &values) {
    std::map<std::string, std::string>::const_iterator it = image.header.find("PRIMARIES");
    if (it == image.header.end())
        return false;
    const std::vector<double> parsed = extractParseDoubleList(it->second);
    if (parsed.size() != 8)
        return false;
    for (size_t i = 0; i < 8; ++i)
        values[i] = parsed[i];
    return true;
}

double extractDeterminant(const ExtractMat3d &matrix) {
    return
        matrix.m[0][0] * (matrix.m[1][1] * matrix.m[2][2] - matrix.m[1][2] * matrix.m[2][1]) -
        matrix.m[0][1] * (matrix.m[1][0] * matrix.m[2][2] - matrix.m[1][2] * matrix.m[2][0]) +
        matrix.m[0][2] * (matrix.m[1][0] * matrix.m[2][1] - matrix.m[1][1] * matrix.m[2][0]);
}

bool extractInvertMatrix(const ExtractMat3d &input, ExtractMat3d &output) {
    const double det = extractDeterminant(input);
    if (!std::isfinite(det) || std::abs(det) <= 1e-12)
        return false;
    const double invDet = 1.0 / det;
    output.m[0][0] =  (input.m[1][1] * input.m[2][2] - input.m[1][2] * input.m[2][1]) * invDet;
    output.m[0][1] = -(input.m[0][1] * input.m[2][2] - input.m[0][2] * input.m[2][1]) * invDet;
    output.m[0][2] =  (input.m[0][1] * input.m[1][2] - input.m[0][2] * input.m[1][1]) * invDet;
    output.m[1][0] = -(input.m[1][0] * input.m[2][2] - input.m[1][2] * input.m[2][0]) * invDet;
    output.m[1][1] =  (input.m[0][0] * input.m[2][2] - input.m[0][2] * input.m[2][0]) * invDet;
    output.m[1][2] = -(input.m[0][0] * input.m[1][2] - input.m[0][2] * input.m[1][0]) * invDet;
    output.m[2][0] =  (input.m[1][0] * input.m[2][1] - input.m[1][1] * input.m[2][0]) * invDet;
    output.m[2][1] = -(input.m[0][0] * input.m[2][1] - input.m[0][1] * input.m[2][0]) * invDet;
    output.m[2][2] =  (input.m[0][0] * input.m[1][1] - input.m[0][1] * input.m[1][0]) * invDet;
    return true;
}

bool extractWeightsFromPrimaries(const std::array<double, 8> &primaries, std::array<double, 3> &weights) {
    const double rx = primaries[0];
    const double ry = primaries[1];
    const double gx = primaries[2];
    const double gy = primaries[3];
    const double bx = primaries[4];
    const double by = primaries[5];
    const double wx = primaries[6];
    const double wy = primaries[7];
    if (ry <= 0.0 || gy <= 0.0 || by <= 0.0 || wy <= 0.0)
        return false;

    ExtractMat3d pxyz;
    pxyz.m[0][0] = rx;
    pxyz.m[1][0] = ry;
    pxyz.m[2][0] = 1.0 - rx - ry;
    pxyz.m[0][1] = gx;
    pxyz.m[1][1] = gy;
    pxyz.m[2][1] = 1.0 - gx - gy;
    pxyz.m[0][2] = bx;
    pxyz.m[1][2] = by;
    pxyz.m[2][2] = 1.0 - bx - by;

    ExtractMat3d inversePxyz;
    if (!extractInvertMatrix(pxyz, inversePxyz))
        return false;

    const ExtractVec3d white{
        wx / wy,
        1.0,
        (1.0 - wx - wy) / wy
    };

    const ExtractVec3d scales{
        inversePxyz.m[0][0] * white.x + inversePxyz.m[0][1] * white.y + inversePxyz.m[0][2] * white.z,
        inversePxyz.m[1][0] * white.x + inversePxyz.m[1][1] * white.y + inversePxyz.m[1][2] * white.z,
        inversePxyz.m[2][0] * white.x + inversePxyz.m[2][1] * white.y + inversePxyz.m[2][2] * white.z
    };

    weights[0] = pxyz.m[1][0] * scales.x;
    weights[1] = pxyz.m[1][1] * scales.y;
    weights[2] = pxyz.m[1][2] * scales.z;
    return std::isfinite(weights[0]) && std::isfinite(weights[1]) && std::isfinite(weights[2]) &&
           (weights[0] > 0.0 || weights[1] > 0.0 || weights[2] > 0.0);
}

bool extractWeightsFromSensorToXyz(const ExtractMat3d &sensorToXyz, std::array<double, 3> &weights) {
    weights[0] = sensorToXyz.m[1][0];
    weights[1] = sensorToXyz.m[1][1];
    weights[2] = sensorToXyz.m[1][2];
    return std::isfinite(weights[0]) && std::isfinite(weights[1]) && std::isfinite(weights[2]) &&
           (weights[0] > 0.0 || weights[1] > 0.0 || weights[2] > 0.0);
}

bool extractParseSensorToXyz(const HdrImage &image, ExtractMat3d &sensorToXyz) {
    if (extractParseHeaderMatrix3x3(image, "SENSOR2XYZ", sensorToXyz))
        return true;

    ExtractMat3d xyzCam;
    if (!extractParseHeaderMatrix3x3(image, "XYZCAM", xyzCam))
        return false;

    std::array<double, 3> premults{{1.0, 1.0, 1.0}};
    extractParseHeaderTriplet(image, "CAM_PREMULTIPLIERS", premults);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            xyzCam.m[row][col] *= premults[static_cast<size_t>(row)];
    }
    return extractInvertMatrix(xyzCam, sensorToXyz);
}

bool extractImageIsMonochromeRgb(const HdrImage &image) {
    const double epsilon = 1e-6;
    for (size_t i = 0; i + 2 < image.rgb.size(); i += 3) {
        const double r = image.rgb[i + 0];
        const double g = image.rgb[i + 1];
        const double b = image.rgb[i + 2];
        const double scale = std::max(std::max(1.0, std::abs(r)), std::max(std::abs(g), std::abs(b)));
        if (std::abs(r - g) > epsilon * scale || std::abs(r - b) > epsilon * scale)
            return false;
    }
    return !image.rgb.empty();
}

bool extractStringContainsInsensitive(const std::string &haystack, const std::string &needle) {
    return lowerCopy(haystack).find(lowerCopy(needle)) != std::string::npos;
}

bool extractHeaderContainsInsensitive(const HdrImage &image, const std::string &needle) {
    for (size_t i = 0; i < image.headerLines.size(); ++i) {
        if (extractStringContainsInsensitive(image.headerLines[i], needle))
            return true;
    }
    return false;
}

bool extractLooksLikeTechnoteamOrLmk(const HdrImage &image) {
    return extractHeaderContainsInsensitive(image, "technoteam") ||
           extractHeaderContainsInsensitive(image, "lmk") ||
           extractHeaderContainsInsensitive(image, "pftopic") ||
           extractHeaderContainsInsensitive(image, "pcftoxyz") ||
           extractHeaderContainsInsensitive(image, "programversion=standard color") ||
           extractHeaderContainsInsensitive(image, "camera=svs") ||
           extractHeaderContainsInsensitive(image, "camera=tt");
}

bool extractLooksLikePlainRadianceRgb(const HdrImage &image) {
    if (!extractHeaderContainsInsensitive(image, "#?radiance") &&
        !extractHeaderContainsInsensitive(image, "format=32-bit_rle_rgbe")) {
        return false;
    }

    if (image.header.find("LuminanceRGB") != image.header.end() ||
        image.header.find("SENSOR2XYZ") != image.header.end() ||
        image.header.find("XYZCAM") != image.header.end() ||
        image.header.find("TargetPrimaries") != image.header.end() ||
        image.header.find("TargetWhitePoint") != image.header.end() ||
        image.header.find("PRIMARIES") != image.header.end()) {
        return false;
    }

    return !extractLooksLikeTechnoteamOrLmk(image);
}

std::array<double, 8> shadowbandRadiancePrimariesAndWhite() {
    return {{0.6400, 0.3300, 0.2900, 0.6000, 0.1500, 0.0600, 0.3333, 0.3333}};
}

std::array<double, 8> shadowbandXyzPrimariesAndWhite() {
    return {{1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.3333, 0.3333}};
}

bool shadowbandSamePrimariesAndWhite(const std::array<double, 8> &a, const std::array<double, 8> &b, double eps = 5e-3) {
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i] - b[i]) > eps)
            return false;
    }
    return true;
}

ExtractMat3d extractRadianceRgbToXyzMatrix() {
    ExtractMat3d matrix{};
    matrix.m[0][0] = 0.51408315;
    matrix.m[0][1] = 0.32388874;
    matrix.m[0][2] = 0.16202811;
    matrix.m[1][0] = 0.26507413;
    matrix.m[1][1] = 0.67011463;
    matrix.m[1][2] = 0.06481124;
    matrix.m[2][0] = 0.02409765;
    matrix.m[2][1] = 0.12285435;
    matrix.m[2][2] = 0.85334803;
    return matrix;
}

bool extractPrimariesToXyzMatrix(const std::array<double, 8> &primaries, ExtractMat3d &matrix) {
    const double rx = primaries[0];
    const double ry = primaries[1];
    const double gx = primaries[2];
    const double gy = primaries[3];
    const double bx = primaries[4];
    const double by = primaries[5];
    const double wx = primaries[6];
    const double wy = primaries[7];
    if (ry <= 0.0 || gy <= 0.0 || by <= 0.0 || wy <= 0.0)
        return false;

    ExtractMat3d pxyz{};
    pxyz.m[0][0] = rx;
    pxyz.m[1][0] = ry;
    pxyz.m[2][0] = 1.0 - rx - ry;
    pxyz.m[0][1] = gx;
    pxyz.m[1][1] = gy;
    pxyz.m[2][1] = 1.0 - gx - gy;
    pxyz.m[0][2] = bx;
    pxyz.m[1][2] = by;
    pxyz.m[2][2] = 1.0 - bx - by;

    ExtractMat3d inversePxyz;
    if (!extractInvertMatrix(pxyz, inversePxyz))
        return false;

    const ExtractVec3d white{
        wx / wy,
        1.0,
        (1.0 - wx - wy) / wy
    };

    const ExtractVec3d scales{
        inversePxyz.m[0][0] * white.x + inversePxyz.m[0][1] * white.y + inversePxyz.m[0][2] * white.z,
        inversePxyz.m[1][0] * white.x + inversePxyz.m[1][1] * white.y + inversePxyz.m[1][2] * white.z,
        inversePxyz.m[2][0] * white.x + inversePxyz.m[2][1] * white.y + inversePxyz.m[2][2] * white.z
    };

    matrix = pxyz;
    for (int col = 0; col < 3; ++col) {
        const double scale = (col == 0 ? scales.x : (col == 1 ? scales.y : scales.z));
        matrix.m[0][col] *= scale;
        matrix.m[1][col] *= scale;
        matrix.m[2][col] *= scale;
    }
    return true;
}

ExtractVec3d extractMul(const ExtractMat3d &m, const ExtractVec3d &v) {
    return {
        m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
        m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
        m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z
    };
}

struct ShadowbandInputColorInfo {
    enum Mode {
        Unsupported,
        RadianceRgb,
        Xyz,
        GenericRgb,
        Sensor
    };

    Mode mode = Unsupported;
    ExtractMat3d sourceToXyz{};
    std::array<double, 8> primariesWhite{};
    std::array<double, 3> luminanceRgb{{0.0, 0.0, 0.0}};
    bool havePrimariesWhite = false;
    bool haveLuminanceRgb = false;
    std::string description;
};

ShadowbandInputColorInfo resolveShadowbandInputColorInfo(const HdrImage &image) {
    ShadowbandInputColorInfo info;
    ExtractMat3d radianceRgbToXyz = extractRadianceRgbToXyzMatrix();

    std::array<double, 3> luminance{};
    if (extractParseHeaderTriplet(image, "LuminanceRGB", luminance)) {
        info.luminanceRgb = luminance;
        info.haveLuminanceRgb = true;
    }

    std::array<double, 8> primaries{};
    if (extractParseTargetPrimariesAndWhite(image, primaries) || extractParseRadiancePrimaries(image, primaries)) {
        info.primariesWhite = primaries;
        info.havePrimariesWhite = true;
        if (shadowbandSamePrimariesAndWhite(primaries, shadowbandXyzPrimariesAndWhite())) {
            info.mode = ShadowbandInputColorInfo::Xyz;
            info.description = "XYZ input";
            return info;
        }
        if (shadowbandSamePrimariesAndWhite(primaries, shadowbandRadiancePrimariesAndWhite())) {
            info.mode = ShadowbandInputColorInfo::RadianceRgb;
            info.sourceToXyz = radianceRgbToXyz;
            info.description = "Radiance RGB input";
            return info;
        }
        if (!extractPrimariesToXyzMatrix(primaries, info.sourceToXyz))
            throw std::runtime_error("shadowband could not derive RGB->XYZ from input primaries.");
        info.mode = ShadowbandInputColorInfo::GenericRgb;
        info.description = "Generic RGB input";
        return info;
    }

    ExtractMat3d sensorToXyz;
    if (extractParseSensorToXyz(image, sensorToXyz)) {
        info.mode = ShadowbandInputColorInfo::Sensor;
        info.sourceToXyz = sensorToXyz;
        info.description = "Sensor/raw-space input";
        return info;
    }

    if (extractLooksLikePlainRadianceRgb(image)) {
        info.mode = ShadowbandInputColorInfo::RadianceRgb;
        info.sourceToXyz = radianceRgbToXyz;
        info.description = "Plain Radiance RGB fallback";
        return info;
    }

    info.description = "Unsupported or unknown HDR color space";
    return info;
}

bool sameShadowbandColorMetadata(const ShadowbandInputColorInfo &a, const ShadowbandInputColorInfo &b) {
    if (a.mode != b.mode)
        return false;
    if (a.havePrimariesWhite != b.havePrimariesWhite)
        return false;
    if (a.havePrimariesWhite && !shadowbandSamePrimariesAndWhite(a.primariesWhite, b.primariesWhite))
        return false;
    if (a.haveLuminanceRgb != b.haveLuminanceRgb)
        return false;
    if (a.haveLuminanceRgb) {
        for (size_t i = 0; i < 3; ++i) {
            if (std::abs(a.luminanceRgb[i] - b.luminanceRgb[i]) > 5e-6)
                return false;
        }
    }
    return true;
}

HdrImage convertShadowbandInputToRadiance(const HdrImage &image, const ShadowbandInputColorInfo &info) {
    if (info.mode == ShadowbandInputColorInfo::Unsupported)
        throw std::runtime_error("shadowband input color space is unsupported; need Rad, XYZ, RGB primaries, or SENSOR2XYZ/XYZCAM.");
    if (info.mode == ShadowbandInputColorInfo::RadianceRgb)
        return image;

    ExtractMat3d xyzToRadiance;
    if (!extractInvertMatrix(extractRadianceRgbToXyzMatrix(), xyzToRadiance))
        throw std::runtime_error("shadowband could not derive XYZ->Radiance RGB conversion.");

    HdrImage out = image;
    for (size_t i = 0; i + 2 < out.rgb.size(); i += 3) {
        const ExtractVec3d src{
            static_cast<double>(image.rgb[i + 0]),
            static_cast<double>(image.rgb[i + 1]),
            static_cast<double>(image.rgb[i + 2])
        };
        const ExtractVec3d xyz = (info.mode == ShadowbandInputColorInfo::Xyz) ? src : extractMul(info.sourceToXyz, src);
        const ExtractVec3d rad = extractMul(xyzToRadiance, xyz);
        out.rgb[i + 0] = static_cast<float>(rad.x);
        out.rgb[i + 1] = static_cast<float>(rad.y);
        out.rgb[i + 2] = static_cast<float>(rad.z);
    }

    const std::set<std::string> removedKeys{
        "primaries", "targetprimaries", "targetwhitepoint", "luminancergb",
        "sensor2xyz", "xyzcam", "cam_premultipliers", "camera2rgb", "whitesaturation", "rgb2rgb"
    };
    std::vector<std::string> cleanLines = headerLinesReplacing(out, removedKeys);
    cleanLines.push_back(formatPrimariesHeader(parseColorSpaceSpec("rad").primaries));
    cleanLines.push_back(formatPairHeader("TargetWhitePoint", parseColorSpaceSpec("rad").white));
    cleanLines.push_back(formatTripletHeader("LuminanceRGB", std::array<float, 3>{{0.26507413f, 0.67011463f, 0.06481124f}}));
    cleanLines.push_back("MERGEHDR_INPUT_COLORSPACE_CONVERTED= " + info.description + " -> Rad");
    out.headerLines = cleanLines;
    out.header.erase("PRIMARIES");
    out.header.erase("TargetPrimaries");
    out.header.erase("TargetWhitePoint");
    out.header.erase("LuminanceRGB");
    out.header.erase("SENSOR2XYZ");
    out.header.erase("XYZCAM");
    out.header.erase("CAM_PREMULTIPLIERS");
    out.header.erase("Camera2RGB");
    out.header.erase("WhiteSaturation");
    out.header.erase("RGB2RGB");
    out.header["TargetPrimaries"] = "0.6400 0.3300 0.2900 0.6000 0.1500 0.0600";
    out.header["TargetWhitePoint"] = "0.3333 0.3333";
    out.header["LuminanceRGB"] = "0.26507413 0.67011463 0.06481124";
    return out;
}

void extractParseViewAnglesFromViewString(const std::string &viewText, double &horizontalDegrees, double &verticalDegrees) {
    std::istringstream iss(viewText);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token)
        tokens.push_back(token);
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == "-vh") {
            const double parsed = std::atof(tokens[i + 1].c_str());
            if (parsed > 0.0)
                horizontalDegrees = parsed;
        } else if (tokens[i] == "-vv") {
            const double parsed = std::atof(tokens[i + 1].c_str());
            if (parsed > 0.0)
                verticalDegrees = parsed;
        }
    }
}

ExtractViewBasis extractResolveViewBasis(const HdrImage &image) {
    ExtractViewBasis view;
    std::map<std::string, std::string>::const_iterator it = image.header.find("VIEW");
    if (it != image.header.end()) {
        std::istringstream iss(it->second);
        std::vector<std::string> tokens;
        std::string token;
        while (iss >> token)
            tokens.push_back(token);
        for (size_t i = 0; i + 3 < tokens.size(); ++i) {
            if (tokens[i] == "-vd") {
                view.vdir.x = std::atof(tokens[i + 1].c_str());
                view.vdir.y = std::atof(tokens[i + 2].c_str());
                view.vdir.z = std::atof(tokens[i + 3].c_str());
            } else if (tokens[i] == "-vu") {
                view.vup.x = std::atof(tokens[i + 1].c_str());
                view.vup.y = std::atof(tokens[i + 2].c_str());
                view.vup.z = std::atof(tokens[i + 3].c_str());
            }
        }
    }
    extractFinalizeViewBasis(view);
    return view;
}

ExtractProjectionMetadata extractDetectProjection(const HdrImage &image) {
    ExtractProjectionMetadata meta;
    if (image.width != image.height)
        return meta;
    std::map<std::string, std::string>::const_iterator viewIt = image.header.find("VIEW");
    if (viewIt != image.header.end())
        extractParseViewAnglesFromViewString(viewIt->second, meta.horizontalViewDegrees, meta.verticalViewDegrees);
    if (viewIt != image.header.end() && extractStringContainsInsensitive(viewIt->second, "-vta")) {
        meta.available = true;
        return meta;
    }
    std::map<std::string, std::string>::const_iterator projIt = image.header.find("MERGEHDR_PROJECTION");
    if (projIt != image.header.end()) {
        const std::string lowered = lowerCopy(trimCopy(projIt->second));
        if (lowered == "equidistant" || lowered == "equiangular")
            meta.available = true;
    }
    return meta;
}

ExtractLuminanceInfo extractResolveLuminanceInfo(const HdrImage &image) {
    ExtractLuminanceInfo info;

    double exposureProduct = 1.0;
    for (size_t i = 0; i < image.headerLines.size(); ++i) {
        const std::string &line = image.headerLines[i];
        if (!extractStringContainsInsensitive(line, "EXPOSURE="))
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const double exposure = std::atof(trimCopy(line.substr(eq + 1)).c_str());
        if (exposure > 0.0)
            exposureProduct *= exposure;
    }
    if (exposureProduct > 0.0)
        info.exposureScale = 1.0 / exposureProduct;

    if (extractImageIsMonochromeRgb(image)) {
        info.mode = ExtractLuminanceInfo::Monochrome;
        return info;
    }

    if (extractParseHeaderTriplet(image, "LuminanceRGB", info.weights)) {
        info.mode = ExtractLuminanceInfo::Weighted;
        return info;
    }

    ExtractMat3d sensorToXyz;
    if (extractParseSensorToXyz(image, sensorToXyz) && extractWeightsFromSensorToXyz(sensorToXyz, info.weights)) {
        info.mode = ExtractLuminanceInfo::Weighted;
        return info;
    }

    std::array<double, 8> primaries{};
    if (extractParseTargetPrimariesAndWhite(image, primaries) && extractWeightsFromPrimaries(primaries, info.weights)) {
        info.mode = ExtractLuminanceInfo::Weighted;
        return info;
    }
    if (extractParseRadiancePrimaries(image, primaries) && extractWeightsFromPrimaries(primaries, info.weights)) {
        info.mode = ExtractLuminanceInfo::Weighted;
        return info;
    }

    if (extractLooksLikeTechnoteamOrLmk(image)) {
        info.mode = ExtractLuminanceInfo::Weighted;
        info.weights = {{0.265074126, 0.670114631, 0.064811243}};
        return info;
    }

    return info;
}

double extractLuminanceAt(const HdrImage &image, size_t x, size_t y, const ExtractLuminanceInfo &info) {
    const size_t index = (y * image.width + x) * 3;
    double luminance = 0.0;
    if (info.mode == ExtractLuminanceInfo::Monochrome) {
        luminance = image.rgb[index + 1];
    } else {
        luminance =
            image.rgb[index + 0] * info.weights[0] +
            image.rgb[index + 1] * info.weights[1] +
            image.rgb[index + 2] * info.weights[2];
    }
    luminance *= 179.0;
    luminance *= info.exposureScale;
    return luminance;
}

ExtractGeometrySample extractGeometryAt(size_t width, size_t height, int x, int y,
        const ExtractViewBasis &view, const ExtractProjectionMetadata &projection) {
    ExtractGeometrySample sample;
    const double nx = (static_cast<double>(x) + 0.5) / static_cast<double>(width) - 0.5;
    const double ny = (static_cast<double>(y) + 0.5) / static_cast<double>(height) - 0.5;
    const double radius = std::sqrt(nx * nx + ny * ny);
    if (radius > 0.5)
        return sample;

    const double horizontalRadians = std::max(1e-9, projection.horizontalViewDegrees * M_PI / 180.0);
    const double verticalRadians = std::max(1e-9, projection.verticalViewDegrees * M_PI / 180.0);
    const double u = nx * horizontalRadians;
    const double v = ny * verticalRadians;
    const double theta = std::sqrt(u * u + v * v);
    const double duDv = (horizontalRadians * verticalRadians) / (static_cast<double>(width) * static_cast<double>(height));
    if (radius <= 1e-12) {
        sample.dir = view.vdir;
        sample.omega = duDv;
        sample.valid = true;
        return sample;
    }

    const double sinTheta = std::sin(theta);
    const double scale = sinTheta / std::max(theta, 1e-12);
    sample.dir = extractNormalize(extractVecAdd(
        extractVecMul(view.vdir, std::cos(theta)),
        extractVecAdd(extractVecMul(view.hv, u * scale), extractVecMul(view.vv, v * scale))));
    sample.omega = duDv * (sinTheta / std::max(theta, 1e-12));
    sample.valid = sample.omega > 0.0 && extractDot(sample.dir, view.vdir) >= 0.0;
    return sample;
}

double extractUniformOmega(const HdrImage &image) {
    if (image.width == 0 || image.height == 0)
        return 0.0;
    return 1.0 / static_cast<double>(image.width * image.height);
}

int extractTopFromBottom(const HdrImage &image, int bottom, int height) {
    return static_cast<int>(image.height) - (bottom + height);
}

int extractTopYFromBottomY(const HdrImage &image, int bottomY) {
    return static_cast<int>(image.height) - 1 - bottomY;
}

ExtractPixelSample extractBuildPixelSample(const HdrImage &image, int x, int y,
        const ExtractLuminanceInfo &luminanceInfo,
        const ExtractProjectionMetadata &projection,
        const ExtractViewBasis &viewBasis) {
    ExtractPixelSample sample;
    sample.x = x;
    sample.y = y;
    const size_t index = (static_cast<size_t>(y) * image.width + static_cast<size_t>(x)) * 3;
    sample.rgb[0] = image.rgb[index + 0];
    sample.rgb[1] = image.rgb[index + 1];
    sample.rgb[2] = image.rgb[index + 2];
    if (luminanceInfo.mode != ExtractLuminanceInfo::Unavailable)
        sample.luminance = extractLuminanceAt(image, static_cast<size_t>(x), static_cast<size_t>(y), luminanceInfo);
    if (projection.available) {
        const ExtractGeometrySample geometry = extractGeometryAt(image.width, image.height, x, y, viewBasis, projection);
        sample.omega = geometry.valid ? geometry.omega : 0.0;
    } else {
        sample.omega = extractUniformOmega(image);
    }
    return sample;
}

std::vector<ExtractPixelSample> extractCollectRectangleSamples(const HdrImage &image,
        const ExtractOptions &opts,
        const ExtractLuminanceInfo &luminanceInfo,
        const ExtractProjectionMetadata &projection,
        const ExtractViewBasis &viewBasis) {
    std::vector<ExtractPixelSample> samples;
    const int top = extractTopFromBottom(image, opts.bottom, opts.height);
    samples.reserve(static_cast<size_t>(opts.width * opts.height));
    for (int y = top; y < top + opts.height; ++y) {
        for (int x = opts.left; x < opts.left + opts.width; ++x)
            samples.push_back(extractBuildPixelSample(image, x, y, luminanceInfo, projection, viewBasis));
    }
    return samples;
}

std::vector<ExtractPixelSample> extractCollectCircleSamples(const HdrImage &image,
        const ExtractOptions &opts,
        const ExtractLuminanceInfo &luminanceInfo,
        const ExtractProjectionMetadata &projection,
        const ExtractViewBasis &viewBasis) {
    if (!projection.available)
        throw std::runtime_error("extract -c requires valid equidistant VIEW/projection metadata.");

    const int centerYTop = extractTopYFromBottomY(image, opts.centerY);
    const ExtractGeometrySample centerGeometry = extractGeometryAt(image.width, image.height,
        opts.centerX, centerYTop, viewBasis, projection);
    if (!centerGeometry.valid)
        throw std::runtime_error("Circle center falls outside the valid angular image area.");

    const double radiusRadians = opts.radiusDegrees * M_PI / 180.0;
    std::vector<ExtractPixelSample> samples;
    for (int y = 0; y < static_cast<int>(image.height); ++y) {
        for (int x = 0; x < static_cast<int>(image.width); ++x) {
            const ExtractGeometrySample geometry = extractGeometryAt(image.width, image.height, x, y, viewBasis, projection);
            if (!geometry.valid)
                continue;
            const double angle = std::acos(extractClampDot(extractDot(centerGeometry.dir, geometry.dir)));
            if (angle > radiusRadians)
                continue;
            samples.push_back(extractBuildPixelSample(image, x, y, luminanceInfo, projection, viewBasis));
        }
    }
    return samples;
}

std::vector<std::string> extractHeaderLinesForImage(const HdrImage &image, const ExtractOptions &opts) {
    std::set<std::string> replacedKeys;
    replacedKeys.insert("view");
    std::vector<std::string> lines = headerLinesReplacing(image, replacedKeys);
    std::ostringstream tag;
    if (opts.mode == ExtractOptions::RankedCircle) {
        tag << "MERGEHDR_EXTRACT= circle x=" << opts.centerX << " y=" << opts.centerY
            << " radius_deg=" << std::setprecision(10) << opts.radiusDegrees;
    } else {
        tag << "MERGEHDR_EXTRACT= rect left=" << opts.left << " bottom=" << opts.bottom
            << " width=" << opts.width << " height=" << opts.height;
    }
    lines.push_back(tag.str());
    return lines;
}

void extractWriteImageResult(const HdrImage &image, const ExtractOptions &opts, const std::vector<ExtractPixelSample> &samples) {
    HdrImage outImage;
    if (opts.mode == ExtractOptions::AverageOfRows || opts.mode == ExtractOptions::AverageOfColumns || opts.mode == ExtractOptions::RankedRectangle) {
        const int top = extractTopFromBottom(image, opts.bottom, opts.height);
        outImage = cropHdrImageTopLeftTopDown(image, opts.left, top, opts.width, opts.height);
    } else {
        if (samples.empty())
            throw std::runtime_error("Circle selection did not include any valid pixels.");
        int minX = std::numeric_limits<int>::max();
        int minY = std::numeric_limits<int>::max();
        int maxX = std::numeric_limits<int>::min();
        int maxY = std::numeric_limits<int>::min();
        for (size_t i = 0; i < samples.size(); ++i) {
            minX = std::min(minX, samples[i].x);
            minY = std::min(minY, samples[i].y);
            maxX = std::max(maxX, samples[i].x);
            maxY = std::max(maxY, samples[i].y);
        }
        outImage.width = static_cast<size_t>(maxX - minX + 1);
        outImage.height = static_cast<size_t>(maxY - minY + 1);
        outImage.rgb.assign(outImage.width * outImage.height * 3, 0.0f);
        outImage.header = image.header;
        outImage.headerLines = image.headerLines;
        for (size_t i = 0; i < samples.size(); ++i) {
            const size_t dstIndex =
                (static_cast<size_t>(samples[i].y - minY) * outImage.width + static_cast<size_t>(samples[i].x - minX)) * 3;
            outImage.rgb[dstIndex + 0] = static_cast<float>(samples[i].rgb[0]);
            outImage.rgb[dstIndex + 1] = static_cast<float>(samples[i].rgb[1]);
            outImage.rgb[dstIndex + 2] = static_cast<float>(samples[i].rgb[2]);
        }
    }

    const std::vector<std::string> headerLines = extractHeaderLinesForImage(image, opts);
    if (!opts.outputPath.empty()) {
        writeRadianceHDR(opts.outputPath, outImage, headerLines, false, true);
        return;
    }

    const std::string tmpdir = temporaryDirectory();
    const fs::path tmpPath(tmpdir);
    const fs::path hdrPath = tmpPath / "extract.hdr";
    writeRadianceHDR(hdrPath.string(), outImage, headerLines, false, true);
    copyBinaryFileToStream(hdrPath.string(), std::cout);
    std::cout.flush();
    removeTree(tmpPath);
}

std::ostream *extractOpenOutputStream(const ExtractOptions &opts, std::ofstream &ownedFile) {
    if (opts.outputPath.empty())
        return &std::cout;
    ownedFile.open(opts.outputPath.c_str(), std::ios::out | std::ios::binary);
    if (!ownedFile)
        throw std::runtime_error("Could not open output file: " + opts.outputPath);
    return &ownedFile;
}

double extractSortValue(const ExtractPixelSample &sample, bool haveLuminance) {
    if (haveLuminance)
        return sample.luminance;
    return std::max(sample.rgb[0], std::max(sample.rgb[1], sample.rgb[2]));
}

void extractWriteRankedData(const std::vector<ExtractPixelSample> &inputSamples,
        bool rgbOutput, bool haveLuminance, std::ostream &out) {
    std::vector<ExtractPixelSample> samples = inputSamples;
    std::stable_sort(samples.begin(), samples.end(), [haveLuminance](const ExtractPixelSample &a, const ExtractPixelSample &b) {
        return extractSortValue(a, haveLuminance) > extractSortValue(b, haveLuminance);
    });

    double cumulativeOmega = 0.0;
    double cumulativeLum = 0.0;
    std::array<double, 3> cumulativeRgb{{0.0, 0.0, 0.0}};
    for (size_t i = 0; i < samples.size(); ++i) {
        cumulativeOmega += samples[i].omega;
        cumulativeLum += samples[i].luminance;
        cumulativeRgb[0] += samples[i].rgb[0];
        cumulativeRgb[1] += samples[i].rgb[1];
        cumulativeRgb[2] += samples[i].rgb[2];

        out << std::setprecision(10) << cumulativeOmega;
        if (rgbOutput) {
            if (haveLuminance)
                out << "\t" << samples[i].luminance;
            out << "\t" << samples[i].rgb[0]
                << "\t" << samples[i].rgb[1]
                << "\t" << samples[i].rgb[2];
            if (haveLuminance)
                out << "\t" << (cumulativeLum / static_cast<double>(i + 1));
            out << "\t" << (cumulativeRgb[0] / static_cast<double>(i + 1))
                << "\t" << (cumulativeRgb[1] / static_cast<double>(i + 1))
                << "\t" << (cumulativeRgb[2] / static_cast<double>(i + 1));
        } else {
            out << "\t" << samples[i].luminance
                << "\t" << (cumulativeLum / static_cast<double>(i + 1));
        }
        out << "\n";
    }
}

void extractWriteSectionData(const std::vector<ExtractPixelSample> &samples, const ExtractOptions &opts,
        bool rgbOutput, bool haveLuminance, std::ostream &out) {
    const size_t bucketCount = opts.mode == ExtractOptions::AverageOfRows
        ? static_cast<size_t>(opts.height)
        : static_cast<size_t>(opts.width);
    std::vector<ExtractSeriesAggregate> aggregates(bucketCount);
    if (samples.empty())
        return;
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    for (size_t i = 0; i < samples.size(); ++i) {
        minX = std::min(minX, samples[i].x);
        minY = std::min(minY, samples[i].y);
    }
    for (size_t i = 0; i < samples.size(); ++i) {
        const size_t index = opts.mode == ExtractOptions::AverageOfRows
            ? static_cast<size_t>(samples[i].y - minY)
            : static_cast<size_t>(samples[i].x - minX);
        ExtractSeriesAggregate &aggregate = aggregates[index];
        aggregate.omegaSum += samples[i].omega;
        aggregate.luminanceSum += samples[i].luminance;
        aggregate.rgbSum[0] += samples[i].rgb[0];
        aggregate.rgbSum[1] += samples[i].rgb[1];
        aggregate.rgbSum[2] += samples[i].rgb[2];
        ++aggregate.count;
    }

    double cumulativeOmega = 0.0;
    double cumulativeLumMeans = 0.0;
    std::array<double, 3> cumulativeRgbMeans{{0.0, 0.0, 0.0}};
    size_t emitted = 0;
    for (size_t i = 0; i < aggregates.size(); ++i) {
        const ExtractSeriesAggregate &aggregate = aggregates[i];
        if (aggregate.count == 0)
            continue;
        ++emitted;
        cumulativeOmega += aggregate.omegaSum;
        const double meanLum = aggregate.luminanceSum / static_cast<double>(aggregate.count);
        const double meanR = aggregate.rgbSum[0] / static_cast<double>(aggregate.count);
        const double meanG = aggregate.rgbSum[1] / static_cast<double>(aggregate.count);
        const double meanB = aggregate.rgbSum[2] / static_cast<double>(aggregate.count);
        cumulativeLumMeans += meanLum;
        cumulativeRgbMeans[0] += meanR;
        cumulativeRgbMeans[1] += meanG;
        cumulativeRgbMeans[2] += meanB;

        out << std::setprecision(10) << cumulativeOmega;
        if (rgbOutput) {
            if (haveLuminance)
                out << "\t" << meanLum;
            out << "\t" << meanR << "\t" << meanG << "\t" << meanB;
            if (haveLuminance)
                out << "\t" << (cumulativeLumMeans / static_cast<double>(emitted));
            out << "\t" << (cumulativeRgbMeans[0] / static_cast<double>(emitted))
                << "\t" << (cumulativeRgbMeans[1] / static_cast<double>(emitted))
                << "\t" << (cumulativeRgbMeans[2] / static_cast<double>(emitted));
        } else {
            out << "\t" << meanLum
                << "\t" << (cumulativeLumMeans / static_cast<double>(emitted));
        }
        out << "\n";
    }
}

void validateExtractOptions(const ExtractOptions &opts) {
    if (opts.inputPath.empty())
        throw std::runtime_error("extract expects one input HDR.");
    if (!fileExists(opts.inputPath))
        throw std::runtime_error("Input HDR not found: " + opts.inputPath);
    if (opts.mode == ExtractOptions::None)
        throw std::runtime_error("extract requires one of -h, -v, -r, or -c.");
    if ((opts.mode == ExtractOptions::AverageOfRows || opts.mode == ExtractOptions::AverageOfColumns || opts.mode == ExtractOptions::RankedRectangle) &&
            (opts.width <= 0 || opts.height <= 0))
        throw std::runtime_error("extract rectangle width and height must be positive.");
    if (opts.mode == ExtractOptions::RankedCircle && !(opts.radiusDegrees > 0.0))
        throw std::runtime_error("extract circle radius must be positive.");
}

int printExtract(const ExtractOptions &opts) {
    validateExtractOptions(opts);
    const HdrImage image = readRadianceHDRTopDown(opts.inputPath);

    if (opts.mode == ExtractOptions::AverageOfRows || opts.mode == ExtractOptions::AverageOfColumns || opts.mode == ExtractOptions::RankedRectangle) {
        const int top = extractTopFromBottom(image, opts.bottom, opts.height);
        if (opts.left < 0 || opts.bottom < 0 || opts.width <= 0 || opts.height <= 0 || top < 0 ||
                static_cast<size_t>(opts.left + opts.width) > image.width ||
                static_cast<size_t>(top + opts.height) > image.height) {
            throw std::runtime_error("extract rectangle falls outside the input HDR.");
        }
    } else {
        const int centerYTop = extractTopYFromBottomY(image, opts.centerY);
        if (opts.centerX < 0 || opts.centerY < 0 ||
                static_cast<size_t>(opts.centerX) >= image.width ||
                centerYTop < 0 || static_cast<size_t>(centerYTop) >= image.height) {
            throw std::runtime_error("extract circle center falls outside the input HDR.");
        }
    }

    const ExtractProjectionMetadata projection = extractDetectProjection(image);
    const ExtractViewBasis viewBasis = extractResolveViewBasis(image);
    const ExtractLuminanceInfo luminanceInfo = extractResolveLuminanceInfo(image);
    const bool haveLuminance = luminanceInfo.mode != ExtractLuminanceInfo::Unavailable;

    if (!opts.rgbOutput && !haveLuminance)
        throw std::runtime_error("This image does not contain enough source color metadata to derive luminance. Use -rgb or provide luminance metadata.");

    std::vector<ExtractPixelSample> samples =
        opts.mode == ExtractOptions::RankedCircle
            ? extractCollectCircleSamples(image, opts, luminanceInfo, projection, viewBasis)
            : extractCollectRectangleSamples(image, opts, luminanceInfo, projection, viewBasis);

    if (opts.imageOutput) {
        extractWriteImageResult(image, opts, samples);
        return 0;
    }

    std::ofstream ownedFile;
    std::ostream *out = extractOpenOutputStream(opts, ownedFile);
    if (opts.mode == ExtractOptions::AverageOfRows || opts.mode == ExtractOptions::AverageOfColumns)
        extractWriteSectionData(samples, opts, opts.rgbOutput, haveLuminance, *out);
    else
        extractWriteRankedData(samples, opts.rgbOutput, haveLuminance, *out);
    return 0;
}

std::vector<float> parseOptionalFloatValues(const std::string &text) {
    if (text.empty())
        return std::vector<float>();
    return parseFloatList(text);
}

std::vector<float> resolveXYZCamValues(const RunOptions &opts, const std::vector<FrameInfo> &frames) {
    if (!opts.xyzcamText.empty())
        return parseFloatList(opts.xyzcamText);

    if (frames.empty())
        return std::vector<float>();

    CameraDetectInfo detected;
    std::string error;
    if (!detectXYZCamAuto(frames.front().filename, detected, &error))
        throw std::runtime_error("Could not determine XYZCAM automatically" + (error.empty() ? std::string(".") : std::string(": ") + error));

    std::vector<float> values(9);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            values[3 * i + j] = detected.xyzcam(i, j);
    return values;
}

ResolvedRawLevels resolveRawLevels(bool blackOverride, int blacklevel,
        bool whiteOverride, int whitepoint, const std::vector<std::string> &files);

ResolvedRawLevels resolveRawLevels(const RunOptions &opts, const std::vector<FrameInfo> &frames) {
    std::vector<std::string> files;
    files.reserve(frames.size());
    for (const FrameInfo &frame : frames)
        files.push_back(frame.filename);
    return resolveRawLevels(opts.blackOverride, opts.blacklevel,
        opts.whiteOverride, opts.whitepoint, files);
}

ResolvedRawLevels resolveRawLevels(bool blackOverride, int blacklevel,
        bool whiteOverride, int whitepoint, const std::vector<std::string> &files) {
    ResolvedRawLevels levels;
    RawLevelInfo rawLevels;
    bool haveAutoLevels = false;
    std::string rawLevelError;

    if ((!blackOverride || !whiteOverride) && !files.empty()) {
        haveAutoLevels = detectRawLevelsAuto(files, rawLevels, &rawLevelError);
        if (!haveAutoLevels && !rawLevelError.empty())
            throw std::runtime_error("Could not determine sensor black/white levels automatically: " + rawLevelError);
    }

    if (blackOverride)
        levels.headerBlacklevel = blacklevel;
    else if (haveAutoLevels)
        levels.headerBlacklevel = rawLevels.blacklevel;

    if (whiteOverride) {
        levels.headerWhitepoint = whitepoint;
    } else if (haveAutoLevels && levels.headerBlacklevel >= 0) {
        levels.headerWhitepoint = std::max(1, rawLevels.absoluteWhitepoint - levels.headerBlacklevel);
    } else if (haveAutoLevels) {
        levels.headerWhitepoint = rawLevels.whitepoint;
    }

    // The core subtracts each frame's own LibRaw black level unless a black level was given.
    levels.coreBlacklevel = blackOverride ? levels.headerBlacklevel : -1;
    if (levels.headerBlacklevel >= 0 && levels.headerWhitepoint >= 0)
        levels.coreWhitepoint = levels.headerBlacklevel + levels.headerWhitepoint;

    return levels;
}

void applyConfigDefaults(RunOptions &opts, const ConfigValues &cfg) {
    auto find = [&](const std::string &key) -> std::map<std::string, std::string>::const_iterator {
        return cfg.values.find(key);
    };
    if (!opts.colorspaceOverride && find("colorspace") != cfg.values.end())
        opts.colorspace = trimCopy(find("colorspace")->second);
    if (!opts.demosaicOverride && find("demosaic") != cfg.values.end())
        opts.demosaic = trimCopy(find("demosaic")->second);
    if (!opts.mergeWeightOverride && find("merge_weight") != cfg.values.end())
        opts.mergeWeight = trimCopy(find("merge_weight")->second);
    if (!opts.fisheyeOverride && find("fisheye") != cfg.values.end())
        opts.fisheye = parseBoolValue(find("fisheye")->second);
    if (find("dual_fisheye") != cfg.values.end())
        opts.dualFisheye = parseBoolValue(find("dual_fisheye")->second);
    if (find("single_fisheye_projection") != cfg.values.end())
        opts.singleFisheyeProjection = lowerCopy(trimCopy(find("single_fisheye_projection")->second));
    if (find("projection_reference_cal") != cfg.values.end())
        opts.projectionReferenceCal = trimCopy(find("projection_reference_cal")->second);
    if (find("projection_view") != cfg.values.end())
        opts.projectionView = trimCopy(find("projection_view")->second);
    if (!opts.xyzcamOverride && find("xyzcam") != cfg.values.end())
        opts.xyzcamText = trimCopy(find("xyzcam")->second);
    if (!opts.rawMultipliersOverride && find("rawmultipliers") != cfg.values.end())
        opts.rawMultipliersText = trimCopy(find("rawmultipliers")->second);
    if (!opts.badpixelsOverride && find("badpixels") != cfg.values.end())
        opts.badpixelsText = trimCopy(find("badpixels")->second);
    if (!opts.cscaleOverride && find("cscale") != cfg.values.end())
        opts.cscaleText = trimCopy(find("cscale")->second);
    if (!opts.vfileOverride && find("vfile") != cfg.values.end())
        opts.vfilePath = trimCopy(find("vfile")->second);
    if (!opts.cropOverride && find("crop") != cfg.values.end())
        opts.crop = parseIntList(find("crop")->second);
    if (find("crop_left") != cfg.values.end())
        opts.cropLeft = parseIntList(find("crop_left")->second);
    if (find("crop_right") != cfg.values.end())
        opts.cropRight = parseIntList(find("crop_right")->second);
    if (find("projection_coefficients") != cfg.values.end())
        opts.projectionCoefficients = parseDoubleList(find("projection_coefficients")->second);
    if (find("projection_source_radius") != cfg.values.end())
        opts.projectionSourceRadius = std::stod(trimCopy(find("projection_source_radius")->second));
    if (find("projection_target_radius") != cfg.values.end())
        opts.projectionTargetRadius = std::stod(trimCopy(find("projection_target_radius")->second));
    if (!opts.blackOverride && find("black") != cfg.values.end()) {
        opts.blacklevel = std::stoi(trimCopy(find("black")->second));
        opts.blackOverride = true;
    }
    if (!opts.whiteOverride && find("white") != cfg.values.end()) {
        opts.whitepoint = std::stoi(trimCopy(find("white")->second));
        opts.whiteOverride = true;
    }
    if (!opts.scaleOverride && find("scale") != cfg.values.end())
        opts.scale = std::stod(trimCopy(find("scale")->second));
    if (!opts.ndOverride && find("nd") != cfg.values.end())
        opts.nd = std::stod(trimCopy(find("nd")->second));
    if (!opts.saturationOverride && find("saturation") != cfg.values.end())
        opts.saturation = std::stod(trimCopy(find("saturation")->second));
    if (!opts.rangeOverride && find("range") != cfg.values.end())
        opts.rangeValue = std::stod(trimCopy(find("range")->second));
    if (!opts.shuttercOverride && find("shutterc") != cfg.values.end()) {
        opts.shutterc = std::stod(trimCopy(find("shutterc")->second));
        opts.shuttercOverride = true;
    }
    if (!opts.foOverride && find("fo") != cfg.values.end()) {
        opts.foText = trimCopy(find("fo")->second);
        opts.foOverride = true;
    }
}

void applyCalibrationDefaults(CalibrationCommonOptions &opts, const ConfigValues &cfg) {
    auto find = [&](const std::string &key) -> std::map<std::string, std::string>::const_iterator {
        return cfg.values.find(key);
    };
    if (!opts.cropOverride && find("crop") != cfg.values.end()) {
        opts.crop = parseIntList(find("crop")->second);
        opts.cropOverride = true;
    }
    if (!opts.badpixelsOverride && find("badpixels") != cfg.values.end()) {
        opts.badpixelsText = trimCopy(find("badpixels")->second);
        opts.badpixelsOverride = !opts.badpixelsText.empty();
    }
    if (!opts.blackOverride && find("black") != cfg.values.end()) {
        opts.blacklevel = std::stoi(trimCopy(find("black")->second));
        opts.blackOverride = true;
    }
    if (!opts.whiteOverride && find("white") != cfg.values.end()) {
        opts.whitepoint = std::stoi(trimCopy(find("white")->second));
        opts.whiteOverride = true;
    }
    if (!opts.scaleOverride && find("scale") != cfg.values.end())
        opts.scale = std::stod(trimCopy(find("scale")->second));
    if (!opts.ndOverride && find("nd") != cfg.values.end())
        opts.nd = std::stod(trimCopy(find("nd")->second));
    if (!opts.saturationOverride && find("saturation") != cfg.values.end())
        opts.saturation = std::stod(trimCopy(find("saturation")->second));
    if (!opts.rangeOverride && find("range") != cfg.values.end())
        opts.rangeValue = std::stod(trimCopy(find("range")->second));
    if (!opts.shuttercOverride && find("shutterc") != cfg.values.end()) {
        opts.shutterc = std::stod(trimCopy(find("shutterc")->second));
        opts.shuttercOverride = true;
    }
    if (!opts.foOverride && find("fo") != cfg.values.end()) {
        opts.foText = trimCopy(find("fo")->second);
        opts.foOverride = true;
    }
}

void finalizeOptionFlags(RunOptions &opts) {
    if (opts.bloomPrevent) {
        if (!opts.mergeWeightOverride)
            opts.mergeWeight = "linearhdr";
        if (!opts.demosaicOverride)
            opts.demosaic = "dht";
    }
    if (opts.rawgrid) {
        opts.colorspace = "raw";
        opts.fisheye = false;
    }
    if (!opts.xyzcamText.empty())
        opts.xyzcamOverride = true;
    if (!opts.rawMultipliersText.empty())
        opts.rawMultipliersOverride = true;
    if (!opts.badpixelsText.empty())
        opts.badpixelsOverride = true;
    if (!opts.cscaleText.empty())
        opts.cscaleOverride = true;
    const std::string vfileToken = lowerCopy(trimCopy(opts.vfilePath));
    if (vfileToken == "none")
        opts.vfilePath.clear();
}

std::string topLevelHelp() {
    std::ostringstream oss;
    std::vector<std::string> profiles = availableProfiles();
    oss << "mergehdr " << MERGEHDR_VERSION << "\n";
    oss << "Convert a RAW bracket into HDR, or run the camera calibration tools.\n\n";
    oss << "Usage:\n";
    oss << "  mergehdr [global options] run [run options] <RAW files...>\n";
    oss << "  mergehdr [global options] makelist [run options] <RAW files...>\n\n";
    oss << "  mergehdr [global options] shutter [options] -seq \"pattern or files\" [-seq ...]\n";
    oss << "  mergehdr [global options] aperture [options] -seq \"pattern or files\" -seq \"pattern or files\" [...]\n\n";
    oss << "  mergehdr [global options] colorcalibrate [options] reference test\n\n";
    oss << "  mergehdr [global options] chartcells [options] image\n\n";
    oss << "  mergehdr [global options] hdrcrop -crop L T W H -o output.hdr input.hdr\n\n";
    oss << "  mergehdr [global options] rotate -angle DEGREES -o output.hdr input.hdr\n\n";
    oss << "  mergehdr [global options] shadowband [options] H.hdr V.hdr ND.hdr\n\n";
    oss << "  mergehdr [global options] extract [options] input.hdr\n\n";
    oss << "  mergehdr [global options] evalglare [options] input.hdr\n\n";
    oss << "  mergehdr [global options] view_visibility [options] -ref reference.hdr -test test.hdr\n\n";
    oss << "  mergehdr view-vos-calculation --interior interior.hdr --exterior exterior.hdr\n";
    oss << "                                  [--window-distance M] [--window-mask mask.png] --output folder\n\n";
    oss << "  mergehdr [global options] perceptualmap [options] --map adaptation input.hdr\n\n";
    oss << "Global options:\n";
    oss << "  -profile, -p NAME   Load a saved profile.\n";
    oss << "  -config, -c PATH    Load a config file directly.\n";
    oss << "  --help              Show this help.\n";
    oss << "  --version           Show the version.\n\n";
    oss << "Commands:\n";
    oss << "  run                 Merge a RAW bracket into HDR.\n";
    oss << "  makelist            Print the resolved merge settings.\n";
    oss << "  shutter             Fit shutter correction from RAW samples.\n";
    oss << "  aperture            Fit aperture correction from RAW samples.\n";
    oss << "  chartcells          Detect chart patch rectangles in a cropped chart image.\n";
    oss << "  colorcalibrate      Fit XYZCAM from reference and test chart data.\n\n";
    oss << "  hdrcrop             Crop an HDR while keeping and extending its metadata.\n\n";
    oss << "  rotate              Rotate an HDR natively on its original canvas.\n\n";
    oss << "  shadowband          Native HDR-only shadowband merge for H/V/ND fisheye inputs.\n\n";
    oss << "  extract             Extract section/region profiles from an HDR.\n\n";
    oss << "  convertprojection   Reproject an HDR while keeping and extending its metadata.\n\n";
    oss << "  evalglare           Native port of Radiance evalglare 3.06 (all view types).\n\n";
    oss << "  view_visibility     Native HDR-VDP3 style reference/test visibility analysis.\n\n";
    oss << "  view-vos-calculation\n";
    oss << "                      DA3/SAM3 building and greenery volume-of-sight calculation.\n\n";
    oss << "  perceptualmap       Experimental single-image perceptual maps.\n\n";
    oss << "Calibration workflow:\n";
    oss << "  1. chartcells       Write patch rectangles for the reference and test chart.\n";
    oss << "  2. colorcalibrate   Fit the color matrix from those sampled patches.\n";
    oss << "  3. shutter          Fit shutter correction from a RAW sequence.\n";
    oss << "  4. aperture         Fit aperture correction if you calibrate multiple f-stops.\n\n";
    oss << "Common run options:\n";
    oss << "  --colorspace        Output space: rad, srgb, xyz, raw.\n";
    oss << "  --crop              Crop the RAW mosaic before merge.\n";
    oss << "  --xyzcam            Override the XYZ->camera matrix.\n";
    oss << "  --badpixels         Repair listed RAW bad pixels before merging.\n";
    oss << "  --demosaic          Demosaicing: ahd or dht.\n";
    oss << "  --merge-weight      Merge weighting: hdrmerge or linearhdr.\n";
    oss << "  --rawgrid           Export a sparse 3-channel raw grid without interpolation.\n";
    oss << "  -vfile, --vfile     Apply a pylinearhdr-compatible angle/RGB vignetting table.\n";
    oss << "  --saturation        Highlight offset from sensor white.\n";
    oss << "  --fisheye           Apply equisolid -> equiangular reprojection.\n";
    oss << "  --bloom-prevent     Shortcut for --merge-weight linearhdr --demosaic dht.\n";
    oss << "  --verbose           Print detailed progress.\n";
    oss << "  See `mergehdr run --help` for the full option list.\n\n";
    oss << "Profiles:\n";
    oss << "  " << joinStrings(profiles, ", ") << "\n";
    return oss.str();
}

std::string runHelp(const std::string &command) {
    std::ostringstream oss;
    oss << "Usage:\n";
    oss << "  mergehdr " << command << " [options] <RAW files...>\n\n";
    oss << "Options:\n";
    oss << "  -colorspace, --colorspace TEXT\n";
    oss << "                     Output space: rad, srgb, xyz, raw.\n";
    oss << "  -crop, --crop L T W H\n";
    oss << "                     Crop the RAW mosaic before merge.\n";
    oss << "  -black, --black INTEGER\n";
    oss << "                     Override the sensor black level.\n";
    oss << "  -white, --white INTEGER\n";
    oss << "                     Override the usable sensor white level above black.\n";
    oss << "  -xyzcam, --xyzcam TEXT\n";
    oss << "                     XYZ->camera matrix, row-major 3x3. If omitted,\n";
    oss << "                     read it from the first RAW file.\n";
    oss << "  -badpixels, --badpixels TEXT\n";
    oss << "                     Bad pixel coordinates as \"x,y x,y ...\".\n";
    oss << "  --rawgrid           Export a sparse 3-channel raw grid without interpolation.\n";
    oss << "                     Forces colorspace raw and skips fisheye reprojection.\n";
    oss << "  --demosaic TEXT\n";
    oss << "                     Demosaicing: ahd or dht.\n";
    oss << "  --merge-weight TEXT\n";
    oss << "                     Merge weighting: hdrmerge or linearhdr.\n";
    oss << "  -rawmultipliers, --rawmultipliers TEXT\n";
    oss << "                     Camera premultipliers (3 or 4 values) applied\n";
    oss << "                     before Camera2RGB is derived.\n";
    oss << "  -cscale, --cscale TEXT\n";
    oss << "                     Output RGB calibration scale.\n";
    oss << "  -vfile, --vfile PATH\n";
    oss << "                     Vignetting table: angle+factor or angle+R+G+B.\n";
    oss << "                     Applied after lens projection using pylinearhdr's\n";
    oss << "                     180-degree pixel-center angle and linear interpolation.\n";
    oss << "  -scale, --scale FLOAT\n";
    oss << "                     Calibration scale factor.\n";
    oss << "  -nd, --nd FLOAT    Neutral-density optical density; multiplier is 10^ND.\n";
    oss << "  -saturation, --saturation FLOAT\n";
    oss << "                     Highlight offset from sensor white.\n";
    oss << "  -range, --range FLOAT\n";
    oss << "                     Shadow floor in normalized sensor units.\n";
    oss << "  -shutterc, --shutterc FLOAT\n";
    oss << "                     Shutter correction factor.\n";
    oss << "  -fo, --fo TEXT     Aperture override pairs nominal,exact.\n";
    oss << "  --fisheye           Apply equisolid -> equiangular reprojection.\n";
    oss << "  --no-fisheye        Skip fisheye reprojection.\n";
    oss << "  --bloom-prevent     Shortcut for --merge-weight linearhdr --demosaic dht.\n";
    oss << "  --correct           Apply shutter/aperture correction.\n";
    oss << "  --no-correct        Use nominal shutter/aperture values.\n";
    oss << "  -o, --output PATH   Write to a file instead of stdout.\n";
    oss << "  --verbose           Print detailed progress.\n";
    oss << "  --help              Show this help.\n";
    return oss.str();
}

GlobalOptions parseGlobalOptions(const std::vector<std::string> &args) {
    GlobalOptions global;
    std::vector<std::string> normalizedArgs = normalizeOptionSpellings(args);
    po::options_description desc("global");
    desc.add_options()
        ("help", "help")
        ("version", "version")
        ("config,c", po::value<std::string>(), "config")
        ("profile,p", po::value<std::string>(), "profile");

    po::variables_map vm;
    std::vector<const char *> argvVec;
    argvVec.push_back("mergehdr");
    for (const std::string &arg : normalizedArgs)
        argvVec.push_back(arg.c_str());

    po::store(po::command_line_parser(static_cast<int>(argvVec.size()), const_cast<char **>(argvVec.data()))
                  .options(desc)
                  .allow_unregistered()
                  .run(),
              vm);
    po::notify(vm);
    global.help = vm.count("help") != 0;
    global.version = vm.count("version") != 0;
    if (vm.count("config"))
        global.configPath = vm["config"].as<std::string>();
    if (vm.count("profile"))
        global.profileName = vm["profile"].as<std::string>();
    return global;
}

RunOptions parseRunOptions(const std::vector<std::string> &args) {
    RunOptions opts;
    std::vector<std::string> normalizedArgs = normalizeOptionSpellings(args);
    po::options_description desc("run");
    desc.add_options()
        ("help", "help")
        ("version", "version")
        ("config,c", po::value<std::string>(), "config")
        ("profile,p", po::value<std::string>(), "profile")
        ("verbose", po::bool_switch(&opts.verbose), "verbose")
        ("bloom-prevent", po::bool_switch(&opts.bloomPrevent), "bloom-prevent")
        ("rawgrid", po::bool_switch(&opts.rawgrid), "rawgrid")
        ("correct", po::bool_switch()->default_value(false), "correct")
        ("no-correct", po::bool_switch()->default_value(false), "no-correct")
        ("fisheye", po::bool_switch()->default_value(false), "fisheye")
        ("no-fisheye", po::bool_switch()->default_value(false), "no-fisheye")
        ("colorspace", po::value<std::string>(&opts.colorspace), "colorspace")
        ("demosaic", po::value<std::string>(&opts.demosaic), "demosaic")
        ("merge-weight", po::value<std::string>(&opts.mergeWeight), "merge-weight")
        ("xyzcam", po::value<std::string>(&opts.xyzcamText), "xyzcam")
        ("badpixels", po::value<std::string>(&opts.badpixelsText), "badpixels")
        ("rawmultipliers", po::value<std::string>(&opts.rawMultipliersText), "rawmultipliers")
        ("cscale", po::value<std::string>(&opts.cscaleText), "cscale")
        ("vfile", po::value<std::string>(&opts.vfilePath), "vignetting file")
        ("fo", po::value<std::string>(&opts.foText), "fo")
        ("shutterc", po::value<double>(&opts.shutterc), "shutterc")
        ("scale", po::value<double>(&opts.scale), "scale")
        ("nd", po::value<double>(&opts.nd), "nd")
        ("saturation", po::value<double>(&opts.saturation), "saturation")
        ("range", po::value<double>(&opts.rangeValue), "range")
        ("black", po::value<int>(&opts.blacklevel), "black")
        ("white", po::value<int>(&opts.whitepoint), "white")
        ("crop", po::value<std::vector<int>>(&opts.crop)->multitoken(), "crop")
        ("output,o", po::value<std::string>(&opts.outputPath), "output")
        ("input-files", po::value<std::vector<std::string>>(&opts.inputs), "input files");

    po::positional_options_description positional;
    positional.add("input-files", -1);

    std::vector<const char *> argvVec;
    argvVec.push_back("run");
    for (const std::string &arg : normalizedArgs)
        argvVec.push_back(arg.c_str());

    po::variables_map vm;
    po::store(po::command_line_parser(static_cast<int>(argvVec.size()), const_cast<char **>(argvVec.data()))
                  .options(desc)
                  .positional(positional)
                  .run(),
              vm);
    po::notify(vm);

    opts.help = vm.count("help") != 0;
    if (vm["correct"].as<bool>())
        opts.correct = true;
    if (vm["no-correct"].as<bool>())
        opts.correct = false;
    if (vm["fisheye"].as<bool>()) {
        opts.fisheye = true;
        opts.fisheyeOverride = true;
    }
    if (vm["no-fisheye"].as<bool>()) {
        opts.fisheye = false;
        opts.fisheyeOverride = true;
    }
    if (vm.count("colorspace"))
        opts.colorspaceOverride = true;
    if (vm.count("demosaic"))
        opts.demosaicOverride = true;
    if (vm.count("merge-weight"))
        opts.mergeWeightOverride = true;
    if (vm.count("xyzcam"))
        opts.xyzcamOverride = true;
    if (vm.count("badpixels"))
        opts.badpixelsOverride = true;
    if (vm.count("rawmultipliers"))
        opts.rawMultipliersOverride = true;
    if (vm.count("cscale"))
        opts.cscaleOverride = true;
    if (vm.count("vfile"))
        opts.vfileOverride = true;
    if (vm.count("crop"))
        opts.cropOverride = true;
    if (vm.count("black"))
        opts.blackOverride = true;
    if (vm.count("white"))
        opts.whiteOverride = true;
    if (vm.count("scale"))
        opts.scaleOverride = true;
    if (vm.count("nd"))
        opts.ndOverride = true;
    if (vm.count("saturation"))
        opts.saturationOverride = true;
    if (vm.count("range"))
        opts.rangeOverride = true;
    if (vm.count("shutterc"))
        opts.shuttercOverride = true;
    if (vm.count("fo"))
        opts.foOverride = true;
    if (vm.count("output"))
        opts.outputOverride = true;
    return opts;
}

void validateRunOptions(const RunOptions &opts) {
    if (opts.inputs.empty())
        throw std::runtime_error("No input files were given.");
    if (opts.cropOverride && opts.crop.size() != 4)
        throw std::runtime_error("Crop expects four integers: left top width height.");
    std::string demosaic = canonicalDemosaic(opts.demosaic);
    if (demosaic != "ahd" && demosaic != "dht")
        throw std::runtime_error("Demosaicing must be 'ahd' or 'dht'.");
    std::string mergeWeight = canonicalMergeWeight(opts.mergeWeight);
    if (mergeWeight != "hdrmerge" && mergeWeight != "linearhdr")
        throw std::runtime_error("Merge weighting must be 'hdrmerge' or 'linearhdr'.");
    if (opts.dualFisheye) {
        if (opts.fisheye)
            throw std::runtime_error("dual_fisheye performs its own calibrated projection; disable the regular fisheye option.");
        if (!opts.crop.empty())
            throw std::runtime_error("dual_fisheye requires the full RAW frame; remove the regular crop option.");
        if (opts.rawgrid)
            throw std::runtime_error("dual_fisheye requires a demosaiced RGB output, not rawgrid.");
        if (opts.cropLeft.size() != 4 || opts.cropRight.size() != 4)
            throw std::runtime_error("dual_fisheye requires crop_left and crop_right as: left top width height.");
        for (const std::vector<int> *crop : {&opts.cropLeft, &opts.cropRight}) {
            if ((*crop)[0] < 0 || (*crop)[1] < 0 || (*crop)[2] <= 0 || (*crop)[3] <= 0)
                throw std::runtime_error("dual_fisheye crop rectangles must have non-negative origins and positive sizes.");
            if ((*crop)[2] != (*crop)[3])
                throw std::runtime_error("dual_fisheye crop rectangles must be square.");
        }
        if (opts.cropLeft[2] != opts.cropRight[2] || opts.cropLeft[3] != opts.cropRight[3])
            throw std::runtime_error("dual_fisheye left and right crops must have matching dimensions.");
        if (opts.projectionCoefficients.size() != 5)
            throw std::runtime_error("dual_fisheye requires exactly five projection_coefficients.");
        if (!(opts.projectionSourceRadius > 0.0) || !(opts.projectionTargetRadius > 0.0))
            throw std::runtime_error("dual_fisheye projection radii must be positive.");
        const double cropRadius = 0.5 * static_cast<double>(opts.cropLeft[2]);
        if (opts.projectionSourceRadius > cropRadius || opts.projectionTargetRadius > cropRadius)
            throw std::runtime_error("dual_fisheye projection radii must fit inside the square crop.");
        const double endpoint = std::accumulate(
            opts.projectionCoefficients.begin(), opts.projectionCoefficients.end(), 0.0);
        if (std::fabs(endpoint - 1.0) > 1e-6)
            throw std::runtime_error("dual_fisheye projection polynomial must preserve the 90-degree edge (P(1)=1).");
        for (int sample = 0; sample <= 4096; ++sample) {
            const double u = static_cast<double>(sample) / 4096.0;
            double value = 0.0;
            double derivative = 0.0;
            evaluateRadialPolynomial(opts.projectionCoefficients, u, value, derivative);
            if (!(derivative > 0.0) || !std::isfinite(derivative))
                throw std::runtime_error("dual_fisheye projection polynomial must be strictly monotonic on u=[0,1].");
        }
    }

    if (!opts.singleFisheyeProjection.empty()) {
        if (opts.singleFisheyeProjection != "paul_bourke_equidistant")
            throw std::runtime_error("Unsupported single_fisheye_projection: " + opts.singleFisheyeProjection);
        if (opts.dualFisheye)
            throw std::runtime_error("single_fisheye_projection and dual_fisheye cannot be enabled together.");
        if (opts.fisheye)
            throw std::runtime_error("single_fisheye_projection performs its own calibrated projection; set fisheye=False.");
        if (opts.rawgrid)
            throw std::runtime_error("single_fisheye_projection requires a demosaiced RGB output, not rawgrid.");
        if (opts.crop.size() != 4 || opts.crop[2] != opts.crop[3])
            throw std::runtime_error("single_fisheye_projection requires a square crop as: left top width height.");
        if (opts.projectionCoefficients.size() != 4)
            throw std::runtime_error("paul_bourke_equidistant requires four projection_coefficients.");
        if (!(opts.projectionSourceRadius > 0.0) || !(opts.projectionTargetRadius > 0.0))
            throw std::runtime_error("single_fisheye_projection radii must be positive.");
        const double cropRadius = 0.5 * static_cast<double>(opts.crop[2]);
        if (opts.projectionSourceRadius > cropRadius || opts.projectionTargetRadius > cropRadius)
            throw std::runtime_error("single_fisheye_projection radii must fit inside the square crop.");
        if (opts.projectionView.empty() || opts.projectionView.find("-vta") == std::string::npos)
            throw std::runtime_error("single_fisheye_projection requires an angular Radiance projection_view (-vta).");
        const double halfPi = 0.5 * std::acos(-1.0);
        for (int sample = 0; sample <= 4096; ++sample) {
            const double phi = halfPi * static_cast<double>(sample) / 4096.0;
            double value = 0.0;
            double derivative = 0.0;
            evaluateRadialPolynomial(opts.projectionCoefficients, phi, value, derivative);
            if (value < 0.0 || !(derivative > 0.0) || !std::isfinite(value) || !std::isfinite(derivative))
                throw std::runtime_error("single_fisheye_projection polynomial must be finite, non-negative, and strictly monotonic on phi=[0,pi/2].");
        }
    }
}

std::vector<std::string> expandSequenceText(const std::string &text) {
    std::vector<std::string> tokens = splitWhitespace(text);
    if (tokens.empty())
        throw std::runtime_error("Encountered an empty sequence.");
    return expandInputs(tokens);
}

std::vector<int> parseCropTextOption(const std::string &text) {
    std::vector<int> crop = parseIntList(text);
    if (crop.size() != 4)
        throw std::runtime_error("Crop expects four integers: left top width height.");
    return crop;
}

std::string shutterHelp(const std::string &command) {
    std::ostringstream oss;
    oss << "Usage:\n";
    oss << "  mergehdr " << command << " [options] -seq \"pattern or files\" [-seq ...]\n\n";
    oss << "Estimate the shutter correction term used by `mergehdr run -shutterc`.\n\n";
    oss << "What it does:\n";
    oss << "  - Samples one RAW channel inside the crop for each frame.\n";
    oss << "  - Keeps only frames whose crop stays fully in range.\n";
    oss << "  - Averages duplicate shutter values.\n";
    oss << "  - Fits x_corrected = x * exp(A * x), where x is the shutter reciprocal.\n";
    oss << "  - Prints `shutterc = A` for use in `mergehdr run` or a profile.\n\n";
    oss << "Good input:\n";
    oss << "  Use a small, even, neutral crop that stays in range across the sequence.\n";
    oss << "  If you pass multiple -seq groups, adjacent groups should overlap in shutter speed.\n\n";
    oss << "Options:\n";
    oss << "  -seq TEXT           One shutter-calibration sequence. Quote each sequence.\n";
    oss << "  -crop TEXT          Crop for the matching -seq as \"L T W H\". Repeats last crop.\n";
    oss << "  -channel TEXT       Raw channel to sample: r, g, or b. Default: g.\n";
    oss << "  -dataout PATH       Write fit data table.\n";
    oss << "  -badpixels TEXT     Bad pixel coordinates as \"x,y x,y ...\".\n";
    oss << "  -black, --black INTEGER\n";
    oss << "                     Override the sensor black level.\n";
    oss << "  -white, --white INTEGER\n";
    oss << "                     Override the usable sensor white level above black.\n";
    oss << "  -scale, --scale FLOAT\n";
    oss << "                     Calibration scale factor.\n";
    oss << "  -nd, --nd FLOAT    Neutral-density optical density; multiplier is 10^ND.\n";
    oss << "  -saturation, --saturation FLOAT\n";
    oss << "                     Highlight offset from sensor white.\n";
    oss << "  -range, --range FLOAT\n";
    oss << "                     Shadow floor in normalized sensor units.\n";
    oss << "  -shutterc, --shutterc FLOAT\n";
    oss << "                     Shutter correction factor used while sampling.\n";
    oss << "  -fo, --fo TEXT     Aperture override pairs nominal,exact.\n";
    oss << "  --help              Show this help.\n";
    oss << "\nExamples:\n";
    oss << "  mergehdr shutter -seq \"*.CR3\" -crop \"2400 2400 300 300\"\n";
    oss << "  mergehdr shutter -seq \"set1/*.CR3\" -seq \"set2/*.CR3\" \\\n";
    oss << "                    -crop \"2400 2400 300 300\" -crop \"1800 2200 250 250\"\n";
    oss << "\nApply the result:\n";
    oss << "  mergehdr run -shutterc <printed value> \"*.CR3\" > out.hdr\n";
    return oss.str();
}

std::string apertureHelp(const std::string &command) {
    std::ostringstream oss;
    oss << "Usage:\n";
    oss << "  mergehdr " << command << " [options] -seq \"pattern or files\" -seq \"pattern or files\" [...]\n\n";
    oss << "Estimate aperture correction pairs for `mergehdr run -fo`.\n\n";
    oss << "What it does:\n";
    oss << "  - Merges each sequence in raw space and samples the crop.\n";
    oss << "  - Uses the first sequence as the reference aperture.\n";
    oss << "  - Prints `fo = nominal,corrected ...` for use in `mergehdr run` or a profile.\n\n";
    oss << "Good input:\n";
    oss << "  Each -seq should be the same scene and lighting with only aperture changing.\n";
    oss << "  Run shutter calibration first and pass its value with -shutterc.\n\n";
    oss << "Options:\n";
    oss << "  -seq TEXT           One aperture series. Quote each sequence.\n";
    oss << "  -crop TEXT          Shared crop as \"L T W H\".\n";
    oss << "  -badpixels TEXT     Bad pixel coordinates as \"x,y x,y ...\".\n";
    oss << "  -shutterc, --shutterc FLOAT\n";
    oss << "                     Required shutter correction factor.\n";
    oss << "  -black, --black INTEGER\n";
    oss << "                     Override the sensor black level.\n";
    oss << "  -white, --white INTEGER\n";
    oss << "                     Override the usable sensor white level above black.\n";
    oss << "  -scale, --scale FLOAT\n";
    oss << "                     Calibration scale factor.\n";
    oss << "  -nd, --nd FLOAT    Neutral-density optical density; multiplier is 10^ND.\n";
    oss << "  -saturation, --saturation FLOAT\n";
    oss << "                     Highlight offset from sensor white.\n";
    oss << "  -range, --range FLOAT\n";
    oss << "                     Shadow floor in normalized sensor units.\n";
    oss << "  -fo, --fo TEXT     Aperture override pairs nominal,exact.\n";
    oss << "  --help              Show this help.\n";
    oss << "\nExample:\n";
    oss << "  mergehdr aperture -shutterc -1.02408e-05 -crop \"2400 2400 300 300\" \\\n";
    oss << "                    -seq \"f4/*.CR3\" -seq \"f5p6/*.CR3\" -seq \"f8/*.CR3\"\n";
    oss << "\nApply the result:\n";
    oss << "  mergehdr run -shutterc <value> -fo \"<printed pairs>\" \"*.CR3\" > out.hdr\n";
    return oss.str();
}

std::string colorCalibrateHelp(const std::string &command) {
    std::ostringstream oss;
    oss << "Usage:\n";
    oss << "  mergehdr " << command << " [options] reference test\n\n";
    oss << "Fit a camera color matrix against reference HDR or TSV data.\n\n";
    oss << "Inputs:\n";
    oss << "  reference           Reference HDR or tab-separated data (1 or 3 columns).\n";
    oss << "  test                Test HDR or tab-separated data.\n\n";
    oss << "How it works:\n";
    oss << "  - If an input is an HDR, sample the rectangles given by its cell file.\n";
    oss << "  - Convert the reference to XYZ using -refcol.\n";
    oss << "  - Interpret the test with --xyzcam, or from HDR headers if available.\n";
    oss << "  - Solve luminance, RGB, linear 3x3, and non-linear color refinements.\n";
    oss << "  - Print candidate XYZCAM matrices ranked by the same metrics used in linearhdr.\n\n";
    oss << "Notes:\n";
    oss << "  Cell files are required for HDR inputs in this native version.\n";
    oss << "  The first sampled patch should still be the white patch.\n\n";
    oss << "Options:\n";
    oss << "  -rc PATH            Cell file for the reference HDR.\n";
    oss << "  -tc PATH            Cell file for the test HDR.\n";
    oss << "  -refcol TEXT        Reference colorspace: rad, srgb, xyz, or 8 values.\n";
    oss << "  -xyzcam TEXT        Optional XYZ->camera override for the test.\n";
    oss << "  -refdataout PATH    Save reference data in XYZ.\n";
    oss << "  -testdataout PATH   Save sampled test data in its input space.\n";
    oss << "  -minimizer TEXT     Final perceptual objective: luv or lab.\n";
    oss << "  --verbose           Print per-cell values for the final solution.\n";
    oss << "  --help              Show this help.\n\n";
    oss << "Examples:\n";
    oss << "  mergehdr colorcalibrate ref.hdr test.hdr -rc ref_cells.txt -tc test_cells.txt\n";
    oss << "  mergehdr colorcalibrate ref.tsv test_raw.hdr -tc test_cells.txt -xyzcam \"...\"\n";
    return oss.str();
}

std::string chartCellsHelp(const std::string &command) {
    std::ostringstream oss;
    oss << "Usage:\n";
    oss << "  mergehdr " << command << " [options] image\n\n";
    oss << "Detect a cropped chart grid and write cell rectangles.\n\n";
    oss << "What it does:\n";
    oss << "  - Reads the HDR natively.\n";
    oss << "  - Finds candidate gutter bands between patches.\n";
    oss << "  - Keeps the most regular grid with consistent cell sizes.\n";
    oss << "  - Shrinks each cell inward to avoid patch borders.\n";
    oss << "  - Finds the most white-like patch from brightness, neutrality, and gray-strip context.\n\n";
    oss << "Options:\n";
    oss << "  -o, --output PATH   Destination cell file.\n";
    oss << "  -preview PATH       Optional JPEG preview with overlay boxes.\n";
    oss << "  -patches INTEGER    Total patch count. If set, choose cols x rows automatically.\n";
    oss << "  -cols INTEGER       Number of columns. Default: 6.\n";
    oss << "  -rows INTEGER       Number of rows. Default: 4.\n";
    oss << "  -inset FLOAT        Inset fraction for each cell. Default: 0.15.\n";
    oss << "  --white-first       Move the detected white patch to index 0.\n";
    oss << "  --row-major         Keep plain row-major order.\n";
    oss << "  --help              Show this help.\n\n";
    oss << "Example:\n";
    oss << "  mergehdr chartcells chart.hdr -o chart_cells.txt -preview chart_cells.jpg\n";
    return oss.str();
}

std::string hdrCropHelp(const std::string &command) {
    std::ostringstream oss;
    oss << "Usage:\n";
    oss << "  mergehdr " << command << " -crop L T W H -o output.hdr input.hdr\n\n";
    oss << "Crop an existing Radiance HDR while keeping its header metadata.\n\n";
    oss << "What it does:\n";
    oss << "  - Reads the source HDR natively.\n";
    oss << "  - Keeps the existing header lines.\n";
    oss << "  - Appends mergehdr crop metadata for the new file.\n";
    oss << "  - Writes a cropped Radiance HDR.\n\n";
    oss << "Options:\n";
    oss << "  -crop, --crop L T W H\n";
    oss << "                     Crop rectangle in top-left image coordinates.\n";
    oss << "  -o, --output PATH  Output HDR path.\n";
    oss << "  --help             Show this help.\n";
    return oss.str();
}

std::string hdrRotateHelp(const std::string &command) {
    std::ostringstream oss;
    oss << "Usage:\n";
    oss << "  mergehdr " << command << " -angle DEGREES -o output.hdr input.hdr\n\n";
    oss << "Rotate an existing Radiance HDR natively on a fixed-size canvas.\n\n";
    oss << "What it does:\n";
    oss << "  - Reads and writes Radiance RGBE directly.\n";
    oss << "  - Preserves the source dimensions and existing header metadata.\n";
    oss << "  - Resamples once with bilinear interpolation.\n";
    oss << "  - Fills pixels outside the rotated source with RGB 0 0 0.\n";
    oss << "  - Appends mergehdr rotation metadata.\n\n";
    oss << "Options:\n";
    oss << "  -angle, --angle FLOAT\n";
    oss << "                     Rotation in degrees. Positive is counter-clockwise;\n";
    oss << "                     negative is clockwise. For example, -1 is 1 degree clockwise.\n";
    oss << "  -o, --output PATH  Output HDR path.\n";
    oss << "  --help             Show this help.\n\n";
    oss << "Example:\n";
    oss << "  mergehdr rotate -angle -1 -o rotated.hdr input.hdr\n";
    return oss.str();
}

std::string shadowbandHelp(const std::string &command) {
    std::ostringstream oss;
    oss << "Usage:\n";
    oss << "  mergehdr " << command << " [options] H_input V_input ND_input\n\n";
    oss << "Blend horizontal, vertical, and no-shadowband fisheye inputs.\n\n";
    oss << "Inputs:\n";
    oss << "  H_input             Horizontal shadowband HDR, raw directory, or quoted raw glob.\n";
    oss << "  V_input             Vertical shadowband HDR, raw directory, or quoted raw glob.\n";
    oss << "  ND_input            No-shadowband HDR, raw directory, or quoted raw glob.\n\n";
    oss << "What it does:\n";
    oss << "  - If an input is raw, first runs native mergehdr raw merge on that set.\n";
    oss << "  - Optionally aligns H to V.\n";
    oss << "  - Optionally converts equisolid fisheye to equiangular.\n";
    oss << "  - Blends H/V safe regions.\n";
    oss << "  - Inpaints the band intersection around the sun.\n";
    oss << "  - Optionally writes a sky-only envmap HDR with SOLARSOURCE/SKYSOURCE headers.\n\n";
    oss << "Options:\n";
    oss << "  -o, --output PATH          Output blended HDR. Default: blended.hdr.\n";
    oss << "  --envmap PATH              Optional sky-only environment map HDR.\n";
    oss << "  --roh FLOAT                Horizontal band rotation correction in degrees.\n";
    oss << "  --rov FLOAT                Vertical band rotation correction in degrees.\n";
    oss << "  --sfov FLOAT               Valid field of view around source in ND image. Default: 2.\n";
    oss << "  --srcsize FLOAT            Source solid angle in steradians. Default: 6.7967e-05.\n";
    oss << "  --bw FLOAT                 Shadowband width in degrees. Default: 2.\n";
    oss << "  --margin INTEGER           Pixel margin cropped before alignment. Default: 20.\n";
    oss << "  --align / --no-align       Enable or disable H->V translation alignment.\n";
    oss << "  --fisheye / --no-fisheye   Enable or disable equisolid -> equiangular reprojection.\n";
    oss << "  --rawext TEXT              Raw extension for directory inputs. Default: CR2.\n";
    oss << "  --bandcfg TEXT             Profile or .cfg for H/V raw merge. Required for raw inputs.\n";
    oss << "  --ndcfg TEXT               Profile or .cfg for ND raw merge. Required for raw inputs.\n";
    oss << "  --sunloc X Y               Override sun pixel location in the ND image.\n";
    oss << "  --check PREFIX             Write debug mask/intermediate HDR files.\n";
    oss << "  --help                     Show this help.\n";
    return oss.str();
}

std::string hdrProjectHelp(const std::string &command) {
    std::ostringstream oss;
    oss << "Usage:\n";
    oss << "  mergehdr " << command << " --equisolid --equidistant -o output.hdr input.hdr\n\n";
    oss << "  mergehdr " << command << " --hemispherical --equidistant -o output.hdr input.hdr\n\n";
    oss << "  mergehdr " << command << " --equisolid --equidistant \\\n";
    oss << "    -p C1 C2 C3 C4 -o output.hdr input.hdr\n\n";
    oss << "Reproject an HDR while keeping its header metadata.\n\n";
    oss << "What it does:\n";
    oss << "  - Reads the source HDR natively.\n";
    oss << "  - Applies the native equisolid or Radiance hemispherical (-vth)\n";
    oss << "    to equidistant reprojection, or an empirical angle polynomial\n";
    oss << "    when -p is supplied.\n";
    oss << "  - Keeps the existing header lines.\n";
    oss << "  - Appends mergehdr projection metadata for the new file.\n";
    oss << "  - Writes a projected Radiance HDR.\n\n";
    oss << "Options:\n";
    oss << "  --equisolid        Treat the input HDR as equisolid.\n";
    oss << "  --hemispherical    Treat the input HDR as Radiance -vth.\n";
    oss << "  --equidistant      Write an equidistant projection.\n";
    oss << "  -p C1 C2 C3 C4    Override the ideal equisolid mapping with\n";
    oss << "                     rho=C1*phi+C2*phi^2+C3*phi^3+C4*phi^4.\n";
    oss << "                     phi is in radians and rho is normalized source radius.\n";
    oss << "  --source-radius R  Source fisheye radius in pixels. Default: width/2.\n";
    oss << "  --target-radius R  Target 90-degree radius in pixels. Default: width/2.\n";
    oss << "  --view TEXT        Radiance angular VIEW written for polynomial output.\n";
    oss << "  -o, --output PATH  Output HDR path.\n";
    oss << "  --help             Show this help.\n";
    return oss.str();
}

std::string extractHelp(const std::string &command) {
    std::ostringstream oss;
    oss << "Usage:\n";
    oss << "  mergehdr " << command << " [ -v LEFT BOTTOM WIDTH HEIGHT ]\n";
    oss << "                     [ -h LEFT BOTTOM WIDTH HEIGHT ]\n";
    oss << "                     [ -r LEFT BOTTOM WIDTH HEIGHT ]\n";
    oss << "                     [ -c X Y RadiusDegrees ]\n";
    oss << "                     [ -i ] [ -rgb ] [ -o output ] input.hdr\n\n";
    oss << "Extract section or region data from a Radiance HDR.\n\n";
    oss << "Modes:\n";
    oss << "  -v                 Average rows across the rectangle, ordered top -> bottom.\n";
    oss << "  -h                 Average columns across the rectangle, ordered left -> right.\n";
    oss << "  -r                 Rank rectangle pixels highest -> lowest.\n";
    oss << "  -c                 Rank circular region pixels highest -> lowest.\n";
    oss << "                     X/Y use the legacy bottom-origin pixel convention.\n";
    oss << "                     Radius is in degrees and needs valid equidistant VIEW metadata.\n\n";
    oss << "Options:\n";
    oss << "  -i                 Output a cropped/masked HDR instead of numeric data.\n";
    oss << "  -rgb               Output raw RGB channel values. If luminance metadata is\n";
    oss << "                     available, include luminance columns too.\n";
    oss << "  -o, --output PATH  Optional output path. Default: stdout.\n";
    oss << "  --help             Show this help.\n\n";
    oss << "Output columns:\n";
    oss << "  Luminance mode     cumulative_omega  L  cumulative_mean_L\n";
    oss << "  RGB mode           cumulative_omega  [L] R G B  cumulative_means...\n";
    oss << "                     If luminance cannot be resolved from metadata, RGB mode\n";
    oss << "                     omits L and uses RGB only.\n\n";
    oss << "Notes:\n";
    oss << "  - If VIEW/projection metadata is missing, solid angle falls back to a\n";
    oss << "    uniform per-pixel weight for -v/-h/-r.\n";
    oss << "  - Circle mode (-c) requires valid equidistant angular metadata.\n";
    oss << "  - Luminance uses the same metadata-aware source luminance path as evalglare.\n";
    return oss.str();
}

std::string viewVisibilityHelp(const std::string &command) {
    std::ostringstream oss;
    oss << "Usage:\n";
    oss << "  mergehdr " << command << " -ref reference.hdr -test test.hdr [options]\n\n";
    oss << "Compute native view visibility and write the packed final HDR.\n\n";
    oss << "Options:\n";
    oss << "  -ref, --ref PATH           Reference HDR.\n";
    oss << "  -test, --test PATH         Test HDR.\n";
    oss << "  -mask, --mask PATH         White mask HDR. If omitted, use the full view.\n";
    oss << "  -o, --output PATH          Output HDR path.\n";
    oss << "  --ppd FLOAT                Pixels per degree. Default: 30.\n";
    oss << "  --sensitivity FLOAT        Sensitivity correction. Default: -1.\n";
    oss << "  --spectral-emission PATH   Spectral emission CSV. Default: OLED profile.\n";
    oss << "  --detail TEXT              hdrvdp3 or contrast-proxy. Default: hdrvdp3.\n";
    oss << "  --input-color TEXT         rad, srgb, xyz (Radiance units), or xyz-cdm2 (XYZ already in cd/m2).\n";
    oss << "                             Values are multiplied by 179 (not for xyz-cdm2) and divided by EXPOSURE.\n";
    oss << "                             Default: rad.\n";
    oss << "  --debug-dir PATH           Write native debug dump directory.\n";
    oss << "  --help                     Show this help.\n\n";
    oss << "Output HDR channels:\n";
    oss << "  R = test contrast\n";
    oss << "  G = visibility\n";
    oss << "  B = reference contrast\n";
    return oss.str();
}

std::string viewVosCalculationHelp(const std::string &command) {
    std::ostringstream oss;
    oss << "Usage:\n";
    oss << "  mergehdr " << command << " --interior interior.hdr --exterior exterior.hdr \\\n";
    oss << "      [--window-distance METRES] [--window-mask mask.png|mask.hdr] --output folder\n\n";
    oss << "Estimate V_built and V_greenery from a registered interior/exterior HDR pair.\n";
    oss << "SAM3 supplies category masks, DA3 supplies metric depth, and VOS is integrated\n";
    oss << "per pixel using the HDR projection's exact solid angle.\n\n";
    oss << "Required options:\n";
    oss << "  --interior PATH           HDR view from the interior observer position.\n";
    oss << "  --exterior PATH           Matching unobstructed exterior HDR.\n";
    oss << "  -o, --output FOLDER       Output directory.\n\n";
    oss << "Optional options:\n";
    oss << "  --window-distance FLOAT   Perpendicular observer-to-window distance in metres.\n";
    oss << "                            If omitted, mergehdr asks before allowing DA3 to\n";
    oss << "                            estimate it from the interior/paired images.\n";
    oss << "  --window-mask PATH        PNG or HDR mask registered to the interior view.\n";
    oss << "                            White pixels are included; black pixels are excluded.\n";
    oss << "                            If omitted, the full valid VIEW is used as before.\n";
    oss << "  --help                    Show this help.\n\n";
    oss << "Primary results:\n";
    oss << "  pair_comparison.json reports V_built_m3 and V_greenery_m3. View satisfaction\n";
    oss << "  is intentionally not calculated until calibrated a, b, and c are supplied.\n";
    return oss.str();
}

std::string perceptualMapHelp(const std::string &command) {
    std::ostringstream oss;
    oss << "Usage:\n";
    oss << "  mergehdr " << command << " --map adaptation [options] input.hdr\n\n";
    oss << "Write an experimental single-image perceptual map as grayscale HDR.\n\n";
    oss << "Options:\n";
    oss << "  --map TEXT                 l, m, rod, lm, adaptation, detectable-contrast, eqv-luminance.\n";
    oss << "  -o, --output PATH          Output HDR path.\n";
    oss << "  --ppd FLOAT                Pixels per degree. Default: 30.\n";
    oss << "  --sensitivity FLOAT        Sensitivity correction. Default: -1.\n";
    oss << "  --spectral-emission PATH   Spectral emission CSV. Default: OLED profile.\n";
    oss << "  --help                     Show this help.\n";
    return oss.str();
}

ShutterOptions parseShutterOptions(const std::vector<std::string> &args) {
    ShutterOptions opts;
    std::vector<std::string> normalizedArgs = normalizeOptionSpellings(args);
    po::options_description desc("shutter");
    desc.add_options()
        ("help", "help")
        ("version", "version")
        ("config,c", po::value<std::string>(), "config")
        ("profile,p", po::value<std::string>(), "profile")
        ("seq", po::value<std::vector<std::string>>(&opts.sequenceTexts)->composing(), "sequence")
        ("crop", po::value<std::vector<std::string>>(&opts.cropTexts)->composing(), "crop")
        ("channel", po::value<std::string>(&opts.channel), "channel")
        ("dataout", po::value<std::string>(&opts.dataOut), "dataout")
        ("badpixels", po::value<std::string>(&opts.common.badpixelsText), "badpixels")
        ("black", po::value<int>(&opts.common.blacklevel), "black")
        ("white", po::value<int>(&opts.common.whitepoint), "white")
        ("scale", po::value<double>(&opts.common.scale), "scale")
        ("nd", po::value<double>(&opts.common.nd), "nd")
        ("saturation", po::value<double>(&opts.common.saturation), "saturation")
        ("range", po::value<double>(&opts.common.rangeValue), "range")
        ("shutterc", po::value<double>(&opts.common.shutterc), "shutterc")
        ("fo", po::value<std::string>(&opts.common.foText), "fo");

    std::vector<const char *> argvVec;
    argvVec.push_back("shutter");
    for (const std::string &arg : normalizedArgs)
        argvVec.push_back(arg.c_str());

    po::variables_map vm;
    po::store(po::command_line_parser(static_cast<int>(argvVec.size()), const_cast<char **>(argvVec.data()))
                  .options(desc)
                  .allow_unregistered()
                  .run(),
              vm);
    po::notify(vm);

    opts.help = vm.count("help") != 0;
    if (vm.count("black"))
        opts.common.blackOverride = true;
    if (vm.count("badpixels"))
        opts.common.badpixelsOverride = true;
    if (vm.count("white"))
        opts.common.whiteOverride = true;
    if (vm.count("scale"))
        opts.common.scaleOverride = true;
    if (vm.count("nd"))
        opts.common.ndOverride = true;
    if (vm.count("saturation"))
        opts.common.saturationOverride = true;
    if (vm.count("range"))
        opts.common.rangeOverride = true;
    if (vm.count("shutterc"))
        opts.common.shuttercOverride = true;
    if (vm.count("fo"))
        opts.common.foOverride = true;
    if (vm.count("crop"))
        opts.common.cropOverride = true;
    return opts;
}

ApertureOptions parseApertureOptions(const std::vector<std::string> &args) {
    ApertureOptions opts;
    std::vector<std::string> normalizedArgs = normalizeOptionSpellings(args);
    po::options_description desc("aperture");
    desc.add_options()
        ("help", "help")
        ("version", "version")
        ("config,c", po::value<std::string>(), "config")
        ("profile,p", po::value<std::string>(), "profile")
        ("seq", po::value<std::vector<std::string>>(&opts.sequenceTexts)->composing(), "sequence")
        ("crop", po::value<std::string>(&opts.cropText), "crop")
        ("badpixels", po::value<std::string>(&opts.common.badpixelsText), "badpixels")
        ("black", po::value<int>(&opts.common.blacklevel), "black")
        ("white", po::value<int>(&opts.common.whitepoint), "white")
        ("scale", po::value<double>(&opts.common.scale), "scale")
        ("nd", po::value<double>(&opts.common.nd), "nd")
        ("saturation", po::value<double>(&opts.common.saturation), "saturation")
        ("range", po::value<double>(&opts.common.rangeValue), "range")
        ("shutterc", po::value<double>(&opts.common.shutterc), "shutterc")
        ("fo", po::value<std::string>(&opts.common.foText), "fo");

    std::vector<const char *> argvVec;
    argvVec.push_back("aperture");
    for (const std::string &arg : normalizedArgs)
        argvVec.push_back(arg.c_str());

    po::variables_map vm;
    po::store(po::command_line_parser(static_cast<int>(argvVec.size()), const_cast<char **>(argvVec.data()))
                  .options(desc)
                  .allow_unregistered()
                  .run(),
              vm);
    po::notify(vm);

    opts.help = vm.count("help") != 0;
    if (vm.count("black"))
        opts.common.blackOverride = true;
    if (vm.count("badpixels"))
        opts.common.badpixelsOverride = true;
    if (vm.count("white"))
        opts.common.whiteOverride = true;
    if (vm.count("scale"))
        opts.common.scaleOverride = true;
    if (vm.count("nd"))
        opts.common.ndOverride = true;
    if (vm.count("saturation"))
        opts.common.saturationOverride = true;
    if (vm.count("range"))
        opts.common.rangeOverride = true;
    if (vm.count("shutterc"))
        opts.common.shuttercOverride = true;
    if (vm.count("fo"))
        opts.common.foOverride = true;
    if (vm.count("crop")) {
        opts.common.cropOverride = true;
        opts.common.crop = parseCropTextOption(opts.cropText);
    }
    return opts;
}

ColorCalibrateOptions parseColorCalibrateOptions(const std::vector<std::string> &args, bool &helpFlag) {
    ColorCalibrateOptions opts;
    helpFlag = false;
    std::string xyzcamText;
    std::vector<std::string> normalizedArgs = normalizeOptionSpellings(args);
    po::options_description desc("colorcalibrate");
    desc.add_options()
        ("help", "help")
        ("version", "version")
        ("config,c", po::value<std::string>(), "config")
        ("profile,p", po::value<std::string>(), "profile")
        ("rc", po::value<std::string>(&opts.referenceCellsPath), "reference cells")
        ("tc", po::value<std::string>(&opts.testCellsPath), "test cells")
        ("refcol", po::value<std::string>(&opts.referenceColor), "reference colorspace")
        ("xyzcam", po::value<std::string>(&xyzcamText), "xyzcam")
        ("refdataout", po::value<std::string>(&opts.referenceDataOut), "reference data output")
        ("testdataout", po::value<std::string>(&opts.testDataOut), "test data output")
        ("minimizer", po::value<std::string>(&opts.minimizer), "minimizer")
        ("verbose", po::bool_switch(&opts.verbose), "verbose")
        ("input-files", po::value<std::vector<std::string>>(), "reference test");

    po::positional_options_description positional;
    positional.add("input-files", -1);

    std::vector<const char *> argvVec;
    argvVec.push_back("colorcalibrate");
    for (const std::string &arg : normalizedArgs)
        argvVec.push_back(arg.c_str());

    po::variables_map vm;
    po::store(po::command_line_parser(static_cast<int>(argvVec.size()), const_cast<char **>(argvVec.data()))
                  .options(desc)
                  .positional(positional)
                  .run(),
              vm);
    po::notify(vm);

    helpFlag = vm.count("help") != 0;
    std::vector<std::string> inputs = vm.count("input-files")
        ? vm["input-files"].as<std::vector<std::string>>()
        : std::vector<std::string>();
    if (inputs.size() >= 1)
        opts.referencePath = inputs[0];
    if (inputs.size() >= 2)
        opts.testPath = inputs[1];
    if (!xyzcamText.empty()) {
        std::vector<float> xyzcamValues;
        for (const std::string &token : splitWhitespace(xyzcamText))
            xyzcamValues.push_back(std::stof(token));
        opts.xyzcamOverride.swap(xyzcamValues);
    }
    return opts;
}

ChartCellsCliOptions parseChartCellsOptions(const std::vector<std::string> &args) {
    ChartCellsCliOptions opts;
    std::vector<std::string> normalizedArgs = normalizeOptionSpellings(args);
    po::options_description desc("chartcells");
    desc.add_options()
        ("help", "help")
        ("version", "version")
        ("config,c", po::value<std::string>(), "config")
        ("profile,p", po::value<std::string>(), "profile")
        ("output,o", po::value<std::string>(&opts.outputPath), "output")
        ("preview", po::value<std::string>(&opts.previewPath), "preview")
        ("patches", po::value<int>(&opts.patchCount), "patches")
        ("cols", po::value<int>(&opts.columns), "columns")
        ("rows", po::value<int>(&opts.rows), "rows")
        ("inset", po::value<double>(&opts.inset), "inset")
        ("white-first", po::bool_switch()->default_value(false), "white-first")
        ("row-major", po::bool_switch()->default_value(false), "row-major")
        ("input-files", po::value<std::vector<std::string>>(), "image");

    po::positional_options_description positional;
    positional.add("input-files", -1);

    std::vector<const char *> argvVec;
    argvVec.push_back("chartcells");
    for (const std::string &arg : normalizedArgs)
        argvVec.push_back(arg.c_str());

    po::variables_map vm;
    po::store(po::command_line_parser(static_cast<int>(argvVec.size()), const_cast<char **>(argvVec.data()))
                  .options(desc)
                  .positional(positional)
                  .run(),
              vm);
    po::notify(vm);

    opts.help = vm.count("help") != 0;
    opts.layoutExplicit = vm.count("cols") != 0 || vm.count("rows") != 0;
    if (vm["white-first"].as<bool>())
        opts.whiteFirst = true;
    if (vm["row-major"].as<bool>())
        opts.whiteFirst = false;

    std::vector<std::string> inputs = vm.count("input-files")
        ? vm["input-files"].as<std::vector<std::string>>()
        : std::vector<std::string>();
    if (!inputs.empty())
        opts.imagePath = inputs.front();
    return opts;
}

HdrCropOptions parseHdrCropOptions(const std::vector<std::string> &args) {
    HdrCropOptions opts;
    std::vector<std::string> normalizedArgs = normalizeOptionSpellings(args);
    po::options_description desc("hdrcrop");
    desc.add_options()
        ("help", "help")
        ("version", "version")
        ("config,c", po::value<std::string>(), "config")
        ("profile,p", po::value<std::string>(), "profile")
        ("output,o", po::value<std::string>(&opts.outputPath), "output")
        ("crop", po::value<std::vector<int>>(&opts.crop)->multitoken(), "crop")
        ("input-files", po::value<std::vector<std::string>>(), "input");

    po::positional_options_description positional;
    positional.add("input-files", -1);

    std::vector<const char *> argvVec;
    argvVec.push_back("hdrcrop");
    for (const std::string &arg : normalizedArgs)
        argvVec.push_back(arg.c_str());

    po::variables_map vm;
    po::store(po::command_line_parser(static_cast<int>(argvVec.size()), const_cast<char **>(argvVec.data()))
                  .options(desc)
                  .positional(positional)
                  .run(),
              vm);
    po::notify(vm);

    opts.help = vm.count("help") != 0;
    std::vector<std::string> inputs = vm.count("input-files")
        ? vm["input-files"].as<std::vector<std::string>>()
        : std::vector<std::string>();
    if (!inputs.empty())
        opts.inputPath = inputs.front();
    return opts;
}

HdrRotateOptions parseHdrRotateOptions(const std::vector<std::string> &args) {
    HdrRotateOptions opts;
    std::vector<std::string> normalizedArgs = normalizeOptionSpellings(args);
    po::options_description desc("rotate");
    desc.add_options()
        ("help", "help")
        ("version", "version")
        ("config,c", po::value<std::string>(), "config")
        ("profile,p", po::value<std::string>(), "profile")
        ("output,o", po::value<std::string>(&opts.outputPath), "output")
        ("angle", po::value<double>(&opts.angleDegrees), "angle")
        ("input-files", po::value<std::vector<std::string>>(), "input");

    po::positional_options_description positional;
    positional.add("input-files", -1);

    std::vector<const char *> argvVec;
    argvVec.push_back("rotate");
    for (const std::string &arg : normalizedArgs)
        argvVec.push_back(arg.c_str());

    po::variables_map vm;
    po::store(po::command_line_parser(static_cast<int>(argvVec.size()), const_cast<char **>(argvVec.data()))
                  .options(desc)
                  .positional(positional)
                  .run(),
              vm);
    po::notify(vm);

    opts.help = vm.count("help") != 0;
    opts.angleSet = vm.count("angle") != 0;
    const std::vector<std::string> inputs = vm.count("input-files")
        ? vm["input-files"].as<std::vector<std::string>>()
        : std::vector<std::string>();
    if (inputs.size() > 1)
        throw std::runtime_error("rotate expects exactly one input HDR.");
    if (!inputs.empty())
        opts.inputPath = inputs.front();
    return opts;
}

HdrProjectOptions parseHdrProjectOptions(const std::vector<std::string> &args) {
    HdrProjectOptions opts;
    std::vector<std::string> normalizedArgs = normalizeOptionSpellings(args);
    std::vector<std::string> filteredArgs;
    filteredArgs.reserve(normalizedArgs.size());
    for (size_t i = 0; i < normalizedArgs.size(); ++i) {
        const std::string &token = normalizedArgs[i];
        if (token == "-p" || token == "--polynomial") {
            if (!opts.polynomialCoefficients.empty())
                throw std::runtime_error("convertprojection accepts only one -p polynomial.");
            if (i + 4 >= normalizedArgs.size())
                throw std::runtime_error("convertprojection -p expects exactly four coefficients: C1 C2 C3 C4.");
            for (size_t coefficient = 1; coefficient <= 4; ++coefficient) {
                size_t parsedCharacters = 0;
                const std::string &value = normalizedArgs[i + coefficient];
                const double parsed = std::stod(value, &parsedCharacters);
                if (parsedCharacters != value.size())
                    throw std::runtime_error("Invalid convertprojection polynomial coefficient: " + value);
                opts.polynomialCoefficients.push_back(parsed);
            }
            i += 4;
            continue;
        }
        filteredArgs.push_back(token);
    }

    po::options_description desc("convertprojection");
    desc.add_options()
        ("help", "help")
        ("version", "version")
        ("config,c", po::value<std::string>(), "config")
        ("profile", po::value<std::string>(), "profile")
        ("output,o", po::value<std::string>(&opts.outputPath), "output")
        ("equisolid", po::bool_switch(&opts.equisolid)->default_value(false), "equisolid")
        ("hemispherical", po::bool_switch(&opts.hemispherical)->default_value(false), "hemispherical")
        ("equidistant", po::bool_switch(&opts.equidistant)->default_value(false), "equidistant")
        ("source-radius", po::value<double>(&opts.sourceRadius), "source radius")
        ("target-radius", po::value<double>(&opts.targetRadius), "target radius")
        ("view", po::value<std::string>(&opts.projectionView), "Radiance VIEW")
        ("input-files", po::value<std::vector<std::string>>(), "input");

    po::positional_options_description positional;
    positional.add("input-files", -1);

    std::vector<const char *> argvVec;
    argvVec.push_back("convertprojection");
    for (const std::string &arg : filteredArgs)
        argvVec.push_back(arg.c_str());

    po::variables_map vm;
    po::store(po::command_line_parser(static_cast<int>(argvVec.size()), const_cast<char **>(argvVec.data()))
                  .options(desc)
                  .positional(positional)
                  .run(),
              vm);
    po::notify(vm);

    opts.help = vm.count("help") != 0;
    std::vector<std::string> inputs = vm.count("input-files")
        ? vm["input-files"].as<std::vector<std::string>>()
        : std::vector<std::string>();
    if (!inputs.empty())
        opts.inputPath = inputs.front();
    return opts;
}

EvalGlareCliOptions parseEvalGlareOptions(const std::vector<std::string> &args) {
    // Thin wrapper: the argument loop (original evalglare semantics plus the port-only long
    // options) lives in glareeval.cpp (parseEvalGlareArguments) so it can be built and tested
    // without boost.
    EvalGlareCliOptions opts;
    opts.parsed = parseEvalGlareArguments(args);
    opts.help = opts.parsed.showHelp;
    opts.native = opts.parsed.options;
    return opts;
}

ExtractOptions parseExtractOptions(const std::vector<std::string> &args) {
    ExtractOptions opts;
    const std::vector<std::string> normalizedArgs = normalizeOptionSpellings(args);
    for (size_t i = 0; i < normalizedArgs.size(); ++i) {
        const std::string &token = normalizedArgs[i];
        if (token == "--help") {
            opts.help = true;
            continue;
        }
        if (token == "-i" || token == "--image") {
            opts.imageOutput = true;
            continue;
        }
        if (token == "--rgb") {
            opts.rgbOutput = true;
            continue;
        }
        if (token == "-o" || token == "--output") {
            if (i + 1 >= normalizedArgs.size())
                throw std::runtime_error("-o/--output expects a path.");
            opts.outputPath = normalizedArgs[++i];
            continue;
        }
        if (token == "-v" || token == "--vertical-section") {
            if (i + 4 >= normalizedArgs.size())
                throw std::runtime_error("-v expects: left bottom width height");
            opts.mode = ExtractOptions::AverageOfRows;
            opts.left = std::stoi(normalizedArgs[++i]);
            opts.bottom = std::stoi(normalizedArgs[++i]);
            opts.width = std::stoi(normalizedArgs[++i]);
            opts.height = std::stoi(normalizedArgs[++i]);
            continue;
        }
        if (token == "-h" || token == "--horizontal-section") {
            if (i + 4 >= normalizedArgs.size())
                throw std::runtime_error("-h expects: left bottom width height");
            opts.mode = ExtractOptions::AverageOfColumns;
            opts.left = std::stoi(normalizedArgs[++i]);
            opts.bottom = std::stoi(normalizedArgs[++i]);
            opts.width = std::stoi(normalizedArgs[++i]);
            opts.height = std::stoi(normalizedArgs[++i]);
            continue;
        }
        if (token == "-r" || token == "--rect") {
            if (i + 4 >= normalizedArgs.size())
                throw std::runtime_error("-r expects: left bottom width height");
            opts.mode = ExtractOptions::RankedRectangle;
            opts.left = std::stoi(normalizedArgs[++i]);
            opts.bottom = std::stoi(normalizedArgs[++i]);
            opts.width = std::stoi(normalizedArgs[++i]);
            opts.height = std::stoi(normalizedArgs[++i]);
            continue;
        }
        if (token == "-c" || token == "--circle") {
            if (i + 3 >= normalizedArgs.size())
                throw std::runtime_error("-c expects: x y radiusDegrees");
            opts.mode = ExtractOptions::RankedCircle;
            opts.centerX = std::stoi(normalizedArgs[++i]);
            opts.centerY = std::stoi(normalizedArgs[++i]);
            opts.radiusDegrees = std::stod(normalizedArgs[++i]);
            continue;
        }
        if (!token.empty() && token[0] == '-') {
            throw std::runtime_error("Unknown extract option: " + token);
        }
        if (!opts.inputPath.empty())
            throw std::runtime_error("extract expects exactly one input HDR.");
        opts.inputPath = token;
    }
    return opts;
}

ViewVisibilitySummaryOptions::DetailMode parseViewVisibilityDetailMode(const std::string &value) {
    const std::string token = lowerCopy(value);
    if (token.empty() || token == "hdrvdp3" || token == "hdr-vdp3" || token == "hdrvdp")
        return ViewVisibilitySummaryOptions::DetailMode::HdrvdpSubset;
    if (token == "contrast-proxy" || token == "contrast_proxy" || token == "proxy")
        return ViewVisibilitySummaryOptions::DetailMode::ContrastProxy;
    throw std::runtime_error("Unsupported --detail value: " + value);
}

ViewVisibilitySummaryOptions::InputColor parseViewVisibilityInputColor(const std::string &value) {
    const std::string token = lowerCopy(value);
    if (token.empty() || token == "rad" || token == "radiance")
        return ViewVisibilitySummaryOptions::InputColor::Rad;
    if (token == "srgb")
        return ViewVisibilitySummaryOptions::InputColor::Srgb;
    if (token == "xyz")
        return ViewVisibilitySummaryOptions::InputColor::Xyz;
    if (token == "xyz-cdm2" || token == "xyz_cdm2")
        return ViewVisibilitySummaryOptions::InputColor::XyzCdm2;
    throw std::runtime_error("Unsupported --input-color value: " + value);
}

PerceptualMapKind parsePerceptualMapKind(const std::string &value) {
    const std::string token = lowerCopy(value);
    if (token == "l")
        return PerceptualMapKind::L;
    if (token == "m")
        return PerceptualMapKind::M;
    if (token == "rod" || token == "r")
        return PerceptualMapKind::Rod;
    if (token == "lm" || token == "l+m" || token == "lplusm")
        return PerceptualMapKind::LPlusM;
    if (token == "adaptation" || token == "adapt")
        return PerceptualMapKind::Adaptation;
    if (token == "detectable-contrast" || token == "detectable_contrast" || token == "detectablecontrast" ||
        token == "detectable" || token == "contrast")
        return PerceptualMapKind::DetectableContrast;
    if (token == "eqv-luminance" || token == "eqv_luminance" || token == "equiv-luminance" ||
        token == "equiv_luminance" || token == "equivlum" || token == "eqvlum")
        return PerceptualMapKind::EqvLuminance;
    throw std::runtime_error("Unsupported --map value: " + value);
}

std::string perceptualMapKindOutputName(PerceptualMapKind kind) {
    switch (kind) {
        case PerceptualMapKind::L: return "l";
        case PerceptualMapKind::M: return "m";
        case PerceptualMapKind::Rod: return "rod";
        case PerceptualMapKind::LPlusM: return "lm";
        case PerceptualMapKind::Adaptation: return "adaptation";
        case PerceptualMapKind::DetectableContrast: return "detectable_contrast";
        case PerceptualMapKind::EqvLuminance: return "eqv_luminance";
        default: return "map";
    }
}

ViewVisibilityCliOptions parseViewVisibilityOptions(const std::vector<std::string> &args) {
    ViewVisibilityCliOptions opts;
    const std::vector<std::string> normalizedArgs = normalizeOptionSpellings(args);
    po::options_description desc("view_visibility");
    std::string detail = "hdrvdp3";
    std::string inputColor = "rad";
    desc.add_options()
        ("help", "help")
        ("version", "version")
        ("config,c", po::value<std::string>(), "config")
        ("profile,p", po::value<std::string>(), "profile")
        ("ref", po::value<std::string>(&opts.native.referencePath), "reference")
        ("reference", po::value<std::string>(&opts.native.referencePath), "reference")
        ("test", po::value<std::string>(&opts.native.testPath), "test")
        ("mask", po::value<std::string>(&opts.native.maskPath), "mask")
        ("output,o", po::value<std::string>(&opts.outputPath), "output")
        ("ppd", po::value<double>(&opts.native.pixelsPerDegree)->default_value(30.0), "pixels per degree")
        ("sensitivity", po::value<double>(&opts.native.sensitivityCorrection)->default_value(-1.0), "sensitivity correction")
        ("spectral-emission", po::value<std::string>(&opts.native.spectralEmissionPath), "spectral emission")
        ("detail", po::value<std::string>(&detail)->default_value("hdrvdp3"), "detail mode")
        ("input-color", po::value<std::string>(&inputColor)->default_value("rad"), "input color space")
        ("debug-dir", po::value<std::string>(&opts.debugDir), "debug dump");

    std::vector<const char *> argvVec;
    argvVec.push_back("view_visibility");
    for (size_t i = 0; i < normalizedArgs.size(); ++i)
        argvVec.push_back(normalizedArgs[i].c_str());

    po::variables_map vm;
    po::store(po::command_line_parser(static_cast<int>(argvVec.size()), const_cast<char **>(argvVec.data())).options(desc).run(), vm);
    po::notify(vm);

    opts.help = vm.count("help") != 0;
    opts.writeDebugDump = !opts.debugDir.empty();
    opts.native.detailMode = parseViewVisibilityDetailMode(detail);
    opts.native.inputColor = parseViewVisibilityInputColor(inputColor);
    return opts;
}

ViewVosCalculationOptions parseViewVosCalculationOptions(const std::vector<std::string> &args) {
    ViewVosCalculationOptions opts;
    const std::vector<std::string> normalizedArgs = normalizeOptionSpellings(args);
    po::options_description desc("view-vos-calculation");
    desc.add_options()
        ("help", "help")
        ("version", "version")
        ("config,c", po::value<std::string>(), "config")
        ("profile,p", po::value<std::string>(), "profile")
        ("interior", po::value<std::string>(&opts.interiorPath), "interior HDR")
        ("exterior", po::value<std::string>(&opts.exteriorPath), "exterior HDR")
        ("window-distance", po::value<double>(&opts.windowDistance), "observer-to-window distance")
        ("window-mask", po::value<std::string>(&opts.windowMaskPath), "white window mask")
        ("output,o", po::value<std::string>(&opts.outputDir), "output directory");

    std::vector<const char *> argvVec;
    argvVec.push_back("view-vos-calculation");
    for (size_t i = 0; i < normalizedArgs.size(); ++i)
        argvVec.push_back(normalizedArgs[i].c_str());

    po::variables_map vm;
    po::store(
        po::command_line_parser(
            static_cast<int>(argvVec.size()), const_cast<char **>(argvVec.data()))
            .options(desc)
            .run(),
        vm);
    po::notify(vm);
    opts.help = vm.count("help") != 0;
    opts.windowDistanceSet = vm.count("window-distance") != 0;
    return opts;
}

PerceptualMapCliOptions parsePerceptualMapOptions(const std::vector<std::string> &args) {
    PerceptualMapCliOptions opts;
    const std::vector<std::string> normalizedArgs = normalizeOptionSpellings(args);
    po::options_description desc("perceptualmap");
    desc.add_options()
        ("help", "help")
        ("version", "version")
        ("config,c", po::value<std::string>(), "config")
        ("profile,p", po::value<std::string>(), "profile")
        ("map", po::value<std::string>(&opts.mapName), "map kind")
        ("output,o", po::value<std::string>(&opts.outputPath), "output")
        ("ppd", po::value<double>(&opts.native.pixelsPerDegree)->default_value(30.0), "pixels per degree")
        ("sensitivity", po::value<double>(&opts.native.sensitivityCorrection)->default_value(-1.0), "sensitivity correction")
        ("spectral-emission", po::value<std::string>(&opts.native.spectralEmissionPath), "spectral emission")
        ("input-files", po::value<std::vector<std::string> >(), "input");

    po::positional_options_description positional;
    positional.add("input-files", -1);

    std::vector<const char *> argvVec;
    argvVec.push_back("perceptualmap");
    for (size_t i = 0; i < normalizedArgs.size(); ++i)
        argvVec.push_back(normalizedArgs[i].c_str());

    po::variables_map vm;
    po::store(po::command_line_parser(static_cast<int>(argvVec.size()), const_cast<char **>(argvVec.data()))
                  .options(desc)
                  .positional(positional)
                  .run(),
              vm);
    po::notify(vm);

    opts.help = vm.count("help") != 0;
    const std::vector<std::string> inputs = vm.count("input-files")
        ? vm["input-files"].as<std::vector<std::string> >()
        : std::vector<std::string>();
    if (!inputs.empty())
        opts.native.inputPath = inputs.front();
    return opts;
}

ShadowbandCliOptions parseShadowbandOptions(const std::vector<std::string> &args) {
    ShadowbandCliOptions opts;
    std::vector<std::string> normalizedArgs = normalizeOptionSpellings(args);
    std::vector<std::string> filteredArgs;
    filteredArgs.reserve(normalizedArgs.size());
    std::vector<int> sunlocValues;
    for (size_t i = 0; i < normalizedArgs.size(); ++i) {
        const std::string &token = normalizedArgs[i];
        if (token == "--sunloc") {
            if (i + 2 >= normalizedArgs.size())
                throw std::runtime_error("shadowband --sunloc expects: x y");
            sunlocValues.push_back(std::stoi(normalizedArgs[i + 1]));
            sunlocValues.push_back(std::stoi(normalizedArgs[i + 2]));
            i += 2;
            continue;
        }
        filteredArgs.push_back(token);
    }

    po::options_description desc("shadowband");
    desc.add_options()
        ("help", "help")
        ("version", "version")
        ("config,c", po::value<std::string>(), "config")
        ("profile,p", po::value<std::string>(), "profile")
        ("output,o", po::value<std::string>(&opts.outputPath)->default_value("blended.hdr"), "output")
        ("envmap", po::value<std::string>(&opts.native.envmapPath), "optional sky-only environment map output")
        ("roh", po::value<double>(&opts.native.roh)->default_value(0.0), "horizontal band rotation correction in degrees")
        ("rov", po::value<double>(&opts.native.rov)->default_value(0.0), "vertical band rotation correction in degrees")
        ("sfov", po::value<double>(&opts.native.sfov)->default_value(2.0), "valid field of view around source in ND image")
        ("srcsize", po::value<double>(&opts.native.srcsize)->default_value(6.7967e-05), "source solid angle in steradians")
        ("bw", po::value<double>(&opts.native.bw)->default_value(2.0), "shadow band width in degrees")
        ("margin", po::value<int>(&opts.native.margin)->default_value(20), "pixel margin to crop before alignment/reprojection")
        ("align", po::bool_switch(&opts.native.align)->default_value(true), "align H to V before blending")
        ("no-align", po::bool_switch()->default_value(false), "disable alignment")
        ("fisheye", po::bool_switch(&opts.native.fisheye)->default_value(true), "convert equisolid fisheye to equiangular")
        ("no-fisheye", po::bool_switch()->default_value(false), "skip fisheye reprojection")
        ("rawext", po::value<std::string>(&opts.rawExt)->default_value("CR2"), "raw extension for directory inputs")
        ("bandcfg", po::value<std::string>(&opts.bandCfg), "profile or .cfg for H/V raw merge")
        ("ndcfg", po::value<std::string>(&opts.ndCfg), "profile or .cfg for ND raw merge")
        ("check", po::value<std::string>(&opts.native.checkPrefix), "debug output prefix")
        ("input-files", po::value<std::vector<std::string> >(), "input");

    po::positional_options_description positional;
    positional.add("input-files", -1);

    std::vector<const char *> argvVec;
    argvVec.push_back("shadowband");
    for (size_t i = 0; i < filteredArgs.size(); ++i)
        argvVec.push_back(filteredArgs[i].c_str());

    po::variables_map vm;
    po::store(po::command_line_parser(static_cast<int>(argvVec.size()), const_cast<char **>(argvVec.data()))
                  .options(desc)
                  .positional(positional)
                  .run(),
              vm);
    po::notify(vm);

    opts.help = vm.count("help") != 0;
    if (vm["no-align"].as<bool>())
        opts.native.align = false;
    if (vm["no-fisheye"].as<bool>())
        opts.native.fisheye = false;

    const std::vector<std::string> inputs = vm.count("input-files")
        ? vm["input-files"].as<std::vector<std::string> >()
        : std::vector<std::string>();
    if (!inputs.empty())
        opts.horizontalPath = inputs[0];
    if (inputs.size() > 1)
        opts.verticalPath = inputs[1];
    if (inputs.size() > 2)
        opts.noShadowPath = inputs[2];
    if (!sunlocValues.empty()) {
        opts.native.haveSunloc = true;
        opts.native.sunPixelX = sunlocValues[0];
        opts.native.sunPixelY = sunlocValues[1];
    }
    return opts;
}

void validateShutterOptions(const ShutterOptions &opts) {
    if (opts.sequenceTexts.empty())
        throw std::runtime_error("At least one -seq is required.");
    std::string channel = lowerCopy(opts.channel);
    if (channel != "r" && channel != "red" && channel != "g" && channel != "green" &&
            channel != "b" && channel != "blue")
        throw std::runtime_error("Channel must be r, g, or b.");
    for (const std::string &cropText : opts.cropTexts)
        parseCropTextOption(cropText);
}

void validateApertureOptions(const ApertureOptions &opts) {
    if (opts.sequenceTexts.size() < 2)
        throw std::runtime_error("Aperture calibration needs at least two -seq inputs.");
    if (!opts.common.shuttercOverride)
        throw std::runtime_error("-shutterc is required for aperture calibration.");
}

void validateColorCalibrateOptions(const ColorCalibrateOptions &opts) {
    if (opts.referencePath.empty() || opts.testPath.empty())
        throw std::runtime_error("colorcalibrate expects both reference and test inputs.");
    std::string minimizer = lowerCopy(opts.minimizer);
    if (minimizer != "luv" && minimizer != "lab")
        throw std::runtime_error("-minimizer must be either luv or lab.");
    if (!opts.referenceCellsPath.empty() && !fileExists(opts.referenceCellsPath))
        throw std::runtime_error("Reference cell file not found: " + opts.referenceCellsPath);
    if (!opts.testCellsPath.empty() && !fileExists(opts.testCellsPath))
        throw std::runtime_error("Test cell file not found: " + opts.testCellsPath);
}

void validateChartCellsOptions(const ChartCellsCliOptions &opts) {
    if (opts.imagePath.empty())
        throw std::runtime_error("chartcells expects one input image.");
    if (!fileExists(opts.imagePath))
        throw std::runtime_error("Input image not found: " + opts.imagePath);
    if (opts.outputPath.empty())
        throw std::runtime_error("chartcells requires -o/--output.");
    if (opts.patchCount < 0)
        throw std::runtime_error("-patches must be positive.");
    if (opts.patchCount > 0 && opts.layoutExplicit && opts.patchCount != opts.columns * opts.rows)
        throw std::runtime_error("-patches must match -cols x -rows when both are set.");
    if (opts.columns <= 0 || opts.rows <= 0)
        throw std::runtime_error("Chart dimensions must be positive.");
    if (!(opts.inset >= 0.0 && opts.inset < 0.45))
        throw std::runtime_error("-inset must be between 0 and 0.45.");
}

void validateHdrCropOptions(const HdrCropOptions &opts) {
    if (opts.inputPath.empty())
        throw std::runtime_error("hdrcrop expects one input HDR.");
    if (!fileExists(opts.inputPath))
        throw std::runtime_error("Input HDR not found: " + opts.inputPath);
    if (opts.outputPath.empty())
        throw std::runtime_error("hdrcrop requires -o/--output.");
    if (opts.crop.size() != 4)
        throw std::runtime_error("Crop expects four integers: left top width height.");
}

void validateHdrRotateOptions(const HdrRotateOptions &opts) {
    if (opts.inputPath.empty())
        throw std::runtime_error("rotate expects one input HDR.");
    if (!fileExists(opts.inputPath))
        throw std::runtime_error("Input HDR not found: " + opts.inputPath);
    if (opts.outputPath.empty())
        throw std::runtime_error("rotate requires -o/--output.");
    if (!opts.angleSet)
        throw std::runtime_error("rotate requires -angle/--angle.");
    if (!std::isfinite(opts.angleDegrees))
        throw std::runtime_error("rotate angle must be finite.");
}

void validateHdrProjectOptions(const HdrProjectOptions &opts) {
    if (opts.inputPath.empty())
        throw std::runtime_error("convertprojection expects one input HDR.");
    if (!fileExists(opts.inputPath))
        throw std::runtime_error("Input HDR not found: " + opts.inputPath);
    if (opts.outputPath.empty())
        throw std::runtime_error("convertprojection requires -o/--output.");
    if (!opts.equidistant)
        throw std::runtime_error("convertprojection requires --equidistant as the output projection.");
    if (opts.equisolid == opts.hemispherical)
        throw std::runtime_error("convertprojection requires exactly one source projection: --equisolid or --hemispherical.");
    if (opts.hemispherical && !opts.polynomialCoefficients.empty())
        throw std::runtime_error("convertprojection -p is only supported with --equisolid.");
    if (!opts.polynomialCoefficients.empty()) {
        if (opts.polynomialCoefficients.size() != 4)
            throw std::runtime_error("convertprojection -p requires exactly four polynomial coefficients.");
        if (opts.sourceRadius < 0.0 || opts.targetRadius < 0.0)
            throw std::runtime_error("convertprojection projection radii cannot be negative.");
        const double halfPi = 0.5 * std::acos(-1.0);
        for (int sample = 0; sample <= 4096; ++sample) {
            const double phi = halfPi * static_cast<double>(sample) / 4096.0;
            double value = 0.0;
            double derivative = 0.0;
            evaluateRadialPolynomial(opts.polynomialCoefficients, phi, value, derivative);
            if (value < 0.0 || !(derivative > 0.0) ||
                    !std::isfinite(value) || !std::isfinite(derivative)) {
                throw std::runtime_error(
                    "convertprojection -p must be finite, non-negative, and strictly monotonic on phi=[0,pi/2].");
            }
        }
    } else if (opts.sourceRadius > 0.0 || opts.targetRadius > 0.0) {
        throw std::runtime_error("--source-radius and --target-radius require convertprojection -p.");
    }
}

void validateEvalGlareOptions(const EvalGlareOptions &opts) {
    // Only checks the original evalglare does not need to make (port restrictions); everything
    // else is handled by the engine with the original's messages.
    if (opts.inputPath.empty())
        throw std::runtime_error("evalglare expects one input HDR.");
    if (opts.inputPath != "-" && !fileExists(opts.inputPath))
        throw std::runtime_error("Input HDR not found: " + opts.inputPath);
    if (!opts.maskPath.empty() && !fileExists(opts.maskPath))
        throw std::runtime_error("Mask HDR not found: " + opts.maskPath);
    const auto sameAsInput = [&](const std::string &path) {
        return opts.inputPath != "-" &&
               fs::absolute(fs::path(path)).lexically_normal() == fs::absolute(fs::path(opts.inputPath)).lexically_normal();
    };
    if (!opts.checkPath.empty()) {
        const std::string extension = lowerExtension(opts.checkPath);
        if (extension != ".hdr" && extension != ".pic")
            throw std::runtime_error("evalglare check output must be .hdr or .pic.");
        if (sameAsInput(opts.checkPath))
            throw std::runtime_error("evalglare check output must not overwrite the input HDR.");
    }
    if (!opts.correctionOutputPath.empty() && sameAsInput(opts.correctionOutputPath))
        throw std::runtime_error("evalglare -m/-M/-N/-O output must not overwrite the input HDR.");
    if (opts.localOnly && opts.maskPath.empty() && opts.zoneCount == 0)
        throw std::runtime_error("evalglare --local-only requires either -A mask.hdr or -l/-L zoning.");
}

void validateViewVisibilityOptions(const ViewVisibilityCliOptions &opts) {
    if (opts.native.referencePath.empty())
        throw std::runtime_error("view_visibility requires -ref/--ref.");
    if (opts.native.testPath.empty())
        throw std::runtime_error("view_visibility requires -test/--test.");
    if (!fileExists(opts.native.referencePath))
        throw std::runtime_error("Reference HDR not found: " + opts.native.referencePath);
    if (!fileExists(opts.native.testPath))
        throw std::runtime_error("Test HDR not found: " + opts.native.testPath);
    if (!opts.native.maskPath.empty() && !fileExists(opts.native.maskPath))
        throw std::runtime_error("Mask HDR not found: " + opts.native.maskPath);
    if (!(opts.native.pixelsPerDegree > 0.0))
        throw std::runtime_error("view_visibility --ppd must be positive.");
    if (!opts.outputPath.empty() && lowerExtension(opts.outputPath) != ".hdr" && lowerExtension(opts.outputPath) != ".pic")
        throw std::runtime_error("view_visibility output must be .hdr or .pic.");
}

void validateViewVosCalculationOptions(const ViewVosCalculationOptions &opts) {
    if (opts.interiorPath.empty())
        throw std::runtime_error("view-vos-calculation requires --interior.");
    if (opts.exteriorPath.empty())
        throw std::runtime_error("view-vos-calculation requires --exterior.");
    if (opts.outputDir.empty())
        throw std::runtime_error("view-vos-calculation requires --output.");
    if (!fileExists(opts.interiorPath) || isDirectoryPath(opts.interiorPath))
        throw std::runtime_error("Interior HDR not found: " + opts.interiorPath);
    if (!fileExists(opts.exteriorPath) || isDirectoryPath(opts.exteriorPath))
        throw std::runtime_error("Exterior HDR not found: " + opts.exteriorPath);
    if (!opts.windowMaskPath.empty() &&
            (!fileExists(opts.windowMaskPath) || isDirectoryPath(opts.windowMaskPath))) {
        throw std::runtime_error("Window mask not found: " + opts.windowMaskPath);
    }
    if (opts.windowDistanceSet &&
            (!std::isfinite(opts.windowDistance) || opts.windowDistance < 0.0)) {
        throw std::runtime_error("--window-distance must be a finite, non-negative value in metres.");
    }
    if (fileExists(opts.outputDir) && !isDirectoryPath(opts.outputDir))
        throw std::runtime_error("--output must be a directory, not a file: " + opts.outputDir);
}

void validatePerceptualMapOptions(const PerceptualMapCliOptions &opts) {
    if (opts.native.inputPath.empty())
        throw std::runtime_error("perceptualmap expects one input HDR.");
    if (!fileExists(opts.native.inputPath))
        throw std::runtime_error("Input HDR not found: " + opts.native.inputPath);
    if (opts.mapName.empty())
        throw std::runtime_error("perceptualmap requires --map.");
    if (!(opts.native.pixelsPerDegree > 0.0))
        throw std::runtime_error("perceptualmap --ppd must be positive.");
    if (!opts.outputPath.empty() && lowerExtension(opts.outputPath) != ".hdr" && lowerExtension(opts.outputPath) != ".pic")
        throw std::runtime_error("perceptualmap output must be .hdr or .pic.");
}

GlobalOptions shadowbandGlobalFromConfigSpec(const std::string &spec) {
    GlobalOptions global;
    if (spec.empty())
        return global;
    if (shadowbandConfigSpecLooksLikePath(spec))
        global.configPath = spec;
    else
        global.profileName = spec;
    return global;
}

void validateShadowbandConfigSpec(const std::string &spec, const std::string &label) {
    if (spec.empty())
        throw std::runtime_error("shadowband raw input requires " + label + ".");
    const GlobalOptions global = shadowbandGlobalFromConfigSpec(spec);
    const std::string configPath = configPathFromGlobal(global);
    if (configPath.empty() || !fileExists(configPath))
        throw std::runtime_error("shadowband could not resolve " + label + ": " + spec);
}

bool shadowbandInputNeedsRawMerge(const std::string &inputPath) {
    if (isDirectoryPath(inputPath))
        return true;
    if (hasWildcard(inputPath))
        return true;
    return !isRadianceHdrPath(inputPath);
}

void validateShadowbandOptions(const ShadowbandCliOptions &opts) {
    if (opts.horizontalPath.empty() || opts.verticalPath.empty() || opts.noShadowPath.empty())
        throw std::runtime_error("shadowband expects three inputs: H_input V_input ND_input");
    if (!fileExists(opts.horizontalPath) && !hasWildcard(opts.horizontalPath))
        throw std::runtime_error("Horizontal shadowband input not found: " + opts.horizontalPath);
    if (!fileExists(opts.verticalPath) && !hasWildcard(opts.verticalPath))
        throw std::runtime_error("Vertical shadowband input not found: " + opts.verticalPath);
    if (!fileExists(opts.noShadowPath) && !hasWildcard(opts.noShadowPath))
        throw std::runtime_error("No-shadowband input not found: " + opts.noShadowPath);
    if (shadowbandInputNeedsRawMerge(opts.horizontalPath))
        validateShadowbandConfigSpec(opts.bandCfg, "--bandcfg");
    if (shadowbandInputNeedsRawMerge(opts.verticalPath))
        validateShadowbandConfigSpec(opts.bandCfg, "--bandcfg");
    if (shadowbandInputNeedsRawMerge(opts.noShadowPath))
        validateShadowbandConfigSpec(opts.ndCfg, "--ndcfg");
    if (!(opts.native.sfov > 0.0))
        throw std::runtime_error("shadowband --sfov must be positive.");
    if (!(opts.native.srcsize > 0.0))
        throw std::runtime_error("shadowband --srcsize must be positive.");
    if (!(opts.native.bw > 0.0))
        throw std::runtime_error("shadowband --bw must be positive.");
    if (opts.native.margin < 0)
        throw std::runtime_error("shadowband --margin must be non-negative.");
    if (trimCopy(opts.rawExt).empty())
        throw std::runtime_error("shadowband --rawext must not be empty.");
    if (!opts.outputPath.empty() && lowerExtension(opts.outputPath) != ".hdr" && lowerExtension(opts.outputPath) != ".pic")
        throw std::runtime_error("shadowband output must be .hdr or .pic.");
    if (!opts.native.envmapPath.empty() && lowerExtension(opts.native.envmapPath) != ".hdr" && lowerExtension(opts.native.envmapPath) != ".pic")
        throw std::runtime_error("shadowband --envmap output must be .hdr or .pic.");
}

std::string formatShutterKey(double shutter) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4) << shutter;
    return oss.str();
}

CalibrationSampleResult parseCalibrationSample(const std::vector<std::string> &captured) {
    std::regex pattern(R"(^RAW_SAMPLE channel=\d+ avg=([^\s]+) fraction=([^\s]+) valid=(\d+) total=(\d+)$)");
    for (std::vector<std::string>::const_reverse_iterator it = captured.rbegin(); it != captured.rend(); ++it) {
        std::smatch match;
        if (std::regex_match(*it, match, pattern)) {
            CalibrationSampleResult result;
            result.average = std::stod(match[1].str());
            result.fraction = std::stod(match[2].str());
            result.validSamples = static_cast<size_t>(std::stoull(match[3].str()));
            result.totalSamples = static_cast<size_t>(std::stoull(match[4].str()));
            return result;
        }
    }
    throw std::runtime_error("Could not parse calibration sample output from mergehdrcore.");
}

int runQuietCommand(const std::string &command, std::vector<std::string> &captured) {
    FILE *pipe = openProcessPipe(command);
    if (!pipe)
        throw std::runtime_error("Unable to launch mergehdrcore.");

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        captured.push_back(line);
    }

    return closeProcessPipe(pipe);
}

CalibrationSampleResult runCalibrationSample(const CalibrationCommonOptions &opts,
        const std::vector<std::string> &inputs, const std::vector<int> &crop,
        const std::string &channel) {
    if (inputs.empty())
        throw std::runtime_error("Calibration sample requires at least one input file.");

    ResolvedRawLevels levels = resolveRawLevels(opts.blackOverride, opts.blacklevel,
        opts.whiteOverride, opts.whitepoint, inputs);

    std::vector<std::string> commandArgs;
    commandArgs.push_back(coreBinaryPath());
    commandArgs.push_back("--samplechannel");
    commandArgs.push_back(lowerCopy(channel));
    commandArgs.push_back("--mergestyle");
    commandArgs.push_back("linearhdr");
    commandArgs.push_back("--linearhdrexposure");
    commandArgs.push_back("--exposurescale");
    commandArgs.push_back((boost::format("%1$.12g") % (opts.scale * std::pow(10.0, opts.nd))).str());
    commandArgs.push_back("--saturation");
    commandArgs.push_back((boost::format("%1$.12g") % std::max(0.5, std::min(0.999999, 1.0 - opts.saturation))).str());
    commandArgs.push_back("--range");
    commandArgs.push_back((boost::format("%1$.12g") % opts.rangeValue).str());
    if (opts.badpixelsOverride && !opts.badpixelsText.empty()) {
        commandArgs.push_back("--badpixels");
        commandArgs.push_back(opts.badpixelsText);
    }
    if (levels.coreBlacklevel >= 0) {
        commandArgs.push_back("--blacklevel");
        commandArgs.push_back(std::to_string(levels.coreBlacklevel));
    }
    if (levels.coreWhitepoint >= 0) {
        commandArgs.push_back("--whitepoint");
        commandArgs.push_back(std::to_string(levels.coreWhitepoint));
    }
    if (!crop.empty()) {
        commandArgs.push_back("--mergecrop");
        commandArgs.push_back((boost::format("%1%,%2%,%3%,%4%") % crop[0] % crop[1] % crop[2] % crop[3]).str());
    }
    if (opts.shuttercOverride) {
        commandArgs.push_back("--shutterc");
        commandArgs.push_back((boost::format("%1$.12g") % opts.shutterc).str());
    }
    if (opts.foOverride) {
        commandArgs.push_back("--fo");
        commandArgs.push_back(opts.foText);
    }
    for (size_t i = 0; i < inputs.size(); ++i)
        commandArgs.push_back(inputs[i]);

    std::vector<std::string> quoted;
    for (size_t i = 0; i < commandArgs.size(); ++i)
        quoted.push_back(processQuote(commandArgs[i]));

    std::vector<std::string> captured;
    int rc = runQuietCommand(joinStrings(quoted, " "), captured);
    if (rc != 0) {
        bool printed = false;
        for (size_t i = 0; i < captured.size(); ++i) {
            if (!shouldSuppressCoreLine(captured[i])) {
                std::cerr << captured[i] << std::endl;
                printed = true;
            }
        }
        if (!printed && !captured.empty())
            std::cerr << captured.back() << std::endl;
        throw std::runtime_error("Calibration sample failed.");
    }

    return parseCalibrationSample(captured);
}

struct ShutterMeasurement {
    double shutter = 0.0;
    double average = 0.0;
    double fraction = 0.0;
};

struct GroupedShutterMeasurement {
    std::string key;
    double shutter = 0.0;
    double average = 0.0;
};

std::vector<GroupedShutterMeasurement> groupShutterMeasurements(const std::vector<ShutterMeasurement> &samples) {
    struct Accum {
        double shutter = 0.0;
        double sum = 0.0;
        size_t count = 0;
    };
    std::map<std::string, Accum> grouped;
    for (size_t i = 0; i < samples.size(); ++i) {
        std::string key = formatShutterKey(samples[i].shutter);
        Accum &acc = grouped[key];
        acc.shutter = samples[i].shutter;
        acc.sum += samples[i].average;
        ++acc.count;
    }

    std::vector<GroupedShutterMeasurement> result;
    for (std::map<std::string, Accum>::const_iterator it = grouped.begin(); it != grouped.end(); ++it) {
        GroupedShutterMeasurement m;
        m.key = it->first;
        m.shutter = it->second.shutter;
        m.average = it->second.sum / static_cast<double>(it->second.count);
        result.push_back(m);
    }
    std::sort(result.begin(), result.end(), [](const GroupedShutterMeasurement &a, const GroupedShutterMeasurement &b) {
        return a.shutter < b.shutter;
    });
    return result;
}

std::pair<double, double> fitExponentialInverse(const std::vector<std::pair<double, double>> &samples) {
    if (samples.size() < 2)
        throw std::runtime_error("Need at least two shutter samples to fit a correction curve.");

    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (size_t i = 0; i < samples.size(); ++i) {
        double x = samples[i].first;
        double y = samples[i].second;
        if (!(y > 0.0))
            throw std::runtime_error("Encountered a non-positive shutter sample while fitting.");
        double ly = std::log(y);
        sx += x;
        sy += ly;
        sxx += x * x;
        sxy += x * ly;
    }
    double n = static_cast<double>(samples.size());
    double denom = n * sxx - sx * sx;
    if (std::abs(denom) < 1e-12)
        throw std::runtime_error("Could not fit shutter correction: sample shutters are degenerate.");
    double A = (n * sxy - sx * sy) / denom;
    double logB = (sy - A * sx) / n;
    return std::make_pair(A, std::exp(logB));
}

int printShutterCalibration(const GlobalOptions &global, const ShutterOptions &opts) {
    std::string configPath = configPathFromGlobal(global);
    ConfigValues cfg = loadConfigValues(configPath);
    ShutterOptions resolved = opts;
    applyCalibrationDefaults(resolved.common, cfg);
    validateShutterOptions(resolved);

    std::vector<std::vector<int>> crops;
    if (!resolved.cropTexts.empty()) {
        for (size_t i = 0; i < resolved.cropTexts.size(); ++i)
            crops.push_back(parseCropTextOption(resolved.cropTexts[i]));
    } else if (resolved.common.cropOverride) {
        crops.push_back(resolved.common.crop);
    }

    std::vector<std::vector<GroupedShutterMeasurement>> groupedSequences;
    std::vector<std::vector<std::pair<double, double>>> alignedData;

    for (size_t seqIndex = 0; seqIndex < resolved.sequenceTexts.size(); ++seqIndex) {
        std::vector<std::string> inputs = expandSequenceText(resolved.sequenceTexts[seqIndex]);
        std::vector<int> crop = crops.empty() ? std::vector<int>() : crops[std::min(seqIndex, crops.size() - 1)];

        std::vector<ShutterMeasurement> kept;
        for (size_t i = 0; i < inputs.size(); ++i) {
            CalibrationSampleResult sample = runCalibrationSample(resolved.common, std::vector<std::string>(1, inputs[i]), crop, resolved.channel);
            FrameInfo frame = readFrameInfo(inputs[i], true, resolved.common.shutterc, parseFoPairs(resolved.common.foText));
            if (sample.fraction > 0.99) {
                ShutterMeasurement m;
                m.shutter = frame.shutterRecip;
                m.average = sample.average;
                m.fraction = sample.fraction;
                kept.push_back(m);
            }
        }

        std::cerr << "sequence " << seqIndex << ": " << kept.size() << " out of "
                  << inputs.size() << " frames in range" << std::endl;
        groupedSequences.push_back(groupShutterMeasurements(kept));
    }

    std::sort(groupedSequences.begin(), groupedSequences.end(),
        [](const std::vector<GroupedShutterMeasurement> &a, const std::vector<GroupedShutterMeasurement> &b) {
            if (a.empty() || b.empty())
                return a.size() < b.size();
            return a.front().shutter < b.front().shutter;
        });

    if (groupedSequences.empty() || groupedSequences.front().empty())
        throw std::runtime_error("No valid shutter samples were found.");

    std::map<std::string, double> mergedValues;
    std::map<std::string, double> mergedShutters;
    std::vector<std::pair<double, double>> sequence0;
    for (size_t i = 0; i < groupedSequences.front().size(); ++i) {
        mergedValues[groupedSequences.front()[i].key] = groupedSequences.front()[i].average;
        mergedShutters[groupedSequences.front()[i].key] = groupedSequences.front()[i].shutter;
        sequence0.push_back(std::make_pair(groupedSequences.front()[i].shutter, groupedSequences.front()[i].average));
    }
    alignedData.push_back(sequence0);

    for (size_t seqIndex = 1; seqIndex < groupedSequences.size(); ++seqIndex) {
        const std::vector<GroupedShutterMeasurement> &seq = groupedSequences[seqIndex];
        std::vector<double> overlap;
        for (size_t i = 0; i < seq.size(); ++i) {
            std::map<std::string, double>::const_iterator it = mergedValues.find(seq[i].key);
            if (it != mergedValues.end() && seq[i].average > 0.0)
                overlap.push_back(it->second / seq[i].average);
        }
        if (overlap.empty())
            throw std::runtime_error("Shutter calibration sequences do not overlap.");

        double scale = std::accumulate(overlap.begin(), overlap.end(), 0.0) / static_cast<double>(overlap.size());
        std::vector<std::pair<double, double>> scaled;
        for (size_t i = 0; i < seq.size(); ++i) {
            double value = seq[i].average * scale;
            mergedValues[seq[i].key] = value;
            mergedShutters[seq[i].key] = seq[i].shutter;
            scaled.push_back(std::make_pair(seq[i].shutter, value));
        }
        alignedData.push_back(scaled);
    }

    std::vector<std::pair<double, double>> merged;
    for (std::map<std::string, double>::const_iterator it = mergedValues.begin(); it != mergedValues.end(); ++it)
        merged.push_back(std::make_pair(mergedShutters[it->first], it->second));
    std::sort(merged.begin(), merged.end());
    if (merged.empty())
        throw std::runtime_error("No merged shutter samples were produced.");

    double base = merged.front().second;
    std::vector<std::pair<double, double>> fitInput;
    for (size_t i = 0; i < merged.size(); ++i) {
        merged[i].second /= base;
        fitInput.push_back(std::make_pair(merged[i].first, 1.0 / merged[i].second));
    }

    std::pair<double, double> fit = fitExponentialInverse(fitInput);

    if (!resolved.dataOut.empty()) {
        std::ofstream out(resolved.dataOut.c_str());
        if (!out)
            throw std::runtime_error("Could not open dataout file: " + resolved.dataOut);
        out << "shutter\tfit\tavg\n";
        for (size_t i = 0; i < merged.size(); ++i)
            out << merged[i].first << "\t" << fit.second * std::exp(fit.first * merged[i].first)
                << "\t" << (1.0 / merged[i].second) << "\n";
    }

    std::cout << "# calculated average relative scaling factors\n";
    for (size_t i = 0; i < merged.size(); ++i)
        std::cout << merged[i].first << " " << merged[i].second << "\n";
    std::cerr << "# coefficients for exponential fit correction factor: B*e^(A*x)\n";
    std::cerr << "A: " << fit.first << "\n";
    std::cerr << "B: " << fit.second << "\n";
    std::cerr << "for configuration file:\n";
    std::cerr << "shutterc = " << fit.first << std::endl;
    return 0;
}

int printApertureCalibration(const GlobalOptions &global, const ApertureOptions &opts) {
    std::string configPath = configPathFromGlobal(global);
    ConfigValues cfg = loadConfigValues(configPath);
    ApertureOptions resolved = opts;
    applyCalibrationDefaults(resolved.common, cfg);
    validateApertureOptions(resolved);

    std::vector<int> crop = resolved.common.crop;
    if (!resolved.cropText.empty())
        crop = parseCropTextOption(resolved.cropText);

    std::vector<std::pair<float, float>> fo = parseFoPairs(resolved.common.foText);
    struct ApertureResult {
        double nominal = 0.0;
        double exact = 0.0;
        double average = 0.0;
        double fraction = 0.0;
    };
    std::vector<ApertureResult> results;

    for (size_t seqIndex = 0; seqIndex < resolved.sequenceTexts.size(); ++seqIndex) {
        std::vector<std::string> inputs = expandSequenceText(resolved.sequenceTexts[seqIndex]);
        if (inputs.empty())
            continue;

        CalibrationSampleResult sample = runCalibrationSample(resolved.common, inputs, crop, "g");
        if (sample.fraction < 1.0) {
            std::cerr << "Warning: sequence " << seqIndex << " does not yield a fully in-range HDR, "
                      << (1.0 - sample.fraction) << " is out of range" << std::endl;
        }

        FrameInfo nominal = readFrameInfo(inputs.front(), false, 0.0, std::vector<std::pair<float, float>>());
        FrameInfo exact = readFrameInfo(inputs.front(), true, resolved.common.shutterc, fo);

        ApertureResult result;
        result.nominal = nominal.aperture;
        result.exact = exact.aperture;
        result.average = sample.average;
        result.fraction = sample.fraction;
        results.push_back(result);
    }

    if (results.size() < 2)
        throw std::runtime_error("Aperture calibration needs at least two valid sequences.");

    double ref = results.front().average;
    std::cout << "Aperture corrections based on nominal aperture F-" << results.front().nominal
              << " (exact: " << results.front().exact << ")\n";
    std::cout << "Nominal:   Exact: Corrected:\n";

    std::vector<std::string> corrections;
    for (size_t i = 1; i < results.size(); ++i) {
        double corrected = std::sqrt(results[i].exact * results[i].exact * ref / results[i].average);
        std::cout << std::fixed << std::setprecision(3)
                  << std::setw(8) << results[i].nominal << " "
                  << std::setw(8) << results[i].exact << " "
                  << std::setw(10) << corrected << "\n";
        std::ostringstream token;
        token << results[i].nominal << "," << std::setprecision(6) << corrected;
        corrections.push_back(token.str());
    }

    std::cout << "fo = " << joinStrings(corrections, " ") << std::endl;
    return 0;
}

int printColorCalibration(const GlobalOptions &global, const ColorCalibrateOptions &opts) {
    (void) global;
    validateColorCalibrateOptions(opts);
    return runColorCalibration(opts, std::cout, std::cerr);
}

int printChartCells(const GlobalOptions &global, const ChartCellsCliOptions &opts) {
    (void) global;
    validateChartCellsOptions(opts);
    ChartCellsOptions native;
    native.imagePath = opts.imagePath;
    native.outputPath = opts.outputPath;
    native.previewPath = opts.previewPath;
    native.patchCount = opts.layoutExplicit ? 0 : opts.patchCount;
    native.columns = opts.columns;
    native.rows = opts.rows;
    native.inset = opts.inset;
    native.whiteFirst = opts.whiteFirst;
    return runChartCells(native, std::cout, std::cerr);
}

int printHdrCrop(const GlobalOptions &global, const HdrCropOptions &opts) {
    validateHdrCropOptions(opts);
    HdrImage source = readRadianceHDRTopDown(opts.inputPath);
    HdrImage cropped = cropHdrImageTopLeftTopDown(source, opts.crop[0], opts.crop[1], opts.crop[2], opts.crop[3]);
    const std::string commandLine = buildHdrCropCommandLine(global, opts);
    const std::vector<std::string> headerLines = buildHdrCropHeaderLines(source, opts, commandLine);
    writeRGBE(opts.outputPath, cropped.width, cropped.height, cropped.rgb.data(), headerLines, false);
    return 0;
}

int printHdrRotate(const GlobalOptions &global, const HdrRotateOptions &opts) {
    validateHdrRotateOptions(opts);
    HdrImage source = readRadianceHDRTopDown(opts.inputPath);
    HdrImage rotated = rotateHdrImageTopDown(source, opts.angleDegrees);
    const std::string commandLine = buildHdrRotateCommandLine(global, opts);
    const std::vector<std::string> headerLines =
        buildHdrRotateHeaderLines(source, opts, commandLine);
    writeRGBE(opts.outputPath, rotated.width, rotated.height, rotated.rgb.data(), headerLines, false);
    return 0;
}

int printHdrProject(const GlobalOptions &global, const HdrProjectOptions &opts) {
    validateHdrProjectOptions(opts);
    HdrImage source = readRadianceHDRTopDown(opts.inputPath);
    HdrProjectOptions resolved = opts;
    HdrImage projected;
    if (resolved.polynomialCoefficients.empty()) {
        projected = resolved.hemispherical
            ? projectHdrImageHemisphericalToEquidistant(source)
            : projectHdrImageEquisolidToEquidistant(source);
    } else {
        if (source.width != source.height)
            throw std::runtime_error("convertprojection -p requires a square input HDR.");
        const double availableRadius = 0.5 * static_cast<double>(source.width);
        if (!(resolved.sourceRadius > 0.0))
            resolved.sourceRadius = availableRadius;
        if (!(resolved.targetRadius > 0.0))
            resolved.targetRadius = availableRadius;
        if (resolved.sourceRadius > availableRadius || resolved.targetRadius > availableRadius)
            throw std::runtime_error("convertprojection projection radii must fit inside the input HDR.");
        projected = projectHdrImageAnglePolynomialToEquidistant(
            source, resolved.polynomialCoefficients,
            resolved.sourceRadius, resolved.targetRadius);
    }
    const std::string commandLine = buildHdrProjectCommandLine(global, resolved);
    const std::vector<std::string> headerLines =
        buildHdrProjectHeaderLines(source, resolved, commandLine);
    writeRGBE(opts.outputPath, projected.width, projected.height, projected.rgb.data(), headerLines, false);
    return 0;
}

fs::path hdrspaceSupportDirectory() {
    if (const char *configured = std::getenv("HDRSPACE_SUPPORT_DIR")) {
        if (*configured)
            return fs::path(configured);
    }
#if defined(_WIN32)
    const char *localAppData = std::getenv("LOCALAPPDATA");
    if (!localAppData || !*localAppData)
        throw std::runtime_error(
            "LOCALAPPDATA is unavailable; set HDRSPACE_SUPPORT_DIR for the DA3 runtime.");
    return fs::path(localAppData) / "hdrspace";
#else
#if defined(_WIN32)
    const char *localAppData = std::getenv("LOCALAPPDATA");
    if (!localAppData || !*localAppData)
        throw std::runtime_error(
            "LOCALAPPDATA is unavailable; set HDRSPACE_SUPPORT_DIR for the DA3 runtime.");
    return fs::path(localAppData) / "hdrspace";
#else
    const char *home = std::getenv("HOME");
    if (!home || !*home)
        throw std::runtime_error(
            "HOME is unavailable; set HDRSPACE_SUPPORT_DIR for the DA3 runtime.");
    return fs::path(home) / "Library" / "Application Support" / "hdrspace";
#endif
#endif
}

fs::path viewVosRuntimeRoot() {
    if (const char *configured = std::getenv("HDRSPACE_DA3_RUNTIME_ROOT")) {
        if (*configured)
            return fs::path(configured);
    }
    return hdrspaceSupportDirectory() / "da3-runtime";
}

fs::path viewVosHfHome() {
    if (const char *configured = std::getenv("HDRSPACE_DA3_HF_HOME")) {
        if (*configured)
            return fs::path(configured);
    }
    return hdrspaceSupportDirectory() / "da3-hf-home";
}

bool isRegularFilePath(const fs::path &path) {
    boost::system::error_code ec;
    return fs::is_regular_file(path, ec) && !ec;
}

fs::path viewVosPythonPath(const fs::path &runtimeRoot) {
    if (const char *configured = std::getenv("HDRSPACE_DA3_PYTHON")) {
        const fs::path candidate(configured);
        if (isRegularFilePath(candidate))
            return fs::absolute(candidate).lexically_normal();
        throw std::runtime_error(
            "HDRSPACE_DA3_PYTHON does not point to a file: " + candidate.string());
    }
    const fs::path candidate = runtimeRoot / ".venv" / "bin" / "python3";
    if (isRegularFilePath(candidate))
        return fs::absolute(candidate).lexically_normal();
    throw std::runtime_error(
        "DA3 Python runtime is not installed at " + candidate.string() +
        ". Install it with mergehdr-image-vos install or from hdrspace's Image VOS setup.");
}

fs::path viewVosHelperPath(const fs::path &runtimeRoot) {
    if (const char *configured = std::getenv("MERGEHDR_IMAGE_VOS_HELPER")) {
        const fs::path candidate(configured);
        if (isRegularFilePath(candidate))
            return fs::absolute(candidate).lexically_normal();
        throw std::runtime_error(
            "MERGEHDR_IMAGE_VOS_HELPER does not point to a file: " + candidate.string());
    }

    const fs::path executable(getExecutablePath());
    const std::vector<fs::path> candidates = {
        runtimeRoot / "da3_vos_helper.py",
        executable.parent_path().parent_path() / "Resources" / "ai" / "da3_vos_helper.py",
        executable.parent_path().parent_path() / "tools" / "da3_vos_helper.py",
    };
    for (const fs::path &candidate : candidates) {
        if (isRegularFilePath(candidate))
            return fs::absolute(candidate).lexically_normal();
    }
    throw std::runtime_error(
        "Unable to locate da3_vos_helper.py. Reinstall the DA3 runtime with "
        "mergehdr-image-vos install or set MERGEHDR_IMAGE_VOS_HELPER.");
}

int runViewVosCalculation(const ViewVosCalculationOptions &opts) {
    validateViewVosCalculationOptions(opts);
    ViewVosCalculationOptions resolved = opts;

    if (!resolved.windowDistanceSet) {
        std::cerr
            << "--window-distance was not supplied. Allow DA3 to estimate the "
               "observer-to-window distance from the interior/paired HDRs? [y/N] "
            << std::flush;
        std::string response;
        if (!std::getline(std::cin, response))
            response.clear();
        response = lowerCopy(trimCopy(response));
        if (response != "y" && response != "yes") {
            std::cerr
                << "View VOS calculation cancelled. Supply --window-distance or rerun and answer y.\n";
            return 2;
        }
        resolved.estimateWindowDistance = true;
    }

    const fs::path runtimeRoot = viewVosRuntimeRoot();
    const fs::path python = viewVosPythonPath(runtimeRoot);
    const fs::path helper = viewVosHelperPath(runtimeRoot);
    const fs::path hfHome = viewVosHfHome();

    std::vector<std::string> command;
    command.push_back(python.string());
    command.push_back(helper.string());
    command.push_back("pair");
    command.push_back("--internal-input");
    command.push_back(resolved.interiorPath);
    command.push_back("--external-input");
    command.push_back(resolved.exteriorPath);
    command.push_back("--output-dir");
    command.push_back(resolved.outputDir);
    if (resolved.windowDistanceSet) {
        std::ostringstream distance;
        distance << std::setprecision(17) << resolved.windowDistance;
        command.push_back("--window-distance");
        command.push_back(distance.str());
    } else if (resolved.estimateWindowDistance) {
        command.push_back("--estimate-window-distance");
    }
    if (!resolved.windowMaskPath.empty()) {
        command.push_back("--window-mask");
        command.push_back(resolved.windowMaskPath);
    }

#if defined(_WIN32)
    _putenv_s("HF_HOME", hfHome.string().c_str());
    _putenv_s("HF_HUB_CACHE", (hfHome / "hub").string().c_str());
    std::vector<std::string> quotedCommand;
    for (const std::string &argument : command)
        quotedCommand.push_back(processQuote(argument));
    std::vector<const char *> spawnArgv;
    for (const std::string &argument : quotedCommand)
        spawnArgv.push_back(argument.c_str());
    spawnArgv.push_back(nullptr);
    const intptr_t spawnStatus = _spawnv(_P_WAIT, python.string().c_str(), spawnArgv.data());
    if (spawnStatus < 0)
        throw std::runtime_error("Unable to start the DA3 VOS process: " + std::string(std::strerror(errno)));
    return static_cast<int>(spawnStatus);
#else
#if defined(_WIN32)
    _putenv_s("HF_HOME", hfHome.string().c_str());
    _putenv_s("HF_HUB_CACHE", (hfHome / "hub").string().c_str());
    std::vector<std::string> quotedCommand;
    for (const std::string &argument : command)
        quotedCommand.push_back(processQuote(argument));
    std::vector<const char *> spawnArgv;
    for (const std::string &argument : quotedCommand)
        spawnArgv.push_back(argument.c_str());
    spawnArgv.push_back(nullptr);
    const intptr_t spawnStatus = _spawnv(_P_WAIT, python.string().c_str(), spawnArgv.data());
    if (spawnStatus < 0)
        throw std::runtime_error("Unable to start the DA3 VOS process: " + std::string(std::strerror(errno)));
    return static_cast<int>(spawnStatus);
#else
    const pid_t child = fork();
    if (child < 0)
        throw std::runtime_error("Unable to start the DA3 VOS process: " + std::string(std::strerror(errno)));
    if (child == 0) {
        setenv("HF_HOME", hfHome.string().c_str(), 1);
        const std::string hubCache = (hfHome / "hub").string();
        setenv("HF_HUB_CACHE", hubCache.c_str(), 1);
        setenv("PYTORCH_ENABLE_MPS_FALLBACK", "1", 1);

        std::vector<char *> argv;
        argv.reserve(command.size() + 1);
        for (std::string &argument : command)
            argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);
        execv(python.string().c_str(), argv.data());
        std::cerr << "Unable to launch " << python.string() << ": "
                  << std::strerror(errno) << std::endl;
        _exit(127);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        throw std::runtime_error("Unable to wait for the DA3 VOS process: " + std::string(std::strerror(errno)));
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        const int signalNumber = WTERMSIG(status);
        std::cerr << "DA3 VOS process terminated by signal " << signalNumber << ".\n";
        return 128 + signalNumber;
    }
    return 1;
#endif
}

int printViewVisibility(const ViewVisibilityCliOptions &opts) {
    validateViewVisibilityOptions(opts);
    ViewVisibilityCliOptions resolved = opts;
    if (resolved.outputPath.empty())
        resolved.outputPath = defaultViewVisibilityOutputPath(resolved.native.testPath);

    std::string implicitMaskPath;
    try {
        if (resolved.native.maskPath.empty()) {
            implicitMaskPath = makeImplicitWhiteMask(resolved.native.referencePath);
            resolved.native.maskPath = implicitMaskPath;
        }

        const ViewVisibilitySummaryResult result = analyzeViewVisibilitySummary(resolved.native);
        if (resolved.writeDebugDump)
            writeViewVisibilityDebugDump(resolved.native, resolved.debugDir);
        writeViewVisibilityResultHdr(resolved.outputPath, result);

        std::cout << std::fixed << std::setprecision(12);
        std::cout << "detail\t" << result.detailLabel << "\n";
        std::cout << "input_color\t" << result.inputColor << "\n";
        std::cout << "viewsize\t" << result.viewSize << "\n";
        std::cout << "refcontrast\t" << result.referenceBaseline << "\n";
        std::cout << "visibility\t" << result.visibilityRatio << "\n";
        std::cout << "quality\t" << result.qualityScore << "\n";
        std::cout << "output\t" << resolved.outputPath << "\n";
    } catch (...) {
        if (!implicitMaskPath.empty())
            removeTree(fs::path(implicitMaskPath).parent_path());
        throw;
    }

    if (!implicitMaskPath.empty())
        removeTree(fs::path(implicitMaskPath).parent_path());
    return 0;
}

int printPerceptualMap(const PerceptualMapCliOptions &opts) {
    validatePerceptualMapOptions(opts);
    const PerceptualMapKind kind = parsePerceptualMapKind(opts.mapName);
    PerceptualMapCliOptions resolved = opts;
    if (resolved.outputPath.empty())
        resolved.outputPath = defaultPerceptualMapOutputPath(resolved.native.inputPath, perceptualMapKindOutputName(kind));

    const PerceptualMapResult result = analyzePerceptualMap(resolved.native, kind);
    writePerceptualMapHdr(resolved.outputPath, result);

    std::cout << "map\t" << result.label << "\n";
    std::cout << "output\t" << resolved.outputPath << "\n";
    return 0;
}

int runMerge(const GlobalOptions &global, const RunOptions &opts);

std::string shadowbandIntermediateHdrPath(const std::string &inputPath, const std::string &label,
                                          const std::string &finalOutputPath) {
    if (isDirectoryPath(inputPath))
        return trimTrailingSlashes(inputPath) + ".hdr";
    const fs::path finalPath(finalOutputPath.empty() ? "blended.hdr" : finalOutputPath);
    const std::string stem = finalPath.stem().string().empty() ? "blended" : finalPath.stem().string();
    return (finalPath.parent_path() / (stem + "_" + label + ".hdr")).string();
}

std::string shadowbandRawPatternFromInput(const std::string &inputPath, const std::string &rawExt) {
    if (isDirectoryPath(inputPath)) {
        std::string ext = rawExt;
        if (!ext.empty() && ext.front() == '.')
            ext.erase(ext.begin());
        return trimTrailingSlashes(inputPath) + "/*." + ext;
    }
    return inputPath;
}

std::string ensureShadowbandHdrInput(const ShadowbandCliOptions &opts, const std::string &inputPath,
                                     const std::string &cfgSpec, const std::string &label) {
    if (!shadowbandInputNeedsRawMerge(inputPath))
        return inputPath;

    RunOptions runOpts;
    runOpts.outputOverride = true;
    runOpts.outputPath = shadowbandIntermediateHdrPath(inputPath, label, opts.outputPath);
    runOpts.inputs.push_back(shadowbandRawPatternFromInput(inputPath, opts.rawExt));

    const GlobalOptions global = shadowbandGlobalFromConfigSpec(cfgSpec);
    const int rc = runMerge(global, runOpts);
    if (rc != 0)
        throw std::runtime_error("shadowband raw pre-merge failed for " + label + " input: " + inputPath);
    return runOpts.outputPath;
}

int printShadowband(const ShadowbandCliOptions &opts) {
    validateShadowbandOptions(opts);

    ShadowbandCliOptions resolved = opts;
    resolved.horizontalPath = ensureShadowbandHdrInput(resolved, resolved.horizontalPath, resolved.bandCfg, "H");
    resolved.verticalPath = ensureShadowbandHdrInput(resolved, resolved.verticalPath, resolved.bandCfg, "V");
    resolved.noShadowPath = ensureShadowbandHdrInput(resolved, resolved.noShadowPath, resolved.ndCfg, "ND");

    if (!resolved.native.haveSunloc) {
        std::array<double, 3> headerDirection{{0.0, 0.0, 0.0}};
        if (tryParseSolarSourceDirectionFromHdrHeader(resolved.noShadowPath, headerDirection)) {
            resolved.native.haveSunDirection = true;
            resolved.native.sunDirection = headerDirection;
        }
    }

    HdrImage horizontal = readRadianceHDRTopDown(resolved.horizontalPath);
    HdrImage vertical = readRadianceHDRTopDown(resolved.verticalPath);
    HdrImage noShadow = readRadianceHDRTopDown(resolved.noShadowPath);

    const ShadowbandInputColorInfo hColor = resolveShadowbandInputColorInfo(horizontal);
    const ShadowbandInputColorInfo vColor = resolveShadowbandInputColorInfo(vertical);
    const ShadowbandInputColorInfo nColor = resolveShadowbandInputColorInfo(noShadow);
    const bool sameInputColorMetadata =
        sameShadowbandColorMetadata(hColor, vColor) &&
        sameShadowbandColorMetadata(hColor, nColor);

    horizontal = convertShadowbandInputToRadiance(horizontal, hColor);
    vertical = convertShadowbandInputToRadiance(vertical, vColor);
    noShadow = convertShadowbandInputToRadiance(noShadow, nColor);

    const ShadowbandMergeResult result = mergeShadowbandNative(horizontal, vertical, noShadow, resolved.native);

    std::vector<std::string> header;
    std::ostringstream sbopts;
    sbopts << std::fixed << std::setprecision(3)
           << "SHADOWBAND= roh:" << resolved.native.roh
           << " rov:" << resolved.native.rov
           << " sfov:" << resolved.native.sfov
           << " srcsize:" << std::setprecision(4) << resolved.native.srcsize
           << " bw:" << resolved.native.bw
           << " align:" << (resolved.native.align ? "True" : "False")
           << " fisheye:" << (resolved.native.fisheye ? "True" : "False");
    if (resolved.native.haveSunloc)
        sbopts << " sunloc:" << resolved.native.sunPixelX << "," << resolved.native.sunPixelY;
    else
        sbopts << " sunloc:None";
    sbopts << " margin:" << resolved.native.margin;
    header.push_back(sbopts.str());
    header.push_back(formatPrimariesHeader(parseColorSpaceSpec("rad").primaries));
    header.push_back(formatPairHeader("TargetWhitePoint", parseColorSpaceSpec("rad").white));
    header.push_back(formatTripletHeader("LuminanceRGB", std::array<float, 3>{{0.26507413f, 0.67011463f, 0.06481124f}}));
    if (!sameInputColorMetadata)
        header.push_back("MERGEHDR_SHADOWBAND_INPUT_COLORSPACE_MISMATCH= converted all inputs to Rad before merge");

    const std::set<std::string> replacedShadowbandKeys{"view"};
    const std::vector<std::string> hh = headerLinesReplacing(horizontal, replacedShadowbandKeys);
    const std::vector<std::string> hv = headerLinesReplacing(vertical, replacedShadowbandKeys);
    const std::vector<std::string> hs = headerLinesReplacing(noShadow, replacedShadowbandKeys);
    for (const std::string &line : result.headerNotes)
        header.push_back(line);
    header.insert(header.end(), hh.begin(), hh.end());
    header.insert(header.end(), hv.begin(), hv.end());
    header.insert(header.end(), hs.begin(), hs.end());
    header.erase(
        std::remove_if(header.begin(), header.end(), [](const std::string &line) {
            return headerKeyLower(line) == "view";
        }),
        header.end()
    );
    header.push_back("VIEW= -vta -vv 180 -vh 180 -vd 0.0 -1.0 0.0 -vp 0 0 0 -vu 0 0 1");

    writeRadianceHDR(resolved.outputPath, result.blended, header, false, true);

    if (!resolved.native.envmapPath.empty() && result.hasSkyOnly) {
        std::vector<std::string> envHeader = header;
        if (result.hasSource) {
            const double pi = std::acos(-1.0);
            const double srcDegrees = std::sqrt(result.sourceSolidAngle / pi) * 360.0 / pi;
            std::ostringstream solar;
            solar << std::setprecision(17)
                  << "SOLARSOURCE= void light sun 0 0 3 "
                  << result.sourceRgb[0] << " " << result.sourceRgb[1] << " " << result.sourceRgb[2]
                  << " sun source solar 0 0 4 "
                  << result.outputSourceDirection[0] << " "
                  << result.outputSourceDirection[1] << " "
                  << result.outputSourceDirection[2] << " "
                  << srcDegrees;
            envHeader.push_back(solar.str());
            envHeader.push_back("SKYSOURCE= void colorpict imgfunc 9 red green blue " + resolved.native.envmapPath +
                                " fisheye.cal fish_u fish_v -rz 180 0 0 imgfunc glow imgglow 0 0 4 1 1 1 0 imgglow source sky 0 0 4 0 -1 0 180");
        }
        writeRadianceHDR(resolved.native.envmapPath, result.skyOnly, envHeader, false, true);
    }

    std::cout << "output\t" << resolved.outputPath << "\n";
    if (!resolved.native.envmapPath.empty() && result.hasSkyOnly)
        std::cout << "envmap\t" << resolved.native.envmapPath << "\n";
    if (result.hasSource) {
        std::cout << std::fixed << std::setprecision(12);
        std::cout << "source_dir\t"
                  << result.outputSourceDirection[0] << "\t"
                  << result.outputSourceDirection[1] << "\t"
                  << result.outputSourceDirection[2] << "\n";
        std::cout << "source_rgb\t"
                  << result.sourceRgb[0] << "\t"
                  << result.sourceRgb[1] << "\t"
                  << result.sourceRgb[2] << "\n";
        std::cout << "source_solid_angle\t" << result.sourceSolidAngle << "\n";
    }
    return 0;
}

int printMakelist(const GlobalOptions &global, const RunOptions &opts) {
    std::string configPath = configPathFromGlobal(global);
    ConfigValues cfg = loadConfigValues(configPath);
    RunOptions resolved = opts;
    applyConfigDefaults(resolved, cfg);
    finalizeOptionFlags(resolved);
    if (!resolved.vfilePath.empty()) {
        resolved.vfilePath = resolveVignettingFilePath(resolved.vfilePath, configPath);
        (void) loadVignettingTable(resolved.vfilePath);
    }
    validateRunOptions(resolved);

    std::vector<std::string> expanded = expandInputs(resolved.inputs);
    std::vector<std::pair<float, float>> fo = parseFoPairs(resolved.foText);
    std::vector<FrameInfo> frames;
    frames.reserve(expanded.size());
    for (const std::string &path : expanded)
        frames.push_back(readFrameInfo(path, resolved.correct, resolved.shutterc, fo));
    std::sort(frames.begin(), frames.end(), [](const FrameInfo &a, const FrameInfo &b) {
        return a.exposureTime < b.exposureTime;
    });

    double scaleTotal = resolved.scale * std::pow(10.0, resolved.nd);
    for (FrameInfo &frame : frames)
        frame.effectiveExposure = effectiveExposure(frame, scaleTotal);

    std::vector<float> xyzcamValues = resolveXYZCamValues(resolved, frames);
    ResolvedRawLevels levels = resolveRawLevels(resolved, frames);

    std::vector<float> rawMultipliers = parseOptionalFloatValues(resolved.rawMultipliersText);
    std::vector<float> cscale = parseOptionalFloatValues(resolved.cscaleText);
    ColorHeaders colorHeaders;
    ColorHeaders *colorHeadersPtr = nullptr;
    if (!xyzcamValues.empty() || lowerCopy(resolved.colorspace) == "raw" || lowerCopy(resolved.colorspace) == "native") {
        colorHeaders = buildColorHeaders(resolved.colorspace, xyzcamValues, rawMultipliers, cscale);
        colorHeadersPtr = &colorHeaders;
    }

    std::string coreStep = buildCoreStep(resolved, xyzcamValues, rawMultipliers, cscale, levels);
    std::string commandLine = buildCommandLine(global, resolved);
    std::vector<std::string> lines = buildHeaderLines(global, resolved, frames, colorHeadersPtr, rawMultipliers, cscale,
                                                      levels.headerBlacklevel, levels.headerWhitepoint, coreStep, commandLine);
    if (!resolved.vfilePath.empty())
        lines.push_back("VIGNETTING_CORRECTION= " + resolved.vfilePath);
    for (const std::string &line : lines)
        std::cout << line << "\n";
    return 0;
}

int runMerge(const GlobalOptions &global, const RunOptions &opts) {
    std::string configPath = configPathFromGlobal(global);
    ConfigValues cfg = loadConfigValues(configPath);
    RunOptions resolved = opts;
    applyConfigDefaults(resolved, cfg);
    finalizeOptionFlags(resolved);
    VignettingTable vignettingTable;
    if (!resolved.vfilePath.empty()) {
        resolved.vfilePath = resolveVignettingFilePath(resolved.vfilePath, configPath);
        vignettingTable = loadVignettingTable(resolved.vfilePath);
    }
    validateRunOptions(resolved);

    std::vector<std::string> expanded = expandInputs(resolved.inputs);
    std::pair<fs::path, fs::path> dualOutputs;
    if (resolved.dualFisheye)
        dualOutputs = dualFisheyeOutputPaths(
            resolved, expanded.empty() ? std::string() : expanded.front());
    std::vector<std::pair<float, float>> fo = parseFoPairs(resolved.foText);
    std::vector<FrameInfo> frames;
    frames.reserve(expanded.size());
    for (const std::string &path : expanded)
        frames.push_back(readFrameInfo(path, resolved.correct, resolved.shutterc, fo));
    std::sort(frames.begin(), frames.end(), [](const FrameInfo &a, const FrameInfo &b) {
        return a.exposureTime < b.exposureTime;
    });

    double scaleTotal = resolved.scale * std::pow(10.0, resolved.nd);
    for (FrameInfo &frame : frames)
        frame.effectiveExposure = effectiveExposure(frame, scaleTotal);

    std::vector<float> xyzcamValues = resolveXYZCamValues(resolved, frames);

    std::vector<float> rawMultipliers = parseOptionalFloatValues(resolved.rawMultipliersText);
    std::vector<float> cscale = parseOptionalFloatValues(resolved.cscaleText);
    ColorHeaders colorHeaders;
    ColorHeaders *colorHeadersPtr = nullptr;
    if (!xyzcamValues.empty() || lowerCopy(resolved.colorspace) == "raw" || lowerCopy(resolved.colorspace) == "native") {
        colorHeaders = buildColorHeaders(resolved.colorspace, xyzcamValues, rawMultipliers, cscale);
        colorHeadersPtr = &colorHeaders;
    }

    emitRangeStats(frames, resolved.saturation, resolved.rangeValue, colorHeadersPtr, cscale);

    ResolvedRawLevels levels = resolveRawLevels(resolved, frames);
    std::string coreStep = buildCoreStep(resolved, xyzcamValues, rawMultipliers, cscale, levels);
    std::string commandLine = buildCommandLine(global, resolved);
    std::vector<std::string> lines = buildHeaderLines(global, resolved, frames, colorHeadersPtr, rawMultipliers, cscale,
                                                      levels.headerBlacklevel, levels.headerWhitepoint, coreStep, commandLine);

    const bool singleFisheyeProjection = !resolved.singleFisheyeProjection.empty();
    const bool applyVignetting = !resolved.vfilePath.empty();
    const bool streamToStdout = !resolved.outputOverride && !resolved.dualFisheye;
    const bool temporaryCoreOutput =
        streamToStdout || resolved.dualFisheye || singleFisheyeProjection || applyVignetting;
    std::string tmpdir;
    fs::path tmpPath;
    fs::path hdrPath;
    if (temporaryCoreOutput) {
        tmpdir = temporaryDirectory();
        tmpPath = fs::path(tmpdir);
        hdrPath = tmpPath / "merged.hdr";
    } else {
        hdrPath = fs::path(resolved.outputPath);
    }

    std::vector<std::string> commandArgs;
    commandArgs.push_back(coreBinaryPath());
    commandArgs.push_back("--output");
    commandArgs.push_back(hdrPath.string());
    commandArgs.push_back("--format");
    commandArgs.push_back("hdr");
    if (resolved.rawgrid) {
        commandArgs.push_back("--rawgrid");
    } else {
        commandArgs.push_back("--demosaic");
        commandArgs.push_back(canonicalDemosaic(resolved.demosaic));
    }
    commandArgs.push_back("--mergestyle");
    commandArgs.push_back(canonicalMergeWeight(resolved.mergeWeight));
    commandArgs.push_back("--colormode");
    commandArgs.push_back("native");
    commandArgs.push_back("--colorspace");
    commandArgs.push_back(resolved.colorspace);
    commandArgs.push_back("--linearhdrexposure");
    commandArgs.push_back("--exposurescale");
    commandArgs.push_back((boost::format("%1$.12g") % scaleTotal).str());
    commandArgs.push_back("--saturation");
    commandArgs.push_back((boost::format("%1$.12g") % std::max(0.5, std::min(0.999999, 1.0 - resolved.saturation))).str());
    commandArgs.push_back("--range");
    commandArgs.push_back((boost::format("%1$.12g") % resolved.rangeValue).str());
    if (!resolved.correct)
        commandArgs.push_back("--nominal");
    if (resolved.fisheye)
        commandArgs.push_back("--solid2ang");
    if (!resolved.crop.empty()) {
        commandArgs.push_back("--mergecrop");
        commandArgs.push_back((boost::format("%1%,%2%,%3%,%4%") % resolved.crop[0] % resolved.crop[1] % resolved.crop[2] % resolved.crop[3]).str());
    }
    if (levels.coreBlacklevel >= 0) {
        commandArgs.push_back("--blacklevel");
        commandArgs.push_back(std::to_string(levels.coreBlacklevel));
    }
    if (levels.coreWhitepoint >= 0) {
        commandArgs.push_back("--whitepoint");
        commandArgs.push_back(std::to_string(levels.coreWhitepoint));
    }
    if (!xyzcamValues.empty()) {
        std::ostringstream xyzText;
        xyzText << std::setprecision(12);
        for (size_t i = 0; i < xyzcamValues.size(); ++i) {
            if (i)
                xyzText << ' ';
            xyzText << xyzcamValues[i];
        }
        commandArgs.push_back("--xyzcam");
        commandArgs.push_back(xyzText.str());
    }
    if (resolved.badpixelsOverride) {
        commandArgs.push_back("--badpixels");
        commandArgs.push_back(resolved.badpixelsText);
    }
    if (!rawMultipliers.empty()) {
        std::ostringstream multText;
        multText << std::setprecision(12);
        for (size_t i = 0; i < rawMultipliers.size(); ++i) {
            if (i)
                multText << ' ';
            multText << rawMultipliers[i];
        }
        commandArgs.push_back("--rawmultipliers");
        commandArgs.push_back(multText.str());
    }
    if (!cscale.empty()) {
        std::ostringstream rgbText;
        rgbText << std::setprecision(12);
        for (size_t i = 0; i < cscale.size(); ++i) {
            if (i)
                rgbText << ' ';
            rgbText << cscale[i];
        }
        commandArgs.push_back("--rgbcal");
        commandArgs.push_back(rgbText.str());
    }
    if (resolved.shuttercOverride) {
        commandArgs.push_back("--shutterc");
        commandArgs.push_back((boost::format("%1$.12g") % resolved.shutterc).str());
    }
    if (resolved.foOverride) {
        commandArgs.push_back("--fo");
        commandArgs.push_back(resolved.foText);
    }
    for (const std::string &line : lines) {
        commandArgs.push_back("--headerline");
        commandArgs.push_back(line);
    }
    for (const FrameInfo &frame : frames)
        commandArgs.push_back(frame.filename);

    std::vector<std::string> quoted;
    for (const std::string &arg : commandArgs)
        quoted.push_back(processQuote(arg));

    std::vector<std::string> captured;
    int rc = runCoreCommand(joinStrings(quoted, " "), resolved, captured);
    if (rc != 0) {
        bool printed = false;
        for (const std::string &line : captured)
            if (!shouldSuppressCoreLine(line)) {
                std::cerr << line << std::endl;
                printed = true;
            }
        if (!printed && !captured.empty())
            std::cerr << captured.back() << std::endl;
        if (temporaryCoreOutput)
            removeTree(tmpPath);
        return rc == 0 ? 1 : rc;
    }

    if (resolved.verbose) {
        for (const std::string &line : captured) {
            if (!shouldSuppressCoreLine(line))
                std::cerr << line << std::endl;
        }
    }

    if (resolved.dualFisheye) {
        try {
            const HdrImage fullImage = readRadianceHDRTopDown(hdrPath.string());
            std::vector<std::string> leftHeader = buildDualFisheyeHeaderLines(
                fullImage, resolved, resolved.cropLeft, "left");
            std::vector<std::string> rightHeader = buildDualFisheyeHeaderLines(
                fullImage, resolved, resolved.cropRight, "right");

            {
                const HdrImage crop = cropHdrImageTopLeftTopDown(
                    fullImage, resolved.cropLeft[0], resolved.cropLeft[1],
                    resolved.cropLeft[2], resolved.cropLeft[3]);
                HdrImage projected = projectHdrImageCalibratedToEquidistant(
                    crop, resolved.projectionCoefficients,
                    resolved.projectionSourceRadius, resolved.projectionTargetRadius);
                if (applyVignetting) {
                    projected = applyVignettingCorrectionPylinearhdr(
                        projected, vignettingTable);
                    leftHeader = appendVignettingHeaderLines(
                        leftHeader, resolved.vfilePath);
                }
                writeRadianceHDR(dualOutputs.first.string(), projected, leftHeader, false, true);
            }
            {
                const HdrImage crop = cropHdrImageTopLeftTopDown(
                    fullImage, resolved.cropRight[0], resolved.cropRight[1],
                    resolved.cropRight[2], resolved.cropRight[3]);
                HdrImage projected = projectHdrImageCalibratedToEquidistant(
                    crop, resolved.projectionCoefficients,
                    resolved.projectionSourceRadius, resolved.projectionTargetRadius);
                if (applyVignetting) {
                    projected = applyVignettingCorrectionPylinearhdr(
                        projected, vignettingTable);
                    rightHeader = appendVignettingHeaderLines(
                        rightHeader, resolved.vfilePath);
                }
                writeRadianceHDR(dualOutputs.second.string(), projected, rightHeader, false, true);
            }
            std::cerr << "left output: " << dualOutputs.first.string() << std::endl;
            std::cerr << "right output: " << dualOutputs.second.string() << std::endl;
        } catch (...) {
            removeTree(tmpPath);
            throw;
        }
        removeTree(tmpPath);
        return 0;
    }

    if (singleFisheyeProjection) {
        try {
            const HdrImage source = readRadianceHDRTopDown(hdrPath.string());
            HdrImage projected = projectHdrImageAnglePolynomialToEquidistant(
                source, resolved.projectionCoefficients,
                resolved.projectionSourceRadius, resolved.projectionTargetRadius);
            std::vector<std::string> projectedHeader =
                buildSingleFisheyeHeaderLines(source, resolved);
            if (applyVignetting) {
                projected = applyVignettingCorrectionPylinearhdr(
                    projected, vignettingTable);
                projectedHeader = appendVignettingHeaderLines(
                    projectedHeader, resolved.vfilePath);
            }
            const fs::path projectedPath = resolved.outputOverride
                ? fs::path(resolved.outputPath)
                : tmpPath / "projected.hdr";
            writeRadianceHDR(projectedPath.string(), projected, projectedHeader, false, true);
            if (streamToStdout) {
                copyBinaryFileToStream(projectedPath.string(), std::cout);
                std::cout.flush();
            }
        } catch (...) {
            removeTree(tmpPath);
            throw;
        }
        removeTree(tmpPath);
        return 0;
    }

    if (applyVignetting) {
        try {
            const HdrImage source = readRadianceHDRTopDown(hdrPath.string());
            const HdrImage corrected = applyVignettingCorrectionPylinearhdr(
                source, vignettingTable);
            const std::vector<std::string> correctedHeader =
                appendVignettingHeaderLines(
                    headerLinesForWrite(source), resolved.vfilePath);
            const fs::path correctedPath = resolved.outputOverride
                ? fs::path(resolved.outputPath)
                : tmpPath / "vignetting-corrected.hdr";
            writeRadianceHDR(
                correctedPath.string(), corrected, correctedHeader, false, true);
            if (streamToStdout) {
                copyBinaryFileToStream(correctedPath.string(), std::cout);
                std::cout.flush();
            }
        } catch (...) {
            removeTree(tmpPath);
            throw;
        }
        removeTree(tmpPath);
        return 0;
    }

    if (streamToStdout) {
        copyBinaryFileToStream(hdrPath.string(), std::cout);
        std::cout.flush();
    }

    if (temporaryCoreOutput)
        removeTree(tmpPath);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
#if defined(_WIN32)
    // HDR output can go to stdout; keep it binary (no CR/LF translation).
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    try {
        if (argc > 0 && argv[0])
            g_invoked_argv0 = argv[0];
        std::vector<std::string> args(argv + 1, argv + argc);
        std::string command;
        std::vector<std::string> commandArgs;

        size_t commandIndex = std::string::npos;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "run" || args[i] == "makelist" ||
                    args[i] == "shutter" || args[i] == "aperture" ||
                    args[i] == "chartcells" || args[i] == "hdrcrop" || args[i] == "rotate" ||
                    args[i] == "shadowband" || args[i] == "extract" || args[i] == "convertprojection" ||
                    args[i] == "colorcalibrate" ||
                    args[i] == "evalglare" ||
                    args[i] == "view_visibility" ||
                    args[i] == "view-vos-calculation" ||
                    args[i] == "perceptualmap") {
                commandIndex = i;
                break;
            }
        }

        if (commandIndex == std::string::npos) {
            GlobalOptions global = parseGlobalOptions(args);
            if (global.version) {
                std::cout << MERGEHDR_VERSION << std::endl;
                return 0;
            }
            std::cout << topLevelHelp();
            return 0;
        }

        command = args[commandIndex];
        commandArgs.reserve(args.size() - 1);
        if (command == "convertprojection" || command == "evalglare") {
            for (size_t i = commandIndex + 1; i < args.size(); ++i)
                commandArgs.push_back(args[i]);
        } else {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i == commandIndex)
                    continue;
                commandArgs.push_back(args[i]);
            }
        }

        std::vector<std::string> globalArgs = args;
        if (command == "convertprojection") {
            globalArgs.clear();
            globalArgs.reserve(args.size());
            for (size_t i = 0; i < args.size(); ++i) {
                if (i > commandIndex && (args[i] == "-p" || args[i] == "--polynomial")) {
                    i = std::min(args.size(), i + 5) - 1;
                    continue;
                }
                globalArgs.push_back(args[i]);
            }
        }

        if (command == "evalglare") {
            // evalglare has its own Radiance-style single-letter options (-p, -c, -v ...); only the
            // arguments before the command are global options.
            globalArgs.assign(args.begin(), args.begin() + static_cast<std::ptrdiff_t>(commandIndex));
        }

        GlobalOptions global = parseGlobalOptions(globalArgs);
        if (global.version) {
            std::cout << MERGEHDR_VERSION << std::endl;
            return 0;
        }

        if (command == "run" || command == "makelist") {
            RunOptions opts = parseRunOptions(commandArgs);
            if (global.help || opts.help) {
                std::cout << runHelp(command);
                return 0;
            }
            if (command == "run")
                return runMerge(global, opts);
            return printMakelist(global, opts);
        }

        if (command == "shutter") {
            ShutterOptions opts = parseShutterOptions(commandArgs);
            if (global.help || opts.help) {
                std::cout << shutterHelp(command);
                return 0;
            }
            return printShutterCalibration(global, opts);
        }

        if (command == "aperture") {
            ApertureOptions opts = parseApertureOptions(commandArgs);
            if (global.help || opts.help) {
                std::cout << apertureHelp(command);
                return 0;
            }
            return printApertureCalibration(global, opts);
        }

        if (command == "colorcalibrate") {
            bool helpFlag = false;
            ColorCalibrateOptions opts = parseColorCalibrateOptions(commandArgs, helpFlag);
            if (global.help || helpFlag) {
                std::cout << colorCalibrateHelp(command);
                return 0;
            }
            return printColorCalibration(global, opts);
        }

        if (command == "chartcells") {
            ChartCellsCliOptions opts = parseChartCellsOptions(commandArgs);
            if (global.help || opts.help) {
                std::cout << chartCellsHelp(command);
                return 0;
            }
            return printChartCells(global, opts);
        }

        if (command == "hdrcrop") {
            HdrCropOptions opts = parseHdrCropOptions(commandArgs);
            if (global.help || opts.help) {
                std::cout << hdrCropHelp(command);
                return 0;
            }
            return printHdrCrop(global, opts);
        }

        if (command == "rotate") {
            HdrRotateOptions opts = parseHdrRotateOptions(commandArgs);
            if (global.help || opts.help) {
                std::cout << hdrRotateHelp(command);
                return 0;
            }
            return printHdrRotate(global, opts);
        }

        if (command == "shadowband") {
            ShadowbandCliOptions opts = parseShadowbandOptions(commandArgs);
            if (global.help || opts.help) {
                std::cout << shadowbandHelp(command);
                return 0;
            }
            return printShadowband(opts);
        }

        if (command == "extract") {
            ExtractOptions opts = parseExtractOptions(commandArgs);
            if (global.help || opts.help) {
                std::cout << extractHelp(command);
                return 0;
            }
            return printExtract(opts);
        }

        if (command == "convertprojection") {
            HdrProjectOptions opts = parseHdrProjectOptions(commandArgs);
            if (global.help || opts.help) {
                std::cout << hdrProjectHelp("convertprojection");
                return 0;
            }
            return printHdrProject(global, opts);
        }

        if (command == "evalglare") {
            EvalGlareCliOptions opts = parseEvalGlareOptions(commandArgs);
            if (global.help || opts.help) {
                std::cout << evalGlareHelp("evalglare");
                return 0;
            }
            if (opts.parsed.showProgramVersion) {
                std::cout << MERGEHDR_VERSION << std::endl;
                return 0;
            }
            std::cerr << opts.parsed.stderrText;
            if (opts.parsed.terminate) {
                std::cout << opts.parsed.stdoutText << std::flush;
                return opts.parsed.exitCode;
            }
            validateEvalGlareOptions(opts.native);
            return printEvalGlare(opts.native);
        }

        if (command == "view_visibility") {
            ViewVisibilityCliOptions opts = parseViewVisibilityOptions(commandArgs);
            if (global.help || opts.help) {
                std::cout << viewVisibilityHelp("view_visibility");
                return 0;
            }
            return printViewVisibility(opts);
        }

        if (command == "view-vos-calculation") {
            ViewVosCalculationOptions opts = parseViewVosCalculationOptions(commandArgs);
            if (global.help || opts.help) {
                std::cout << viewVosCalculationHelp("view-vos-calculation");
                return 0;
            }
            return runViewVosCalculation(opts);
        }

        if (command == "perceptualmap") {
            PerceptualMapCliOptions opts = parsePerceptualMapOptions(commandArgs);
            if (global.help || opts.help) {
                std::cout << perceptualMapHelp("perceptualmap");
                return 0;
            }
            return printPerceptualMap(opts);
        }

        std::cerr << "Unknown command: " << command << std::endl;
        return 1;
    } catch (const std::exception &e) {
        std::cerr << "mergehdr: " << e.what() << std::endl;
        return 1;
    }
}
