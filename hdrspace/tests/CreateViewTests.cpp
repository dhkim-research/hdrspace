// Tests for the Create View calculations. Expected values come from geometry, not from the code:
// a regression in the window solid angle, the ray convention or the scene files shows up here.

#include "TestHarness.h"

#include "native/createview/Environment.h"
#include "native/createview/Geometry.h"
#include "native/createview/SceneFile.h"
#include "native/createview/ViewFile.h"
#include "hdrimage.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

using namespace hdrspace::createview;

namespace {

constexpr double kPi = 3.14159265358979323846;

fs::path makeTemporaryFolder(const std::string& name) {
    const fs::path folder = fs::temp_directory_path() / ("hdrspace-tests-" + name);
    fs::remove_all(folder);
    fs::create_directories(folder);
    return folder;
}

void writeText(const fs::path& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
}

std::string readText(const fs::path& path) {
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

CreateViewViewInfo perspectiveView(std::array<double, 3> position, std::array<double, 3> direction, double vh, double vv) {
    CreateViewViewInfo view;
    view.valid = true;
    view.projectionToken = "-vtv";
    view.position = position;
    view.direction = direction;
    view.up = {0.0, 0.0, 1.0};
    view.vh = vh;
    view.vv = vv;
    return view;
}

// Solid angle of an axis-aligned rectangle in a plane at distance d, measured from the foot of the
// perpendicular: the sum of the four corner rectangles atan(a b / (d sqrt(d^2 + a^2 + b^2))).
double rectangleSolidAngle(double d, double x0, double x1, double z0, double z1) {
    auto corner = [d](double a, double b) {
        const double sign = (a < 0.0) != (b < 0.0) ? -1.0 : 1.0;
        a = std::fabs(a);
        b = std::fabs(b);
        return sign * std::atan(a * b / (d * std::sqrt(d * d + a * a + b * b)));
    };
    return corner(x1, z1) - corner(x0, z1) - corner(x1, z0) + corner(x0, z0);
}

} // namespace

TEST_CASE("point in polygon: a trapezoid has the right area in both vertex orders") {
    const std::vector<std::array<double, 2>> ccw{{0.2, 0.2}, {0.8, 0.2}, {0.7, 0.8}, {0.3, 0.8}};
    const std::vector<std::array<double, 2>> cw(ccw.rbegin(), ccw.rend());
    for (const auto* polygon : {&ccw, &cw}) {
        constexpr int kGrid = 256;
        int inside = 0;
        for (int y = 0; y < kGrid; ++y) {
            for (int x = 0; x < kGrid; ++x) {
                inside += createViewPointInPolygon2D({(x + 0.5) / kGrid, (y + 0.5) / kGrid}, *polygon) ? 1 : 0;
            }
        }
        CHECK_NEAR(inside / double(kGrid * kGrid), 0.30, 0.003);
    }
}

TEST_CASE("view rays: screen y = 0 is the bottom of the view") {
    const auto view = perspectiveView({0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, 60.0, 60.0);
    const std::array<double, 3> forward{0.0, 1.0, 0.0};
    const std::array<double, 3> right{1.0, 0.0, 0.0};
    const std::array<double, 3> up{0.0, 0.0, 1.0};
    const auto bottom = createViewPerspectiveRayDirection(view, forward, right, up, 0.5, 0.0);
    const auto top = createViewPerspectiveRayDirection(view, forward, right, up, 0.5, 1.0);
    const auto leftEdge = createViewPerspectiveRayDirection(view, forward, right, up, 0.0, 0.5);
    CHECK_NEAR(std::atan2(bottom[2], bottom[1]) * 180.0 / kPi, -30.0, 1e-9);
    CHECK_NEAR(std::atan2(top[2], top[1]) * 180.0 / kPi, 30.0, 1e-9);
    CHECK_NEAR(std::atan2(leftEdge[0], leftEdge[1]) * 180.0 / kPi, -30.0, 1e-9);
}

TEST_CASE("solid angle of a 60 x 45 degree perspective view") {
    const auto view = perspectiveView({0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, 60.0, 45.0);
    const std::array<double, 3> forward{0.0, 1.0, 0.0};
    const std::array<double, 3> right{1.0, 0.0, 0.0};
    const std::array<double, 3> up{0.0, 0.0, 1.0};
    const double omega = createViewSphericalQuadSolidAngle(
        createViewPerspectiveRayDirection(view, forward, right, up, 0.0, 0.0),
        createViewPerspectiveRayDirection(view, forward, right, up, 1.0, 0.0),
        createViewPerspectiveRayDirection(view, forward, right, up, 1.0, 1.0),
        createViewPerspectiveRayDirection(view, forward, right, up, 0.0, 1.0));
    CHECK_NEAR(omega, 4.0 * std::asin(std::sin(30.0 * kPi / 180.0) * std::sin(22.5 * kPi / 180.0)), 1e-9);
}

TEST_CASE("window solid angle seen by a camera tilted 1.1 degrees up") {
    CreateViewRoomInfo room;
    CreateViewPreviewState::Surface window;
    window.semanticLabel = "Windows";
    window.vertices = {{-1.0, 4.0, 0.9}, {1.0, 4.0, 0.9}, {1.0, 4.0, 2.1}, {-1.0, 4.0, 2.1}};
    room.surfaces.push_back(window);
    for (bool reversed : {false, true}) {
        if (reversed) {
            std::reverse(room.surfaces[0].vertices.begin(), room.surfaces[0].vertices.end());
        }
        const auto info = computeCreateViewWindowFov(room, perspectiveView({0.0, 0.0, 1.2}, {0.0, 1.0, 0.02}, 60.0, 45.0));
        // The window is 4 m away and spans x -1..1, z -0.3..0.9 around the camera's foot point.
        const double expected = rectangleSolidAngle(4.0, -1.0, 1.0, -0.3, 0.9);
        CHECK(info.contributingSurfaceCount == 1);
        CHECK_NEAR(info.solidAngleSteradians, expected, 0.03 * expected);
    }
}

TEST_CASE("room: files the room loads through !xform are not given to oconv again") {
    const fs::path folder = makeTemporaryFolder("xform");
    writeText(folder / "room.rad",
        "void plastic wall\n0\n0\n5 .5 .5 .5 0 0\n\n"
        "wall polygon floor\n0\n0\n12 0 0 0  1 0 0  1 1 0  0 1 0\n\n"
        "!xform -t 2 0 0 desk.rad\n");
    writeText(folder / "desk.rad", "wall polygon desk_top\n0\n0\n12 0 0 0.7  1 0 0.7  1 1 0.7  0 1 0.7\n");
    const auto room = parseCreateViewRoom((folder / "room.rad").string(), folder);
    CHECK(room.valid);
    CHECK(room.xformCount == 1);
    CHECK(room.roomLoadedPaths.contains(fs::weakly_canonical(folder / "desk.rad")));
    CHECK(collectCreateViewRenderDependencyPaths(room, folder / "room.rad").empty());
    fs::remove_all(folder);
}

TEST_CASE("room: a comment after a primitive and an alias do not end the reading") {
    const fs::path folder = makeTemporaryFolder("comments");
    writeText(folder / "room.rad",
        "void plastic white 0 0 5 .8 .8 .8 0 0 # paint\n"
        "void alias wall_paint white\n"
        "wall_paint polygon floor\n0\n0\n12 0 0 0  1 0 0  1 1 0  0 1 0\n"
        "void glass clear 0 0 3 .9 .9 .9\n"
        "clear polygon win1 0 0 12 0 1 0  1 1 0  1 1 1  0 1 1\n");
    const auto room = parseCreateViewRoom((folder / "room.rad").string(), folder);
    CHECK(room.polygonCount == 2);
    CHECK(room.parseWarnings.empty());
    CHECK(room.materials.contains("clear"));
    CHECK(room.materials.contains("wall_paint") && room.materials.at("wall_paint").aliasOf == "white");
    CHECK(room.note.find("not found") == std::string::npos);
    fs::remove_all(folder);
}

TEST_CASE("room: a missing material is reported as a stand-in") {
    const fs::path folder = makeTemporaryFolder("standin");
    writeText(folder / "room.rad", "brick polygon wall1\n0\n0\n12 0 0 0  1 0 0  1 0 1  0 0 1\n");
    const auto room = parseCreateViewRoom((folder / "room.rad").string(), folder);
    CHECK(room.materials.contains("brick") && room.materials.at("brick").synthesized);
    CHECK(room.note.find("1 material(s) not found (brick)") != std::string::npos);
    fs::remove_all(folder);
}

TEST_CASE("room: frames and panels are not windows; brackets in folder names are not patterns") {
    CHECK(inferCreateViewSemanticLabel("window_frame", "alu", "metal") == "Structure");
    CHECK(inferCreateViewSemanticLabel("wall_panel", "paint", "plastic") == "Walls");
    CHECK(inferCreateViewSemanticLabel("ceiling_panel", "paint", "plastic") == "Ceilings");
    CHECK(inferCreateViewSemanticLabel("south_pane", "clear", "glass") == "Windows");
    CHECK(inferCreateViewSemanticLabel("win1", "clear_glass", "glass") == "Windows");
    const fs::path folder = makeTemporaryFolder("brackets") / "Office [v2]";
    fs::create_directories(folder);
    writeText(folder / "desk.rad", "\n");
    const auto paths = expandCreateViewPaths(folder, "desk.rad");
    CHECK(paths.size() == 1 && fs::exists(paths.front()));
    fs::remove_all(folder.parent_path());
}

TEST_CASE("Write to source puts a missing definition before the first primitive using it") {
    const fs::path folder = makeTemporaryFolder("insert");
    const fs::path file = folder / "room.rad";
    writeText(file, "void plastic grey\n0\n0\n5 .2 .2 .2 0 0\n\nbrick polygon wall1\n0\n0\n12 0 0 0  1 0 0  1 0 1  0 0 1\n");
    CreateViewMaterialInfo brick;
    brick.name = "brick";
    brick.modifier = "void";
    brick.type = "plastic";
    brick.synthesized = true;
    CreateViewMaterialOverride edit;
    edit.radianceType = "plastic";
    edit.numericArgs = {0.6, 0.3, 0.2, 0.0, 0.0};
    std::string error;
    CHECK(rewriteCreateViewMaterialDefinition(file, brick, edit, error));
    const std::string text = readText(file);
    const size_t definition = text.find("void plastic brick");
    const size_t use = text.find("brick polygon wall1");
    CHECK(definition != std::string::npos && use != std::string::npos && definition < use);
    CreateViewMaterialInfo alias;
    alias.name = "wall_paint";
    alias.type = "plastic";
    alias.aliasOf = "grey";
    CHECK(!rewriteCreateViewMaterialDefinition(file, alias, edit, error));
    fs::remove_all(folder);
}

TEST_CASE("environment: a .rad environment keeps its lines and gets absolute, quoted file names") {
    const fs::path folder = makeTemporaryFolder("radenv") / "Lighting Study";
    fs::create_directories(folder);
    HdrImage sky;
    sky.width = 4;
    sky.height = 4;
    sky.rgb.assign(4 * 4 * 3, 1.0f);
    writeRadianceHDR((folder / "sky.hdr").string(), sky);
    writeText(folder / "env.rad",
        "void colorpict skymap\n7 red green blue sky.hdr fisheye.cal fish_u fish_v\n0\n0\n\n"
        "skymap glow skyglow\n0\n0\n4 1 1 1 0\n\n"
        "skyglow source sky\n0\n0\n4 0 -1 0 180\n");
    const auto env = parseCreateViewEnvironment((folder / "env.rad").string(), folder);
    CHECK(env.valid);
    const std::string text = buildCreateViewEnvironmentRadText(env, "/opt/hdrspace lib/fisheye.cal");
    CHECK(text.find("\"" + (folder / "sky.hdr").lexically_normal().string() + "\"") != std::string::npos);
    CHECK(text.find("\"/opt/hdrspace lib/fisheye.cal\"") != std::string::npos);
    CHECK(text.find("env.rad") == std::string::npos);
    CHECK(std::count(text.begin(), text.end(), '\n') >= 10);
    fs::remove_all(folder.parent_path());
}

TEST_CASE("Write to source replaces one material and keeps the next one") {
    const fs::path folder = makeTemporaryFolder("materials");
    const fs::path file = folder / "materials.rad";
    writeText(file, "void plastic white\n0\n0\n5 .5 .5 .5 0 0\n\nvoid plastic grey\n0\n0\n5 .2 .2 .2 0 0\n");
    CreateViewMaterialInfo white;
    white.name = "white";
    white.modifier = "void";
    white.type = "plastic";
    white.sourcePath = file;
    CreateViewMaterialOverride edit;
    edit.radianceType = "plastic";
    edit.numericArgs = {0.8, 0.8, 0.8, 0.0, 0.0};
    std::string error;
    CHECK(rewriteCreateViewMaterialDefinition(file, white, edit, error));
    CHECK(error.empty());
    const std::string text = readText(file);
    CHECK(text.find("void plastic white\n0\n0\n5 0.800000 0.800000 0.800000 0.000000 0.000000\n") != std::string::npos);
    CHECK(text.find("void plastic grey\n0\n0\n5 .2 .2 .2 0 0\n") != std::string::npos);
    fs::remove_all(folder);
}

TEST_CASE("Write to source refuses material types with another argument layout") {
    const fs::path folder = makeTemporaryFolder("light");
    const fs::path file = folder / "lights.rad";
    const std::string original = "void light lamp\n0\n0\n3 100 100 100\n";
    writeText(file, original);
    CreateViewMaterialInfo lamp;
    lamp.name = "lamp";
    lamp.modifier = "void";
    lamp.type = "light";
    CreateViewMaterialOverride edit;
    edit.radianceType = "light";
    std::string error;
    CHECK(!rewriteCreateViewMaterialDefinition(file, lamp, edit, error));
    CHECK(error.find("plastic, metal and glass") != std::string::npos);
    CHECK(readText(file) == original);
    fs::remove_all(folder);
}

TEST_CASE("view files: only Radiance's six projections are accepted") {
    CreateViewViewInfo injected;
    parseCreateViewViewTokens({"-vtv;touch", "-vp", "0", "0", "1", "-vd", "0", "1", "0", "-vh", "60", "-vv", "45"}, injected);
    CHECK(injected.projectionToken.empty());
    CreateViewViewInfo fisheye;
    parseCreateViewViewTokens({"-vta", "-vp", "0", "0", "1", "-vd", "0", "1", "0", "-vh", "180", "-vv", "180"}, fisheye);
    CHECK(fisheye.projectionToken == "-vta");
    CHECK_NEAR(fisheye.vh, 180.0, 0.0);
}

TEST_CASE("view files: a view without -vt is rendered and analysed as Radiance's default -vtv") {
    CreateViewViewInfo view;
    parseCreateViewViewTokens({"-vp", "0", "0", "1", "-vd", "0", "1", "0", "-vh", "60", "-vv", "45"}, view);
    CHECK(view.projectionToken.empty());
    CHECK(createViewIsPerspectiveProjection(view));
    CHECK(buildCreateViewRenderViewArgs(view).rfind("-vtv ", 0) == 0);
}

TEST_CASE("environment: header source text with '!' is not copied into the scene") {
    const fs::path folder = makeTemporaryFolder("environment");
    const fs::path file = folder / "sky.hdr";
    HdrImage image;
    image.width = 16;
    image.height = 16;
    image.rgb.assign(16 * 16 * 3, 0.5f);
    writeRadianceHDR(file.string(), image, {"VIEW= -vta -vh 180 -vv 180 -vd 0 0 1 -vu 0 1 0",
                                            "SOLARSOURCE= void light sun 0 0 3 1 1 1 !touch hdrspace-test"});
    const auto info = parseCreateViewEnvironment(file.string(), folder);
    CHECK(info.rawSolarSource.empty());
    CHECK(info.note.find("Ignored the SOLARSOURCE") != std::string::npos);
    CHECK(buildCreateViewEnvironmentRadText(info, "fisheye.cal").find('!') == std::string::npos);
    fs::remove_all(folder);
}
