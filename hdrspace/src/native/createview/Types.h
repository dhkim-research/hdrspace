#pragma once

// Data types of the Create View tool: the parsed room, view and environment, material edits and
// the preview geometry. Plain data only; the functions that fill them are in the other files here.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace hdrspace::createview {

struct CreateViewPreviewState {
    struct PointSample {
        std::array<double, 3> position{0.0, 0.0, 0.0};
        std::array<float, 3> color{0.72f, 0.76f, 0.82f};
        float alpha = 0.8f;
    };
    bool hasRoomBounds = false;
    std::array<double, 3> roomMin{0.0, 0.0, 0.0};
    std::array<double, 3> roomMax{1.0, 1.0, 1.0};
    bool hasView = false;
    std::array<double, 3> cameraPos{0.0, 0.0, 0.0};
    std::array<double, 3> cameraDir{0.0, -1.0, 0.0};
    std::array<double, 3> up{0.0, 0.0, 1.0};
    double vh = 0.0;
    double vv = 0.0;
    bool hasEnvironment = false;
    std::array<double, 3> environmentDir{0.0, -1.0, 0.0};
    std::array<double, 3> environmentUp{0.0, 0.0, 1.0};
    bool hasSkyDirection = false;
    std::array<double, 3> skyDirection{0.0, -1.0, 0.0};
    bool hasSun = false;
    std::array<double, 3> sunDirection{0.0, -1.0, 0.0};
    std::array<float, 3> sunColor{1.0f, 0.92f, 0.72f};
    double sunSolidAngle = 0.0;
    std::string projectionLabel;
    std::vector<PointSample> pointCloud;
    struct Surface {
        std::string identifier;
        std::string modifier;
        std::string type;
        std::string semanticLabel;
        std::vector<std::array<double, 3>> vertices;
        std::array<float, 3> color{0.72f, 0.76f, 0.82f};
        float alpha = 0.88f;
        bool transparent = false;
    };
    std::vector<Surface> environmentSurfaces;
    std::vector<Surface> surfaces;
    std::map<std::string, size_t> semanticCounts;
};

struct CreateViewMaterialInfo {
    std::string name;
    std::string modifier;
    std::string type;
    std::vector<std::string> sargs1;
    std::vector<std::string> sargs2;
    std::vector<std::string> sargs3;
    std::vector<double> numericArgs;
    fs::path sourcePath;
    bool synthesized = false;
};

struct CreateViewMaterialOverride {
    std::string modifier;
    std::string radianceType;
    std::array<double, 5> numericArgs{0.7, 0.7, 0.7, 0.0, 0.15};
    std::array<float, 3> previewColor{0.7f, 0.7f, 0.7f};
    float previewAlpha = 0.85f;
    bool transparent = false;
    std::string sourceLabel;
};

struct CreateViewRoomInfo {
    bool valid = false;
    bool hasBounds = false;
    std::string sourceLabel;
    std::array<double, 3> min{0.0, 0.0, 0.0};
    std::array<double, 3> max{0.0, 0.0, 0.0};
    size_t primitiveCount = 0;
    size_t polygonCount = 0;
    size_t commandCount = 0;
    size_t includeCount = 0;
    size_t xformCount = 0;
    std::map<std::string, CreateViewMaterialInfo> materials;
    std::map<std::string, fs::path> modifierSourcePaths;
    std::set<fs::path> parsedPaths;      // every file read while parsing (canonical)
    std::set<fs::path> roomLoadedPaths;  // the room file and what it loads itself (!include / !xform)
    std::vector<CreateViewPreviewState::Surface> surfaces;
    std::vector<CreateViewPreviewState::PointSample> pointCloud;
    std::map<std::string, size_t> semanticCounts;
    std::string note;
};

struct CreateViewViewInfo {
    bool valid = false;
    std::string sourceLabel;
    std::string projectionToken;
    std::string projectionLabel;
    std::array<double, 3> position{0.0, 0.0, 0.0};
    std::array<double, 3> direction{0.0, -1.0, 0.0};
    std::array<double, 3> up{0.0, 0.0, 1.0};
    double vh = 0.0;
    double vv = 0.0;
    double vo = 0.0;
    double va = 0.0;
    double vs = 0.0;
    double vl = 0.0;
    std::string note;
};

struct CreateViewWindowFovInfo {
    bool valid = false;
    size_t totalSurfaceCount = 0;
    size_t contributingSurfaceCount = 0;
    size_t contributingVertexCount = 0;
    double leftDegrees = 0.0;
    double rightDegrees = 0.0;
    double upDegrees = 0.0;
    double downDegrees = 0.0;
    double widthDegrees = 0.0;
    double heightDegrees = 0.0;
    double solidAngleSteradians = 0.0;
    double solidAnglePercentOfView = 0.0;
    std::string note;
};

struct CreateViewWindowMaskFovInfo {
    bool valid = false;
    size_t requestedWidth = 0;
    size_t requestedHeight = 0;
    size_t actualWidth = 0;
    size_t actualHeight = 0;
    size_t contributingSurfaceCount = 0;
    size_t activePixelCount = 0;
    double threshold = 0.1;
    double leftDegrees = 0.0;
    double rightDegrees = 0.0;
    double upDegrees = 0.0;
    double downDegrees = 0.0;
    double widthDegrees = 0.0;
    double heightDegrees = 0.0;
    double solidAngleSteradians = 0.0;
    double solidAnglePercentOfView = 0.0;
    fs::path debugMaskPath;
    std::string note;
};

struct CreateViewEnvironmentInfo {
    struct Patch {
        std::array<std::array<double, 3>, 4> directions{};
        std::array<float, 3> color{0.6f, 0.7f, 0.85f};
        float alpha = 0.78f;
    };

    bool valid = false;
    bool hasImage = false;
    bool hasView = false;
    bool hasSkySource = false;
    bool hasSun = false;
    bool hasDirectSun = false;
    fs::path sourcePath;
    std::string sourceLabel;
    std::string projectionLabel;
    std::array<double, 3> direction{0.0, -1.0, 0.0};
    std::array<double, 3> up{0.0, 0.0, 1.0};
    std::array<double, 3> skyDirection{0.0, -1.0, 0.0};
    std::array<double, 3> sunDirection{0.0, -1.0, 0.0};
    std::array<float, 3> sunColor{1.0f, 0.92f, 0.72f};
    double sunSolidAngle = 0.0;
    double vh = 180.0;
    double vv = 180.0;
    size_t width = 0;
    size_t height = 0;
    std::string orientation;
    std::string skyDescription;
    double imageZRotationDegrees = 0.0;
    std::string rawSkySource;
    std::string rawSolarSource;
    std::vector<std::string> headerLines;
    std::vector<Patch> patches;
    std::string note;
};

struct CreateViewInlinePrimitive {
    std::string modifier;
    std::string type;
    std::string identifier;
    std::vector<double> numericArgs;
};

struct CreateViewTransform {
    std::array<double, 16> m{
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
};

} // namespace hdrspace::createview
