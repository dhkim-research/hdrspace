#include "camera_detect.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>

#include <libraw/libraw.h>

namespace {

bool hasXYZCamValues(const float matrix[4][3], int colors) {
    const int channels = std::min(colors, 4);
    for (int row = 0; row < channels; ++row)
        for (int col = 0; col < 3; ++col)
            if (std::fabs(matrix[row][col]) > 1e-8f)
                return true;
    return false;
}

bool isDaylightIlluminant(unsigned illuminant) {
    return illuminant == LIBRAW_WBI_D65 || illuminant == LIBRAW_WBI_Daylight ||
           illuminant == LIBRAW_WBI_D55 || illuminant == LIBRAW_WBI_D75 ||
           illuminant == LIBRAW_WBI_D50 || illuminant == LIBRAW_WBI_Flash;
}

bool dngDaylightXYZCam(const libraw_colordata_t &color, int colors, Eigen::Matrix3f &matrix) {
    int selected = -1;
    for (int index = 0; index < 2; ++index) {
        if (color.dng_color[index].illuminant == LIBRAW_WBI_D65 &&
            hasXYZCamValues(color.dng_color[index].colormatrix, colors)) {
            selected = index;
            break;
        }
    }

    if (selected < 0) {
        for (int index = 0; index < 2; ++index) {
            if (isDaylightIlluminant(color.dng_color[index].illuminant) &&
                hasXYZCamValues(color.dng_color[index].colormatrix, colors)) {
                selected = index;
                break;
            }
        }
    }

    if (selected < 0)
        return false;

    const libraw_dng_color_t &dng = color.dng_color[selected];
    const bool haveCalibration = (dng.parsedfields & LIBRAW_DNGFM_CALIBRATION) != 0;
    const bool haveAnalogBalance =
        (color.dng_levels.parsedfields & LIBRAW_DNGFM_ANALOGBALANCE) != 0;
    const int channels = std::min(colors, 3);

    matrix.setZero();
    for (int row = 0; row < channels; ++row) {
        for (int col = 0; col < 3; ++col) {
            double value = 0.0;
            for (int inner = 0; inner < channels; ++inner) {
                const double calibration = haveCalibration
                    ? dng.calibration[row][inner]
                    : (row == inner ? 1.0 : 0.0);
                value += calibration * dng.colormatrix[inner][col];
            }
            if (haveAnalogBalance)
                value *= color.dng_levels.analogbalance[row];
            matrix(row, col) = static_cast<float>(value);
        }
    }
    return matrix.cwiseAbs().maxCoeff() > 1e-8f;
}

struct ObservedRawScale {
    int maximum = 0;
    std::size_t samplesAboveMismatch = 0;
    std::size_t totalSamples = 0;
};

ObservedRawScale observeRawScale(LibRaw &raw, int blacklevel, int decoderWhitepoint) {
    ObservedRawScale stats;
    if (!raw.imgdata.image)
        return stats;

    const libraw_image_sizes_t &sizes = raw.imgdata.sizes;
    const std::size_t width = sizes.iwidth ? sizes.iwidth : sizes.width;
    const std::size_t height = sizes.iheight ? sizes.iheight : sizes.height;
    const int range = decoderWhitepoint - blacklevel;
    const int mismatchThreshold = range > 0
        ? std::min(65535, blacklevel + 2 * range)
        : 65535;

    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const int channel = raw.fcol(static_cast<int>(y), static_cast<int>(x));
            if (channel < 0 || channel > 3)
                continue;
            const int value = static_cast<int>(raw.imgdata.image[y * width + x][channel]) + blacklevel;
            stats.maximum = std::max(stats.maximum, value);
            if (value > mismatchThreshold)
                ++stats.samplesAboveMismatch;
            ++stats.totalSamples;
        }
    }
    return stats;
}

bool prepareBlackNormalizedRaw(LibRaw &raw, int &blacklevel, int &absoluteWhitepoint,
        std::string *errorMessage) {
    const libraw_colordata_t &sourceColor = raw.imgdata.color;
    blacklevel = effectiveRawBlackLevel(
        sourceColor.black, sourceColor.cblack,
        sizeof(sourceColor.cblack) / sizeof(sourceColor.cblack[0]));
    const int sourceWhitepoint = sourceColor.maximum
        ? static_cast<int>(sourceColor.maximum)
        : 0x3FFF;

    int err = raw.raw2image();
    if (err != LIBRAW_SUCCESS) {
        if (errorMessage)
            *errorMessage = std::string("LibRaw raw2image failed: ") + libraw_strerror(err);
        return false;
    }
    err = raw.subtract_black();
    if (err != LIBRAW_SUCCESS) {
        if (errorMessage)
            *errorMessage = std::string("LibRaw subtract_black failed: ") + libraw_strerror(err);
        return false;
    }

    // subtract_black() applies LibRaw's complete per-channel and repeated
    // BlackLevel table and changes maximum to the usable level above the
    // common black component. Add our reporting scalar back so the existing
    // merge core can continue to use one scalar black/absolute-white pair.
    const int usableWhitepoint = raw.imgdata.color.maximum
        ? static_cast<int>(raw.imgdata.color.maximum)
        : std::max(1, sourceWhitepoint - blacklevel);
    absoluteWhitepoint = std::min(65535, blacklevel + usableWhitepoint);
    return true;
}

} // namespace

int effectiveRawBlackLevel(unsigned black, const unsigned *cblack, std::size_t cblackCount) {
    // LibRaw layout: cblack[4] and cblack[5] hold BlackLevelRepeatDim,
    // followed by the repeated pattern at cblack[6]. cblack[0..3] are
    // channel corrections. The scalar is only a reporting/reconstruction
    // baseline; LibRaw performs the actual per-pixel subtraction.
    std::uint64_t total = black;

    if (cblack && cblackCount != 0) {
        const std::size_t channelCount = std::min<std::size_t>(4, cblackCount);
        std::uint64_t channelSum = 0;
        for (std::size_t i = 0; i < channelCount; ++i)
            channelSum += cblack[i];
        if (channelCount != 0) {
            total = black + (channelSum + channelCount / 2) / channelCount;
        }
    }

    if (cblack && cblackCount > 6 && cblack[4] != 0 && cblack[5] != 0) {
        const std::size_t rows = cblack[4];
        const std::size_t columns = cblack[5];
        const std::size_t available = cblackCount - 6;
        if (columns <= available && rows <= available / columns) {
            const std::size_t patternCount = rows * columns;
            std::uint64_t sum = 0;
            for (std::size_t i = 0; i < patternCount; ++i)
                sum += cblack[6 + i];
            total += (sum + patternCount / 2) / patternCount;
        }
    }
    return static_cast<int>(std::min<std::uint64_t>(65535, total));
}

bool detectXYZCamAuto(const std::string &filename, CameraDetectInfo &info, std::string *errorMessage) {
    LibRaw raw;
    int err = raw.open_file(filename.c_str());
    if (err != LIBRAW_SUCCESS) {
        if (errorMessage)
            *errorMessage = libraw_strerror(err);
        return false;
    }

    bool haveMatrix = false;
    for (int row = 0; row < 3; ++row) {
        info.rgbgMultipliers[row] = 1.0f;
        info.d65Multipliers[row] = static_cast<float>(raw.imgdata.color.pre_mul[row]);
        for (int col = 0; col < 3; ++col) {
            float value = static_cast<float>(raw.imgdata.color.cam_xyz[row][col]);
            info.xyzcam(row, col) = value;
            haveMatrix = haveMatrix || std::fabs(value) > 1e-8f;
        }
    }
    info.rgbgMultipliers[3] = 1.0f;
    info.d65Multipliers[3] = static_cast<float>(raw.imgdata.color.pre_mul[3]);

    if (!haveMatrix)
        haveMatrix = dngDaylightXYZCam(raw.imgdata.color, raw.imgdata.idata.colors, info.xyzcam);

    if (!haveMatrix && errorMessage)
        *errorMessage = "camera metadata did not contain a usable XYZCAM or DNG daylight ColorMatrix";

    return haveMatrix;
}

bool detectRawLevelsAuto(const std::string &filename, RawLevelInfo &info, std::string *errorMessage) {
    return detectRawLevelsAuto(std::vector<std::string>{ filename }, info, errorMessage);
}

bool detectRawLevelsAuto(const std::vector<std::string> &filenames, RawLevelInfo &info, std::string *errorMessage) {
    if (filenames.empty()) {
        if (errorMessage)
            *errorMessage = "no RAW files were provided";
        return false;
    }

    LibRaw raw;
    int err = raw.open_file(filenames.front().c_str());
    if (err != LIBRAW_SUCCESS) {
        if (errorMessage)
            *errorMessage = libraw_strerror(err);
        return false;
    }

    err = raw.unpack();
    if (err != LIBRAW_SUCCESS) {
        if (errorMessage)
            *errorMessage = libraw_strerror(err);
        return false;
    }

    if (!prepareBlackNormalizedRaw(raw, info.blacklevel, info.absoluteWhitepoint, errorMessage))
        return false;
    ObservedRawScale observed = observeRawScale(raw, info.blacklevel, info.absoluteWhitepoint);
    raw.recycle();

    for (std::size_t i = 1; i < filenames.size(); ++i) {
        LibRaw nextRaw;
        err = nextRaw.open_file(filenames[i].c_str());
        if (err != LIBRAW_SUCCESS) {
            if (errorMessage)
                *errorMessage = libraw_strerror(err);
            return false;
        }
        err = nextRaw.unpack();
        if (err != LIBRAW_SUCCESS) {
            if (errorMessage)
                *errorMessage = libraw_strerror(err);
            return false;
        }
        int frameBlacklevel = -1;
        int frameAbsoluteWhitepoint = -1;
        if (!prepareBlackNormalizedRaw(nextRaw, frameBlacklevel, frameAbsoluteWhitepoint, errorMessage))
            return false;
        ObservedRawScale frameObserved = observeRawScale(nextRaw, info.blacklevel, info.absoluteWhitepoint);
        observed.maximum = std::max(observed.maximum, frameObserved.maximum);
        observed.samplesAboveMismatch += frameObserved.samplesAboveMismatch;
        observed.totalSamples += frameObserved.totalSamples;
    }

    info.absoluteWhitepoint = adjustRawWhitepointForObservedScale(
        info.blacklevel,
        info.absoluteWhitepoint,
        observed.maximum,
        observed.samplesAboveMismatch,
        observed.totalSamples);
    info.whitepoint = std::max(1, info.absoluteWhitepoint - info.blacklevel);
    return true;
}

int adjustRawWhitepointForObservedScale(int blacklevel, int decoderWhitepoint,
        int observedMaximum, std::size_t samplesAboveMismatch, std::size_t totalSamples) {
    const int range = decoderWhitepoint - blacklevel;
    const int observedRange = observedMaximum - blacklevel;
    if (range <= 0 || observedRange <= range * 2 || observedMaximum < 32768)
        return decoderWhitepoint;

    const std::size_t requiredSamples = std::max<std::size_t>(64, totalSamples / 1000000);
    if (samplesAboveMismatch < requiredSamples)
        return decoderWhitepoint;

    if (observedMaximum >= 0xFF00)
        return std::max(decoderWhitepoint, 0xFF00);
    return std::max(decoderWhitepoint, observedMaximum);
}
