#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <Eigen/Core>

struct CameraDetectInfo {
    Eigen::Matrix3f xyzcam = Eigen::Matrix3f::Identity();
    std::array<float, 4> rgbgMultipliers = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<float, 4> d65Multipliers = { 1.0f, 1.0f, 1.0f, 1.0f };
};

bool detectXYZCamAuto(const std::string &filename, CameraDetectInfo &info, std::string *errorMessage = nullptr);

struct RawLevelInfo {
    int blacklevel = -1;
    int whitepoint = -1;
    int absoluteWhitepoint = -1;
};

bool detectRawLevelsAuto(const std::string &filename, RawLevelInfo &info, std::string *errorMessage = nullptr);
bool detectRawLevelsAuto(const std::vector<std::string> &filenames, RawLevelInfo &info,
                         std::string *errorMessage = nullptr);

// LibRaw stores a base black level in black, DNG BlackLevelRepeatDim values
// in cblack[6..], and per-channel corrections in cblack[0..3]. MergeHDR
// reports one scalar baseline after LibRaw has applied the full table.
int effectiveRawBlackLevel(unsigned black, const unsigned *cblack, std::size_t cblackCount);

int adjustRawWhitepointForObservedScale(int blacklevel, int decoderWhitepoint,
                                        int observedMaximum, std::size_t samplesAboveMismatch,
                                        std::size_t totalSamples);
