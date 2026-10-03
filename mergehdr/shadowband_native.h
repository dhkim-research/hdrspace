#ifndef MERGEHDR_SHADOWBAND_NATIVE_H
#define MERGEHDR_SHADOWBAND_NATIVE_H

#include "hdrimage.h"

#include <array>
#include <string>
#include <vector>

struct ShadowbandMergeOptions {
    double roh = 0.0;
    double rov = 0.0;
    double sfov = 2.0;
    double srcsize = 6.7967e-05;
    double bw = 2.0;
    int margin = 20;
    bool align = true;
    bool fisheye = true;
    bool haveSunloc = false;
    int sunPixelX = 0;
    int sunPixelY = 0;
    bool haveSunDirection = false;
    std::array<double, 3> sunDirection{{0.0, 1.0, 0.0}};
    std::string envmapPath;
    std::string checkPrefix;
};

struct ShadowbandMergeResult {
    HdrImage blended;
    HdrImage skyOnly;
    bool hasSkyOnly = false;
    bool hasSource = false;
    std::array<double, 3> outputSourceDirection{{0.0, 0.0, 1.0}};
    std::array<double, 3> sourceRgb{{0.0, 0.0, 0.0}};
    double sourceSolidAngle = 0.0;
    double sourcePixelX = -1.0;
    double sourcePixelY = -1.0;
    std::vector<std::string> headerNotes;
};

ShadowbandMergeResult mergeShadowbandNative(
    const HdrImage &horizontal,
    const HdrImage &vertical,
    const HdrImage &noshadow,
    const ShadowbandMergeOptions &opts
);

#endif
