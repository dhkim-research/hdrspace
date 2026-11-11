#pragma once

// Vector, transform, projection and solid-angle helpers of Create View, and the Window FoV and solid angle.
// Conventions: screen coordinates run 0..1 with y = 0 at the bottom of the view (the row order of
// HdrImage as readRadianceHDR stores it).

#include "Types.h"

struct HdrImage;

namespace hdrspace::createview {

std::string formatCreateViewVec3(const std::array<double, 3>& v, int precision = 2);

std::array<double, 3> createViewVec3Add(const std::array<double, 3>& a, const std::array<double, 3>& b);

std::array<double, 3> createViewVec3Scale(const std::array<double, 3>& v, double s);

std::array<double, 3> createViewVec3Sub(const std::array<double, 3>& a, const std::array<double, 3>& b);

double createViewVec3Dot(const std::array<double, 3>& a, const std::array<double, 3>& b);

std::array<double, 3> createViewVec3Cross(const std::array<double, 3>& a, const std::array<double, 3>& b);

double createViewVec3Length(const std::array<double, 3>& v);

std::array<double, 3> createViewVec3Normalize(const std::array<double, 3>& v, const std::array<double, 3>& fallback = {0.0, 0.0, 1.0});

bool createViewIsPerspectiveProjection(const CreateViewViewInfo& viewInfo);

std::array<double, 3> createViewPointToCameraSpace(
    const std::array<double, 3>& point,
    const CreateViewViewInfo& viewInfo,
    const std::array<double, 3>& forward,
    const std::array<double, 3>& right,
    const std::array<double, 3>& trueUp
);

std::vector<std::array<double, 3>> createViewClipPolygonAgainstPlane(
    const std::vector<std::array<double, 3>>& polygon,
    const std::function<double(const std::array<double, 3>&)>& signedDistance
);

std::vector<std::array<double, 3>> createViewClipPerspectivePolygonToFrustum(
    const std::vector<std::array<double, 3>>& polygon,
    const CreateViewViewInfo& viewInfo
);

std::array<double, 2> createViewProjectPerspectiveCameraPoint(
    const std::array<double, 3>& cameraPoint,
    const CreateViewViewInfo& viewInfo
);

std::array<double, 3> createViewPerspectiveRayDirection(
    const CreateViewViewInfo& viewInfo,
    const std::array<double, 3>& forward,
    const std::array<double, 3>& right,
    const std::array<double, 3>& trueUp,
    double screenX,
    double screenY
);

double createViewTriangleSolidAngle(
    const std::array<double, 3>& a,
    const std::array<double, 3>& b,
    const std::array<double, 3>& c
);

double createViewSphericalQuadSolidAngle(
    const std::array<double, 3>& p00,
    const std::array<double, 3>& p10,
    const std::array<double, 3>& p11,
    const std::array<double, 3>& p01
);

bool createViewPointInPolygon2D(const std::array<double, 2>& point, const std::vector<std::array<double, 2>>& polygon);

std::array<double, 3> createViewVec3RotateZ(const std::array<double, 3>& v, double degrees);

void growCreateViewBounds(CreateViewRoomInfo& info, double x, double y, double z);

CreateViewTransform createViewTransformMultiply(const CreateViewTransform& a, const CreateViewTransform& b);

CreateViewTransform createViewTranslate(double tx, double ty, double tz);

CreateViewTransform createViewScale(double sx, double sy, double sz);

CreateViewTransform createViewRotateX(double degrees);

CreateViewTransform createViewRotateY(double degrees);

CreateViewTransform createViewRotateZ(double degrees);

std::array<double, 3> createViewApply(const CreateViewTransform& t, double x, double y, double z);

void growCreateViewBoundsTransformed(CreateViewRoomInfo& info, const CreateViewTransform& t, double x, double y, double z);

CreateViewWindowFovInfo computeCreateViewWindowFov(const CreateViewRoomInfo& roomInfo, const CreateViewViewInfo& viewInfo);

bool createViewMaterialTypeIsTransparent(const std::string& materialType);

bool createViewContainsAnyToken(const std::string& lowerText, std::initializer_list<std::string_view> tokens);

// Name tests on lower-case "identifier modifier type" text. Glazing: window, glass, glaz..., skylight,
// pane(s) but not panel. Frame: frame, mullion, transom, jamb, sill, header. Shade: blinds, shades,
// screens and the like.
bool createViewNameLooksGlazing(const std::string& lowerText);
bool createViewNameLooksFrame(const std::string& lowerText);
bool createViewNameLooksShade(const std::string& lowerText);

bool createViewSurfaceCountsAsMaskWindow(const CreateViewPreviewState::Surface& surface, const CreateViewRoomInfo& roomInfo);

CreateViewWindowMaskFovInfo computeCreateViewWindowMaskFov(
    const HdrImage& image,
    const CreateViewViewInfo& viewInfo,
    size_t requestedWidth,
    size_t requestedHeight,
    size_t contributingSurfaceCount,
    double threshold
);

std::array<double, 3> createViewDirectionFromAzimuthElevation(
    const std::array<double, 3>& centerDir,
    const std::array<double, 3>& upDir,
    double azimuth,
    double elevation
);

} // namespace hdrspace::createview
