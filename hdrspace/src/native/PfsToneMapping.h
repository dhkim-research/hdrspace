#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace native {

enum class EPfsToneMappingProvider {
    NativeInteractive,
    Reinhard02,
    Reinhard05,
    Mantiuk06,
    Mantiuk08,
    Drago03,
    Durand02,
    Fattal02,
    Pattanaik00,
    Ferradans11,
    Mai11,
    Ward97, // Radiance pcond (Ward Larson, Rushmeier and Piatko 1997)
};

struct PfsToneMappingParameterSpec {
    std::string label;
    std::string option;
    std::string defaultValue;
    std::string tooltip;
    bool optional = false;
    // Presentation only (inspector): shown under "Advanced", and the value used by the
    // original pfstools man-page example (empty = same as the default).
    bool advanced = false;
    std::string exampleValue;
};

struct PfsToneMappingToggleSpec {
    std::string label;
    std::string option;
    bool defaultValue = false;
    std::string tooltip;
    bool advanced = false;
    int exampleValue = -1; // -1 = same as the default
};

struct PfsToneMappingProviderSpec {
    EPfsToneMappingProvider provider = EPfsToneMappingProvider::NativeInteractive;
    std::string label;
    std::string executable;
    std::string slug;
    std::string description;
    std::vector<PfsToneMappingParameterSpec> parameters;
    std::vector<PfsToneMappingToggleSpec> toggles;
    // Presentation only: tile name, tile detail line and the man-page example command
    // (empty = the man page has no example that differs from the defaults).
    std::string shortName;
    std::string detail;
    std::string exampleCommand;
};

struct PfsToneMappingRequest {
    EPfsToneMappingProvider provider = EPfsToneMappingProvider::NativeInteractive;
    std::filesystem::path toolsDirectory;
    std::filesystem::path radianceDirectory; // Radiance bin (pcond, pfilt) for Ward97
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
    std::vector<std::string> parameterValues;
    std::vector<bool> toggleValues;
    bool radianceAbsolute = true;
    std::string previewWidth;
};

struct PfsToneMappingCommand {
    bool ok = false;
    std::string command;
    std::string displayCommand;
    std::string error;
};

const std::vector<PfsToneMappingProviderSpec>& pfsToneMappingProviderSpecs();
const PfsToneMappingProviderSpec& pfsToneMappingProviderSpec(EPfsToneMappingProvider provider);
std::filesystem::path detectPfsToolsDirectory();
PfsToneMappingCommand buildPfsToneMappingCommand(const PfsToneMappingRequest& request);

} // namespace native
