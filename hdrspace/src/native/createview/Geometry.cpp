#include "Geometry.h"

#include "Environment.h"
#include "Geometry.h"
#include "SceneFile.h"
#include "ViewFile.h"
#include "../core/TextUtil.h"
#include "hdrimage.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace hdrspace::createview {

using namespace hdrspace::text;

std::string formatCreateViewVec3(const std::array<double, 3>& v, int precision) {
    return std::format("{:.{}f}, {:.{}f}, {:.{}f}", v[0], precision, v[1], precision, v[2], precision);
}

std::array<double, 3> createViewVec3Add(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

std::array<double, 3> createViewVec3Scale(const std::array<double, 3>& v, double s) {
    return {v[0] * s, v[1] * s, v[2] * s};
}

std::array<double, 3> createViewVec3Sub(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

double createViewVec3Dot(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<double, 3> createViewVec3Cross(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return {
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    };
}

double createViewVec3Length(const std::array<double, 3>& v) {
    return std::sqrt(createViewVec3Dot(v, v));
}

std::array<double, 3> createViewVec3Normalize(const std::array<double, 3>& v, const std::array<double, 3>& fallback) {
    const double len = createViewVec3Length(v);
    if (len < 1e-9) {
        return fallback;
    }
    return {v[0] / len, v[1] / len, v[2] / len};
}

bool createViewIsPerspectiveProjection(const CreateViewViewInfo& viewInfo) {
    return viewInfo.projectionToken == "-vtv" || viewInfo.projectionToken.empty();
}

std::array<double, 3> createViewPointToCameraSpace(
    const std::array<double, 3>& point,
    const CreateViewViewInfo& viewInfo,
    const std::array<double, 3>& forward,
    const std::array<double, 3>& right,
    const std::array<double, 3>& trueUp
) {
    const auto rel = createViewVec3Sub(point, viewInfo.position);
    return {
        createViewVec3Dot(rel, right),
        createViewVec3Dot(rel, trueUp),
        createViewVec3Dot(rel, forward),
    };
}

std::vector<std::array<double, 3>> createViewClipPolygonAgainstPlane(
    const std::vector<std::array<double, 3>>& polygon,
    const std::function<double(const std::array<double, 3>&)>& signedDistance
) {
    constexpr double kClipEpsilon = 1e-9;

    std::vector<std::array<double, 3>> out;
    if (polygon.empty()) {
        return out;
    }

    out.reserve(polygon.size() + 4);
    std::array<double, 3> previous = polygon.back();
    double previousDistance = signedDistance(previous);
    bool previousInside = previousDistance >= -kClipEpsilon;

    for (const auto& current : polygon) {
        const double currentDistance = signedDistance(current);
        const bool currentInside = currentDistance >= -kClipEpsilon;

        if (previousInside != currentInside) {
            const double denominator = previousDistance - currentDistance;
            if (std::abs(denominator) > 1e-12) {
                const double t = std::clamp(previousDistance / denominator, 0.0, 1.0);
                out.push_back({
                    previous[0] + (current[0] - previous[0]) * t,
                    previous[1] + (current[1] - previous[1]) * t,
                    previous[2] + (current[2] - previous[2]) * t,
                });
            }
        }
        if (currentInside) {
            out.push_back(current);
        }

        previous = current;
        previousDistance = currentDistance;
        previousInside = currentInside;
    }

    return out;
}

std::vector<std::array<double, 3>> createViewClipPerspectivePolygonToFrustum(
    const std::vector<std::array<double, 3>>& polygon,
    const CreateViewViewInfo& viewInfo
) {
    constexpr double kNearEpsilon = 1e-6;

    if (polygon.size() < 3 || viewInfo.vh <= 0.0 || viewInfo.vv <= 0.0) {
        return {};
    }

    const double horizontalScale = 2.0 * std::tan(viewInfo.vh * M_PI / 360.0);
    const double verticalScale = 2.0 * std::tan(viewInfo.vv * M_PI / 360.0);
    const double leftSlope = (viewInfo.vs - 0.5) * horizontalScale;
    const double rightSlope = (viewInfo.vs + 0.5) * horizontalScale;
    const double bottomSlope = (viewInfo.vl - 0.5) * verticalScale;
    const double topSlope = (viewInfo.vl + 0.5) * verticalScale;

    auto clipped = polygon;
    clipped = createViewClipPolygonAgainstPlane(clipped, [&](const auto& p) { return p[2] - kNearEpsilon; });
    clipped = createViewClipPolygonAgainstPlane(clipped, [&](const auto& p) { return p[0] - leftSlope * p[2]; });
    clipped = createViewClipPolygonAgainstPlane(clipped, [&](const auto& p) { return rightSlope * p[2] - p[0]; });
    clipped = createViewClipPolygonAgainstPlane(clipped, [&](const auto& p) { return p[1] - bottomSlope * p[2]; });
    clipped = createViewClipPolygonAgainstPlane(clipped, [&](const auto& p) { return topSlope * p[2] - p[1]; });
    return clipped;
}

std::array<double, 2> createViewProjectPerspectiveCameraPoint(
    const std::array<double, 3>& cameraPoint,
    const CreateViewViewInfo& viewInfo
) {
    const double horizontalScale = 2.0 * std::tan(viewInfo.vh * M_PI / 360.0);
    const double verticalScale = 2.0 * std::tan(viewInfo.vv * M_PI / 360.0);
    return {
        cameraPoint[0] / (cameraPoint[2] * horizontalScale) + 0.5 - viewInfo.vs,
        cameraPoint[1] / (cameraPoint[2] * verticalScale) + 0.5 - viewInfo.vl,
    };
}

std::array<double, 3> createViewPerspectiveRayDirection(
    const CreateViewViewInfo& viewInfo,
    const std::array<double, 3>& forward,
    const std::array<double, 3>& right,
    const std::array<double, 3>& trueUp,
    double screenX,
    double screenY
) {
    const double horizontalScale = 2.0 * std::tan(viewInfo.vh * M_PI / 360.0);
    const double verticalScale = 2.0 * std::tan(viewInfo.vv * M_PI / 360.0);
    const double x = screenX + viewInfo.vs - 0.5;
    const double y = screenY + viewInfo.vl - 0.5;
    return createViewVec3Normalize(createViewVec3Add(
        createViewVec3Add(forward, createViewVec3Scale(right, x * horizontalScale)),
        createViewVec3Scale(trueUp, y * verticalScale)
    ), forward);
}

double createViewTriangleSolidAngle(
    const std::array<double, 3>& a,
    const std::array<double, 3>& b,
    const std::array<double, 3>& c
) {
    const auto crossBC = createViewVec3Cross(b, c);
    const double numerator = std::abs(createViewVec3Dot(a, crossBC));
    const double denominator = 1.0 + createViewVec3Dot(a, b) + createViewVec3Dot(b, c) + createViewVec3Dot(c, a);
    return 2.0 * std::atan2(numerator, denominator);
}

double createViewSphericalQuadSolidAngle(
    const std::array<double, 3>& p00,
    const std::array<double, 3>& p10,
    const std::array<double, 3>& p11,
    const std::array<double, 3>& p01
) {
    return createViewTriangleSolidAngle(p00, p10, p11) +
           createViewTriangleSolidAngle(p00, p11, p01);
}

bool createViewPointInPolygon2D(const std::array<double, 2>& point, const std::vector<std::array<double, 2>>& polygon) {
    if (polygon.size() < 3) {
        return false;
    }

    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const auto& a = polygon[j];
        const auto& b = polygon[i];
        const bool crosses = ((a[1] > point[1]) != (b[1] > point[1])) &&
            (point[0] <= (b[0] - a[0]) * (point[1] - a[1]) / (b[1] - a[1]) + a[0]);
        if (crosses) {
            inside = !inside;
        }
    }
    return inside;
}

std::array<double, 3> createViewVec3RotateZ(const std::array<double, 3>& v, double degrees) {
    const double radians = degrees * M_PI / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    return {
        c * v[0] - s * v[1],
        s * v[0] + c * v[1],
        v[2]
    };
}

void growCreateViewBounds(CreateViewRoomInfo& info, double x, double y, double z) {
    if (!info.hasBounds) {
        info.min = {x, y, z};
        info.max = {x, y, z};
        info.hasBounds = true;
        return;
    }
    info.min[0] = std::min(info.min[0], x);
    info.min[1] = std::min(info.min[1], y);
    info.min[2] = std::min(info.min[2], z);
    info.max[0] = std::max(info.max[0], x);
    info.max[1] = std::max(info.max[1], y);
    info.max[2] = std::max(info.max[2], z);
}

CreateViewTransform createViewTransformMultiply(const CreateViewTransform& a, const CreateViewTransform& b) {
    CreateViewTransform out;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[static_cast<size_t>(row * 4 + k)] * b.m[static_cast<size_t>(k * 4 + col)];
            }
            out.m[static_cast<size_t>(row * 4 + col)] = sum;
        }
    }
    return out;
}

CreateViewTransform createViewTranslate(double tx, double ty, double tz) {
    CreateViewTransform out;
    out.m[3] = tx;
    out.m[7] = ty;
    out.m[11] = tz;
    return out;
}

CreateViewTransform createViewScale(double sx, double sy, double sz) {
    CreateViewTransform out;
    out.m[0] = sx;
    out.m[5] = sy;
    out.m[10] = sz;
    return out;
}

CreateViewTransform createViewRotateX(double degrees) {
    const double r = degrees * M_PI / 180.0;
    const double c = std::cos(r);
    const double s = std::sin(r);
    CreateViewTransform out;
    out.m[5] = c;
    out.m[6] = -s;
    out.m[9] = s;
    out.m[10] = c;
    return out;
}

CreateViewTransform createViewRotateY(double degrees) {
    const double r = degrees * M_PI / 180.0;
    const double c = std::cos(r);
    const double s = std::sin(r);
    CreateViewTransform out;
    out.m[0] = c;
    out.m[2] = s;
    out.m[8] = -s;
    out.m[10] = c;
    return out;
}

CreateViewTransform createViewRotateZ(double degrees) {
    const double r = degrees * M_PI / 180.0;
    const double c = std::cos(r);
    const double s = std::sin(r);
    CreateViewTransform out;
    out.m[0] = c;
    out.m[1] = -s;
    out.m[4] = s;
    out.m[5] = c;
    return out;
}

std::array<double, 3> createViewApply(const CreateViewTransform& t, double x, double y, double z) {
    return {
        t.m[0] * x + t.m[1] * y + t.m[2] * z + t.m[3],
        t.m[4] * x + t.m[5] * y + t.m[6] * z + t.m[7],
        t.m[8] * x + t.m[9] * y + t.m[10] * z + t.m[11],
    };
}

void growCreateViewBoundsTransformed(CreateViewRoomInfo& info, const CreateViewTransform& t, double x, double y, double z) {
    const auto p = createViewApply(t, x, y, z);
    growCreateViewBounds(info, p[0], p[1], p[2]);
}

CreateViewWindowFovInfo computeCreateViewWindowFov(const CreateViewRoomInfo& roomInfo, const CreateViewViewInfo& viewInfo) {
    CreateViewWindowFovInfo info;
    if (!viewInfo.valid) {
        info.note = "Load a valid camera view to measure window angles.";
        return info;
    }

    if (!createViewIsPerspectiveProjection(viewInfo)) {
        info.note = "Window FoV and solid angle are currently clipped exactly for perspective views only.";
        return info;
    }

    if (viewInfo.vh <= 0.0 || viewInfo.vv <= 0.0 || viewInfo.vh >= 180.0 || viewInfo.vv >= 180.0) {
        info.note = "The camera FoV is not valid for a perspective window-angle estimate.";
        return info;
    }

    const auto forward = createViewVec3Normalize(viewInfo.direction, {0.0, -1.0, 0.0});
    auto right = createViewVec3Cross(forward, viewInfo.up);
    right = createViewVec3Normalize(right, {1.0, 0.0, 0.0});
    const auto trueUp = createViewVec3Normalize(createViewVec3Cross(right, forward), {0.0, 0.0, 1.0});

    bool haveAngles = false;
    double minHorizontal = 0.0;
    double maxHorizontal = 0.0;
    double minVertical = 0.0;
    double maxVertical = 0.0;

    struct ClippedWindowPolygon {
        std::vector<std::array<double, 3>> cameraVertices;
        std::vector<std::array<double, 2>> screenVertices;
    };
    std::vector<ClippedWindowPolygon> clippedPolygons;

    for (const auto& surface : roomInfo.surfaces) {
        if (surface.semanticLabel != "Windows" || surface.vertices.size() < 3) {
            continue;
        }
        {
            // Frames and shades are left out, as in the window-mask render.
            std::string materialType;
            if (const auto materialIt = roomInfo.materials.find(surface.modifier); materialIt != roomInfo.materials.end()) {
                materialType = materialIt->second.type;
            }
            const std::string lowerText = createViewLower(surface.identifier + " " + surface.modifier + " " + materialType);
            if (createViewNameLooksFrame(lowerText) || createViewNameLooksShade(lowerText)) {
                continue;
            }
        }
        ++info.totalSurfaceCount;
        std::vector<std::array<double, 3>> cameraPolygon;
        cameraPolygon.reserve(surface.vertices.size());
        for (const auto& vertex : surface.vertices) {
            cameraPolygon.push_back(createViewPointToCameraSpace(vertex, viewInfo, forward, right, trueUp));
        }

        auto clipped = createViewClipPerspectivePolygonToFrustum(cameraPolygon, viewInfo);
        if (clipped.size() < 3) {
            continue;
        }

        ClippedWindowPolygon projected;
        projected.cameraVertices = clipped;
        projected.screenVertices.reserve(clipped.size());

        for (const auto& cameraPoint : clipped) {
            const double horizontal = std::atan2(cameraPoint[0], cameraPoint[2]) * 180.0 / M_PI;
            const double vertical = std::atan2(cameraPoint[1], cameraPoint[2]) * 180.0 / M_PI;
            if (!haveAngles) {
                minHorizontal = maxHorizontal = horizontal;
                minVertical = maxVertical = vertical;
                haveAngles = true;
            } else {
                minHorizontal = std::min(minHorizontal, horizontal);
                maxHorizontal = std::max(maxHorizontal, horizontal);
                minVertical = std::min(minVertical, vertical);
                maxVertical = std::max(maxVertical, vertical);
            }
            ++info.contributingVertexCount;
            projected.screenVertices.push_back(createViewProjectPerspectiveCameraPoint(cameraPoint, viewInfo));
        }

        ++info.contributingSurfaceCount;
        clippedPolygons.push_back(std::move(projected));
    }

    if (!info.totalSurfaceCount) {
        info.note = "No window surfaces were found in the room geometry.";
        return info;
    }
    if (!haveAngles) {
        info.note = "Window surfaces exist, but none of their vertices are in front of the camera.";
        return info;
    }

    info.valid = true;
    info.leftDegrees = std::max(0.0, -minHorizontal);
    info.rightDegrees = std::max(0.0, maxHorizontal);
    info.downDegrees = std::max(0.0, -minVertical);
    info.upDegrees = std::max(0.0, maxVertical);
    info.widthDegrees = maxHorizontal - minHorizontal;
    info.heightDegrees = maxVertical - minVertical;

    constexpr int kSolidAngleGridResolution = 256;
    std::vector<uint8_t> covered(static_cast<size_t>(kSolidAngleGridResolution * kSolidAngleGridResolution), 0);
    for (const auto& polygon : clippedPolygons) {
        double minX = 1.0;
        double minY = 1.0;
        double maxX = 0.0;
        double maxY = 0.0;
        for (const auto& uv : polygon.screenVertices) {
            minX = std::min(minX, uv[0]);
            minY = std::min(minY, uv[1]);
            maxX = std::max(maxX, uv[0]);
            maxY = std::max(maxY, uv[1]);
        }

        const int x0 = std::clamp(static_cast<int>(std::floor(minX * kSolidAngleGridResolution)), 0, kSolidAngleGridResolution - 1);
        const int x1 = std::clamp(static_cast<int>(std::ceil(maxX * kSolidAngleGridResolution)) - 1, 0, kSolidAngleGridResolution - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(minY * kSolidAngleGridResolution)), 0, kSolidAngleGridResolution - 1);
        const int y1 = std::clamp(static_cast<int>(std::ceil(maxY * kSolidAngleGridResolution)) - 1, 0, kSolidAngleGridResolution - 1);
        if (x1 < x0 || y1 < y0) {
            continue;
        }

        for (int py = y0; py <= y1; ++py) {
            const double sampleY = (static_cast<double>(py) + 0.5) / static_cast<double>(kSolidAngleGridResolution);
            for (int px = x0; px <= x1; ++px) {
                const double sampleX = (static_cast<double>(px) + 0.5) / static_cast<double>(kSolidAngleGridResolution);
                if (createViewPointInPolygon2D({sampleX, sampleY}, polygon.screenVertices)) {
                    covered[static_cast<size_t>(py * kSolidAngleGridResolution + px)] = 1;
                }
            }
        }
    }

    double solidAngle = 0.0;
    for (int py = 0; py < kSolidAngleGridResolution; ++py) {
        const double y0 = static_cast<double>(py) / static_cast<double>(kSolidAngleGridResolution);
        const double y1 = static_cast<double>(py + 1) / static_cast<double>(kSolidAngleGridResolution);
        for (int px = 0; px < kSolidAngleGridResolution; ++px) {
            if (!covered[static_cast<size_t>(py * kSolidAngleGridResolution + px)]) {
                continue;
            }
            const double x0 = static_cast<double>(px) / static_cast<double>(kSolidAngleGridResolution);
            const double x1 = static_cast<double>(px + 1) / static_cast<double>(kSolidAngleGridResolution);
            const auto d00 = createViewPerspectiveRayDirection(viewInfo, forward, right, trueUp, x0, y0);
            const auto d10 = createViewPerspectiveRayDirection(viewInfo, forward, right, trueUp, x1, y0);
            const auto d11 = createViewPerspectiveRayDirection(viewInfo, forward, right, trueUp, x1, y1);
            const auto d01 = createViewPerspectiveRayDirection(viewInfo, forward, right, trueUp, x0, y1);
            solidAngle += createViewSphericalQuadSolidAngle(d00, d10, d11, d01);
        }
    }

    const auto f00 = createViewPerspectiveRayDirection(viewInfo, forward, right, trueUp, 0.0, 0.0);
    const auto f10 = createViewPerspectiveRayDirection(viewInfo, forward, right, trueUp, 1.0, 0.0);
    const auto f11 = createViewPerspectiveRayDirection(viewInfo, forward, right, trueUp, 1.0, 1.0);
    const auto f01 = createViewPerspectiveRayDirection(viewInfo, forward, right, trueUp, 0.0, 1.0);
    const double fullViewSolidAngle = createViewSphericalQuadSolidAngle(f00, f10, f11, f01);
    info.solidAngleSteradians = solidAngle;
    info.solidAnglePercentOfView = fullViewSolidAngle > 1e-9 ? (solidAngle / fullViewSolidAngle * 100.0) : 0.0;
    info.note = std::format(
        "{} window face(s), {} clipped vertices, {:.3f} sr ({:.1f}% of view)",
        info.contributingSurfaceCount,
        info.contributingVertexCount,
        info.solidAngleSteradians,
        info.solidAnglePercentOfView
    );
    return info;
}

bool createViewMaterialTypeIsTransparent(const std::string& materialType) {
    const std::string lower = createViewLower(materialType);
    return lower == "glass" || lower == "dielectric" || lower == "trans" ||
           lower == "trans2" || lower == "bsdf" || lower == "mixfunc";
}

bool createViewContainsAnyToken(const std::string& lowerText, std::initializer_list<std::string_view> tokens) {
    for (const auto token : tokens) {
        if (lowerText.find(token) != std::string::npos) {
            return true;
        }
    }
    return false;
}

namespace {

// "pane" or "panes", but not "panel" (wall_panel and ceiling_panel were taken for glazing)
bool createViewNameHasPane(const std::string& lowerText) {
    for (size_t pos = lowerText.find("pane"); pos != std::string::npos; pos = lowerText.find("pane", pos + 1)) {
        const size_t after = pos + 4;
        if (after >= lowerText.size() || lowerText[after] != 'l') {
            return true;
        }
    }
    return false;
}

} // namespace

bool createViewNameLooksGlazing(const std::string& lowerText) {
    return createViewContainsAnyToken(lowerText, {"window", "glass", "glaz", "skylight"}) || createViewNameHasPane(lowerText);
}

bool createViewNameLooksFrame(const std::string& lowerText) {
    return createViewContainsAnyToken(lowerText, {"frame", "mullion", "transom", "jamb", "sill", "header"});
}

bool createViewNameLooksShade(const std::string& lowerText) {
    return createViewContainsAnyToken(lowerText, {
        "blind", "shade", "roller", "fabric", "screen", "slat", "louver", "curtain", "drape"
    });
}

bool createViewSurfaceCountsAsMaskWindow(const CreateViewPreviewState::Surface& surface, const CreateViewRoomInfo& roomInfo) {
    if (createViewLower(surface.type) != "polygon" || surface.vertices.size() < 3) {
        return false;
    }

    std::string materialType;
    auto materialIt = roomInfo.materials.find(surface.modifier);
    if (materialIt != roomInfo.materials.end()) {
        materialType = materialIt->second.type;
    }

    const std::string lowerText = createViewLower(surface.identifier + " " + surface.modifier + " " + materialType);
    const bool transparentType = createViewMaterialTypeIsTransparent(materialType);
    const bool looksWindow = createViewNameLooksGlazing(lowerText) || createViewContainsAnyToken(lowerText, {"vision"});
    const bool looksFrame = createViewNameLooksFrame(lowerText);
    const bool looksShade = createViewNameLooksShade(lowerText);

    if (looksFrame || looksShade) {
        return false;
    }
    if (transparentType && (surface.semanticLabel == "Windows" || looksWindow)) {
        return true;
    }
    return looksWindow && surface.semanticLabel == "Windows";
}

CreateViewWindowMaskFovInfo computeCreateViewWindowMaskFov(
    const HdrImage& image,
    const CreateViewViewInfo& viewInfo,
    size_t requestedWidth,
    size_t requestedHeight,
    size_t contributingSurfaceCount,
    double threshold
) {
    CreateViewWindowMaskFovInfo info;
    info.requestedWidth = requestedWidth;
    info.requestedHeight = requestedHeight;
    info.actualWidth = image.width;
    info.actualHeight = image.height;
    info.contributingSurfaceCount = contributingSurfaceCount;
    info.threshold = threshold;

    if (!viewInfo.valid) {
        info.note = "Load a valid camera view to refine window angles.";
        return info;
    }
    if (!createViewIsPerspectiveProjection(viewInfo)) {
        info.note = "Mask-based Window FoV currently supports perspective views only.";
        return info;
    }
    if (image.width == 0 || image.height == 0) {
        info.note = "Temporary window mask image is empty.";
        return info;
    }

    const auto forward = createViewVec3Normalize(viewInfo.direction, {0.0, -1.0, 0.0});
    auto right = createViewVec3Cross(forward, viewInfo.up);
    right = createViewVec3Normalize(right, {1.0, 0.0, 0.0});
    const auto trueUp = createViewVec3Normalize(createViewVec3Cross(right, forward), {0.0, 0.0, 1.0});

    const size_t cornerWidth = image.width + 1;
    const size_t cornerHeight = image.height + 1;
    std::vector<std::array<double, 3>> cornerRays(cornerWidth * cornerHeight);
    for (size_t y = 0; y < cornerHeight; ++y) {
        const double screenY = image.height > 0 ? static_cast<double>(y) / static_cast<double>(image.height) : 0.0;
        for (size_t x = 0; x < cornerWidth; ++x) {
            const double screenX = image.width > 0 ? static_cast<double>(x) / static_cast<double>(image.width) : 0.0;
            cornerRays[y * cornerWidth + x] = createViewPerspectiveRayDirection(viewInfo, forward, right, trueUp, screenX, screenY);
        }
    }

    bool haveAngles = false;
    double minHorizontal = 0.0;
    double maxHorizontal = 0.0;
    double minVertical = 0.0;
    double maxVertical = 0.0;
    double solidAngle = 0.0;
    double fullViewSolidAngle = 0.0;

    auto updateAnglesFromRay = [&](const std::array<double, 3>& ray) {
        const double camX = createViewVec3Dot(ray, right);
        const double camY = createViewVec3Dot(ray, trueUp);
        const double camZ = createViewVec3Dot(ray, forward);
        const double horizontal = std::atan2(camX, camZ) * 180.0 / M_PI;
        const double vertical = std::atan2(camY, camZ) * 180.0 / M_PI;
        if (!haveAngles) {
            minHorizontal = maxHorizontal = horizontal;
            minVertical = maxVertical = vertical;
            haveAngles = true;
        } else {
            minHorizontal = std::min(minHorizontal, horizontal);
            maxHorizontal = std::max(maxHorizontal, horizontal);
            minVertical = std::min(minVertical, vertical);
            maxVertical = std::max(maxVertical, vertical);
        }
    };

    for (size_t y = 0; y < image.height; ++y) {
        for (size_t x = 0; x < image.width; ++x) {
            const auto& d00 = cornerRays[y * cornerWidth + x];
            const auto& d10 = cornerRays[y * cornerWidth + (x + 1)];
            const auto& d11 = cornerRays[(y + 1) * cornerWidth + (x + 1)];
            const auto& d01 = cornerRays[(y + 1) * cornerWidth + x];
            const double pixelSolidAngle = createViewSphericalQuadSolidAngle(d00, d10, d11, d01);
            fullViewSolidAngle += pixelSolidAngle;

            const size_t index = (y * image.width + x) * 3;
            const double maskValue = std::max({
                static_cast<double>(image.rgb[index + 0]),
                static_cast<double>(image.rgb[index + 1]),
                static_cast<double>(image.rgb[index + 2]),
            });
            if (maskValue <= threshold) {
                continue;
            }

            ++info.activePixelCount;
            solidAngle += pixelSolidAngle;
            updateAnglesFromRay(d00);
            updateAnglesFromRay(d10);
            updateAnglesFromRay(d11);
            updateAnglesFromRay(d01);
        }
    }

    if (!haveAngles || info.activePixelCount == 0) {
        info.note = "The temporary window mask did not contain any bright window pixels.";
        return info;
    }

    info.valid = true;
    info.leftDegrees = std::max(0.0, -minHorizontal);
    info.rightDegrees = std::max(0.0, maxHorizontal);
    info.downDegrees = std::max(0.0, -minVertical);
    info.upDegrees = std::max(0.0, maxVertical);
    info.widthDegrees = maxHorizontal - minHorizontal;
    info.heightDegrees = maxVertical - minVertical;
    info.solidAngleSteradians = solidAngle;
    info.solidAnglePercentOfView = fullViewSolidAngle > 1e-9 ? (solidAngle / fullViewSolidAngle * 100.0) : 0.0;
    info.note = std::format(
        "Mask-based result from a temporary {} × {} render (actual {} × {}), threshold {:.2f}, {} active pixel(s), {} white window polygon(s).",
        requestedWidth,
        requestedHeight,
        image.width,
        image.height,
        threshold,
        info.activePixelCount,
        contributingSurfaceCount
    );
    return info;
}

std::array<double, 3> createViewDirectionFromAzimuthElevation(
    const std::array<double, 3>& centerDir,
    const std::array<double, 3>& upDir,
    double azimuth,
    double elevation
) {
    const auto forward = createViewVec3Normalize(centerDir, {0.0, -1.0, 0.0});
    auto right = createViewVec3Normalize(createViewVec3Cross(forward, upDir), {1.0, 0.0, 0.0});
    auto screenUp = createViewVec3Normalize(createViewVec3Cross(right, forward), {0.0, 0.0, 1.0});
    const double sinTheta = std::sin(elevation);
    const double cosTheta = std::cos(elevation);
    auto direction = createViewVec3Add(
        createViewVec3Scale(forward, cosTheta),
        createViewVec3Add(
            createViewVec3Scale(right, std::cos(azimuth) * sinTheta),
            createViewVec3Scale(screenUp, std::sin(azimuth) * sinTheta)
        )
    );
    return createViewVec3Normalize(direction, forward);
}

} // namespace hdrspace::createview
