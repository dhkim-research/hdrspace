#include "glareeval.h"

#include "hdrimage.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <chrono>
#include <clocale>
#include <complex>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_OPENMP)
#include <omp.h>
#endif

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#include <mach-o/dyld.h>
#endif
#if defined(_WIN32)
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void *, wchar_t *, unsigned long);
#endif
#if __has_include(<fftw3.h>)
#include <fftw3.h>
#define MERGEHDR_HAS_FFTW 1
#elif __has_include("/opt/homebrew/include/fftw3.h")
#include "/opt/homebrew/include/fftw3.h"
#define MERGEHDR_HAS_FFTW 1
#elif __has_include("/usr/local/include/fftw3.h")
#include "/usr/local/include/fftw3.h"
#define MERGEHDR_HAS_FFTW 1
#endif

extern "C" {
#include "convolve.h"
}

#include <unsupported/Eigen/FFT>

namespace {

const double kPi = std::acos(-1.0);
const double kWhiteEfficacy = 179.0;
const double kDgmScaleThreshold = 2.346;
const double kDgmThresholdCapDegrees = 6.552;

struct CachedHdrImageEntry {
    std::filesystem::file_time_type mtime;
    HdrImage image;
};

double pvalueDecimalRoundTrip(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.4g", value);
    return std::strtod(buffer, nullptr);
}

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

const char *viewVisibilityInputColorLabel(ViewVisibilitySummaryOptions::InputColor inputColor) {
    switch (inputColor) {
        case ViewVisibilitySummaryOptions::InputColor::Srgb: return "srgb";
        case ViewVisibilitySummaryOptions::InputColor::Xyz: return "xyz";
        case ViewVisibilitySummaryOptions::InputColor::XyzCdm2: return "xyz-cdm2";
        case ViewVisibilitySummaryOptions::InputColor::Rad:
        default: return "rad";
    }
}

// `inputScale` comes from viewVisibilityInputScale() for the same picture and input colour.
Vec3d viewVisibilityInputXyzAt(
    const HdrImage &image,
    size_t pixelIndex,
    ViewVisibilitySummaryOptions::InputColor inputColor,
    double inputScale
) {
    const size_t index = pixelIndex * 3;
    const double c0 = static_cast<double>(image.rgb[index + 0]);
    const double c1 = static_cast<double>(image.rgb[index + 1]);
    const double c2 = static_cast<double>(image.rgb[index + 2]);

    if (inputColor == ViewVisibilitySummaryOptions::InputColor::Xyz ||
        inputColor == ViewVisibilitySummaryOptions::InputColor::XyzCdm2)
        return Vec3d{c0 * inputScale, c1 * inputScale, c2 * inputScale};

    const double r = c0 * inputScale;
    const double g = c1 * inputScale;
    const double b = c2 * inputScale;
    if (inputColor == ViewVisibilitySummaryOptions::InputColor::Srgb) {
        return Vec3d{
            0.4124564 * r + 0.3575761 * g + 0.1804375 * b,
            0.2126729 * r + 0.7151522 * g + 0.0721750 * b,
            0.0193339 * r + 0.1191920 * g + 0.9503041 * b,
        };
    }

    return Vec3d{
        0.51408315 * r + 0.32388874 * g + 0.16202811 * b,
        0.26507413 * r + 0.67011463 * g + 0.06481124 * b,
        0.02409765 * r + 0.12285435 * g + 0.85334803 * b,
    };
}

struct ViewBasis {
    Vec3d vdir{0.0, 1.0, 0.0};
    Vec3d vup{0.0, 0.0, 1.0};
    Vec3d hv;
    Vec3d vv;
};

struct HeaderLuminanceInfo {
    enum Mode {
        Unavailable,
        Weighted,
        Monochrome
    };

    Mode mode = Unavailable;
    std::array<double, 3> weights{{0.0, 0.0, 0.0}};
    double exposureScale = 1.0;
    std::string description;
};

struct Mat3d {
    double m[3][3] = {
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
        {0.0, 0.0, 1.0},
    };
};

struct EquivalentLuminanceInputInfo {
    enum Mode {
        Unsupported,
        RadianceRgb,
        Srgb,
        Xyz
    };

    Mode mode = Unsupported;
    Mat3d rgbToXyz;
    Mat3d xyzToRadianceRgb;
    double exposureScale = 1.0;
    std::string description;
};

std::string trimCopy(const std::string &value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return value.substr(begin, end - begin);
}

std::vector<double> parseDoubleList(const std::string &value) {
    std::vector<double> result;
    std::istringstream iss(value);
    double parsed = 0.0;
    while (iss >> parsed)
        result.push_back(parsed);
    return result;
}

bool parseHeaderTriplet(const HdrImage &image, const std::string &key, std::array<double, 3> &values) {
    std::map<std::string, std::string>::const_iterator it = image.header.find(key);
    if (it == image.header.end())
        return false;
    const std::vector<double> parsed = parseDoubleList(it->second);
    if (parsed.size() != 3)
        return false;
    values[0] = parsed[0];
    values[1] = parsed[1];
    values[2] = parsed[2];
    return true;
}

bool stringContainsInsensitive(const std::string &haystack, const std::string &needle);

// Product of the picture's EXPOSURE, as Radiance reads it (isexpos/exposval): every line that
// starts with "EXPOSURE=" multiplies; indented lines are headers of input pictures copied by the
// writing program (pcomb and others) and do not describe these pixels.
double radianceExposureProduct(const HdrImage &image) {
    const std::vector<std::string> &lines = image.rawHeaderLines.empty() ? image.headerLines : image.rawHeaderLines;
    double exposureProduct = 1.0;
    for (const std::string &line : lines) {
        if (line.compare(0, 9, "EXPOSURE=") != 0)
            continue;
        const double exposure = std::atof(line.c_str() + 9);
        if (exposure > 0.0)
            exposureProduct *= exposure;
    }
    return exposureProduct;
}

double headerExposureScale(const HdrImage &image) {
    const double exposureProduct = radianceExposureProduct(image);
    if (exposureProduct > 0.0)
        return 1.0 / exposureProduct;
    return 1.0;
}

// Factor from stored pixel values to cd/m² (XYZ) or to luminous RGB for the HDR-VDP input:
// Radiance values (Rad, sRGB, XYZ) are multiplied by 179, as in luminanceAt(), and the header
// EXPOSURE is undone. XyzCdm2 marks XYZ already stored in cd/m².
// EXPOSURE follows Radiance, see radianceExposureProduct().
double viewVisibilityInputScale(const HdrImage &image, ViewVisibilitySummaryOptions::InputColor inputColor) {
    const double exposureScale = headerExposureScale(image);
    if (inputColor == ViewVisibilitySummaryOptions::InputColor::XyzCdm2)
        return exposureScale;
    return kWhiteEfficacy * exposureScale;
}

bool parseTargetPrimariesAndWhite(const HdrImage &image, std::array<double, 8> &values) {
    std::map<std::string, std::string>::const_iterator primariesIt = image.header.find("TargetPrimaries");
    std::map<std::string, std::string>::const_iterator whiteIt = image.header.find("TargetWhitePoint");
    if (primariesIt == image.header.end() || whiteIt == image.header.end())
        return false;

    const std::vector<double> primaries = parseDoubleList(primariesIt->second);
    const std::vector<double> white = parseDoubleList(whiteIt->second);
    if (primaries.size() != 6 || white.size() != 2)
        return false;

    for (size_t i = 0; i < 6; ++i)
        values[i] = primaries[i];
    values[6] = white[0];
    values[7] = white[1];
    return true;
}

bool parseRadiancePrimaries(const HdrImage &image, std::array<double, 8> &values) {
    std::map<std::string, std::string>::const_iterator it = image.header.find("PRIMARIES");
    if (it == image.header.end())
        return false;
    const std::vector<double> parsed = parseDoubleList(it->second);
    if (parsed.size() != 8)
        return false;
    for (size_t i = 0; i < 8; ++i)
        values[i] = parsed[i];
    return true;
}

bool parseHeaderMatrix3x3(const HdrImage &image, const std::string &key, Mat3d &matrix) {
    std::map<std::string, std::string>::const_iterator it = image.header.find(key);
    if (it == image.header.end())
        return false;
    const std::vector<double> parsed = parseDoubleList(it->second);
    if (parsed.size() != 9)
        return false;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            matrix.m[row][col] = parsed[static_cast<size_t>(row * 3 + col)];
    }
    return true;
}

double determinant(const Mat3d &matrix) {
    return
        matrix.m[0][0] * (matrix.m[1][1] * matrix.m[2][2] - matrix.m[1][2] * matrix.m[2][1]) -
        matrix.m[0][1] * (matrix.m[1][0] * matrix.m[2][2] - matrix.m[1][2] * matrix.m[2][0]) +
        matrix.m[0][2] * (matrix.m[1][0] * matrix.m[2][1] - matrix.m[1][1] * matrix.m[2][0]);
}

bool invertMatrix(const Mat3d &input, Mat3d &output) {
    const double det = determinant(input);
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

bool parseSensorToXyz(const HdrImage &image, Mat3d &sensorToXyz) {
    if (parseHeaderMatrix3x3(image, "SENSOR2XYZ", sensorToXyz))
        return true;

    Mat3d xyzCam;
    if (!parseHeaderMatrix3x3(image, "XYZCAM", xyzCam))
        return false;

    std::array<double, 3> premults{{1.0, 1.0, 1.0}};
    parseHeaderTriplet(image, "CAM_PREMULTIPLIERS", premults);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            xyzCam.m[row][col] *= premults[static_cast<size_t>(row)];
    }

    return invertMatrix(xyzCam, sensorToXyz);
}

std::array<double, 8> radiancePrimariesAndWhite() {
    return {{0.6400, 0.3300, 0.2900, 0.6000, 0.1500, 0.0600, 0.3333, 0.3333}};
}

std::array<double, 8> srgbPrimariesAndWhite() {
    return {{0.6400, 0.3300, 0.3000, 0.6000, 0.1500, 0.0600, 0.3127, 0.3290}};
}

std::array<double, 8> xyzPrimariesAndWhite() {
    return {{1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.3333, 0.3333}};
}

bool samePrimariesAndWhite(const std::array<double, 8> &a, const std::array<double, 8> &b, double eps = 5e-3) {
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i] - b[i]) > eps)
            return false;
    }
    return true;
}

Mat3d radianceRgbToXyzMatrix() {
    Mat3d matrix{};
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

bool primariesToXyzMatrix(const std::array<double, 8> &primaries, Mat3d &matrix) {
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

    Mat3d pxyz{};
    pxyz.m[0][0] = rx;
    pxyz.m[1][0] = ry;
    pxyz.m[2][0] = 1.0 - rx - ry;
    pxyz.m[0][1] = gx;
    pxyz.m[1][1] = gy;
    pxyz.m[2][1] = 1.0 - gx - gy;
    pxyz.m[0][2] = bx;
    pxyz.m[1][2] = by;
    pxyz.m[2][2] = 1.0 - bx - by;

    Mat3d inversePxyz;
    if (!invertMatrix(pxyz, inversePxyz))
        return false;

    const Vec3d white{
        wx / wy,
        1.0,
        (1.0 - wx - wy) / wy,
    };

    const Vec3d scales{
        inversePxyz.m[0][0] * white.x + inversePxyz.m[0][1] * white.y + inversePxyz.m[0][2] * white.z,
        inversePxyz.m[1][0] * white.x + inversePxyz.m[1][1] * white.y + inversePxyz.m[1][2] * white.z,
        inversePxyz.m[2][0] * white.x + inversePxyz.m[2][1] * white.y + inversePxyz.m[2][2] * white.z,
    };

    matrix = pxyz;
    for (int col = 0; col < 3; ++col) {
        matrix.m[0][col] *= (col == 0 ? scales.x : (col == 1 ? scales.y : scales.z));
        matrix.m[1][col] *= (col == 0 ? scales.x : (col == 1 ? scales.y : scales.z));
        matrix.m[2][col] *= (col == 0 ? scales.x : (col == 1 ? scales.y : scales.z));
    }
    return true;
}

bool weightsFromPrimaries(const std::array<double, 8> &primaries, std::array<double, 3> &weights) {
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

    Mat3d pxyz;
    pxyz.m[0][0] = rx;
    pxyz.m[1][0] = ry;
    pxyz.m[2][0] = 1.0 - rx - ry;
    pxyz.m[0][1] = gx;
    pxyz.m[1][1] = gy;
    pxyz.m[2][1] = 1.0 - gx - gy;
    pxyz.m[0][2] = bx;
    pxyz.m[1][2] = by;
    pxyz.m[2][2] = 1.0 - bx - by;

    Mat3d inversePxyz;
    if (!invertMatrix(pxyz, inversePxyz))
        return false;

    const Vec3d white{
        wx / wy,
        1.0,
        (1.0 - wx - wy) / wy,
    };

    const Vec3d scales{
        inversePxyz.m[0][0] * white.x + inversePxyz.m[0][1] * white.y + inversePxyz.m[0][2] * white.z,
        inversePxyz.m[1][0] * white.x + inversePxyz.m[1][1] * white.y + inversePxyz.m[1][2] * white.z,
        inversePxyz.m[2][0] * white.x + inversePxyz.m[2][1] * white.y + inversePxyz.m[2][2] * white.z,
    };

    weights[0] = pxyz.m[1][0] * scales.x;
    weights[1] = pxyz.m[1][1] * scales.y;
    weights[2] = pxyz.m[1][2] * scales.z;
    return std::isfinite(weights[0]) && std::isfinite(weights[1]) && std::isfinite(weights[2]) &&
           (weights[0] > 0.0 || weights[1] > 0.0 || weights[2] > 0.0);
}

bool weightsFromSensorToXyz(const Mat3d &sensorToXyz, std::array<double, 3> &weights) {
    weights[0] = sensorToXyz.m[1][0];
    weights[1] = sensorToXyz.m[1][1];
    weights[2] = sensorToXyz.m[1][2];
    return std::isfinite(weights[0]) && std::isfinite(weights[1]) && std::isfinite(weights[2]) &&
           (weights[0] > 0.0 || weights[1] > 0.0 || weights[2] > 0.0);
}

bool imageIsMonochromeRgb(const HdrImage &image) {
    const double epsilon = 1e-6;
    for (size_t i = 0; i + 2 < image.rgb.size(); i += 3) {
        const double r = image.rgb[i + 0];
        const double g = image.rgb[i + 1];
        const double b = image.rgb[i + 2];
        const double scale = std::max({1.0, std::abs(r), std::abs(g), std::abs(b)});
        if (std::abs(r - g) > epsilon * scale || std::abs(r - b) > epsilon * scale)
            return false;
    }
    return !image.rgb.empty();
}

bool stringContainsInsensitive(const std::string &haystack, const std::string &needle);

bool headerContainsInsensitive(const HdrImage &image, const std::string &needle) {
    for (const std::string &line : image.headerLines) {
        if (stringContainsInsensitive(line, needle))
            return true;
    }
    return false;
}

bool looksLikeTechnoteamOrLmk(const HdrImage &image) {
    return headerContainsInsensitive(image, "technoteam") ||
           headerContainsInsensitive(image, "lmk") ||
           headerContainsInsensitive(image, "pftopic") ||
           headerContainsInsensitive(image, "pcftoxyz") ||
           headerContainsInsensitive(image, "programversion=standard color") ||
           headerContainsInsensitive(image, "camera=svs") ||
           headerContainsInsensitive(image, "camera=tt");
}

std::string lowerCopy(const std::string &value);

// A Radiance RGBE picture: a FORMAT header whose value is 32-bit_rle_rgbe (case-insensitive), or no
// FORMAT line at all in a file whose magic line is "#?RADIANCE" (Radiance then assumes RGBE).
// (The previous test searched headerLines for "format=32-bit_rle_rgbe" / "#?radiance", which never
// matched: readRadianceHDR() stores "FORMAT= 32-bit_rle_rgbe" and does not keep the magic line in
// headerLines, so plain colour Radiance pictures were rejected.)
bool headerDeclaresRadianceRgbe(const HdrImage &image) {
    bool sawFormat = false;
    for (const auto &entry : image.header) {
        if (lowerCopy(trimCopy(entry.first)) != "format")
            continue;
        sawFormat = true;
        if (lowerCopy(trimCopy(entry.second)) == "32-bit_rle_rgbe")
            return true;
    }
    for (const std::string &line : image.headerLines) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || lowerCopy(trimCopy(line.substr(0, eq))) != "format")
            continue;
        sawFormat = true;
        if (lowerCopy(trimCopy(line.substr(eq + 1))) == "32-bit_rle_rgbe")
            return true;
    }
    if (sawFormat)
        return false;
    return lowerCopy(trimCopy(image.magicLine)) == "#?radiance";
}

bool looksLikePlainRadianceRgb(const HdrImage &image) {
    if (!headerDeclaresRadianceRgbe(image))
        return false;

    if (image.header.find("LuminanceRGB") != image.header.end() ||
        image.header.find("SENSOR2XYZ") != image.header.end() ||
        image.header.find("XYZCAM") != image.header.end() ||
        image.header.find("TargetPrimaries") != image.header.end() ||
        image.header.find("TargetWhitePoint") != image.header.end() ||
        image.header.find("PRIMARIES") != image.header.end()) {
        return false;
    }

    return !looksLikeTechnoteamOrLmk(image);
}

EquivalentLuminanceInputInfo resolveEquivalentLuminanceInputInfo(const HdrImage &image) {
    EquivalentLuminanceInputInfo info;
    info.exposureScale = headerExposureScale(image);

    if (imageIsMonochromeRgb(image)) {
        info.description = "Eqv_Luminance requires color HDR input; monochrome/luminance-only input is not supported.";
        return info;
    }

    std::array<double, 8> primaries{};
    if (parseTargetPrimariesAndWhite(image, primaries) || parseRadiancePrimaries(image, primaries)) {
        if (samePrimariesAndWhite(primaries, xyzPrimariesAndWhite())) {
            info.mode = EquivalentLuminanceInputInfo::Xyz;
            info.description = "XYZ input";
            return info;
        }
        if (samePrimariesAndWhite(primaries, srgbPrimariesAndWhite())) {
            info.mode = EquivalentLuminanceInputInfo::Srgb;
            if (!primariesToXyzMatrix(primaries, info.rgbToXyz))
                throw std::runtime_error("Could not derive sRGB->XYZ conversion for Eqv_Luminance.");
            if (!invertMatrix(radianceRgbToXyzMatrix(), info.xyzToRadianceRgb))
                throw std::runtime_error("Could not derive XYZ->Radiance RGB conversion for Eqv_Luminance.");
            info.description = "sRGB input";
            return info;
        }
        if (samePrimariesAndWhite(primaries, radiancePrimariesAndWhite())) {
            info.mode = EquivalentLuminanceInputInfo::RadianceRgb;
            info.rgbToXyz = radianceRgbToXyzMatrix();
            info.description = "Radiance RGB input";
            return info;
        }
        info.description = "Eqv_Luminance only supports Rad, sRGB, or XYZ HDR inputs.";
        return info;
    }

    Mat3d sensorToXyz;
    if (parseSensorToXyz(image, sensorToXyz)) {
        info.description = "Eqv_Luminance only supports Rad, sRGB, or XYZ HDR inputs; raw/sensor-space input is not supported.";
        return info;
    }

    if (looksLikePlainRadianceRgb(image)) {
        info.mode = EquivalentLuminanceInputInfo::RadianceRgb;
        info.rgbToXyz = radianceRgbToXyzMatrix();
        info.description = "Plain Radiance RGB fallback";
        return info;
    }

    info.description = "Eqv_Luminance needs a supported color space (Rad, sRGB, or XYZ).";
    return info;
}

bool profileEnabled() {
    const char *env = std::getenv("MERGEHDR_EVALGLARE_PROFILE");
    if (env == nullptr)
        return false;
    std::string value = trimCopy(env);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

void logProfileStage(const std::string &name,
        const std::chrono::steady_clock::time_point &begin,
        const std::chrono::steady_clock::time_point &end,
        const std::string &extra = std::string()) {
    if (!profileEnabled())
        return;
    const double seconds = std::chrono::duration<double>(end - begin).count();
    std::cerr << "[evalglare] " << name << ": " << std::fixed << std::setprecision(3) << seconds << "s";
    if (!extra.empty())
        std::cerr << "  " << extra;
    std::cerr << std::endl;
}

std::string lowerCopy(const std::string &value) {
    std::string result = value;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool stringContainsInsensitive(const std::string &haystack, const std::string &needle) {
    return lowerCopy(haystack).find(lowerCopy(needle)) != std::string::npos;
}

Vec3d operator+(const Vec3d &a, const Vec3d &b) {
    return Vec3d{a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3d operator*(const Vec3d &v, double scalar) {
    return Vec3d{v.x * scalar, v.y * scalar, v.z * scalar};
}

double dot(const Vec3d &a, const Vec3d &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3d cross(const Vec3d &a, const Vec3d &b) {
    return Vec3d{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

double norm(const Vec3d &v) {
    return std::sqrt(dot(v, v));
}

Vec3d normalize(const Vec3d &v) {
    const double length = norm(v);
    if (length <= 0.0)
        return Vec3d();
    return v * (1.0 / length);
}

double clampDot(double value) {
    return std::max(-1.0, std::min(1.0, value));
}

void finalizeViewBasis(ViewBasis &view) {
    view.vdir = normalize(view.vdir);
    view.vup = normalize(view.vup);
    view.hv = normalize(cross(view.vdir, view.vup));
    if (norm(view.hv) <= 0.0)
        view.hv = Vec3d{1.0, 0.0, 0.0};
    view.vv = normalize(cross(view.hv, view.vdir));
    if (norm(view.vv) <= 0.0)
        view.vv = Vec3d{0.0, 0.0, 1.0};
}

HeaderLuminanceInfo resolveLuminanceInfo(const HdrImage &image) {
    HeaderLuminanceInfo info;

    info.exposureScale = headerExposureScale(image);

    if (imageIsMonochromeRgb(image)) {
        info.mode = HeaderLuminanceInfo::Monochrome;
        info.description = "Monochrome RGB input";
        return info;
    }

    if (parseHeaderTriplet(image, "LuminanceRGB", info.weights)) {
        info.mode = HeaderLuminanceInfo::Weighted;
        info.description = "LuminanceRGB header";
        return info;
    }

    Mat3d sensorToXyz;
    if (parseSensorToXyz(image, sensorToXyz) && weightsFromSensorToXyz(sensorToXyz, info.weights)) {
        info.mode = HeaderLuminanceInfo::Weighted;
        info.description = image.header.find("SENSOR2XYZ") != image.header.end()
            ? "SENSOR2XYZ header"
            : "XYZCAM header";
        return info;
    }

    std::array<double, 8> primaries{};
    if (parseTargetPrimariesAndWhite(image, primaries) && weightsFromPrimaries(primaries, info.weights)) {
        info.mode = HeaderLuminanceInfo::Weighted;
        info.description = "TargetPrimaries/TargetWhitePoint header";
        return info;
    }
    if (parseRadiancePrimaries(image, primaries) && weightsFromPrimaries(primaries, info.weights)) {
        info.mode = HeaderLuminanceInfo::Weighted;
        info.description = "Radiance PRIMARIES header";
        return info;
    }

    if (looksLikeTechnoteamOrLmk(image)) {
        info.mode = HeaderLuminanceInfo::Weighted;
        info.weights = {{0.265074126, 0.670114631, 0.064811243}};
        info.description = "Technoteam/LMK Radiance fallback";
        return info;
    }

    if (looksLikePlainRadianceRgb(image)) {
        info.mode = HeaderLuminanceInfo::Weighted;
        info.weights = {{0.265074126, 0.670114631, 0.064811243}};
        info.description = "Plain Radiance RGB fallback";
        return info;
    }

    info.description = "No usable source color metadata was found. Need LuminanceRGB, SENSOR2XYZ/XYZCAM, or primaries; monochrome RGB is also supported.";

    return info;
}

HeaderLuminanceInfo resolveViewVisibilityLuminanceInfo(const HdrImage &image) {
    HeaderLuminanceInfo info = resolveLuminanceInfo(image);
    if (info.mode != HeaderLuminanceInfo::Unavailable)
        return info;

    // View-visibility comparisons are primarily used with rendered RGB HDR pairs.
    // If no explicit camera/source metadata exists, fall back to Radiance RGB
    // luminance weights instead of rejecting the image outright.
    if (!image.rgb.empty() && image.rgb.size() == image.width * image.height * 3) {
        info.mode = HeaderLuminanceInfo::Weighted;
        info.weights = {{0.265074126, 0.670114631, 0.064811243}};
        info.description = "View visibility RGB fallback";
    }

    return info;
}

double luminanceAt(const HdrImage &image, size_t x, size_t y, const HeaderLuminanceInfo &info) {
    const size_t index = (y * image.width + x) * 3;
    double luminance = 0.0;
    if (info.mode == HeaderLuminanceInfo::Monochrome) {
        luminance = image.rgb[index + 1];
    } else {
        luminance =
            image.rgb[index + 0] * info.weights[0] +
            image.rgb[index + 1] * info.weights[1] +
            image.rgb[index + 2] * info.weights[2];
    }
    luminance *= kWhiteEfficacy;
    luminance *= info.exposureScale;
    return luminance;
}

double radianceRgbLuminanceRawAt(const HdrImage &image, size_t x, size_t y) {
    const size_t index = (y * image.width + x) * 3;
    return
        static_cast<double>(image.rgb[index + 0]) * 0.265074126 +
        static_cast<double>(image.rgb[index + 1]) * 0.670114631 +
        static_cast<double>(image.rgb[index + 2]) * 0.064811243;
}

double compareMaskWeightAt(const HdrImage &image, size_t x, size_t y) {
    const size_t index = (y * image.width + x) * 3;
    return std::max(0.0, static_cast<double>(image.rgb[index + 0]));
}

Vec3d mulMat3Vec(const Mat3d &matrix, const Vec3d &value);

Vec3d equivalentLuminanceXyzAt(
    const HdrImage &image,
    size_t x,
    size_t y,
    const EquivalentLuminanceInputInfo &info
) {
    const size_t index = (y * image.width + x) * 3;
    const double scale = kWhiteEfficacy * info.exposureScale;
    const Vec3d channels{
        static_cast<double>(image.rgb[index + 0]) * scale,
        static_cast<double>(image.rgb[index + 1]) * scale,
        static_cast<double>(image.rgb[index + 2]) * scale,
    };
    if (info.mode == EquivalentLuminanceInputInfo::Xyz)
        return channels;
    return mulMat3Vec(info.rgbToXyz, channels);
}

double approximateScotopicLuminanceFromXyz(const Vec3d &xyz) {
    const double X = std::max(xyz.x, 1e-8);
    const double Y = std::max(0.0, xyz.y);
    const double Z = std::max(0.0, xyz.z);
    if (Y <= 0.0)
        return 0.0;
    const double scotopicRatio = 1.33 * (1.0 + (Y + Z) / X) - 1.68;
    return std::max(0.0, Y * scotopicRatio);
}

double approximateScotopicLuminanceFromRgb(const Vec3d &rgbRadiance) {
    const double r = std::max(0.0, rgbRadiance.x);
    const double g = std::max(0.0, rgbRadiance.y);
    const double b = std::max(0.0, rgbRadiance.z);
    return std::max(0.0, 412.0 * (0.062 * r + 0.608 * g + 0.330 * b));
}

double equivalentLuminanceFromXyzAndScotopic(const Vec3d &xyz, double Ls) {
    static const double kX555 = 0.5120501 / (0.5120501 + 1.0 + 0.005749999);
    static const double kY555 = 1.0 / (0.5120501 + 1.0 + 0.005749999);
    const double X = std::max(0.0, xyz.x);
    const double Lp = std::max(0.0, xyz.y);
    const double Z = std::max(0.0, xyz.z);
    Ls = std::max(0.0, Ls);
    const double xyzSum = X + Lp + Z;
    if (Lp <= 0.0 || Ls <= 0.0 || xyzSum <= 0.0)
        return 0.0;

    const double xin = X / xyzSum;
    const double yin = Lp / xyzSum;
    if (!(yin > 0.0))
        return 0.0;

    const auto chromFa = [](double x, double y) {
        return -0.0054 - 0.21 * x + 0.77 * y + 1.44 * x * x - 2.97 * x * y
             + 1.59 * y * y - 2.11 * (1.0 - x - y) * y * y;
    };
    const auto chromF = [](double a, double y) {
        return std::log(a) / 2.0 - std::log(y);
    };

    const double chromF555 = chromF(chromFa(kX555, kY555), kY555);
    const double chromFaIn = chromFa(xin, yin);
    if (!(chromFaIn > 0.0))
        return 0.0;

    const double acEl = 1.3 * std::sqrt(Lp) / (std::sqrt(Lp) + 2.24);
    const double aEl = Lp / (Lp + 0.05);
    const double cEl = acEl * (chromF(chromFaIn, yin) - chromF555);
    const double eqvLum = std::pow(Lp, aEl) * std::pow(Ls, 1.0 - aEl) * std::exp(cEl);
    if (!std::isfinite(eqvLum) || eqvLum <= 0.0)
        return 0.0;
    return eqvLum;
}

double equivalentLuminanceAt(
    const HdrImage &image,
    size_t x,
    size_t y,
    const EquivalentLuminanceInputInfo &info
) {
    const size_t index = (y * image.width + x) * 3;
    const Vec3d rgbRadiance{
        static_cast<double>(image.rgb[index + 0]) * info.exposureScale,
        static_cast<double>(image.rgb[index + 1]) * info.exposureScale,
        static_cast<double>(image.rgb[index + 2]) * info.exposureScale,
    };

    const Vec3d xyz = equivalentLuminanceXyzAt(image, x, y, info);
    double Ls = 0.0;
    if (info.mode == EquivalentLuminanceInputInfo::Xyz) {
        Ls = approximateScotopicLuminanceFromXyz(xyz);
    } else if (info.mode == EquivalentLuminanceInputInfo::Srgb) {
        const Vec3d xyzRadiometric = mulMat3Vec(info.rgbToXyz, rgbRadiance);
        const Vec3d radianceRgb = mulMat3Vec(info.xyzToRadianceRgb, xyzRadiometric);
        Ls = approximateScotopicLuminanceFromRgb(radianceRgb);
    } else {
        Ls = approximateScotopicLuminanceFromRgb(rgbRadiance);
    }
    return equivalentLuminanceFromXyzAndScotopic(xyz, Ls);
}

std::vector<double> computeEquivalentLuminanceMap(const HdrImage &image) {
    const EquivalentLuminanceInputInfo info = resolveEquivalentLuminanceInputInfo(image);
    if (info.mode == EquivalentLuminanceInputInfo::Unsupported)
        throw std::runtime_error(info.description.empty()
            ? "Eqv_Luminance could not be derived for this HDR input."
            : info.description);

    std::vector<double> values(image.width * image.height, 0.0);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(values.size()); ++i) {
        const size_t idx = static_cast<size_t>(i);
        const size_t y = idx / image.width;
        const size_t x = idx - y * image.width;
        values[idx] = equivalentLuminanceAt(image, x, y, info);
    }
    return values;
}

std::vector<double> buildIntegralImage(const std::vector<double> &values, size_t width, size_t height) {
    std::vector<double> integral((width + 1) * (height + 1), 0.0);
    for (size_t y = 0; y < height; ++y) {
        double rowSum = 0.0;
        for (size_t x = 0; x < width; ++x) {
            rowSum += values[y * width + x];
            integral[(y + 1) * (width + 1) + (x + 1)] = integral[y * (width + 1) + (x + 1)] + rowSum;
        }
    }
    return integral;
}

double integralRectSum(
    const std::vector<double> &integral,
    size_t width,
    size_t x0,
    size_t y0,
    size_t x1,
    size_t y1
) {
    const size_t stride = width + 1;
    return integral[y1 * stride + x1]
         - integral[y0 * stride + x1]
         - integral[y1 * stride + x0]
         + integral[y0 * stride + x0];
}

std::vector<double> computeContrastProxyMap(
    const std::vector<double> &luminance,
    size_t width,
    size_t height,
    double pixelsPerDegree,
    double sensitivityCorrection
) {
    const std::vector<double> integral = buildIntegralImage(luminance, width, height);
    const int radius = std::max(1, std::min(12, static_cast<int>(std::lround(std::max(1.0, pixelsPerDegree) / 24.0))));
    const double sensitivityFloor = std::pow(10.0, sensitivityCorrection - 2.0);
    const double eps = 1e-6;

    std::vector<double> contrast(width * height, 0.0);
    for (size_t y = 0; y < height; ++y) {
        const size_t y0 = static_cast<size_t>(std::max<int>(0, static_cast<int>(y) - radius));
        const size_t y1 = static_cast<size_t>(std::min<int>(static_cast<int>(height), static_cast<int>(y) + radius + 1));
        for (size_t x = 0; x < width; ++x) {
            const size_t x0 = static_cast<size_t>(std::max<int>(0, static_cast<int>(x) - radius));
            const size_t x1 = static_cast<size_t>(std::min<int>(static_cast<int>(width), static_cast<int>(x) + radius + 1));
            const double regionSum = integralRectSum(integral, width, x0, y0, x1, y1);
            const double regionArea = static_cast<double>((x1 - x0) * (y1 - y0));
            const double localMean = std::max(eps, regionSum / std::max(1.0, regionArea));
            const double current = std::max(eps, luminance[y * width + x]);
            double localContrast = std::abs(std::log10(current) - std::log10(localMean));
            localContrast = std::max(0.0, localContrast - sensitivityFloor);
            contrast[y * width + x] = localContrast;
        }
    }
    return contrast;
}

std::vector<double> gaussianKernel1D(double sigma, bool normalize = true) {
    sigma = std::max(1e-6, sigma);
    int kernelSize = static_cast<int>(std::round(sigma * 6.0));
    if (kernelSize < 1)
        kernelSize = 1;
    if ((kernelSize % 2) == 0)
        ++kernelSize;
    const int radius = std::max(0, kernelSize / 2);
    std::vector<double> kernel(static_cast<size_t>(radius * 2 + 1), 0.0);
    const double invSigma2 = 1.0 / (2.0 * sigma * sigma);
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double value = std::exp(-(i * i) * invSigma2);
        kernel[static_cast<size_t>(i + radius)] = value;
        sum += value;
    }
    if (normalize) {
        for (double &value : kernel)
            value /= std::max(sum, 1e-12);
    } else {
        const double center = std::max(kernel[static_cast<size_t>(radius)], 1e-12);
        for (double &value : kernel)
            value /= center;
    }
    return kernel;
}

std::vector<double> gaussianBlurSeparable(
    const std::vector<double> &src,
    size_t width,
    size_t height,
    double sigma
) {
    if (src.empty() || width == 0 || height == 0)
        return {};

    const std::vector<double> kernel = gaussianKernel1D(sigma);
    const int radius = static_cast<int>(kernel.size() / 2);
    std::vector<double> tmp(width * height, 0.0);
    std::vector<double> dst(width * height, 0.0);

    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            double accum = 0.0;
            for (int k = -radius; k <= radius; ++k) {
                const int sampleX = std::max(0, std::min(static_cast<int>(width) - 1, static_cast<int>(x) + k));
                accum += src[y * width + static_cast<size_t>(sampleX)] * kernel[static_cast<size_t>(k + radius)];
            }
            tmp[y * width + x] = accum;
        }
    }

    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            double accum = 0.0;
            for (int k = -radius; k <= radius; ++k) {
                const int sampleY = std::max(0, std::min(static_cast<int>(height) - 1, static_cast<int>(y) + k));
                accum += tmp[static_cast<size_t>(sampleY) * width + x] * kernel[static_cast<size_t>(k + radius)];
            }
            dst[y * width + x] = accum;
        }
    }

    return dst;
}

std::vector<double> gaussianBlurSeparableWithPad(
    const std::vector<double> &src,
    size_t width,
    size_t height,
    double sigma,
    bool replicateEdges,
    double padValue
) {
    if (src.empty() || width == 0 || height == 0)
        return {};

    const std::vector<double> kernel = gaussianKernel1D(sigma);
    const int radius = static_cast<int>(kernel.size() / 2);
    std::vector<double> tmp(width * height, 0.0);
    std::vector<double> dst(width * height, 0.0);

    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            double accum = 0.0;
            for (int k = -radius; k <= radius; ++k) {
                const int sampleX = static_cast<int>(x) + k;
                double sample = padValue;
                if (sampleX >= 0 && sampleX < static_cast<int>(width)) {
                    sample = src[y * width + static_cast<size_t>(sampleX)];
                } else if (replicateEdges) {
                    const int clampedX = std::max(0, std::min(static_cast<int>(width) - 1, sampleX));
                    sample = src[y * width + static_cast<size_t>(clampedX)];
                }
                accum += sample * kernel[static_cast<size_t>(k + radius)];
            }
            tmp[y * width + x] = accum;
        }
    }

    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            double accum = 0.0;
            for (int k = -radius; k <= radius; ++k) {
                const int sampleY = static_cast<int>(y) + k;
                double sample = padValue;
                if (sampleY >= 0 && sampleY < static_cast<int>(height)) {
                    sample = tmp[static_cast<size_t>(sampleY) * width + x];
                } else if (replicateEdges) {
                    const int clampedY = std::max(0, std::min(static_cast<int>(height) - 1, sampleY));
                    sample = tmp[static_cast<size_t>(clampedY) * width + x];
                }
                accum += sample * kernel[static_cast<size_t>(k + radius)];
            }
            dst[y * width + x] = accum;
        }
    }

    return dst;
}

std::vector<double> resizeBilinear(
    const std::vector<double> &src,
    size_t srcWidth,
    size_t srcHeight,
    size_t dstWidth,
    size_t dstHeight
) {
    if (src.empty() || srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0)
        return {};
    if (srcWidth == dstWidth && srcHeight == dstHeight)
        return src;

    std::vector<double> dst(dstWidth * dstHeight, 0.0);
    const double scaleX = static_cast<double>(srcWidth) / static_cast<double>(dstWidth);
    const double scaleY = static_cast<double>(srcHeight) / static_cast<double>(dstHeight);

    for (size_t y = 0; y < dstHeight; ++y) {
        const double srcY = (static_cast<double>(y) + 0.5) * scaleY - 0.5;
        const int y0 = std::max(0, std::min(static_cast<int>(srcHeight) - 1, static_cast<int>(std::floor(srcY))));
        const int y1 = std::max(0, std::min(static_cast<int>(srcHeight) - 1, y0 + 1));
        const double ty = std::max(0.0, std::min(1.0, srcY - static_cast<double>(y0)));
        for (size_t x = 0; x < dstWidth; ++x) {
            const double srcX = (static_cast<double>(x) + 0.5) * scaleX - 0.5;
            const int x0 = std::max(0, std::min(static_cast<int>(srcWidth) - 1, static_cast<int>(std::floor(srcX))));
            const int x1 = std::max(0, std::min(static_cast<int>(srcWidth) - 1, x0 + 1));
            const double tx = std::max(0.0, std::min(1.0, srcX - static_cast<double>(x0)));

            const double v00 = src[static_cast<size_t>(y0) * srcWidth + static_cast<size_t>(x0)];
            const double v10 = src[static_cast<size_t>(y0) * srcWidth + static_cast<size_t>(x1)];
            const double v01 = src[static_cast<size_t>(y1) * srcWidth + static_cast<size_t>(x0)];
            const double v11 = src[static_cast<size_t>(y1) * srcWidth + static_cast<size_t>(x1)];

            const double top = v00 * (1.0 - tx) + v10 * tx;
            const double bottom = v01 * (1.0 - tx) + v11 * tx;
            dst[y * dstWidth + x] = top * (1.0 - ty) + bottom * ty;
        }
    }

    return dst;
}

void padImagePostInto(
    std::vector<double> &dst,
    const std::vector<double> &src,
    size_t width,
    size_t height,
    size_t paddedWidth,
    size_t paddedHeight,
    bool replicateEdges,
    bool symmetricEdges,
    double padValue
) {
    dst.assign(paddedWidth * paddedHeight, padValue);
    auto symmetricIndex = [](int index, int size) {
        if (size <= 1)
            return 0;
        while (index < 0 || index >= size) {
            if (index < 0)
                index = -index - 1;
            else
                index = 2 * size - index - 1;
        }
        return index;
    };
    for (size_t y = 0; y < paddedHeight; ++y) {
        const size_t srcY = y < height ? y : ((replicateEdges && height > 0) ? height - 1 : height);
        for (size_t x = 0; x < paddedWidth; ++x) {
            if (x < width && y < height) {
                dst[y * paddedWidth + x] = src[y * width + x];
            } else if (replicateEdges && srcY < height && width > 0) {
                const size_t srcX = x < width ? x : width - 1;
                dst[y * paddedWidth + x] = src[srcY * width + srcX];
            } else if (symmetricEdges && width > 0 && height > 0) {
                const int sy = symmetricIndex(static_cast<int>(y), static_cast<int>(height));
                const int sx = symmetricIndex(static_cast<int>(x), static_cast<int>(width));
                dst[y * paddedWidth + x] = src[static_cast<size_t>(sy) * width + static_cast<size_t>(sx)];
            }
        }
    }
}

std::vector<double> padImagePost(
    const std::vector<double> &src,
    size_t width,
    size_t height,
    size_t paddedWidth,
    size_t paddedHeight,
    bool replicateEdges,
    bool symmetricEdges,
    double padValue
) {
    std::vector<double> dst;
    padImagePostInto(dst, src, width, height, paddedWidth, paddedHeight, replicateEdges, symmetricEdges, padValue);
    return dst;
}

#if defined(__APPLE__)
struct DftSetupCache {
    std::map<size_t, vDSP_DFT_SetupD> forward;
    std::map<size_t, vDSP_DFT_SetupD> inverse;

    ~DftSetupCache() {
        for (auto &entry : forward)
            vDSP_DFT_DestroySetupD(entry.second);
        for (auto &entry : inverse)
            vDSP_DFT_DestroySetupD(entry.second);
    }

    vDSP_DFT_SetupD get(size_t n, vDSP_DFT_Direction direction) {
        auto &cache = direction == vDSP_DFT_FORWARD ? forward : inverse;
        auto it = cache.find(n);
        if (it != cache.end())
            return it->second;
        vDSP_DFT_SetupD created = vDSP_DFT_zop_CreateSetupD(nullptr, static_cast<vDSP_Length>(n), direction);
        if (!created)
            return nullptr;
        cache.emplace(n, created);
        return created;
    }
};
#endif

std::vector<std::complex<double> > fft2RealEigen(
    const std::vector<double> &src,
    size_t width,
    size_t height
) {
    Eigen::FFT<double> fft;
    std::vector<std::complex<double> > rowFreqs(width * height);
    for (size_t y = 0; y < height; ++y) {
        std::vector<std::complex<double> > rowIn(width);
        std::vector<std::complex<double> > rowOut;
        for (size_t x = 0; x < width; ++x)
            rowIn[x] = std::complex<double>(src[y * width + x], 0.0);
        fft.fwd(rowOut, rowIn);
        for (size_t x = 0; x < width; ++x)
            rowFreqs[y * width + x] = rowOut[x];
    }

    std::vector<std::complex<double> > dst(width * height);
    for (size_t x = 0; x < width; ++x) {
        std::vector<std::complex<double> > colIn(height);
        std::vector<std::complex<double> > colOut;
        for (size_t y = 0; y < height; ++y)
            colIn[y] = rowFreqs[y * width + x];
        fft.fwd(colOut, colIn);
        for (size_t y = 0; y < height; ++y)
            dst[y * width + x] = colOut[y];
    }
    return dst;
}

std::vector<double> ifft2RealEigen(
    const std::vector<std::complex<double> > &src,
    size_t width,
    size_t height
) {
    Eigen::FFT<double> fft;
    std::vector<std::complex<double> > colSpatial(width * height);
    for (size_t x = 0; x < width; ++x) {
        std::vector<std::complex<double> > colIn(height);
        std::vector<std::complex<double> > colOut;
        for (size_t y = 0; y < height; ++y)
            colIn[y] = src[y * width + x];
        fft.inv(colOut, colIn);
        for (size_t y = 0; y < height; ++y)
            colSpatial[y * width + x] = colOut[y];
    }

    std::vector<double> dst(width * height, 0.0);
    for (size_t y = 0; y < height; ++y) {
        std::vector<std::complex<double> > rowIn(width);
        std::vector<std::complex<double> > rowOut;
        for (size_t x = 0; x < width; ++x)
            rowIn[x] = colSpatial[y * width + x];
        fft.inv(rowOut, rowIn);
        for (size_t x = 0; x < width; ++x)
            dst[y * width + x] = rowOut[x].real();
    }
    return dst;
}

#if defined(MERGEHDR_HAS_FFTW)
#ifndef MERGEHDR_ENABLE_OPENMP
#define MERGEHDR_ENABLE_OPENMP 1
#endif

int fftwThreadCount() {
#if defined(_OPENMP)
    return std::max(1, omp_get_max_threads());
#else
    return 1;
#endif
}

void ensureFftwThreadingInitialized() {
    static std::once_flag sInitFlag;
    std::call_once(sInitFlag, []() {
#if MERGEHDR_ENABLE_OPENMP
        if (fftw_init_threads() == 0)
            return;
        fftw_plan_with_nthreads(fftwThreadCount());
#endif
    });
}

struct FftwPlanKey {
    size_t width = 0;
    size_t height = 0;
    int direction = FFTW_FORWARD;

    bool operator<(const FftwPlanKey &other) const {
        return std::tie(width, height, direction) < std::tie(other.width, other.height, other.direction);
    }
};

struct FftwPlanCache {
    struct Entry {
        fftw_plan plan = nullptr;
        fftw_complex *scratchIn = nullptr;
        fftw_complex *scratchOut = nullptr;
    };

    ~FftwPlanCache() {
        for (auto &kv : plans) {
            if (kv.second.plan)
                fftw_destroy_plan(kv.second.plan);
            if (kv.second.scratchIn)
                fftw_free(kv.second.scratchIn);
            if (kv.second.scratchOut)
                fftw_free(kv.second.scratchOut);
        }
    }

    fftw_plan get(size_t width, size_t height, int direction) {
        ensureFftwThreadingInitialized();
        const FftwPlanKey key{width, height, direction};
        auto it = plans.find(key);
        if (it != plans.end())
            return it->second.plan;

        const size_t count = width * height;
        Entry entry;
        entry.scratchIn = static_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * count));
        entry.scratchOut = static_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * count));
        if (!entry.scratchIn || !entry.scratchOut) {
            if (entry.scratchIn)
                fftw_free(entry.scratchIn);
            if (entry.scratchOut)
                fftw_free(entry.scratchOut);
            return nullptr;
        }

        entry.plan = fftw_plan_dft_2d(
            static_cast<int>(height),
            static_cast<int>(width),
            entry.scratchIn,
            entry.scratchOut,
            direction,
            FFTW_ESTIMATE
        );
        // The plan only fixed the size, the out-of-place layout and the fftw_malloc alignment of
        // these arrays; FFTW_ESTIMATE planning does not touch them and every execution passes its
        // own fftw_malloc'd arrays (fftw_execute_dft), so they are not kept with the plan.
        fftw_free(entry.scratchIn);
        fftw_free(entry.scratchOut);
        entry.scratchIn = nullptr;
        entry.scratchOut = nullptr;
        if (!entry.plan)
            return nullptr;

        auto inserted = plans.emplace(key, entry);
        return inserted.first->second.plan;
    }

    std::map<FftwPlanKey, Entry> plans;
};

struct FftwIoBufferCache {
    struct Entry {
        fftw_complex *in = nullptr;
        fftw_complex *out = nullptr;
    };

    ~FftwIoBufferCache() {
        for (auto &kv : buffers) {
            if (kv.second.in)
                fftw_free(kv.second.in);
            if (kv.second.out)
                fftw_free(kv.second.out);
        }
    }

    Entry get(size_t width, size_t height) {
        const std::pair<size_t, size_t> key{width, height};
        auto it = buffers.find(key);
        if (it != buffers.end())
            return it->second;

        const size_t count = width * height;
        Entry entry;
        entry.in = static_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * count));
        entry.out = static_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * count));
        if (!entry.in || !entry.out) {
            if (entry.in)
                fftw_free(entry.in);
            if (entry.out)
                fftw_free(entry.out);
            throw std::bad_alloc();
        }
        auto inserted = buffers.emplace(key, entry);
        return inserted.first->second;
    }

    std::map<std::pair<size_t, size_t>, Entry> buffers;
};

bool preferFftwForSize(size_t width, size_t height) {
#if defined(__APPLE__)
    return width * height >= 512 * 512;
#else
    return true;
#endif
}

// The plans used by fft2RealFftw() (forward) and ifft2RealFftw() (backward). They are shared with
// fftwFilterPaddedReal() so that every transform of a given size runs the same plan.
FftwPlanCache &fftwForwardPlanCache() {
    static FftwPlanCache sPlanCache;
    return sPlanCache;
}

FftwPlanCache &fftwBackwardPlanCache() {
    static FftwPlanCache sPlanCache;
    return sPlanCache;
}

std::vector<std::complex<double> > fft2RealFftw(
    const std::vector<double> &src,
    size_t width,
    size_t height
) {
    const size_t count = width * height;
    static thread_local FftwIoBufferCache sIoBufferCache;
    const FftwIoBufferCache::Entry buffers = sIoBufferCache.get(width, height);
    fftw_complex *in = buffers.in;
    fftw_complex *out = buffers.out;
    for (size_t i = 0; i < count; ++i) {
        in[i][0] = src[i];
        in[i][1] = 0.0;
    }
    fftw_plan plan = fftwForwardPlanCache().get(width, height, FFTW_FORWARD);
    if (!plan)
        return fft2RealEigen(src, width, height);
    fftw_execute_dft(plan, in, out);
    std::vector<std::complex<double> > dst(count);
    for (size_t i = 0; i < count; ++i)
        dst[i] = std::complex<double>(out[i][0], out[i][1]);
    return dst;
}

std::vector<double> ifft2RealFftw(
    const std::vector<std::complex<double> > &src,
    size_t width,
    size_t height
) {
    const size_t count = width * height;
    static thread_local FftwIoBufferCache sIoBufferCache;
    const FftwIoBufferCache::Entry buffers = sIoBufferCache.get(width, height);
    fftw_complex *in = buffers.in;
    fftw_complex *out = buffers.out;
    for (size_t i = 0; i < count; ++i) {
        in[i][0] = src[i].real();
        in[i][1] = src[i].imag();
    }
    fftw_plan plan = fftwBackwardPlanCache().get(width, height, FFTW_BACKWARD);
    if (!plan)
        return ifft2RealEigen(src, width, height);
    fftw_execute_dft(plan, in, out);
    const double invScale = 1.0 / static_cast<double>(count);
    std::vector<double> dst(count, 0.0);
    for (size_t i = 0; i < count; ++i)
        dst[i] = out[i][0] * invScale;
    return dst;
}

// One fftw_malloc'd complex array (the allocator, and so the alignment, of the FFTW I/O arrays).
struct FftwComplexArray {
    fftw_complex *data = nullptr;

    explicit FftwComplexArray(size_t count)
        : data(static_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * count))) {
        if (!data)
            throw std::bad_alloc();
    }
    ~FftwComplexArray() {
        fftw_free(data);
    }
    FftwComplexArray(const FftwComplexArray &) = delete;
    FftwComplexArray &operator=(const FftwComplexArray &) = delete;
};

// padImagePostInto() written straight into an FFTW input array: real parts = the padded image,
// imaginary parts = 0, i.e. exactly what fft2RealFftw() copies from the padded vector.
void padImagePostIntoFftw(
    fftw_complex *dst,
    const std::vector<double> &src,
    size_t width,
    size_t height,
    size_t paddedWidth,
    size_t paddedHeight,
    bool replicateEdges,
    bool symmetricEdges,
    double padValue
) {
    const size_t count = paddedWidth * paddedHeight;
    for (size_t i = 0; i < count; ++i) {
        dst[i][0] = padValue;
        dst[i][1] = 0.0;
    }
    auto symmetricIndex = [](int index, int size) {
        if (size <= 1)
            return 0;
        while (index < 0 || index >= size) {
            if (index < 0)
                index = -index - 1;
            else
                index = 2 * size - index - 1;
        }
        return index;
    };
    for (size_t y = 0; y < paddedHeight; ++y) {
        const size_t srcY = y < height ? y : ((replicateEdges && height > 0) ? height - 1 : height);
        for (size_t x = 0; x < paddedWidth; ++x) {
            if (x < width && y < height) {
                dst[y * paddedWidth + x][0] = src[y * width + x];
            } else if (replicateEdges && srcY < height && width > 0) {
                const size_t srcX = x < width ? x : width - 1;
                dst[y * paddedWidth + x][0] = src[srcY * width + srcX];
            } else if (symmetricEdges && width > 0 && height > 0) {
                const int sy = symmetricIndex(static_cast<int>(y), static_cast<int>(height));
                const int sx = symmetricIndex(static_cast<int>(x), static_cast<int>(width));
                dst[y * paddedWidth + x][0] = src[static_cast<size_t>(sy) * width + static_cast<size_t>(sx)];
            }
        }
    }
}

// FFTW core of fastConvFftLikeMatlab() and of the FFT branch of fastGaussLikeMatlab(), i.e. of
//   crop(ifft2Real(fft2Real(padImagePost(src, 2*width x 2*height)) .* freqFilter)),
// computed with the same FFTW plans, the same input values and the same floating-point operations
// (complex *= double scales both parts; the inverse is the real part times 1/count), but in two
// padded complex arrays that are freed on return, instead of the padded image, the spectrum
// vector, the full-size inverse vector and the four FFTW I/O arrays that fft2RealFftw() and
// ifft2RealFftw() keep cached per size and thread. The spectrum is filtered in place and the
// backward plan runs from the spectrum array into the (no longer needed) input array.
// Returns false, leaving `cropped` alone, whenever fft2Real()/ifft2Real() would not take their
// FFTW path for this size or a plan is unavailable; the caller then runs its original code.
bool fftwFilterPaddedReal(
    const std::vector<double> &src,
    size_t width,
    size_t height,
    const std::vector<double> &freqFilter,
    bool replicateEdges,
    bool symmetricEdges,
    double padValue,
    std::vector<double> &cropped
) {
    const size_t paddedWidth = width * 2;
    const size_t paddedHeight = height * 2;
    if (!preferFftwForSize(paddedWidth, paddedHeight))
        return false;
    const fftw_plan forward = fftwForwardPlanCache().get(paddedWidth, paddedHeight, FFTW_FORWARD);
    if (!forward)
        return false;
    const fftw_plan backward = fftwBackwardPlanCache().get(paddedWidth, paddedHeight, FFTW_BACKWARD);
    if (!backward)
        return false;

    const size_t count = paddedWidth * paddedHeight;
    FftwComplexArray in(count);
    {
        FftwComplexArray out(count);
        padImagePostIntoFftw(in.data, src, width, height, paddedWidth, paddedHeight, replicateEdges, symmetricEdges, padValue);
        fftw_execute_dft(forward, in.data, out.data);
        for (size_t i = 0; i < count; ++i) {
            out.data[i][0] *= freqFilter[i];
            out.data[i][1] *= freqFilter[i];
        }
        fftw_execute_dft(backward, out.data, in.data);
    }
    const double invScale = 1.0 / static_cast<double>(count);
    cropped.assign(width * height, 0.0);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x)
            cropped[y * width + x] = in.data[y * paddedWidth + x][0] * invScale;
    }
    return true;
}
#endif

std::vector<std::complex<double> > fft2Real(
    const std::vector<double> &src,
    size_t width,
    size_t height
) {
#if defined(MERGEHDR_HAS_FFTW)
    if (preferFftwForSize(width, height))
        return fft2RealFftw(src, width, height);
#endif
#if defined(__APPLE__)
    static DftSetupCache sDftCache;
    std::vector<std::complex<double> > rowFreqs(width * height);
    std::vector<std::complex<double> > dst(width * height);
    std::vector<double> inReal(std::max(width, height), 0.0);
    std::vector<double> inImag(std::max(width, height), 0.0);
    std::vector<double> outReal(std::max(width, height), 0.0);
    std::vector<double> outImag(std::max(width, height), 0.0);

    const vDSP_DFT_SetupD rowSetup = sDftCache.get(width, vDSP_DFT_FORWARD);
    const vDSP_DFT_SetupD colSetup = sDftCache.get(height, vDSP_DFT_FORWARD);
    if (!rowSetup || !colSetup) {
#if defined(MERGEHDR_HAS_FFTW)
        return fft2RealFftw(src, width, height);
#else
        return fft2RealEigen(src, width, height);
#endif
    }
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            inReal[x] = src[y * width + x];
            inImag[x] = 0.0;
        }
        vDSP_DFT_ExecuteD(rowSetup, inReal.data(), inImag.data(), outReal.data(), outImag.data());
        for (size_t x = 0; x < width; ++x)
            rowFreqs[y * width + x] = std::complex<double>(outReal[x], outImag[x]);
    }
    for (size_t x = 0; x < width; ++x) {
        for (size_t y = 0; y < height; ++y) {
            const std::complex<double> value = rowFreqs[y * width + x];
            inReal[y] = value.real();
            inImag[y] = value.imag();
        }
        vDSP_DFT_ExecuteD(colSetup, inReal.data(), inImag.data(), outReal.data(), outImag.data());
        for (size_t y = 0; y < height; ++y)
            dst[y * width + x] = std::complex<double>(outReal[y], outImag[y]);
    }
    return dst;
#else
#if defined(MERGEHDR_HAS_FFTW)
    return fft2RealFftw(src, width, height);
#else
    return fft2RealEigen(src, width, height);
#endif
#endif
}

std::vector<double> ifft2Real(
    const std::vector<std::complex<double> > &src,
    size_t width,
    size_t height
) {
#if defined(MERGEHDR_HAS_FFTW)
    if (preferFftwForSize(width, height))
        return ifft2RealFftw(src, width, height);
#endif
#if defined(__APPLE__)
    static DftSetupCache sDftCache;
    std::vector<std::complex<double> > colSpatial(width * height);
    std::vector<double> inReal(std::max(width, height), 0.0);
    std::vector<double> inImag(std::max(width, height), 0.0);
    std::vector<double> outReal(std::max(width, height), 0.0);
    std::vector<double> outImag(std::max(width, height), 0.0);

    const vDSP_DFT_SetupD colSetup = sDftCache.get(height, vDSP_DFT_INVERSE);
    const vDSP_DFT_SetupD rowSetup = sDftCache.get(width, vDSP_DFT_INVERSE);
    if (!colSetup || !rowSetup) {
#if defined(MERGEHDR_HAS_FFTW)
        return ifft2RealFftw(src, width, height);
#else
        return ifft2RealEigen(src, width, height);
#endif
    }
    for (size_t x = 0; x < width; ++x) {
        for (size_t y = 0; y < height; ++y) {
            const std::complex<double> value = src[y * width + x];
            inReal[y] = value.real();
            inImag[y] = value.imag();
        }
        vDSP_DFT_ExecuteD(colSetup, inReal.data(), inImag.data(), outReal.data(), outImag.data());
        for (size_t y = 0; y < height; ++y)
            colSpatial[y * width + x] = std::complex<double>(outReal[y], outImag[y]);
    }
    std::vector<double> dst(width * height, 0.0);
    const double invScale = 1.0 / static_cast<double>(width * height);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const std::complex<double> value = colSpatial[y * width + x];
            inReal[x] = value.real();
            inImag[x] = value.imag();
        }
        vDSP_DFT_ExecuteD(rowSetup, inReal.data(), inImag.data(), outReal.data(), outImag.data());
        for (size_t x = 0; x < width; ++x)
            dst[y * width + x] = outReal[x] * invScale;
    }
    return dst;
#else
#if defined(MERGEHDR_HAS_FFTW)
    return ifft2RealFftw(src, width, height);
#else
    return ifft2RealEigen(src, width, height);
#endif
#endif
}

std::vector<double> fastGaussLikeMatlab(
    const std::vector<double> &src,
    size_t width,
    size_t height,
    double sigma,
    bool doNorm,
    bool symmetricEdges,
    bool replicateEdges,
    double padValue
) {
    if (src.empty() || width == 0 || height == 0)
        return {};

    const auto localFormatCacheDouble = [](double value) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(9) << value;
        return oss.str();
    };
    const std::string sigmaKey = localFormatCacheDouble(sigma);

    if (sigma >= 4.3) {
        const size_t paddedWidth = width * 2;
        const size_t paddedHeight = height * 2;
        static std::map<std::string, std::vector<double> > sKernelFreqCache;
        const std::string kernelKey =
            std::to_string(paddedWidth) + "x" + std::to_string(paddedHeight) +
            "|sigma=" + sigmaKey +
            "|norm=" + (doNorm ? "1" : "0");
        auto kernelIt = sKernelFreqCache.find(kernelKey);
        if (kernelIt == sKernelFreqCache.end()) {
            std::vector<double> kernelFreq(paddedWidth * paddedHeight, 0.0);
            const double twoPi = 2.0 * kPi;
            const double invW = 1.0 / static_cast<double>(paddedWidth);
            const double invH = 1.0 / static_cast<double>(paddedHeight);
            double kernelSum = 0.0;
            for (size_t y = 0; y < paddedHeight; ++y) {
                const double ky0 = std::fmod(0.5 + static_cast<double>(y) * invH, 1.0) - 0.5;
                const double ky1 = ky0 * twoPi;
                for (size_t x = 0; x < paddedWidth; ++x) {
                    const double kx0 = std::fmod(0.5 + static_cast<double>(x) * invW, 1.0) - 0.5;
                    const double kx1 = kx0 * twoPi;
                    const double value = std::exp(-0.5 * (kx1 * kx1 + ky1 * ky1) * sigma * sigma);
                    kernelFreq[y * paddedWidth + x] = value;
                    kernelSum += value;
                }
            }
            if (!doNorm) {
                const double factor = static_cast<double>(kernelFreq.size()) / std::max(kernelSum, 1e-12);
                for (double &value : kernelFreq)
                    value *= factor;
            }
            kernelIt = sKernelFreqCache.emplace(kernelKey, std::move(kernelFreq)).first;
        }
        const std::vector<double> &kernelFreq = kernelIt->second;

#if defined(MERGEHDR_HAS_FFTW)
        {
            std::vector<double> cropped;
            if (fftwFilterPaddedReal(src, width, height, kernelFreq, replicateEdges, symmetricEdges, padValue, cropped))
                return cropped;
        }
#endif
        const std::vector<double> padded = padImagePost(src, width, height, paddedWidth, paddedHeight, replicateEdges, symmetricEdges, padValue);
        std::vector<std::complex<double> > imageFft = fft2Real(padded, paddedWidth, paddedHeight);
        for (size_t i = 0; i < imageFft.size(); ++i)
            imageFft[i] *= kernelFreq[i];
        const std::vector<double> blurredPadded = ifft2Real(imageFft, paddedWidth, paddedHeight);
        std::vector<double> cropped(width * height, 0.0);
        for (size_t y = 0; y < height; ++y) {
            for (size_t x = 0; x < width; ++x)
                cropped[y * width + x] = blurredPadded[y * paddedWidth + x];
        }
        return cropped;
    }

    static std::map<std::string, std::vector<double> > sKernel1DCache;
    const std::string kernel1dKey = sigmaKey + "|norm=" + (doNorm ? std::string("1") : std::string("0"));
    auto kernelIt = sKernel1DCache.find(kernel1dKey);
    if (kernelIt == sKernel1DCache.end())
        kernelIt = sKernel1DCache.emplace(kernel1dKey, gaussianKernel1D(sigma, doNorm)).first;
    const std::vector<double> &kernel = kernelIt->second;
    const int radius = static_cast<int>(kernel.size() / 2);
    std::vector<double> tmp(width * height, 0.0);
    std::vector<double> dst(width * height, 0.0);
    auto symmetricIndex = [](int index, int size) {
        if (size <= 1)
            return 0;
        while (index < 0 || index >= size) {
            if (index < 0)
                index = -index - 1;
            else
                index = 2 * size - index - 1;
        }
        return index;
    };

    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            double accum = 0.0;
            for (int k = -radius; k <= radius; ++k) {
                const int sampleX = static_cast<int>(x) + k;
                double sample = padValue;
                if (sampleX >= 0 && sampleX < static_cast<int>(width)) {
                    sample = src[y * width + static_cast<size_t>(sampleX)];
                } else if (symmetricEdges) {
                    const int reflectedX = symmetricIndex(sampleX, static_cast<int>(width));
                    sample = src[y * width + static_cast<size_t>(reflectedX)];
                } else if (replicateEdges) {
                    const int clampedX = std::max(0, std::min(static_cast<int>(width) - 1, sampleX));
                    sample = src[y * width + static_cast<size_t>(clampedX)];
                }
                accum += sample * kernel[static_cast<size_t>(k + radius)];
            }
            tmp[y * width + x] = accum;
        }
    }

    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            double accum = 0.0;
            for (int k = -radius; k <= radius; ++k) {
                const int sampleY = static_cast<int>(y) + k;
                double sample = padValue;
                if (sampleY >= 0 && sampleY < static_cast<int>(height)) {
                    sample = tmp[static_cast<size_t>(sampleY) * width + x];
                } else if (symmetricEdges) {
                    const int reflectedY = symmetricIndex(sampleY, static_cast<int>(height));
                    sample = tmp[static_cast<size_t>(reflectedY) * width + x];
                } else if (replicateEdges) {
                    const int clampedY = std::max(0, std::min(static_cast<int>(height) - 1, sampleY));
                    sample = tmp[static_cast<size_t>(clampedY) * width + x];
                }
                accum += sample * kernel[static_cast<size_t>(k + radius)];
            }
            dst[y * width + x] = accum;
        }
    }

    return dst;
}

std::vector<double> createCycdegImage(size_t height, size_t width, double pixelsPerDegree) {
    std::vector<double> rho(width * height, 0.0);
    const double nyquist = 0.5 * pixelsPerDegree;
    for (size_t y = 0; y < height; ++y) {
        const double ky0 = std::fmod(0.5 + static_cast<double>(y) / static_cast<double>(height), 1.0) - 0.5;
        const double ky = ky0 * nyquist * 2.0;
        for (size_t x = 0; x < width; ++x) {
            const double kx0 = std::fmod(0.5 + static_cast<double>(x) / static_cast<double>(width), 1.0) - 0.5;
            const double kx = kx0 * nyquist * 2.0;
            rho[y * width + x] = std::sqrt(kx * kx + ky * ky);
        }
    }
    return rho;
}

std::vector<double> fastConvFftLikeMatlab(
    const std::vector<double> &src,
    size_t width,
    size_t height,
    const std::vector<double> &freqFilter,
    bool symmetricEdges,
    bool replicateEdges,
    double padValue
) {
    if (src.empty() || width == 0 || height == 0)
        return {};

#if defined(MERGEHDR_HAS_FFTW)
    {
        std::vector<double> cropped;
        if (fftwFilterPaddedReal(src, width, height, freqFilter, replicateEdges, symmetricEdges, padValue, cropped))
            return cropped;
    }
#endif
    const size_t paddedWidth = width * 2;
    const size_t paddedHeight = height * 2;
    const std::vector<double> padded = padImagePost(
        src, width, height, paddedWidth, paddedHeight, replicateEdges, symmetricEdges, padValue);
    std::vector<std::complex<double> > imageFft = fft2Real(padded, paddedWidth, paddedHeight);
    for (size_t i = 0; i < imageFft.size(); ++i)
        imageFft[i] *= freqFilter[i];
    const std::vector<double> filtered = ifft2Real(imageFft, paddedWidth, paddedHeight);

    std::vector<double> cropped(width * height, 0.0);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x)
            cropped[y * width + x] = filtered[y * paddedWidth + x];
    }
    return cropped;
}

double matlabCubicKernel(double x) {
    constexpr double a = -0.5;
    x = std::abs(x);
    if (x < 1.0)
        return (a + 2.0) * x * x * x - (a + 3.0) * x * x + 1.0;
    if (x < 2.0)
        return a * x * x * x - 5.0 * a * x * x + 8.0 * a * x - 4.0 * a;
    return 0.0;
}

std::vector<std::pair<int, double> > matlabResizeWeights(
    size_t srcSize,
    size_t dstSize,
    size_t dstIndex
) {
    const double scale = static_cast<double>(dstSize) / static_cast<double>(srcSize);
    const double invScale = 1.0 / scale;
    const double kernelScale = scale < 1.0 ? invScale : 1.0;
    const double kernelRadius = 2.0 * kernelScale;
    const double srcPos = (static_cast<double>(dstIndex) + 0.5) * invScale - 0.5;
    const int left = static_cast<int>(std::floor(srcPos - kernelRadius));
    const int right = static_cast<int>(std::ceil(srcPos + kernelRadius));

    std::vector<std::pair<int, double> > weights;
    weights.reserve(static_cast<size_t>(std::max(0, right - left + 1)));
    double sum = 0.0;
    for (int idx = left; idx <= right; ++idx) {
        double distance = srcPos - static_cast<double>(idx);
        if (scale < 1.0)
            distance *= scale;
        double weight = matlabCubicKernel(distance);
        if (scale < 1.0)
            weight *= scale;
        if (weight == 0.0)
            continue;
        const int clamped = std::max(0, std::min(static_cast<int>(srcSize) - 1, idx));
        weights.emplace_back(clamped, weight);
        sum += weight;
    }
    if (std::abs(sum) > 1e-12) {
        for (auto &entry : weights)
            entry.second /= sum;
    }
    return weights;
}

struct ResizeWeightCacheEntry {
    std::vector<std::vector<std::pair<int, double> > > xWeights;
    std::vector<std::vector<std::pair<int, double> > > yWeights;
};

std::vector<double> resizeMatlabBicubic(
    const std::vector<double> &src,
    size_t srcWidth,
    size_t srcHeight,
    size_t dstWidth,
    size_t dstHeight
) {
    if (src.empty() || srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0)
        return {};
    if (srcWidth == dstWidth && srcHeight == dstHeight)
        return src;

    static std::map<std::array<size_t, 4>, ResizeWeightCacheEntry> sWeightCache;
    const std::array<size_t, 4> key{{srcWidth, srcHeight, dstWidth, dstHeight}};
    auto it = sWeightCache.find(key);
    if (it == sWeightCache.end()) {
        ResizeWeightCacheEntry entry;
        entry.xWeights.resize(dstWidth);
        entry.yWeights.resize(dstHeight);
        for (size_t x = 0; x < dstWidth; ++x)
            entry.xWeights[x] = matlabResizeWeights(srcWidth, dstWidth, x);
        for (size_t y = 0; y < dstHeight; ++y)
            entry.yWeights[y] = matlabResizeWeights(srcHeight, dstHeight, y);
        it = sWeightCache.emplace(key, std::move(entry)).first;
    }
    const auto &xWeights = it->second.xWeights;
    const auto &yWeights = it->second.yWeights;

    std::vector<double> tmp(dstWidth * srcHeight, 0.0);
    for (size_t y = 0; y < srcHeight; ++y) {
        for (size_t x = 0; x < dstWidth; ++x) {
            double sum = 0.0;
            for (const auto &entry : xWeights[x])
                sum += src[y * srcWidth + static_cast<size_t>(entry.first)] * entry.second;
            tmp[y * dstWidth + x] = sum;
        }
    }

    std::vector<double> dst(dstWidth * dstHeight, 0.0);
    for (size_t y = 0; y < dstHeight; ++y) {
        for (size_t x = 0; x < dstWidth; ++x) {
            double sum = 0.0;
            for (const auto &entry : yWeights[y])
                sum += tmp[static_cast<size_t>(entry.first) * dstWidth + x] * entry.second;
            dst[y * dstWidth + x] = sum;
        }
    }
    return dst;
}

struct NativeHdrvdpParams;
double hdrvdpMtf(double rho, const NativeHdrvdpParams &params);

double dominantFrequencyCpdFromBand(
    const std::vector<double> &band,
    size_t width,
    size_t height,
    double pixelsPerDegree
) {
    if (band.empty() || width == 0 || height == 0)
        return 0.0;

    const std::vector<std::complex<double> > freq = fft2Real(band, width, height);
    double bestMagnitude = -1.0;
    size_t bestX = 0;
    size_t bestY = 0;
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            if (x == 0 && y == 0)
                continue;
            const double magnitude = std::norm(freq[y * width + x]);
            if (magnitude > bestMagnitude) {
                bestMagnitude = magnitude;
                bestX = x;
                bestY = y;
            }
        }
    }

    const double kx = (std::fmod(0.5 + static_cast<double>(bestX) / static_cast<double>(width), 1.0) - 0.5) * pixelsPerDegree;
    const double ky = (std::fmod(0.5 + static_cast<double>(bestY) / static_cast<double>(height), 1.0) - 0.5) * pixelsPerDegree;
    return std::sqrt(kx * kx + ky * ky);
}

std::vector<double> downsample2Average(
    const std::vector<double> &src,
    size_t srcWidth,
    size_t srcHeight,
    size_t &dstWidth,
    size_t &dstHeight
) {
    dstWidth = std::max<size_t>(1, srcWidth / 2);
    dstHeight = std::max<size_t>(1, srcHeight / 2);
    std::vector<double> dst(dstWidth * dstHeight, 0.0);
    for (size_t y = 0; y < dstHeight; ++y) {
        for (size_t x = 0; x < dstWidth; ++x) {
            const size_t sx = x * 2;
            const size_t sy = y * 2;
            double sum = 0.0;
            double count = 0.0;
            for (size_t oy = 0; oy < 2 && sy + oy < srcHeight; ++oy) {
                for (size_t ox = 0; ox < 2 && sx + ox < srcWidth; ++ox) {
                    sum += src[(sy + oy) * srcWidth + (sx + ox)];
                    count += 1.0;
                }
            }
            dst[y * dstWidth + x] = sum / std::max(count, 1.0);
        }
    }
    return dst;
}

constexpr int kPyrtoolsReduce = 0;
constexpr int kPyrtoolsExpand = 1;

std::vector<double> rowMajorToMatlabColumnMajor(
    const std::vector<double> &src,
    size_t width,
    size_t height
) {
    std::vector<double> dst(width * height, 0.0);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x)
            dst[y + x * height] = src[y * width + x];
    }
    return dst;
}

std::vector<double> matlabColumnMajorToRowMajor(
    const std::vector<double> &src,
    size_t width,
    size_t height
) {
    std::vector<double> dst(width * height, 0.0);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x)
            dst[y * width + x] = src[y + x * height];
    }
    return dst;
}

int pyrtoolsAbs(int value) {
    return value >= 0 ? value : -value;
}

void pyrtoolsReflect1Filter(
    const double *filt,
    int xDim,
    int yDim,
    int xPos,
    int yPos,
    double *result,
    int reduceOrExpand
) {
    const int filtSize = xDim * yDim;
    const int xBase = xPos > 0 ? (xDim - 1) : 0;
    const int yBase = xDim * (yPos > 0 ? (yDim - 1) : 0);
    int xOverhang = xPos > 0 ? (xPos - 1) : (xPos < 0 ? (xPos + 1) : 0);
    int yOverhang = xDim * (yPos > 0 ? (yPos - 1) : (yPos < 0 ? (yPos + 1) : 0));
    const int mxPos = xPos < 0 ? (xDim / 2) : ((xDim - 1) / 2);
    const int myPos = xDim * (yPos < 0 ? (yDim / 2) : ((yDim - 1) / 2));

    std::fill(result, result + static_cast<size_t>(filtSize), 0.0);

    if (reduceOrExpand == kPyrtoolsReduce) {
        for (int yFilt = 0, yRes = yOverhang - yBase;
             yFilt < filtSize;
             yFilt += xDim, yRes += xDim) {
            for (int xFilt = yFilt, xRes = xOverhang - xBase;
                 xFilt < yFilt + xDim;
                 ++xFilt, ++xRes) {
                result[pyrtoolsAbs(yBase - pyrtoolsAbs(yRes)) + pyrtoolsAbs(xBase - pyrtoolsAbs(xRes))] += filt[xFilt];
            }
        }
        return;
    }

    yOverhang = pyrtoolsAbs(yOverhang);
    xOverhang = pyrtoolsAbs(xOverhang);

    for (int yRes = yBase, yFilt = yBase - yOverhang;
         yFilt > yBase - filtSize;
         yFilt -= xDim, yRes -= xDim) {
        for (int xRes = xBase, xFilt = xBase - xOverhang;
             xFilt > xBase - xDim;
             --xRes, --xFilt) {
            result[pyrtoolsAbs(yRes) + pyrtoolsAbs(xRes)] += filt[pyrtoolsAbs(yFilt) + pyrtoolsAbs(xFilt)];
        }
        if ((xOverhang != mxPos) && (xPos != 0)) {
            for (int xRes = xBase, xFilt = xBase - 2 * mxPos + xOverhang;
                 xFilt > xBase - xDim;
                 --xRes, --xFilt) {
                result[pyrtoolsAbs(yRes) + pyrtoolsAbs(xRes)] += filt[pyrtoolsAbs(yFilt) + pyrtoolsAbs(xFilt)];
            }
        }
    }

    if ((yOverhang != myPos) && (yPos != 0)) {
        for (int yRes = yBase, yFilt = yBase - 2 * myPos + yOverhang;
             yFilt > yBase - filtSize;
             yFilt -= xDim, yRes -= xDim) {
            for (int xRes = xBase, xFilt = xBase - xOverhang;
                 xFilt > xBase - xDim;
                 --xRes, --xFilt) {
                result[pyrtoolsAbs(yRes) + pyrtoolsAbs(xRes)] += filt[pyrtoolsAbs(yFilt) + pyrtoolsAbs(xFilt)];
            }
            if ((xOverhang != mxPos) && (xPos != 0)) {
                for (int xRes = xBase, xFilt = xBase - 2 * mxPos + xOverhang;
                     xFilt > xBase - xDim;
                     --xRes, --xFilt) {
                    result[pyrtoolsAbs(yRes) + pyrtoolsAbs(xRes)] += filt[pyrtoolsAbs(yFilt) + pyrtoolsAbs(xFilt)];
                }
            }
        }
    }
}

void pyrtoolsInternalReduceReflect1(
    const double *image,
    int xDim,
    int yDim,
    const double *filt,
    double *temp,
    int xFdim,
    int yFdim,
    int xStart,
    int xStep,
    int xStop,
    int yStart,
    int yStep,
    int yStop,
    double *result
) {
    const int filtSize = xFdim * yFdim;
    const int yCtrStopInitial = yDim - ((yFdim == 1) ? 0 : yFdim);
    const int xCtrStopInitial = xDim - ((xFdim == 1) ? 0 : xFdim);
    const int xResDim = (xStop - xStart + xStep - 1) / xStep;
    int xCtrStart = (xFdim == 1) ? 0 : 1;
    int yCtrStart = (yFdim == 1) ? 0 : 1;
    const int xFmid = xFdim / 2;
    const int yFmid = yFdim / 2;
    int yCtrStop = yCtrStopInitial;
    int xCtrStop = xCtrStopInitial;

    xStart -= xFmid;
    yStart -= yFmid;
    xStop -= xFmid;
    yStop -= yFmid;

    if (xStop < xCtrStop)
        xCtrStop = xStop;
    if (yStop < yCtrStop)
        yCtrStop = yStop;

    auto innerProd = [&](int xCorner, int yCorner, int resPos) {
        double sum = 0.0;
        for (int imPos = yCorner * xDim + xCorner, filtPos = 0, xFiltStop = xFdim;
             xFiltStop <= filtSize;
             imPos += (xDim - xFdim), xFiltStop += xFdim) {
            for (; filtPos < xFiltStop; ++filtPos, ++imPos)
                sum += image[imPos] * temp[filtPos];
        }
        result[resPos] = sum;
    };

    int resPos = 0;
    int yPos = yStart;
    for (; yPos < yCtrStart; yPos += yStep) {
        int xPos = xStart;
        for (; xPos < xCtrStart; xPos += xStep, ++resPos) {
            pyrtoolsReflect1Filter(filt, xFdim, yFdim, xPos - 1, yPos - 1, temp, kPyrtoolsReduce);
            innerProd(0, 0, resPos);
        }

        pyrtoolsReflect1Filter(filt, xFdim, yFdim, 0, yPos - 1, temp, kPyrtoolsReduce);
        for (; xPos < xCtrStop; xPos += xStep, ++resPos)
            innerProd(xPos, 0, resPos);

        for (; xPos < xStop; xPos += xStep, ++resPos) {
            pyrtoolsReflect1Filter(filt, xFdim, yFdim, xPos - xCtrStop + 1, yPos - 1, temp, kPyrtoolsReduce);
            innerProd(xCtrStop, 0, resPos);
        }
    }

    yCtrStart = yPos;
    int baseResPos = resPos;
    int xPos = xStart;
    for (; xPos < xCtrStart; xPos += xStep, ++baseResPos) {
        pyrtoolsReflect1Filter(filt, xFdim, yFdim, xPos - 1, 0, temp, kPyrtoolsReduce);
        for (yPos = yCtrStart, resPos = baseResPos; yPos < yCtrStop; yPos += yStep, resPos += xResDim)
            innerProd(0, yPos, resPos);
    }

    pyrtoolsReflect1Filter(filt, xFdim, yFdim, 0, 0, temp, kPyrtoolsReduce);
    for (; xPos < xCtrStop; xPos += xStep, ++baseResPos) {
        for (yPos = yCtrStart, resPos = baseResPos; yPos < yCtrStop; yPos += yStep, resPos += xResDim)
            innerProd(xPos, yPos, resPos);
    }

    for (; xPos < xStop; xPos += xStep, ++baseResPos) {
        pyrtoolsReflect1Filter(filt, xFdim, yFdim, xPos - xCtrStop + 1, 0, temp, kPyrtoolsReduce);
        for (yPos = yCtrStart, resPos = baseResPos; yPos < yCtrStop; yPos += yStep, resPos += xResDim)
            innerProd(xCtrStop, yPos, resPos);
    }

    resPos -= (xResDim - 1);
    for (; yPos < yStop; yPos += yStep) {
        xPos = xStart;
        for (; xPos < xCtrStart; xPos += xStep, ++resPos) {
            pyrtoolsReflect1Filter(filt, xFdim, yFdim, xPos - 1, yPos - yCtrStop + 1, temp, kPyrtoolsReduce);
            innerProd(0, yCtrStop, resPos);
        }

        pyrtoolsReflect1Filter(filt, xFdim, yFdim, 0, yPos - yCtrStop + 1, temp, kPyrtoolsReduce);
        for (; xPos < xCtrStop; xPos += xStep, ++resPos)
            innerProd(xPos, yCtrStop, resPos);

        for (; xPos < xStop; xPos += xStep, ++resPos) {
            pyrtoolsReflect1Filter(filt, xFdim, yFdim, xPos - xCtrStop + 1, yPos - yCtrStop + 1, temp, kPyrtoolsReduce);
            innerProd(xCtrStop, yCtrStop, resPos);
        }
    }
}

void pyrtoolsInternalExpandReflect1(
    const double *image,
    const double *filt,
    double *temp,
    int xFdim,
    int yFdim,
    int xStart,
    int xStep,
    int xStop,
    int yStart,
    int yStep,
    int yStop,
    double *result,
    int xDim,
    int yDim
) {
    const int filtSize = xFdim * yFdim;
    const int xCtrStart = (xFdim == 1) ? 0 : 1;
    const int yCtrStart = (yFdim == 1) ? 0 : 1;
    int xCtrStop = xDim - ((xFdim == 1) ? 0 : xFdim);
    int yCtrStop = yDim - ((yFdim == 1) ? 0 : yFdim);
    const int xFmid = xFdim / 2;
    const int yFmid = yFdim / 2;
    const int xImDim = (xStop - xStart + xStep - 1) / xStep;

    xStart -= xFmid;
    yStart -= yFmid;
    xStop -= xFmid;
    yStop -= yFmid;

    if (xStop < xCtrStop)
        xCtrStop = xStop;
    if (yStop < yCtrStop)
        yCtrStop = yStop;

    auto innerProd = [&](double value, int xCorner, int yCorner) {
        for (int resPos = yCorner * xDim + xCorner, filtPos = 0, xFiltStop = xFdim;
             xFiltStop <= filtSize;
             resPos += (xDim - xFdim), xFiltStop += xFdim) {
            for (; filtPos < xFiltStop; ++filtPos, ++resPos)
                result[resPos] += value * temp[filtPos];
        }
    };

    int imPos = 0;
    int yPos = yStart;
    for (; yPos < yCtrStart; yPos += yStep) {
        int xPos = xStart;
        for (; xPos < xCtrStart; xPos += xStep, ++imPos) {
            pyrtoolsReflect1Filter(filt, xFdim, yFdim, xPos - 1, yPos - 1, temp, kPyrtoolsExpand);
            innerProd(image[imPos], 0, 0);
        }

        pyrtoolsReflect1Filter(filt, xFdim, yFdim, 0, yPos - 1, temp, kPyrtoolsExpand);
        for (; xPos < xCtrStop; xPos += xStep, ++imPos)
            innerProd(image[imPos], xPos, 0);

        for (; xPos < xStop; xPos += xStep, ++imPos) {
            pyrtoolsReflect1Filter(filt, xFdim, yFdim, xPos - xCtrStop + 1, yPos - 1, temp, kPyrtoolsExpand);
            innerProd(image[imPos], xCtrStop, 0);
        }
    }

    const int yCtrStartHeld = yPos;
    int baseImPos = imPos;
    int xPos = xStart;
    for (; xPos < xCtrStart; xPos += xStep, ++baseImPos) {
        pyrtoolsReflect1Filter(filt, xFdim, yFdim, xPos - 1, 0, temp, kPyrtoolsExpand);
        for (yPos = yCtrStartHeld, imPos = baseImPos; yPos < yCtrStop; yPos += yStep, imPos += xImDim)
            innerProd(image[imPos], 0, yPos);
    }

    pyrtoolsReflect1Filter(filt, xFdim, yFdim, 0, 0, temp, kPyrtoolsExpand);
    for (; xPos < xCtrStop; xPos += xStep, ++baseImPos) {
        for (yPos = yCtrStartHeld, imPos = baseImPos; yPos < yCtrStop; yPos += yStep, imPos += xImDim)
            innerProd(image[imPos], xPos, yPos);
    }

    for (; xPos < xStop; xPos += xStep, ++baseImPos) {
        pyrtoolsReflect1Filter(filt, xFdim, yFdim, xPos - xCtrStop + 1, 0, temp, kPyrtoolsExpand);
        for (yPos = yCtrStartHeld, imPos = baseImPos; yPos < yCtrStop; yPos += yStep, imPos += xImDim)
            innerProd(image[imPos], xCtrStop, yPos);
    }

    imPos -= (xImDim - 1);
    for (; yPos < yStop; yPos += yStep) {
        xPos = xStart;
        for (; xPos < xCtrStart; xPos += xStep, ++imPos) {
            pyrtoolsReflect1Filter(filt, xFdim, yFdim, xPos - 1, yPos - yCtrStop + 1, temp, kPyrtoolsExpand);
            innerProd(image[imPos], 0, yCtrStop);
        }

        pyrtoolsReflect1Filter(filt, xFdim, yFdim, 0, yPos - yCtrStop + 1, temp, kPyrtoolsExpand);
        for (; xPos < xCtrStop; xPos += xStep, ++imPos)
            innerProd(image[imPos], xPos, yCtrStop);

        for (; xPos < xStop; xPos += xStep, ++imPos) {
            pyrtoolsReflect1Filter(filt, xFdim, yFdim, xPos - xCtrStop + 1, yPos - yCtrStop + 1, temp, kPyrtoolsExpand);
            innerProd(image[imPos], xCtrStop, yCtrStop);
        }
    }
}

std::vector<double> correlateReflect1(
    const std::vector<double> &src,
    size_t srcWidth,
    size_t srcHeight,
    const std::vector<double> &filt,
    size_t filtWidth,
    size_t filtHeight,
    size_t stepY = 1,
    size_t stepX = 1,
    size_t startY = 0,
    size_t startX = 0
) {
    if (src.empty() || srcWidth == 0 || srcHeight == 0 || filt.empty())
        return {};

    const size_t dstHeight = (srcHeight <= startY) ? 0 : ((srcHeight - startY + stepY - 1) / stepY);
    const size_t dstWidth = (srcWidth <= startX) ? 0 : ((srcWidth - startX + stepX - 1) / stepX);
    const std::vector<double> imageCm = rowMajorToMatlabColumnMajor(src, srcWidth, srcHeight);
    const std::vector<double> filterCm = rowMajorToMatlabColumnMajor(filt, filtWidth, filtHeight);
    std::vector<double> resultCm(dstWidth * dstHeight, 0.0);
    std::vector<double> temp(filtWidth * filtHeight, 0.0);
    char edges[] = "reflect1";
    internal_reduce(
        const_cast<double*>(imageCm.data()),
        static_cast<int>(srcHeight),
        static_cast<int>(srcWidth),
        const_cast<double*>(filterCm.data()),
        temp.data(),
        static_cast<int>(filtHeight),
        static_cast<int>(filtWidth),
        static_cast<int>(startY),
        static_cast<int>(stepY),
        static_cast<int>(srcHeight),
        static_cast<int>(startX),
        static_cast<int>(stepX),
        static_cast<int>(srcWidth),
        resultCm.data(),
        edges
    );

    return matlabColumnMajorToRowMajor(resultCm, dstWidth, dstHeight);
}

std::vector<double> upsampleAndCorrelateReflect1(
    const std::vector<double> &src,
    size_t srcWidth,
    size_t srcHeight,
    const std::vector<double> &filt,
    size_t filtWidth,
    size_t filtHeight,
    size_t outWidth,
    size_t outHeight,
    size_t stepY = 1,
    size_t stepX = 1,
    size_t startY = 0,
    size_t startX = 0,
    const std::vector<double> *add = nullptr
) {
    const std::vector<double> imageCm = rowMajorToMatlabColumnMajor(src, srcWidth, srcHeight);
    const std::vector<double> filterCm = rowMajorToMatlabColumnMajor(filt, filtWidth, filtHeight);
    std::vector<double> resultCm(outWidth * outHeight, 0.0);
    if (add && add->size() == resultCm.size())
        resultCm = rowMajorToMatlabColumnMajor(*add, outWidth, outHeight);
    std::vector<double> temp(filtWidth * filtHeight, 0.0);
    char edges[] = "reflect1";
    internal_expand(
        const_cast<double*>(imageCm.data()),
        const_cast<double*>(filterCm.data()),
        temp.data(),
        static_cast<int>(filtHeight),
        static_cast<int>(filtWidth),
        static_cast<int>(startY),
        static_cast<int>(stepY),
        static_cast<int>(outHeight),
        static_cast<int>(startX),
        static_cast<int>(stepX),
        static_cast<int>(outWidth),
        resultCm.data(),
        static_cast<int>(outHeight),
        static_cast<int>(outWidth),
        edges
    );

    return matlabColumnMajorToRowMajor(resultCm, outWidth, outHeight);
}

int maxPyrHeight(size_t imageHeight, size_t imageWidth, size_t filterHeight, size_t filterWidth) {
    if (imageHeight < filterHeight || imageWidth < filterWidth)
        return 0;
    return 1 + maxPyrHeight(imageHeight / 2, imageWidth / 2, filterHeight, filterWidth);
}

#include "sp3_filters.inc"

struct ExactVisibilityPyramidLevel {
    size_t width = 0;
    size_t height = 0;
    double frequencyCpd = 0.0;
    std::vector<std::vector<double> > bands;
};

struct ExactVisibilityPyramid {
    std::vector<ExactVisibilityPyramidLevel> levels;
    size_t baseWidth = 0;
    size_t baseHeight = 0;
};

const std::vector<double> &sp3Lo0Filter() {
    return kLo0filt;
}

const std::vector<double> &sp3Hi0Filter() {
    return kHi0filt;
}

const std::vector<double> &sp3LoFilter() {
    return kLofilt;
}

const std::array<std::vector<double>, 4> &sp3BandFilters() {
    static const std::array<std::vector<double>, 4> kFilters = [] {
        std::array<std::vector<double>, 4> filters;
        for (size_t orientation = 0; orientation < filters.size(); ++orientation) {
            const size_t offset = orientation * 81;
            filters[orientation].assign(kBfilts.begin() + static_cast<std::ptrdiff_t>(offset),
                kBfilts.begin() + static_cast<std::ptrdiff_t>(offset + 81));
        }
        return filters;
    }();
    return kFilters;
}

void removeBaseBandDc(ExactVisibilityPyramid &pyramid) {
    if (pyramid.levels.empty() || pyramid.levels.back().bands.empty() || pyramid.levels.back().bands.front().empty())
        return;
    std::vector<double> &baseBand = pyramid.levels.back().bands.front();
    double meanValue = 0.0;
    for (double value : baseBand)
        meanValue += value;
    meanValue /= static_cast<double>(baseBand.size());
    for (double &value : baseBand)
        value -= meanValue;
}

ExactVisibilityPyramid buildExactVisibilityPyramid(
    const std::vector<double> &signal,
    size_t width,
    size_t height,
    double pixelsPerDegree
) {
    ExactVisibilityPyramid pyramid;
    if (signal.empty() || width == 0 || height == 0)
        return pyramid;

    const auto &lo0filt = sp3Lo0Filter();
    const auto &hi0filt = sp3Hi0Filter();
    const auto &lofilt = sp3LoFilter();
    const auto &bfilts = sp3BandFilters();

    const int maxHeight = maxPyrHeight(height, width, 17, 17);
    const int pyrHeight = std::max(0, std::min(
        static_cast<int>(std::ceil(std::log2(std::max(1.0, pixelsPerDegree)))) - 2,
        maxHeight));

    ExactVisibilityPyramidLevel hi0;
    hi0.width = width;
    hi0.height = height;
    hi0.frequencyCpd = pixelsPerDegree / 2.0;
    hi0.bands.push_back(correlateReflect1(signal, width, height, hi0filt, 15, 15));
    pyramid.levels.push_back(std::move(hi0));

    std::vector<double> lowpass = correlateReflect1(signal, width, height, lo0filt, 9, 9);
    size_t currentWidth = width;
    size_t currentHeight = height;
    double currentFreq = pixelsPerDegree / 4.0;

    for (int level = 0; level < pyrHeight; ++level) {
        ExactVisibilityPyramidLevel orientedLevel;
        orientedLevel.width = currentWidth;
        orientedLevel.height = currentHeight;
        orientedLevel.frequencyCpd = currentFreq;
        orientedLevel.bands.reserve(bfilts.size());
        for (const std::vector<double> &bandFilter : bfilts)
            orientedLevel.bands.push_back(correlateReflect1(lowpass, currentWidth, currentHeight, bandFilter, 9, 9));
        pyramid.levels.push_back(std::move(orientedLevel));

        lowpass = correlateReflect1(lowpass, currentWidth, currentHeight, lofilt, 17, 17, 2, 2, 0, 0);
        currentWidth = (currentWidth + 1) / 2;
        currentHeight = (currentHeight + 1) / 2;
        currentFreq *= 0.5;
    }

    ExactVisibilityPyramidLevel baseLevel;
    baseLevel.width = currentWidth;
    baseLevel.height = currentHeight;
    baseLevel.frequencyCpd = currentFreq;
    baseLevel.bands.push_back(std::move(lowpass));
    pyramid.levels.push_back(std::move(baseLevel));
    pyramid.baseWidth = currentWidth;
    pyramid.baseHeight = currentHeight;
    return pyramid;
}

std::vector<double> reconstructExactVisibilityPyramidResponse(
    const ExactVisibilityPyramid &pyramid,
    size_t outputWidth,
    size_t outputHeight
) {
    if (pyramid.levels.empty() || pyramid.levels.back().bands.empty())
        return std::vector<double>(outputWidth * outputHeight, 0.0);

    const auto &lo0filt = sp3Lo0Filter();
    const auto &hi0filt = sp3Hi0Filter();
    const auto &lofilt = sp3LoFilter();
    const auto &bfilts = sp3BandFilters();

    std::vector<double> current = pyramid.levels.back().bands.front();
    size_t currentWidth = pyramid.levels.back().width;
    size_t currentHeight = pyramid.levels.back().height;

    for (size_t levelIndex = pyramid.levels.size() - 1; levelIndex-- > 1;) {
        const ExactVisibilityPyramidLevel &level = pyramid.levels[levelIndex];
        current = upsampleAndCorrelateReflect1(
            current, currentWidth, currentHeight,
            lofilt, 17, 17,
            level.width, level.height,
            2, 2, 0, 0
        );
        for (size_t orientation = 0; orientation < level.bands.size() && orientation < bfilts.size(); ++orientation) {
            current = upsampleAndCorrelateReflect1(
                level.bands[orientation], level.width, level.height,
                bfilts[orientation], 9, 9,
                level.width, level.height,
                1, 1, 0, 0,
                &current
            );
        }
        currentWidth = level.width;
        currentHeight = level.height;
    }

    std::vector<double> reconstructed = upsampleAndCorrelateReflect1(
        current, currentWidth, currentHeight,
        lo0filt, 9, 9,
        outputWidth, outputHeight
    );
    reconstructed = upsampleAndCorrelateReflect1(
        pyramid.levels.front().bands.front(), pyramid.levels.front().width, pyramid.levels.front().height,
        hi0filt, 15, 15,
        outputWidth, outputHeight,
        1, 1, 0, 0,
        &reconstructed
    );
    return reconstructed;
}

const std::vector<double> &sp0Lo0Filter() {
    static const std::vector<double> kFilter{
        -4.514000e-04, -1.137100e-04, -3.725800e-04, -3.743860e-03, -3.725800e-04, -1.137100e-04, -4.514000e-04,
        -1.137100e-04, -6.119520e-03, -1.344160e-02, -7.563200e-03, -1.344160e-02, -6.119520e-03, -1.137100e-04,
        -3.725800e-04, -1.344160e-02,  6.441488e-02,  1.524935e-01,  6.441488e-02, -1.344160e-02, -3.725800e-04,
        -3.743860e-03, -7.563200e-03,  1.524935e-01,  3.153017e-01,  1.524935e-01, -7.563200e-03, -3.743860e-03,
        -3.725800e-04, -1.344160e-02,  6.441488e-02,  1.524935e-01,  6.441488e-02, -1.344160e-02, -3.725800e-04,
        -1.137100e-04, -6.119520e-03, -1.344160e-02, -7.563200e-03, -1.344160e-02, -6.119520e-03, -1.137100e-04,
        -4.514000e-04, -1.137100e-04, -3.725800e-04, -3.743860e-03, -3.725800e-04, -1.137100e-04, -4.514000e-04,
    };
    return kFilter;
}

const std::vector<double> &sp0LoFilter() {
    static const std::vector<double> kFilter{
        -2.257000e-04, -8.064400e-04, -5.686000e-05,  8.741400e-04, -1.862800e-04, -1.031640e-03, -1.871920e-03, -1.031640e-03, -1.862800e-04,  8.741400e-04, -5.686000e-05, -8.064400e-04, -2.257000e-04,
        -8.064400e-04,  1.417620e-03, -1.903800e-04, -2.449060e-03, -4.596420e-03, -7.006740e-03, -6.948900e-03, -7.006740e-03, -4.596420e-03, -2.449060e-03, -1.903800e-04,  1.417620e-03, -8.064400e-04,
        -5.686000e-05, -1.903800e-04, -3.059760e-03, -6.401000e-03, -6.720800e-03, -5.236180e-03, -3.781600e-03, -5.236180e-03, -6.720800e-03, -6.401000e-03, -3.059760e-03, -1.903800e-04, -5.686000e-05,
         8.741400e-04, -2.449060e-03, -6.401000e-03, -5.260020e-03,  3.938620e-03,  1.722078e-02,  2.449600e-02,  1.722078e-02,  3.938620e-03, -5.260020e-03, -6.401000e-03, -2.449060e-03,  8.741400e-04,
        -1.862800e-04, -4.596420e-03, -6.720800e-03,  3.938620e-03,  3.220744e-02,  6.306262e-02,  7.624674e-02,  6.306262e-02,  3.220744e-02,  3.938620e-03, -6.720800e-03, -4.596420e-03, -1.862800e-04,
        -1.031640e-03, -7.006740e-03, -5.236180e-03,  1.722078e-02,  6.306262e-02,  1.116388e-01,  1.348999e-01,  1.116388e-01,  6.306262e-02,  1.722078e-02, -5.236180e-03, -7.006740e-03, -1.031640e-03,
        -1.871920e-03, -6.948900e-03, -3.781600e-03,  2.449600e-02,  7.624674e-02,  1.348999e-01,  1.576508e-01,  1.348999e-01,  7.624674e-02,  2.449600e-02, -3.781600e-03, -6.948900e-03, -1.871920e-03,
        -1.031640e-03, -7.006740e-03, -5.236180e-03,  1.722078e-02,  6.306262e-02,  1.116388e-01,  1.348999e-01,  1.116388e-01,  6.306262e-02,  1.722078e-02, -5.236180e-03, -7.006740e-03, -1.031640e-03,
        -1.862800e-04, -4.596420e-03, -6.720800e-03,  3.938620e-03,  3.220744e-02,  6.306262e-02,  7.624674e-02,  6.306262e-02,  3.220744e-02,  3.938620e-03, -6.720800e-03, -4.596420e-03, -1.862800e-04,
         8.741400e-04, -2.449060e-03, -6.401000e-03, -5.260020e-03,  3.938620e-03,  1.722078e-02,  2.449600e-02,  1.722078e-02,  3.938620e-03, -5.260020e-03, -6.401000e-03, -2.449060e-03,  8.741400e-04,
        -5.686000e-05, -1.903800e-04, -3.059760e-03, -6.401000e-03, -6.720800e-03, -5.236180e-03, -3.781600e-03, -5.236180e-03, -6.720800e-03, -6.401000e-03, -3.059760e-03, -1.903800e-04, -5.686000e-05,
        -8.064400e-04,  1.417620e-03, -1.903800e-04, -2.449060e-03, -4.596420e-03, -7.006740e-03, -6.948900e-03, -7.006740e-03, -4.596420e-03, -2.449060e-03, -1.903800e-04,  1.417620e-03, -8.064400e-04,
        -2.257000e-04, -8.064400e-04, -5.686000e-05,  8.741400e-04, -1.862800e-04, -1.031640e-03, -1.871920e-03, -1.031640e-03, -1.862800e-04,  8.741400e-04, -5.686000e-05, -8.064400e-04, -2.257000e-04,
    };
    return kFilter;
}

const std::vector<double> &sp0Hi0Filter() {
    static const std::vector<double> kFilter{
         5.997200e-04, -6.068000e-05, -3.324900e-04, -3.325600e-04, -2.406600e-04, -3.325600e-04, -3.324900e-04, -6.068000e-05,  5.997200e-04,
        -6.068000e-05,  1.263100e-04,  4.927100e-04,  1.459700e-04, -3.732100e-04,  1.459700e-04,  4.927100e-04,  1.263100e-04, -6.068000e-05,
        -3.324900e-04,  4.927100e-04, -1.616650e-03, -1.437358e-02, -2.420138e-02, -1.437358e-02, -1.616650e-03,  4.927100e-04, -3.324900e-04,
        -3.325600e-04,  1.459700e-04, -1.437358e-02, -6.300923e-02, -9.623594e-02, -6.300923e-02, -1.437358e-02,  1.459700e-04, -3.325600e-04,
        -2.406600e-04, -3.732100e-04, -2.420138e-02, -9.623594e-02,  8.554893e-01, -9.623594e-02, -2.420138e-02, -3.732100e-04, -2.406600e-04,
        -3.325600e-04,  1.459700e-04, -1.437358e-02, -6.300923e-02, -9.623594e-02, -6.300923e-02, -1.437358e-02,  1.459700e-04, -3.325600e-04,
        -3.324900e-04,  4.927100e-04, -1.616650e-03, -1.437358e-02, -2.420138e-02, -1.437358e-02, -1.616650e-03,  4.927100e-04, -3.324900e-04,
        -6.068000e-05,  1.263100e-04,  4.927100e-04,  1.459700e-04, -3.732100e-04,  1.459700e-04,  4.927100e-04,  1.263100e-04, -6.068000e-05,
         5.997200e-04, -6.068000e-05, -3.324900e-04, -3.325600e-04, -2.406600e-04, -3.325600e-04, -3.324900e-04, -6.068000e-05,  5.997200e-04,
    };
    return kFilter;
}

const std::vector<double> &sp0BandFilter() {
    static const std::vector<double> kFilter{
        -9.066000e-05, -1.738640e-03, -4.942500e-03, -7.889390e-03, -1.009473e-02, -7.889390e-03, -4.942500e-03, -1.738640e-03, -9.066000e-05,
        -1.738640e-03, -4.625150e-03, -7.272540e-03, -7.623410e-03, -9.091950e-03, -7.623410e-03, -7.272540e-03, -4.625150e-03, -1.738640e-03,
        -4.942500e-03, -7.272540e-03, -2.129540e-02, -2.435662e-02, -3.487008e-02, -2.435662e-02, -2.129540e-02, -7.272540e-03, -4.942500e-03,
        -7.889390e-03, -7.623410e-03, -2.435662e-02, -1.730466e-02, -3.158605e-02, -1.730466e-02, -2.435662e-02, -7.623410e-03, -7.889390e-03,
        -1.009473e-02, -9.091950e-03, -3.487008e-02, -3.158605e-02,  9.464195e-01, -3.158605e-02, -3.487008e-02, -9.091950e-03, -1.009473e-02,
        -7.889390e-03, -7.623410e-03, -2.435662e-02, -1.730466e-02, -3.158605e-02, -1.730466e-02, -2.435662e-02, -7.623410e-03, -7.889390e-03,
        -4.942500e-03, -7.272540e-03, -2.129540e-02, -2.435662e-02, -3.487008e-02, -2.435662e-02, -2.129540e-02, -7.272540e-03, -4.942500e-03,
        -1.738640e-03, -4.625150e-03, -7.272540e-03, -7.623410e-03, -9.091950e-03, -7.623410e-03, -7.272540e-03, -4.625150e-03, -1.738640e-03,
        -9.066000e-05, -1.738640e-03, -4.942500e-03, -7.889390e-03, -1.009473e-02, -7.889390e-03, -4.942500e-03, -1.738640e-03, -9.066000e-05,
    };
    return kFilter;
}

struct VisibilityPyramidLevel {
    size_t width = 0;
    size_t height = 0;
    double frequencyCpd = 0.0;
    std::vector<double> band;
};

struct VisibilityPyramid {
    std::vector<VisibilityPyramidLevel> levels;
    size_t baseWidth = 0;
    size_t baseHeight = 0;
};

struct SpectralCurve1 {
    std::vector<double> wavelengths;
    std::vector<double> values;
};

struct SpectralCurve3 {
    std::vector<double> wavelengths;
    std::vector<Vec3d> values;
};

std::vector<double> computePchipSlopes(
    const std::vector<double> &x,
    const std::vector<double> &y
) {
    const size_t n = x.size();
    std::vector<double> d(n, 0.0);
    if (n < 2)
        return d;
    if (n == 2) {
        const double h = std::max(x[1] - x[0], 1e-12);
        const double slope = (y[1] - y[0]) / h;
        d[0] = slope;
        d[1] = slope;
        return d;
    }

    std::vector<double> h(n - 1, 0.0);
    std::vector<double> delta(n - 1, 0.0);
    for (size_t i = 0; i + 1 < n; ++i) {
        h[i] = std::max(x[i + 1] - x[i], 1e-12);
        delta[i] = (y[i + 1] - y[i]) / h[i];
    }

    auto endpointSlope = [&](size_t idx0, size_t idx1) {
        const double h0 = h[idx0];
        const double h1 = h[idx1];
        const double del0 = delta[idx0];
        const double del1 = delta[idx1];
        double slope = ((2.0 * h0 + h1) * del0 - h0 * del1) / std::max(h0 + h1, 1e-12);
        if (slope * del0 <= 0.0)
            slope = 0.0;
        else if ((del0 * del1 < 0.0) && (std::abs(slope) > std::abs(3.0 * del0)))
            slope = 3.0 * del0;
        return slope;
    };

    d[0] = endpointSlope(0, 1);
    d[n - 1] = endpointSlope(n - 2, n - 3);

    for (size_t i = 1; i + 1 < n; ++i) {
        if (delta[i - 1] == 0.0 || delta[i] == 0.0 || delta[i - 1] * delta[i] < 0.0) {
            d[i] = 0.0;
            continue;
        }
        const double w1 = 2.0 * h[i] + h[i - 1];
        const double w2 = h[i] + 2.0 * h[i - 1];
        d[i] = (w1 + w2) / ((w1 / delta[i - 1]) + (w2 / delta[i]));
    }
    return d;
}

double evaluatePchip(
    const std::vector<double> &x,
    const std::vector<double> &y,
    const std::vector<double> &slopes,
    double query
) {
    if (x.empty() || y.empty() || slopes.empty())
        return 0.0;
    if (query < x.front() || query > x.back())
        return 0.0;
    if (query == x.back())
        return y.back();

    const auto upper = std::upper_bound(x.begin(), x.end(), query);
    const size_t hi = static_cast<size_t>(upper - x.begin());
    const size_t lo = hi - 1;
    const double span = std::max(x[hi] - x[lo], 1e-12);
    const double t = (query - x[lo]) / span;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    const double h10 = t3 - 2.0 * t2 + t;
    const double h01 = -2.0 * t3 + 3.0 * t2;
    const double h11 = t3 - t2;
    return h00 * y[lo] + h10 * span * slopes[lo] + h01 * y[hi] + h11 * span * slopes[hi];
}

SpectralCurve1 resampleSpectralCurve1Pchip(const SpectralCurve1 &source) {
    SpectralCurve1 curve;
    if (source.wavelengths.empty())
        return curve;
    constexpr double kLambdaMin = 360.0;
    constexpr double kLambdaMax = 780.0;
    constexpr size_t kLambdaCount = 420;
    curve.wavelengths.reserve(kLambdaCount);
    curve.values.reserve(kLambdaCount);
    const std::vector<double> slopes = computePchipSlopes(source.wavelengths, source.values);
    for (size_t i = 0; i < kLambdaCount; ++i) {
        const double t = kLambdaCount > 1 ? static_cast<double>(i) / static_cast<double>(kLambdaCount - 1) : 0.0;
        const double wavelength = kLambdaMin + (kLambdaMax - kLambdaMin) * t;
        curve.wavelengths.push_back(wavelength);
        curve.values.push_back(evaluatePchip(source.wavelengths, source.values, slopes, wavelength));
    }
    return curve;
}

SpectralCurve3 resampleSpectralCurve3Pchip(const SpectralCurve3 &source) {
    SpectralCurve3 curve;
    if (source.wavelengths.empty())
        return curve;
    constexpr double kLambdaMin = 360.0;
    constexpr double kLambdaMax = 780.0;
    constexpr size_t kLambdaCount = 420;
    curve.wavelengths.reserve(kLambdaCount);
    curve.values.reserve(kLambdaCount);

    std::vector<double> xs = source.wavelengths;
    std::vector<double> ysX(source.values.size(), 0.0);
    std::vector<double> ysY(source.values.size(), 0.0);
    std::vector<double> ysZ(source.values.size(), 0.0);
    for (size_t i = 0; i < source.values.size(); ++i) {
        ysX[i] = source.values[i].x;
        ysY[i] = source.values[i].y;
        ysZ[i] = source.values[i].z;
    }
    const std::vector<double> slopesX = computePchipSlopes(xs, ysX);
    const std::vector<double> slopesY = computePchipSlopes(xs, ysY);
    const std::vector<double> slopesZ = computePchipSlopes(xs, ysZ);

    for (size_t i = 0; i < kLambdaCount; ++i) {
        const double t = kLambdaCount > 1 ? static_cast<double>(i) / static_cast<double>(kLambdaCount - 1) : 0.0;
        const double wavelength = kLambdaMin + (kLambdaMax - kLambdaMin) * t;
        curve.wavelengths.push_back(wavelength);
        curve.values.push_back(Vec3d{
            evaluatePchip(xs, ysX, slopesX, wavelength),
            evaluatePchip(xs, ysY, slopesY, wavelength),
            evaluatePchip(xs, ysZ, slopesZ, wavelength),
        });
    }
    return curve;
}

struct NativeHdrvdpPnLookup {
    std::vector<double> coneY;
    std::vector<double> coneJnd;
    std::vector<double> rodY;
    std::vector<double> rodJnd;
};

struct HdrvdpCsfLookup {
    std::vector<double> csfLa;
    std::vector<double> csfLogLa;
    std::vector<std::vector<double> > sensitivities;
};

// Bits of NativeHdrvdpPreparedImage::fields: which per-pixel vectors an entry holds. Each caller
// asks only for the vectors it reads; a vector that is computed has the same values as before.
constexpr unsigned kPreparedLResponse = 1u << 0;
constexpr unsigned kPreparedMResponse = 1u << 1;
constexpr unsigned kPreparedRodResponse = 1u << 2;
constexpr unsigned kPreparedLuminance = 1u << 3;
constexpr unsigned kPreparedAdaptation = 1u << 4;
constexpr unsigned kPreparedAchromatic = 1u << 5;
constexpr unsigned kPreparedNativeRgbPreMtf = 1u << 6;
constexpr unsigned kPreparedNativeRgb = 1u << 7;
// view visibility (contrast maps and side-by-side comparison)
constexpr unsigned kPreparedVisibilityFields = kPreparedAdaptation | kPreparedAchromatic;
// perceptual maps: every map kind of one picture is served by one entry
constexpr unsigned kPreparedPerceptualFields = kPreparedLResponse | kPreparedMResponse |
    kPreparedRodResponse | kPreparedLuminance | kPreparedAdaptation | kPreparedAchromatic;
// view-visibility debug dump
constexpr unsigned kPreparedAllFields = kPreparedPerceptualFields | kPreparedNativeRgbPreMtf | kPreparedNativeRgb;

struct NativeHdrvdpSpectralContext;

struct NativeHdrvdpPreparedImage {
    std::vector<double> lResponse;
    std::vector<double> mResponse;
    std::vector<double> rodResponse;
    std::vector<double> luminance;
    std::vector<double> adaptation;
    std::vector<double> achromatic;
    std::vector<double> nativeRgbPreMtf;
    std::vector<double> nativeRgb;
    // kPrepared* bits of the vectors above that were computed (the others are empty). The former
    // post-MTF XYZ vector is gone: it was filled but never read.
    unsigned fields = 0;
    // The (never evicted) cached spectral context the vectors were computed with. Missing vectors
    // are later added with this same context, and the pre-MTF channels are recomputed from the
    // picture with its matrix (hdrvdpPreMtfChannelsAt) instead of being stored.
    const NativeHdrvdpSpectralContext *spectralContext = nullptr;
};

enum class HdrvdpPreparedImageMode {
    DirectChannels,
    NativeTransformed,
};

struct CachedPreparedHdrvdpImageEntry {
    std::filesystem::file_time_type mtime;
    NativeHdrvdpPreparedImage prepared;
};

struct CachedViewVisibilityResultEntry {
    std::filesystem::file_time_type referenceMtime;
    std::filesystem::file_time_type testMtime;
    std::filesystem::file_time_type maskMtime;
    ViewVisibilitySummaryResult result;
};

struct CachedPerceptualMapResultEntry {
    std::filesystem::file_time_type inputMtime;
    PerceptualMapResult result;
};

struct NativeHdrvdpParams {
    double baseSensitivityCorrection = 0.203943775672;
    double maskSelf = 1.41846291721;
    double maskXo = -50.0;
    double maskXn = 0.136877512403;
    double maskQ = 0.108934275615;
    double maskP = 0.3424;
    double psychFuncSlope = 0.34;
    double siSigma = -0.502280453708;
    double siSize = -0.034244;
    double ignoreFreqsLowerThan = 1.0;
    double maskingNorm = 0.0;
    int maskingPoolSize = 3;
    bool doSiGauss = false;
    bool doPuDilate = false;
    int puDilate = 3;
    double rodSensitivity = 0.0;
    double age = 24.0;
    double aeslSlopeFreq = -2.711;
    double aeslBase = -0.125539;
    std::array<double, 4> mtfA{};
    std::array<double, 4> mtfB{{0.028, 0.37, 37.0, 360.0}};
    std::array<double, 4> csfSa{{315.98, 6.7977, 1.6008, 0.25534}};
    std::array<double, 6> csfSr{{1.1732, 1.32, 1.095, 0.5547, 2.9899, 1.8}};
    std::array<double, 7> csfLums{{0.0002, 0.002, 0.02, 0.2, 2.0, 20.0, 150.0}};
    std::array<std::array<double, 5>, 7> csfParams{{
        {{0.699404, 1.26181, 4.27832, 0.361902, 3.11914}},
        {{1.00865, 0.893585, 4.27832, 0.361902, 2.18938}},
        {{1.41627, 0.84864, 3.57253, 0.530355, 3.12486}},
        {{1.90256, 0.699243, 3.94545, 0.68608, 4.41846}},
        {{2.28867, 0.530826, 4.25337, 0.866916, 4.65117}},
        {{2.46011, 0.459297, 3.78765, 0.981028, 4.33546}},
        {{2.5145, 0.312626, 4.15264, 0.952367, 3.22389}},
    }};
};

struct NativeHdrvdpSpectralContext {
    SpectralCurve3 emissionPreAod;
    SpectralCurve3 emissionPostAod;
    SpectralCurve3 xyzCmf;
    SpectralCurve3 lmsSens;
    SpectralCurve1 rodSens;
    Mat3d nativeToXyzPreAod{{{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}}};
    Mat3d xyzToNativePreAod{{{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}}};
    Mat3d nativeToXyz{{{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}}};
    Mat3d xyzToNative{{{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}}};
    std::array<std::array<double, 4>, 3> nativeToLmsr{};
};

NativeHdrvdpParams makeNativeHdrvdpParams() {
    NativeHdrvdpParams params;
    const double par0 = 0.061466549455263;
    const double par1 = 0.99727370023777070;
    params.mtfA = {{
        par1 * 0.426,
        par1 * 0.574,
        (1.0 - par1) * par0,
        (1.0 - par1) * (1.0 - par0),
    }};
    return params;
}

std::string formatCacheDouble(double value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(9) << value;
    return oss.str();
}

const HdrImage &readRadianceHDRCached(const std::string &path, bool addHalfStep = true, bool flipY = true) {
    namespace fs = std::filesystem;
    static std::map<std::string, CachedHdrImageEntry> sCache;

    const fs::file_time_type mtime = fs::last_write_time(fs::path(path));
    const std::string key =
        path +
        (addHalfStep ? "|halfstep=1" : "|halfstep=0") +
        (flipY ? "|flipy=1" : "|flipy=0");
    auto it = sCache.find(key);
    if (it == sCache.end() || it->second.mtime != mtime) {
        CachedHdrImageEntry entry;
        entry.mtime = mtime;
        entry.image = readRadianceHDR(path, addHalfStep, flipY);
        it = sCache.insert_or_assign(key, std::move(entry)).first;
    }
    return it->second.image;
}

void removeBaseBandDc(VisibilityPyramid &pyramid) {
    if (pyramid.levels.empty())
        return;
    std::vector<double> &baseBand = pyramid.levels.back().band;
    if (baseBand.empty())
        return;
    double meanValue = 0.0;
    for (double value : baseBand)
        meanValue += value;
    meanValue /= static_cast<double>(baseBand.size());
    for (double &value : baseBand)
        value -= meanValue;
}

std::vector<double> parseCsvNumbers(const std::string &line) {
    std::string normalized = line;
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::vector<double> values;
    std::istringstream iss(normalized);
    double parsed = 0.0;
    while (iss >> parsed)
        values.push_back(parsed);
    return values;
}

void writeSpectralCurveCsv(const std::string &path, const SpectralCurve1 &curve) {
    std::ofstream out(path);
    out << std::setprecision(15);
    for (size_t i = 0; i < curve.wavelengths.size() && i < curve.values.size(); ++i)
        out << curve.wavelengths[i] << "," << curve.values[i] << "\n";
}

void writeSpectralCurveCsv(const std::string &path, const SpectralCurve3 &curve) {
    std::ofstream out(path);
    out << std::setprecision(15);
    for (size_t i = 0; i < curve.wavelengths.size() && i < curve.values.size(); ++i)
        out << curve.wavelengths[i] << ","
            << curve.values[i].x << ","
            << curve.values[i].y << ","
            << curve.values[i].z << "\n";
}

// Folder of the running executable; inside hdrspace.app this is Contents/MacOS.
static std::filesystem::path currentExecutableDirectory() {
    namespace fs = std::filesystem;
    std::error_code ec;
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        const fs::path executable = fs::weakly_canonical(fs::path(buffer.c_str()), ec);
        if (!ec)
            return executable.parent_path();
    }
#elif defined(__linux__)
    const fs::path executable = fs::read_symlink("/proc/self/exe", ec);
    if (!ec)
        return executable.parent_path();
#elif defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const unsigned long count = GetModuleFileNameW(nullptr, &buffer[0], static_cast<unsigned long>(buffer.size()));
    if (count > 0 && count < buffer.size()) {
        buffer.resize(count);
        return fs::path(buffer).parent_path();
    }
#endif
    return {};
}

std::string resolveHdrvdpResourcePath(const std::string &anchorPath, const std::string &relativePath) {
    namespace fs = std::filesystem;

    const fs::path requested(relativePath);
    if (requested.is_absolute() && fs::exists(requested))
        return requested.string();

    const fs::path anchorDir = fs::path(anchorPath).parent_path();
    const fs::path analysisDir = anchorDir.parent_path();
    const fs::path hdrvdpRoot = analysisDir / "hdrvdp3";
    // The HDR-VDP data shipped with the app (Contents/Resources/hdrvdp3), so the
    // bundled command-line tools find it without a source checkout.
    const fs::path executableDir = currentExecutableDirectory();
    const fs::path bundledHdrvdpRoot = executableDir.empty()
        ? fs::path()
        : executableDir.parent_path() / "Resources" / "hdrvdp3";

    std::vector<fs::path> candidates{
        requested,
        anchorDir / requested,
        analysisDir / requested,
        hdrvdpRoot / requested,
        bundledHdrvdpRoot / requested,
#if defined(_WIN32)
        // Windows package: bin/mergehdr.exe next to hdrvdp3/data.
        executableDir.empty() ? fs::path() : executableDir.parent_path() / "hdrvdp3" / requested,
#endif
    };

    if (!requested.has_parent_path()) {
        candidates.push_back(hdrvdpRoot / "data" / requested);
        candidates.push_back(bundledHdrvdpRoot / "data" / requested);
    } else if (requested.begin() != requested.end() && (*requested.begin()) == "data") {
        candidates.push_back(hdrvdpRoot / requested);
        candidates.push_back(bundledHdrvdpRoot / requested);
    }

    for (const fs::path &candidate : candidates) {
        if (!candidate.empty() && fs::exists(candidate))
            return candidate.string();
    }

    return relativePath;
}

SpectralCurve1 loadSpectralCurve1(const std::string &path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("Could not open spectral data: " + path);

    SpectralCurve1 raw;
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<double> parsed = parseCsvNumbers(line);
        if (parsed.size() < 2)
            continue;
        raw.wavelengths.push_back(parsed[0]);
        raw.values.push_back(parsed[1]);
    }

    if (raw.wavelengths.empty())
        throw std::runtime_error("Spectral data is empty: " + path);
    return resampleSpectralCurve1Pchip(raw);
}

SpectralCurve3 loadSpectralCurve3(const std::string &path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("Could not open spectral data: " + path);

    SpectralCurve3 raw;
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<double> parsed = parseCsvNumbers(line);
        if (parsed.size() < 4)
            continue;
        raw.wavelengths.push_back(parsed[0]);
        raw.values.push_back(Vec3d{parsed[1], parsed[2], parsed[3]});
    }

    if (raw.wavelengths.empty())
        throw std::runtime_error("Spectral data is empty: " + path);
    return resampleSpectralCurve3Pchip(raw);
}

double interpolateCurve1(const SpectralCurve1 &curve, double wavelength) {
    if (wavelength <= curve.wavelengths.front())
        return curve.values.front();
    if (wavelength >= curve.wavelengths.back())
        return curve.values.back();
    const auto upper = std::lower_bound(curve.wavelengths.begin(), curve.wavelengths.end(), wavelength);
    if (upper == curve.wavelengths.begin())
        return curve.values.front();
    const size_t hi = static_cast<size_t>(upper - curve.wavelengths.begin());
    const size_t lo = hi - 1;
    const double span = std::max(curve.wavelengths[hi] - curve.wavelengths[lo], 1e-12);
    const double t = (wavelength - curve.wavelengths[lo]) / span;
    return curve.values[lo] * (1.0 - t) + curve.values[hi] * t;
}

Vec3d interpolateCurve3(const SpectralCurve3 &curve, double wavelength) {
    if (wavelength <= curve.wavelengths.front())
        return curve.values.front();
    if (wavelength >= curve.wavelengths.back())
        return curve.values.back();
    const auto upper = std::lower_bound(curve.wavelengths.begin(), curve.wavelengths.end(), wavelength);
    if (upper == curve.wavelengths.begin())
        return curve.values.front();
    const size_t hi = static_cast<size_t>(upper - curve.wavelengths.begin());
    const size_t lo = hi - 1;
    const double span = std::max(curve.wavelengths[hi] - curve.wavelengths[lo], 1e-12);
    const double t = (wavelength - curve.wavelengths[lo]) / span;
    return curve.values[lo] * (1.0 - t) + curve.values[hi] * t;
}

Vec3d mulMat3Vec(const Mat3d &matrix, const Vec3d &value) {
    return Vec3d{
        matrix.m[0][0] * value.x + matrix.m[0][1] * value.y + matrix.m[0][2] * value.z,
        matrix.m[1][0] * value.x + matrix.m[1][1] * value.y + matrix.m[1][2] * value.z,
        matrix.m[2][0] * value.x + matrix.m[2][1] * value.y + matrix.m[2][2] * value.z,
    };
}

double hdrvdpJointRodConeSens(double la, const NativeHdrvdpParams &params) {
    const double safeLa = std::max(la, 1e-8);
    return params.csfSa[0] * std::pow(std::pow(params.csfSa[1] / safeLa, params.csfSa[2]) + 1.0, -params.csfSa[3]);
}

double clampd(double value, double lo, double hi) {
    return std::max(lo, std::min(hi, value));
}

bool hdrvdpDiffMaskChannelChanged(double referenceValue, double testValue) {
    if (referenceValue == 0.0)
        return testValue != 0.0;
    return (std::abs(testValue - referenceValue) / referenceValue) > 0.001;
}

double geoMeanPositive(const std::vector<double> &values) {
    if (values.empty())
        return 0.0;
    double sumLogs = 0.0;
    for (double value : values)
        sumLogs += std::log(std::max(value, 1e-8));
    return std::exp(sumLogs / static_cast<double>(values.size()));
}

double hdrvdpAesl(double rho, const NativeHdrvdpParams &params) {
    const double gamma = std::pow(10.0, params.aeslBase);
    const double ageTerm = std::max(0.0, params.age - 24.0);
    return std::pow(10.0,
        -(std::pow(10.0, params.aeslSlopeFreq * std::log2(std::max(rho + gamma, 1e-8)))) * ageTerm);
}

double pupilDStanleyDavies(double luminance, double area) {
    const double la = std::max(luminance * area, 1e-8);
    const double x = std::pow(la / 846.0, 0.41);
    return 7.75 - 5.75 * (x / (x + 2.0));
}

double pupilDUnified(double luminance, double area, double age) {
    const double y0 = 28.58;
    const double clampedAge = clampd(age, 20.0, 83.0);
    const double dSd = pupilDStanleyDavies(luminance, area);
    return dSd + (clampedAge - y0) * (0.02132 - 0.009562 * dSd);
}

double hdrvdpRodSens(double la, const NativeHdrvdpParams &params) {
    const double safeLa = std::max(la, 1e-8);
    const double peakL = params.csfSr[0];
    double response = 0.0;
    if (safeLa > peakL) {
        response = std::exp(-std::pow(std::abs(std::log10(safeLa / peakL)), params.csfSr[4]) / params.csfSr[3]);
    } else {
        response = std::exp(-std::pow(std::abs(std::log10(safeLa / peakL)), params.csfSr[2]) / params.csfSr[1]);
    }
    return response * std::pow(10.0, params.csfSr[5] + params.rodSensitivity);
}

double interpolateVectorLinear(const std::vector<double> &xs, const std::vector<double> &ys, double x) {
    if (xs.empty() || ys.empty())
        return 0.0;
    if (x <= xs.front())
        return ys.front();
    if (x >= xs.back())
        return ys.back();
    const auto upper = std::lower_bound(xs.begin(), xs.end(), x);
    if (upper == xs.begin())
        return ys.front();
    const size_t hi = static_cast<size_t>(upper - xs.begin());
    const size_t lo = hi - 1;
    const double span = std::max(xs[hi] - xs[lo], 1e-12);
    const double t = (x - xs[lo]) / span;
    return ys[lo] * (1.0 - t) + ys[hi] * t;
}

NativeHdrvdpPnLookup buildNativeHdrvdpPnLookup(const NativeHdrvdpParams &params, double sensitivityCorrection) {
    NativeHdrvdpPnLookup lookup;
    const size_t sampleCount = 2048;
    lookup.coneY.resize(sampleCount);
    lookup.coneJnd.resize(sampleCount);
    lookup.rodY.resize(sampleCount);
    lookup.rodJnd.resize(sampleCount);

    std::vector<double> cL(sampleCount, 0.0);
    std::vector<double> logCL(sampleCount, 0.0);
    for (size_t i = 0; i < sampleCount; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(sampleCount - 1);
        logCL[i] = -5.0 + 10.0 * t;
        cL[i] = std::pow(10.0, logCL[i]);
    }

    std::vector<double> sA(sampleCount, 0.0);
    std::vector<double> sR(sampleCount, 0.0);
    std::vector<double> sC(sampleCount, 0.0);
    std::vector<double> coneBase(sampleCount, 0.0);
    for (size_t i = 0; i < sampleCount; ++i) {
        sA[i] = hdrvdpJointRodConeSens(cL[i], params);
        sR[i] = hdrvdpRodSens(cL[i], params);
        coneBase[i] = std::max(sA[i] - sR[i], 1e-3);
    }
    for (size_t i = 0; i < sampleCount; ++i) {
        const double sampleLum = std::min(cL.back(), cL[i] * 2.0);
        sC[i] = 0.5 * interpolateVectorLinear(cL, coneBase, sampleLum);
    }

    const double sensitivityScale = std::pow(10.0, params.baseSensitivityCorrection + sensitivityCorrection);
    const double jacobian = std::log(10.0);
    double coneAccum = 0.0;
    double rodAccum = 0.0;
    for (size_t i = 0; i < sampleCount; ++i) {
        if (i > 0) {
            const double dx = logCL[i] - logCL[i - 1];
            coneAccum += 0.5 * dx * ((sC[i - 1] * jacobian) + (sC[i] * jacobian));
            rodAccum += 0.5 * dx * ((sR[i - 1] * jacobian) + (sR[i] * jacobian));
        }
        lookup.coneY[i] = logCL[i];
        lookup.rodY[i] = logCL[i];
        lookup.coneJnd[i] = coneAccum * sensitivityScale;
        lookup.rodJnd[i] = rodAccum * sensitivityScale;
    }

    return lookup;
}

const NativeHdrvdpPnLookup &getNativeHdrvdpPnLookupCached(
    const NativeHdrvdpParams &params,
    double sensitivityCorrection
) {
    static std::map<std::string, NativeHdrvdpPnLookup> sCache;
    const std::string key =
        "sens=" + formatCacheDouble(sensitivityCorrection) +
        "|base=" + formatCacheDouble(params.baseSensitivityCorrection);
    auto it = sCache.find(key);
    if (it == sCache.end())
        it = sCache.emplace(key, buildNativeHdrvdpPnLookup(params, sensitivityCorrection)).first;
    return it->second;
}

NativeHdrvdpSpectralContext buildNativeHdrvdpSpectralContext(
    const std::string &anchorPath,
    const std::string &spectralEmissionPath,
    const NativeHdrvdpParams &params
) {
    const std::string emissionPath = resolveHdrvdpResourcePath(anchorPath, spectralEmissionPath.empty() ? "data/emission_spectra_oled.csv" : spectralEmissionPath);
    const std::string xyzPath = resolveHdrvdpResourcePath(emissionPath, "data/ciexyz31.csv");
    const std::string conePath = resolveHdrvdpResourcePath(emissionPath, "data/log_cone_smith_pokorny_1975.csv");
    const std::string rodPath = resolveHdrvdpResourcePath(emissionPath, "data/cie_scotopic_lum.txt");

    const SpectralCurve3 emission = loadSpectralCurve3(emissionPath);
    const SpectralCurve3 xyz = loadSpectralCurve3(xyzPath);
    const SpectralCurve3 logCone = loadSpectralCurve3(conePath);
    const SpectralCurve1 rod = loadSpectralCurve1(rodPath);

    SpectralCurve3 cone;
    cone.wavelengths = logCone.wavelengths;
    cone.values.reserve(logCone.values.size());
    double minLogCone = std::numeric_limits<double>::infinity();
    for (const Vec3d &value : logCone.values) {
        minLogCone = std::min(minLogCone, value.x);
        minLogCone = std::min(minLogCone, value.y);
        minLogCone = std::min(minLogCone, value.z);
    }
    if (!std::isfinite(minLogCone))
        minLogCone = 0.0;
    for (const Vec3d &value : logCone.values) {
        const double lx = value.x == 0.0 ? minLogCone : value.x;
        const double ly = value.y == 0.0 ? minLogCone : value.y;
        const double lz = value.z == 0.0 ? minLogCone : value.z;
        cone.values.push_back(Vec3d{
            std::pow(10.0, lx),
            std::pow(10.0, ly),
            std::pow(10.0, lz),
        });
    }

    SpectralCurve3 adjustedEmission = emission;
    {
        const std::vector<double> lam{400, 410, 420, 430, 440, 450, 460, 470, 480, 490, 500, 510, 520,
            530, 540, 550, 560, 570, 580, 590, 600, 610, 620, 630, 640, 650};
        const std::vector<double> tl1{0.600, 0.510, 0.433, 0.377, 0.327, 0.295, 0.267, 0.233, 0.207,
            0.187, 0.167, 0.147, 0.133, 0.120, 0.107, 0.093, 0.080, 0.067, 0.053, 0.040, 0.033, 0.027,
            0.020, 0.013, 0.007, 0.000};
        const std::vector<double> tl2{1.000, 0.583, 0.300, 0.116, 0.033, 0.005, 0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        const std::vector<double> tl1Slopes = computePchipSlopes(lam, tl1);
        const std::vector<double> tl2Slopes = computePchipSlopes(lam, tl2);
        const double clampedAge = clampd(params.age, 20.0, 83.0);
        for (size_t i = 0; i < adjustedEmission.wavelengths.size(); ++i) {
            const double wavelength = clampd(adjustedEmission.wavelengths[i], lam.front(), lam.back());
            const double tl1Value = evaluatePchip(lam, tl1, tl1Slopes, wavelength);
            const double tl2Value = evaluatePchip(lam, tl2, tl2Slopes, wavelength);
            const double odY = tl1Value + tl2Value;
            double od = 0.0;
            if (clampedAge <= 60.0)
                od = tl1Value * (1.0 + 0.02 * (clampedAge - 32.0)) + tl2Value;
            else
                od = tl1Value * (1.56 + 0.0667 * (clampedAge - 60.0)) + tl2Value;
            const double transFilter = std::pow(10.0, -(od - odY));
            adjustedEmission.values[i].x *= transFilter;
            adjustedEmission.values[i].y *= transFilter;
            adjustedEmission.values[i].z *= transFilter;
        }
    }

    NativeHdrvdpSpectralContext context;
    context.emissionPreAod = emission;
    context.xyzCmf = xyz;
    context.rodSens = rod;

    auto accumulateNativeToXyz = [&](const SpectralCurve3& emissionCurve, Mat3d& outMatrix) {
        const size_t localSampleCount = std::min({
            emissionCurve.wavelengths.size(),
            emissionCurve.values.size(),
            xyz.wavelengths.size(),
            xyz.values.size(),
        });
        for (size_t i = 1; i < localSampleCount; ++i) {
            const double dx = std::max(emissionCurve.wavelengths[i] - emissionCurve.wavelengths[i - 1], 0.0);
            const double scale = 0.5 * dx * kWhiteEfficacy;
            const Vec3d eSens0 = emissionCurve.values[i - 1];
            const Vec3d eSens1 = emissionCurve.values[i];
            const Vec3d xyz0 = xyz.values[i - 1];
            const Vec3d xyz1 = xyz.values[i];
            auto trapPair = [&](double a0, double a1, double b0, double b1) {
                return scale * ((a0 * b0) + (a1 * b1));
            };
            outMatrix.m[0][0] += trapPair(xyz0.x, xyz1.x, eSens0.x, eSens1.x);
            outMatrix.m[0][1] += trapPair(xyz0.x, xyz1.x, eSens0.y, eSens1.y);
            outMatrix.m[0][2] += trapPair(xyz0.x, xyz1.x, eSens0.z, eSens1.z);
            outMatrix.m[1][0] += trapPair(xyz0.y, xyz1.y, eSens0.x, eSens1.x);
            outMatrix.m[1][1] += trapPair(xyz0.y, xyz1.y, eSens0.y, eSens1.y);
            outMatrix.m[1][2] += trapPair(xyz0.y, xyz1.y, eSens0.z, eSens1.z);
            outMatrix.m[2][0] += trapPair(xyz0.z, xyz1.z, eSens0.x, eSens1.x);
            outMatrix.m[2][1] += trapPair(xyz0.z, xyz1.z, eSens0.y, eSens1.y);
            outMatrix.m[2][2] += trapPair(xyz0.z, xyz1.z, eSens0.z, eSens1.z);
        }
        const double yNorm = std::max(outMatrix.m[1][0] + outMatrix.m[1][1] + outMatrix.m[1][2], 1e-12);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col)
                outMatrix.m[row][col] /= yNorm;
        }
    };

    accumulateNativeToXyz(emission, context.nativeToXyzPreAod);
    if (!invertMatrix(context.nativeToXyzPreAod, context.xyzToNativePreAod))
        throw std::runtime_error("Could not invert pre-AOD native display transform for HDR-VDP subset.");
    const size_t sampleCount = std::min({
        adjustedEmission.wavelengths.size(),
        adjustedEmission.values.size(),
        xyz.wavelengths.size(),
        xyz.values.size(),
        cone.wavelengths.size(),
        cone.values.size(),
        rod.wavelengths.size(),
        rod.values.size(),
    });
    for (size_t i = 1; i < sampleCount; ++i) {
        const double dx = std::max(adjustedEmission.wavelengths[i] - adjustedEmission.wavelengths[i - 1], 0.0);
        const double scale = 0.5 * dx * kWhiteEfficacy;

        const Vec3d eSens0 = adjustedEmission.values[i - 1];
        const Vec3d eSens1 = adjustedEmission.values[i];
        const Vec3d xyz0 = xyz.values[i - 1];
        const Vec3d xyz1 = xyz.values[i];
        const Vec3d cone0 = cone.values[i - 1];
        const Vec3d cone1 = cone.values[i];
        const double rod0 = rod.values[i - 1];
        const double rod1 = rod.values[i];

        auto trapPair = [&](double a0, double a1, double b0, double b1) {
            return scale * ((a0 * b0) + (a1 * b1));
        };

        context.nativeToXyz.m[0][0] += trapPair(xyz0.x, xyz1.x, eSens0.x, eSens1.x);
        context.nativeToXyz.m[0][1] += trapPair(xyz0.x, xyz1.x, eSens0.y, eSens1.y);
        context.nativeToXyz.m[0][2] += trapPair(xyz0.x, xyz1.x, eSens0.z, eSens1.z);
        context.nativeToXyz.m[1][0] += trapPair(xyz0.y, xyz1.y, eSens0.x, eSens1.x);
        context.nativeToXyz.m[1][1] += trapPair(xyz0.y, xyz1.y, eSens0.y, eSens1.y);
        context.nativeToXyz.m[1][2] += trapPair(xyz0.y, xyz1.y, eSens0.z, eSens1.z);
        context.nativeToXyz.m[2][0] += trapPair(xyz0.z, xyz1.z, eSens0.x, eSens1.x);
        context.nativeToXyz.m[2][1] += trapPair(xyz0.z, xyz1.z, eSens0.y, eSens1.y);
        context.nativeToXyz.m[2][2] += trapPair(xyz0.z, xyz1.z, eSens0.z, eSens1.z);

        context.nativeToLmsr[0][0] += trapPair(cone0.x, cone1.x, eSens0.x, eSens1.x);
        context.nativeToLmsr[1][0] += trapPair(cone0.x, cone1.x, eSens0.y, eSens1.y);
        context.nativeToLmsr[2][0] += trapPair(cone0.x, cone1.x, eSens0.z, eSens1.z);
        context.nativeToLmsr[0][1] += trapPair(cone0.y, cone1.y, eSens0.x, eSens1.x);
        context.nativeToLmsr[1][1] += trapPair(cone0.y, cone1.y, eSens0.y, eSens1.y);
        context.nativeToLmsr[2][1] += trapPair(cone0.y, cone1.y, eSens0.z, eSens1.z);
        context.nativeToLmsr[0][2] += trapPair(cone0.z, cone1.z, eSens0.x, eSens1.x);
        context.nativeToLmsr[1][2] += trapPair(cone0.z, cone1.z, eSens0.y, eSens1.y);
        context.nativeToLmsr[2][2] += trapPair(cone0.z, cone1.z, eSens0.z, eSens1.z);
        context.nativeToLmsr[0][3] += trapPair(rod0, rod1, eSens0.x, eSens1.x);
        context.nativeToLmsr[1][3] += trapPair(rod0, rod1, eSens0.y, eSens1.y);
        context.nativeToLmsr[2][3] += trapPair(rod0, rod1, eSens0.z, eSens1.z);
    }

    const double yNorm = std::max(context.nativeToXyz.m[1][0] + context.nativeToXyz.m[1][1] + context.nativeToXyz.m[1][2], 1e-12);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            context.nativeToXyz.m[row][col] /= yNorm;
    }

    const double lmNorm = std::max(
        context.nativeToLmsr[0][0] + context.nativeToLmsr[1][0] + context.nativeToLmsr[2][0] +
        context.nativeToLmsr[0][1] + context.nativeToLmsr[1][1] + context.nativeToLmsr[2][1], 1e-12);
    for (auto &row : context.nativeToLmsr) {
        for (double &value : row)
            value /= lmNorm;
    }

    if (!invertMatrix(context.nativeToXyz, context.xyzToNative))
        throw std::runtime_error("Could not invert native display transform for HDR-VDP subset.");

    context.emissionPostAod = adjustedEmission;
    context.lmsSens.wavelengths = cone.wavelengths;
    context.lmsSens.values.reserve(cone.values.size());
    for (size_t i = 0; i < cone.values.size(); ++i) {
        context.lmsSens.values.push_back(Vec3d{
            cone.values[i].x,
            cone.values[i].y,
            cone.values[i].z,
        });
    }

    return context;
}

const NativeHdrvdpSpectralContext &getNativeHdrvdpSpectralContextCached(
    const std::string &anchorPath,
    const std::string &spectralEmissionPath,
    const NativeHdrvdpParams &params
) {
    static std::map<std::string, NativeHdrvdpSpectralContext> sCache;
    const std::string emissionPath = resolveHdrvdpResourcePath(anchorPath, spectralEmissionPath.empty() ? "data/emission_spectra_oled.csv" : spectralEmissionPath);
    const std::string key = emissionPath + "|age=" + formatCacheDouble(params.age);
    auto it = sCache.find(key);
    if (it == sCache.end())
        it = sCache.emplace(key, buildNativeHdrvdpSpectralContext(anchorPath, spectralEmissionPath, params)).first;
    return it->second;
}

const std::vector<double> &getHdrvdpMtfFilterCached(
    size_t width,
    size_t height,
    double pixelsPerDegree,
    const NativeHdrvdpParams &params
) {
    static std::map<std::string, std::vector<double> > sCache;
    const size_t paddedWidth = width * 2;
    const size_t paddedHeight = height * 2;
    const std::string key =
        std::to_string(paddedWidth) + "x" + std::to_string(paddedHeight) +
        "|ppd=" + formatCacheDouble(pixelsPerDegree);
    auto it = sCache.find(key);
    if (it == sCache.end()) {
        const std::vector<double> rho2 = createCycdegImage(paddedHeight, paddedWidth, pixelsPerDegree);
        std::vector<double> mtfFilter(rho2.size(), 0.0);
        for (size_t i = 0; i < rho2.size(); ++i)
            mtfFilter[i] = hdrvdpMtf(rho2[i], params);
        it = sCache.emplace(key, std::move(mtfFilter)).first;
    }
    return it->second;
}

double pnLookupValue(const std::vector<double> &yAxis, const std::vector<double> &jndAxis, double response) {
    const double minResponse = std::pow(10.0, yAxis.front());
    const double maxResponse = std::pow(10.0, yAxis.back());
    const double logResponse = std::log10(std::max(minResponse, std::min(maxResponse, response)));
    return interpolateVectorLinear(yAxis, jndAxis, logResponse);
}

std::vector<double> hdrvdpLocalAdaptMap(
    const std::vector<double> &luminance,
    size_t width,
    size_t height,
    double pixelsPerDegree
);

double hdrvdpMtf(double rho, const NativeHdrvdpParams &params);

// Input of the optical MTF for pixel `pixelIndex`: the input XYZ for DirectChannels, the
// display-native RGB clamped to 1e-6 for NativeTransformed (the "nativeRgbPreMtf" channels).
Vec3d hdrvdpPreMtfChannelsAt(
    const HdrImage &image,
    size_t pixelIndex,
    HdrvdpPreparedImageMode mode,
    const Mat3d &xyzToNativePreAod,
    ViewVisibilitySummaryOptions::InputColor inputColor,
    double inputScale
) {
    const Vec3d xyzIn = viewVisibilityInputXyzAt(image, pixelIndex, inputColor, inputScale);
    if (mode == HdrvdpPreparedImageMode::NativeTransformed) {
        const Vec3d native = mulMat3Vec(xyzToNativePreAod, xyzIn);
        return Vec3d{std::max(1e-6, native.x), std::max(1e-6, native.y), std::max(1e-6, native.z)};
    }
    return xyzIn;
}

// Computes the per-pixel HDR-VDP inputs of one picture. Only the vectors named in `fields`
// (kPrepared* bits) are stored; the others are not kept (and, where nothing else needs them, not
// computed). Every stored vector has exactly the values of the former all-fields version.
// `previous`, if given, is an entry of the same picture and settings computed with the same
// spectral context: its adaptation and achromatic vectors (same code, same inputs, so the same
// values) are moved over instead of being computed again.
NativeHdrvdpPreparedImage prepareNativeHdrvdpImage(
    const HdrImage &image,
    const NativeHdrvdpSpectralContext &spectralContext,
    double sensitivityCorrection,
    double pixelsPerDegree,
    HdrvdpPreparedImageMode mode,
    ViewVisibilitySummaryOptions::InputColor inputColor,
    unsigned fields,
    NativeHdrvdpPreparedImage *previous = nullptr
) {
    const NativeHdrvdpParams params = makeNativeHdrvdpParams();
    const NativeHdrvdpPnLookup &pn = getNativeHdrvdpPnLookupCached(params, sensitivityCorrection);
    NativeHdrvdpPreparedImage prepared;
    prepared.fields = fields;
    prepared.spectralContext = &spectralContext;
    const size_t pixelCount = image.width * image.height;
    const bool keepLResponse = (fields & kPreparedLResponse) != 0;
    const bool keepMResponse = (fields & kPreparedMResponse) != 0;
    const bool keepRodResponse = (fields & kPreparedRodResponse) != 0;
    const bool keepLuminance = (fields & kPreparedLuminance) != 0;
    const bool keepAdaptation = (fields & kPreparedAdaptation) != 0;
    const bool keepAchromatic = (fields & kPreparedAchromatic) != 0;
    const bool keepNativeRgbPreMtf = (fields & kPreparedNativeRgbPreMtf) != 0;
    const bool keepNativeRgb = (fields & kPreparedNativeRgb) != 0;
    if (keepNativeRgbPreMtf)
        prepared.nativeRgbPreMtf.resize(pixelCount * 3, 0.0);

    std::vector<double> workX(pixelCount, 0.0);
    std::vector<double> workY(pixelCount, 0.0);
    std::vector<double> workZ(pixelCount, 0.0);

    const double inputScale = viewVisibilityInputScale(image, inputColor);
    for (size_t i = 0; i < pixelCount; ++i) {
        const Vec3d channels = hdrvdpPreMtfChannelsAt(image, i, mode, spectralContext.xyzToNativePreAod, inputColor, inputScale);
        workX[i] = channels.x;
        workY[i] = channels.y;
        workZ[i] = channels.z;
        if (keepNativeRgbPreMtf) {
            prepared.nativeRgbPreMtf[i * 3 + 0] = workX[i];
            prepared.nativeRgbPreMtf[i * 3 + 1] = workY[i];
            prepared.nativeRgbPreMtf[i * 3 + 2] = workZ[i];
        }
    }

    const std::vector<double> &mtfFilter = getHdrvdpMtfFilterCached(
        image.width, image.height, pixelsPerDegree, params);

    workX = fastConvFftLikeMatlab(workX, image.width, image.height, mtfFilter, true, false, 0.0);
    workY = fastConvFftLikeMatlab(workY, image.width, image.height, mtfFilter, true, false, 0.0);
    workZ = fastConvFftLikeMatlab(workZ, image.width, image.height, mtfFilter, true, false, 0.0);

    // Pass 1: clamp the post-MTF channels in place (from here on workX/workY/workZ hold the
    // former nativeRgb channels) and compute the L+M luminance of the local adaptation map.
    std::vector<double> luminance(pixelCount, 0.0);
    for (size_t i = 0; i < pixelCount; ++i) {
        const double workR = std::max(1e-5, workX[i]);
        const double workG = std::max(1e-5, workY[i]);
        const double workB = std::max(1e-5, workZ[i]);
        workX[i] = workR;
        workY[i] = workG;
        workZ[i] = workB;

        const double lResp = workR * spectralContext.nativeToLmsr[0][0] + workG * spectralContext.nativeToLmsr[1][0] + workB * spectralContext.nativeToLmsr[2][0];
        const double mResp = workR * spectralContext.nativeToLmsr[0][1] + workG * spectralContext.nativeToLmsr[1][1] + workB * spectralContext.nativeToLmsr[2][1];
        luminance[i] = std::max(1e-6, lResp + mResp);
    }

    const bool reuseAdaptation = previous && (previous->fields & kPreparedAdaptation) != 0;
    const bool reuseAchromatic = keepAchromatic && previous && (previous->fields & kPreparedAchromatic) != 0;
    std::vector<double> adaptation = reuseAdaptation
        ? std::move(previous->adaptation)
        : hdrvdpLocalAdaptMap(luminance, image.width, image.height, pixelsPerDegree);
    if (keepLuminance)
        prepared.luminance = std::move(luminance);
    else
        std::vector<double>().swap(luminance);
    const double meanAdapt = std::max(geoMeanPositive(adaptation), 1e-8);
    const double area = (static_cast<double>(image.width) * static_cast<double>(image.height)) /
        std::max(pixelsPerDegree * pixelsPerDegree, 1e-8);
    const double dRef = pupilDUnified(meanAdapt, area, 28.0);
    const double dAge = pupilDUnified(meanAdapt, area, params.age);
    const double lumReduction = (dRef > 1e-8) ? ((dAge * dAge) / (dRef * dRef)) : 1.0;
    // The former code first stored the unreduced achromatic response and, whenever the pupil
    // correction applies, overwrote it with the reduced one; each pixel now gets the value that
    // was finally kept, with the same expressions.
    const bool reduceLuminance = std::abs(lumReduction - 1.0) > 1e-8;

    // Pass 2: the cone/rod responses and the achromatic response.
    if (keepLResponse)
        prepared.lResponse.resize(pixelCount, 0.0);
    if (keepMResponse)
        prepared.mResponse.resize(pixelCount, 0.0);
    if (keepRodResponse)
        prepared.rodResponse.resize(pixelCount, 0.0);
    if (reuseAchromatic)
        prepared.achromatic = std::move(previous->achromatic);
    else if (keepAchromatic)
        prepared.achromatic.resize(pixelCount, 0.0);
    for (size_t i = 0; i < pixelCount; ++i) {
        const double workR = workX[i];
        const double workG = workY[i];
        const double workB = workZ[i];

        const double lResp = workR * spectralContext.nativeToLmsr[0][0] + workG * spectralContext.nativeToLmsr[1][0] + workB * spectralContext.nativeToLmsr[2][0];
        const double mResp = workR * spectralContext.nativeToLmsr[0][1] + workG * spectralContext.nativeToLmsr[1][1] + workB * spectralContext.nativeToLmsr[2][1];
        const double rodResp = workR * spectralContext.nativeToLmsr[0][3] + workG * spectralContext.nativeToLmsr[1][3] + workB * spectralContext.nativeToLmsr[2][3];
        if (keepLResponse)
            prepared.lResponse[i] = std::max(1e-6, lResp);
        if (keepMResponse)
            prepared.mResponse[i] = std::max(1e-6, mResp);
        if (keepRodResponse)
            prepared.rodResponse[i] = std::max(1e-6, rodResp);

        if (!keepAchromatic || reuseAchromatic)
            continue;
        if (reduceLuminance) {
            const double lRespReduced = pnLookupValue(pn.coneY, pn.coneJnd,
                std::max((workX[i] * spectralContext.nativeToLmsr[0][0] +
                          workY[i] * spectralContext.nativeToLmsr[1][0] +
                          workZ[i] * spectralContext.nativeToLmsr[2][0]) * lumReduction, 1e-8));
            const double mRespReduced = pnLookupValue(pn.coneY, pn.coneJnd,
                std::max((workX[i] * spectralContext.nativeToLmsr[0][1] +
                          workY[i] * spectralContext.nativeToLmsr[1][1] +
                          workZ[i] * spectralContext.nativeToLmsr[2][1]) * lumReduction, 1e-8));
            const double rRespReduced = pnLookupValue(pn.rodY, pn.rodJnd,
                std::max((workX[i] * spectralContext.nativeToLmsr[0][3] +
                          workY[i] * spectralContext.nativeToLmsr[1][3] +
                          workZ[i] * spectralContext.nativeToLmsr[2][3]) * lumReduction, 1e-8));
            prepared.achromatic[i] = lRespReduced + mRespReduced + rRespReduced;
        } else {
            const double pL = pnLookupValue(pn.coneY, pn.coneJnd, std::max(lResp, 1e-8));
            const double pM = pnLookupValue(pn.coneY, pn.coneJnd, std::max(mResp, 1e-8));
            const double pR = pnLookupValue(pn.rodY, pn.rodJnd, std::max(rodResp, 1e-8));
            prepared.achromatic[i] = pL + pM + pR;
        }
    }
    if (keepAdaptation)
        prepared.adaptation = std::move(adaptation);
    if (keepNativeRgb) {
        prepared.nativeRgb.resize(pixelCount * 3, 0.0);
        for (size_t i = 0; i < pixelCount; ++i) {
            prepared.nativeRgb[i * 3 + 0] = workX[i];
            prepared.nativeRgb[i * 3 + 1] = workY[i];
            prepared.nativeRgb[i * 3 + 2] = workZ[i];
        }
    }
    return prepared;
}

std::string perceptualMapKindLabel(PerceptualMapKind kind) {
    switch (kind) {
        case PerceptualMapKind::L: return "L";
        case PerceptualMapKind::M: return "M";
        case PerceptualMapKind::Rod: return "Rod";
        case PerceptualMapKind::LPlusM: return "L+M";
        case PerceptualMapKind::Adaptation: return "Adaptation";
        case PerceptualMapKind::DetectableContrast: return "Detectable contrast";
        case PerceptualMapKind::EqvLuminance: return "Eqv_Luminance";
        default: return "PerceptualMap";
    }
}

// The prepared vectors analyzePerceptualMap() reads for `kind` (see perceptualMapValues()).
unsigned perceptualMapPreparedFields(PerceptualMapKind kind) {
    switch (kind) {
        case PerceptualMapKind::L: return kPreparedLResponse;
        case PerceptualMapKind::M: return kPreparedMResponse;
        case PerceptualMapKind::Rod: return kPreparedRodResponse;
        case PerceptualMapKind::LPlusM: return kPreparedLuminance;
        case PerceptualMapKind::Adaptation: return kPreparedAdaptation;
        case PerceptualMapKind::DetectableContrast: return kPreparedAchromatic | kPreparedAdaptation;
        default: return kPreparedPerceptualFields;
    }
}

const std::vector<double> &perceptualMapValues(
    const NativeHdrvdpPreparedImage &prepared,
    PerceptualMapKind kind
) {
    switch (kind) {
        case PerceptualMapKind::L: return prepared.lResponse;
        case PerceptualMapKind::M: return prepared.mResponse;
        case PerceptualMapKind::Rod: return prepared.rodResponse;
        case PerceptualMapKind::LPlusM: return prepared.luminance;
        case PerceptualMapKind::Adaptation: return prepared.adaptation;
        default: return prepared.luminance;
    }
}

// Prepared images are cached per picture and settings. `requiredFields` (kPrepared* bits) are
// the vectors the caller reads; when the cached entry lacks some of them (or there is none) the
// entry is recomputed with requiredFields | computeFields | the fields it already had, and
// replaces the old entry.
const NativeHdrvdpPreparedImage &getPreparedNativeHdrvdpImageCached(
    const std::string &imagePath,
    const std::string &anchorPath,
    const std::string &spectralEmissionPath,
    double sensitivityCorrection,
    double pixelsPerDegree,
    HdrvdpPreparedImageMode mode,
    unsigned requiredFields,
    unsigned computeFields,
    bool addHalfStep = true,
    bool flipY = true,
    ViewVisibilitySummaryOptions::InputColor inputColor =
        ViewVisibilitySummaryOptions::InputColor::Rad
) {
    namespace fs = std::filesystem;
    static std::map<std::string, CachedPreparedHdrvdpImageEntry> sCache;

    const fs::file_time_type mtime = fs::last_write_time(fs::path(imagePath));
    const std::string key =
        imagePath +
        "|anchor=" + anchorPath +
        "|emission=" + spectralEmissionPath +
        "|sens=" + formatCacheDouble(sensitivityCorrection) +
        "|ppd=" + formatCacheDouble(pixelsPerDegree) +
        "|mode=" + std::string(mode == HdrvdpPreparedImageMode::NativeTransformed ? "native" : "direct") +
        "|input_color=" + std::string(viewVisibilityInputColorLabel(inputColor)) +
        "|halfstep=" + std::string(addHalfStep ? "1" : "0") +
        "|flipy=" + std::string(flipY ? "1" : "0");

    auto it = sCache.find(key);
    const bool current = it != sCache.end() && it->second.mtime == mtime;
    if (!current || (it->second.prepared.fields & requiredFields) != requiredFields) {
        const unsigned fields = requiredFields | computeFields | (current ? it->second.prepared.fields : 0u);
        // The old entry is replaced: take it out of the cache before computing the new one.
        NativeHdrvdpPreparedImage previous;
        if (current)
            previous = std::move(it->second.prepared);
        if (it != sCache.end())
            sCache.erase(it);
        CachedPreparedHdrvdpImageEntry entry;
        entry.mtime = mtime;
        const HdrImage &image = readRadianceHDRCached(imagePath, addHalfStep, flipY);
        // A current entry is completed with the spectral context its vectors were computed with
        // (the former all-fields entry would have been reused as is); otherwise it is resolved
        // as before.
        const NativeHdrvdpParams params = makeNativeHdrvdpParams();
        const NativeHdrvdpSpectralContext &spectralContext = current
            ? *previous.spectralContext
            : getNativeHdrvdpSpectralContextCached(anchorPath, spectralEmissionPath, params);
        entry.prepared = prepareNativeHdrvdpImage(
            image,
            spectralContext,
            sensitivityCorrection,
            pixelsPerDegree,
            mode,
            inputColor,
            fields,
            current ? &previous : nullptr
        );
        previous = NativeHdrvdpPreparedImage();
        it = sCache.insert_or_assign(key, std::move(entry)).first;
    }
    return it->second.prepared;
}

VisibilityPyramid buildVisibilityPyramid(
    const std::vector<double> &signal,
    size_t width,
    size_t height,
    double pixelsPerDegree
) {
    VisibilityPyramid pyramid;
    if (signal.empty() || width == 0 || height == 0)
        return pyramid;

    const auto &lo0filt = sp0Lo0Filter();
    const auto &hi0filt = sp0Hi0Filter();
    const auto &lofilt = sp0LoFilter();
    const auto &bfilt = sp0BandFilter();

    const int maxHeight = maxPyrHeight(height, width, 13, 13);
    const int pyrHeight = std::max(0, std::min(static_cast<int>(std::ceil(std::log2(std::max(1.0, pixelsPerDegree)))) - 2, maxHeight));

    VisibilityPyramidLevel hi0;
    hi0.width = width;
    hi0.height = height;
    hi0.frequencyCpd = pixelsPerDegree / 2.0;
    hi0.band = correlateReflect1(signal, width, height, hi0filt, 9, 9);
    pyramid.levels.push_back(std::move(hi0));

    std::vector<double> lowpass = correlateReflect1(signal, width, height, lo0filt, 7, 7);
    size_t currentWidth = width;
    size_t currentHeight = height;
    double currentFreq = pixelsPerDegree / 4.0;

    for (int level = 0; level < pyrHeight; ++level) {
        VisibilityPyramidLevel bandLevel;
        bandLevel.width = currentWidth;
        bandLevel.height = currentHeight;
        bandLevel.frequencyCpd = currentFreq;
        bandLevel.band = correlateReflect1(lowpass, currentWidth, currentHeight, bfilt, 9, 9);
        pyramid.levels.push_back(std::move(bandLevel));

        const std::vector<double> nextLowpass = correlateReflect1(lowpass, currentWidth, currentHeight, lofilt, 13, 13, 2, 2, 0, 0);
        lowpass = nextLowpass;
        currentWidth = (currentWidth + 1) / 2;
        currentHeight = (currentHeight + 1) / 2;
        currentFreq *= 0.5;
    }

    VisibilityPyramidLevel baseLevel;
    baseLevel.width = currentWidth;
    baseLevel.height = currentHeight;
    baseLevel.frequencyCpd = currentFreq;
    baseLevel.band = lowpass;
    pyramid.levels.push_back(std::move(baseLevel));

    pyramid.baseWidth = currentWidth;
    pyramid.baseHeight = currentHeight;
    return pyramid;
}

std::vector<double> reconstructVisibilityPyramidResponse(
    const std::vector<VisibilityPyramidLevel> &levels,
    size_t baseWidth,
    size_t baseHeight,
    size_t outputWidth,
    size_t outputHeight
) {
    if (levels.empty())
        return std::vector<double>(outputWidth * outputHeight, 0.0);

    const auto &lo0filt = sp0Lo0Filter();
    const auto &hi0filt = sp0Hi0Filter();
    const auto &lofilt = sp0LoFilter();
    const auto &bfilt = sp0BandFilter();

    std::vector<double> current = levels.back().band;
    size_t currentWidth = levels.back().width;
    size_t currentHeight = levels.back().height;

    for (size_t idx = levels.size() - 1; idx-- > 1;) {
        const VisibilityPyramidLevel &level = levels[idx];
        current = upsampleAndCorrelateReflect1(
            current, currentWidth, currentHeight,
            lofilt, 13, 13,
            level.width, level.height,
            2, 2, 0, 0
        );
        const std::vector<double> filteredBand = upsampleAndCorrelateReflect1(
            level.band, level.width, level.height,
            bfilt, 9, 9,
            level.width, level.height
        );
        for (size_t i = 0; i < current.size(); ++i)
            current[i] += filteredBand[i];
        currentWidth = level.width;
        currentHeight = level.height;
    }

    std::vector<double> reconstructed = upsampleAndCorrelateReflect1(
        current, currentWidth, currentHeight,
        lo0filt, 7, 7,
        outputWidth, outputHeight
    );
    const std::vector<double> highpass = upsampleAndCorrelateReflect1(
        levels.front().band, levels.front().width, levels.front().height,
        hi0filt, 9, 9,
        outputWidth, outputHeight
    );
    for (size_t i = 0; i < reconstructed.size(); ++i)
        reconstructed[i] += highpass[i];
    return reconstructed;
}

double linearInterpolate(
    const std::array<double, 7> &xs,
    const std::array<double, 7> &ys,
    double x
) {
    if (x <= xs.front())
        return ys.front();
    if (x >= xs.back())
        return ys.back();
    for (size_t i = 1; i < xs.size(); ++i) {
        if (x <= xs[i]) {
            const double span = std::max(xs[i] - xs[i - 1], 1e-12);
            const double t = (x - xs[i - 1]) / span;
            return ys[i - 1] * (1.0 - t) + ys[i] * t;
        }
    }
    return ys.back();
}

double signPow(double x, double e) {
    return (x < 0.0 ? -1.0 : 1.0) * std::pow(std::abs(x), e);
}

double hdrvdpMtf(double rho, const NativeHdrvdpParams &params) {
    const double freq = std::max(0.0, rho);
    double mtf = 0.0;
    for (size_t i = 0; i < params.mtfA.size(); ++i)
        mtf += params.mtfA[i] * std::exp(-params.mtfB[i] * freq);
    return std::max(mtf, 1e-6);
}

double hdrvdpNcsf(double rho, double luminance, const NativeHdrvdpParams &params, double sensitivityCorrection) {
    (void)sensitivityCorrection;
    const double logLum = std::log10(std::max(luminance, params.csfLums.front()));
    std::array<double, 7> logLums{};
    for (size_t i = 0; i < params.csfLums.size(); ++i)
        logLums[i] = std::log10(params.csfLums[i]);

    std::array<double, 7> p1{};
    std::array<double, 7> p2{};
    std::array<double, 7> p3{};
    std::array<double, 7> p4{};
    for (size_t i = 0; i < params.csfParams.size(); ++i) {
        p1[i] = params.csfParams[i][1];
        p2[i] = params.csfParams[i][2];
        p3[i] = params.csfParams[i][3];
        p4[i] = params.csfParams[i][4];
    }

    const double par1 = linearInterpolate(logLums, p1, logLum);
    const double par2 = linearInterpolate(logLums, p2, logLum);
    const double par3 = linearInterpolate(logLums, p3, logLum);
    const double par4 = linearInterpolate(logLums, p4, logLum);
    const double freq = std::max(rho, 1e-6);
    const double termA = 1.0 + std::pow(par1 * freq, par2);
    const double denom = 1.0 - std::exp(-std::pow(freq / 7.0, 2.0));
    const double termB = std::pow(1.0 / std::max(denom, 1e-8), par3);
    double sens = par4 / std::sqrt(termA * termB);
    sens /= hdrvdpMtf(freq, params);
    sens *= hdrvdpAesl(freq, params);
    if (rho <= 1e-4)
        sens = 0.0;
    return std::max(0.0, sens);
}

HdrvdpCsfLookup buildHdrvdpCsfLookup(
    const std::vector<VisibilityPyramidLevel> &levels,
    const NativeHdrvdpParams &params,
    double sensitivityCorrection
) {
    HdrvdpCsfLookup lookup;
    constexpr size_t kSampleCount = 256;
    lookup.csfLa.resize(kSampleCount);
    lookup.csfLogLa.resize(kSampleCount);
    lookup.sensitivities.resize(levels.size());

    for (size_t i = 0; i < kSampleCount; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kSampleCount - 1);
        const double logLa = -5.0 + 10.0 * t;
        lookup.csfLogLa[i] = logLa;
        lookup.csfLa[i] = std::pow(10.0, logLa);
    }

    for (size_t bandIndex = 0; bandIndex < levels.size(); ++bandIndex) {
        lookup.sensitivities[bandIndex].resize(kSampleCount);
        for (size_t i = 0; i < kSampleCount; ++i) {
            lookup.sensitivities[bandIndex][i] = hdrvdpNcsf(
                levels[bandIndex].frequencyCpd,
                lookup.csfLa[i],
                params,
                sensitivityCorrection
            );
        }
    }

    return lookup;
}

const HdrvdpCsfLookup &getHdrvdpCsfLookupCached(
    const std::vector<VisibilityPyramidLevel> &levels,
    const NativeHdrvdpParams &params,
    double sensitivityCorrection
) {
    static std::map<std::string, HdrvdpCsfLookup> sCache;
    std::ostringstream oss;
    oss << "sens=" << formatCacheDouble(sensitivityCorrection);
    for (const auto &level : levels) {
        oss << "|f=" << formatCacheDouble(level.frequencyCpd)
            << "|w=" << level.width
            << "|h=" << level.height;
    }
    const std::string key = oss.str();
    auto it = sCache.find(key);
    if (it == sCache.end())
        it = sCache.emplace(key, buildHdrvdpCsfLookup(levels, params, sensitivityCorrection)).first;
    return it->second;
}

double sampleHdrvdpCsfLookup(
    const HdrvdpCsfLookup &lookup,
    size_t bandIndex,
    double luminance
) {
    if (lookup.csfLa.empty() || bandIndex >= lookup.sensitivities.size())
        return 0.0;
    const double clampedLa = clampd(luminance, lookup.csfLa.front(), lookup.csfLa.back());
    const double logLa = std::log10(std::max(clampedLa, 1e-8));
    return std::max(0.0, interpolateVectorLinear(
        lookup.csfLogLa,
        lookup.sensitivities[bandIndex],
        logLa
    ));
}

double psychometricProbability(double contrast, const NativeHdrvdpParams &params) {
    const double pf = std::pow(10.0, params.psychFuncSlope);
    const double a = std::pow(-std::log(0.5), 1.0 / pf);
    const double scaled = std::abs(a * contrast);
    return 1.0 - std::exp(-std::pow(scaled, pf));
}

std::vector<double> meanFilter3x3(const std::vector<double> &src, size_t width, size_t height) {
    std::vector<double> dst(width * height, 0.0);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            double sum = 0.0;
            for (int oy = -1; oy <= 1; ++oy) {
                const int sy = static_cast<int>(y) + oy;
                if (sy < 0 || sy >= static_cast<int>(height))
                    continue;
                for (int ox = -1; ox <= 1; ++ox) {
                    const int sx = static_cast<int>(x) + ox;
                    if (sx < 0 || sx >= static_cast<int>(width))
                        continue;
                    sum += src[static_cast<size_t>(sy) * width + static_cast<size_t>(sx)];
                }
            }
            dst[y * width + x] = sum / 9.0;
        }
    }
    return dst;
}

std::vector<double> maxFilterApprox(const std::vector<double> &src, size_t width, size_t height, int radius);

std::vector<double> matlabMaskingPool(
    const std::vector<double> &src,
    size_t width,
    size_t height,
    double maskingNorm,
    int poolSize
) {
    if (src.empty() || width == 0 || height == 0 || poolSize <= 1)
        return src;

    const int radius = poolSize / 2;
    const double effectiveNorm = std::pow(10.0, maskingNorm);
    std::vector<double> powered(width * height, 0.0);
    for (size_t i = 0; i < src.size(); ++i)
        powered[i] = std::pow(std::max(src[i], 0.0), effectiveNorm);

    std::vector<double> pooled(width * height, 0.0);
    const double kernelWeight = 1.0 / static_cast<double>(poolSize * poolSize);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            double accum = 0.0;
            for (int ky = -radius; ky <= radius; ++ky) {
                const int sy = static_cast<int>(y) + ky;
                if (sy < 0 || sy >= static_cast<int>(height))
                    continue;
                for (int kx = -radius; kx <= radius; ++kx) {
                    const int sx = static_cast<int>(x) + kx;
                    if (sx < 0 || sx >= static_cast<int>(width))
                        continue;
                    accum += powered[static_cast<size_t>(sy) * width + static_cast<size_t>(sx)] * kernelWeight;
                }
            }
            pooled[y * width + x] = std::pow(std::max(accum, 0.0), 1.0 / effectiveNorm);
        }
    }
    return pooled;
}

std::vector<double> mutualMaskingMatlab(
    const std::vector<double> &testBand,
    const std::vector<double> &referenceBand,
    size_t width,
    size_t height,
    const NativeHdrvdpParams &params
) {
    std::vector<double> masking(width * height, 0.0);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(masking.size()); ++i)
        masking[i] = std::min(std::abs(testBand[i]), std::abs(referenceBand[i]));

    if (params.doSiGauss) {
        return fastGaussLikeMatlab(
            masking, width, height, std::pow(10.0, params.siSize), false, false, false, 0.0);
    }

    if (params.doPuDilate) {
        return maxFilterApprox(masking, width, height, params.puDilate);
    }

    return matlabMaskingPool(masking, width, height, params.maskingNorm, params.maskingPoolSize);
}

std::vector<double> hdrvdpLocalAdaptMap(
    const std::vector<double> &luminance,
    size_t width,
    size_t height,
    double pixelsPerDegree
) {
    const double sigma = std::pow(10.0, -0.781367) * pixelsPerDegree;
    std::vector<double> logLum(width * height, 0.0);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(logLum.size()); ++i)
        logLum[i] = std::log(std::max(luminance[i], 1e-6));
    std::vector<double> blurred = fastGaussLikeMatlab(
        logLum,
        width,
        height,
        std::max(0.6, sigma),
        true,
        false,
        true,
        0.0);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(blurred.size()); ++i)
        blurred[i] = std::exp(blurred[i]);
    return blurred;
}

std::vector<double> maxFilterApprox(const std::vector<double> &src, size_t width, size_t height, int radius) {
    if (radius <= 0)
        return src;
    std::vector<double> dst(width * height, 0.0);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long y = 0; y < static_cast<long long>(height); ++y) {
        for (size_t x = 0; x < width; ++x) {
            double m = 0.0;
            for (int oy = -radius; oy <= radius; ++oy) {
                const int sy = std::max(0, std::min(static_cast<int>(height) - 1, static_cast<int>(y) + oy));
                for (int ox = -radius; ox <= radius; ++ox) {
                    const int sx = std::max(0, std::min(static_cast<int>(width) - 1, static_cast<int>(x) + ox));
                    m = std::max(m, src[static_cast<size_t>(sy) * width + static_cast<size_t>(sx)]);
                }
            }
            dst[y * width + x] = m;
        }
    }
    return dst;
}

double minkowskiSum(const std::vector<double> &values, double p) {
    if (values.empty())
        return 0.0;
    double accum = 0.0;
    for (double value : values)
        accum += std::pow(std::abs(value), p);
    return std::pow(accum / static_cast<double>(values.size()), 1.0 / p);
}

double meanOf(const std::vector<double> &values) {
    if (values.empty())
        return 0.0;
    double sum = 0.0;
    for (double v : values)
        sum += v;
    return sum / static_cast<double>(values.size());
}

std::vector<double> extractChannel(const std::vector<double> &rgb, int channel) {
    if (channel < 0 || channel > 2 || rgb.empty())
        return {};
    std::vector<double> out(rgb.size() / 3, 0.0);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = rgb[i * 3 + static_cast<size_t>(channel)];
    return out;
}

std::vector<VisibilityPyramidLevel> makeLookupLevelsFromExact(const ExactVisibilityPyramid &pyramid) {
    std::vector<VisibilityPyramidLevel> levels;
    levels.reserve(pyramid.levels.size());
    for (const ExactVisibilityPyramidLevel &level : pyramid.levels) {
        VisibilityPyramidLevel converted;
        converted.width = level.width;
        converted.height = level.height;
        converted.frequencyCpd = level.frequencyCpd;
        levels.push_back(std::move(converted));
    }
    return levels;
}

struct HdrvdpSubsetSideBySideResult {
    std::vector<double> undetectMap;
    std::vector<double> contrastMap;
    std::vector<double> probabilityMap;
    double qualityJod = 10.0;
};

void writeGrayMapRaw(
    const std::string &path,
    size_t width,
    size_t height,
    const std::vector<double> &values);

std::vector<double> computeHdrvdpExactContrastMap(
    const std::vector<double> &achromatic,
    const std::vector<double> &adaptMap,
    size_t width,
    size_t height,
    double pixelsPerDegree,
    double sensitivityCorrection
) {
    const NativeHdrvdpParams params = makeNativeHdrvdpParams();
    ExactVisibilityPyramid pyramid = buildExactVisibilityPyramid(achromatic, width, height, pixelsPerDegree);
    removeBaseBandDc(pyramid);
    const std::vector<VisibilityPyramidLevel> lookupLevels = makeLookupLevelsFromExact(pyramid);
    const HdrvdpCsfLookup &csfLookup = getHdrvdpCsfLookupCached(lookupLevels, params, sensitivityCorrection);
    ExactVisibilityPyramid responsePyramid = pyramid;
    const double eps = 1e-8;
    const size_t bandCount = responsePyramid.levels.size();

    for (size_t levelIndex = 0; levelIndex < bandCount; ++levelIndex) {
        const ExactVisibilityPyramidLevel &level = pyramid.levels[levelIndex];
        if (levelIndex == bandCount - 1 || level.frequencyCpd <= 2.0) {
            for (std::vector<double> &band : responsePyramid.levels[levelIndex].bands)
                std::fill(band.begin(), band.end(), 0.0);
            continue;
        }

        const double bandNorm = std::pow(2.0, static_cast<double>(levelIndex));
        const std::vector<double> adaptResized = resizeMatlabBicubic(adaptMap, width, height, level.width, level.height);
        for (size_t orientation = 0; orientation < level.bands.size(); ++orientation) {
            std::vector<double> response(level.width * level.height, 0.0);
            #if defined(_OPENMP)
            #pragma omp parallel for schedule(static)
            #endif
            for (long long i = 0; i < static_cast<long long>(response.size()); ++i) {
                const double adapt = std::max(eps, adaptResized[static_cast<size_t>(i)]);
                const double csf = sampleHdrvdpCsfLookup(csfLookup, levelIndex, adapt);
                const double bandValue = level.bands[orientation][static_cast<size_t>(i)] / std::max(bandNorm, 1.0);
                const double visibleProbability = psychometricProbability(bandValue * csf, params);
                response[static_cast<size_t>(i)] = std::log(1.0 - visibleProbability + 1e-8) * bandNorm;
            }
            responsePyramid.levels[levelIndex].bands[orientation] = std::move(response);
        }
    }

    std::vector<double> reconstructed = reconstructExactVisibilityPyramidResponse(responsePyramid, width, height);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(reconstructed.size()); ++i)
        reconstructed[static_cast<size_t>(i)] = std::max(0.0, std::min(1.0, 1.0 - std::exp(-std::abs(reconstructed[static_cast<size_t>(i)]))));
    return reconstructed;
}

HdrvdpSubsetSideBySideResult computeHdrvdpExactSideBySide(
    const NativeHdrvdpPreparedImage &reference,
    const NativeHdrvdpPreparedImage &test,
    const std::vector<double> &maskValues,
    const std::vector<double> &diffMask,
    size_t width,
    size_t height,
    double pixelsPerDegree,
    double sensitivityCorrection
) {
    const NativeHdrvdpParams params = makeNativeHdrvdpParams();
    ExactVisibilityPyramid refPyramid = buildExactVisibilityPyramid(reference.achromatic, width, height, pixelsPerDegree);
    ExactVisibilityPyramid testPyramid = buildExactVisibilityPyramid(test.achromatic, width, height, pixelsPerDegree);
    removeBaseBandDc(refPyramid);
    removeBaseBandDc(testPyramid);
    const std::vector<VisibilityPyramidLevel> lookupLevels = makeLookupLevelsFromExact(refPyramid);
    const HdrvdpCsfLookup &csfLookup = getHdrvdpCsfLookupCached(lookupLevels, params, sensitivityCorrection);
    const size_t bandCount = std::min(refPyramid.levels.size(), testPyramid.levels.size());

    ExactVisibilityPyramid dPyramid = refPyramid;
    double qErr = 0.0;
    const double eps = 1e-6;
    const double p = std::pow(10.0, params.maskP);
    const double maskQPower = std::pow(10.0, params.maskQ);
    const double pf = std::pow(10.0, params.psychFuncSlope) / p;
    const double kMaskSelf = std::pow(10.0, params.maskSelf);
    const double kMaskXo = std::pow(10.0, params.maskXo);
    const double kMaskXn = std::pow(10.0, params.maskXn);
    size_t usedBands = 0;

    std::vector<std::vector<std::vector<double> > > selfMasks(bandCount);
    std::vector<std::vector<double> > maskXoSums(bandCount);

    for (size_t levelIndex = 0; levelIndex < bandCount; ++levelIndex) {
        const ExactVisibilityPyramidLevel &refLevel = refPyramid.levels[levelIndex];
        const ExactVisibilityPyramidLevel &testLevel = testPyramid.levels[levelIndex];
        const double bandNorm = std::pow(2.0, static_cast<double>(levelIndex));
        const size_t orientationCount = std::min(refLevel.bands.size(), testLevel.bands.size());
        selfMasks[levelIndex].resize(orientationCount);
        maskXoSums[levelIndex].assign(refLevel.width * refLevel.height, 0.0);

        for (size_t orientation = 0; orientation < orientationCount; ++orientation) {
            std::vector<double> normalizedTest(refLevel.width * refLevel.height, 0.0);
            std::vector<double> normalizedRef(refLevel.width * refLevel.height, 0.0);
            #if defined(_OPENMP)
            #pragma omp parallel for schedule(static)
            #endif
            for (long long i = 0; i < static_cast<long long>(normalizedTest.size()); ++i) {
                normalizedTest[static_cast<size_t>(i)] = testLevel.bands[orientation][static_cast<size_t>(i)] / std::max(bandNorm, 1.0);
                normalizedRef[static_cast<size_t>(i)] = refLevel.bands[orientation][static_cast<size_t>(i)] / std::max(bandNorm, 1.0);
            }
            selfMasks[levelIndex][orientation] = mutualMaskingMatlab(
                normalizedTest, normalizedRef, refLevel.width, refLevel.height, params);
            if (orientationCount > 1) {
                for (size_t i = 0; i < maskXoSums[levelIndex].size(); ++i)
                    maskXoSums[levelIndex][i] += selfMasks[levelIndex][orientation][i];
            }
        }
    }

    for (size_t levelIndex = 0; levelIndex < bandCount; ++levelIndex) {
        const ExactVisibilityPyramidLevel &refLevel = refPyramid.levels[levelIndex];
        const ExactVisibilityPyramidLevel &testLevel = testPyramid.levels[levelIndex];
        if (params.ignoreFreqsLowerThan > 0.0 && refLevel.frequencyCpd < params.ignoreFreqsLowerThan) {
            for (std::vector<double> &band : dPyramid.levels[levelIndex].bands)
                std::fill(band.begin(), band.end(), 0.0);
            continue;
        }
        ++usedBands;
        const double bandNorm = std::pow(2.0, static_cast<double>(levelIndex));
        const std::vector<double> refAdaptResized = resizeMatlabBicubic(reference.adaptation, width, height, refLevel.width, refLevel.height);
        const std::vector<double> testAdaptResized = resizeMatlabBicubic(test.adaptation, width, height, testLevel.width, testLevel.height);
        std::vector<double> meanAdaptResized(refLevel.width * refLevel.height, 0.0);
        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(meanAdaptResized.size()); ++i) {
            const size_t idx = static_cast<size_t>(i);
            const double refAdapt = std::max(refAdaptResized[idx], eps);
            const double testAdapt = std::max(testAdaptResized[idx], eps);
            meanAdaptResized[idx] = std::pow(10.0, 0.5 * (std::log10(refAdapt) + std::log10(testAdapt)));
        }
        const double baseBandMeanAdapt = std::max(geoMeanPositive(meanAdaptResized), eps);

        const std::vector<double> diffMaskBand = resizeMatlabBicubic(diffMask, width, height, refLevel.width, refLevel.height);
        const std::vector<double> maskBand = resizeMatlabBicubic(maskValues, width, height, refLevel.width, refLevel.height);
        const size_t orientationCount = std::min(refLevel.bands.size(), testLevel.bands.size());

        for (size_t orientation = 0; orientation < orientationCount; ++orientation) {
            std::vector<double> dBand(refLevel.width * refLevel.height, 0.0);
            std::vector<double> dForQ(refLevel.width * refLevel.height, 0.0);
            std::vector<double> maskXn(refLevel.width * refLevel.height, 0.0);

            if (levelIndex > 0) {
                const size_t prevOrientation = std::min(orientation, selfMasks[levelIndex - 1].size() - 1);
                const std::vector<double> prevMask = resizeMatlabBicubic(
                    selfMasks[levelIndex - 1][prevOrientation],
                    refPyramid.levels[levelIndex - 1].width,
                    refPyramid.levels[levelIndex - 1].height,
                    refLevel.width,
                    refLevel.height);
                #if defined(_OPENMP)
                #pragma omp parallel for schedule(static)
                #endif
                for (long long i = 0; i < static_cast<long long>(maskXn.size()); ++i)
                    maskXn[static_cast<size_t>(i)] += std::max(0.0, prevMask[static_cast<size_t>(i)]);
            }
            if (levelIndex + 1 < bandCount - 1) {
                const size_t nextOrientation = std::min(orientation, selfMasks[levelIndex + 1].size() - 1);
                const std::vector<double> nextMask = resizeMatlabBicubic(
                    selfMasks[levelIndex + 1][nextOrientation],
                    refPyramid.levels[levelIndex + 1].width,
                    refPyramid.levels[levelIndex + 1].height,
                    refLevel.width,
                    refLevel.height);
                #if defined(_OPENMP)
                #pragma omp parallel for schedule(static)
                #endif
                for (long long i = 0; i < static_cast<long long>(maskXn.size()); ++i)
                    maskXn[static_cast<size_t>(i)] += std::max(0.0, nextMask[static_cast<size_t>(i)]);
            }

            double baseBandFreq = refLevel.frequencyCpd;
            if (levelIndex == bandCount - 1) {
                std::vector<double> bandDiff(refLevel.width * refLevel.height, 0.0);
                #if defined(_OPENMP)
                #pragma omp parallel for schedule(static)
                #endif
                for (long long i = 0; i < static_cast<long long>(bandDiff.size()); ++i) {
                    const double testBand = testLevel.bands[orientation][static_cast<size_t>(i)] / std::max(bandNorm, 1.0);
                    const double refBand = refLevel.bands[orientation][static_cast<size_t>(i)] / std::max(bandNorm, 1.0);
                    bandDiff[static_cast<size_t>(i)] = testBand - refBand;
                }
                baseBandFreq = std::max(
                    dominantFrequencyCpdFromBand(bandDiff, refLevel.width, refLevel.height, refLevel.frequencyCpd * 4.0),
                    1e-4);
            }

            const bool doMaskXo = orientationCount > 1;
            const std::vector<double> &selfMask = selfMasks[levelIndex][orientation];
            #if defined(_OPENMP)
            #pragma omp parallel for schedule(static)
            #endif
            for (long long i = 0; i < static_cast<long long>(dBand.size()); ++i) {
                const size_t idx = static_cast<size_t>(i);
                const double testBand = testLevel.bands[orientation][idx] / std::max(bandNorm, 1.0);
                const double refBand = refLevel.bands[orientation][idx] / std::max(bandNorm, 1.0);
                double nNcsf = 0.0;
                if (levelIndex == bandCount - 1) {
                    const double csf = hdrvdpNcsf(baseBandFreq, baseBandMeanAdapt, params, sensitivityCorrection);
                    nNcsf = 1.0 / std::max(csf, 1e-6);
                } else {
                    const double csf = sampleHdrvdpCsfLookup(csfLookup, levelIndex, std::max(eps, meanAdaptResized[idx]));
                    nNcsf = 1.0 / std::max(csf, 1e-6);
                }
                const double exDiff = signPow(testBand - refBand, p);
                const double bandMaskXo = doMaskXo ? std::max(maskXoSums[levelIndex][idx] - selfMask[idx], 0.0) : 0.0;
                const double nMask =
                    kMaskSelf * std::pow(std::abs(selfMask[idx]), maskQPower) +
                    kMaskXo * std::pow(std::abs(bandMaskXo), maskQPower) +
                    kMaskXn * std::pow(std::abs(maskXn[idx]), maskQPower);
                const double denom = std::sqrt(std::pow(nNcsf, 2.0 * p) + nMask * nMask);
                const double d = exDiff / std::max(denom, 1e-8);
                dBand[idx] = signPow(d, pf) * bandNorm;
                dForQ[idx] = d * diffMaskBand[idx];
            }
            dPyramid.levels[levelIndex].bands[orientation] = std::move(dBand);
            qErr += minkowskiSum(dForQ, 0.8) / static_cast<double>(std::max<size_t>(1, bandCount));
        }
    }

    HdrvdpSubsetSideBySideResult result;
    result.undetectMap.resize(width * height, 0.0);
    std::vector<double> reconstructed = reconstructExactVisibilityPyramidResponse(dPyramid, width, height);
    std::vector<double> sMap(width * height, 0.0);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(reconstructed.size()); ++i)
        sMap[static_cast<size_t>(i)] = std::abs(reconstructed[static_cast<size_t>(i)]);
    if (usedBands > 0) {
        const double sigma = std::pow(10.0, params.siSigma) * pixelsPerDegree;
        std::vector<double> logUndetect(width * height, 0.0);
        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(sMap.size()); ++i) {
            const double pMap = 1.0 - std::exp(std::log(0.5) * sMap[static_cast<size_t>(i)]);
            logUndetect[static_cast<size_t>(i)] = std::log(std::max(1e-4, 1.0 - pMap));
        }
        const std::vector<double> pooled = fastGaussLikeMatlab(
            logUndetect, width, height, std::max(0.6, sigma), true, false, false, 0.0);
        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(sMap.size()); ++i)
            sMap[static_cast<size_t>(i)] = 1.0 - std::exp(pooled[static_cast<size_t>(i)]);
    } else {
        std::fill(sMap.begin(), sMap.end(), 0.0);
    }
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(sMap.size()); ++i) {
        const double pDetect = std::max(0.0, std::min(1.0, sMap[static_cast<size_t>(i)]));
        result.undetectMap[static_cast<size_t>(i)] = std::max(0.0, std::min(1.0, 1.0 - pDetect));
    }

    const double qualityFloor = -4.0;
    const double maxQ = 10.0;
    const double qValue = maxQ + qualityFloor - std::log(std::exp(qualityFloor) + qErr);
    const double qualityClamped = std::max(0.0, std::min(10.0, qValue));
    result.qualityJod = 10.0 - 0.5200 * std::pow(std::max(10.0 - qualityClamped, 0.0), 1.2812);
    result.qualityJod = std::max(0.0, std::min(10.0, result.qualityJod));
    return result;
}

std::vector<double> computeHdrvdpSubsetContrastMap(
    const std::vector<double> &achromatic,
    const std::vector<double> &adaptMap,
    size_t width,
    size_t height,
    double pixelsPerDegree,
    double sensitivityCorrection
) {
    const NativeHdrvdpParams params = makeNativeHdrvdpParams();
    VisibilityPyramid pyramid = buildVisibilityPyramid(achromatic, width, height, pixelsPerDegree);
    removeBaseBandDc(pyramid);
    const HdrvdpCsfLookup &csfLookup = getHdrvdpCsfLookupCached(pyramid.levels, params, sensitivityCorrection);
    // The responses overwrite the band coefficients in place (coefficient i is only read by
    // response i), instead of filling a full copy of the pyramid.
    std::vector<VisibilityPyramidLevel> &responseLevels = pyramid.levels;
    const double eps = 1e-8;

    const size_t bandCount = responseLevels.size();
    for (size_t levelIndex = 0; levelIndex < bandCount; ++levelIndex) {
        VisibilityPyramidLevel &level = responseLevels[levelIndex];
        if (levelIndex == bandCount - 1 || level.frequencyCpd <= 2.0) {
            std::fill(level.band.begin(), level.band.end(), 0.0);
            continue;
        }
        const double bandNorm = std::pow(2.0, static_cast<double>(levelIndex));
        const std::vector<double> adaptResized = resizeMatlabBicubic(adaptMap, width, height, level.width, level.height);

        std::vector<double> &response = level.band;
        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(level.width * level.height); ++i) {
            const double adapt = std::max(eps, adaptResized[i]);
            const double csf = sampleHdrvdpCsfLookup(csfLookup, levelIndex, adapt);
            const double bandValue = level.band[i] / std::max(bandNorm, 1.0);
            const double visibleProbability = psychometricProbability(bandValue * csf, params);
            response[i] = std::log(1.0 - visibleProbability + 1e-8) * bandNorm;
        }
    }

    std::vector<double> reconstructed = reconstructVisibilityPyramidResponse(
        responseLevels,
        pyramid.baseWidth,
        pyramid.baseHeight,
        width,
        height
    );
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(reconstructed.size()); ++i)
        reconstructed[i] = std::max(0.0, std::min(1.0, 1.0 - std::exp(-std::abs(reconstructed[i]))));
    return reconstructed;
}

HdrvdpSubsetSideBySideResult computeHdrvdpSubsetSideBySide(
    const NativeHdrvdpPreparedImage &reference,
    const NativeHdrvdpPreparedImage &test,
    std::vector<double> diffMask,
    size_t width,
    size_t height,
    double pixelsPerDegree,
    double sensitivityCorrection,
    const std::string *debugOutDir = nullptr,
    bool keepDetailMaps = false
) {
    // keepDetailMaps: also fill result.contrastMap and result.probabilityMap (read only by the
    // debug dump); otherwise they stay empty and only undetectMap and qualityJod are produced.
    // diffMask is taken by value so that it can be released after the band loop.
    const NativeHdrvdpParams params = makeNativeHdrvdpParams();
    VisibilityPyramid refPyramid = buildVisibilityPyramid(reference.achromatic, width, height, pixelsPerDegree);
    VisibilityPyramid testPyramid = buildVisibilityPyramid(test.achromatic, width, height, pixelsPerDegree);
    removeBaseBandDc(refPyramid);
    removeBaseBandDc(testPyramid);
    const HdrvdpCsfLookup &csfLookup = getHdrvdpCsfLookupCached(refPyramid.levels, params, sensitivityCorrection);
    const size_t levelCount = std::min(refPyramid.levels.size(), testPyramid.levels.size());
    const double eps = 1e-6;
    std::vector<double> logMeanAdaptFull(width * height, 0.0);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(logMeanAdaptFull.size()); ++i) {
        const double meanAdapt = 0.5 * (reference.adaptation[static_cast<size_t>(i)] + test.adaptation[static_cast<size_t>(i)]);
        const double clamped = clampd(std::max(meanAdapt, eps), csfLookup.csfLa.front(), csfLookup.csfLa.back());
        logMeanAdaptFull[static_cast<size_t>(i)] = std::log10(clamped);
    }

    // Same levels as the reference pyramid; every band of the first levelCount levels is assigned
    // in the level loop below (the reference coefficients a copy would hold are never read).
    std::vector<VisibilityPyramidLevel> dLevels(refPyramid.levels.size());
    for (size_t levelIndex = 0; levelIndex < dLevels.size(); ++levelIndex) {
        dLevels[levelIndex].width = refPyramid.levels[levelIndex].width;
        dLevels[levelIndex].height = refPyramid.levels[levelIndex].height;
        dLevels[levelIndex].frequencyCpd = refPyramid.levels[levelIndex].frequencyCpd;
        if (levelIndex >= levelCount)
            dLevels[levelIndex].band = refPyramid.levels[levelIndex].band;
    }
    double qErr = 0.0;
    const double p = std::pow(10.0, params.maskP);
    const double maskQPower = std::pow(10.0, params.maskQ);
    const double pf = std::pow(10.0, params.psychFuncSlope) / p;
    const double kMaskSelf = std::pow(10.0, params.maskSelf);
    const double kMaskXn = std::pow(10.0, params.maskXn);
    size_t usedBands = 0;
    std::vector<std::vector<double> > selfMasks(levelCount);
    std::vector<double> previousDForQ;

    #if defined(_OPENMP)
    #pragma omp parallel for schedule(dynamic, 1)
    #endif
    for (long long levelIndexSigned = 0; levelIndexSigned < static_cast<long long>(levelCount); ++levelIndexSigned) {
        const size_t levelIndex = static_cast<size_t>(levelIndexSigned);
        const VisibilityPyramidLevel &refLevel = refPyramid.levels[levelIndex];
        const VisibilityPyramidLevel &testLevel = testPyramid.levels[levelIndex];
        const double bandNorm = std::pow(2.0, static_cast<double>(levelIndex));
        std::vector<double> normalizedTest(refLevel.width * refLevel.height, 0.0);
        std::vector<double> normalizedRef(refLevel.width * refLevel.height, 0.0);
        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(normalizedTest.size()); ++i) {
            normalizedTest[i] = testLevel.band[i] / std::max(bandNorm, 1.0);
            normalizedRef[i] = refLevel.band[i] / std::max(bandNorm, 1.0);
        }
        selfMasks[levelIndex] = mutualMaskingMatlab(
            normalizedTest, normalizedRef, refLevel.width, refLevel.height, params);
    }

    for (size_t levelIndex = 0; levelIndex < levelCount; ++levelIndex) {
        const VisibilityPyramidLevel &refLevel = refPyramid.levels[levelIndex];
        const VisibilityPyramidLevel &testLevel = testPyramid.levels[levelIndex];
        const bool ignoreLowFreqBand = params.ignoreFreqsLowerThan > 0.0 && refLevel.frequencyCpd < params.ignoreFreqsLowerThan;
        std::vector<double> maskXn(refLevel.width * refLevel.height, 0.0);
        if (levelIndex > 0) {
            const std::vector<double> prevMask = resizeMatlabBicubic(
                selfMasks[levelIndex - 1],
                refPyramid.levels[levelIndex - 1].width,
                refPyramid.levels[levelIndex - 1].height,
                refLevel.width,
                refLevel.height);
            #if defined(_OPENMP)
            #pragma omp parallel for schedule(static)
            #endif
            for (long long i = 0; i < static_cast<long long>(maskXn.size()); ++i)
                maskXn[i] += std::max(0.0, prevMask[static_cast<size_t>(i)]);
        }
        if (levelIndex + 1 < levelCount - 1) {
            const std::vector<double> nextMask = resizeMatlabBicubic(
                selfMasks[levelIndex + 1],
                refPyramid.levels[levelIndex + 1].width,
                refPyramid.levels[levelIndex + 1].height,
                refLevel.width,
                refLevel.height);
            #if defined(_OPENMP)
            #pragma omp parallel for schedule(static)
            #endif
            for (long long i = 0; i < static_cast<long long>(maskXn.size()); ++i)
                maskXn[i] += std::max(0.0, nextMask[static_cast<size_t>(i)]);
        }
        std::vector<double> logMeanAdaptResized = resizeMatlabBicubic(logMeanAdaptFull, width, height, refLevel.width, refLevel.height);
        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(logMeanAdaptResized.size()); ++i)
            logMeanAdaptResized[static_cast<size_t>(i)] = clampd(logMeanAdaptResized[static_cast<size_t>(i)], csfLookup.csfLogLa.front(), csfLookup.csfLogLa.back());
        const double baseBandMeanAdapt = std::max(std::pow(10.0, meanOf(logMeanAdaptResized)), eps);
        const std::vector<double> diffMaskBand = resizeMatlabBicubic(diffMask, width, height, refLevel.width, refLevel.height);
        std::vector<double> dForQ(refLevel.width * refLevel.height, 0.0);
        std::vector<double> csfMap;
        std::vector<double> nNcsfMap;
        std::vector<double> nMaskMap;
        std::vector<double> dMap;
        if (debugOutDir && levelIndex < 4) {
            csfMap.assign(refLevel.width * refLevel.height, 0.0);
            nNcsfMap.assign(refLevel.width * refLevel.height, 0.0);
            nMaskMap.assign(refLevel.width * refLevel.height, 0.0);
            dMap.assign(refLevel.width * refLevel.height, 0.0);
        }
        if (ignoreLowFreqBand) {
            dLevels[levelIndex].band.assign(refLevel.band.size(), 0.0);
            if (!previousDForQ.empty()) {
                dForQ = previousDForQ;
                qErr += minkowskiSum(previousDForQ, 0.8) / static_cast<double>(std::max<size_t>(1, levelCount));
            }
            if (debugOutDir && levelIndex < 4) {
                const std::string prefix = "native_side_band" + std::to_string(levelIndex + 1) + "_orient1_";
                writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "logla.bin")).string(), refLevel.width, refLevel.height, logMeanAdaptResized);
                writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "csf.bin")).string(), refLevel.width, refLevel.height, csfMap);
                writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "nncsf.bin")).string(), refLevel.width, refLevel.height, nNcsfMap);
                writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "nmask.bin")).string(), refLevel.width, refLevel.height, nMaskMap);
                writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "d.bin")).string(), refLevel.width, refLevel.height, dMap);
                writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "dp.bin")).string(), refLevel.width, refLevel.height, dForQ);
                writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "diffmask.bin")).string(), refLevel.width, refLevel.height, diffMaskBand);
                writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "test.bin")).string(), refLevel.width, refLevel.height, testLevel.band);
                writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "ref.bin")).string(), refLevel.width, refLevel.height, refLevel.band);
                writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "self.bin")).string(), refLevel.width, refLevel.height, selfMasks[levelIndex]);
                writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "maskxn.bin")).string(), refLevel.width, refLevel.height, maskXn);
                writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "coeff.bin")).string(), refLevel.width, refLevel.height, dLevels[levelIndex].band);
            }
            continue;
        }
        ++usedBands;
        const double bandNorm = std::pow(2.0, static_cast<double>(levelIndex));
        std::vector<double> dBand(refLevel.width * refLevel.height, 0.0);

        double baseBandFreq = refLevel.frequencyCpd;
        if (levelIndex == levelCount - 1) {
            std::vector<double> bandDiff(refLevel.width * refLevel.height, 0.0);
            #if defined(_OPENMP)
            #pragma omp parallel for schedule(static)
            #endif
            for (long long i = 0; i < static_cast<long long>(bandDiff.size()); ++i) {
                const double testBand = testLevel.band[i] / std::max(bandNorm, 1.0);
                const double refBand = refLevel.band[i] / std::max(bandNorm, 1.0);
                bandDiff[i] = testBand - refBand;
            }
            baseBandFreq = std::max(
                dominantFrequencyCpdFromBand(bandDiff, refLevel.width, refLevel.height, refLevel.frequencyCpd * 4.0),
                1e-4);
        }

        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(dBand.size()); ++i) {
            const double testBand = testLevel.band[i] / std::max(bandNorm, 1.0);
            const double refBand = refLevel.band[i] / std::max(bandNorm, 1.0);
            double nNcsf = 0.0;
                if (levelIndex == levelCount - 1) {
                    const double csf = hdrvdpNcsf(baseBandFreq, baseBandMeanAdapt, params, sensitivityCorrection);
                    nNcsf = 1.0 / std::max(csf, 1e-6);
                    if (debugOutDir && levelIndex < 4)
                        csfMap[static_cast<size_t>(i)] = csf;
                } else {
                    const double csf = std::max(0.0, interpolateVectorLinear(
                        csfLookup.csfLogLa,
                        csfLookup.sensitivities[levelIndex],
                        logMeanAdaptResized[i]
                    ));
                    nNcsf = 1.0 / std::max(csf, 1e-6);
                    if (debugOutDir && levelIndex < 4)
                        csfMap[static_cast<size_t>(i)] = csf;
                }
            const double selfMaskValue = std::max(0.0, selfMasks[levelIndex][i]);
            const double exDiff = signPow(testBand - refBand, p);
            const double nMask =
                kMaskSelf * std::pow(std::abs(selfMaskValue), maskQPower) +
                kMaskXn * std::pow(std::abs(maskXn[i]), maskQPower);
            const double denom = std::sqrt(std::pow(nNcsf, 2.0 * p) + nMask * nMask);
            const double d = exDiff / std::max(denom, 1e-8);
            dBand[i] = signPow(d, pf) * bandNorm;
            dForQ[i] = d * diffMaskBand[i];
            if (debugOutDir && levelIndex < 4) {
                nNcsfMap[static_cast<size_t>(i)] = nNcsf;
                nMaskMap[static_cast<size_t>(i)] = nMask;
                dMap[static_cast<size_t>(i)] = d;
            }
        }
        dLevels[levelIndex].band = std::move(dBand);
        if (debugOutDir && levelIndex < 4) {
            const std::string prefix = "native_side_band" + std::to_string(levelIndex + 1) + "_orient1_";
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "logla.bin")).string(), refLevel.width, refLevel.height, logMeanAdaptResized);
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "csf.bin")).string(), refLevel.width, refLevel.height, csfMap);
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "nncsf.bin")).string(), refLevel.width, refLevel.height, nNcsfMap);
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "nmask.bin")).string(), refLevel.width, refLevel.height, nMaskMap);
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "d.bin")).string(), refLevel.width, refLevel.height, dMap);
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "dp.bin")).string(), refLevel.width, refLevel.height, dForQ);
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "diffmask.bin")).string(), refLevel.width, refLevel.height, diffMaskBand);
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "test.bin")).string(), refLevel.width, refLevel.height, testLevel.band);
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "ref.bin")).string(), refLevel.width, refLevel.height, refLevel.band);
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "self.bin")).string(), refLevel.width, refLevel.height, selfMasks[levelIndex]);
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "maskxn.bin")).string(), refLevel.width, refLevel.height, maskXn);
            std::vector<double> coeffMap(refLevel.width * refLevel.height, 0.0);
            #if defined(_OPENMP)
            #pragma omp parallel for schedule(static)
            #endif
            for (long long i = 0; i < static_cast<long long>(coeffMap.size()); ++i)
                coeffMap[static_cast<size_t>(i)] = dLevels[levelIndex].band[static_cast<size_t>(i)] / std::max(bandNorm, 1.0);
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / (prefix + "coeff.bin")).string(), refLevel.width, refLevel.height, coeffMap);
        }
        qErr += minkowskiSum(dForQ, 0.8) / static_cast<double>(std::max<size_t>(1, levelCount));
        previousDForQ = std::move(dForQ);
    }
    // Only dLevels (and the base size, a plain number) are read from here on.
    std::vector<double>().swap(diffMask);
    std::vector<VisibilityPyramidLevel>().swap(refPyramid.levels);
    std::vector<VisibilityPyramidLevel>().swap(testPyramid.levels);
    std::vector<std::vector<double> >().swap(selfMasks);
    std::vector<double>().swap(logMeanAdaptFull);
    std::vector<double>().swap(previousDForQ);

    HdrvdpSubsetSideBySideResult result;
    if (keepDetailMaps)
        result.contrastMap.resize(width * height, 0.0);
    std::vector<double> reconstructed = reconstructVisibilityPyramidResponse(
        dLevels,
        refPyramid.baseWidth,
        refPyramid.baseHeight,
        width,
        height
    );
    std::vector<double> sMap(width * height, 0.0);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(reconstructed.size()); ++i) {
        sMap[i] = std::abs(reconstructed[i]);
        if (keepDetailMaps)
            result.contrastMap[i] = sMap[i];
    }
    if (debugOutDir) {
        writeGrayMapRaw((std::filesystem::path(*debugOutDir) / "native_side_S_raw.bin").string(), width, height, reconstructed);
        for (size_t levelIndex = 0; levelIndex < dLevels.size(); ++levelIndex) {
            std::vector<VisibilityPyramidLevel> bandOnly = dLevels;
            for (size_t other = 0; other < bandOnly.size(); ++other) {
                if (other == levelIndex)
                    continue;
                std::fill(bandOnly[other].band.begin(), bandOnly[other].band.end(), 0.0);
            }
            const std::vector<double> bandRecon = reconstructVisibilityPyramidResponse(
                bandOnly,
                refPyramid.baseWidth,
                refPyramid.baseHeight,
                width,
                height
            );
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / ("native_side_recon_band" + std::to_string(levelIndex + 1) + ".bin")).string(),
                width, height, bandRecon);
        }
    }
    std::vector<double>().swap(reconstructed);
    std::vector<VisibilityPyramidLevel>().swap(dLevels);
    std::vector<double> pMap(width * height, 0.0);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(sMap.size()); ++i)
        pMap[i] = 1.0 - std::exp(std::log(0.5) * sMap[i]);
    std::vector<double>().swap(sMap);
    if (debugOutDir) {
        writeGrayMapRaw((std::filesystem::path(*debugOutDir) / "native_side_P_prepool.bin").string(), width, height, pMap);
    }
    if (usedBands > 0) {
        const double sigma = std::pow(10.0, params.siSigma) * pixelsPerDegree;
        // logUndetect is computed in place of pMap (element i only reads pMap[i]); pMap is
        // overwritten from the pooled values below anyway.
        std::vector<double> &logUndetect = pMap;
        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(pMap.size()); ++i)
            logUndetect[i] = std::log(1e-4 + (1.0 - pMap[i]));
        const std::vector<double> pooled = fastGaussLikeMatlab(
            logUndetect, width, height, std::max(0.6, sigma), true, false, false, 0.0);
        if (debugOutDir) {
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / "native_side_log_undetect.bin").string(), width, height, logUndetect);
            writeGrayMapRaw((std::filesystem::path(*debugOutDir) / "native_side_pooled_log_undetect.bin").string(), width, height, pooled);
        }
        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(pMap.size()); ++i)
            pMap[i] = 1.0 - std::exp(pooled[i]);
    } else {
        std::fill(pMap.begin(), pMap.end(), 0.0);
    }
    result.undetectMap.resize(width * height, 0.0);
    if (keepDetailMaps)
        result.probabilityMap.resize(width * height, 0.0);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(pMap.size()); ++i) {
        const double pDetect = std::max(0.0, std::min(1.0, pMap[i]));
        if (keepDetailMaps)
            result.probabilityMap[i] = pDetect;
        result.undetectMap[i] = std::max(0.0, std::min(1.0, 1.0 - pDetect));
    }

    const double qualityFloor = -4.0;
    const double maxQ = 10.0;
    const double qValue = maxQ + qualityFloor - std::log(std::exp(qualityFloor) + qErr);
    const double qualityClamped = std::max(0.0, std::min(10.0, qValue));
    result.qualityJod = 10.0 - 0.5200 * std::pow(std::max(10.0 - qualityClamped, 0.0), 1.2812);
    result.qualityJod = std::max(0.0, std::min(10.0, result.qualityJod));
    return result;
}

double sigmaForDir(const Vec3d &dir, const ViewBasis &view) {
    return std::acos(clampDot(dot(view.vdir, dir)));
}

double dgmThresholdSolidAngleForDir(const Vec3d &dir, const ViewBasis &view) {
    const double beta = sigmaForDir(dir, view);
    const double horizontal = std::abs(dot(dir, view.hv));
    const double vertical = std::abs(dot(dir, view.vv));
    const double alpha = std::atan2(horizontal, vertical);
    const double base = std::cos(alpha) * (beta - 0.230914) + beta - 0.284456;

    double gammaDegrees = kDgmScaleThreshold * 0.238658;
    if (base > 0.0) {
        gammaDegrees = kDgmScaleThreshold * (std::pow(base, 2.02609 * beta) + 0.238658);
    }
    gammaDegrees = std::min(gammaDegrees, kDgmThresholdCapDegrees);

    const double gamma = gammaDegrees * kPi / 180.0;
    return 2.0 * kPi * (1.0 - std::cos(gamma));
}

// ============================================================================================
// Native evalglare: faithful C++ port of Radiance evalglare V3.06 (src/util/evalglare.c by
// J. Wienold, Fraunhofer ISE / EPFL) together with the Radiance code it relies on:
// pictool.c (picture + per-pixel geometry caches), image.c (setview, viewray, pix2loc,
// getviewopt, sscanview, isview, viewfile), header.c/resolu.c (header and orientation handling)
// and muc_randvar.c (statistics). Expressions are transliterated operation by operation,
// including the original float/double types, so that results match the original bit for bit
// where the original is deterministic. Port-only extensions are marked "port:".
// ============================================================================================
namespace eg {

// ---- color.h: CIE luminance coefficients evaluated exactly like the Radiance macros ----
#define EG_CIE_x_r 0.640
#define EG_CIE_y_r 0.330
#define EG_CIE_x_g 0.290
#define EG_CIE_y_g 0.600
#define EG_CIE_x_b 0.150
#define EG_CIE_y_b 0.060
#define EG_CIE_x_w (1./3.)
#define EG_CIE_y_w (1./3.)
#define EG_CIE_D (EG_CIE_x_r*(EG_CIE_y_g - EG_CIE_y_b) + EG_CIE_x_g*(EG_CIE_y_b - EG_CIE_y_r) + \
                  EG_CIE_x_b*(EG_CIE_y_r - EG_CIE_y_g))
#define EG_CIE_C_rD ((1./EG_CIE_y_w) * (EG_CIE_x_w*(EG_CIE_y_g - EG_CIE_y_b) - \
                     EG_CIE_y_w*(EG_CIE_x_g - EG_CIE_x_b) + EG_CIE_x_g*EG_CIE_y_b - EG_CIE_x_b*EG_CIE_y_g))
#define EG_CIE_C_gD ((1./EG_CIE_y_w) * (EG_CIE_x_w*(EG_CIE_y_b - EG_CIE_y_r) - \
                     EG_CIE_y_w*(EG_CIE_x_b - EG_CIE_x_r) - EG_CIE_x_r*EG_CIE_y_b + EG_CIE_x_b*EG_CIE_y_r))
#define EG_CIE_C_bD ((1./EG_CIE_y_w) * (EG_CIE_x_w*(EG_CIE_y_r - EG_CIE_y_g) - \
                     EG_CIE_y_w*(EG_CIE_x_r - EG_CIE_x_g) + EG_CIE_x_r*EG_CIE_y_g - EG_CIE_x_g*EG_CIE_y_r))
// = 0.26510582010582007, 0.67010582010581987, 0.064788359788359784 (current Radiance color.h,
// equal-energy white); older Radiance releases used 0.3333 white (0.265074126/0.670114631/0.064811243).
const double CIE_rf = (EG_CIE_y_r*EG_CIE_C_rD/EG_CIE_D);
const double CIE_gf = (EG_CIE_y_g*EG_CIE_C_gD/EG_CIE_D);
const double CIE_bf = (EG_CIE_y_b*EG_CIE_C_bD/EG_CIE_D);
#undef EG_CIE_x_r
#undef EG_CIE_y_r
#undef EG_CIE_x_g
#undef EG_CIE_y_g
#undef EG_CIE_x_b
#undef EG_CIE_y_b
#undef EG_CIE_x_w
#undef EG_CIE_y_w
#undef EG_CIE_D
#undef EG_CIE_C_rD
#undef EG_CIE_C_gD
#undef EG_CIE_C_bD

const double WHTEFFICACY = 179.;
const double FTINY = 1e-6;
const double PI = 3.14159265358979323846;
const double kMPi = 3.14159265358979323846;  // M_PI as used by pictool.c
const int XDECR = kRadianceXDecr;
const int YDECR = kRadianceYDecr;
const int YMAJOR = kRadianceYMajor;
const int VT_PER = 'v';
const int VT_PAR = 'l';
const int VT_ANG = 'a';
const int VT_HEM = 'h';
const int VT_PLS = 's';
const int VT_CYL = 'c';
const char *const kReleaseName = "evalglare 3.06 release 01.10.2025 by J.Wienold, EPFL";

inline double luminance(const float *c) {
    return WHTEFFICACY * (CIE_rf*c[0] + CIE_gf*c[1] + CIE_bf*c[2]);
}
inline double DOT(const double *a, const double *b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
inline void VCOPY(double *a, const double *b) {
    a[0] = b[0];
    a[1] = b[1];
    a[2] = b[2];
}
inline void fcross(double *vres, const double *v1, const double *v2) {
    vres[0] = v1[1]*v2[2] - v1[2]*v2[1];
    vres[1] = v1[2]*v2[0] - v1[0]*v2[2];
    vres[2] = v1[0]*v2[1] - v1[1]*v2[0];
}
// fvect.c normalize() incl. its first-order approximation close to unit length
double normalize(double *v) {
    double len, d;
    d = DOT(v, v);
    if (d == 0.0)
        return 0.0;
    if ((d <= 1.0 + FTINY) & (d >= 1.0 - FTINY)) {
        len = 0.5 + 0.5*d;
        d = 2.0 - len;
    } else {
        len = std::sqrt(d);
        d = 1.0/len;
    }
    v[0] *= d;
    v[1] *= d;
    v[2] *= d;
    return len;
}

// printf-style text sink. Numbers always use '.' as decimal point (like the C locale of the
// original), independent of the locale of the hosting process.
void appendFormatted(std::string &sink, const char *fmt, va_list ap) {
    char decimalPoint = '.';
    if (const std::lconv *lc = std::localeconv()) {
        if (lc->decimal_point && lc->decimal_point[0])
            decimalPoint = lc->decimal_point[0];
    }
    char spec[32];
    char buffer[512];
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') {
            sink.push_back(*p);
            continue;
        }
        if (p[1] == '%') {
            sink.push_back('%');
            ++p;
            continue;
        }
        size_t n = 0;
        spec[n++] = *p++;
        while (*p && std::strchr("-+ #0123456789.lh", *p) && n < sizeof(spec) - 2)
            spec[n++] = *p++;
        if (!*p)
            break;
        const char conv = *p;
        spec[n++] = conv;
        spec[n] = '\0';
        int written = 0;
        switch (conv) {
            case 'd':
            case 'i':
                written = std::snprintf(buffer, sizeof(buffer), spec, va_arg(ap, int));
                break;
            case 'f':
            case 'e':
            case 'g':
            case 'E':
            case 'G':
                written = std::snprintf(buffer, sizeof(buffer), spec, va_arg(ap, double));
                if (decimalPoint != '.') {
                    for (int k = 0; k < written && k < static_cast<int>(sizeof(buffer)); ++k) {
                        if (buffer[k] == decimalPoint)
                            buffer[k] = '.';
                    }
                }
                break;
            case 's': {
                const char *s = va_arg(ap, const char *);
                sink += s ? s : "(null)";
                continue;
            }
            case 'c':
                sink.push_back(static_cast<char>(va_arg(ap, int)));
                continue;
            default:
                sink += spec;
                continue;
        }
        if (written > 0)
            sink.append(buffer, static_cast<size_t>(std::min<int>(written, static_cast<int>(sizeof(buffer)) - 1)));
    }
}

struct Io {
    std::string out;  // stdout of the original
    std::string err;  // stderr of the original
    void outf(const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        appendFormatted(out, fmt, ap);
        va_end(ap);
    }
    void errf(const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        appendFormatted(err, fmt, ap);
        va_end(ap);
    }
};

// Thrown for the original's exit() calls; the collected stdout/stderr stay valid.
struct EvalGlareExit {
    int status;
};

// ---------------------------------------------------------------------------------------
// image.c: views
// ---------------------------------------------------------------------------------------
struct RadView {
    int type = VT_PER;
    double vp[3] = {0., 0., 0.};
    double vdir[3] = {0., 1., 0.};
    double vup[3] = {0., 0., 1.};
    double vdist = 1.;
    double horiz = 45.;
    double vert = 45.;
    double hoff = 0.;
    double voff = 0.;
    double vfore = 0.;
    double vaft = 0.;
    double hvec[3] = {0., 0., 0.};
    double vvec[3] = {0., 0., 0.};
    double hn2 = 0.;
    double vn2 = 0.;
};

const char *setview(RadView &v) {
    static const char ill_horiz[] = "illegal horizontal view size";
    static const char ill_vert[] = "illegal vertical view size";

    if (((v.vfore < -FTINY) | (v.vaft < -FTINY)) || ((v.vaft > FTINY) & (v.vaft <= v.vfore)))
        return "illegal fore/aft clipping plane";
    if (v.vdist <= FTINY)
        return "illegal view distance";
    v.vdist *= normalize(v.vdir);
    if (v.vdist == 0.0)
        return "zero view direction";
    if (normalize(v.vup) == 0.0)
        return "zero view up vector";
    fcross(v.hvec, v.vdir, v.vup);
    if (normalize(v.hvec) == 0.0)
        return "view up parallel to view direction";
    fcross(v.vvec, v.hvec, v.vdir);
    if (v.horiz <= FTINY)
        return ill_horiz;
    if (v.vert <= FTINY)
        return ill_vert;
    switch (v.type) {
        case VT_PAR:
            v.hn2 = v.horiz;
            v.vn2 = v.vert;
            break;
        case VT_PER:
            if (v.horiz >= 180.0-FTINY)
                return ill_horiz;
            if (v.vert >= 180.0-FTINY)
                return ill_vert;
            v.hn2 = 2.0 * std::tan(v.horiz*(PI/360.));
            v.vn2 = 2.0 * std::tan(v.vert*(PI/360.));
            break;
        case VT_CYL:
            if (v.horiz > 360.0+FTINY)
                return ill_horiz;
            if (v.vert >= 180.0-FTINY)
                return ill_vert;
            v.hn2 = v.horiz * (PI/180.0);
            v.vn2 = 2.0 * std::tan(v.vert*(PI/360.));
            break;
        case VT_ANG:
            if (v.horiz > 360.0+FTINY)
                return ill_horiz;
            if (v.vert > 360.0+FTINY)
                return ill_vert;
            v.hn2 = v.horiz * (PI/180.0);
            v.vn2 = v.vert * (PI/180.0);
            break;
        case VT_HEM:
            if (v.horiz > 180.0+FTINY)
                return ill_horiz;
            if (v.vert > 180.0+FTINY)
                return ill_vert;
            v.hn2 = 2.0 * std::sin(v.horiz*(PI/360.));
            v.vn2 = 2.0 * std::sin(v.vert*(PI/360.));
            break;
        case VT_PLS:
            if (v.horiz >= 360.0-FTINY)
                return ill_horiz;
            if (v.vert >= 360.0-FTINY)
                return ill_vert;
            v.hn2 = 2.*std::sin(v.horiz*(PI/360.)) / (1.0 + std::cos(v.horiz*(PI/360.)));
            v.vn2 = 2.*std::sin(v.vert*(PI/360.)) / (1.0 + std::cos(v.vert*(PI/360.)));
            break;
        default:
            return "unknown view type";
    }
    if (v.type != VT_ANG && v.type != VT_PLS) {
        if (v.type != VT_CYL) {
            v.hvec[0] *= v.hn2;
            v.hvec[1] *= v.hn2;
            v.hvec[2] *= v.hn2;
        }
        v.vvec[0] *= v.vn2;
        v.vvec[1] *= v.vn2;
        v.vvec[2] *= v.vn2;
    }
    v.hn2 *= v.hn2;
    v.vn2 *= v.vn2;
    return nullptr;
}

// image.c viewray() (the ray origin is not needed by evalglare and is not computed).
double viewray(double *direc, const RadView &v, double x, double y) {
    double d, z;

    x += v.hoff - 0.5;
    y += v.voff - 0.5;
    switch (v.type) {
        case VT_PAR:
            VCOPY(direc, v.vdir);
            return (v.vaft > FTINY ? v.vaft - v.vfore : 0.0);
        case VT_PER:
            direc[0] = v.vdir[0] + x*v.hvec[0] + y*v.vvec[0];
            direc[1] = v.vdir[1] + x*v.hvec[1] + y*v.vvec[1];
            direc[2] = v.vdir[2] + x*v.hvec[2] + y*v.vvec[2];
            d = normalize(direc);
            return (v.vaft > FTINY ? (v.vaft - v.vfore)*d : 0.0);
        case VT_HEM:
            z = 1.0 - x*x*v.hn2 - y*y*v.vn2;
            if (z < 0.0)
                return -1.0;
            z = std::sqrt(z);
            direc[0] = z*v.vdir[0] + x*v.hvec[0] + y*v.vvec[0];
            direc[1] = z*v.vdir[1] + x*v.hvec[1] + y*v.vvec[1];
            direc[2] = z*v.vdir[2] + x*v.hvec[2] + y*v.vvec[2];
            return (v.vaft > FTINY ? v.vaft - v.vfore : 0.0);
        case VT_CYL:
            d = x * v.horiz * (PI/180.0);
            z = std::cos(d);
            x = std::sin(d);
            direc[0] = z*v.vdir[0] + x*v.hvec[0] + y*v.vvec[0];
            direc[1] = z*v.vdir[1] + x*v.hvec[1] + y*v.vvec[1];
            direc[2] = z*v.vdir[2] + x*v.hvec[2] + y*v.vvec[2];
            d = normalize(direc);
            return (v.vaft > FTINY ? (v.vaft - v.vfore)*d : 0.0);
        case VT_ANG:
            x *= (1.0/180.0)*v.horiz;
            y *= (1.0/180.0)*v.vert;
            d = x*x + y*y;
            if (d > 1.0)
                return -1.0;
            d = std::sqrt(d);
            z = std::cos(PI*d);
            d = d <= FTINY ? PI : std::sqrt(1.0 - z*z)/d;
            x *= d;
            y *= d;
            direc[0] = z*v.vdir[0] + x*v.hvec[0] + y*v.vvec[0];
            direc[1] = z*v.vdir[1] + x*v.hvec[1] + y*v.vvec[1];
            direc[2] = z*v.vdir[2] + x*v.hvec[2] + y*v.vvec[2];
            return (v.vaft > FTINY ? v.vaft - v.vfore : 0.0);
        case VT_PLS:
            x *= std::sqrt(v.hn2);
            y *= std::sqrt(v.vn2);
            d = x*x + y*y;
            z = (1. - d)/(1. + d);
            x *= (1. + z);
            y *= (1. + z);
            direc[0] = z*v.vdir[0] + x*v.hvec[0] + y*v.vvec[0];
            direc[1] = z*v.vdir[1] + x*v.hvec[1] + y*v.vvec[1];
            direc[2] = z*v.vdir[2] + x*v.hvec[2] + y*v.vvec[2];
            return (v.vaft > FTINY ? v.vaft - v.vfore : 0.0);
    }
    return -1.0;
}

inline void pix2loc(double *loc, int xr, int yr, int rt, int px, int py) {
    int x, y;
    if (rt & YMAJOR) {
        x = px;
        y = py;
    } else {
        x = py;
        y = px;
    }
    if (rt & XDECR)
        x = xr-1 - x;
    if (rt & YDECR)
        y = yr-1 - y;
    loc[0] = (x+.5)/xr;
    loc[1] = (y+.5)/yr;
}

// words.c fskip()/iskip(), badarg.c badarg() with the "fff"/"f" formats used by getviewopt()
const char *iskip(const char *s) {
    while (std::isspace(static_cast<unsigned char>(*s)))
        s++;
    s += (*s == '-') | (*s == '+');
    if (!std::isdigit(static_cast<unsigned char>(*s)))
        return nullptr;
    do
        s++;
    while (std::isdigit(static_cast<unsigned char>(*s)));
    return s;
}
const char *fskip(const char *s) {
    const char *cp;
    while (std::isspace(static_cast<unsigned char>(*s)))
        s++;
    s += (*s == '-') | (*s == '+');
    cp = s;
    while (std::isdigit(static_cast<unsigned char>(*cp)))
        cp++;
    if (*cp == '.') {
        cp++;
        s++;
        while (std::isdigit(static_cast<unsigned char>(*cp)))
            cp++;
    }
    if (cp == s)
        return nullptr;
    if ((*cp == 'e') | (*cp == 'E'))
        return std::isspace(static_cast<unsigned char>(*++cp)) ? nullptr : iskip(cp);
    return cp;
}
bool isfltd(const char *s) {
    const char *cp = fskip(s);
    return cp != nullptr && std::strchr(" \t\r\n", *cp) != nullptr;
}
int badarg(int ac, const char *const *av, const char *fl) {
    int i;
    if (fl == nullptr)
        fl = "";
    for (i = 1; *fl; i++, av++, fl++) {
        if (i > ac || *av == nullptr)
            return -1;
        if (*fl == 'f') {
            if (!isfltd(*av))
                return i;
        } else {
            return -1;
        }
    }
    return 0;
}

// image.c getviewopt(): av[0] is the option, ac the number of available arguments incl. av[0]
int getviewopt(RadView &v, int ac, const char *const *av) {
#define EG_CHECK(c, l) \
    if ((av[0][c] && !std::isspace(static_cast<unsigned char>(av[0][c]))) || badarg(ac-1, av+1, l)) \
        return -1
    if (ac <= 0 || av[0][0] != '-' || av[0][1] != 'v')
        return -1;
    switch (av[0][2]) {
        case 't':
            if (!av[0][3] || std::isspace(static_cast<unsigned char>(av[0][3])))
                return -1;
            EG_CHECK(4, "");
            v.type = av[0][3];
            return 0;
        case 'p':
            EG_CHECK(3, "fff");
            v.vp[0] = std::atof(av[1]);
            v.vp[1] = std::atof(av[2]);
            v.vp[2] = std::atof(av[3]);
            return 3;
        case 'd':
            EG_CHECK(3, "fff");
            v.vdir[0] = std::atof(av[1]);
            v.vdir[1] = std::atof(av[2]);
            v.vdir[2] = std::atof(av[3]);
            v.vdist = 1.;
            return 3;
        case 'u':
            EG_CHECK(3, "fff");
            v.vup[0] = std::atof(av[1]);
            v.vup[1] = std::atof(av[2]);
            v.vup[2] = std::atof(av[3]);
            return 3;
        case 'h':
            EG_CHECK(3, "f");
            v.horiz = std::atof(av[1]);
            return 1;
        case 'v':
            EG_CHECK(3, "f");
            v.vert = std::atof(av[1]);
            return 1;
        case 'o':
            EG_CHECK(3, "f");
            v.vfore = std::atof(av[1]);
            return 1;
        case 'a':
            EG_CHECK(3, "f");
            v.vaft = std::atof(av[1]);
            return 1;
        case 's':
            EG_CHECK(3, "f");
            v.hoff = std::atof(av[1]);
            return 1;
        case 'l':
            EG_CHECK(3, "f");
            v.voff = std::atof(av[1]);
            return 1;
        default:
            return -1;
    }
#undef EG_CHECK
}

// image.c sscanview(): view options inside one header/view-file line
int sscanview(RadView &vp, const char *s) {
    int ac;
    const char *av[4];
    int na;
    int nvopts = 0;

    while (std::isspace(static_cast<unsigned char>(*s)))
        if (!*s++)
            return 0;
    while (*s) {
        ac = 0;
        do {
            if (ac || *s == '-')
                av[ac++] = s;
            while (*s && !std::isspace(static_cast<unsigned char>(*s)))
                s++;
            while (std::isspace(static_cast<unsigned char>(*s)))
                s++;
        } while (*s && ac < 4);
        if ((na = getviewopt(vp, ac, av)) >= 0) {
            if (na+1 < ac)
                s = av[na+1];
            nvopts++;
        } else if (ac > 1) {
            s = av[1];
        }
    }
    return nvopts;
}

// image.c isview(); the program name of the original is "evalglare"
bool isview(const char *s) {
    static const char *const altname[] = {"evalglare", "VIEW=", "rpict", "rvu", "rpiece", "rxpiece",
                                          "pinterp", "rview", nullptr};
    const char *cp = s;
    while (*cp && !std::isspace(static_cast<unsigned char>(*cp)))
        cp++;
    while (cp > s && cp[-1] != '/')
        cp--;
    for (const char *const *an = altname; *an != nullptr; an++)
        if (!std::strncmp(*an, cp, std::strlen(*an)))
            return true;
    return false;
}

// header.c getheader(): replays the stored header lines through the original fgets() loop
// (MAXLINE=2048, incl. its ungetc() handling of long lines) and calls fn for every line the
// original callback would see (with its trailing '\n').
template <typename Fn>
void forEachHeaderLine(const std::vector<std::string> &lines, Fn fn) {
    std::string stream;
    for (const std::string &line : lines) {
        stream += line;
        stream += '\n';
    }
    stream += '\n';  // the empty line that ends the header
    if (stream.empty() || !std::isprint(static_cast<unsigned char>(stream[0])))
        return;
    size_t pos = 0;
    int pushedBack = -1;
    const int kMaxLine = 2048;
    char buf[kMaxLine];
    for (;;) {
        buf[kMaxLine-2] = '\n';
        // fgets(buf, MAXLINE, fp)
        int count = 0;
        bool gotAny = false;
        while (count < kMaxLine - 1) {
            int c;
            if (pushedBack >= 0) {
                c = pushedBack;
                pushedBack = -1;
            } else if (pos < stream.size()) {
                c = static_cast<unsigned char>(stream[pos++]);
            } else {
                break;
            }
            gotAny = true;
            buf[count++] = static_cast<char>(c);
            if (c == '\n')
                break;
        }
        if (!gotAny)
            return;
        buf[count] = '\0';
        if (buf[buf[0] == '\r'] == '\n')
            return;  // end of header
        if (buf[kMaxLine-2] != '\n') {
            pushedBack = static_cast<unsigned char>(buf[kMaxLine-2]);
            buf[kMaxLine-2] = '\0';
        }
        fn(static_cast<const char *>(buf));
    }
}

// image.c viewfile() for -vf: returns -1 if the file cannot be opened, else the number of
// view lines found (0 = bad view file)
int viewfile(const std::string &fname, RadView &vp) {
    std::FILE *fp = (fname == "-") ? stdin : std::fopen(fname.c_str(), "r");
    if (!fp)
        return -1;
    int ok = 0;
    int firstc = std::fgetc(fp);
    if (firstc != EOF && std::isprint(firstc)) {
        std::ungetc(firstc, fp);
        char buf[2048];
        for (;;) {
            buf[2048-2] = '\n';
            if (std::fgets(buf, 2048, fp) == nullptr)
                break;
            if (buf[buf[0] == '\r'] == '\n')
                break;
            if (buf[2048-2] != '\n') {
                std::ungetc(buf[2048-2], fp);
                buf[2048-2] = '\0';
            }
            if (isview(buf) && sscanview(vp, buf) > 0)
                ok++;
        }
    }
    if (fp != stdin)
        std::fclose(fp);
    return ok;
}

std::string fprintview(const RadView &vp) {
    std::string s;
    Io tmp;
    tmp.outf(" -vt%c", vp.type);
    tmp.outf(" -vp %.7g %.7g %.7g", vp.vp[0], vp.vp[1], vp.vp[2]);
    tmp.outf(" -vd %.7g %.7g %.7g", vp.vdir[0]*vp.vdist, vp.vdir[1]*vp.vdist, vp.vdir[2]*vp.vdist);
    tmp.outf(" -vu %.5g %.5g %.5g", vp.vup[0], vp.vup[1], vp.vup[2]);
    tmp.outf(" -vh %.5g -vv %.5g", vp.horiz, vp.vert);
    tmp.outf(" -vo %.5g -va %.5g", vp.vfore, vp.vaft);
    tmp.outf(" -vs %.5g -vl %.5g", vp.hoff, vp.voff);
    return tmp.out;
}

// Exact replacement for "acos(D) * k <= r" (k = 1 or 2) that only evaluates acos() close to the
// decision boundary. Arguments outside [-1,1] (and NaN) behave like acos() (NaN -> false).
struct AcosLeq {
    double k = 2.0;
    double r = 0.0;
    double lo = 0.0;
    double hi = 0.0;
    AcosLeq() = default;
    AcosLeq(double rr, double kk) : k(kk), r(rr) {
        const double inf = std::numeric_limits<double>::infinity();
        const double half = rr / kk;
        if (!(half == half) || half < 0.0) {
            lo = inf;   // never true
            hi = inf;
        } else if (half >= PI) {
            lo = -inf;  // always true
            hi = -inf;
        } else {
            const double t = std::cos(half);
            hi = t + 1e-12;
            lo = t - 1e-12;
        }
    }
    bool operator()(double D) const {
        if (D <= 1.0 && D >= -1.0) {
            if (D >= hi)
                return true;
            if (D <= lo)
                return false;
        }
        return std::acos(D) * k <= r;
    }
};

// ---------------------------------------------------------------------------------------
// pictool.c: picture with per-pixel geometry caches (pixinfo) and the glare source list
// ---------------------------------------------------------------------------------------
enum {
    PICT_GSN = 0,
    PICT_Z1_GSN,
    PICT_Z2_GSN,
    PICT_NPIX,
    PICT_AVPOSX,
    PICT_AVPOSY,
    PICT_AVLUM,
    PICT_AVOMEGA,
    PICT_DXMAX,
    PICT_DYMAX,
    PICT_LMIN,
    PICT_LMAX,
    PICT_EGLARE,
    PICT_DGLARE,
    PICT_GLSIZE
};
using GlareInfo = std::array<double, PICT_GLSIZE>;

// port: bounding box of all pixels ever added to a glare source; only used to restrict the
// original whole-picture loops to the region where the source can have pixels (same order).
struct GsBox {
    int x0 = std::numeric_limits<int>::max();
    int x1 = -1;
    int y0 = std::numeric_limits<int>::max();
    int y1 = -1;
    bool empty() const { return x1 < x0; }
    void add(int x, int y) {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    void merge(const GsBox &o) {
        if (o.empty())
            return;
        add(o.x0, o.y0);
        add(o.x1, o.y1);
    }
};

// Per-pixel geometry of pictool's pict_update_evalglare_caches(): centre direction
// (pict_get_dir), solid angle (pict_get_sangle) and whether viewray() succeeded at the pixel
// centre (the condition pict_get_hangle() returns). Stored column major (index x*yr+y) like
// pictool's pixinfo array.
struct GeometryCache {
    int xr = 0;
    int yr = 0;
    int rt = 0;
    RadView view;
    std::vector<double> dir;
    std::vector<double> omega;
    std::vector<unsigned char> centerOk;
    std::vector<std::pair<int, int>> normalErrors;   // pixels with "Normal error" (yy-major order)
    std::vector<double> normalErrorAngles;
};

bool sameViewGeometry(const RadView &a, const RadView &b) {
    // the fields viewray() depends on, compared bit for bit
    return a.type == b.type && a.hoff == b.hoff && a.voff == b.voff && a.horiz == b.horiz &&
           a.vert == b.vert && a.hn2 == b.hn2 && a.vn2 == b.vn2 && a.vfore == b.vfore &&
           a.vaft == b.vaft &&
           std::memcmp(a.vdir, b.vdir, sizeof(a.vdir)) == 0 &&
           std::memcmp(a.hvec, b.hvec, sizeof(a.hvec)) == 0 &&
           std::memcmp(a.vvec, b.vvec, sizeof(a.vvec)) == 0 &&
           std::memcmp(a.vp, b.vp, sizeof(a.vp)) == 0;
}

// pictool.c splane_normal() with the two edge directions already evaluated (viewray results of
// the same arguments are identical, so the four corner rays of a pixel are computed once).
inline int splaneNormalFromDirs(const double *e1in, bool ok1, const double *e2in, bool ok2, double *n) {
    double e1[3] = {0, 0, 0};
    double e2[3] = {0, 0, 0};
    if (ok1)
        VCOPY(e1, e1in);
    n[0] = n[1] = n[2] = 0;
    if (ok2)
        VCOPY(n, e2in);
    e2[0] = n[0] - e1[0];
    e2[1] = n[1] - e1[1];
    e2[2] = n[2] - e1[2];
    n[0] = e1[1]*e2[2] - e1[2]*e2[1];
    n[1] = e1[2]*e2[0] - e1[0]*e2[2];
    n[2] = e1[0]*e2[1] - e1[1]*e2[0];
    if (normalize(n) == 0.0)
        return 0;
    return 1;
}

// pictool.c pict_get_sangle(). NOTE (Radiance quirk, replicated on purpose): unlike
// pict_get_ray(), which converts the bottom-up pictool row y with pix2loc(x, yr-1-y),
// pict_get_sangle() calls pix2loc(x, y) WITHOUT that flip, i.e. the solid angle of row y is the one
// of the vertically mirrored pixel position (identical for vertically symmetric views).
double pictGetSangle(const RadView &v, int xr, int yr, int rt, int x, int y, bool &normalError, double &errorAngle) {
    double pc[2];
    double hpx, hpy;
    pix2loc(pc, xr, yr, rt, x, y);
    hpx = (0.5/xr);
    hpy = (0.5/yr);
    pc[0] -= hpx;
    pc[1] -= hpy;
    const double ax = pc[0], ay = pc[1];
    const double bx = pc[0], by = (pc[1]+2.0*hpy);
    const double cx = (pc[0]+2.0*hpx), cy = (pc[1]+2.0*hpy);
    const double dx = (pc[0]+2.0*hpx), dy = pc[1];
    double da[3], db[3], dc[3], dd[3];
    const bool oa = viewray(da, v, ax, ay) >= 0;
    const bool ob = viewray(db, v, bx, by) >= 0;
    const bool oc = viewray(dc, v, cx, cy) >= 0;
    const bool od = viewray(dd, v, dx, dy) >= 0;
    double n[4][3];
    int i = splaneNormalFromDirs(da, oa, db, ob, n[0]);
    i &= splaneNormalFromDirs(db, ob, dc, oc, n[1]);
    i &= splaneNormalFromDirs(dc, oc, dd, od, n[2]);
    i &= splaneNormalFromDirs(dd, od, da, oa, n[3]);
    if (!i)
        return 0;
    double ang = 0;
    for (i = 0; i < 4; i++) {
        double a = DOT(n[i], n[(i+1)%4]);
        a = std::acos(a);
        a = std::fabs(a);
        ang += kMPi - a;
    }
    ang = ang - 2.0*kMPi;
    if ((ang > (2.0*kMPi)) || ang < 0) {
        normalError = true;
        errorAngle = ang;
        return -1.0;
    }
    return ang;
}

// pict_update_evalglare_caches(). fallbackView: pictool keeps the previously cached direction
// where viewray() fails for the new view; when the picture's view is replaced by a command-line
// view, that is the direction of the header view (if viewray() succeeded for it), otherwise the
// zero vector of the freshly allocated cache.
std::shared_ptr<GeometryCache> buildGeometry(const RadView &view, int xr, int yr, int rt,
                                             const RadView *fallbackView) {
    auto g = std::make_shared<GeometryCache>();
    g->xr = xr;
    g->yr = yr;
    g->rt = rt;
    g->view = view;
    const size_t n = static_cast<size_t>(xr) * static_cast<size_t>(yr);
    g->dir.assign(n * 3, 0.0);
    g->omega.assign(n, 0.0);
    g->centerOk.assign(n, 0);
    std::vector<unsigned char> errFlag(n, 0);
    std::vector<double> errAng(n, 0.0);
#if defined(_OPENMP)
#pragma omp parallel for schedule(dynamic, 4)
#endif
    for (int x = 0; x < xr; ++x) {
        for (int y = 0; y < yr; ++y) {
            const size_t i = static_cast<size_t>(x) * static_cast<size_t>(yr) + static_cast<size_t>(y);
            double pc[2];
            double d[3];
            pix2loc(pc, xr, yr, rt, x, yr - 1 - y);
            if (viewray(d, view, pc[0], pc[1]) >= 0) {
                VCOPY(&g->dir[i*3], d);
                g->centerOk[i] = 1;
            } else if (fallbackView && viewray(d, *fallbackView, pc[0], pc[1]) >= 0) {
                VCOPY(&g->dir[i*3], d);
            }
            bool ne = false;
            double ea = 0.0;
            g->omega[i] = pictGetSangle(view, xr, yr, rt, x, y, ne, ea);
            if (ne) {
                errFlag[i] = 1;
                errAng[i] = ea;
            }
        }
    }
    for (int yy = 0; yy < yr; ++yy) {
        const int y = (rt & YDECR) ? yr - 1 - yy : yy;
        for (int x = 0; x < xr; ++x) {
            const size_t i = static_cast<size_t>(x) * static_cast<size_t>(yr) + static_cast<size_t>(y);
            if (errFlag[i]) {
                g->normalErrors.emplace_back(x, y);
                g->normalErrorAngles.push_back(errAng[i]);
            }
        }
    }
    return g;
}

// port: the geometry only depends on view, resolution and orientation; with
// EvalGlareOptions::cacheGeometry it is cached across calls (the GUI evaluates the same picture
// repeatedly). Only the most recent geometry is kept.
std::mutex sGeometryMutex;
std::shared_ptr<const GeometryCache> sLastGeometry;

void clearGeometryCache() {
    std::lock_guard<std::mutex> lock(sGeometryMutex);
    sLastGeometry.reset();
}

std::shared_ptr<const GeometryCache> cachedGeometry(const RadView &view, int xr, int yr, int rt,
                                                    const RadView *fallbackView) {
    if (fallbackView != nullptr)
        return buildGeometry(view, xr, yr, rt, fallbackView);
    std::mutex &sMutex = sGeometryMutex;
    std::shared_ptr<const GeometryCache> &sLast = sLastGeometry;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (sLast && sLast->xr == xr && sLast->yr == yr && sLast->rt == rt && sameViewGeometry(sLast->view, view))
            return sLast;
        sLast.reset();  // release the old geometry before building a new one
    }
    std::shared_ptr<const GeometryCache> built = buildGeometry(view, xr, yr, rt, nullptr);
    std::lock_guard<std::mutex> lock(sMutex);
    sLast = built;
    return built;
}

// port: conservative spatial index of glare source ids over pixel directions (cells of a cube grid
// with edge >= the search half angle). If no cell around a direction lists a source that could be
// merged into, no pixel of such a source lies within the search radius and the (exact) walk of
// find_near_pgs() cannot find one, so it is skipped. Lists only grow (stale ids are harmless).
struct DirGrid {
    double inv = 0.0;
    std::unordered_map<unsigned long long, std::vector<int>> cells;
    static unsigned long long pack(long a, long b, long c) {
        return (static_cast<unsigned long long>(a & 0x1FFFFF) << 42) |
               (static_cast<unsigned long long>(b & 0x1FFFFF) << 21) |
               static_cast<unsigned long long>(c & 0x1FFFFF);
    }
    long cellIndex(double v) const { return static_cast<long>(std::floor((v + 2.0) * inv)); }
    void add(const double *d, int g) {
        std::vector<int> &v = cells[pack(cellIndex(d[0]), cellIndex(d[1]), cellIndex(d[2]))];
        if (std::find(v.begin(), v.end(), g) == v.end())
            v.push_back(g);
    }
    template <typename Pred>
    bool any(const double *d, Pred pred) const {
        const long a = cellIndex(d[0]), b = cellIndex(d[1]), c = cellIndex(d[2]);
        for (long i = a - 1; i <= a + 1; ++i)
            for (long j = b - 1; j <= b + 1; ++j)
                for (long k = c - 1; k <= c + 1; ++k) {
                    const auto it = cells.find(pack(i, j, k));
                    if (it == cells.end())
                        continue;
                    for (int g : it->second)
                        if (pred(g))
                            return true;
                }
        return false;
    }
};

struct Pict {
    int xr = 0;
    int yr = 0;
    int rt = kRadiancePixStandard;
    RadView view;                 // p->view: modified in place by normalize() calls like pictool
    bool validView = true;
    bool viewFixed = false;       // normalize() of vdir/vup no longer changes them
    bool liveSameAsCache = true;  // view equals geo->view in all fields viewray() uses
    std::shared_ptr<const GeometryCache> geo;
    std::vector<float> col;       // RGB per pixel, column major
    std::vector<int> gsn;
    std::vector<unsigned char> pgs;
    std::vector<GlareInfo> gli;
    std::vector<GsBox> box;
    std::string comment;
    std::unique_ptr<DirGrid> grid;   // port: see DirGrid (built for the merge searches)
    float gridRadius = -1.0f;
    // port: largest av_omega of the glare sources lo..hi, cached until a source changes
    unsigned long long omegaVersion = 0;
    unsigned long long cachedVersion = ~0ULL;
    int cachedLo = 0;
    int cachedHi = -1;
    double cachedMaxOmega = 0.0;
    double maxOmega(int lo, int hi) {
        if (cachedVersion != omegaVersion || cachedLo != lo || cachedHi != hi) {
            double m = -std::numeric_limits<double>::infinity();
            for (int g = std::max(lo, 0); g <= hi && g < static_cast<int>(gli.size()); ++g)
                if (gli[static_cast<size_t>(g)][PICT_AVOMEGA] > m)
                    m = gli[static_cast<size_t>(g)][PICT_AVOMEGA];
            cachedVersion = omegaVersion;
            cachedLo = lo;
            cachedHi = hi;
            cachedMaxOmega = m;
        }
        return cachedMaxOmega;
    }

    size_t idx(int x, int y) const { return static_cast<size_t>(x) * static_cast<size_t>(yr) + static_cast<size_t>(y); }
    float *color(int x, int y) { return &col[idx(x, y) * 3]; }
    const float *color(int x, int y) const { return &col[idx(x, y) * 3]; }
    double omega(int x, int y) const { return geo->omega[idx(x, y)]; }
    const double *cdir(int x, int y) const { return &geo->dir[idx(x, y) * 3]; }
    bool centerOk(int x, int y) const { return geo->centerOk[idx(x, y)] != 0; }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < xr && y < yr; }
    // pict_is_validpixel()
    bool valid(int x, int y) const {
        const size_t i = idx(x, y);
        return ((view.type != VT_ANG) || (DOT(&geo->dir[i*3], view.vdir) >= 0.0)) && geo->omega[i] > 0.0;
    }
    void newGli() {
        GlareInfo g;
        g.fill(0.0);
        gli.push_back(g);
        box.push_back(GsBox());
    }
    double &gl(int i, int field) { return gli[static_cast<size_t>(i)][static_cast<size_t>(field)]; }
};

// normalize(vdir), normalize(vup) as done by every pict_get_sigma/tau/vangle/hangle call
void normalizeViewVectors(Pict &p) {
    if (p.viewFixed)
        return;
    double vd[3], vu[3];
    VCOPY(vd, p.view.vdir);
    VCOPY(vu, p.view.vup);
    normalize(p.view.vdir);
    normalize(p.view.vup);
    const bool unchanged = std::memcmp(vd, p.view.vdir, sizeof(vd)) == 0 &&
                           std::memcmp(vu, p.view.vup, sizeof(vu)) == 0;
    if (unchanged)
        p.viewFixed = true;
    else
        p.liveSameAsCache = sameViewGeometry(p.view, p.geo->view);
}

// pict_get_dir(): direction at integer pixel (x,y) with the current view
bool pictGetDir(const Pict &p, int x, int y, double *dir) {
    if (p.liveSameAsCache && p.inside(x, y)) {
        const size_t i = p.idx(x, y);
        if (!p.geo->centerOk[i])
            return false;
        VCOPY(dir, &p.geo->dir[i*3]);
        return true;
    }
    double pc[2];
    pix2loc(pc, p.xr, p.yr, p.rt, x, p.yr - 1 - y);
    return viewray(dir, p.view, pc[0], pc[1]) >= 0;
}

// The double coordinates of pictool's angle functions are truncated to int by pict_get_dir().
inline int truncCoord(double v) {
    if (!(v == v) || v >= 2147483648.0 || v <= -2147483649.0)
        return std::numeric_limits<int>::min();  // x86 cvttsd2si result for NaN/overflow
    return static_cast<int>(v);
}

bool pictGetSigma(Pict &p, double x, double y, double *s) {
    double pvdir[3];
    if (!pictGetDir(p, truncCoord(x), truncCoord(y), pvdir))
        return false;
    normalizeViewVectors(p);
    *s = std::acos(DOT(p.view.vdir, pvdir));
    return true;
}

bool pictGetTau(Pict &p, double x, double y, double *t) {
    double hv[3], pvdir[3];
    double s;
    int i;
    if (!pictGetSigma(p, x, y, &s))
        return false;
    if (!pictGetDir(p, truncCoord(x), truncCoord(y), pvdir))
        return false;
    VCOPY(hv, pvdir);
    normalize(hv);
    for (i = 0; i < 3; i++) {
        hv[i] /= std::cos(s);
    }
    hv[0] = hv[0] - p.view.vdir[0];
    hv[1] = hv[1] - p.view.vdir[1];
    hv[2] = hv[2] - p.view.vdir[2];
    normalize(hv);
    *t = std::acos(DOT(p.view.vup, hv));
    return true;
}

void orthoCoord(Pict &p, double *vv, double *hv) {
    normalizeViewVectors(p);
    fcross(hv, p.view.vdir, p.view.vup);
    fcross(vv, p.view.vdir, hv);
}

bool pictGetVangle(Pict &p, double x, double y, double *a) {
    double hv[3], vv[3], pvdir[3];
    orthoCoord(p, vv, hv);
    if (!pictGetDir(p, truncCoord(x), truncCoord(y), pvdir))
        return false;
    *a = std::acos(DOT(vv, pvdir)) - (kMPi/2.0);
    return true;
}

bool pictGetHangle(Pict &p, double x, double y, double *a) {
    double hv[3], vv[3], pvdir[3];
    orthoCoord(p, vv, hv);
    if (!pictGetDir(p, truncCoord(x), truncCoord(y), pvdir))
        return false;
    *a = ((kMPi/2.0) - std::acos(DOT(hv, pvdir)));
    return true;
}

// The loops of evalglare only use pict_get_hangle() as validity test of the pixel centre.
inline bool hangleOk(Pict &p, int x, int y) {
    normalizeViewVectors(p);
    return p.centerOk(x, y);
}

} // namespace eg

namespace eg {

// ---------------------------------------------------------------------------------------
// muc_randvar.c (one-dimensional, samples stored)
// ---------------------------------------------------------------------------------------
struct RandVar {
    int n = 0;
    double w = 0.0;
    double sum = 0.0;
    double sumSqr = 0.0;
    double minV = 1e10;    // FHUGE
    double maxV = -1e10;
    std::vector<double> samples;

    void add(double s) {
        samples.push_back(s);
        const double val = s*1.0;
        sum += val;
        sumSqr += s*s*1.0;
        if (minV > val)
            minV = val;
        if (maxV < val)
            maxV = val;
        w += 1.0;
        n++;
    }
    // muc_rvar_get_vx(): returns false (output untouched) without samples
    bool variance(double &vx) const {
        if (w == 0.0)
            return false;
        const double ex = sum/w;
        const double ex2 = sumSqr/w;
        vx = ex2 - ex*ex;
        return true;
    }
    double sortedAt(long k) {
        // value of the k-th smallest sample (like qsort + index); out of range -> 0 (undefined)
        if (k < 0 || k >= static_cast<long>(samples.size()))
            return 0.0;
        std::nth_element(samples.begin(), samples.begin() + k, samples.end());
        return samples[static_cast<size_t>(k)];
    }
    double median() {
        double val = sortedAt(n/2);
        if (n % 2 == 0) {
            val += sortedAt(n/2 - 1);
            val /= 2.0;
        }
        return val;
    }
    // muc_rvar_get_percentile(): "rv->n % 1/percentile == 0" is always true, so the value is
    // always averaged with the element below (indices truncated like the implicit int conversion)
    double percentile(double p) {
        double val = sortedAt(static_cast<long>(n*p));
        val += sortedAt(static_cast<long>(n*p - 1));
        val /= 2.0;
        return val;
    }
    bool boundingBox(double *b) const {
        if (n == 0)
            return false;
        b[0] = minV;
        b[1] = maxV;
        return true;
    }
};

// ---------------------------------------------------------------------------------------
// evalglare.c subroutines
// ---------------------------------------------------------------------------------------
void add_pixel_to_gs(Pict &p, int x, int y, int gsn, Io &io) {
    double old_av_posx, old_av_posy, old_av_lum, old_omega, act_omega, new_omega, act_lum,
        temp_av_posx, temp_av_posy;

    p.gl(gsn, PICT_NPIX) = p.gl(gsn, PICT_NPIX) + 1;
    old_av_posx = p.gl(gsn, PICT_AVPOSX);
    old_av_posy = p.gl(gsn, PICT_AVPOSY);
    old_av_lum = p.gl(gsn, PICT_AVLUM);
    old_omega = p.gl(gsn, PICT_AVOMEGA);

    act_omega = p.omega(x, y);
    act_lum = luminance(p.color(x, y));
    new_omega = old_omega + act_omega;
    p.gl(gsn, PICT_AVLUM) = (old_av_lum * old_omega + act_lum * act_omega) / new_omega;

    temp_av_posx = (old_av_posx * old_omega * old_av_lum + x * act_omega * act_lum) /
                   (old_av_lum * old_omega + act_lum * act_omega);
    p.gl(gsn, PICT_AVPOSX) = temp_av_posx;
    temp_av_posy = (old_av_posy * old_omega * old_av_lum + y * act_omega * act_lum) /
                   (old_av_lum * old_omega + act_lum * act_omega);
    p.gl(gsn, PICT_AVPOSY) = temp_av_posy;
    if (std::isnan(p.gl(gsn, PICT_AVPOSX)))
        io.errf("error in add_pixel_to_gs %d %d %f %f %f %f\n", x, y, old_av_posy, old_omega, act_omega, new_omega);

    p.gl(gsn, PICT_AVOMEGA) = new_omega;
    p.omegaVersion++;
    p.gsn[p.idx(x, y)] = gsn;
    if (p.grid)
        p.grid->add(p.cdir(x, y), gsn);
    if (act_lum < p.gl(gsn, PICT_LMIN))
        p.gl(gsn, PICT_LMIN) = act_lum;
    if (act_lum > p.gl(gsn, PICT_LMAX))
        p.gl(gsn, PICT_LMAX) = act_lum;
    p.box[static_cast<size_t>(gsn)].add(x, y);
}

// subroutine for peak extraction. NOTE: the original limits the x range by the picture HEIGHT
// (x_max = ysize-1); kept. Columns beyond the picture width (tall pictures) are outside the
// original's memory (undefined) and are treated as outside the radius.
int find_split(Pict &p, int x, int y, double r, int i_split_start, int i_split_end) {
    int i_find_split, x_min, x_max, y_min, y_max, ix, iy, iix, iiy, dx, dy, out_r;
    const AcosLeq within(r, 2.0);
    const double *d0 = p.cdir(x, y);

    i_find_split = 0;
    x_min = 0;
    y_min = 0;
    x_max = p.yr - 1;
    y_max = p.yr - 1;
    // port: no pixel of a source in [i_split_start, i_split_end] near this direction -> 0
    // (the direction grid of find_near_pgs() uses cells for the larger merge radius)
    if (p.grid && r / 2.0 <= static_cast<double>(p.gridRadius) / 2.0 &&
            !p.grid->any(d0, [&](int g) { return g >= i_split_start && g <= i_split_end; }))
        return 0;
    // port: with a single candidate source the result is that source as soon as one of its
    // pixels is visited (the original returns the last match, which then has the same number)
    const bool singleCandidate = (i_split_start == i_split_end);

    for (iiy = 1; iiy <= 2; iiy++) {
        dy = iiy * 2 - 3;
        if (dy == -1)
            iy = y;
        else
            iy = y + 1;
        while (iy <= y_max && iy >= y_min) {
            out_r = 0;
            for (iix = 1; iix <= 2; iix++) {
                dx = iix * 2 - 3;
                if (dx == -1)
                    ix = x;
                else
                    ix = x + 1;
                while (ix <= x_max && ix >= x_min && iy >= y_min) {
                    const bool inPicture = ix < p.xr;
                    if (inPicture && within(DOT(d0, p.cdir(ix, iy)))) {
                        out_r = 1;
                        const int g = p.gsn[p.idx(ix, iy)];
                        if (g >= i_split_start && g <= i_split_end) {
                            i_find_split = g;
                            if (singleCandidate)
                                return i_find_split;
                        }
                    } else {
                        ix = -99;
                    }
                    ix = ix + dx;
                }
            }
            if (out_r == 0)
                iy = -99;
            iy = iy + dy;
        }
    }
    return i_find_split;
}

// subroutine to find nearby glare source pixels (and to merge glare sources)
int find_near_pgs(Pict &p, int x, int y, float r, int act_gsn, int max_gsn, int min_gsn, Io &io) {
    int dx, dy, i_near_gs, x_min, x_max, y_min, y_max, ix, iy, iix, iiy, old_gsn, new_gsn,
        find_gsn, change, stop_y_search, stop_x_search;
    int ixm[3];
    const AcosLeq within(static_cast<double>(r), 2.0);
    const double *d0 = p.cdir(x, y);

    i_near_gs = 0;
    stop_y_search = 0;
    stop_x_search = 0;
    x_min = 0;
    y_min = 0;
    if (act_gsn == 0)
        x_max = x;
    else
        x_max = p.xr - 1;
    y_max = p.yr - 1;

    old_gsn = p.gsn[p.idx(x, y)];
    new_gsn = old_gsn;
    change = 0;
    if (act_gsn > 0) {
        i_near_gs = p.gsn[p.idx(x, y)];
        // port: the walk below can only change something if a glare source within
        // [min_gsn, max_gsn] has a larger solid angle than old_gsn; otherwise it has no effect.
        const double oldOmega = p.gl(old_gsn, PICT_AVOMEGA);
        if (!(p.maxOmega(min_gsn, max_gsn) > oldOmega))
            return i_near_gs;
        // port: skip the walk if no source that could qualify has pixels near this direction
        const double rho = static_cast<double>(r) / 2.0;
        if (rho > 1e-4 && rho < 1.0) {
            if (!p.grid || p.gridRadius != r) {
                p.grid.reset(new DirGrid());
                p.grid->inv = 1.0 / (rho * (1.0 + 1e-6) + 1e-9);
                p.gridRadius = r;
                for (int gx = 0; gx < p.xr; ++gx)
                    for (int gy = 0; gy < p.yr; ++gy) {
                        const int g = p.gsn[p.idx(gx, gy)];
                        if (g > 0)
                            p.grid->add(p.cdir(gx, gy), g);
                    }
            }
            const bool candidate = p.grid->any(d0, [&](int g) {
                return g >= min_gsn && g <= max_gsn && g < static_cast<int>(p.gli.size()) &&
                       p.gl(g, PICT_AVOMEGA) > oldOmega;
            });
            if (!candidate)
                return i_near_gs;
        }
    } else if (max_gsn < 1) {
        return 0;  // port: no glare source exists yet, the walk cannot find one
    }
    for (iiy = 1; iiy <= 2; iiy++) {
        dy = iiy * 2 - 3;
        if (dy == -1)
            iy = y;
        else
            iy = y + 1;
        ixm[1] = x;
        ixm[2] = x;
        stop_y_search = 0;
        // (once stop_y_search is set the original keeps iterating the remaining rows without
        // doing anything; leaving the loop is equivalent)
        while (iy <= y_max && iy >= y_min && stop_y_search == 0) {
            for (iix = 1; iix <= 2; iix++) {
                dx = iix * 2 - 3;
                ix = (ixm[1] + ixm[2]) / 2;
                stop_x_search = 0;
                while (ix <= x_max && ix >= x_min && stop_x_search == 0 && stop_y_search == 0) {
                    if (within(DOT(d0, p.cdir(ix, iy)))) {
                        const int g = p.gsn[p.idx(ix, iy)];
                        if (g > 0) {
                            if (act_gsn == 0) {
                                i_near_gs = g;
                                stop_x_search = 1;
                                stop_y_search = 1;
                            } else {
                                find_gsn = g;
                                if (p.gl(old_gsn, PICT_AVOMEGA) < p.gl(find_gsn, PICT_AVOMEGA) &&
                                        p.gl(find_gsn, PICT_AVOMEGA) > p.gl(new_gsn, PICT_AVOMEGA) &&
                                        find_gsn >= min_gsn && find_gsn <= max_gsn) {
                                    new_gsn = find_gsn;
                                    change = 1;
                                    stop_x_search = 1;
                                    stop_y_search = 1;
                                }
                            }
                        }
                    } else {
                        ixm[iix] = ix - dx;
                        stop_x_search = 1;
                    }
                    ix = ix + dx;
                }
            }
            iy = iy + dy;
        }
    }
    if (change > 0) {
        p.gl(old_gsn, PICT_AVLUM) = 0.;
        p.gl(old_gsn, PICT_AVOMEGA) = 0.;
        p.omegaVersion++;
        p.gl(old_gsn, PICT_NPIX) = 0.;
        p.gl(old_gsn, PICT_LMAX) = 0.;
        i_near_gs = new_gsn;
        // the original scans the whole picture (x-major); only the bounding box of old_gsn can
        // contain its pixels, so the scan is restricted to it (same order, same result). The
        // validity test of the original uses the search centre (x,y), which is always valid here.
        const GsBox b = p.box[static_cast<size_t>(old_gsn)];
        if (!b.empty() && p.valid(x, y)) {
            for (int xx = b.x0; xx <= b.x1; xx++)
                for (int yy = b.y0; yy <= b.y1; yy++) {
                    if (p.gsn[p.idx(xx, yy)] == old_gsn)
                        add_pixel_to_gs(p, xx, yy, new_gsn, io);
                }
        }
    }
    return i_near_gs;
}

// subroutine for the calculation of the task luminance (iy outer loop, no validity test, like the
// original)
double get_task_lum(Pict &p, int x, int y, float r, int task_color, std::vector<unsigned char> *member) {
    int x_min, x_max, y_min, y_max, ix, iy;
    double av_lum, omega_sum, act_lum;
    const AcosLeq within(static_cast<double>(r), 2.0);
    double d0[3];
    VCOPY(d0, p.cdir(x, y));

    x_max = p.xr - 1;
    y_max = p.yr - 1;
    x_min = 0;
    y_min = 0;
    av_lum = 0.0;
    omega_sum = 0.0;
    for (iy = y_min; iy <= y_max; iy++) {
        for (ix = x_min; ix <= x_max; ix++) {
            if (within(DOT(d0, p.cdir(ix, iy)))) {
                if (member)
                    (*member)[static_cast<size_t>(iy) * static_cast<size_t>(p.xr) + static_cast<size_t>(ix)] = 1;
                float *c = p.color(ix, iy);
                act_lum = luminance(c);
                av_lum += p.omega(ix, iy) * act_lum;
                omega_sum += p.omega(ix, iy);
                if (task_color == 1) {
                    c[0] = 0.0f;
                    c[1] = 0.0f;
                    c[2] = static_cast<float>(act_lum / WHTEFFICACY / CIE_bf);
                }
            }
        }
    }
    av_lum = av_lum / omega_sum;
    return av_lum;
}

// subroutine for coloring the glare sources
int setglcolor(Pict &p, int x, int y, int acol, int uniform_gs, double u_r, double u_g, double u_b) {
    int icol;
    double act_lum, l;
    double pr, pg, pb;

    l = u_r+u_g+u_b;
    icol = acol;
    if (acol == -1) {
        icol = 999;
    } else {
        if (acol > 0)
            icol = acol % 5 + 1;
    }
    if (uniform_gs == 1)
        icol = 998;
    switch (icol) {
        case 0: pr = 1.0 / CIE_rf; pg = 0.0 / CIE_gf; pb = 0.0 / CIE_bf; break;
        case 1: pr = 0.0 / CIE_rf; pg = 1.0 / CIE_gf; pb = 0.0 / CIE_bf; break;
        case 2: pr = 0.15 / CIE_rf; pg = 0.15 / CIE_gf; pb = 0.7 / CIE_bf; break;
        case 3: pr = .5 / CIE_rf; pg = .5 / CIE_gf; pb = 0.0 / CIE_bf; break;
        case 4: pr = .5 / CIE_rf; pg = .0 / CIE_gf; pb = .5 / CIE_bf; break;
        case 5: pr = .0 / CIE_rf; pg = .5 / CIE_gf; pb = .5 / CIE_bf; break;
        case 6: pr = 0.333 / CIE_rf; pg = 0.333 / CIE_gf; pb = 0.333 / CIE_bf; break;
        case 999: pr = 1.0 / WHTEFFICACY; pg = 1.0 / WHTEFFICACY; pb = 1.0 / WHTEFFICACY; break;
        case 998: pr = u_r /(l* CIE_rf); pg = u_g /(l* CIE_gf); pb = u_b /(l* CIE_bf); break;
        default: pr = pg = pb = 0.0; break;  // (uninitialised in the original; not reachable)
    }
    float *c = p.color(x, y);
    act_lum = luminance(c);
    c[0] = static_cast<float>(pr * act_lum / WHTEFFICACY);
    c[1] = static_cast<float>(pg * act_lum / WHTEFFICACY);
    c[2] = static_cast<float>(pb * act_lum / WHTEFFICACY);
    return icol;
}

// subroutine for removing a pixel from a glare source
void split_pixel_from_gs(Pict &p, int x, int y, int new_gsn, Io &io) {
    int old_gsn;
    double old_av_posx, old_av_posy, old_av_lum, old_omega, act_omega, new_omega, act_lum,
        temp_av_posx, temp_av_posy;

    old_gsn = p.gsn[p.idx(x, y)];
    p.gl(old_gsn, PICT_NPIX) = p.gl(old_gsn, PICT_NPIX) - 1;
    act_omega = p.omega(x, y);
    old_av_posx = p.gl(old_gsn, PICT_AVPOSX);
    old_av_posy = p.gl(old_gsn, PICT_AVPOSY);
    old_omega = p.gl(old_gsn, PICT_AVOMEGA);
    new_omega = old_omega - act_omega;
    p.gl(old_gsn, PICT_AVOMEGA) = new_omega;
    p.omegaVersion++;
    act_lum = luminance(p.color(x, y));
    old_av_lum = p.gl(old_gsn, PICT_AVLUM);
    p.gl(old_gsn, PICT_AVLUM) = (old_av_lum * old_omega - act_lum * act_omega) / new_omega;
    temp_av_posx = (old_av_posx *old_av_lum* old_omega - x *act_lum* act_omega) /
                   (old_av_lum*old_omega - act_lum* act_omega);
    p.gl(old_gsn, PICT_AVPOSX) = temp_av_posx;
    temp_av_posy = (old_av_posy *old_av_lum* old_omega - y *act_lum* act_omega) /
                   (old_av_lum*old_omega - act_lum* act_omega);
    p.gl(old_gsn, PICT_AVPOSY) = temp_av_posy;
    add_pixel_to_gs(p, x, y, new_gsn, io);
}

// subroutine for the calculation of the position index. The coordinates are passed as float
// and truncated to int by pict_get_dir() (the source centroid is truncated, not rounded).
// If no direction exists at the position the original uses uninitialised variables; 0 is used.
float get_posindex(Pict &p, float x, float y, int postype) {
    float posindex;
    double teta = 0.0, beta, phi = 0.0, sigma = 0.0, tau = 0.0, deg;

    if (p.viewFixed) {
        // port: once normalize() no longer changes the view vectors the four pictool calls have
        // no side effects; evaluate them with the same operations but without the repeated
        // direction lookups (pict_get_tau() recomputes sigma) and the unused horizontal angle.
        double pvdir[3];
        if (pictGetDir(p, truncCoord(x), truncCoord(y), pvdir)) {
            double hv[3], vv[3], h[3];
            fcross(hv, p.view.vdir, p.view.vup);
            fcross(vv, p.view.vdir, hv);
            phi = std::acos(DOT(vv, pvdir)) - (kMPi/2.0);
            sigma = std::acos(DOT(p.view.vdir, pvdir));
            VCOPY(h, pvdir);
            normalize(h);
            const double cs = std::cos(sigma);
            for (int i = 0; i < 3; i++)
                h[i] /= cs;
            h[0] = h[0] - p.view.vdir[0];
            h[1] = h[1] - p.view.vdir[1];
            h[2] = h[2] - p.view.vdir[2];
            normalize(h);
            tau = std::acos(DOT(p.view.vup, h));
            teta = 1.0;  // (only tested against 0 in the original)
        }
    } else {
        pictGetVangle(p, x, y, &phi);
        pictGetHangle(p, x, y, &teta);
        pictGetSigma(p, x, y, &sigma);
        pictGetTau(p, x, y, &tau);
    }

    deg = 180 / 3.1415927;
    if (phi == 0)
        phi = 0.00001;
    if (sigma <= 0)
        sigma = -sigma;
    if (teta == 0)
        teta = 0.0001;
    tau = tau * deg;
    sigma = sigma * deg;

    if (postype == 1) {
        // KIM model
        posindex = static_cast<float>(std::exp((sigma-(-0.000009*tau*tau*tau+0.0014*tau*tau+0.0866*tau+21.633)) /
                                               (-0.000009*tau*tau*tau+0.0013*tau*tau+0.0853*tau+8.772)));
    } else {
        // Guth model, equation from IES lighting handbook
        posindex = static_cast<float>(std::exp((35.2 - 0.31889 * tau - 1.22 * std::exp(-2 * tau / 9)) / 1000 * sigma +
                                               (21 + 0.26667 * tau - 0.002963 * tau * tau) / 100000 * sigma * sigma));
        // below line of sight, using Iwata model, CIE2010, converted coordinate system
        if (phi < 0) {
            beta = std::atan(std::tan(sigma/deg) * std::sqrt(1 + 0.3225 * std::pow(std::cos(tau/deg), 2))) * deg;
            posindex = static_cast<float>(std::exp(6.49 / 1000 * beta + 21.0 / 100000 * beta * beta));
        }
        if (posindex > 16)
            posindex = 16;
    }
    return posindex;
}

double get_upper_cut_2eyes(float teta) {
    return std::pow(7.7458218+0.00057407915*teta-0.00021746318*teta*teta+8.5572726e-6*teta*teta*teta, 2);
}
double get_lower_cut_2eyes(float teta) {
    return 1/(-0.014699242-1.5541106e-5*teta+4.6898068e-6*teta*teta-5.1539687e-8*teta*teta*teta);
}
double get_lower_cut_central(float teta) {
    double phi = (68.227109-2.9524084*teta+0.046674262*teta*teta) /
                 (1-0.042317294*teta+0.00075698419*teta*teta-6.5364285e-7*teta*teta*teta);
    if (teta > 73)
        phi = 60;
    return phi;
}

inline void setGrey(float *c, double v) {
    c[0] = static_cast<float>(v);
    c[1] = static_cast<float>(v);
    c[2] = static_cast<float>(v);
}

// cut_view_1 / cut_view_2 / cut_view_3 (cutting the field of view)
void cutView(Pict &p, int type) {
    double ang, teta = 0.0, phi = 0.0, phi2 = 0.0, border, lum, newlum;
    for (int x = 0; x < p.xr; x++)
        for (int y = 0; y < p.yr; y++) {
            if (!pictGetHangle(p, x, y, &ang))
                continue;
            float *c = p.color(x, y);
            const bool cond3 = DOT(p.cdir(x, y), p.view.vdir) >= 0.0;
            if ((type == 3) ? cond3 : p.valid(x, y)) {
                pictGetVangle(p, x, y, &phi2);
                if (type != 1)
                    pictGetHangle(p, x, y, &teta);
                pictGetSigma(p, x, y, &phi);
                pictGetTau(p, x, y, &teta);
                phi = phi*180/3.1415927;
                phi2 = phi2*180/3.1415927;
                teta = teta*180/3.1415927;
                if (type == 3) {
                    lum = luminance(c);
                    newlum = lum/get_posindex(p, x, y, 0);
                    setGrey(c, newlum/WHTEFFICACY);
                }
                if (teta < 0)
                    teta = -teta;
                if (type == 1) {
                    if (phi2 > 0) {
                        border = get_upper_cut_2eyes(static_cast<float>(teta));
                        if (phi > border)
                            setGrey(c, 0);
                    } else {
                        border = get_lower_cut_2eyes(static_cast<float>(180-teta));
                        if (-phi < border && teta > 135)
                            setGrey(c, 0);
                    }
                } else {
                    if (phi2 > 0) {
                        border = 60;
                        if (phi > border)
                            setGrey(c, 0);
                    } else {
                        border = get_lower_cut_central(static_cast<float>(180-teta));
                        if (phi > border)
                            setGrey(c, 0);
                    }
                }
            } else {
                setGrey(c, 0);
            }
        }
}

float get_dgi(Pict &p, float lum_backg, int igs, const std::vector<float> &pos) {
    float dgi, sum_glare, omega_s;
    sum_glare = 0;
    omega_s = 0;
    for (int i = 0; i <= igs; i++) {
        if (p.gl(i, PICT_NPIX) > 0) {
            omega_s = static_cast<float>(p.gl(i, PICT_AVOMEGA) / pos[i] / pos[i]);
            sum_glare = static_cast<float>(sum_glare + 0.478 * std::pow(p.gl(i, PICT_AVLUM), 1.6) * std::pow(omega_s, 0.8) /
                        (lum_backg + 0.07 * std::pow(p.gl(i, PICT_AVOMEGA), 0.5) * p.gl(i, PICT_AVLUM)));
        }
    }
    dgi = static_cast<float>(10 * std::log10(static_cast<double>(sum_glare)));
    return dgi;
}

float get_dgi_mod(Pict &p, float lum_a, int igs, const std::vector<float> &pos) {
    float dgi_mod, sum_glare, omega_s;
    sum_glare = 0;
    omega_s = 0;
    for (int i = 0; i <= igs; i++) {
        if (p.gl(i, PICT_NPIX) > 0) {
            omega_s = static_cast<float>(p.gl(i, PICT_AVOMEGA) / pos[i] / pos[i]);
            sum_glare = static_cast<float>(sum_glare + 0.478 * std::pow(p.gl(i, PICT_AVLUM), 1.6) * std::pow(omega_s, 0.8) /
                        (std::pow(lum_a, 0.85) + 0.07 * std::pow(p.gl(i, PICT_AVOMEGA), 0.5) * p.gl(i, PICT_AVLUM)));
        }
    }
    dgi_mod = static_cast<float>(10 * std::log10(static_cast<double>(sum_glare)));
    return dgi_mod;
}

double get_dgp(Pict &p, double E_v, int igs, double a1, double a2, double a3, double a4, double a5,
               double c1, double c2, double c3, const std::vector<float> &pos) {
    double dgp;
    double sum_glare = 0;
    if (igs > 0) {
        for (int i = 0; i <= igs; i++) {
            if (p.gl(i, PICT_NPIX) > 0) {
                sum_glare += std::pow(p.gl(i, PICT_AVLUM), a1) / std::pow(static_cast<double>(pos[i]), a4) *
                             std::pow(p.gl(i, PICT_AVOMEGA), a2);
            }
        }
        dgp = c1 * std::pow(E_v, a5) + c3 + c2 * std::log10(1 + sum_glare / std::pow(E_v, a3));
    } else {
        dgp = c3 + c1 * std::pow(E_v, a5);
    }
    if (dgp > 1)
        dgp = 1;
    return dgp;
}

float get_dgr(Pict &p, double lum_a, int igs, const std::vector<float> &pos) {
    float dgr;
    double sum_glare = 0;
    int i_glare = 0;
    for (int i = 0; i <= igs; i++) {
        if (p.gl(i, PICT_NPIX) > 0) {
            i_glare = i_glare + 1;
            sum_glare += (0.5 * p.gl(i, PICT_AVLUM) *
                          (20.4 * p.gl(i, PICT_AVOMEGA) + 1.52 * std::pow(p.gl(i, PICT_AVOMEGA), 0.2) - 0.075)) /
                         (pos[i] * std::pow(lum_a, 0.44));
        }
    }
    dgr = static_cast<float>(std::pow(sum_glare, std::pow(i_glare, -0.0914)));
    return dgr;
}

float get_vcp(float dgr) {
    float vcp;
    vcp = static_cast<float>(50 * std::erf((6.374 - 1.3227 * std::log(static_cast<double>(dgr))) / 1.414213562373) + 50);
    if (dgr > 750)
        vcp = 0;
    if (dgr < 20)
        vcp = 100;
    return vcp;
}

float get_ugr(Pict &p, double lum_backg, int igs, const std::vector<float> &pos) {
    float ugr;
    double sum_glare = 0;
    for (int i = 0; i <= igs; i++) {
        if (p.gl(i, PICT_NPIX) > 0)
            sum_glare += std::pow(p.gl(i, PICT_AVLUM) / pos[i], 2) * p.gl(i, PICT_AVOMEGA);
    }
    ugr = static_cast<float>(8 * std::log10(0.25 / lum_backg * sum_glare));
    if (sum_glare == 0)
        ugr = 0.0;
    if (lum_backg <= 0)
        ugr = -99.0;
    return ugr;
}

float get_ugr_exp(Pict &p, double lum_backg, double lum_a, int igs, const std::vector<float> &pos) {
    float ugr_exp;
    double sum_glare = 0;
    for (int i = 0; i <= igs; i++) {
        if (p.gl(i, PICT_NPIX) > 0)
            sum_glare += std::pow(1 / pos[i], 2) * p.gl(i, PICT_AVLUM) * p.gl(i, PICT_AVOMEGA);
    }
    ugr_exp = static_cast<float>(8 * std::log10(lum_a) + 8 * std::log10(sum_glare/lum_backg));
    return ugr_exp;
}

float get_ugp(Pict &p, double lum_backg, int igs, const std::vector<float> &pos) {
    float ugp;
    double sum_glare = 0;
    for (int i = 0; i <= igs; i++) {
        if (p.gl(i, PICT_NPIX) > 0)
            sum_glare += std::pow(p.gl(i, PICT_AVLUM) / pos[i], 2) * p.gl(i, PICT_AVOMEGA);
    }
    ugp = static_cast<float>(0.26 * std::log10(0.25 / lum_backg * sum_glare));
    return ugp;
}

float get_ugp2(Pict &p, double lum_backg, int igs, const std::vector<float> &pos) {
    double sum_glare = 0, ugp2;
    for (int i = 0; i <= igs; i++) {
        if (p.gl(i, PICT_NPIX) > 0)
            sum_glare += std::pow(p.gl(i, PICT_AVLUM) / pos[i], 2) * p.gl(i, PICT_AVOMEGA);
    }
    ugp2 = 1/std::pow(1.0+2.0/7.0*std::pow(sum_glare/lum_backg, -0.2), 10.0);
    return static_cast<float>(ugp2);
}

// disability glare according to Poynter. sigmaAt[i]: pict_get_sigma() at the (double) centroid;
// when it fails the original keeps the previous loop value.
float get_disability(Pict &p, double lum_backg, int igs, const std::vector<double> &sigmaAt,
                     const std::vector<unsigned char> &sigmaOk, Io &io) {
    (void)lum_backg;
    float disab;
    double sum_glare = 0, sigma = 0, deg;
    deg = 180 / 3.1415927;
    for (int i = 0; i <= igs; i++) {
        if (p.gl(i, PICT_NPIX) > 0) {
            if (sigmaOk[i])
                sigma = sigmaAt[i];
            sum_glare += p.gl(i, PICT_AVLUM) * std::cos(sigma + 0.00000000001) * p.gl(i, PICT_AVOMEGA) /
                         (deg * sigma + 0.00000000001);
            if (std::isnan(sum_glare)) {
                io.outf("sigma for %f %f\n", p.gl(i, PICT_AVPOSX), p.gl(i, PICT_AVPOSY));
                io.outf("omega for %f %f\n", p.gl(i, PICT_AVPOSX), p.gl(i, PICT_AVPOSY));
                io.outf("avlum for %f %f\n", p.gl(i, PICT_AVPOSX), p.gl(i, PICT_AVPOSY));
                io.outf("avlum for %f %f %f\n", p.gl(i, PICT_AVPOSX), p.gl(i, PICT_AVPOSY), sigma);
            }
        }
    }
    disab = static_cast<float>(5 * sum_glare);
    return disab;
}

float get_cgi(Pict &p, double E_v, double E_v_dir, int igs, const std::vector<float> &pos) {
    float cgi;
    double sum_glare = 0;
    for (int i = 0; i <= igs; i++) {
        if (p.gl(i, PICT_NPIX) > 0)
            sum_glare += std::pow(p.gl(i, PICT_AVLUM) / pos[i], 2) * p.gl(i, PICT_AVOMEGA);
    }
    cgi = static_cast<float>(8 * std::log10((2 * (1 + E_v_dir / 500) / E_v) * sum_glare));
    return cgi;
}

float get_pgsv_con(double E_v, double E_mask, double omega_mask, double lum_mask_av, double Lavg) {
    (void)E_v;
    (void)E_mask;
    double Lb = (2*3.14159265359*Lavg-lum_mask_av*omega_mask)/(2*3.14159265359-omega_mask);
    return static_cast<float>(3.2*std::log10(lum_mask_av)-0.64*std::log10(omega_mask)+
                              (0.79*std::log10(omega_mask)-0.61)*std::log10(Lb)-8.2);
}

float get_pgsv_sat(double E_v) {
    return static_cast<float>(3.3-(0.57+3.3)/(1+std::pow(E_v/3.14159265359/1250, 1.7)));
}

float get_pgsv(double E_v, double E_mask, double omega_mask, double lum_mask_av, double Ltask, double Lavg, Io &io) {
    float pgsv;
    double Lb = (2*3.14159265359*Lavg-lum_mask_av*omega_mask)/(2*3.14159265359-omega_mask);
    if (Lb == 0.0) {
        io.errf(" warning: Background luminance is 0 or masking area = full image! pgsv cannot be calculated (set to -99)!!\n");
        pgsv = -99;
    } else {
        if ((lum_mask_av/Lb) > (E_v/(3.14159265359*Ltask)))
            pgsv = get_pgsv_con(E_v, E_mask, omega_mask, lum_mask_av, Lavg);
        else
            pgsv = get_pgsv_sat(E_v);
    }
    return pgsv;
}

} // namespace eg

namespace eg {

// ---------------------------------------------------------------------------------------
// Picture input (pictool pict_read): header (views, exposure), orientation, grey conversion
// ---------------------------------------------------------------------------------------
struct PictHeader {
    RadView view;
    int viewLines = 0;              // hi.ok
    const char *viewError = nullptr;
    double exposure = 1.0;
    bool invalidExposure = false;   // "EXPOSURE=" together with a tab -> the original stops
};

// gethinfo() applied to every header line; the view starts from pict_init()'s stdview + setview()
PictHeader parsePictHeader(const HdrImage &img) {
    PictHeader h;
    setview(h.view);  // pict_init(): p->view = stdview; pict_update_view(p)
    const std::vector<std::string> fallback = img.headerLines;
    const std::vector<std::string> &lines = img.rawHeaderLines.empty() ? fallback : img.rawHeaderLines;
    forEachHeaderLine(lines, [&](const char *s) {
        if (h.invalidExposure)
            return;
        if (std::strstr(s, "EXPOSURE=") != nullptr && std::strstr(s, "\t") != nullptr) {
            h.invalidExposure = true;
            return;
        }
        if (isview(s) && sscanview(h.view, s) > 0) {
            h.viewLines++;
        } else if (!std::strncmp(s, "EXPOSURE=", 9)) {
            h.exposure *= std::atof(s + 9);
        }
    });
    h.viewError = setview(h.view);  // pict_update_view()
    return h;
}

void throwInvalidExposure(Io &io) {
    io.errf("error: header contains invalid exposure entry!!!!\n");
    io.errf("check exposure and correct header setting !\n");
    io.errf("stopping !!!!\n");
    throw EvalGlareExit{1};
}

// Luminance weights of the port (header-aware). Plain Radiance pictures use the exact CIE
// coefficients of the original.
struct LuminanceWeights {
    std::array<double, 3> w{{CIE_rf, CIE_gf, CIE_bf}};
    std::string description;
};

LuminanceWeights resolveGlareWeights(const HdrImage &img) {
    LuminanceWeights lw;
    const HeaderLuminanceInfo info = resolveLuminanceInfo(img);
    if (info.mode == HeaderLuminanceInfo::Unavailable)
        throw std::runtime_error("Could not determine luminance conversion for this image. " + info.description);
    lw.description = info.description;
    if (info.mode == HeaderLuminanceInfo::Weighted &&
            info.description != "Plain Radiance RGB fallback" &&
            info.description != "Technoteam/LMK Radiance fallback") {
        lw.w = info.weights;
    }
    // Monochrome pictures (r == g == b) and the Radiance fallbacks use the original coefficients;
    // for grey pixels this is identical to using the green channel.
    return lw;
}

// Grey picture values of pict_read() + pict_update_evalglare_caches():
// col = colr * (1/exposure) (float), lum = luminance(col)/179 (float)
std::vector<float> greyValues(const HdrImage &img, double exposure, const std::array<double, 3> &w,
                              const std::vector<double> *equivalentLuminance) {
    const int xr = static_cast<int>(img.width);
    const int yr = static_cast<int>(img.height);
    std::vector<float> grey(static_cast<size_t>(xr) * static_cast<size_t>(yr));
    const double sf = 1.0/exposure;
#if defined(_OPENMP)
#pragma omp parallel for schedule(static)
#endif
    for (int y = 0; y < yr; ++y) {
        for (int x = 0; x < xr; ++x) {
            const size_t src = static_cast<size_t>(y) * static_cast<size_t>(xr) + static_cast<size_t>(x);
            const size_t dst = static_cast<size_t>(x) * static_cast<size_t>(yr) + static_cast<size_t>(y);
            if (equivalentLuminance) {
                grey[dst] = static_cast<float>((*equivalentLuminance)[src] / 179.0);
                continue;
            }
            float c[3];
            c[0] = static_cast<float>(img.rgb[src*3 + 0] * sf);
            c[1] = static_cast<float>(img.rgb[src*3 + 1] * sf);
            c[2] = static_cast<float>(img.rgb[src*3 + 2] * sf);
            grey[dst] = static_cast<float>((WHTEFFICACY * (w[0]*c[0] + w[1]*c[1] + w[2]*c[2])) / 179.0);
        }
    }
    return grey;
}

struct SimplePict {      // mask and multi-image pictures: grey colours only
    int xr = 0;
    int yr = 0;
    std::vector<float> grey;  // column major
};

HdrImage readPictureFile(const std::string &path) {
    if (path == "-") {
#if defined(_WIN32)
        throw std::runtime_error("Reading the picture from stdin is not supported on this platform.");
#else
        return readRadianceHDR("/dev/stdin");
#endif
    }
    return readRadianceHDR(path);
}

SimplePict readSimplePict(const std::string &path, bool headerAwareWeights, Io &io) {
    HdrImage img = readPictureFile(path);
    PictHeader h = parsePictHeader(img);
    if (h.invalidExposure)
        throwInvalidExposure(io);
    if (h.viewError)
        io.errf("pict_update_view: %s\n", h.viewError);
    SimplePict sp;
    sp.xr = static_cast<int>(img.width);
    sp.yr = static_cast<int>(img.height);
    std::array<double, 3> w{{CIE_rf, CIE_gf, CIE_bf}};
    if (headerAwareWeights)
        w = resolveGlareWeights(img).w;
    sp.grey = greyValues(img, h.exposure, w, nullptr);
    return sp;
}

// pictool pict_write(): "#?RADIANCE", date, comment, VIEW, FORMAT, resolution, scanlines
void pictWrite(const Pict &p, const std::string &path, Io &io) {
    if (path.empty()) {
        io.errf("Can't open %s for writing\n", "");
        return;
    }
    HdrImage out;
    out.width = static_cast<size_t>(p.xr);
    out.height = static_cast<size_t>(p.yr);
    out.rgb.resize(out.width * out.height * 3);
    for (int x = 0; x < p.xr; ++x)
        for (int y = 0; y < p.yr; ++y) {
            const float *c = p.color(x, y);
            float *d = &out.rgb[(static_cast<size_t>(y) * out.width + static_cast<size_t>(x)) * 3];
            d[0] = c[0];
            d[1] = c[1];
            d[2] = c[2];
        }
    std::vector<std::string> header;
    const std::time_t now = std::time(nullptr);
    char buf[64];
    if (const std::tm *lt = std::localtime(&now)) {
        std::snprintf(buf, sizeof(buf), "CAPDATE= %04d:%02d:%02d %02d:%02d:%02d", lt->tm_year+1900, lt->tm_mon+1,
                      lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
        header.push_back(buf);
    }
    if (const std::tm *gt = std::gmtime(&now)) {
        std::snprintf(buf, sizeof(buf), "GMT= %04d:%02d:%02d %02d:%02d:%02d", gt->tm_year+1900, gt->tm_mon+1,
                      gt->tm_mday, gt->tm_hour, gt->tm_min, gt->tm_sec);
        header.push_back(buf);
    }
    std::string comment = p.comment;
    while (!comment.empty()) {
        const size_t nl = comment.find('\n');
        header.push_back(comment.substr(0, nl));
        if (nl == std::string::npos)
            break;
        comment = comment.substr(nl + 1);
    }
    header.push_back("VIEW=" + fprintview(p.view));
    try {
        writeRadianceHDROriented(path, out, header, p.rt);
    } catch (const std::exception &) {
        io.errf("Can't open %s for writing\n", path.c_str());
    }
}

// ---------------------------------------------------------------------------------------
// The evalglare main program
// ---------------------------------------------------------------------------------------
struct RunState {
    const EvalGlareOptions &o;
    EvalGlareAnalysisResult &res;
    Io &io;
};

int convertY(const EvalGlareOptions &o, int y, int height) {
    return o.yFromBottom ? y : height - 1 - y;
}

void runMain(const EvalGlareOptions &o, EvalGlareAnalysisResult &res, Io &io) {
    Pict pict;
    Pict *p = &pict;
    // ---- program options (variables of the original) ----
    int lowlight = o.lowLightCorrection ? 1 : 0;
    int multi_image_mode = o.multiImageMode ? 1 : 0;
    double dir_ill = o.directIlluminance;
    double delta_E = 0.0;
    int no_glaresources = 0;
    double n_corner_px = 0;
    double zero_corner_px = 0;
    int force = o.force ? 1 : 0;
    double dist = 0.0;
    double u_r = o.uniformRed, u_g = o.uniformGreen, u_b = o.uniformBlue;
    int uniform_gs = o.uniformSourceColor ? 1 : 0;
    double band_angle = o.bandAngle;
    int band_calc = (o.bandRequested || o.bandAngle > 0.0) ? 1 : 0;
    int band_color = band_calc;
    int zones = o.zoneCount;
    double angle_z1 = 0, angle_z2 = 0;
    int x_zone = 0, y_zone = 0;
    double per_75_z2 = 0, per_95_z2 = 0;
    double lum_pos_mean = 0, lum_pos2_mean = 0;
    double lum_band_av = 0.0, omega_band = 0.0;
    float pgsv = 0.0f, pgsv_con = 0.0f, pgsv_sat = 0.0f;
    double E_v_mask = 0.0, Ez1 = 0.0, Ez2 = 0.0;
    double lum_z1_av = 0.0, omega_z1 = 0.0, lum_z2_av = 0.0, omega_z2 = 0.0;
    int i_z1 = 0, i_z2 = 0;
    int apply_disability = o.applyDisabilityThreshold ? 1 : 0;
    double disability_thresh = o.disabilityThreshold;
    double Lveil_cie_sum = 0.0;
    int skip_second_scan = 0;
    double lum_total_max = 0.0;
    int calcfast = 0;
    double age_corr_factor = 1.0;
    double dgp_ext = 0;
    double E_vl_ext = o.externalVerticalIlluminance;
    double new_lum_max = o.correctionNewLuminance;
    double lum_max = o.correctionLuminanceLimit;
    double omegat = 0.0;
    int xt = 0, yt = 0;
    int x_disk = o.diskX, y_disk = 0;
    double angle_disk = o.diskAngle;
    int yfillmin = 0, yfillmax = 0;
    int cut_view = 0, cut_view_type = o.cutViewType;
    double setvalue = 2e09;
    double omega_cos_contr = 0.0;
    double lum_ideal = 0.0;
    double max_angle = o.maxAngle;
    float lum_thres = static_cast<float>(o.lumThreshold);
    float lum_task = 0.0f;
    int task_lum = o.taskActive ? 1 : 0;
    int sgs = o.smoothing ? 1 : 0;
    int splithigh = o.disableSplit ? 0 : 1;
    int detail_out = (o.detailed || !o.maskPath.empty()) ? 1 : 0;
    int detail_out2 = o.detailedShort ? 1 : 0;
    int posindex_picture = o.positionIndexPicture ? 1 : 0;
    int checkfile = o.checkPath.empty() ? 0 : 1;
    int ext_vill = o.externalIlluminance ? 1 : 0;
    int fill = o.fillRows ? 1 : 0;
    const double a1 = o.dgpA1, a2 = o.dgpA2, a3 = o.dgpA3, a4 = o.dgpA4, a5 = o.dgpA5;
    const double c1 = o.dgpC1, c2 = o.dgpC2, c3 = o.dgpC3;
    int non_cos_lb = o.backgroundMode;
    int posindex_2 = o.positionIndexModel;
    int task_color = o.taskColor ? 1 : 0;
    float limit = static_cast<float>(o.splitLimit);
    int set_lum_max = o.clipLuminance ? 1 : 0;
    int set_lum_max2 = o.luminanceCorrection;
    int img_corr = (set_lum_max || set_lum_max2) ? 1 : 0;
    double abs_max = 0;
    double E_v_contr = 0.0;
    double low_light_corr = 1.0;
    int output = o.outputMode;
    int calc_vill = o.verticalIlluminanceOnly ? 1 : 0;
    double band_avlum = -99;
    (void)band_avlum;
    int masking = o.maskPath.empty() ? 0 : 1;
    double lum_mask_av = 0.0, omega_mask = 0.0;
    int i_mask = 0;
    int actual_igs = 0;
    double LUM_replace = o.replacementLuminance;
    int thres_activate = o.thresholdExplicit ? 1 : 0;
    int patchmode = o.patchMode;
    double patch_angle = o.patchAngleDegrees;
    double lum_source = lum_thres;
    int igs = 0;
    double sum_glare = 0.0;   // (uninitialised in the original)
    double lum_backg_cos = 0.0;
    double sigma = 0.0;
    int i = 0;
    int x, y;
    double lum, act_lum, r_actual, ang;
    if (o.taskActive) {
        xt = o.taskX;
        omegat = o.taskAngle;
    }
    if (zones > 0) {
        x_zone = o.zoneCenterX;
        angle_z1 = o.zoneAngle1;
        angle_z2 = zones == 1 ? -1 : o.zoneAngle2;
    }
    if (!o.cutViewWriteOnly && (o.cutViewRequested || o.cutViewType != 0))
        cut_view = 1;
    if (o.cutViewWriteOnly)
        cut_view = 2;
    if (fill) {
        yfillmax = o.fillYMax;
        yfillmin = o.fillYMin;
    }

    RandVar s_mask, s_band, s_z1, s_z2;
    const bool wantPositionStats = (output == 0 && detail_out == 1 && !o.simple);  // port: only when printed

    // set multiplier for task method to 5, if not specified
    if (task_lum == 1 && thres_activate == 0)
        lum_thres = 5.0f;
    if (multi_image_mode)
        calcfast = 1;  // (-Q also sets output = 3 while the options are parsed)
    if (output == 1 && ext_vill == 1)
        calcfast = 1;
    if (output == 2 && ext_vill == 1)
        calcfast = 2;
    if (masking == 1 && zones > 0) {
        io.errf(" masking and zoning cannot be activated at the same time!\n");
        throw EvalGlareExit{1};
    }

    // ---- mask picture (read while parsing the options in the original) ----
    SimplePict pm;
    if (masking)
        pm = readSimplePict(o.maskPath, false, io);

    // ---- read picture file ----
    PictHeader hdr;
    std::shared_ptr<std::vector<double>> equivalentMap;
    {
        HdrImage img = readPictureFile(o.inputPath);
        hdr = parsePictHeader(img);
        if (hdr.invalidExposure)
            throwInvalidExposure(io);
        if (hdr.viewError)
            io.errf("pict_update_view: %s\n", hdr.viewError);
        pict.xr = static_cast<int>(img.width);
        pict.yr = static_cast<int>(img.height);
        pict.rt = img.radianceOrientation | YMAJOR;
        pict.view = hdr.view;
        pict.validView = !(hdr.viewError != nullptr || hdr.viewLines == 0);
        const LuminanceWeights lw = resolveGlareWeights(img);
        if (o.equivLuminance)
            equivalentMap = std::make_shared<std::vector<double>>(computeEquivalentLuminanceMap(img));
        std::vector<float> grey = greyValues(img, hdr.exposure, lw.w, equivalentMap.get());
        equivalentMap.reset();
        img = HdrImage();  // release the input pixels early
        pict.col.resize(grey.size() * 3);
        for (size_t k = 0; k < grey.size(); ++k)
            pict.col[k*3] = pict.col[k*3+1] = pict.col[k*3+2] = grey[k];
    }
    const size_t npixels = static_cast<size_t>(pict.xr) * static_cast<size_t>(pict.yr);

    // ---- view handling: header view, or the command-line view (overriding it completely) ----
    RadView userview;
    int gotuserview = 0;
    userview.horiz = 0;
    userview.vert = 0;
    userview.type = 0;
    {
        std::vector<const char *> av;
        for (const std::string &s : o.viewOptions)
            av.push_back(s.c_str());
        av.push_back(nullptr);
        const int ac = static_cast<int>(o.viewOptions.size());
        for (int k = 0; k < ac; ++k) {
            if (!std::strcmp(av[k], "-vf")) {
                if (k + 1 >= ac)
                    throw std::runtime_error("evalglare: -vf needs a view file");
                const int rv = viewfile(av[k + 1], userview);
                if (rv < 0) {
                    io.errf("%s: cannot open view file \"%s\"\n", "evalglare", av[k + 1]);
                    throw EvalGlareExit{1};
                } else if (rv == 0) {
                    io.errf("%s: bad view file \"%s\"\n", "evalglare", av[k + 1]);
                    throw EvalGlareExit{1};
                }
                gotuserview++;
                ++k;
                continue;
            }
            const int rv = getviewopt(userview, ac - k, av.data() + k);
            if (rv < 0)
                throw std::runtime_error(std::string("evalglare: bad view option ") + av[k]);
            k += rv;
            gotuserview++;
        }
    }
    const RadView *fallbackView = nullptr;
    RadView headerView = pict.view;
    if (gotuserview) {
        if (pict.validView)
            io.errf("warning: overriding image view by commandline argument\n");
        if ((userview.horiz == 0) || (userview.vert == 0) || (userview.type == 0)) {
            io.errf("error: if specified, a view must at least contain -vt -vv and -vh\n");
            throw EvalGlareExit{1};
        }
        pict.view = userview;
        if (const char *err = setview(pict.view)) {
            io.errf("pict_update_view: %s\n", err);
            io.errf("error: invalid view specified");
            throw EvalGlareExit{1};
        }
        fallbackView = &headerView;
        // second pict_update_evalglare_caches(): the picture is converted to grey again
        for (size_t k = 0; k < npixels; ++k) {
            float *c = &pict.col[k*3];
            const float v = static_cast<float>(luminance(c)/179.0);
            c[0] = c[1] = c[2] = v;
        }
    } else if (!pict.validView) {
        io.errf("error: no valid view specified\n");
        throw EvalGlareExit{1};
    }
    {
        const auto geoBegin = std::chrono::steady_clock::now();
        if (o.cacheGeometry)
            pict.geo = cachedGeometry(pict.view, pict.xr, pict.yr, pict.rt, fallbackView);
        else
            pict.geo = buildGeometry(pict.view, pict.xr, pict.yr, pict.rt, fallbackView);
        logProfileStage("geometry", geoBegin, std::chrono::steady_clock::now());
    }
    for (size_t k = 0; k < pict.geo->normalErrors.size(); ++k)
        io.errf("Normal error in pict_get_sangle %f %d %d\n", pict.geo->normalErrorAngles[k],
                pict.geo->normalErrors[k].first, pict.geo->normalErrors[k].second);
    pict.liveSameAsCache = sameViewGeometry(pict.view, pict.geo->view);
    pict.gsn.assign(npixels, 0);
    pict.pgs.assign(npixels, 0);
    pict.comment = o.commandLine.empty() ? std::string("evalglare ") : o.commandLine;
    pict.comment += "\n";
    pict.comment += kReleaseName;
    pict.comment += " (mergehdr native port)\n";

    res.width = static_cast<size_t>(pict.xr);
    res.height = static_cast<size_t>(pict.yr);
    // y coordinates of the options (GUI: top-down)
    yt = convertY(o, o.taskY, pict.yr);
    y_zone = convertY(o, o.zoneCenterY, pict.yr);
    y_disk = convertY(o, o.diskY, pict.yr);
    if (fill && !o.yFromBottom) {
        yfillmax = pict.yr - 1 - o.fillYMax;
        yfillmin = pict.yr - 1 - o.fillYMin;
    }

    // ---- several checks ----
    if (p->view.type == VT_PAR) {
        io.errf("error: wrong view type! must not be parallel ! \n");
        throw EvalGlareExit{1};
    }
    if (patchmode > 0 && p->view.type != VT_ANG) {
        io.errf("error: Patchmode only possible with view type vta  !  Stopping... \n");
        throw EvalGlareExit{1};
    }
    if (p->view.type == VT_PER)
        io.errf("warning: image has perspective view type specified in header ! \n");
    if (masking == 1) {
        if (p->xr != pm.xr || p->yr != pm.yr) {
            io.errf("error: masking image has other resolution than main image ! \n");
            io.errf("size must be identical \n");
            io.outf("resolution main image : %dx%d\n", p->xr, p->yr);
            io.outf("resolution masking image : %dx%d\n", pm.xr, pm.yr);
            throw EvalGlareExit{1};
        }
    }

    // ---- check size of search radius ----
    int rmx = (p->xr / 2);
    int rmy = (p->yr / 2);
    int rx = (p->xr / 2 + 10);
    int ry = (p->yr / 2 + 10);
    double r_center;
    {
        const double zero[3] = {0, 0, 0};
        const double *dc = p->inside(rmx, rmy) ? p->cdir(rmx, rmy) : zero;
        const double *dr = p->inside(rx, ry) ? p->cdir(rx, ry) : zero;  // (outside: undefined in the original)
        r_center = std::acos(DOT(dc, dr)) * 2 / 10;
    }
    double search_pix = max_angle / r_center;
    if (search_pix < 1.0) {
        io.errf("warning: search radius less than 1 pixel! deactivating smoothing and peak extraction...\n");
        splithigh = 0;
        sgs = 0;
    } else {
        if (search_pix < 3.0)
            io.errf("warning: search radius less than 3 pixels! -> %f \n", search_pix);
    }

    // ---- check task position ----
    if (task_lum == 1) {
        if (xt >= p->xr || yt >= p->yr || xt < 0 || yt < 0) {
            io.errf("error: task position outside picture!! exit...");
            throw EvalGlareExit{1};
        }
    }
    // port: the original does not check these positions (out-of-range access)
    if (zones > 0 && !p->inside(x_zone, y_zone))
        throw std::runtime_error("evalglare: zone center outside picture");
    if (set_lum_max2 >= 2 && !p->inside(x_disk, y_disk))
        throw std::runtime_error("evalglare: disk center outside picture");

    double sang = 0.0;
    double E_v = 0.0;
    double E_v_dir = 0.0;
    double avlum = 0.0;
    p->newGli();
    igs = 0;
    p->gl(igs, PICT_Z1_GSN) = 0;
    p->gl(igs, PICT_Z2_GSN) = 0;

    std::vector<double> nopos, posw, posw2;  // port: samples for the medians (only when printed)

    if (multi_image_mode < 1) {
        // ---- cut out GUTH field of view and exit without glare evaluation (-g) ----
        if (cut_view == 2) {
            if (cut_view_type == 1) {
                cutView(*p, 1);
            } else if (cut_view_type == 2) {
                cutView(*p, 2);
            } else if (cut_view_type == 3) {
                io.errf("warning: pixel luminance is weighted by position index - do not use image for glare evaluations!!");
                cutView(*p, 3);
            } else {
                io.errf("error: no valid option for view cutting!!");
                throw EvalGlareExit{1};
            }
            pictWrite(*p, o.checkPath, io);
            throw EvalGlareExit{1};
        }

        // ---- write positionindex into checkfile and exit (-p) ----
        if (posindex_picture == 1) {
            for (x = 0; x < p->xr; x++)
                for (y = 0; y < p->yr; y++) {
                    if (pictGetHangle(*p, x, y, &ang)) {
                        if (p->valid(x, y)) {
                            lum = get_posindex(*p, static_cast<float>(x), static_cast<float>(y), posindex_2) / WHTEFFICACY;
                            setGrey(p->color(x, y), lum);
                        }
                    }
                }
            pictWrite(*p, o.checkPath, io);
            throw EvalGlareExit{1};
        }

        // ---- fill, if necessary from 0 to yfillmin ----
        if (fill == 1) {
            for (x = 0; x < p->xr; x++)
                for (y = yfillmin; y > 0; y = y - 1) {
                    const int y1 = y + 1;
                    if (y1 >= p->yr)
                        continue;  // (outside the picture: undefined in the original)
                    lum = luminance(p->color(x, y1));
                    setGrey(p->color(x, y), lum / 179.0);
                }
        }

        if (calcfast == 0) {
            const auto evBegin = std::chrono::steady_clock::now();
            std::vector<float> posAt;   // port: position indices precomputed in parallel when needed
            bool posReady = false;
            auto posindexAt = [&](int px, int py) -> double {
                if (!posReady && p->viewFixed) {
                    const auto posBegin = std::chrono::steady_clock::now();
                    posAt.assign(npixels, 0.0f);
#if defined(_OPENMP)
#pragma omp parallel for schedule(dynamic, 8)
#endif
                    for (int qx = 0; qx < p->xr; ++qx)
                        for (int qy = 0; qy < p->yr; ++qy)
                            if (p->centerOk(qx, qy) && p->valid(qx, qy))
                                posAt[p->idx(qx, qy)] = get_posindex(*p, static_cast<float>(qx), static_cast<float>(qy), posindex_2);
                    posReady = true;
                    logProfileStage("position-index", posBegin, std::chrono::steady_clock::now());
                }
                if (posReady)
                    return posAt[p->idx(px, py)];
                return get_posindex(*p, static_cast<float>(px), static_cast<float>(py), posindex_2);
            };
            if (wantPositionStats) {
                nopos.reserve(npixels);
                posw.reserve(npixels);
                posw2.reserve(npixels);
            }
            for (x = 0; x < p->xr; x++)
                for (y = 0; y < p->yr; y++) {
                    lum = luminance(p->color(x, y));
                    dist = std::sqrt(static_cast<double>((x-rmx)*(x-rmx)+(y-rmy)*(y-rmy)));
                    if (dist > ((rmx+rmy)/2)) {
                        n_corner_px = n_corner_px+1;
                        if (lum < 7.0)
                            zero_corner_px = zero_corner_px+1;
                    }
                    if (hangleOk(*p, x, y)) {
                        if (p->valid(x, y)) {
                            if (fill == 1 && y >= yfillmax) {
                                const int y1 = y - 1;
                                if (y1 >= 0) {  // (outside the picture: undefined in the original)
                                    lum = luminance(p->color(x, y1));
                                    setGrey(p->color(x, y), lum / 179.0);
                                }
                            }
                            if (lum > abs_max)
                                abs_max = lum;
                            // set luminance restriction, if -m is set
                            if (img_corr == 1) {
                                if (set_lum_max == 1 && lum >= lum_max) {
                                    setGrey(p->color(x, y), new_lum_max / 179.0);
                                    lum = luminance(p->color(x, y));
                                }
                                if (set_lum_max2 == 1 && lum >= lum_max) {
                                    E_v_contr += DOT(p->view.vdir, p->cdir(x, y)) * p->omega(x, y) * lum;
                                    omega_cos_contr += DOT(p->view.vdir, p->cdir(x, y)) * p->omega(x, y) * 1;
                                }
                                if (set_lum_max2 == 2) {
                                    r_actual = std::acos(DOT(p->cdir(x_disk, y_disk), p->cdir(x, y))) * 2;
                                    if (x_disk == x && y_disk == y)
                                        r_actual = 0.0;
                                    if (r_actual <= angle_disk) {
                                        E_v_contr += DOT(p->view.vdir, p->cdir(x, y)) * p->omega(x, y) * lum;
                                        omega_cos_contr += DOT(p->view.vdir, p->cdir(x, y)) * p->omega(x, y) * 1;
                                    }
                                }
                            }
                            const double om = p->omega(x, y);
                            sang += om;
                            E_v += DOT(p->view.vdir, p->cdir(x, y)) * om * lum;
                            avlum += om * lum;
                            if (wantPositionStats) {
                                const double pos = posindexAt(x, y);
                                double lp = lum;
                                nopos.push_back(lp);
                                lp = lum/pos;
                                lum_pos_mean += lp*om;
                                posw.push_back(lp);
                                lp = lp/pos;
                                lum_pos2_mean += lp*om;
                                posw2.push_back(lp);
                            }
                        } else {
                            setGrey(p->color(x, y), 0.0);
                        }
                    } else {
                        setGrey(p->color(x, y), 0.0);
                    }
                }
            logProfileStage("illuminance-loop", evBegin, std::chrono::steady_clock::now());

            // check if image has black corners AND a perspective view
            if (zero_corner_px/n_corner_px > 0.70 && (p->view.type == VT_PER)) {
                io.outf(" corner pixels are to  %f %% black! \n", zero_corner_px/n_corner_px*100);
                io.outf("error! The image has black corners AND header contains a perspective view type definition !!!\n");
                if (force == 0) {
                    io.outf("stopping...!!!!\n");
                    throw EvalGlareExit{1};
                }
            }
            lum_pos_mean = lum_pos_mean/sang;
            lum_pos2_mean = lum_pos2_mean/sang;

            if ((set_lum_max2 >= 1 && E_v_contr > 0 && (E_vl_ext - E_v) > 0) || set_lum_max2 == 3) {
                if (set_lum_max2 < 3) {
                    lum_ideal = (E_vl_ext - E_v + E_v_contr) / omega_cos_contr;
                    if (set_lum_max2 == 2 && lum_ideal >= 2e9)
                        io.outf("warning! luminance of replacement pixels would be larger than 2e9 cd/m2. Value set to 2e9cd/m2!\n");
                    if (lum_ideal > 0 && lum_ideal < setvalue)
                        setvalue = lum_ideal;
                    io.outf("change luminance values!! lum_ideal,setvalue,E_vl_ext,E_v,E_v_contr %f  %f %f %f %f\n",
                            lum_ideal, setvalue, E_vl_ext, E_v, E_v_contr);
                } else {
                    setvalue = LUM_replace;
                }
                for (x = 0; x < p->xr; x++)
                    for (y = 0; y < p->yr; y++) {
                        if (hangleOk(*p, x, y)) {
                            if (p->valid(x, y)) {
                                lum = luminance(p->color(x, y));
                                if (set_lum_max2 == 1 && lum >= lum_max) {
                                    setGrey(p->color(x, y), setvalue / 179.0);
                                } else {
                                    if (set_lum_max2 > 1) {
                                        r_actual = std::acos(DOT(p->cdir(x_disk, y_disk), p->cdir(x, y))) * 2;
                                        if (x_disk == x && y_disk == y)
                                            r_actual = 0.0;
                                        if (r_actual <= angle_disk)
                                            setGrey(p->color(x, y), setvalue / 179.0);
                                    }
                                }
                            }
                        }
                    }
                pictWrite(*p, o.correctionOutputPath, io);
                throw EvalGlareExit{1};
            }
            if (set_lum_max == 1)
                pictWrite(*p, o.correctionOutputPath, io);

            if (calc_vill == 1) {
                io.outf("%f\n", E_v);
                res.verticalIlluminance = E_v;
                throw EvalGlareExit{1};
            }
        } else {
            // in fast calculation mode: ev=ev_ext and sang=2*pi
            sang = 2*3.14159265359;
            lum_task = static_cast<float>(E_vl_ext/sang);
            E_v = E_vl_ext;
        }

        // ---- cut out GUTH field of view for glare evaluation (-G) ----
        if (cut_view == 1) {
            if (cut_view_type == 1) {
                cutView(*p, 1);
            } else if (cut_view_type == 2) {
                cutView(*p, 2);
            } else if (cut_view_type == 3) {
                io.errf("warning: pixel luminance is weighted by position index - do not use image for glare evaluations!!");
                cutView(*p, 3);
            } else {
                io.errf("error: no valid option for view cutting!!");
                throw EvalGlareExit{1};
            }
        }

        if (calcfast == 0) {
            avlum = avlum / sang;
            lum_task = static_cast<float>(avlum);
        }
        if (task_lum == 1) {
            res.taskAssignments.assign(npixels, 0);
            lum_task = static_cast<float>(get_task_lum(*p, xt, yt, static_cast<float>(omegat), task_color, &res.taskAssignments));
        }
        lum_source = lum_thres * lum_task;
        if (lum_thres > 100)
            lum_source = lum_thres;

        // ---- local-only (port): restrict the glare source search to the mask/zone region ----
        std::vector<unsigned char> localRegion;
        if (o.localOnly) {
            localRegion.assign(npixels, 0);
            const AcosLeq inZone1(angle_z1, 2.0);
            const AcosLeq inZone2(angle_z2, 2.0);
            for (x = 0; x < p->xr; x++)
                for (y = 0; y < p->yr; y++) {
                    const size_t k = p->idx(x, y);
                    if (masking) {
                        float c[3] = {pm.grey[k], pm.grey[k], pm.grey[k]};
                        localRegion[k] = luminance(c) > 0.1;
                    } else if (zones > 0) {
                        const double D = DOT(p->cdir(x, y), p->cdir(x_zone, y_zone));
                        const bool z1 = inZone1(D);
                        const bool z2 = !z1 && zones == 2 && inZone2(D);
                        localRegion[k] = z1 || z2;
                    }
                }
        }
        auto localOk = [&](int px, int py) {
            return localRegion.empty() || localRegion[p->idx(px, py)] != 0;
        };

        // ---- first glare source scan: find primary glare sources ----
        const auto scanBegin = std::chrono::steady_clock::now();
        if (patchmode == 0) {
            int lastpixelwas_gs;
            for (x = 0; x < p->xr; x++) {
                lastpixelwas_gs = 0;
                for (y = 0; y < p->yr; y++) {
                    if (hangleOk(*p, x, y)) {
                        if (p->valid(x, y)) {
                            act_lum = luminance(p->color(x, y));
                            if (act_lum > lum_source && localOk(x, y)) {
                                if (act_lum > lum_total_max)
                                    lum_total_max = act_lum;
                                if (lastpixelwas_gs == 0 || search_pix <= 1.0)
                                    actual_igs = find_near_pgs(*p, x, y, static_cast<float>(max_angle), 0, igs, 1, io);
                                if (actual_igs == 0) {
                                    igs = igs + 1;
                                    p->newGli();
                                    p->gl(igs, PICT_LMIN) = HUGE_VAL;
                                    p->gl(igs, PICT_EGLARE) = 0.0;
                                    p->gl(igs, PICT_DGLARE) = 0.0;
                                    p->gl(igs, PICT_Z1_GSN) = 0;
                                    p->gl(igs, PICT_Z2_GSN) = 0;
                                    actual_igs = igs;
                                }
                                p->gsn[p->idx(x, y)] = actual_igs;
                                p->pgs[p->idx(x, y)] = 1;
                                add_pixel_to_gs(*p, x, y, actual_igs, io);
                                lastpixelwas_gs = actual_igs;
                            } else {
                                p->pgs[p->idx(x, y)] = 0;
                                lastpixelwas_gs = 0;
                            }
                        }
                    }
                }
            }
        } else {
            // patchmode on! calculation only for angular projection!
            const double angle_v = p->view.vert;
            const double angle_h = p->view.horiz;
            const int patch_pixdistance_x = static_cast<int>(std::floor(p->xr/angle_h*patch_angle));
            const int patch_pixdistance_y = static_cast<int>(std::floor(p->yr/angle_v*patch_angle));
            const int nx_patch = static_cast<int>(std::floor(angle_v/patch_angle)+1);
            const int ny_patch = static_cast<int>(std::floor(angle_h/patch_angle)+1);
            if (patch_pixdistance_x <= 0 || patch_pixdistance_y <= 0)
                throw std::runtime_error("evalglare: patch angle too small for this picture (division by zero in the original)");
            const int y_offset = static_cast<int>(std::floor(patch_pixdistance_y/2));
            const int x_offset = static_cast<int>(std::floor(patch_pixdistance_x/2));
            int ix_offset = 0;
            for (int iy = 1; iy <= ny_patch; iy++) {
                for (int ix = 1; ix <= nx_patch; ix++) {
                    igs = igs + 1;
                    p->newGli();
                    p->gl(igs, PICT_LMIN) = HUGE_VAL;
                    p->gl(igs, PICT_EGLARE) = 0.0;
                    p->gl(igs, PICT_DGLARE) = 0.0;
                    p->gl(igs, PICT_Z1_GSN) = 0;
                    p->gl(igs, PICT_Z2_GSN) = 0;
                    p->gl(igs, PICT_DXMAX) = (x_offset+ix_offset*x_offset+(ix-1)*patch_pixdistance_x)*1.0;
                    p->gl(igs, PICT_DYMAX) = (y_offset+(iy-1)*patch_pixdistance_y)*1.0;
                }
                ix_offset = ix_offset+1;
                if (ix_offset == 2)
                    ix_offset = 0;
            }
            for (y = 0; y < p->yr; y++) {
                for (x = 0; x < p->xr; x++) {
                    if (hangleOk(*p, x, y)) {
                        if (p->valid(x, y)) {
                            act_lum = luminance(p->color(x, y));
                            if (act_lum > lum_source && localOk(x, y)) {
                                if (act_lum > lum_total_max)
                                    lum_total_max = act_lum;
                                const int y_lines = static_cast<int>(std::floor((y)/patch_pixdistance_y));
                                double xxx = (x+0.0)/(patch_pixdistance_x+0.0)-0.5*(y_lines % 2);
                                if (xxx < 0)
                                    xxx = 0.0;
                                const int i1 = static_cast<int>(y_lines*(nx_patch)+std::floor(xxx)+1);
                                int i2 = 0;
                                int add = 0;
                                if (y_lines % 2 == 1)
                                    add = 1;
                                if (i1 < 1 || i1 > igs)
                                    continue;  // (outside the patch list: undefined in the original)
                                if (y > p->gl(i1, PICT_DYMAX)) {
                                    if (x > p->gl(i1, PICT_DXMAX))
                                        i2 = i1+nx_patch+add;
                                    else
                                        i2 = i1+nx_patch-1+add;
                                } else {
                                    if (x > p->gl(i1, PICT_DXMAX))
                                        i2 = i1-nx_patch+add;
                                    else
                                        i2 = i1-nx_patch-1+add;
                                }
                                if (i2 > igs || i2 < 1) {
                                    actual_igs = i1;
                                } else {
                                    if (((x-p->gl(i1, PICT_DXMAX))*(x-p->gl(i1, PICT_DXMAX))+(y-p->gl(i1, PICT_DYMAX))*(y-p->gl(i1, PICT_DYMAX))) <
                                            ((x-p->gl(i2, PICT_DXMAX))*(x-p->gl(i2, PICT_DXMAX))+(y-p->gl(i2, PICT_DYMAX))*(y-p->gl(i2, PICT_DYMAX))))
                                        actual_igs = i1;
                                    else
                                        actual_igs = i2;
                                }
                                p->gsn[p->idx(x, y)] = actual_igs;
                                p->pgs[p->idx(x, y)] = 1;
                                add_pixel_to_gs(*p, x, y, actual_igs, io);
                            }
                        }
                    }
                }
            }
        }
        logProfileStage("first-scan", scanBegin, std::chrono::steady_clock::now(), "sources=" + std::to_string(igs));

        if (calcfast == 1 || search_pix <= 1.0 || calcfast == 2 || patchmode > 0)
            skip_second_scan = 1;

        // ---- second glare source scan: combine glare sources facing each other ----
        const auto scan2Begin = std::chrono::steady_clock::now();
        int change = 1;
        if (skip_second_scan == 0) {
            // port: the pixels with gsn > 0 do not change during this scan; visit only them (in
            // the original x-major order).
            std::vector<std::pair<int, int>> sourcePixels;
            for (x = 0; x < p->xr; x++)
                for (y = 0; y < p->yr; y++)
                    if (p->gsn[p->idx(x, y)] > 0)
                        sourcePixels.emplace_back(x, y);
            while (change == 1) {
                change = 0;
                for (const auto &sp : sourcePixels) {
                    x = sp.first;
                    y = sp.second;
                    if (hangleOk(*p, x, y)) {
                        int checkpixels = 1;
                        const int before_igs = p->gsn[p->idx(x, y)];
                        // port: find_near_pgs() cannot change anything if no source in 1..igs has a
                        // larger solid angle (no other side effects) -> skip the pixel
                        if (!(p->maxOmega(1, igs) > p->gl(before_igs, PICT_AVOMEGA)))
                            continue;
                        if (x > 1 && x < p->xr-2 && y > 1 && y < p->yr-2) {
                            const int x1 = x-1, x2 = x+1, y1 = y-1, y2 = y+1;
                            if (before_igs == p->gsn[p->idx(x1, y)] && before_igs == p->gsn[p->idx(x2, y)] &&
                                    before_igs == p->gsn[p->idx(x, y1)] && before_igs == p->gsn[p->idx(x, y2)] &&
                                    before_igs == p->gsn[p->idx(x1, y1)] && before_igs == p->gsn[p->idx(x2, y1)] &&
                                    before_igs == p->gsn[p->idx(x1, y2)] && before_igs == p->gsn[p->idx(x2, y2)]) {
                                checkpixels = 0;
                                actual_igs = before_igs;
                            }
                        }
                        if (p->valid(x, y) && before_igs > 0 && checkpixels == 1) {
                            actual_igs = find_near_pgs(*p, x, y, static_cast<float>(max_angle), before_igs, igs, 1, io);
                            if (!(actual_igs == before_igs))
                                change = 1;
                            if (before_igs > 0)
                                actual_igs = p->gsn[p->idx(x, y)];
                        }
                    }
                }
            }
        }
        logProfileStage("second-scan", scan2Begin, std::chrono::steady_clock::now());

        // ---- smoothing: add secondary glare sources ----
        int i_max = igs;
        if (sgs == 1) {
            const auto smoothBegin = std::chrono::steady_clock::now();
            x = (p->xr / 2);
            y = (p->yr / 2);
            rx = (p->xr / 2 + 10);
            ry = (p->yr / 2 + 10);
            {
                const double zero[3] = {0, 0, 0};
                const double *dr = p->inside(rx, ry) ? p->cdir(rx, ry) : zero;
                r_center = std::acos(DOT(p->cdir(x, y), dr)) * 2 / 10;
            }
            search_pix = max_angle / r_center / 1.75;
            const float r = static_cast<float>(search_pix);
            // port: primary pixels (pgs == 1) do not change during the smoothing; windows without
            // primary pixels of a source cannot add the pixel to that source (omega_gs = 0).
            std::vector<int> prefix(static_cast<size_t>(p->xr + 1) * static_cast<size_t>(p->yr + 1), 0);
            auto pref = [&](int px, int py) -> int & { return prefix[static_cast<size_t>(px) * static_cast<size_t>(p->yr + 1) + static_cast<size_t>(py)]; };
            for (int px = 0; px < p->xr; ++px)
                for (int py = 0; py < p->yr; ++py)
                    pref(px + 1, py + 1) = pref(px, py + 1) + pref(px + 1, py) - pref(px, py) +
                        ((p->pgs[p->idx(px, py)] == 1 && p->gsn[p->idx(px, py)] > 0) ? 1 : 0);
            std::vector<double> omegaGs(static_cast<size_t>(i_max + 1));
            for (x = 0; x < p->xr; x++) {
                for (y = 0; y < p->yr; y++) {
                    if (!hangleOk(*p, x, y))
                        continue;
                    if (!(p->valid(x, y) && p->gsn[p->idx(x, y)] == 0))
                        continue;
                    // add_secondary_gs() window (float arithmetic, truncation and the "-2" clamp
                    // of the original)
                    int x_min = static_cast<int>(static_cast<float>(x) - r);
                    if (x_min < 0)
                        x_min = 0;
                    int x_max = static_cast<int>(static_cast<float>(x) + r);
                    if (x_max > p->xr - 1)
                        x_max = p->xr - 2;
                    int y_min = static_cast<int>(static_cast<float>(y) - r);
                    if (y_min < 0)
                        y_min = 0;
                    int y_max = static_cast<int>(static_cast<float>(y) + r);
                    if (y_max > p->yr - 1)
                        y_max = p->yr - 2;
                    if (x_max < x_min || y_max < y_min)
                        continue;
                    const int primaries = pref(x_max + 1, y_max + 1) - pref(x_min, y_max + 1) - pref(x_max + 1, y_min) + pref(x_min, y_min);
                    if (primaries == 0)
                        continue;
                    double omega_total = 0.0;
                    std::fill(omegaGs.begin(), omegaGs.end(), 0.0);
                    for (int ix = x_min; ix <= x_max; ix++)
                        for (int iy = y_min; iy <= y_max; iy++) {
                            const double rr = std::sqrt(static_cast<double>((x - ix) * (x - ix) + (y - iy) * (y - iy)));
                            if (rr <= r) {
                                const double om = p->omega(ix, iy);
                                omega_total += om;
                                const size_t k = p->idx(ix, iy);
                                const int g = p->gsn[k];
                                if (p->pgs[k] == 1 && g > 0 && g <= i_max)
                                    omegaGs[static_cast<size_t>(g)] = omegaGs[static_cast<size_t>(g)] + 1 * om;
                            }
                        }
                    for (i = 1; i <= i_max; i++) {
                        if (p->gl(i, PICT_NPIX) > 0) {
                            if (omegaGs[static_cast<size_t>(i)] / omega_total > 0.2)
                                add_pixel_to_gs(*p, x, y, i, io);
                        }
                    }
                }
            }
            logProfileStage("smoothing", smoothBegin, std::chrono::steady_clock::now());
        }

        // ---- extract extremes from glare sources to extra glare source ----
        if (splithigh == 1 && lum_total_max > limit) {
            const auto splitBegin = std::chrono::steady_clock::now();
            const double r_split = max_angle / 2.0;
            for (i = 0; i <= i_max; i++) {
                if (p->gl(i, PICT_NPIX) > 0) {
                    const double l_max = p->gl(i, PICT_LMAX);
                    const int i_splitstart = igs + 1;
                    if (l_max >= limit) {
                        const GsBox b = p->box[static_cast<size_t>(i)];
                        for (x = b.x0; x <= b.x1; x++)
                            for (y = b.y0; y <= b.y1; y++) {
                                if (hangleOk(*p, x, y)) {
                                    if (p->valid(x, y) && luminance(p->color(x, y)) >= limit && p->gsn[p->idx(x, y)] == i) {
                                        int i_split;
                                        if (i_splitstart == (igs + 1)) {
                                            igs = igs + 1;
                                            p->newGli();
                                            p->gl(igs, PICT_Z1_GSN) = 0;
                                            p->gl(igs, PICT_Z2_GSN) = 0;
                                            p->gl(igs, PICT_EGLARE) = 0.0;
                                            p->gl(igs, PICT_DGLARE) = 0.0;
                                            p->gl(igs, PICT_LMIN) = 99999999999999.999;
                                            i_split = igs;
                                        } else {
                                            i_split = find_split(*p, x, y, r_split, i_splitstart, igs);
                                        }
                                        if (i_split == 0) {
                                            igs = igs + 1;
                                            p->newGli();
                                            p->gl(igs, PICT_Z1_GSN) = 0;
                                            p->gl(igs, PICT_Z2_GSN) = 0;
                                            p->gl(igs, PICT_EGLARE) = 0.0;
                                            p->gl(igs, PICT_DGLARE) = 0.0;
                                            p->gl(igs, PICT_LMIN) = 99999999999999.999;
                                            i_split = igs;
                                        }
                                        split_pixel_from_gs(*p, x, y, i_split, io);
                                    }
                                }
                            }
                    }
                    change = 1;
                    while (change == 1) {
                        change = 0;
                        GsBox ub;
                        for (int k = i_splitstart; k <= igs; ++k)
                            ub.merge(p->box[static_cast<size_t>(k)]);
                        if (ub.empty())
                            break;
                        for (x = ub.x0; x <= ub.x1; x++)
                            for (y = ub.y0; y <= ub.y1; y++) {
                                const int before_igs = p->gsn[p->idx(x, y)];
                                if (before_igs >= i_splitstart) {
                                    if (hangleOk(*p, x, y)) {
                                        if (p->valid(x, y) && before_igs > 0) {
                                            actual_igs = find_near_pgs(*p, x, y, static_cast<float>(max_angle), before_igs, igs, i_splitstart, io);
                                            if (!(actual_igs == before_igs))
                                                change = 1;
                                            if (before_igs > 0)
                                                actual_igs = p->gsn[p->idx(x, y)];
                                        }
                                    }
                                }
                            }
                    }
                }
            }
            logProfileStage("peak-extraction", splitBegin, std::chrono::steady_clock::now());
        }

        // ---- calculation of direct vertical illuminance for CGI and for disability glare, coloring glare sources ----
        if (calcfast == 0 || calcfast == 2) {
            for (x = 0; x < p->xr; x++)
                for (y = 0; y < p->yr; y++) {
                    if (hangleOk(*p, x, y)) {
                        if (p->valid(x, y)) {
                            if (p->gsn[p->idx(x, y)] > 0) {
                                actual_igs = p->gsn[p->idx(x, y)];
                                delta_E = DOT(p->view.vdir, p->cdir(x, y)) * p->omega(x, y) * luminance(p->color(x, y));
                                p->gl(actual_igs, PICT_EGLARE) = p->gl(actual_igs, PICT_EGLARE) + delta_E;
                                E_v_dir = E_v_dir + delta_E;
                                setglcolor(*p, x, y, actual_igs, uniform_gs, u_r, u_g, u_b);
                            }
                        }
                    }
                }
            lum_backg_cos = (E_v - E_v_dir) / 3.1415927;
        }

        // ---- calc of band luminance distribution if applied ----
        if (band_calc == 1) {
            res.bandAssignments.assign(npixels, 0);
            const int x_max = p->xr - 1;
            const int y_max = p->yr - 1;
            const int y_mid = static_cast<int>(y_max/2);
            const AcosLeq inBand(band_angle, 1.0);
            for (x = 0; x <= x_max; x++)
                for (y = 0; y <= y_max; y++) {
                    if (hangleOk(*p, x, y)) {
                        if (p->valid(x, y)) {
                            if (inBand(DOT(p->cdir(x, y_mid), p->cdir(x, y))) || (y == y_mid)) {
                                res.bandAssignments[static_cast<size_t>(y) * static_cast<size_t>(p->xr) + static_cast<size_t>(x)] = 1;
                                float *c = p->color(x, y);
                                s_band.add(luminance(c));
                                act_lum = luminance(c);
                                lum_band_av += p->omega(x, y) * act_lum;
                                omega_band += p->omega(x, y);
                                if (band_color == 1) {
                                    c[0] = 0.0f;
                                    c[1] = static_cast<float>(act_lum / WHTEFFICACY / CIE_gf);
                                    c[2] = 0.0f;
                                }
                            }
                        }
                    }
                }
            lum_band_av = lum_band_av/omega_band;
            double lum_band_std = 0.0;
            s_band.variance(lum_band_std);
            const double per_75_band = s_band.percentile(0.75);
            const double per_95_band = s_band.percentile(0.95);
            const double lum_band_median = s_band.median();
            double bbox_band[2] = {0.0, 0.0};
            s_band.boundingBox(bbox_band);
            io.outf("band:band_omega,band_av_lum,band_median_lum,band_std_lum,band_perc_75,band_perc_95,band_lum_min,band_lum_max: %f %f %f %f %f %f %f %f\n",
                    omega_band, lum_band_av, lum_band_median, std::sqrt(lum_band_std), per_75_band, per_95_band, bbox_band[0], bbox_band[1]);
            res.hasBand = true;
            res.bandSolidAngle = omega_band;
            res.bandAverageLuminance = lum_band_av;
            res.bandMedianLuminance = lum_band_median;
            res.bandStdLuminance = std::sqrt(lum_band_std);
            res.bandPercentile75 = per_75_band;
            res.bandPercentile95 = per_95_band;
            res.bandMinLuminance = bbox_band[0];
            res.bandMaxLuminance = bbox_band[1];
        }

        // ---- glare source statistics ----
        // position index (float, truncated centroid) of every glare source, computed once
        std::vector<float> posOf(static_cast<size_t>(igs + 1), 1.0f);
        auto refreshPosindex = [&]() {
            posOf.assign(static_cast<size_t>(igs + 1), 1.0f);
            for (int k = 0; k <= igs; ++k)
                if (p->gl(k, PICT_NPIX) > 0)
                    posOf[static_cast<size_t>(k)] = get_posindex(*p, static_cast<float>(p->gl(k, PICT_AVPOSX)),
                                                                 static_cast<float>(p->gl(k, PICT_AVPOSY)), posindex_2);
        };
        refreshPosindex();
        double lum_sources = 0;
        double omega_sources = 0;
        i = 0;
        for (x = 0; x <= igs; x++) {
            if (p->gl(x, PICT_NPIX) > 0) {
                sum_glare += std::pow(p->gl(x, PICT_AVLUM), 2.0) * p->gl(x, PICT_AVOMEGA) / std::pow(static_cast<double>(posOf[static_cast<size_t>(x)]), 2.0);
                lum_sources += p->gl(x, PICT_AVLUM) * p->gl(x, PICT_AVOMEGA);
                omega_sources += p->gl(x, PICT_AVOMEGA);
                i = i+1;
            }
        }
        sum_glare = c2*std::log10(1 + sum_glare / std::pow(E_v, a3));
        double lum_backg;
        if (sang == omega_sources)
            lum_backg = avlum;
        else
            lum_backg = (sang * avlum - lum_sources) / (sang - omega_sources);
        if (i == 0)
            lum_sources = 0.0;
        else
            lum_sources = lum_sources/omega_sources;
        if (non_cos_lb == 0)
            lum_backg = lum_backg_cos;
        if (non_cos_lb == 2)
            lum_backg = E_v / 3.1415927;

        // ---- masking ----
        if (masking == 1) {
            for (x = 0; x < p->xr; x++)
                for (y = 0; y < p->yr; y++) {
                    if (hangleOk(*p, x, y)) {
                        if (p->valid(x, y)) {
                            const size_t k = p->idx(x, y);
                            float mc[3] = {pm.grey[k], pm.grey[k], pm.grey[k]};
                            if (luminance(mc) > 0.1) {
                                i_mask = i_mask+1;
                                s_mask.add(luminance(p->color(x, y)));
                                omega_mask += p->omega(x, y);
                                lum_mask_av += p->omega(x, y) * luminance(p->color(x, y));
                                E_v_mask += DOT(p->view.vdir, p->cdir(x, y)) * p->omega(x, y) * luminance(p->color(x, y));
                                pm.grey[k] = static_cast<float>(luminance(p->color(x, y))/179.0);
                            } else {
                                setGrey(p->color(x, y), 0.0);
                            }
                        }
                    }
                }
            lum_mask_av = lum_mask_av/omega_mask;
            double lum_mask_std = 0.0;
            s_mask.variance(lum_mask_std);
            const double per_75_mask = s_mask.percentile(0.75);
            const double per_95_mask = s_mask.percentile(0.95);
            const double lum_mask_median = s_mask.median();
            double bbox[2] = {0.0, 0.0};
            s_mask.boundingBox(bbox);
            // PSGV only why masking of window is applied!
            if (task_lum == 0 || lum_task == 0.0) {
                io.errf(" warning: Task area not set or task luminance=0 ! pgsv cannot be calculated (set to -99)!!\n");
                pgsv = -99;
            } else {
                pgsv = get_pgsv(E_v, E_v_mask, omega_mask, lum_mask_av, lum_task, avlum, io);
            }
            pgsv_con = get_pgsv_con(E_v, E_v_mask, omega_mask, lum_mask_av, avlum);
            pgsv_sat = get_pgsv_sat(E_v);
            if (detail_out == 1) {
                io.outf("masking:no_pixels,omega,av_lum,median_lum,std_lum,perc_75,perc_95,lum_min,lum_max,pgsv_con,pgsv_sat,pgsv,Ev_mask: %i %f %f %f %f %f %f %f %f %f %f %f %f\n",
                        i_mask, omega_mask, lum_mask_av, lum_mask_median, std::sqrt(lum_mask_std), per_75_mask, per_95_mask,
                        bbox[0], bbox[1], static_cast<double>(pgsv_con), static_cast<double>(pgsv_sat), static_cast<double>(pgsv), E_v_mask);
            }
            res.mask.active = true;
            res.mask.pixelCount = i_mask;
            res.mask.solidAngle = omega_mask;
            res.mask.averageLuminance = lum_mask_av;
            res.mask.medianLuminance = lum_mask_median;
            res.mask.stdLuminance = std::sqrt(lum_mask_std);
            res.mask.percentile75 = per_75_mask;
            res.mask.percentile95 = per_95_mask;
            res.mask.minLuminance = bbox[0];
            res.mask.maxLuminance = bbox[1];
            res.mask.pgsvCon = pgsv_con;
            res.mask.pgsvSat = pgsv_sat;
            res.mask.pgsv = pgsv;
            res.mask.verticalIlluminance = E_v_mask;
        }

        // ---- zones ----
        if (zones > 0) {
            res.zoneAssignments.assign(npixels, 0);
            const AcosLeq inZone1(angle_z1, 2.0);
            const AcosLeq inZone2(angle_z2, 2.0);
            for (x = 0; x < p->xr; x++)
                for (y = 0; y < p->yr; y++) {
                    if (hangleOk(*p, x, y)) {
                        if (p->valid(x, y)) {
                            const double D = DOT(p->cdir(x, y), p->cdir(x_zone, y_zone));
                            const double lum_actual = luminance(p->color(x, y));
                            const int act_gsn = p->gsn[p->idx(x, y)];
                            // zone 1
                            const bool z1 = inZone1(D);
                            if (z1) {
                                res.zoneAssignments[static_cast<size_t>(y) * static_cast<size_t>(p->xr) + static_cast<size_t>(x)] = 1;
                                i_z1 = i_z1+1;
                                s_z1.add(lum_actual);
                                omega_z1 += p->omega(x, y);
                                lum_z1_av += p->omega(x, y) * lum_actual;
                                Ez1 += DOT(p->view.vdir, p->cdir(x, y)) * p->omega(x, y) * lum_actual;
                                setglcolor(*p, x, y, 1, 1, 0.66, 0.01, 0.33);
                                if (act_gsn > 0) {
                                    if (p->gl(act_gsn, PICT_Z1_GSN) == 0) {
                                        p->newGli();
                                        igs = igs + 1;
                                        p->gl(act_gsn, PICT_Z1_GSN) = igs*1.000;
                                        p->gl(igs, PICT_Z1_GSN) = -1.0;
                                        p->gl(igs, PICT_Z2_GSN) = -1.0;
                                    }
                                    const int splitgs = static_cast<int>(p->gl(act_gsn, PICT_Z1_GSN));
                                    split_pixel_from_gs(*p, x, y, splitgs, io);
                                    // move direct illuminance contribution into zone -value
                                    delta_E = DOT(p->view.vdir, p->cdir(x, y)) * p->omega(x, y) * luminance(p->color(x, y));
                                    p->gl(act_gsn, PICT_EGLARE) = p->gl(act_gsn, PICT_EGLARE) - delta_E;
                                    p->gl(igs, PICT_EGLARE) = p->gl(igs, PICT_EGLARE) + delta_E;
                                }
                            }
                            // zone 2 (r_actual > angle_z1 && r_actual <= angle_z2)
                            if (!z1 && D >= -1.0 && D <= 1.0 && inZone2(D)) {
                                res.zoneAssignments[static_cast<size_t>(y) * static_cast<size_t>(p->xr) + static_cast<size_t>(x)] = 2;
                                i_z2 = i_z2+1;
                                s_z2.add(lum_actual);
                                omega_z2 += p->omega(x, y);
                                lum_z2_av += p->omega(x, y) * lum_actual;
                                Ez2 += DOT(p->view.vdir, p->cdir(x, y)) * p->omega(x, y) * lum_actual;
                                setglcolor(*p, x, y, 1, 1, 0.65, 0.33, 0.02);
                                if (act_gsn > 0) {
                                    if (p->gl(act_gsn, PICT_Z2_GSN) == 0) {
                                        p->newGli();
                                        igs = igs + 1;
                                        p->gl(act_gsn, PICT_Z2_GSN) = igs*1.000;
                                        p->gl(igs, PICT_Z1_GSN) = -2.0;
                                        p->gl(igs, PICT_Z2_GSN) = -2.0;
                                    }
                                    const int splitgs = static_cast<int>(p->gl(act_gsn, PICT_Z2_GSN));
                                    split_pixel_from_gs(*p, x, y, splitgs, io);
                                    delta_E = DOT(p->view.vdir, p->cdir(x, y)) * p->omega(x, y) * luminance(p->color(x, y));
                                    p->gl(act_gsn, PICT_EGLARE) = p->gl(act_gsn, PICT_EGLARE) - delta_E;
                                    p->gl(igs, PICT_EGLARE) = p->gl(igs, PICT_EGLARE) + delta_E;
                                }
                            }
                        }
                    }
                }
            // average luminance in zones, percentiles, median, min and max
            double lum_z2_std = 0.0, lum_z2_median = 0.0, bbox_z2[2] = {0.0, 0.0};
            if (zones == 2) {
                lum_z2_av = lum_z2_av/omega_z2;
                s_z2.variance(lum_z2_std);
                per_75_z2 = s_z2.percentile(0.75);
                per_95_z2 = s_z2.percentile(0.95);
                lum_z2_median = s_z2.median();
                s_z2.boundingBox(bbox_z2);
            }
            lum_z1_av = lum_z1_av/omega_z1;
            double lum_z1_std = 0.0;
            s_z1.variance(lum_z1_std);
            const double per_75_z1 = s_z1.percentile(0.75);
            const double per_95_z1 = s_z1.percentile(0.95);
            const double lum_z1_median = s_z1.median();
            double bbox_z1[2] = {0.0, 0.0};
            s_z1.boundingBox(bbox_z1);
            if (detail_out == 1) {
                io.outf("zoning:z1_omega,z1_av_lum,z1_median_lum,z1_std_lum,z1_perc_75,z1_perc_95,z1_lum_min,z1_lum_max,Ez1,i_z1: %f %f %f %f %f %f %f %f %f %i\n",
                        omega_z1, lum_z1_av, lum_z1_median, std::sqrt(lum_z1_std), per_75_z1, per_95_z1, bbox_z1[0], bbox_z1[1], Ez1, i_z1);
                if (zones == 2) {
                    // port: prints the zone-2 pixel count (the original prints i_z1 here)
                    io.outf("zoning:z2_omega,z2_av_lum,z2_median_lum,z2_std_lum,z2_perc_75,z2_perc_95,z2_lum_min,z2_lum_max,Ez2,i_z2:  %f %f %f %f %f %f %f %f %f %i\n",
                            omega_z2, lum_z2_av, lum_z2_median, std::sqrt(lum_z2_std), per_75_z2, per_95_z2, bbox_z2[0], bbox_z2[1], Ez2, i_z2);
                }
            }
            auto fillZone = [](EvalGlareZoneStats &z, int count, double omega, double av, double median, double stdv,
                               double p75, double p95, const double *bb, double ez) {
                z.active = true;
                z.pixelCount = count;
                z.solidAngle = omega;
                z.averageLuminance = av;
                z.medianLuminance = median;
                z.stdLuminance = stdv;
                z.percentile75 = p75;
                z.percentile95 = p95;
                z.minLuminance = bb[0];
                z.maxLuminance = bb[1];
                z.verticalIlluminance = ez;
            };
            fillZone(res.zone1, i_z1, omega_z1, lum_z1_av, lum_z1_median, std::sqrt(lum_z1_std), per_75_z1, per_95_z1, bbox_z1, Ez1);
            if (zones == 2)
                fillZone(res.zone2, i_z2, omega_z2, lum_z2_av, lum_z2_median, std::sqrt(lum_z2_std), per_75_z2, per_95_z2, bbox_z2, Ez2);
            res.zoneCount = zones;
            res.zoneCenterX = o.zoneCenterX;
            res.zoneCenterY = o.zoneCenterY;
            res.zoneAngle1 = o.zoneAngle1;
            res.zoneAngle2 = o.zoneAngle2;
            refreshPosindex();
        }

        // ---- resorting glare source numbers ----
        std::vector<int> new_gs_number(static_cast<size_t>(igs + 1), 0);
        i = 0;
        for (x = 0; x <= igs; x++) {
            if (p->gl(x, PICT_NPIX) > 0) {
                i = i + 1;
                p->gl(x, PICT_DGLARE) = i;
                new_gs_number[static_cast<size_t>(x)] = i;
            }
        }
        no_glaresources = i;

        // ---- glare sources (patch coefficient output) ----
        if (output == 4) {
            i = 0;
            for (x = 0; x <= igs; x++) {
                if (p->gl(x, PICT_NPIX) > 0) {
                    i = i + 1;
                    const int x2 = truncCoord(p->gl(x, PICT_AVPOSX));
                    const int y2 = truncCoord(p->gl(x, PICT_AVPOSY));
                    const double zero4[3] = {0.0, 0.0, 0.0};
                    const double *d2 = p->inside(x2, y2) ? p->cdir(x2, y2) : zero4;
                    io.outf("%i %f %f %f %f %.10f %f %f %f %f \n", i, p->gl(x, PICT_NPIX), p->gl(x, PICT_AVPOSX), p->gl(x, PICT_AVPOSY),
                            p->gl(x, PICT_AVLUM), p->gl(x, PICT_AVOMEGA), static_cast<double>(posOf[static_cast<size_t>(x)]),
                            d2[0], d2[1], d2[2]);
                }
            }
            for (y = 0; y < p->yr; y++)
                for (x = 0; x < p->xr; x++) {
                    if (hangleOk(*p, x, y)) {
                        if (p->valid(x, y)) {
                            if (p->gsn[p->idx(x, y)] > 0) {
                                i = p->gsn[p->idx(x, y)];
                                io.outf("%i %i %f %f %f \n", i, new_gs_number[static_cast<size_t>(i)], p->cdir(x, y)[0], p->cdir(x, y)[1], p->cdir(x, y)[2]);
                            }
                        }
                    }
                }
        }

        // per-source sigma (pict_get_sigma at the double centroid) for the detail output and Lveil
        std::vector<double> sigmaAt(static_cast<size_t>(igs + 1), 0.0);
        std::vector<unsigned char> sigmaOk(static_cast<size_t>(igs + 1), 0);
        for (int k = 0; k <= igs; ++k)
            if (p->gl(k, PICT_NPIX) > 0)
                sigmaOk[static_cast<size_t>(k)] = pictGetSigma(*p, p->gl(k, PICT_AVPOSX), p->gl(k, PICT_AVPOSY), &sigmaAt[static_cast<size_t>(k)]);

        // cached direction at the truncated centroid (x2 = av_posx etc.)
        const double zeroDir[3] = {0.0, 0.0, 0.0};
        auto centroidDir = [&](int k) -> const double * {
            const int x2 = truncCoord(p->gl(k, PICT_AVPOSX));
            const int y2 = truncCoord(p->gl(k, PICT_AVPOSY));
            return p->inside(x2, y2) ? p->cdir(x2, y2) : zeroDir;  // (outside: undefined in the original)
        };
        double dgp = 0.0;
        std::vector<EvalGlareSourceDetail> details;
        auto sourceDetail = [&](int k, int number) {
            EvalGlareSourceDetail d;
            const double *dk = centroidDir(k);
            d.index = number;
            d.pixelCount = static_cast<int>(p->gl(k, PICT_NPIX));
            d.avgPosX = p->gl(k, PICT_AVPOSX);
            d.avgPosY = p->yr - p->gl(k, PICT_AVPOSY);
            d.avgLum = p->gl(k, PICT_AVLUM);
            d.omega = p->gl(k, PICT_AVOMEGA);
            d.posIndex = posOf[static_cast<size_t>(k)];
            d.xdir = dk[0];
            d.ydir = dk[1];
            d.zdir = dk[2];
            d.eGlare = p->gl(k, PICT_EGLARE);
            d.glareZone = p->gl(k, PICT_Z1_GSN) < 0 ? static_cast<int>(-p->gl(k, PICT_Z1_GSN)) : 0;
            return d;
        };

        // ---- print detailed output ----
        if (detail_out == 1 && output < 3) {
            dgp = get_dgp(*p, E_v, igs, a1, a2, a3, a4, a5, c1, c2, c3, posOf);
            io.outf("%i No pixels x-pos y-pos L_s Omega_s Posindx L_b L_t E_v Edir Max_Lum Sigma xdir ydir zdir Eglare Lveil_cie teta glare_zone r_contrast r_dgp\n", i);
            if (i == 0) {
                io.outf("%i %f %f %f %f %.10f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f\n", i, 0.0, 0.0,
                        0.0, 0.0, 0.0, 0.0, lum_backg, static_cast<double>(lum_task), E_v, E_v_dir, abs_max, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
            } else {
                i = 0;
                for (x = 0; x <= igs; x++) {
                    if (p->gl(x, PICT_NPIX) > 0) {
                        i = i + 1;
                        if (sigmaOk[static_cast<size_t>(x)])
                            sigma = sigmaAt[static_cast<size_t>(x)];
                        const double *d2 = centroidDir(x);
                        const double teta = 180.0 / 3.1415927 * std::acos(DOT(p->view.vdir, d2));
                        double Lveil_cie = 10*p->gl(x, PICT_EGLARE)/(teta*teta+0.0000000000000001);
                        if (apply_disability == 1 && Lveil_cie <= disability_thresh)
                            Lveil_cie = 0;
                        Lveil_cie_sum = Lveil_cie_sum + Lveil_cie;
                        const double r_glare = c2*std::log10(1 + std::pow(p->gl(x, PICT_AVLUM), 2) * p->gl(x, PICT_AVOMEGA) /
                                                             std::pow(static_cast<double>(posOf[static_cast<size_t>(x)]), 2.0) / std::pow(E_v, a3));
                        const double r_contrast = r_glare/sum_glare;
                        const double r_dgp = r_glare/dgp;
                        int act_gsn;
                        if (p->gl(x, PICT_Z1_GSN) < 0)
                            act_gsn = static_cast<int>(-p->gl(x, PICT_Z1_GSN));
                        else
                            act_gsn = 0;
                        io.outf("%i %f %f %f %f %.10f %f %f %f %f %f %f %f %f %f %f %f %f %f %i %f %f \n",
                                i, p->gl(x, PICT_NPIX), p->gl(x, PICT_AVPOSX), p->yr - p->gl(x, PICT_AVPOSY),
                                p->gl(x, PICT_AVLUM), p->gl(x, PICT_AVOMEGA), static_cast<double>(posOf[static_cast<size_t>(x)]),
                                lum_backg, static_cast<double>(lum_task), E_v, E_v_dir, abs_max, sigma * 180 / 3.1415927,
                                d2[0], d2[1], d2[2], p->gl(x, PICT_EGLARE), Lveil_cie, teta, act_gsn,
                                r_contrast, r_dgp);
                        EvalGlareSourceDetail d = sourceDetail(x, i);
                        d.sigma = sigma * 180 / 3.1415927;
                        d.lveilCie = Lveil_cie;
                        d.theta = teta;
                        d.rContrast = r_contrast;
                        d.rDgp = r_dgp;
                        details.push_back(d);
                    }
                }
            }
        }
        if (details.empty() && no_glaresources > 0) {
            // structured per-source data for callers that did not request the detailed output
            int number = 0;
            for (x = 0; x <= igs; x++) {
                if (p->gl(x, PICT_NPIX) > 0) {
                    EvalGlareSourceDetail d = sourceDetail(x, ++number);
                    if (sigmaOk[static_cast<size_t>(x)])
                        d.sigma = sigmaAt[static_cast<size_t>(x)] * 180 / 3.1415927;
                    d.theta = 180.0 / 3.1415927 * std::acos(DOT(p->view.vdir, centroidDir(x)));
                    d.lveilCie = 10*d.eGlare/(d.theta*d.theta+0.0000000000000001);
                    details.push_back(d);
                }
            }
        }

        // ---- port: DGM (appended column) ----
        double dgmValue = 0.0, dgmTerm = 0.0;
        {
            ViewBasis basis;
            basis.vdir = Vec3d{p->view.vdir[0], p->view.vdir[1], p->view.vdir[2]};
            basis.vup = Vec3d{p->view.vup[0], p->view.vup[1], p->view.vup[2]};
            finalizeViewBasis(basis);
            double dgmSum = 0.0;
            int count = 0;
            for (x = 0; x <= igs; x++) {
                if (p->gl(x, PICT_NPIX) > 0) {
                    ++count;
                    const double *dk = centroidDir(x);
                    const Vec3d dir{dk[0], dk[1], dk[2]};
                    const double posIndex = posOf[static_cast<size_t>(x)];
                    const double omega = p->gl(x, PICT_AVOMEGA);
                    const double omegaStar = std::max(omega, dgmThresholdSolidAngleForDir(dir, basis));
                    dgmSum += std::pow(p->gl(x, PICT_AVLUM), 2.0) * omega * omega /
                              (std::max(omegaStar, 1e-16) * std::pow(posIndex, 2.0));
                }
            }
            if (count > 0) {
                dgmTerm = 0.092 * std::log10(1.0 + dgmSum / std::pow(std::max(E_v, 1e-9), 1.87));
                dgmValue = std::min(1.0, 5.87e-05 * E_v + 0.159 + dgmTerm);
            } else {
                dgmValue = std::min(1.0, 0.159 + 5.87e-05 * E_v);
            }
        }

        // ---- calculation of indicees ----
        float dgi = 0, ugr = 0, vcp = 100, cgi = 0, dgr = 0;
        double ugp = 0, ugp2 = 0, ugr_exp = 0, dgi_mod = 0, Lveil = 0;
        double lum_nopos_median = 0, lum_pos_median = 0, lum_pos2_median = 0;
        if (output < 3) {
            const double E_v2 = E_v;
            if (E_v2 < 100)
                io.errf("Notice: Vertical illuminance is below 100 lux !!\n");
            dgp = get_dgp(*p, E_v2, igs, a1, a2, a3, a4, a5, c1, c2, c3, posOf);
            // low light correction
            if (lowlight == 1) {
                if (E_v < 1000)
                    low_light_corr = 1.0*std::exp(0.024*E_v-4)/(1+std::exp(0.024*E_v-4));
                else
                    low_light_corr = 1.0;
                dgp = low_light_corr*dgp;
            }
            dgp = age_corr_factor*dgp;
            if (ext_vill == 1) {
                if (E_vl_ext < 100)
                    io.errf("Notice: Vertical illuminance is below 100 lux !!\n");
            }
            if (calcfast == 0) {
                const double lum_a = E_v2/3.1415927;
                dgi = get_dgi(*p, static_cast<float>(lum_backg), igs, posOf);
                ugr = get_ugr(*p, lum_backg, igs, posOf);
                ugp = get_ugp(*p, lum_backg, igs, posOf);
                ugp2 = get_ugp2(*p, lum_backg, igs, posOf);
                ugr_exp = get_ugr_exp(*p, lum_backg_cos, lum_a, igs, posOf);
                dgi_mod = get_dgi_mod(*p, static_cast<float>(lum_a), igs, posOf);
                cgi = get_cgi(*p, E_v, E_v_dir, igs, posOf);
                dgr = get_dgr(*p, avlum, igs, posOf);
                vcp = get_vcp(dgr);
                Lveil = get_disability(*p, avlum, igs, sigmaAt, sigmaOk, io);
                if (no_glaresources == 0) {
                    dgi = 0.0;
                    ugr = 0.0;
                    ugp = 0.0;
                    ugp2 = 0.0;
                    ugr_exp = 0.0;
                    dgi_mod = 0.0;
                    cgi = 0.0;
                    dgr = 0.0;
                    vcp = 100.0;
                }
            }
            // check dgp range
            if (dgp <= 0.2)
                io.errf("Notice: Low brightness scene. dgp below 0.2! dgp might underestimate glare sources\n");
            if (E_v < 380)
                io.errf("Notice: Low brightness scene. Vertical illuminance less than 380 lux! dgp might underestimate glare sources\n");

            if (output == 0) {
                if (detail_out == 1) {
                    if (ext_vill == 1) {
                        dgp_ext = get_dgp(*p, E_vl_ext, igs, a1, a2, a3, a4, a5, c1, c2, c3, posOf);
                        dgp = dgp_ext;
                        if (E_vl_ext < 1000 && lowlight == 1)
                            low_light_corr = 1.0*std::exp(0.024*E_vl_ext-4)/(1+std::exp(0.024*E_vl_ext-4));
                        else
                            low_light_corr = 1.0;
                        dgp = low_light_corr*dgp;
                        dgp = age_corr_factor*dgp;
                    }
                    if (o.simple) {
                        // port: short summary line (-S)
                        io.outf("dgp,av_lum,E_v,lum_backg,E_v_dir,dgi,ugr,vcp,cgi,lum_sources,omega_sources,Lveil,Lveil_cie,dgm: %f %f %f %f %f %f %f %f %f %f %f %f %f %f\n",
                                dgp, avlum, E_v, lum_backg, E_v_dir, static_cast<double>(dgi), static_cast<double>(ugr), static_cast<double>(vcp),
                                static_cast<double>(cgi), lum_sources, omega_sources, Lveil, Lveil_cie_sum, dgmValue);
                    } else {
                        // medians (muc_rvar_get_median)
                        RandVar rv;
                        rv.samples.swap(nopos);
                        rv.n = static_cast<int>(rv.samples.size());
                        lum_nopos_median = rv.median();
                        rv.samples.swap(posw);
                        rv.n = static_cast<int>(rv.samples.size());
                        lum_pos_median = rv.median();
                        rv.samples.swap(posw2);
                        rv.n = static_cast<int>(rv.samples.size());
                        lum_pos2_median = rv.median();
                        // port: av_lum_pos2 is printed once divided by the solid angle (the original
                        // divides lum_pos2_mean by sang twice); the dgm column is appended
                        io.outf("dgp,av_lum,E_v,lum_backg,E_v_dir,dgi,ugr,vcp,cgi,lum_sources,omega_sources,Lveil,Lveil_cie,dgr,ugp,ugr_exp,dgi_mod,av_lum_pos,av_lum_pos2,med_lum,med_lum_pos,med_lum_pos2,ugp2,dgm: %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f\n",
                                dgp, avlum, E_v, lum_backg, E_v_dir, static_cast<double>(dgi), static_cast<double>(ugr), static_cast<double>(vcp),
                                static_cast<double>(cgi), lum_sources, omega_sources, Lveil, Lveil_cie_sum, static_cast<double>(dgr), ugp, ugr_exp,
                                dgi_mod, lum_pos_mean, lum_pos2_mean, lum_nopos_median, lum_pos_median, lum_pos2_median, ugp2, dgmValue);
                    }
                } else {
                    if (detail_out2 == 1) {
                        io.outf("dgp,dgi,ugr,vcp,cgi,dgp_ext,Ev_calc,abs_max,Lveil,dgm: %f %f %f %f %f %f %f %f %f %f \n",
                                dgp, static_cast<double>(dgi), static_cast<double>(ugr), static_cast<double>(vcp), static_cast<double>(cgi),
                                dgp_ext, E_v, abs_max, Lveil, dgmValue);
                    } else {
                        if (ext_vill == 1) {
                            dgp_ext = get_dgp(*p, E_vl_ext, igs, a1, a2, a3, a4, a5, c1, c2, c3, posOf);
                            if (E_vl_ext < 1000 && lowlight == 1)
                                low_light_corr = 1.0*std::exp(0.024*E_vl_ext-4)/(1+std::exp(0.024*E_vl_ext-4));
                            else
                                low_light_corr = 1.0;
                            dgp = low_light_corr*dgp_ext;
                            dgp = age_corr_factor*dgp;
                        }
                        io.outf("dgp,dgi,ugr,vcp,cgi,Lveil,dgm: %f %f %f %f %f %f %f  \n",
                                dgp, static_cast<double>(dgi), static_cast<double>(ugr), static_cast<double>(vcp), static_cast<double>(cgi),
                                Lveil, dgmValue);
                    }
                }
            } else {
                dgp_ext = get_dgp(*p, E_vl_ext, igs, a1, a2, a3, a4, a5, c1, c2, c3, posOf);
                dgp = dgp_ext;
                if (E_vl_ext < 1000 && lowlight == 1)
                    low_light_corr = 1.0*std::exp(0.024*E_vl_ext-4)/(1+std::exp(0.024*E_vl_ext-4));
                else
                    low_light_corr = 1.0;
                dgp = low_light_corr*dgp;
                if (calcfast == 2) {
                    lum_backg_cos = (E_vl_ext-dir_ill)/3.1415927;
                    ugr = get_ugr(*p, lum_backg_cos, igs, posOf);
                    io.outf("%f %f \n", dgp, static_cast<double>(ugr));
                } else {
                    io.outf("%f\n", dgp);
                }
            }
        }

        // ---- structured results ----
        res.sourceCount = no_glaresources;
        res.averageLuminance = avlum;
        res.averageLuminancePos = lum_pos_mean;
        res.averageLuminancePos2 = lum_pos2_mean;
        res.medianLuminance = lum_nopos_median;
        res.medianLuminancePos = lum_pos_median;
        res.medianLuminancePos2 = lum_pos2_median;
        res.verticalIlluminance = E_v;
        res.directVerticalIlluminance = E_v_dir;
        res.backgroundLuminance = lum_backg;
        res.sourceAverageLuminance = lum_sources;
        res.sourceSolidAngle = omega_sources;
        res.dgp = dgp;
        res.dgm = dgmValue;
        res.dgmGlareTerm = dgmTerm;
        res.dgi = dgi;
        res.ugr = ugr;
        res.vcp = vcp;
        res.cgi = cgi;
        res.dgr = dgr;
        res.ugp = ugp;
        res.ugp2 = ugp2;
        res.ugrExp = ugr_exp;
        res.dgiMod = dgi_mod;
        res.disabilityGlare = Lveil;
        res.lveilCieSum = Lveil_cie_sum;
        res.maxLuminance = abs_max;
        res.taskLuminance = lum_task;
        res.totalSolidAngle = sang;
        res.glareTerm = sum_glare;
        res.sources = details;
        res.sourceAssignments.assign(npixels, -1);
        for (x = 0; x < p->xr; x++)
            for (y = 0; y < p->yr; y++) {
                const int g = p->gsn[p->idx(x, y)];
                if (g > 0 && g <= igs && new_gs_number[static_cast<size_t>(g)] > 0)
                    res.sourceAssignments[static_cast<size_t>(y) * static_cast<size_t>(p->xr) + static_cast<size_t>(x)] =
                        new_gs_number[static_cast<size_t>(g)] - 1;
            }
        res.completed = true;
    } else {
        // ---- only multiimagemode ----
        const int num_scans = o.multiImageExtraScans;
        double r_split;
        int lastpixelwas_gs;
        for (size_t j = 0; j < o.multiImages.size(); j++) {
            const EvalGlareMultiImage &mi = o.multiImages[j];
            SimplePict pcoeff = readSimplePict(mi.path, true, io);
            const int x_temp = mi.x;
            const int y_temp = o.yFromBottom ? mi.y : p->yr - mi.y - pcoeff.yr;
            for (int jj = 0; jj <= num_scans; jj++) {
                const double scale = jj == 0 ? 1.0 : (static_cast<size_t>(jj - 1) < mi.scales.size() ? mi.scales[static_cast<size_t>(jj - 1)] : 0.0);
                // copy luminance value into big image and remove glare sources
                for (x = 0; x < pcoeff.xr; x++) {
                    for (y = 0; y < pcoeff.yr; y++) {
                        int xmap = x_temp+x;
                        int ymap = y_temp+y;
                        if (xmap < 0)
                            xmap = 0;
                        if (ymap < 0)
                            ymap = 0;
                        if (!p->inside(xmap, ymap))
                            continue;  // (outside the main picture: undefined in the original)
                        const float g = pcoeff.grey[static_cast<size_t>(x) * static_cast<size_t>(pcoeff.yr) + static_cast<size_t>(y)];
                        float *c = p->color(xmap, ymap);
                        c[0] = static_cast<float>(scale*g);
                        c[1] = static_cast<float>(scale*g);
                        c[2] = static_cast<float>(scale*g);
                        p->gsn[p->idx(xmap, ymap)] = 0;
                        p->pgs[p->idx(xmap, ymap)] = 0;
                    }
                }
                actual_igs = 0;
                // first glare source scan: find primary glare sources
                for (x = 0; x < pcoeff.xr; x++) {
                    lastpixelwas_gs = 0;
                    for (y = 0; y < pcoeff.yr; y++) {
                        const int xmap = x_temp+x;
                        const int ymap = y_temp+y;
                        if (!p->inside(xmap, ymap))
                            continue;
                        if (hangleOk(*p, xmap, ymap)) {
                            if (p->valid(xmap, ymap)) {
                                act_lum = luminance(p->color(xmap, ymap));
                                if (act_lum > lum_source) {
                                    if (act_lum > lum_total_max)
                                        lum_total_max = act_lum;
                                    if (lastpixelwas_gs == 0 || search_pix <= 1.0)
                                        actual_igs = find_near_pgs(*p, xmap, ymap, static_cast<float>(max_angle), 0, igs, 1, io);
                                    if (actual_igs == 0) {
                                        igs = igs + 1;
                                        p->newGli();
                                        p->gl(igs, PICT_EGLARE) = 0.0;
                                        actual_igs = igs;
                                    }
                                    p->gsn[p->idx(xmap, ymap)] = actual_igs;
                                    p->pgs[p->idx(xmap, ymap)] = 1;
                                    add_pixel_to_gs(*p, xmap, ymap, actual_igs, io);
                                    lastpixelwas_gs = actual_igs;
                                    delta_E = DOT(p->view.vdir, p->cdir(xmap, ymap)) * p->omega(xmap, ymap) * luminance(p->color(xmap, ymap));
                                    p->gl(actual_igs, PICT_EGLARE) = p->gl(actual_igs, PICT_EGLARE) + delta_E;
                                } else {
                                    p->pgs[p->idx(xmap, ymap)] = 0;
                                    lastpixelwas_gs = 0;
                                }
                            }
                        }
                    }
                }
                // peak extraction
                const int i_max = igs;
                r_split = max_angle / 2.0;
                for (i = 0; i <= i_max; i++) {
                    if (p->gl(i, PICT_NPIX) > 0) {
                        const double l_max = p->gl(i, PICT_LMAX);
                        const int i_splitstart = igs + 1;
                        if (l_max >= limit) {
                            for (x = 0; x < pcoeff.xr; x++)
                                for (y = 0; y < pcoeff.yr; y++) {
                                    const int xmap = x_temp+x;
                                    const int ymap = y_temp+y;
                                    if (!p->inside(xmap, ymap))
                                        continue;
                                    if (hangleOk(*p, xmap, ymap)) {
                                        if (p->valid(xmap, ymap) && luminance(p->color(xmap, ymap)) >= limit && p->gsn[p->idx(xmap, ymap)] == i) {
                                            int i_split;
                                            if (i_splitstart == (igs + 1)) {
                                                igs = igs + 1;
                                                p->newGli();
                                                p->gl(igs, PICT_Z1_GSN) = 0;
                                                p->gl(igs, PICT_Z2_GSN) = 0;
                                                p->gl(igs, PICT_EGLARE) = 0.0;
                                                p->gl(igs, PICT_DGLARE) = 0.0;
                                                p->gl(igs, PICT_LMIN) = 99999999999999.999;
                                                i_split = igs;
                                            } else {
                                                i_split = find_split(*p, xmap, ymap, r_split, i_splitstart, igs);
                                            }
                                            if (i_split == 0) {
                                                igs = igs + 1;
                                                p->newGli();
                                                p->gl(igs, PICT_Z1_GSN) = 0;
                                                p->gl(igs, PICT_Z2_GSN) = 0;
                                                p->gl(igs, PICT_EGLARE) = 0.0;
                                                p->gl(igs, PICT_DGLARE) = 0.0;
                                                p->gl(igs, PICT_LMIN) = 99999999999999.999;
                                                i_split = igs;
                                            }
                                            split_pixel_from_gs(*p, xmap, ymap, i_split, io);
                                        }
                                    }
                                }
                        }
                        int change = 1;
                        while (change == 1) {
                            change = 0;
                            for (x = 0; x < pcoeff.xr; x++)
                                for (y = 0; y < pcoeff.yr; y++) {
                                    const int xmap = x_temp+x;
                                    const int ymap = y_temp+y;
                                    if (!p->inside(xmap, ymap))
                                        continue;
                                    const int before_igs = p->gsn[p->idx(xmap, ymap)];
                                    if (before_igs >= i_splitstart) {
                                        if (hangleOk(*p, xmap, ymap)) {
                                            if (p->valid(xmap, ymap) && before_igs > 0) {
                                                actual_igs = find_near_pgs(*p, xmap, ymap, static_cast<float>(max_angle), before_igs, igs, i_splitstart, io);
                                                if (!(actual_igs == before_igs))
                                                    change = 1;
                                                if (before_igs > 0)
                                                    actual_igs = p->gsn[p->idx(xmap, ymap)];
                                            }
                                        }
                                    }
                                }
                        }
                    }
                }
                // calculation of direct vertical illuminance for the multi-image-mode
                for (x = 0; x < pcoeff.xr; x++)
                    for (y = 0; y < pcoeff.yr; y++) {
                        const int xmap = x_temp+x;
                        const int ymap = y_temp+y;
                        if (!p->inside(xmap, ymap))
                            continue;
                        if (hangleOk(*p, xmap, ymap)) {
                            if (p->valid(xmap, ymap)) {
                                if (p->gsn[p->idx(xmap, ymap)] > 0) {
                                    actual_igs = p->gsn[p->idx(xmap, ymap)];
                                    delta_E = DOT(p->view.vdir, p->cdir(xmap, ymap)) * p->omega(xmap, ymap) * luminance(p->color(xmap, ymap));
                                    p->gl(actual_igs, PICT_EGLARE) = p->gl(actual_igs, PICT_EGLARE) + delta_E;
                                }
                            }
                        }
                    }
                i = 0;
                for (x = 0; x <= igs; x++)
                    if (p->gl(x, PICT_NPIX) > 0)
                        i = i + 1;
                no_glaresources = i;
                io.outf("%i ", no_glaresources);
                i = 0;
                for (x = 0; x <= igs; x++) {
                    if (p->gl(x, PICT_NPIX) > 0) {
                        i = i + 1;
                        const int x2 = truncCoord(p->gl(x, PICT_AVPOSX));
                        const int y2 = truncCoord(p->gl(x, PICT_AVPOSY));
                        const float pos = get_posindex(*p, static_cast<float>(p->gl(x, PICT_AVPOSX)), static_cast<float>(p->gl(x, PICT_AVPOSY)), posindex_2);
                        const double zero[3] = {0, 0, 0};
                        const double *d2 = p->inside(x2, y2) ? p->cdir(x2, y2) : zero;
                        io.outf("%f %.10f %f %f %f %f %f ", p->gl(x, PICT_AVLUM), p->gl(x, PICT_AVOMEGA), static_cast<double>(pos),
                                d2[0], d2[1], d2[2], p->gl(x, PICT_EGLARE));
                    }
                    p->gl(x, PICT_NPIX) = 0;
                    p->gl(x, PICT_AVLUM) = 0;
                    p->gl(x, PICT_AVPOSY) = 0;
                    p->gl(x, PICT_AVPOSX) = 0;
                    p->gl(x, PICT_AVOMEGA) = 0;
                    p->omegaVersion++;
                    p->box[static_cast<size_t>(x)] = GsBox();
                }
                io.outf("\n");
                // empty big image and remove glare sources
                for (x = 0; x < pcoeff.xr; x++) {
                    for (y = 0; y < pcoeff.yr; y++) {
                        const int xmap = x_temp+x;
                        const int ymap = y_temp+y;
                        if (!p->inside(xmap, ymap))
                            continue;
                        setGrey(p->color(xmap, ymap), 0);
                        p->gsn[p->idx(xmap, ymap)] = 0;
                        p->pgs[p->idx(xmap, ymap)] = 0;
                    }
                }
                igs = 0;
            }
        }
        res.completed = true;
    }

    // ---- output picture ----
    if (checkfile == 1)
        pictWrite(*p, o.checkPath, io);
}

} // namespace eg

namespace eg {

// Structured re-formatting of a result in the original output format (used by formatEvalGlare()
// when the result was produced with other output options than requested).
std::string formatFromResult(const EvalGlareAnalysisResult &r, bool detailed, bool simple) {
    Io io;
    if (r.hasBand) {
        io.outf("band:band_omega,band_av_lum,band_median_lum,band_std_lum,band_perc_75,band_perc_95,band_lum_min,band_lum_max: %f %f %f %f %f %f %f %f\n",
                r.bandSolidAngle, r.bandAverageLuminance, r.bandMedianLuminance, r.bandStdLuminance, r.bandPercentile75,
                r.bandPercentile95, r.bandMinLuminance, r.bandMaxLuminance);
    }
    if (detailed && r.mask.active) {
        io.outf("masking:no_pixels,omega,av_lum,median_lum,std_lum,perc_75,perc_95,lum_min,lum_max,pgsv_con,pgsv_sat,pgsv,Ev_mask: %i %f %f %f %f %f %f %f %f %f %f %f %f\n",
                r.mask.pixelCount, r.mask.solidAngle, r.mask.averageLuminance, r.mask.medianLuminance, r.mask.stdLuminance,
                r.mask.percentile75, r.mask.percentile95, r.mask.minLuminance, r.mask.maxLuminance, r.mask.pgsvCon,
                r.mask.pgsvSat, r.mask.pgsv, r.mask.verticalIlluminance);
    }
    if (detailed && r.zoneCount > 0 && r.zone1.active) {
        io.outf("zoning:z1_omega,z1_av_lum,z1_median_lum,z1_std_lum,z1_perc_75,z1_perc_95,z1_lum_min,z1_lum_max,Ez1,i_z1: %f %f %f %f %f %f %f %f %f %i\n",
                r.zone1.solidAngle, r.zone1.averageLuminance, r.zone1.medianLuminance, r.zone1.stdLuminance,
                r.zone1.percentile75, r.zone1.percentile95, r.zone1.minLuminance, r.zone1.maxLuminance,
                r.zone1.verticalIlluminance, r.zone1.pixelCount);
        if (r.zoneCount > 1 && r.zone2.active) {
            io.outf("zoning:z2_omega,z2_av_lum,z2_median_lum,z2_std_lum,z2_perc_75,z2_perc_95,z2_lum_min,z2_lum_max,Ez2,i_z2:  %f %f %f %f %f %f %f %f %f %i\n",
                    r.zone2.solidAngle, r.zone2.averageLuminance, r.zone2.medianLuminance, r.zone2.stdLuminance,
                    r.zone2.percentile75, r.zone2.percentile95, r.zone2.minLuminance, r.zone2.maxLuminance,
                    r.zone2.verticalIlluminance, r.zone2.pixelCount);
        }
    }
    if (detailed) {
        io.outf("%i No pixels x-pos y-pos L_s Omega_s Posindx L_b L_t E_v Edir Max_Lum Sigma xdir ydir zdir Eglare Lveil_cie teta glare_zone r_contrast r_dgp\n",
                r.sourceCount);
        if (r.sources.empty()) {
            io.outf("%i %f %f %f %f %.10f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f\n", 0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                    r.backgroundLuminance, r.taskLuminance, r.verticalIlluminance, r.directVerticalIlluminance, r.maxLuminance,
                    0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
        }
        for (const EvalGlareSourceDetail &s : r.sources) {
            io.outf("%i %f %f %f %f %.10f %f %f %f %f %f %f %f %f %f %f %f %f %f %i %f %f \n",
                    s.index, static_cast<double>(s.pixelCount), s.avgPosX, s.avgPosY, s.avgLum, s.omega, s.posIndex,
                    r.backgroundLuminance, r.taskLuminance, r.verticalIlluminance, r.directVerticalIlluminance, r.maxLuminance,
                    s.sigma, s.xdir, s.ydir, s.zdir, s.eGlare, s.lveilCie, s.theta, s.glareZone, s.rContrast, s.rDgp);
        }
        if (simple) {
            io.outf("dgp,av_lum,E_v,lum_backg,E_v_dir,dgi,ugr,vcp,cgi,lum_sources,omega_sources,Lveil,Lveil_cie,dgm: %f %f %f %f %f %f %f %f %f %f %f %f %f %f\n",
                    r.dgp, r.averageLuminance, r.verticalIlluminance, r.backgroundLuminance, r.directVerticalIlluminance, r.dgi,
                    r.ugr, r.vcp, r.cgi, r.sourceAverageLuminance, r.sourceSolidAngle, r.disabilityGlare, r.lveilCieSum, r.dgm);
        } else {
            io.outf("dgp,av_lum,E_v,lum_backg,E_v_dir,dgi,ugr,vcp,cgi,lum_sources,omega_sources,Lveil,Lveil_cie,dgr,ugp,ugr_exp,dgi_mod,av_lum_pos,av_lum_pos2,med_lum,med_lum_pos,med_lum_pos2,ugp2,dgm: %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f\n",
                    r.dgp, r.averageLuminance, r.verticalIlluminance, r.backgroundLuminance, r.directVerticalIlluminance, r.dgi,
                    r.ugr, r.vcp, r.cgi, r.sourceAverageLuminance, r.sourceSolidAngle, r.disabilityGlare, r.lveilCieSum, r.dgr,
                    r.ugp, r.ugrExp, r.dgiMod, r.averageLuminancePos, r.averageLuminancePos2, r.medianLuminance,
                    r.medianLuminancePos, r.medianLuminancePos2, r.ugp2, r.dgm);
        }
    } else {
        io.outf("dgp,dgi,ugr,vcp,cgi,Lveil,dgm: %f %f %f %f %f %f %f  \n", r.dgp, r.dgi, r.ugr, r.vcp, r.cgi, r.disabilityGlare, r.dgm);
    }
    return io.out;
}

} // namespace eg

} // namespace

HdrLuminanceEvaluator resolveHdrLuminanceEvaluator(const HdrImage &image) {
    const HeaderLuminanceInfo info = resolveLuminanceInfo(image);

    HdrLuminanceEvaluator evaluator;
    evaluator.valid = info.mode != HeaderLuminanceInfo::Unavailable;
    evaluator.monochrome = info.mode == HeaderLuminanceInfo::Monochrome;
    evaluator.weights = info.weights;
    evaluator.exposureScale = info.exposureScale;
    evaluator.description = info.description;
    return evaluator;
}

double evaluateHdrLuminance(const HdrImage &image, size_t x, size_t y, const HdrLuminanceEvaluator &evaluator) {
    const size_t index = (y * image.width + x) * 3;
    double luminance = 0.0;
    if (evaluator.monochrome) {
        luminance = image.rgb[index + 1];
    } else {
        luminance =
            image.rgb[index + 0] * evaluator.weights[0] +
            image.rgb[index + 1] * evaluator.weights[1] +
            image.rgb[index + 2] * evaluator.weights[2];
    }
    luminance *= kWhiteEfficacy;
    luminance *= evaluator.exposureScale;
    return luminance;
}

std::string evalGlareHelp(const std::string &command) {
    std::ostringstream oss;
    oss << command << ":\n";
    oss << "  Native C++ port of Radiance evalglare 3.06 (J. Wienold, Fraunhofer ISE/EPFL).\n";
    oss << "  Works with every Radiance view type (-vta -vth -vtv -vtc -vts; not -vtl) taken from the\n";
    oss << "  VIEW header or from the command line, and with all Radiance picture orientations.\n\n";
    oss << "Syntax:\n";
    oss << "  mergehdr " << command << " [options] [picture.hdr]   (no picture: read stdin)\n\n";
    oss << "Pixel coordinates (x y) are Radiance picture coordinates: x from the left, y from the\n";
    oss << "BOTTOM row (as in the original evalglare). Angles are in degrees unless --radians is\n";
    oss << "given (the -z patch angle is in degrees like in the original; --radians makes it radians).\n";
    oss << "Options are read until the first argument that does not start with '-' (the picture).\n\n";
    oss << "Options (original evalglare):\n";
    oss << "  -vt? -vp x y z -vd x y z -vu x y z -vh deg -vv deg -vo d -va d -vs s -vl l\n";
    oss << "              View options (replace the picture's view; need at least -vt -vh -vv).\n";
    oss << "  -vf file    Read view options from a view file.\n";
    oss << "  -v          Print the evalglare release and stop.\n";
    oss << "  -d          Detailed output (one row per glare source + summary).\n";
    oss << "  -D          Short detailed summary line (dgp,dgi,ugr,vcp,cgi,dgp_ext,Ev_calc,abs_max,Lveil).\n";
    oss << "  -b VALUE    Glare source threshold: cd/m^2 if VALUE > 100, else multiplier of the average\n";
    oss << "              (task) luminance (default 2000; 5 when -t/-T is used without -b).\n";
    oss << "  -r ANGLE    Search radius for combining glare source pixels (default 0.2 rad = 11.46 deg).\n";
    oss << "  -y / -x     Enable (default) / disable peak extraction.   -Y LUM  Peak extraction limit (50000).\n";
    oss << "  -s          Smoothing (add secondary glare source pixels).\n";
    oss << "  -t X Y A    Task area at pixel X Y with opening angle A.  -T X Y A  Same, coloured in -c.\n";
    oss << "  -l X Y A    Zone 1 around X Y (opening angle A).  -L X Y A1 A2  Zones 1 and 2.\n";
    oss << "  -B ANGLE    Band luminance statistics (half angle).\n";
    oss << "  -A FILE     Masking picture (statistics + PGSV of the masked area, implies -d).\n";
    oss << "  -c FILE     Write the check picture (.hdr or .pic).\n";
    oss << "  -u R G B    Uniform colour for the glare sources in the check picture.\n";
    oss << "  -G TYPE     Cut the field of view (1: total field of view, 2: field seen by both eyes,\n";
    oss << "              3: as 2 with luminance weighted by the position index, experimental).\n";
    oss << "  -g TYPE     Cut the field of view, write it to the -c file and stop.\n";
    oss << "  -p          Write the position index picture to the -c file and stop.\n";
    oss << "  -P MODEL    Position index model (0: Guth/Iwata (default), 1: Kim).\n";
    oss << "  -i EV       External vertical illuminance.  -I EV YMAX YMIN  Same + fill rows.\n";
    oss << "  -V          Print only the vertical illuminance.\n";
    oss << "  -k THRESH   Disability glare (Lveil_cie) threshold.\n";
    oss << "  -q MODE     Background luminance: 0 (E_v-E_dir)/pi (default), 1 average, 2 E_v/pi.\n";
    oss << "  -w a1 a2 a3 a4 a5 c1 c2 c3   DGP coefficients.\n";
    oss << "  -C l+|l-|0  Low-light correction on/off (default off).  -4  Low-light correction off.\n";
    oss << "  -1 / -2 DIR / -3   Fast output modes (dgp; dgp ugr with direct illuminance; none).\n";
    oss << "  -m LMAX LNEW FILE  Clip luminance and write FILE.  -M LMAX EV FILE  Correct to EV, write, stop.\n";
    oss << "  -N X Y A EV FILE   Replace a disk to match EV.    -O X Y A LUM FILE  Replace a disk by LUM.\n";
    oss << "  -z ANGLE MODE      Patch mode (vta only; MODE 3: coefficient output).\n";
    oss << "  -Q N S N*(IMAGE X Y S*(SCALE))   Multi-image mode.\n";
    oss << "  -f          Force: continue although the corners are black in a -vtv picture.\n";
    oss << "  -a AGE      (not supported, stops like the original)\n\n";
    oss << "Port-only options:\n";
    oss << "  --radians       Interpret all angle arguments as radians.\n";
    oss << "  -S, --simple    With -d: shorter summary line (no medians).  -ds = -d -S.\n";
    oss << "  --equivlum      Use Eqv_Luminance instead of luminance (Rad/sRGB/XYZ input only).\n";
    oss << "  --local-only, -lf  Restrict glare source detection to the -A mask or the -l/-L zones.\n";
    oss << "  --profile NAME  Accepted and ignored.   --help, --version\n";
    oss << "  Long aliases: --check --detail --threshold --radius --band --mask --zone --zones --task\n";
    oss << "  --task-color --enable-split --split-limit --cut-view --disable-split --force;\n";
    oss << "  sticky forms -G2, -g2, -Y50000.\n\n";
    oss << "Luminance uses the picture's colour metadata (LuminanceRGB, SENSOR2XYZ/XYZCAM, primaries);\n";
    oss << "plain Radiance pictures use the Radiance coefficients like the original. Every labelled\n";
    oss << "summary line gets an additional dgm column.\n";
    return oss.str();
}

EvalGlareCliParseResult parseEvalGlareArguments(const std::vector<std::string> &args) {
    EvalGlareCliParseResult r;
    EvalGlareOptions &o = r.options;
    o.yFromBottom = true;
    o.commandLine = "evalglare ";
    for (const std::string &a : args) {
        o.commandLine += a;
        o.commandLine += " ";
    }
    bool radians = false;
    for (const std::string &a : args)
        if (a == "--radians")
            radians = true;
    const double toRad = radians ? 1.0 : M_PI / 180.0;

    std::vector<const char *> av;
    av.push_back("evalglare");
    for (const std::string &a : args)
        av.push_back(a.c_str());
    av.push_back(nullptr);
    const int argc = static_cast<int>(av.size()) - 1;
    static const char *const usageText =
        "Usage: evalglare [-s][-d][-c picture][-t xpos ypos angle] [-T xpos ypos angle] [-b fact] [-r angle] "
        "[-y] [-Y lum] [-i Ev] [-I Ev ymax ymin] [-v] picfile\n";
    static const std::map<std::string, std::string> aliases = {
        {"--check", "-c"}, {"--detail", "-d"}, {"--threshold", "-b"}, {"--radius", "-r"}, {"--band", "-B"},
        {"--mask", "-A"}, {"--zone", "-l"}, {"--zones", "-L"}, {"--task", "-t"}, {"--task-color", "-T"},
        {"--enable-split", "-y"}, {"--split-limit", "-Y"}, {"--cut-view", "-G"}, {"--disable-split", "-x"},
        {"--force", "-f"}};

    int i;
    bool failed = false;
    auto fail = [&](const std::string &text) {
        r.terminate = true;
        r.exitCode = 1;
        r.stderrText += text;
        failed = true;
    };
    auto nextArg = [&]() -> const char * {
        if (i + 1 >= argc) {
            fail(std::string("evalglare: missing argument for option ") + av[i] + "\n");
            return nullptr;
        }
        return av[++i];
    };
    for (i = 1; i < argc && av[i][0] == '-'; i++) {
        const std::string tok = av[i];
        // ---- port-only spellings ----
        if (tok == "--help" || tok == "-help") {
            r.showHelp = true;
            return r;
        }
        if (tok == "--version") {
            r.showProgramVersion = true;
            return r;
        }
        if (tok == "--radians")
            continue;
        if (tok == "--equivlum") {
            o.equivLuminance = true;
            continue;
        }
        if (tok == "--local-only" || tok == "-lf") {
            o.localOnly = true;
            continue;
        }
        if (tok == "--simple" || tok == "-S") {
            o.simple = true;
            continue;
        }
        if (tok == "-ds") {
            o.detailed = true;
            o.simple = true;
            continue;
        }
        if (tok == "--profile") {
            if (!nextArg())
                return r;
            continue;
        }
        char opt = av[i][1];
        const char *sticky = nullptr;
        const auto alias = aliases.find(tok);
        if (alias != aliases.end()) {
            opt = alias->second[1];
        } else if (tok.size() > 2 && (opt == 'G' || opt == 'g' || opt == 'Y') && eg::isfltd(av[i] + 2)) {
            sticky = av[i] + 2;
        } else if (opt == '-') {
            fail(usageText);
            return r;
        } else {
            // view options are tried first, like the original's getviewopt()
            eg::RadView scratch;
            const int rv = eg::getviewopt(scratch, argc - i, av.data() + i);
            if (rv >= 0) {
                for (int k = 0; k <= rv; ++k)
                    o.viewOptions.push_back(av[i + k]);
                i += rv;
                continue;
            }
        }
        auto value = [&]() -> const char * { return sticky ? sticky : nextArg(); };
        const char *v1 = nullptr;
        const char *v2 = nullptr;
        const char *v3 = nullptr;
        const char *v4 = nullptr;
        const char *v5 = nullptr;
        switch (opt) {
            case 'a':
                if (!(v1 = nextArg()))
                    return r;
                r.stdoutText += "age factor not supported any more \n";
                r.terminate = true;
                r.exitCode = 1;
                return r;
            case 'A':
                if (!(v1 = nextArg()))
                    return r;
                o.maskPath = v1;
                o.detailed = true;
                break;
            case 'b':
                if (!(v1 = nextArg()))
                    return r;
                o.lumThreshold = std::atof(v1);
                o.thresholdExplicit = true;
                break;
            case 'c':
                if (!(v1 = nextArg()))
                    return r;
                o.checkPath = v1;
                break;
            case 'u':
                if (!(v1 = nextArg()) || !(v2 = nextArg()) || !(v3 = nextArg()))
                    return r;
                o.uniformSourceColor = true;
                o.uniformRed = std::atof(v1);
                o.uniformGreen = std::atof(v2);
                o.uniformBlue = std::atof(v3);
                break;
            case 'r':
                if (!(v1 = nextArg()))
                    return r;
                o.maxAngle = std::atof(v1) * toRad;
                break;
            case 'z':
                if (!(v1 = nextArg()) || !(v2 = nextArg()))
                    return r;
                o.patchAngleDegrees = radians ? std::atof(v1) * (180.0 / M_PI) : std::atof(v1);
                o.patchMode = std::atoi(v2);
                if (o.patchMode == 3)
                    o.outputMode = 4;
                break;
            case 's':
                o.smoothing = true;
                break;
            case 'f':
                o.force = true;
                break;
            case 'k':
                if (!(v1 = nextArg()))
                    return r;
                o.applyDisabilityThreshold = true;
                o.disabilityThreshold = std::atof(v1);
                break;
            case 'p':
                o.positionIndexPicture = true;
                break;
            case 'P':
                if (!(v1 = nextArg()))
                    return r;
                o.positionIndexModel = std::atoi(v1);
                break;
            case 'y':
                o.disableSplit = false;
                break;
            case 'x':
                o.disableSplit = true;
                break;
            case 'Y':
                if (!(v1 = value()))
                    return r;
                o.disableSplit = false;
                o.splitLimit = std::atof(v1);
                break;
            case 'i':
                if (!(v1 = nextArg()))
                    return r;
                o.externalIlluminance = true;
                o.externalVerticalIlluminance = std::atof(v1);
                break;
            case 'I':
                if (!(v1 = nextArg()) || !(v2 = nextArg()) || !(v3 = nextArg()))
                    return r;
                o.externalIlluminance = true;
                o.fillRows = true;
                o.externalVerticalIlluminance = std::atof(v1);
                o.fillYMax = std::atoi(v2);
                o.fillYMin = std::atoi(v3);
                break;
            case 'd':
                o.detailed = true;
                break;
            case 'D':
                o.detailedShort = true;
                break;
            case 'm':
                if (!(v1 = nextArg()) || !(v2 = nextArg()) || !(v3 = nextArg()))
                    return r;
                o.clipLuminance = true;
                o.correctionLuminanceLimit = std::atof(v1);
                o.correctionNewLuminance = std::atof(v2);
                o.correctionOutputPath = v3;
                break;
            case 'M':
                if (!(v1 = nextArg()) || !(v2 = nextArg()) || !(v3 = nextArg()))
                    return r;
                o.luminanceCorrection = 1;
                o.correctionLuminanceLimit = std::atof(v1);
                o.externalVerticalIlluminance = std::atof(v2);
                o.correctionOutputPath = v3;
                break;
            case 'N':
                if (!(v1 = nextArg()) || !(v2 = nextArg()) || !(v3 = nextArg()) || !(v4 = nextArg()) || !(v5 = nextArg()))
                    return r;
                o.luminanceCorrection = 2;
                o.diskX = std::atoi(v1);
                o.diskY = std::atoi(v2);
                o.diskAngle = std::atof(v3) * toRad;
                o.externalVerticalIlluminance = std::atof(v4);
                o.correctionOutputPath = v5;
                break;
            case 'O':
                if (!(v1 = nextArg()) || !(v2 = nextArg()) || !(v3 = nextArg()) || !(v4 = nextArg()) || !(v5 = nextArg()))
                    return r;
                o.luminanceCorrection = 3;
                o.diskX = std::atoi(v1);
                o.diskY = std::atoi(v2);
                o.diskAngle = std::atof(v3) * toRad;
                o.replacementLuminance = std::atof(v4);
                o.correctionOutputPath = v5;
                break;
            case 'q':
                if (!(v1 = nextArg()))
                    return r;
                o.backgroundMode = std::atoi(v1);
                break;
            case 't':
            case 'T':
                if (!(v1 = nextArg()) || !(v2 = nextArg()) || !(v3 = nextArg()))
                    return r;
                o.taskActive = true;
                o.taskX = std::atoi(v1);
                o.taskY = std::atoi(v2);
                o.taskAngle = std::atof(v3) * toRad;
                o.taskColor = (opt == 'T');
                break;
            case 'l':
                if (!(v1 = nextArg()) || !(v2 = nextArg()) || !(v3 = nextArg()))
                    return r;
                o.zoneCount = 1;
                o.zoneCenterX = std::atoi(v1);
                o.zoneCenterY = std::atoi(v2);
                o.zoneAngle1 = std::atof(v3) * toRad;
                o.zoneAngle2 = 0.0;
                break;
            case 'L':
                if (!(v1 = nextArg()) || !(v2 = nextArg()) || !(v3 = nextArg()) || !(v4 = nextArg()))
                    return r;
                o.zoneCount = 2;
                o.zoneCenterX = std::atoi(v1);
                o.zoneCenterY = std::atoi(v2);
                o.zoneAngle1 = std::atof(v3) * toRad;
                o.zoneAngle2 = std::atof(v4) * toRad;
                break;
            case 'B':
                if (!(v1 = nextArg()))
                    return r;
                o.bandRequested = true;
                o.bandAngle = std::atof(v1) * toRad;
                break;
            case 'w': {
                const char *w[8];
                for (int k = 0; k < 8; ++k)
                    if (!(w[k] = nextArg()))
                        return r;
                o.dgpA1 = std::atof(w[0]);
                o.dgpA2 = std::atof(w[1]);
                o.dgpA3 = std::atof(w[2]);
                o.dgpA4 = std::atof(w[3]);
                o.dgpA5 = std::atof(w[4]);
                o.dgpC1 = std::atof(w[5]);
                o.dgpC2 = std::atof(w[6]);
                o.dgpC3 = std::atof(w[7]);
                break;
            }
            case 'V':
                o.verticalIlluminanceOnly = true;
                break;
            case 'G':
                if (!(v1 = value()))
                    return r;
                o.cutViewRequested = true;
                o.cutViewWriteOnly = false;
                o.cutViewType = static_cast<int>(std::atof(v1));
                break;
            case 'g':
                if (!(v1 = value()))
                    return r;
                o.cutViewWriteOnly = true;
                o.cutViewType = static_cast<int>(std::atof(v1));
                break;
            case 'C':
                if (!(v1 = nextArg()))
                    return r;
                if (!std::strcmp(v1, "l-"))
                    o.lowLightCorrection = false;
                if (!std::strcmp(v1, "l+"))
                    o.lowLightCorrection = true;
                if (!std::strcmp(v1, "0"))
                    o.lowLightCorrection = false;
                break;
            case '1':
                o.outputMode = 1;
                break;
            case '2':
                if (!(v1 = nextArg()))
                    return r;
                o.outputMode = 2;
                o.directIlluminance = std::atof(v1);
                break;
            case '3':
                o.outputMode = 3;
                break;
            case '4':
                o.lowLightCorrection = false;
                break;
            case 'Q': {
                if (!(v1 = nextArg()) || !(v2 = nextArg()))
                    return r;
                o.multiImageMode = true;
                o.outputMode = 3;
                const int numImages = std::atoi(v1);
                const int numScans = std::atoi(v2);
                o.multiImageExtraScans = numScans;
                o.multiImages.clear();
                for (int j = 0; j < numImages; ++j) {
                    EvalGlareMultiImage mi;
                    const char *name = nextArg();
                    if (!name)
                        return r;
                    const char *mx = nextArg();
                    if (!mx)
                        return r;
                    const char *my = nextArg();
                    if (!my)
                        return r;
                    mi.path = name;
                    mi.x = std::atoi(mx);
                    mi.y = std::atoi(my);
                    for (int jj = 1; jj <= numScans; ++jj) {
                        const char *sc = nextArg();
                        if (!sc)
                            return r;
                        mi.scales.push_back(std::atof(sc));
                    }
                    o.multiImages.push_back(mi);
                }
                break;
            }
            case 'v':
                if (av[i][2] == '\0') {
                    r.stdoutText += std::string(eg::kReleaseName) + " \n";
                    r.terminate = true;
                    r.exitCode = 1;
                    return r;
                }
                if (av[i][2] != 'f') {
                    fail(usageText);
                    return r;
                }
                if (!(v1 = nextArg()))
                    return r;
                o.viewOptions.push_back("-vf");
                o.viewOptions.push_back(v1);
                break;
            default:
                fail(usageText);
                return r;
        }
        if (failed)
            return r;
    }
    if (i < argc) {
        o.inputPath = av[i];
        if (i + 1 < argc) {
            std::string ignored;
            for (int k = i + 1; k < argc; ++k)
                ignored += std::string(" ") + av[k];
            r.stderrText += "evalglare: warning: arguments after the picture are ignored (like the original):" + ignored + "\n";
        }
    } else {
        o.inputPath = "-";
    }
    return r;
}

namespace {

void runEvalGlareInto(const EvalGlareOptions &opts, EvalGlareAnalysisResult &res, eg::Io &io) {
    try {
        eg::runMain(opts, res, io);
        res.exitStatus = 0;
    } catch (const eg::EvalGlareExit &e) {
        res.exitStatus = e.status;
    }
    res.outputText = io.out;
    res.messages = io.err;
    res.outputDetailed = opts.detailed || !opts.maskPath.empty();
    res.outputSimple = opts.simple;
}

} // namespace

void clearEvalGlareCaches() {
    eg::clearGeometryCache();
}

EvalGlareAnalysisResult analyzeEvalGlare(const EvalGlareOptions &opts) {
    EvalGlareAnalysisResult res;
    eg::Io io;
    runEvalGlareInto(opts, res, io);
    if (res.exitStatus != 0 && !res.completed) {
        std::string message = trimCopy(res.messages);
        if (message.empty())
            message = trimCopy(res.outputText);
        if (message.empty())
            message = "evalglare stopped.";
        throw std::runtime_error(message);
    }
    return res;
}


PerceptualMapResult analyzePerceptualMap(const PerceptualMapOptions &opts, PerceptualMapKind kind) {
    if (opts.inputPath.empty())
        throw std::runtime_error("Choose an input HDR image first.");

    namespace fs = std::filesystem;
    static std::map<std::string, CachedPerceptualMapResultEntry> sResultCache;
    const fs::file_time_type inputMtime = fs::last_write_time(fs::path(opts.inputPath));
    const std::string resultKey =
        opts.inputPath +
        "|kind=" + perceptualMapKindLabel(kind) +
        "|ppd=" + formatCacheDouble(opts.pixelsPerDegree) +
        "|sens=" + formatCacheDouble(opts.sensitivityCorrection) +
        "|emission=" + opts.spectralEmissionPath +
        "|input_color=" + std::string(viewVisibilityInputColorLabel(opts.inputColor));

    std::map<std::string, CachedPerceptualMapResultEntry>::iterator cachedIt = sResultCache.find(resultKey);
    if (cachedIt != sResultCache.end() && cachedIt->second.inputMtime == inputMtime)
        return cachedIt->second.result;

    const HdrImage &image = readRadianceHDRCached(opts.inputPath, false, false);
    if (image.width == 0 || image.height == 0)
        throw std::runtime_error("Input HDR image is empty.");

    PerceptualMapResult result;
    result.width = image.width;
    result.height = image.height;
    result.kind = kind;
    result.label = perceptualMapKindLabel(kind);
    if (kind == PerceptualMapKind::EqvLuminance) {
        const std::vector<double> source = computeEquivalentLuminanceMap(image);
        result.values.resize(source.size(), 0.0f);
        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(source.size()); ++i)
            result.values[static_cast<size_t>(i)] = static_cast<float>(std::max(0.0, source[static_cast<size_t>(i)]));
    } else {
        // Reads only the vectors of this map kind, but a (re)computed entry gets all perceptual-map
        // vectors, so that switching between map kinds of one picture reuses the cached entry.
        const NativeHdrvdpPreparedImage &prepared = getPreparedNativeHdrvdpImageCached(
            opts.inputPath,
            opts.inputPath,
            opts.spectralEmissionPath,
            opts.sensitivityCorrection,
            opts.pixelsPerDegree,
            HdrvdpPreparedImageMode::DirectChannels,
            perceptualMapPreparedFields(kind),
            kPreparedPerceptualFields,
            false,
            false,
            opts.inputColor
        );
        if (kind == PerceptualMapKind::DetectableContrast) {
            const std::vector<double> source = computeHdrvdpSubsetContrastMap(
                prepared.achromatic,
                prepared.adaptation,
                image.width,
                image.height,
                opts.pixelsPerDegree,
                opts.sensitivityCorrection
            );
            result.values.resize(source.size(), 0.0f);
            #if defined(_OPENMP)
            #pragma omp parallel for schedule(static)
            #endif
            for (long long i = 0; i < static_cast<long long>(source.size()); ++i)
                result.values[static_cast<size_t>(i)] = static_cast<float>(std::max(0.0, source[static_cast<size_t>(i)]));
        } else {
            const std::vector<double> &source = perceptualMapValues(prepared, kind);
            result.values.resize(source.size(), 0.0f);
            #if defined(_OPENMP)
            #pragma omp parallel for schedule(static)
            #endif
            for (long long i = 0; i < static_cast<long long>(source.size()); ++i)
                result.values[static_cast<size_t>(i)] = static_cast<float>(std::max(0.0, source[static_cast<size_t>(i)]));
        }
    }

    CachedPerceptualMapResultEntry cacheEntry;
    cacheEntry.inputMtime = inputMtime;
    cacheEntry.result = result;
    sResultCache.insert_or_assign(resultKey, std::move(cacheEntry));
    return result;
}

ViewVisibilitySummaryResult analyzeViewVisibilitySummary(const ViewVisibilitySummaryOptions &opts) {
    if (opts.referencePath.empty() || opts.testPath.empty())
        throw std::runtime_error("Choose both reference and test HDR images first.");
    if (opts.maskPath.empty())
        throw std::runtime_error("Choose a white mask HDR first.");

    namespace fs = std::filesystem;
    static std::map<std::string, CachedViewVisibilityResultEntry> sResultCache;
    const fs::file_time_type referenceMtime = fs::last_write_time(fs::path(opts.referencePath));
    const fs::file_time_type testMtime = fs::last_write_time(fs::path(opts.testPath));
    const fs::file_time_type maskMtime = fs::last_write_time(fs::path(opts.maskPath));
    const std::string resultKey =
        opts.referencePath +
        "|test=" + opts.testPath +
        "|mask=" + opts.maskPath +
        "|mode=" + std::to_string(static_cast<int>(opts.detailMode)) +
        "|input_color=" + std::string(viewVisibilityInputColorLabel(opts.inputColor)) +
        "|ppd=" + formatCacheDouble(opts.pixelsPerDegree) +
        "|sens=" + formatCacheDouble(opts.sensitivityCorrection) +
        "|emission=" + opts.spectralEmissionPath;
    auto cachedResultIt = sResultCache.find(resultKey);
    if (cachedResultIt != sResultCache.end() &&
        cachedResultIt->second.referenceMtime == referenceMtime &&
        cachedResultIt->second.testMtime == testMtime &&
        cachedResultIt->second.maskMtime == maskMtime) {
        return cachedResultIt->second.result;
    }

    const HdrImage &reference = readRadianceHDRCached(opts.referencePath, false, false);
    const HdrImage &test = readRadianceHDRCached(opts.testPath, false, false);
    // The two mask decodings are only read by the loop below, so they are not kept in the picture
    // cache (same reader and flags as readRadianceHDRCached, so the same pixels).
    HdrImage mask = readRadianceHDR(opts.maskPath, false, false);
    HdrImage maskForIntegrate = readRadianceHDR(opts.maskPath, true, false);

    if (reference.width != test.width || reference.height != test.height ||
        reference.width != mask.width || reference.height != mask.height ||
        reference.width != maskForIntegrate.width || reference.height != maskForIntegrate.height) {
        throw std::runtime_error("Reference, test, and mask images must have the same resolution.");
    }

    const size_t pixelCount = reference.width * reference.height;

    ViewVisibilitySummaryResult result;
    result.width = reference.width;
    result.height = reference.height;
    result.inputColor = viewVisibilityInputColorLabel(opts.inputColor);
    double proxyPreservationSum = 0.0;

    // The mask threshold value is only ever compared with 0.5, so per pixel only the outcome is
    // kept: 1 if "> 0.5", 0 if "<= 0.5", 2 if neither (NaN).
    std::vector<double> maskValues(pixelCount, 0.0);
    std::vector<unsigned char> maskThresholdClass(pixelCount, 0);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(pixelCount); ++i) {
        const size_t idx = static_cast<size_t>(i);
        const size_t y = idx / reference.width;
        const size_t x = idx - y * reference.width;
        maskValues[idx] = compareMaskWeightAt(mask, x, y);
        const double maskThreshold = pvalueDecimalRoundTrip(radianceRgbLuminanceRawAt(maskForIntegrate, x, y));
        maskThresholdClass[idx] = maskThreshold > 0.5 ? 1 : (maskThreshold <= 0.5 ? 0 : 2);
    }
    mask = HdrImage();
    maskForIntegrate = HdrImage();

    if (opts.detailMode == ViewVisibilitySummaryOptions::DetailMode::ContrastProxy) {
        std::vector<double> refLuminance(pixelCount, 0.0);
        std::vector<double> testLuminance(pixelCount, 0.0);
        if (opts.inputColor == ViewVisibilitySummaryOptions::InputColor::Rad) {
            const HeaderLuminanceInfo refInfo = resolveViewVisibilityLuminanceInfo(reference);
            const HeaderLuminanceInfo testInfo = resolveViewVisibilityLuminanceInfo(test);
            if (refInfo.mode == HeaderLuminanceInfo::Unavailable) {
                throw std::runtime_error("Reference image does not contain usable luminance metadata.");
            }
            if (testInfo.mode == HeaderLuminanceInfo::Unavailable) {
                throw std::runtime_error("Test image does not contain usable luminance metadata.");
            }
            #if defined(_OPENMP)
            #pragma omp parallel for schedule(static)
            #endif
            for (long long i = 0; i < static_cast<long long>(pixelCount); ++i) {
                const size_t idx = static_cast<size_t>(i);
                const size_t y = idx / reference.width;
                const size_t x = idx - y * reference.width;
                refLuminance[idx] = std::max(0.0, luminanceAt(reference, x, y, refInfo));
                testLuminance[idx] = std::max(0.0, luminanceAt(test, x, y, testInfo));
            }
        } else {
            const double refInputScale = viewVisibilityInputScale(reference, opts.inputColor);
            const double testInputScale = viewVisibilityInputScale(test, opts.inputColor);
            #if defined(_OPENMP)
            #pragma omp parallel for schedule(static)
            #endif
            for (long long i = 0; i < static_cast<long long>(pixelCount); ++i) {
                const size_t idx = static_cast<size_t>(i);
                refLuminance[idx] = std::max(
                    0.0, viewVisibilityInputXyzAt(reference, idx, opts.inputColor, refInputScale).y);
                testLuminance[idx] = std::max(
                    0.0, viewVisibilityInputXyzAt(test, idx, opts.inputColor, testInputScale).y);
            }
        }
        result.detailLabel = "Contrast proxy";
        const std::vector<double> refContrast = computeContrastProxyMap(
            refLuminance,
            reference.width,
            reference.height,
            opts.pixelsPerDegree,
            opts.sensitivityCorrection
        );
        const std::vector<double> testContrast = computeContrastProxyMap(
            testLuminance,
            test.width,
            test.height,
            opts.pixelsPerDegree,
            opts.sensitivityCorrection
        );

        result.testContrastMap.assign(pixelCount, 0.0f);
        result.visibilityMap.assign(pixelCount, 0.0f);
        result.referenceContrastMap.assign(pixelCount, 0.0f);
        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(pixelCount); ++i) {
            const double refValue = refContrast[i];
            const double testValue = testContrast[i];
            const double preservation = refValue > 1e-9
                ? std::max(0.0, std::min(1.0, testValue / refValue))
                : 1.0;
            const double maskWeight = maskValues[i];
            const double visibleValue = preservation * refValue * maskWeight * maskWeight;

            result.testContrastMap[i] = static_cast<float>(testValue * maskWeight);
            result.visibilityMap[i] = static_cast<float>(visibleValue);
            result.referenceContrastMap[i] = static_cast<float>(refValue * maskWeight);

        }
        for (size_t i = 0; i < pixelCount; ++i) {
            if (maskThresholdClass[i] == 0)  // maskThreshold <= 0.5
                continue;

            const double refValue = refContrast[i];
            const double testValue = testContrast[i];
            const double preservation = refValue > 1e-9
                ? std::max(0.0, std::min(1.0, testValue / refValue))
                : 1.0;
            proxyPreservationSum += preservation;
        }
        result.qualityScore = std::max(
            0.0,
            std::min(10.0, 10.0 * proxyPreservationSum / std::max<size_t>(1, pixelCount)));
    } else {
        result.detailLabel = "HDR-VDP3";
        // Only the adaptation and achromatic vectors are read below (kPreparedVisibilityFields).
        const NativeHdrvdpPreparedImage &referenceContrastPrepared = getPreparedNativeHdrvdpImageCached(
            opts.referencePath, opts.referencePath, opts.spectralEmissionPath, opts.sensitivityCorrection, opts.pixelsPerDegree,
            HdrvdpPreparedImageMode::DirectChannels, kPreparedVisibilityFields, kPreparedVisibilityFields, false, false, opts.inputColor);
        const NativeHdrvdpPreparedImage &testContrastPrepared = getPreparedNativeHdrvdpImageCached(
            opts.testPath, opts.testPath, opts.spectralEmissionPath, opts.sensitivityCorrection, opts.pixelsPerDegree,
            HdrvdpPreparedImageMode::DirectChannels, kPreparedVisibilityFields, kPreparedVisibilityFields, false, false, opts.inputColor);
        const NativeHdrvdpPreparedImage &referenceSideBySidePrepared = getPreparedNativeHdrvdpImageCached(
            opts.referencePath, opts.referencePath, opts.spectralEmissionPath, opts.sensitivityCorrection, opts.pixelsPerDegree,
            HdrvdpPreparedImageMode::NativeTransformed, kPreparedVisibilityFields, kPreparedVisibilityFields, false, false, opts.inputColor);
        const NativeHdrvdpPreparedImage &testSideBySidePrepared = getPreparedNativeHdrvdpImageCached(
            opts.testPath, opts.testPath, opts.spectralEmissionPath, opts.sensitivityCorrection, opts.pixelsPerDegree,
            HdrvdpPreparedImageMode::NativeTransformed, kPreparedVisibilityFields, kPreparedVisibilityFields, false, false, opts.inputColor);
        // The diff mask compares the pre-MTF native channels of the two NativeTransformed
        // preparations. They are recomputed from the same cached pictures with the same function
        // and spectral-context matrix as in prepareNativeHdrvdpImage() instead of being cached.
        std::vector<double> diffMask(pixelCount, 0.0);
        const double refInputScale = viewVisibilityInputScale(reference, opts.inputColor);
        const double testInputScale = viewVisibilityInputScale(test, opts.inputColor);
        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(pixelCount); ++i) {
            const Vec3d refNative = hdrvdpPreMtfChannelsAt(reference, static_cast<size_t>(i),
                HdrvdpPreparedImageMode::NativeTransformed, referenceSideBySidePrepared.spectralContext->xyzToNativePreAod, opts.inputColor,
                refInputScale);
            const Vec3d testNative = hdrvdpPreMtfChannelsAt(test, static_cast<size_t>(i),
                HdrvdpPreparedImageMode::NativeTransformed, testSideBySidePrepared.spectralContext->xyzToNativePreAod, opts.inputColor,
                testInputScale);
            const double refR = refNative.x;
            const double refG = refNative.y;
            const double refB = refNative.z;
            const double testR = testNative.x;
            const double testG = testNative.y;
            const double testB = testNative.z;
            const bool different =
                hdrvdpDiffMaskChannelChanged(refR, testR) ||
                hdrvdpDiffMaskChannelChanged(refG, testG) ||
                hdrvdpDiffMaskChannelChanged(refB, testB);
            diffMask[i] = different ? maskValues[static_cast<size_t>(i)] : 0.0;
        }
        // The side-by-side comparison and the two contrast maps are independent of each other;
        // the comparison (the largest working set) runs first so that the contrast maps are not
        // held while it runs.
        const HdrvdpSubsetSideBySideResult sideBySide = computeHdrvdpSubsetSideBySide(
            referenceSideBySidePrepared,
            testSideBySidePrepared,
            std::move(diffMask),
            reference.width,
            reference.height,
            opts.pixelsPerDegree,
            opts.sensitivityCorrection
        );
        const std::vector<double> refDetectable = computeHdrvdpSubsetContrastMap(
            referenceContrastPrepared.achromatic,
            referenceContrastPrepared.adaptation,
            reference.width,
            reference.height,
            opts.pixelsPerDegree,
            opts.sensitivityCorrection
        );
        const std::vector<double> testDetectable = computeHdrvdpSubsetContrastMap(
            testContrastPrepared.achromatic,
            testContrastPrepared.adaptation,
            test.width,
            test.height,
            opts.pixelsPerDegree,
            opts.sensitivityCorrection
        );

        result.testContrastMap.assign(pixelCount, 0.0f);
        result.visibilityMap.assign(pixelCount, 0.0f);
        result.referenceContrastMap.assign(pixelCount, 0.0f);
        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(pixelCount); ++i) {
            const double refValue = refDetectable[i];
            const double testValue = testDetectable[i];
            const double maskWeight = maskValues[i];
            const double visibleValue = sideBySide.undetectMap[i] * refValue * maskWeight * maskWeight;
            result.testContrastMap[i] = static_cast<float>(testValue * maskWeight);
            result.visibilityMap[i] = static_cast<float>(visibleValue);
            result.referenceContrastMap[i] = static_cast<float>(refValue * maskWeight);
        }
        result.qualityScore = sideBySide.qualityJod;
    }

    {
        std::vector<float> finalRgb(pixelCount * 3, 0.0f);
        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(pixelCount); ++i) {
            const size_t idx = static_cast<size_t>(i);
            finalRgb[idx * 3 + 0] = result.testContrastMap[idx];
            finalRgb[idx * 3 + 1] = result.visibilityMap[idx];
            finalRgb[idx * 3 + 2] = result.referenceContrastMap[idx];
        }
        quantizeRadianceRgbRoundTrip(finalRgb);
        #if defined(_OPENMP)
        #pragma omp parallel for schedule(static)
        #endif
        for (long long i = 0; i < static_cast<long long>(pixelCount); ++i) {
            const size_t idx = static_cast<size_t>(i);
            result.testContrastMap[idx] = finalRgb[idx * 3 + 0];
            result.visibilityMap[idx] = finalRgb[idx * 3 + 1];
            result.referenceContrastMap[idx] = finalRgb[idx * 3 + 2];
        }
    }

    const double binaryMaskPixelValue = pvalueDecimalRoundTrip(
        static_cast<double>(quantizeRadianceGrayRoundTrip(1.0f)));
    result.viewSize = 0.0;
    result.viewPixelCount = 0;
    result.referenceTotal = 0.0;
    result.visibilityTotal = 0.0;
    for (size_t i = 0; i < pixelCount; ++i) {
        result.referenceTotal += pvalueDecimalRoundTrip(static_cast<double>(
            quantizeRadianceGrayRoundTrip(result.referenceContrastMap[i])));
        result.visibilityTotal += pvalueDecimalRoundTrip(static_cast<double>(
            quantizeRadianceGrayRoundTrip(result.visibilityMap[i])));
        if (maskThresholdClass[i] == 1) {  // maskThreshold > 0.5
            result.viewSize += binaryMaskPixelValue;
            ++result.viewPixelCount;
        }
    }

    if (opts.detailMode == ViewVisibilitySummaryOptions::DetailMode::ContrastProxy) {
        result.qualityScore = std::max(
            0.0,
            std::min(10.0, 10.0 * proxyPreservationSum / std::max<size_t>(1, result.viewPixelCount)));
    }

    if (result.viewPixelCount == 0)
        throw std::runtime_error("The selected mask does not contain any white pixels above 0.5.");

    result.referenceBaseline = result.referenceTotal / std::max(result.viewSize, 1e-9);
    result.visibilityRatio = result.visibilityTotal / std::max(result.referenceTotal, 1e-9);
    CachedViewVisibilityResultEntry cacheEntry;
    cacheEntry.referenceMtime = referenceMtime;
    cacheEntry.testMtime = testMtime;
    cacheEntry.maskMtime = maskMtime;
    cacheEntry.result = result;
    sResultCache.insert_or_assign(resultKey, std::move(cacheEntry));
    return result;
}

void writeViewVisibilityResultHdr(const std::string &path, const ViewVisibilitySummaryResult &result) {
    if (result.width == 0 || result.height == 0) {
        throw std::runtime_error("View visibility result is empty.");
    }
    if (result.testContrastMap.size() != result.width * result.height ||
        result.visibilityMap.size() != result.width * result.height ||
        result.referenceContrastMap.size() != result.width * result.height) {
        throw std::runtime_error("View visibility result maps are incomplete.");
    }

    HdrImage image;
    image.width = result.width;
    image.height = result.height;
    image.rgb.resize(result.width * result.height * 3, 0.0f);

    for (size_t i = 0; i < result.width * result.height; ++i) {
        image.rgb[i * 3 + 0] = result.testContrastMap[i];
        image.rgb[i * 3 + 1] = result.visibilityMap[i];
        image.rgb[i * 3 + 2] = result.referenceContrastMap[i];
    }

    std::vector<std::string> headerLines;
    headerLines.push_back("MERGEHDR_VIEWVIS= 1");
    headerLines.push_back("MERGEHDR_VIEWVIS_DETAIL= " + (result.detailLabel.empty() ? std::string("unknown") : result.detailLabel));
    headerLines.push_back("MERGEHDR_VIEWVIS_INPUT_COLOR= " + (result.inputColor.empty() ? std::string("rad") : result.inputColor));
    headerLines.push_back("MERGEHDR_VIEWVIS_CHANNELS= test_contrast visibility reference_contrast");
    headerLines.push_back("MERGEHDR_VIEWVIS_VIEWSIZE= " + std::to_string(result.viewSize));
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(9) << result.referenceBaseline;
        headerLines.push_back("MERGEHDR_VIEWVIS_REFBASELINE= " + oss.str());
    }
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(9) << result.visibilityRatio;
        headerLines.push_back("MERGEHDR_VIEWVIS_RATIO= " + oss.str());
    }
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(9) << result.qualityScore;
        headerLines.push_back("MERGEHDR_VIEWVIS_Q= " + oss.str());
    }

    writeRadianceHDRMatlabCompatible(path, image, headerLines, false, true);
}

void writePerceptualMapHdr(const std::string &path, const PerceptualMapResult &result) {
    if (result.width == 0 || result.height == 0 || result.values.size() != result.width * result.height)
        throw std::runtime_error("Perceptual map result is empty.");

    HdrImage image;
    image.width = result.width;
    image.height = result.height;
    image.rgb.resize(result.width * result.height * 3, 0.0f);
    for (size_t i = 0; i < result.values.size(); ++i) {
        image.rgb[i * 3 + 0] = result.values[i];
        image.rgb[i * 3 + 1] = result.values[i];
        image.rgb[i * 3 + 2] = result.values[i];
    }

    std::vector<std::string> headerLines;
    headerLines.push_back("MERGEHDR_PERCEPTUALMAP= 1");
    headerLines.push_back("MERGEHDR_PERCEPTUALMAP_KIND= " + result.label);
    headerLines.push_back("MERGEHDR_PERCEPTUALMAP_CHANNELS= " + result.label + " " + result.label + " " + result.label);
    writeRadianceHDR(path, image, headerLines, false, true);
}

namespace {
void writeGrayMapHdr(
    const std::string &path,
    size_t width,
    size_t height,
    const std::vector<double> &map,
    const std::vector<std::string> &extraHeaders = std::vector<std::string>()
) {
    HdrImage image;
    image.width = width;
    image.height = height;
    image.rgb.resize(width * height * 3, 0.0f);
    for (size_t i = 0; i < width * height; ++i) {
        const float value = static_cast<float>(map[i]);
        image.rgb[i * 3 + 0] = value;
        image.rgb[i * 3 + 1] = value;
        image.rgb[i * 3 + 2] = value;
    }
    writeRadianceHDR(path, image, extraHeaders, false, true);
}

void writeRgbMapHdr(
    const std::string &path,
    size_t width,
    size_t height,
    const std::vector<double> &rgb,
    const std::vector<std::string> &extraHeaders = std::vector<std::string>()
) {
    if (rgb.size() < width * height * 3)
        throw std::runtime_error("RGB debug map is incomplete: " + path);
    HdrImage image;
    image.width = width;
    image.height = height;
    image.rgb.resize(width * height * 3, 0.0f);
    for (size_t i = 0; i < width * height * 3; ++i)
        image.rgb[i] = static_cast<float>(rgb[i]);
    writeRadianceHDR(path, image, extraHeaders, false, true);
}

void writeGrayMapRaw(
    const std::string &path,
    size_t width,
    size_t height,
    const std::vector<double> &map
) {
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("Failed to open raw map for writing: " + path);
    const uint64_t h = static_cast<uint64_t>(height);
    const uint64_t w = static_cast<uint64_t>(width);
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));
    out.write(reinterpret_cast<const char*>(&w), sizeof(w));
    out.write(reinterpret_cast<const char*>(map.data()), static_cast<std::streamsize>(map.size() * sizeof(double)));
}

struct PerceptualResponseComponentMaps {
    std::vector<double> l;
    std::vector<double> m;
    std::vector<double> rod;
    std::vector<double> sum;
};

PerceptualResponseComponentMaps computePerceptualResponseComponents(
    const NativeHdrvdpPreparedImage &prepared,
    const NativeHdrvdpSpectralContext &spectralContext,
    const NativeHdrvdpPnLookup &pn,
    const NativeHdrvdpParams &params,
    size_t width,
    size_t height,
    double pixelsPerDegree
) {
    const size_t pixelCount = width * height;
    PerceptualResponseComponentMaps maps;
    maps.l.resize(pixelCount, 0.0);
    maps.m.resize(pixelCount, 0.0);
    maps.rod.resize(pixelCount, 0.0);
    maps.sum.resize(pixelCount, 0.0);

    const double meanAdapt = std::max(geoMeanPositive(prepared.adaptation), 1e-8);
    const double area = (static_cast<double>(width) * static_cast<double>(height)) /
        std::max(pixelsPerDegree * pixelsPerDegree, 1e-8);
    const double dRef = pupilDUnified(meanAdapt, area, 28.0);
    const double dAge = pupilDUnified(meanAdapt, area, params.age);
    const double lumReduction = (dRef > 1e-8) ? ((dAge * dAge) / (dRef * dRef)) : 1.0;

    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(pixelCount); ++i) {
        const size_t idx = static_cast<size_t>(i);
        const double workR = prepared.nativeRgb[idx * 3 + 0];
        const double workG = prepared.nativeRgb[idx * 3 + 1];
        const double workB = prepared.nativeRgb[idx * 3 + 2];
        const double lResp = (workR * spectralContext.nativeToLmsr[0][0] +
                              workG * spectralContext.nativeToLmsr[1][0] +
                              workB * spectralContext.nativeToLmsr[2][0]) * lumReduction;
        const double mResp = (workR * spectralContext.nativeToLmsr[0][1] +
                              workG * spectralContext.nativeToLmsr[1][1] +
                              workB * spectralContext.nativeToLmsr[2][1]) * lumReduction;
        const double rodResp = (workR * spectralContext.nativeToLmsr[0][3] +
                                workG * spectralContext.nativeToLmsr[1][3] +
                                workB * spectralContext.nativeToLmsr[2][3]) * lumReduction;
        maps.l[idx] = pnLookupValue(pn.coneY, pn.coneJnd, std::max(lResp, 1e-8));
        maps.m[idx] = pnLookupValue(pn.coneY, pn.coneJnd, std::max(mResp, 1e-8));
        maps.rod[idx] = pnLookupValue(pn.rodY, pn.rodJnd, std::max(rodResp, 1e-8));
        maps.sum[idx] = maps.l[idx] + maps.m[idx] + maps.rod[idx];
    }

    return maps;
}
}

void writeViewVisibilityDebugDump(const ViewVisibilitySummaryOptions &opts, const std::string &outDir) {
    if (opts.detailMode != ViewVisibilitySummaryOptions::DetailMode::HdrvdpSubset) {
        throw std::runtime_error("Debug dump currently supports only HDR-VDP3 detail mode.");
    }
    namespace fs = std::filesystem;
    fs::create_directories(outDir);

    const HdrImage &reference = readRadianceHDRCached(opts.referencePath, false, false);
    const HdrImage &test = readRadianceHDRCached(opts.testPath, false, false);
    const HdrImage &mask = readRadianceHDRCached(opts.maskPath, false, false);
    if (reference.width != test.width || reference.height != test.height ||
        reference.width != mask.width || reference.height != mask.height) {
        throw std::runtime_error("Reference, test, and mask images must have the same resolution.");
    }

    const size_t pixelCount = reference.width * reference.height;
    std::vector<double> maskValues(pixelCount, 0.0);
    std::vector<double> maskThreshold(pixelCount, 0.0);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(pixelCount); ++i) {
        const size_t idx = static_cast<size_t>(i);
        const size_t y = idx / reference.width;
        const size_t x = idx - y * reference.width;
        maskValues[idx] = compareMaskWeightAt(mask, x, y);
        maskThreshold[idx] = radianceRgbLuminanceRawAt(mask, x, y);
    }

    const NativeHdrvdpPreparedImage &referencePrepared = getPreparedNativeHdrvdpImageCached(
        opts.referencePath, opts.referencePath, opts.spectralEmissionPath, opts.sensitivityCorrection, opts.pixelsPerDegree,
        HdrvdpPreparedImageMode::DirectChannels, kPreparedAllFields, kPreparedAllFields, false, false, opts.inputColor);
    const NativeHdrvdpPreparedImage &testPrepared = getPreparedNativeHdrvdpImageCached(
        opts.testPath, opts.testPath, opts.spectralEmissionPath, opts.sensitivityCorrection, opts.pixelsPerDegree,
        HdrvdpPreparedImageMode::DirectChannels, kPreparedAllFields, kPreparedAllFields, false, false, opts.inputColor);
    const NativeHdrvdpPreparedImage &referenceSideBySidePrepared = getPreparedNativeHdrvdpImageCached(
        opts.referencePath, opts.referencePath, opts.spectralEmissionPath, opts.sensitivityCorrection, opts.pixelsPerDegree,
        HdrvdpPreparedImageMode::NativeTransformed, kPreparedAllFields, kPreparedAllFields, false, false, opts.inputColor);
    const NativeHdrvdpPreparedImage &testSideBySidePrepared = getPreparedNativeHdrvdpImageCached(
        opts.testPath, opts.testPath, opts.spectralEmissionPath, opts.sensitivityCorrection, opts.pixelsPerDegree,
        HdrvdpPreparedImageMode::NativeTransformed, kPreparedAllFields, kPreparedAllFields, false, false, opts.inputColor);
    const NativeHdrvdpSpectralContext &refSpectralContext =
        getNativeHdrvdpSpectralContextCached(opts.referencePath, opts.spectralEmissionPath, makeNativeHdrvdpParams());
    std::vector<double> refXyzX(pixelCount, 0.0);
    std::vector<double> refXyzY(pixelCount, 0.0);
    std::vector<double> refXyzZ(pixelCount, 0.0);
    std::vector<double> testXyzX(pixelCount, 0.0);
    std::vector<double> testXyzY(pixelCount, 0.0);
    std::vector<double> testXyzZ(pixelCount, 0.0);
    std::vector<double> refInputY(pixelCount, 0.0);
    std::vector<double> testInputY(pixelCount, 0.0);
    std::vector<double> refPostMtfY(pixelCount, 0.0);
    std::vector<double> testPostMtfY(pixelCount, 0.0);
    std::vector<double> refLmSum(pixelCount, 0.0);
    std::vector<double> testLmSum(pixelCount, 0.0);
    std::vector<double> refLogLmSum(pixelCount, 0.0);
    std::vector<double> testLogLmSum(pixelCount, 0.0);
    std::vector<double> refAdaptLogBlur(pixelCount, 0.0);
    std::vector<double> testAdaptLogBlur(pixelCount, 0.0);
    std::vector<double> sideRefInputY(pixelCount, 0.0);
    std::vector<double> sideTestInputY(pixelCount, 0.0);
    std::vector<double> sideRefPostMtfY(pixelCount, 0.0);
    std::vector<double> sideTestPostMtfY(pixelCount, 0.0);
    std::vector<double> sideRefLmSum(pixelCount, 0.0);
    std::vector<double> sideTestLmSum(pixelCount, 0.0);
    std::vector<double> sideRefLogLmSum(pixelCount, 0.0);
    std::vector<double> sideTestLogLmSum(pixelCount, 0.0);
    std::vector<double> sideRefAdaptLogBlur(pixelCount, 0.0);
    std::vector<double> sideTestAdaptLogBlur(pixelCount, 0.0);
    const double refInputScale = viewVisibilityInputScale(reference, opts.inputColor);
    const double testInputScale = viewVisibilityInputScale(test, opts.inputColor);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(pixelCount); ++i) {
        const size_t idx = static_cast<size_t>(i);
        const Vec3d refXyz = viewVisibilityInputXyzAt(reference, idx, opts.inputColor, refInputScale);
        const Vec3d testXyz = viewVisibilityInputXyzAt(test, idx, opts.inputColor, testInputScale);
        refXyzX[idx] = refXyz.x;
        refXyzY[idx] = refXyz.y;
        refXyzZ[idx] = refXyz.z;
        testXyzX[idx] = testXyz.x;
        testXyzY[idx] = testXyz.y;
        testXyzZ[idx] = testXyz.z;
        refInputY[idx] = referencePrepared.nativeRgbPreMtf[idx * 3 + 1];
        testInputY[idx] = testPrepared.nativeRgbPreMtf[idx * 3 + 1];
        refPostMtfY[idx] = referencePrepared.nativeRgb[idx * 3 + 1];
        testPostMtfY[idx] = testPrepared.nativeRgb[idx * 3 + 1];
        refLmSum[idx] = referencePrepared.luminance[idx];
        testLmSum[idx] = testPrepared.luminance[idx];
        refLogLmSum[idx] = std::log(std::max(referencePrepared.luminance[idx], 1e-6));
        testLogLmSum[idx] = std::log(std::max(testPrepared.luminance[idx], 1e-6));
        sideRefInputY[idx] = referenceSideBySidePrepared.nativeRgbPreMtf[idx * 3 + 1];
        sideTestInputY[idx] = testSideBySidePrepared.nativeRgbPreMtf[idx * 3 + 1];
        sideRefPostMtfY[idx] = referenceSideBySidePrepared.nativeRgb[idx * 3 + 1];
        sideTestPostMtfY[idx] = testSideBySidePrepared.nativeRgb[idx * 3 + 1];
        sideRefLmSum[idx] = referenceSideBySidePrepared.luminance[idx];
        sideTestLmSum[idx] = testSideBySidePrepared.luminance[idx];
        sideRefLogLmSum[idx] = std::log(std::max(referenceSideBySidePrepared.luminance[idx], 1e-6));
        sideTestLogLmSum[idx] = std::log(std::max(testSideBySidePrepared.luminance[idx], 1e-6));
    }

    const double adaptSigma = std::pow(10.0, -0.781367) * opts.pixelsPerDegree;
    refAdaptLogBlur = fastGaussLikeMatlab(
        refLogLmSum,
        reference.width,
        reference.height,
        std::max(0.6, adaptSigma),
        true,
        false,
        true,
        0.0);
    testAdaptLogBlur = fastGaussLikeMatlab(
        testLogLmSum,
        test.width,
        test.height,
        std::max(0.6, adaptSigma),
        true,
        false,
        true,
        0.0);
    sideRefAdaptLogBlur = fastGaussLikeMatlab(
        sideRefLogLmSum,
        reference.width,
        reference.height,
        std::max(0.6, adaptSigma),
        true,
        false,
        true,
        0.0);
    sideTestAdaptLogBlur = fastGaussLikeMatlab(
        sideTestLogLmSum,
        test.width,
        test.height,
        std::max(0.6, adaptSigma),
        true,
        false,
        true,
        0.0);

    std::vector<double> diffMask(pixelCount, 0.0);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(pixelCount); ++i) {
        const double refR = referenceSideBySidePrepared.nativeRgbPreMtf[i * 3 + 0];
        const double refG = referenceSideBySidePrepared.nativeRgbPreMtf[i * 3 + 1];
        const double refB = referenceSideBySidePrepared.nativeRgbPreMtf[i * 3 + 2];
        const double testR = testSideBySidePrepared.nativeRgbPreMtf[i * 3 + 0];
        const double testG = testSideBySidePrepared.nativeRgbPreMtf[i * 3 + 1];
        const double testB = testSideBySidePrepared.nativeRgbPreMtf[i * 3 + 2];
        const bool different =
            hdrvdpDiffMaskChannelChanged(refR, testR) ||
            hdrvdpDiffMaskChannelChanged(refG, testG) ||
            hdrvdpDiffMaskChannelChanged(refB, testB);
        diffMask[i] = different ? maskValues[static_cast<size_t>(i)] : 0.0;
    }

    const std::vector<double> refDetectable = computeHdrvdpSubsetContrastMap(
        referencePrepared.achromatic,
        referencePrepared.adaptation,
        reference.width,
        reference.height,
        opts.pixelsPerDegree,
        opts.sensitivityCorrection
    );
    const std::vector<double> testDetectable = computeHdrvdpSubsetContrastMap(
        testPrepared.achromatic,
        testPrepared.adaptation,
        test.width,
        test.height,
        opts.pixelsPerDegree,
        opts.sensitivityCorrection
    );
    const NativeHdrvdpParams params = makeNativeHdrvdpParams();
    const NativeHdrvdpPnLookup &pn = getNativeHdrvdpPnLookupCached(params, opts.sensitivityCorrection);
    const PerceptualResponseComponentMaps refPerceptual = computePerceptualResponseComponents(
        referencePrepared, refSpectralContext, pn, params, reference.width, reference.height, opts.pixelsPerDegree);
    const PerceptualResponseComponentMaps testPerceptual = computePerceptualResponseComponents(
        testPrepared, refSpectralContext, pn, params, test.width, test.height, opts.pixelsPerDegree);
    const PerceptualResponseComponentMaps sideRefPerceptual = computePerceptualResponseComponents(
        referenceSideBySidePrepared, refSpectralContext, pn, params, reference.width, reference.height, opts.pixelsPerDegree);
    const PerceptualResponseComponentMaps sideTestPerceptual = computePerceptualResponseComponents(
        testSideBySidePrepared, refSpectralContext, pn, params, test.width, test.height, opts.pixelsPerDegree);
    VisibilityPyramid refContrastPyramid = buildVisibilityPyramid(referencePrepared.achromatic, reference.width, reference.height, opts.pixelsPerDegree);
    VisibilityPyramid testContrastPyramid = buildVisibilityPyramid(testPrepared.achromatic, test.width, test.height, opts.pixelsPerDegree);
    removeBaseBandDc(refContrastPyramid);
    removeBaseBandDc(testContrastPyramid);
    const HdrvdpCsfLookup &contrastLookup = getHdrvdpCsfLookupCached(refContrastPyramid.levels, params, opts.sensitivityCorrection);
    const HdrvdpSubsetSideBySideResult sideBySide = computeHdrvdpSubsetSideBySide(
        referenceSideBySidePrepared,
        testSideBySidePrepared,
        diffMask,
        reference.width,
        reference.height,
        opts.pixelsPerDegree,
        opts.sensitivityCorrection,
        &outDir,
        true
    );

    std::vector<double> visibleD(pixelCount, 0.0);
    std::vector<double> maskedRef(pixelCount, 0.0);
    std::vector<double> maskedTest(pixelCount, 0.0);
    std::vector<double> pMap(pixelCount, 0.0);
    #if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
    #endif
    for (long long i = 0; i < static_cast<long long>(pixelCount); ++i) {
        const double maskWeight = maskValues[static_cast<size_t>(i)];
        const double undetect = sideBySide.undetectMap[static_cast<size_t>(i)];
        pMap[static_cast<size_t>(i)] = std::max(0.0, std::min(1.0, 1.0 - undetect));
        maskedRef[static_cast<size_t>(i)] = refDetectable[static_cast<size_t>(i)] * maskWeight;
        maskedTest[static_cast<size_t>(i)] = testDetectable[static_cast<size_t>(i)] * maskWeight;
        visibleD[static_cast<size_t>(i)] = undetect * refDetectable[static_cast<size_t>(i)] * maskWeight * maskWeight;
    }

    writeGrayMapHdr((fs::path(outDir) / "native_ref_adapt.hdr").string(), reference.width, reference.height, referencePrepared.adaptation);
    writeGrayMapHdr((fs::path(outDir) / "native_test_adapt.hdr").string(), test.width, test.height, testPrepared.adaptation);
    writeGrayMapRaw((fs::path(outDir) / "native_ref_adapt.bin").string(), reference.width, reference.height, referencePrepared.adaptation);
    writeGrayMapRaw((fs::path(outDir) / "native_test_adapt.bin").string(), test.width, test.height, testPrepared.adaptation);
    writeGrayMapHdr((fs::path(outDir) / "native_side_ref_adapt.hdr").string(), reference.width, reference.height, referenceSideBySidePrepared.adaptation);
    writeGrayMapHdr((fs::path(outDir) / "native_side_test_adapt.hdr").string(), test.width, test.height, testSideBySidePrepared.adaptation);
    writeGrayMapRaw((fs::path(outDir) / "native_side_ref_adapt.bin").string(), reference.width, reference.height, referenceSideBySidePrepared.adaptation);
    writeGrayMapRaw((fs::path(outDir) / "native_side_test_adapt.bin").string(), test.width, test.height, testSideBySidePrepared.adaptation);
    writeRgbMapHdr((fs::path(outDir) / "native_ref_pre_mtf_rgb.hdr").string(), reference.width, reference.height, referencePrepared.nativeRgbPreMtf,
        {"MERGEHDR_VIEWVIS_DEBUG_STEP= direct_pre_mtf_rgb", "MERGEHDR_VIEWVIS_DEBUG_CHANNELS= native_r native_g native_b"});
    writeRgbMapHdr((fs::path(outDir) / "native_test_pre_mtf_rgb.hdr").string(), test.width, test.height, testPrepared.nativeRgbPreMtf,
        {"MERGEHDR_VIEWVIS_DEBUG_STEP= direct_pre_mtf_rgb", "MERGEHDR_VIEWVIS_DEBUG_CHANNELS= native_r native_g native_b"});
    writeRgbMapHdr((fs::path(outDir) / "native_ref_post_mtf_rgb.hdr").string(), reference.width, reference.height, referencePrepared.nativeRgb,
        {"MERGEHDR_VIEWVIS_DEBUG_STEP= direct_post_mtf_rgb", "MERGEHDR_VIEWVIS_DEBUG_CHANNELS= native_r native_g native_b"});
    writeRgbMapHdr((fs::path(outDir) / "native_test_post_mtf_rgb.hdr").string(), test.width, test.height, testPrepared.nativeRgb,
        {"MERGEHDR_VIEWVIS_DEBUG_STEP= direct_post_mtf_rgb", "MERGEHDR_VIEWVIS_DEBUG_CHANNELS= native_r native_g native_b"});
    writeRgbMapHdr((fs::path(outDir) / "native_side_ref_pre_mtf_rgb.hdr").string(), reference.width, reference.height, referenceSideBySidePrepared.nativeRgbPreMtf,
        {"MERGEHDR_VIEWVIS_DEBUG_STEP= side_by_side_pre_mtf_rgb", "MERGEHDR_VIEWVIS_DEBUG_CHANNELS= native_r native_g native_b"});
    writeRgbMapHdr((fs::path(outDir) / "native_side_test_pre_mtf_rgb.hdr").string(), test.width, test.height, testSideBySidePrepared.nativeRgbPreMtf,
        {"MERGEHDR_VIEWVIS_DEBUG_STEP= side_by_side_pre_mtf_rgb", "MERGEHDR_VIEWVIS_DEBUG_CHANNELS= native_r native_g native_b"});
    writeRgbMapHdr((fs::path(outDir) / "native_side_ref_post_mtf_rgb.hdr").string(), reference.width, reference.height, referenceSideBySidePrepared.nativeRgb,
        {"MERGEHDR_VIEWVIS_DEBUG_STEP= side_by_side_post_mtf_rgb", "MERGEHDR_VIEWVIS_DEBUG_CHANNELS= native_r native_g native_b"});
    writeRgbMapHdr((fs::path(outDir) / "native_side_test_post_mtf_rgb.hdr").string(), test.width, test.height, testSideBySidePrepared.nativeRgb,
        {"MERGEHDR_VIEWVIS_DEBUG_STEP= side_by_side_post_mtf_rgb", "MERGEHDR_VIEWVIS_DEBUG_CHANNELS= native_r native_g native_b"});
    writeGrayMapHdr((fs::path(outDir) / "native_side_ref_input_y.hdr").string(), reference.width, reference.height, sideRefInputY);
    writeGrayMapHdr((fs::path(outDir) / "native_side_test_input_y.hdr").string(), test.width, test.height, sideTestInputY);
    writeGrayMapRaw((fs::path(outDir) / "native_side_ref_input_y.bin").string(), reference.width, reference.height, sideRefInputY);
    writeGrayMapRaw((fs::path(outDir) / "native_side_test_input_y.bin").string(), test.width, test.height, sideTestInputY);
    writeGrayMapRaw((fs::path(outDir) / "native_side_ref_native_r.bin").string(), reference.width, reference.height, extractChannel(referenceSideBySidePrepared.nativeRgbPreMtf, 0));
    writeGrayMapRaw((fs::path(outDir) / "native_side_ref_native_g.bin").string(), reference.width, reference.height, extractChannel(referenceSideBySidePrepared.nativeRgbPreMtf, 1));
    writeGrayMapRaw((fs::path(outDir) / "native_side_ref_native_b.bin").string(), reference.width, reference.height, extractChannel(referenceSideBySidePrepared.nativeRgbPreMtf, 2));
    writeGrayMapRaw((fs::path(outDir) / "native_side_test_native_r.bin").string(), test.width, test.height, extractChannel(testSideBySidePrepared.nativeRgbPreMtf, 0));
    writeGrayMapRaw((fs::path(outDir) / "native_side_test_native_g.bin").string(), test.width, test.height, extractChannel(testSideBySidePrepared.nativeRgbPreMtf, 1));
    writeGrayMapRaw((fs::path(outDir) / "native_side_test_native_b.bin").string(), test.width, test.height, extractChannel(testSideBySidePrepared.nativeRgbPreMtf, 2));
    writeGrayMapHdr((fs::path(outDir) / "native_side_ref_post_mtf_y.hdr").string(), reference.width, reference.height, sideRefPostMtfY);
    writeGrayMapHdr((fs::path(outDir) / "native_side_test_post_mtf_y.hdr").string(), test.width, test.height, sideTestPostMtfY);
    writeGrayMapRaw((fs::path(outDir) / "native_side_ref_post_mtf_y.bin").string(), reference.width, reference.height, sideRefPostMtfY);
    writeGrayMapRaw((fs::path(outDir) / "native_side_test_post_mtf_y.bin").string(), test.width, test.height, sideTestPostMtfY);
    writeGrayMapHdr((fs::path(outDir) / "native_side_ref_lm_sum.hdr").string(), reference.width, reference.height, sideRefLmSum);
    writeGrayMapHdr((fs::path(outDir) / "native_side_test_lm_sum.hdr").string(), test.width, test.height, sideTestLmSum);
    writeGrayMapRaw((fs::path(outDir) / "native_side_ref_lm_sum.bin").string(), reference.width, reference.height, sideRefLmSum);
    writeGrayMapRaw((fs::path(outDir) / "native_side_test_lm_sum.bin").string(), test.width, test.height, sideTestLmSum);
    writeGrayMapHdr((fs::path(outDir) / "native_side_ref_log_lm_sum.hdr").string(), reference.width, reference.height, sideRefLogLmSum);
    writeGrayMapHdr((fs::path(outDir) / "native_side_test_log_lm_sum.hdr").string(), test.width, test.height, sideTestLogLmSum);
    writeGrayMapRaw((fs::path(outDir) / "native_side_ref_log_lm_sum.bin").string(), reference.width, reference.height, sideRefLogLmSum);
    writeGrayMapRaw((fs::path(outDir) / "native_side_test_log_lm_sum.bin").string(), test.width, test.height, sideTestLogLmSum);
    writeGrayMapHdr((fs::path(outDir) / "native_side_ref_adapt_log_blur.hdr").string(), reference.width, reference.height, sideRefAdaptLogBlur);
    writeGrayMapHdr((fs::path(outDir) / "native_side_test_adapt_log_blur.hdr").string(), test.width, test.height, sideTestAdaptLogBlur);
    writeGrayMapRaw((fs::path(outDir) / "native_side_ref_adapt_log_blur.bin").string(), reference.width, reference.height, sideRefAdaptLogBlur);
    writeGrayMapRaw((fs::path(outDir) / "native_side_test_adapt_log_blur.bin").string(), test.width, test.height, sideTestAdaptLogBlur);
    writeGrayMapHdr((fs::path(outDir) / "native_ref_input_y.hdr").string(), reference.width, reference.height, refInputY);
    writeGrayMapHdr((fs::path(outDir) / "native_test_input_y.hdr").string(), test.width, test.height, testInputY);
    writeGrayMapRaw((fs::path(outDir) / "native_ref_xyz_x.bin").string(), reference.width, reference.height, refXyzX);
    writeGrayMapRaw((fs::path(outDir) / "native_ref_xyz_y.bin").string(), reference.width, reference.height, refXyzY);
    writeGrayMapRaw((fs::path(outDir) / "native_ref_xyz_z.bin").string(), reference.width, reference.height, refXyzZ);
    writeGrayMapRaw((fs::path(outDir) / "native_test_xyz_x.bin").string(), test.width, test.height, testXyzX);
    writeGrayMapRaw((fs::path(outDir) / "native_test_xyz_y.bin").string(), test.width, test.height, testXyzY);
    writeGrayMapRaw((fs::path(outDir) / "native_test_xyz_z.bin").string(), test.width, test.height, testXyzZ);
    writeGrayMapRaw((fs::path(outDir) / "native_ref_input_y.bin").string(), reference.width, reference.height, refInputY);
    writeGrayMapRaw((fs::path(outDir) / "native_test_input_y.bin").string(), test.width, test.height, testInputY);
    writeGrayMapHdr((fs::path(outDir) / "native_ref_post_mtf_y.hdr").string(), reference.width, reference.height, refPostMtfY);
    writeGrayMapHdr((fs::path(outDir) / "native_test_post_mtf_y.hdr").string(), test.width, test.height, testPostMtfY);
    writeGrayMapRaw((fs::path(outDir) / "native_ref_post_mtf_y.bin").string(), reference.width, reference.height, refPostMtfY);
    writeGrayMapRaw((fs::path(outDir) / "native_test_post_mtf_y.bin").string(), test.width, test.height, testPostMtfY);
    writeGrayMapHdr((fs::path(outDir) / "native_ref_lm_sum.hdr").string(), reference.width, reference.height, refLmSum);
    writeGrayMapHdr((fs::path(outDir) / "native_test_lm_sum.hdr").string(), test.width, test.height, testLmSum);
    writeGrayMapRaw((fs::path(outDir) / "native_ref_lm_sum.bin").string(), reference.width, reference.height, refLmSum);
    writeGrayMapRaw((fs::path(outDir) / "native_test_lm_sum.bin").string(), test.width, test.height, testLmSum);
    writeGrayMapHdr((fs::path(outDir) / "native_ref_log_lm_sum.hdr").string(), reference.width, reference.height, refLogLmSum);
    writeGrayMapHdr((fs::path(outDir) / "native_test_log_lm_sum.hdr").string(), test.width, test.height, testLogLmSum);
    writeGrayMapRaw((fs::path(outDir) / "native_ref_log_lm_sum.bin").string(), reference.width, reference.height, refLogLmSum);
    writeGrayMapRaw((fs::path(outDir) / "native_test_log_lm_sum.bin").string(), test.width, test.height, testLogLmSum);
    writeGrayMapHdr((fs::path(outDir) / "native_ref_adapt_log_blur.hdr").string(), reference.width, reference.height, refAdaptLogBlur);
    writeGrayMapHdr((fs::path(outDir) / "native_test_adapt_log_blur.hdr").string(), test.width, test.height, testAdaptLogBlur);
    writeGrayMapRaw((fs::path(outDir) / "native_ref_adapt_log_blur.bin").string(), reference.width, reference.height, refAdaptLogBlur);
    writeGrayMapRaw((fs::path(outDir) / "native_test_adapt_log_blur.bin").string(), test.width, test.height, testAdaptLogBlur);
    writeGrayMapHdr((fs::path(outDir) / "native_ref_l_response.hdr").string(), reference.width, reference.height, referencePrepared.lResponse);
    writeGrayMapHdr((fs::path(outDir) / "native_test_l_response.hdr").string(), test.width, test.height, testPrepared.lResponse);
    writeGrayMapHdr((fs::path(outDir) / "native_ref_m_response.hdr").string(), reference.width, reference.height, referencePrepared.mResponse);
    writeGrayMapHdr((fs::path(outDir) / "native_test_m_response.hdr").string(), test.width, test.height, testPrepared.mResponse);
    writeGrayMapHdr((fs::path(outDir) / "native_ref_rod_response.hdr").string(), reference.width, reference.height, referencePrepared.rodResponse);
    writeGrayMapHdr((fs::path(outDir) / "native_test_rod_response.hdr").string(), test.width, test.height, testPrepared.rodResponse);
    writeGrayMapHdr((fs::path(outDir) / "native_ref_p_l.hdr").string(), reference.width, reference.height, refPerceptual.l);
    writeGrayMapHdr((fs::path(outDir) / "native_test_p_l.hdr").string(), test.width, test.height, testPerceptual.l);
    writeGrayMapHdr((fs::path(outDir) / "native_ref_p_m.hdr").string(), reference.width, reference.height, refPerceptual.m);
    writeGrayMapHdr((fs::path(outDir) / "native_test_p_m.hdr").string(), test.width, test.height, testPerceptual.m);
    writeGrayMapHdr((fs::path(outDir) / "native_ref_p_rod.hdr").string(), reference.width, reference.height, refPerceptual.rod);
    writeGrayMapHdr((fs::path(outDir) / "native_test_p_rod.hdr").string(), test.width, test.height, testPerceptual.rod);
    writeGrayMapHdr((fs::path(outDir) / "native_ref_p_sum.hdr").string(), reference.width, reference.height, refPerceptual.sum);
    writeGrayMapHdr((fs::path(outDir) / "native_test_p_sum.hdr").string(), test.width, test.height, testPerceptual.sum);
    writeGrayMapHdr((fs::path(outDir) / "native_side_ref_p_sum.hdr").string(), reference.width, reference.height, sideRefPerceptual.sum);
    writeGrayMapHdr((fs::path(outDir) / "native_side_test_p_sum.hdr").string(), test.width, test.height, sideTestPerceptual.sum);
    writeGrayMapHdr((fs::path(outDir) / "native_ref_P.hdr").string(), reference.width, reference.height, referencePrepared.achromatic);
    writeGrayMapHdr((fs::path(outDir) / "native_test_P.hdr").string(), test.width, test.height, testPrepared.achromatic);
    writeGrayMapRaw((fs::path(outDir) / "native_ref_P.bin").string(), reference.width, reference.height, referencePrepared.achromatic);
    writeGrayMapRaw((fs::path(outDir) / "native_test_P.bin").string(), test.width, test.height, testPrepared.achromatic);
    writeGrayMapHdr((fs::path(outDir) / "native_ref_C.hdr").string(), reference.width, reference.height, maskedRef);
    writeGrayMapHdr((fs::path(outDir) / "native_test_C.hdr").string(), test.width, test.height, maskedTest);
    writeGrayMapRaw((fs::path(outDir) / "native_ref_C.bin").string(), reference.width, reference.height, maskedRef);
    writeGrayMapRaw((fs::path(outDir) / "native_test_C.bin").string(), test.width, test.height, maskedTest);
    writeGrayMapHdr((fs::path(outDir) / "native_P_map.hdr").string(), reference.width, reference.height, pMap);
    writeGrayMapHdr((fs::path(outDir) / "native_D.hdr").string(), reference.width, reference.height, visibleD);
    writeGrayMapRaw((fs::path(outDir) / "native_P_map.bin").string(), reference.width, reference.height, pMap);
    writeGrayMapRaw((fs::path(outDir) / "native_D.bin").string(), reference.width, reference.height, visibleD);
    writeGrayMapHdr((fs::path(outDir) / "native_mask_values.hdr").string(), reference.width, reference.height, maskValues);
    writeGrayMapHdr((fs::path(outDir) / "native_mask_threshold.hdr").string(), reference.width, reference.height, maskThreshold);
    writeGrayMapHdr((fs::path(outDir) / "native_diff_mask.hdr").string(), reference.width, reference.height, diffMask);
    writeGrayMapRaw((fs::path(outDir) / "native_mask_values.bin").string(), reference.width, reference.height, maskValues);
    writeGrayMapRaw((fs::path(outDir) / "native_mask_threshold.bin").string(), reference.width, reference.height, maskThreshold);
    writeGrayMapRaw((fs::path(outDir) / "native_diff_mask.bin").string(), reference.width, reference.height, diffMask);
    writeSpectralCurveCsv((fs::path(outDir) / "native_emission_pre_aod.csv").string(), refSpectralContext.emissionPreAod);
    writeSpectralCurveCsv((fs::path(outDir) / "native_emission_post_aod.csv").string(), refSpectralContext.emissionPostAod);
    writeSpectralCurveCsv((fs::path(outDir) / "native_xyz_cmf.csv").string(), refSpectralContext.xyzCmf);
    writeSpectralCurveCsv((fs::path(outDir) / "native_lms_sens.csv").string(), refSpectralContext.lmsSens);
    writeSpectralCurveCsv((fs::path(outDir) / "native_rod_sens.csv").string(), refSpectralContext.rodSens);

    std::ofstream manifest((fs::path(outDir) / "native_step_manifest.txt").string());
    manifest << "View visibility native step maps\n";
    manifest << "1. *_pre_mtf_rgb.hdr: XYZ/direct or display-native RGB before eye optical MTF.\n";
    manifest << "2. *_post_mtf_rgb.hdr and *_post_mtf_y.hdr: image after the optical MTF frequency filter.\n";
    manifest << "3. *_l_response.hdr, *_m_response.hdr, *_rod_response.hdr: physical cone/rod responses after MTF.\n";
    manifest << "4. *_p_l.hdr, *_p_m.hdr, *_p_rod.hdr, *_p_sum.hdr: perceptual/JND-like components; p_sum equals the achromatic response used for contrast.\n";
    manifest << "5. *_lm_sum.hdr: L+M luminance used for local adaptation.\n";
    manifest << "6. *_log_lm_sum.hdr: log(L+M) before adaptation blur.\n";
    manifest << "7. *_adapt_log_blur.hdr: blurred log-luminance before exp().\n";
    manifest << "8. *_adapt.hdr: local adaptation map exp(gaussian(log(L+M))).\n";
    manifest << "9. native_ref_C.hdr/native_test_C.hdr: detectable contrast maps inside the mask.\n";
    manifest << "10. native_P_map.hdr: probability of detecting a difference; native_D.hdr: preserved reference visibility.\n";
    manifest << "adaptation_sigma_px=" << std::max(0.6, adaptSigma) << "\n";
    manifest << "ppd=" << opts.pixelsPerDegree << "\n";
    manifest << "input_color=" << viewVisibilityInputColorLabel(opts.inputColor) << "\n";

    std::ofstream contrastBands((fs::path(outDir) / "native_contrast_band_debug.csv").string());
    contrastBands << "image,band,orient,freq,band_mean,band_abs_mean,csf_mean,response_mean,response_abs_mean\n";
    const auto dumpContrastBandDebug = [&](const char *label, const VisibilityPyramid &pyramid, const std::vector<double> &adaptMap) {
        const double eps = 1e-8;
        for (size_t levelIndex = 0; levelIndex < pyramid.levels.size(); ++levelIndex) {
            const VisibilityPyramidLevel &level = pyramid.levels[levelIndex];
            const std::vector<double> adaptResized = resizeMatlabBicubic(adaptMap, reference.width, reference.height, level.width, level.height);
            const double bandNorm = std::pow(2.0, static_cast<double>(levelIndex));
            std::vector<double> rawBand;
            std::vector<double> rawCsf;
            std::vector<double> rawResponse;
            if (levelIndex < 3) {
                rawBand.resize(level.band.size(), 0.0);
                rawCsf.resize(level.band.size(), 0.0);
                rawResponse.resize(level.band.size(), 0.0);
            }
            double bandMean = 0.0;
            double bandAbsMean = 0.0;
            double csfMean = 0.0;
            double responseMean = 0.0;
            double responseAbsMean = 0.0;
            for (size_t i = 0; i < level.band.size(); ++i) {
                const double bandValue = level.band[i] / std::max(bandNorm, 1.0);
                if (levelIndex < 3)
                    rawBand[i] = bandValue;
                bandMean += bandValue;
                bandAbsMean += std::abs(bandValue);
                const double adapt = std::max(eps, adaptResized[i]);
                const double csf = sampleHdrvdpCsfLookup(contrastLookup, levelIndex, adapt);
                if (levelIndex < 3)
                    rawCsf[i] = csf;
                csfMean += csf;
                double response = 0.0;
                if (!(levelIndex == pyramid.levels.size() - 1 || level.frequencyCpd <= 2.0)) {
                    const double visibleProbability = psychometricProbability(bandValue * csf, params);
                    response = std::log(1.0 - visibleProbability + 1e-8);
                }
                if (levelIndex < 3)
                    rawResponse[i] = response;
                responseMean += response;
                responseAbsMean += std::abs(response);
            }
            const double count = std::max<size_t>(1, level.band.size());
            contrastBands << label << ","
                          << (levelIndex + 1) << ",1," << level.frequencyCpd << ","
                          << (bandMean / count) << ","
                          << (bandAbsMean / count) << ","
                          << (csfMean / count) << ","
                          << (responseMean / count) << ","
                          << (responseAbsMean / count) << "\n";
            if (levelIndex < 3) {
                const std::string prefix = std::string("native_") + label + "_contrast_band" + std::to_string(levelIndex + 1) + "_orient1_";
                writeGrayMapRaw((fs::path(outDir) / (prefix + "input.bin")).string(), level.width, level.height, rawBand);
                writeGrayMapRaw((fs::path(outDir) / (prefix + "csf.bin")).string(), level.width, level.height, rawCsf);
                writeGrayMapRaw((fs::path(outDir) / (prefix + "response.bin")).string(), level.width, level.height, rawResponse);
            }
        }
    };
    dumpContrastBandDebug("ref", refContrastPyramid, referencePrepared.adaptation);
    dumpContrastBandDebug("test", testContrastPyramid, testPrepared.adaptation);

    std::ofstream meta((fs::path(outDir) / "native_summary.txt").string());
    meta << std::setprecision(12);
    meta << "quality=" << sideBySide.qualityJod << "\n";
    meta << "input_color=" << viewVisibilityInputColorLabel(opts.inputColor) << "\n";
    writeGrayMapHdr((fs::path(outDir) / "native_side_C_map.hdr").string(), reference.width, reference.height, sideBySide.contrastMap);
    writeGrayMapHdr((fs::path(outDir) / "native_side_P_map.hdr").string(), reference.width, reference.height, sideBySide.probabilityMap);
    writeGrayMapRaw((fs::path(outDir) / "native_side_C_map.bin").string(), reference.width, reference.height, sideBySide.contrastMap);
    writeGrayMapRaw((fs::path(outDir) / "native_side_P_map.bin").string(), reference.width, reference.height, sideBySide.probabilityMap);
    meta << "xyzToNative=\n";
    for (int row = 0; row < 3; ++row)
        meta << refSpectralContext.xyzToNative.m[row][0] << ","
             << refSpectralContext.xyzToNative.m[row][1] << ","
             << refSpectralContext.xyzToNative.m[row][2] << "\n";
    meta << "xyzToNativePreAod=\n";
    for (int row = 0; row < 3; ++row)
        meta << refSpectralContext.xyzToNativePreAod.m[row][0] << ","
             << refSpectralContext.xyzToNativePreAod.m[row][1] << ","
             << refSpectralContext.xyzToNativePreAod.m[row][2] << "\n";
    meta << "nativeToLmsr=\n";
    for (int row = 0; row < 3; ++row)
        meta << refSpectralContext.nativeToLmsr[row][0] << ","
             << refSpectralContext.nativeToLmsr[row][1] << ","
             << refSpectralContext.nativeToLmsr[row][2] << ","
             << refSpectralContext.nativeToLmsr[row][3] << "\n";

}

std::string formatEvalGlare(const EvalGlareAnalysisResult &result, bool detailed, bool simple) {
    if (!result.outputText.empty() && result.outputDetailed == detailed && result.outputSimple == simple)
        return result.outputText;
    return eg::formatFromResult(result, detailed, simple);
}

int printEvalGlare(const EvalGlareOptions &opts) {
    EvalGlareAnalysisResult res;
    eg::Io io;
    try {
        runEvalGlareInto(opts, res, io);
    } catch (...) {
        std::cout << io.out << std::flush;
        std::cerr << io.err << std::flush;
        throw;
    }
    std::cout << res.outputText << std::flush;
    std::cerr << res.messages << std::flush;
    return res.exitStatus;
}
