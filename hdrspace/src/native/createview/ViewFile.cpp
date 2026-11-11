#include "ViewFile.h"

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

std::string radianceViewProjectionLabel(const std::string& token) {
    if (token == "-vta") return "Angular fisheye";
    if (token == "-vth") return "Hemispherical fisheye";
    if (token == "-vtv") return "Perspective";
    if (token == "-vtc") return "Cylindrical panorama";
    if (token == "-vts") return "Stereographic";
    if (token == "-vtl") return "Parallel";
    return token.empty() ? "--" : token;
}

bool parseCreateViewViewTokens(const std::vector<std::string>& tokens, CreateViewViewInfo& info) {
    bool hasPosition = false;
    bool hasDirection = false;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        // Only Radiance's six projections are accepted: the token is later put into shell
        // commands, so anything else (for example "-vtv;cmd") is ignored.
        if (token == "-vtv" || token == "-vtl" || token == "-vta" || token == "-vth" || token == "-vtc" || token == "-vts") {
            info.projectionToken = token;
            info.projectionLabel = radianceViewProjectionLabel(token);
        } else if (token == "-vp" && i + 3 < tokens.size()) {
            if (tryParseFloating(tokens[i + 1], info.position[0]) &&
                tryParseFloating(tokens[i + 2], info.position[1]) &&
                tryParseFloating(tokens[i + 3], info.position[2])) {
                hasPosition = true;
            }
            i += 3;
        } else if (token == "-vd" && i + 3 < tokens.size()) {
            if (tryParseFloating(tokens[i + 1], info.direction[0]) &&
                tryParseFloating(tokens[i + 2], info.direction[1]) &&
                tryParseFloating(tokens[i + 3], info.direction[2])) {
                hasDirection = true;
            }
            i += 3;
        } else if (token == "-vu" && i + 3 < tokens.size()) {
            tryParseFloating(tokens[i + 1], info.up[0]);
            tryParseFloating(tokens[i + 2], info.up[1]);
            tryParseFloating(tokens[i + 3], info.up[2]);
            i += 3;
        } else if (token == "-vh" && i + 1 < tokens.size()) {
            tryParseFloating(tokens[i + 1], info.vh);
            ++i;
        } else if (token == "-vv" && i + 1 < tokens.size()) {
            tryParseFloating(tokens[i + 1], info.vv);
            ++i;
        } else if (token == "-vo" && i + 1 < tokens.size()) {
            tryParseFloating(tokens[i + 1], info.vo);
            ++i;
        } else if (token == "-va" && i + 1 < tokens.size()) {
            tryParseFloating(tokens[i + 1], info.va);
            ++i;
        } else if (token == "-vs" && i + 1 < tokens.size()) {
            tryParseFloating(tokens[i + 1], info.vs);
            ++i;
        } else if (token == "-vl" && i + 1 < tokens.size()) {
            tryParseFloating(tokens[i + 1], info.vl);
            ++i;
        }
    }

    info.valid = hasPosition && hasDirection;
    if (info.projectionLabel.empty()) {
        info.projectionLabel = "Radiance view";
    }
    return info.valid;
}

CreateViewViewInfo parseCreateViewView(const std::string& spec, const fs::path& workingDir) {
    CreateViewViewInfo info;
    const auto value = trim(spec);
    if (value.empty()) {
        info.note = "Choose a .vf file or paste an rvu view command.";
        return info;
    }

    auto tryLine = [&](std::string line, const std::string& sourceLabel) -> bool {
        line = trim(line);
        if (line.empty()) {
            return false;
        }
        if (line.rfind("VIEW=", 0) == 0) {
            line = trim(line.substr(5));
        }
        auto tokens = splitWhitespace(line);
        if (tokens.empty()) {
            return false;
        }
        if (tokens.front() == "rvu" || tokens.front() == "rview" || tokens.front() == "vwrays") {
            tokens.erase(tokens.begin());
        }
        CreateViewViewInfo parsed;
        parsed.sourceLabel = sourceLabel;
        if (!parseCreateViewViewTokens(tokens, parsed)) {
            return false;
        }
        info = parsed;
        return true;
    };

    fs::path path{value};
    // The text may be an inline view line rather than a path: error_code overloads, so a long line
    // (ENAMETOOLONG) does not throw out of the refresh (#64).
    std::error_code pathError;
    if (path.is_relative()) {
        const fs::path resolved = fs::weakly_canonical(workingDir / path, pathError);
        path = pathError ? workingDir / path : resolved;
    }
    if (fs::is_regular_file(path, pathError)) {
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) {
            if (tryLine(line, path.filename().string())) {
                return info;
            }
        }
        info.note = "The .vf file did not contain a parsable view command.";
        return info;
    }

    if (tryLine(value, "Inline view spec")) {
        return info;
    }

    info.note = "Could not parse the view specification.";
    return info;
}

bool parseCreateViewRenderResolution(const std::string& text, int& value) {
    int parsed = 0;
    if (!tryParseInteger(trim(text), parsed) || parsed <= 0) {
        return false;
    }
    value = parsed;
    return true;
}

std::string buildCreateViewRenderViewArgs(const CreateViewViewInfo& viewInfo) {
    std::ostringstream args;
    // A view without -vt is perspective in Radiance (-vtv); the window analysis treats it the same way.
    const std::string projection = viewInfo.projectionToken.empty() ? "-vtv" : viewInfo.projectionToken;
    args << projection
         << " -vp " << viewInfo.position[0] << " " << viewInfo.position[1] << " " << viewInfo.position[2]
         << " -vd " << viewInfo.direction[0] << " " << viewInfo.direction[1] << " " << viewInfo.direction[2]
         << " -vu " << viewInfo.up[0] << " " << viewInfo.up[1] << " " << viewInfo.up[2]
         << " -vh " << viewInfo.vh
         << " -vv " << viewInfo.vv
         << " -vo " << viewInfo.vo
         << " -va " << viewInfo.va
         << " -vs " << viewInfo.vs
         << " -vl " << viewInfo.vl;
    return args.str();
}

} // namespace hdrspace::createview
