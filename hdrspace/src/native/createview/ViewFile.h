#pragma once

// Reading Radiance views (.vf, rvu text) for Create View and writing view options for the renderer.

#include "Types.h"

namespace hdrspace::createview {

std::string radianceViewProjectionLabel(const std::string& token);

bool parseCreateViewViewTokens(const std::vector<std::string>& tokens, CreateViewViewInfo& info);

CreateViewViewInfo parseCreateViewView(const std::string& spec, const fs::path& workingDir);

bool parseCreateViewRenderResolution(const std::string& text, int& value);

std::string buildCreateViewRenderViewArgs(const CreateViewViewInfo& viewInfo);

} // namespace hdrspace::createview
