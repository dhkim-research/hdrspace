// Tests for the shared Radiance header rule (mergehdr hdrimage.h): which header lines describe a
// picture. Glare, extract, Blur, Statistics, gaze tone mapping and the viewer all read headers with
// it, so a change here changes every tool at once.

#include "TestHarness.h"

#include "hdrimage.h"

#include <string>
#include <vector>

namespace {

std::string valueOf(const std::map<std::string, std::string>& values, const std::string& key) {
    const auto it = values.find(key);
    return it == values.end() ? std::string{"<none>"} : it->second;
}

} // namespace

TEST_CASE("header: colour keys on the top lines win over copied headers") {
    // A pylinearhdr picture converted again: the top lines say Radiance RGB, the copy says sRGB.
    const std::vector<std::string> lines = {
        "#?RADIANCE",
        "\tLuminanceRGB= 0.21263901 0.71516868 0.07219232",
        "\tXYZCAM= 1 0 0 0 1 0 0 0 1",
        "LuminanceRGB= 0.26507413 0.67011463 0.06481124",
        "TargetPrimaries= 0.6400 0.3300 0.2900 0.6000 0.1500 0.0600",
        "FORMAT=32-bit_rle_rgbe",
    };
    const auto values = radianceDescribingValues(lines);
    CHECK(valueOf(values, "LuminanceRGB") == "0.26507413 0.67011463 0.06481124");
    CHECK(valueOf(values, "XYZCAM") == "<none>");
}

TEST_CASE("header: without colour keys on top, the nearest copied header gives them (pcomb)") {
    const std::vector<std::string> lines = {
        "#?RADIANCE",
        "pcomb -s 0.5 in.hdr",
        "\tLuminanceRGB= 0 1 0",
        "\tPRIMARIES= 1 0 0 1 0 0 0.3333 0.3333",
        "\tVIEW= -vta -vh 180 -vv 180",
        "\tEXPOSURE=2",
        "FORMAT=32-bit_rle_rgbe",
    };
    const auto values = radianceDescribingValues(lines);
    CHECK(valueOf(values, "LuminanceRGB") == "0 1 0");
    CHECK(valueOf(values, "PRIMARIES") == "1 0 0 1 0 0 0.3333 0.3333");
    // Geometry and exposure count only on the top lines, as in Radiance and evalglare.
    CHECK(valueOf(values, "VIEW") == "<none>");
    CHECK_NEAR(radianceHeaderExposureProduct(lines), 1.0, 1e-12);
}

TEST_CASE("header: the nearest copy gives its colour keys as a whole") {
    const std::vector<std::string> lines = {
        "#?RADIANCE",
        "\t\tLuminanceRGB= 0.21263901 0.71516868 0.07219232",
        "\t\tPRIMARIES= 0.64 0.33 0.30 0.60 0.15 0.06 0.3127 0.3290",
        "\tLuminanceRGB= 0 1 0",
        "FORMAT=32-bit_rle_rgbe",
    };
    const auto values = radianceDescribingValues(lines);
    CHECK(valueOf(values, "LuminanceRGB") == "0 1 0");
    CHECK(valueOf(values, "PRIMARIES") == "<none>");
}

TEST_CASE("header: EXPOSURE multiplies over the top lines only") {
    const std::vector<std::string> lines = {"#?RADIANCE", "EXPOSURE=2", "\tEXPOSURE=4", "EXPOSURE= 0.25"};
    CHECK_NEAR(radianceHeaderExposureProduct(lines), 0.5, 1e-12);
    CHECK_NEAR(radianceHeaderExposureProduct({"#?RADIANCE", "FORMAT=32-bit_rle_rgbe"}), 1.0, 1e-12);
}

TEST_CASE("header: a later top line wins, and the cd/m2 mark is found") {
    const std::vector<std::string> lines = {
        "#?RADIANCE",
        "VIEW= -vta -vh 180 -vv 180",
        "VIEW= -vtv -vh 60 -vv 45",
        "HDRSPACE_COLOR= XYZ cd/m2",
    };
    const auto values = radianceDescribingValues(lines);
    CHECK(valueOf(values, "VIEW") == "-vtv -vh 60 -vv 45");
    CHECK(radianceValuesInCdm2(values));
    CHECK(!radianceValuesInCdm2(radianceDescribingValues({"#?RADIANCE", "HDRSPACE_COLOR= xyz"})));
}
