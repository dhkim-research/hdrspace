#pragma once

// Reading a Create View environment (fisheye HDR or .rad sky) and writing it for the renderer.

#include "Types.h"

struct HdrImage;

namespace hdrspace::createview {

std::vector<std::string> createViewHeaderValues(const HdrImage& image, const std::string& key);

std::string createViewHeaderValue(const HdrImage& image, const std::string& key);

double parseCreateViewInlineRotateZDegrees(const std::string& text);

std::vector<CreateViewInlinePrimitive> parseCreateViewInlinePrimitives(const std::string& text);

bool parseCreateViewEnvironmentSourceFromHeader(
    const std::string& text,
    std::array<double, 3>& direction,
    double& solidAngle,
    std::array<float, 3>* color = nullptr
);

std::string sanitizeCreateViewEnvironmentRadText(const std::string& text);

bool parseCreateViewEnvironmentSourcesFromRadText(
    const std::string& text,
    std::array<double, 3>& skyDirection,
    std::array<float, 3>& skyColor,
    std::array<double, 3>& sunDirection,
    double& sunSolidAngle,
    std::array<float, 3>& sunColor,
    bool& hasSky,
    bool& hasSun
);

std::vector<CreateViewEnvironmentInfo::Patch> buildCreateViewUniformEnvironmentPatches(
    const std::array<double, 3>& worldCenterDir,
    const std::array<double, 3>& worldUp,
    const std::array<float, 3>& color,
    float alpha = 0.90f
);

std::array<float, 3> sampleCreateViewHdrAngularFisheye(
    const HdrImage& image,
    const std::array<double, 3>& direction,
    const std::array<double, 3>& viewDir,
    const std::array<double, 3>& up,
    double imageZRotationDegrees = 0.0
);

std::vector<CreateViewEnvironmentInfo::Patch> buildCreateViewEnvironmentPatches(
    const HdrImage& image,
    const std::array<double, 3>& worldCenterDir,
    const std::array<double, 3>& worldUp,
    const std::array<double, 3>& imageViewDir,
    const std::array<double, 3>& imageUp,
    double imageZRotationDegrees
);

CreateViewEnvironmentInfo parseCreateViewEnvironment(const std::string& spec, const fs::path& workingDir);

std::string buildCreateViewEnvironmentRadText(const CreateViewEnvironmentInfo& environmentInfo, const fs::path& fisheyeCalPath);

} // namespace hdrspace::createview
