#ifndef MERGEHDR_GLAREEVAL_H
#define MERGEHDR_GLAREEVAL_H

#include "hdrimage.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

// One "-Q" multi-image entry: image name, position of its pixel (0,0) in the main image and the
// scale factors of the extra scans (the first scan always uses 1.0).
struct EvalGlareMultiImage {
    std::string path;
    int x = 0;
    int y = 0;
    std::vector<double> scales;
};

// Options of the native evalglare (faithful port of Radiance evalglare V3.06). All angles are in
// radians except patchAngleDegrees (the original -z angle is in degrees). The fields up to
// inputPath are the original GUI-facing fields; everything after them was added for the complete
// evalglare option set and defaults to the previous behaviour.
struct EvalGlareOptions {
    bool help = false;
    bool detailed = false;                 // -d
    bool simple = false;                   // -S / --simple (port only: short -d summary line)
    bool force = false;                    // -f (original meaning: ignore the black-corner/-vtv check)
    bool equivLuminance = false;           // --equivlum (port only)
    bool disableSplit = false;             // -x (true) / -y, -Y (false)
    bool thresholdExplicit = false;        // -b given (otherwise -t/-T switch the threshold to 5)
    bool localOnly = false;                // --local-only / -lf (port only)
    bool taskActive = false;               // -t / -T
    bool taskColor = false;                // -T
    double lumThreshold = 2000.0;          // -b
    double maxAngle = 0.2;                 // -r
    double bandAngle = 0.0;                // -B (half angle)
    double splitLimit = 50000.0;           // -Y
    std::string maskPath;                  // -A
    std::string checkPath;                 // -c (also the output of -g and -p)
    int cutViewType = 0;                   // -G type (or -g type with cutViewWriteOnly)
    int taskX = 0;
    int taskY = 0;
    double taskAngle = 0.0;
    int zoneCount = 0;                     // 1: -l, 2: -L
    int zoneCenterX = 0;
    int zoneCenterY = 0;
    double zoneAngle1 = 0.0;
    double zoneAngle2 = 0.0;
    std::string inputPath;                 // "-" reads the picture from stdin (CLI)

    // ---- added: full evalglare option set (defaults keep the previous GUI behaviour) ----
    // Pixel y coordinates (taskY, zoneCenterY, diskY, fillYMax/fillYMin, multi-image y) are
    // Radiance picture rows counted from the bottom (original evalglare/CLI) when true, and
    // top-down rows (GUI) when false; they are converted with y' = height-1-y in the engine.
    bool yFromBottom = false;
    std::vector<std::string> viewOptions;  // command-line view: -vt? -vp -vd -vu -vh -vv -vo -va -vs -vl, -vf file
    bool bandRequested = false;            // -B given (the band is also active whenever bandAngle > 0)
    bool cutViewRequested = false;         // -G given (the cut is also active whenever cutViewType != 0)
    bool cutViewWriteOnly = false;         // -g: write the cut picture to checkPath and stop
    bool smoothing = false;                // -s
    bool applyDisabilityThreshold = false; // -k
    double disabilityThreshold = 0.0;
    bool positionIndexPicture = false;     // -p: write the position index picture to checkPath and stop
    int positionIndexModel = 0;            // -P (0: Guth/Iwata, 1: Kim)
    bool externalIlluminance = false;      // -i / -I
    double externalVerticalIlluminance = 0.0; // E_vl_ext (set by -i, -I, -M, -N)
    bool fillRows = false;                 // -I
    int fillYMax = 0;
    int fillYMin = 0;
    bool clipLuminance = false;            // -m
    int luminanceCorrection = 0;           // 1: -M, 2: -N, 3: -O
    double correctionLuminanceLimit = 0.0; // -m / -M luminance limit
    double correctionNewLuminance = 0.0;   // -m replacement luminance
    int diskX = 0;                         // -N / -O
    int diskY = 0;
    double diskAngle = 0.0;
    double replacementLuminance = 0.0;     // -O
    std::string correctionOutputPath;      // picture written by -m/-M/-N/-O
    int backgroundMode = 0;                // -q (0: (E_v-E_dir)/pi, 1: average, 2: E_v/pi)
    double dgpA1 = 2.0, dgpA2 = 1.0, dgpA3 = 1.87, dgpA4 = 2.0, dgpA5 = 1.0; // -w
    double dgpC1 = 5.87e-05, dgpC2 = 0.092, dgpC3 = 0.159;
    bool verticalIlluminanceOnly = false;  // -V
    bool lowLightCorrection = false;       // -C l+ (on) / l- or 0 (off), -4 (off)
    int outputMode = 0;                    // 1: -1, 2: -2, 3: -3, 4: -z ... 3
    double directIlluminance = 0.0;        // -2 dir_ill
    bool detailedShort = false;            // -D
    bool uniformSourceColor = false;       // -u
    double uniformRed = 0.33333, uniformGreen = 0.33333, uniformBlue = 0.33333;
    double patchAngleDegrees = 25.0;       // -z angle (degrees, as in the original)
    int patchMode = 0;                     // -z mode
    bool multiImageMode = false;           // -Q
    int multiImageExtraScans = 0;
    std::vector<EvalGlareMultiImage> multiImages;
    std::string commandLine;               // recorded in the header of written pictures
    // Keep the per-pixel geometry (directions + solid angles, ~33 bytes/pixel) after the call so a
    // later call with the same view, size and orientation skips its computation (GUI re-runs);
    // clearEvalGlareCaches() releases it. Off by default: nothing is retained between calls.
    bool cacheGeometry = false;
};

// Result of parseEvalGlareArguments(): a boost-free, faithful re-implementation of the original
// evalglare argument loop plus the port-only long options.
struct EvalGlareCliParseResult {
    EvalGlareOptions options;
    bool showHelp = false;            // --help
    bool showProgramVersion = false;  // --version (the caller prints its own version)
    bool terminate = false;           // stop without analysing: print the texts, return exitCode
    int exitCode = 0;
    std::string stdoutText;           // e.g. the -v release line
    std::string stderrText;           // e.g. usage errors, warnings
};

struct EvalGlareMaskStats {
    bool active = false;
    int pixelCount = 0;
    double solidAngle = 0.0;
    double averageLuminance = 0.0;
    double medianLuminance = 0.0;
    double stdLuminance = 0.0;
    double percentile75 = 0.0;
    double percentile95 = 0.0;
    double minLuminance = 0.0;
    double maxLuminance = 0.0;
    double pgsvCon = -99.0;
    double pgsvSat = -99.0;
    double pgsv = -99.0;
    double verticalIlluminance = 0.0;
};

struct EvalGlareZoneStats {
    bool active = false;
    int pixelCount = 0;
    double solidAngle = 0.0;
    double averageLuminance = 0.0;
    double medianLuminance = 0.0;
    double stdLuminance = 0.0;
    double percentile75 = 0.0;
    double percentile95 = 0.0;
    double minLuminance = 0.0;
    double maxLuminance = 0.0;
    double verticalIlluminance = 0.0;
};

struct EvalGlareSourceDetail {
    int index = 0;
    int pixelCount = 0;
    int glareZone = 0;
    double avgPosX = 0.0;
    double avgPosY = 0.0;
    double avgLum = 0.0;
    double omega = 0.0;
    double posIndex = 0.0;
    double sigma = 0.0;
    double xdir = 0.0;
    double ydir = 0.0;
    double zdir = 0.0;
    double eGlare = 0.0;
    double lveilCie = 0.0;
    double theta = 0.0;
    double rContrast = 0.0;
    double rDgp = 0.0;
};

struct EvalGlareAnalysisResult {
    size_t width = 0;
    size_t height = 0;
    int sourceCount = 0;
    double averageLuminance = 0.0;
    double averageLuminancePos = 0.0;
    double averageLuminancePos2 = 0.0;
    double medianLuminance = 0.0;
    double medianLuminancePos = 0.0;
    double medianLuminancePos2 = 0.0;
    double verticalIlluminance = 0.0;
    double directVerticalIlluminance = 0.0;
    double backgroundLuminance = 0.0;
    double sourceAverageLuminance = 0.0;
    double sourceSolidAngle = 0.0;
    double dgp = 0.0;
    double dgm = 0.0;
    double dgi = 0.0;
    double ugr = 0.0;
    double vcp = 100.0;
    double cgi = 0.0;
    double dgr = 0.0;
    double ugp = 0.0;
    double ugp2 = 0.0;
    double ugrExp = 0.0;
    double dgiMod = 0.0;
    double disabilityGlare = 0.0;
    double lveilCieSum = 0.0;
    double maxLuminance = 0.0;
    double taskLuminance = 0.0;
    double totalSolidAngle = 0.0;
    double glareTerm = 0.0;
    double dgmGlareTerm = 0.0;
    bool hasBand = false;
    double bandSolidAngle = 0.0;
    double bandAverageLuminance = 0.0;
    double bandMedianLuminance = 0.0;
    double bandStdLuminance = 0.0;
    double bandPercentile75 = 0.0;
    double bandPercentile95 = 0.0;
    double bandMinLuminance = 0.0;
    double bandMaxLuminance = 0.0;
    EvalGlareMaskStats mask;
    int zoneCount = 0;
    int zoneCenterX = 0;
    int zoneCenterY = 0;
    double zoneAngle1 = 0.0;
    double zoneAngle2 = 0.0;
    EvalGlareZoneStats zone1;
    EvalGlareZoneStats zone2;
    std::vector<EvalGlareSourceDetail> sources;
    std::vector<int> sourceAssignments;  // per pixel (row 0 = bottom): index into sources or -1
    std::vector<int> zoneAssignments;    // per pixel (row 0 = bottom): 0, 1 or 2
    // ---- added ----
    std::vector<unsigned char> bandAssignments;  // per pixel (row 0 = bottom): 1 = in the -B band
    std::vector<unsigned char> taskAssignments;  // per pixel (row 0 = bottom): 1 = in the task area
    std::string outputText;        // what evalglare prints to stdout for these options
    std::string messages;          // what evalglare prints to stderr (warnings, notices, errors)
    int exitStatus = 0;            // evalglare exit status (1 for the original's early exits)
    bool completed = false;        // the glare evaluation ran to the end
    bool outputDetailed = false;   // options used for outputText (see formatEvalGlare)
    bool outputSimple = false;
};

struct ViewVisibilitySummaryOptions {
    enum class DetailMode {
        HdrvdpSubset,
        ContrastProxy
    };

    enum class InputColor {
        Rad,
        Srgb,
        Xyz
    };

    std::string referencePath;
    std::string testPath;
    std::string maskPath;
    double pixelsPerDegree = 30.0;
    double sensitivityCorrection = -1.0;
    std::string spectralEmissionPath;
    DetailMode detailMode = DetailMode::HdrvdpSubset;
    InputColor inputColor = InputColor::Rad;
};

struct ViewVisibilitySummaryResult {
    size_t width = 0;
    size_t height = 0;
    double viewSize = 0.0;
    size_t viewPixelCount = 0;
    double referenceTotal = 0.0;
    double visibilityTotal = 0.0;
    double referenceBaseline = 0.0;
    double visibilityRatio = 0.0;
    double qualityScore = 0.0;
    std::string detailLabel;
    std::string inputColor;
    std::vector<float> testContrastMap;
    std::vector<float> visibilityMap;
    std::vector<float> referenceContrastMap;
};

enum class PerceptualMapKind {
    L,
    M,
    Rod,
    LPlusM,
    Adaptation,
    DetectableContrast,
    EqvLuminance
};

struct PerceptualMapOptions {
    std::string inputPath;
    double pixelsPerDegree = 30.0;
    double sensitivityCorrection = -1.0;
    std::string spectralEmissionPath;
    // How the file's three channels are read (as View Visibility's input colour). Rad is the
    // former fixed behaviour; Xyz with three equal channels reads a luminance (cd/m²) picture.
    ViewVisibilitySummaryOptions::InputColor inputColor = ViewVisibilitySummaryOptions::InputColor::Rad;
};

struct PerceptualMapResult {
    size_t width = 0;
    size_t height = 0;
    PerceptualMapKind kind = PerceptualMapKind::L;
    std::string label;
    std::vector<float> values;
};

struct HdrLuminanceEvaluator {
    bool valid = false;
    bool monochrome = false;
    std::array<double, 3> weights{{0.0, 0.0, 0.0}};
    double exposureScale = 1.0;
    std::string description;
};

std::string evalGlareHelp(const std::string &command);
// Parses evalglare arguments (without the program/command name) like the original argv loop.
EvalGlareCliParseResult parseEvalGlareArguments(const std::vector<std::string> &args);
// Runs the evaluation; throws std::runtime_error if evalglare stopped with an error.
EvalGlareAnalysisResult analyzeEvalGlare(const EvalGlareOptions &opts);
// With EvalGlareOptions::cacheGeometry the per-pixel geometry of the last evaluated picture is
// kept for the next call (same view, size and orientation); this releases it.
void clearEvalGlareCaches();
ViewVisibilitySummaryResult analyzeViewVisibilitySummary(const ViewVisibilitySummaryOptions &opts);
PerceptualMapResult analyzePerceptualMap(const PerceptualMapOptions &opts, PerceptualMapKind kind);
HdrLuminanceEvaluator resolveHdrLuminanceEvaluator(const HdrImage &image);
double evaluateHdrLuminance(const HdrImage &image, size_t x, size_t y, const HdrLuminanceEvaluator &evaluator);
void writeViewVisibilityResultHdr(const std::string &path, const ViewVisibilitySummaryResult &result);
void writePerceptualMapHdr(const std::string &path, const PerceptualMapResult &result);
void writeViewVisibilityDebugDump(const ViewVisibilitySummaryOptions &opts, const std::string &outDir);
std::string formatEvalGlare(const EvalGlareAnalysisResult &result, bool detailed, bool simple);
int printEvalGlare(const EvalGlareOptions &opts);

#endif
