#include "native/PfsToneMapping.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace native {
namespace {

constexpr const char* kPostGammaOption = "@post-gamma";

std::string trim(std::string value) {
    const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string shellQuote(const std::string& value) {
    std::string result = "'";
    for (char ch : value) {
        if (ch == '\'') {
            result += "'\\''";
        } else {
            result += ch;
        }
    }
    result += "'";
    return result;
}

PfsToneMappingParameterSpec parameter(
    std::string label,
    std::string option,
    std::string defaultValue,
    std::string tooltip,
    bool optional = false
) {
    return {
        std::move(label),
        std::move(option),
        std::move(defaultValue),
        std::move(tooltip),
        optional,
        false,
        {},
    };
}

PfsToneMappingToggleSpec toggle(
    std::string label,
    std::string option,
    bool defaultValue,
    std::string tooltip
) {
    return {
        std::move(label),
        std::move(option),
        defaultValue,
        std::move(tooltip),
        false,
        -1,
    };
}

// Tile names, man-page examples and the parameters kept under "Advanced". Example values
// are copied from the EXAMPLES section of each pfstools 2.2.0 man page; operators whose
// example equals the defaults, or applies to uncalibrated input only, have none.
struct PresentationEntry {
    EPfsToneMappingProvider provider;
    const char* shortName;
    const char* detail;
    const char* exampleCommand;
    std::vector<std::pair<std::string, std::string>> examples; // option -> value ("1"/"0" for toggles)
    std::vector<std::string> advanced;                          // options
};

std::vector<PfsToneMappingProviderSpec> withPresentation(std::vector<PfsToneMappingProviderSpec> specs) {
    const std::vector<PresentationEntry> entries{
        {EPfsToneMappingProvider::NativeInteractive, "Native", "gaze", "", {}, {}},
        {EPfsToneMappingProvider::Reinhard02, "Reinhard \u2019""02", "2002 \u00b7 photographic",
         "pfstmo_reinhard02 -s | pfsgamma -g 2.2", {{"--scales", "1"}},
         {"--range", "--lower", "--upper", "--temporal-coherent"}},
        {EPfsToneMappingProvider::Reinhard05, "Reinhard \u2019""05", "2005 \u00b7 photoreceptor", "", {}, {}},
        {EPfsToneMappingProvider::Mantiuk06, "Mantiuk \u2019""06", "2006 \u00b7 contrast mapping",
         "pfstmo_mantiuk06 -f 0.5 | pfsgamma -g 2.2", {{"--factor", "0.5"}},
         {"--itmax", "--tol", "--bcg"}},
        {EPfsToneMappingProvider::Mantiuk08, "Mantiuk \u2019""08", "2008 \u00b7 display adaptive",
         "pfstmo_mantiuk08 -d g=2.6:l=500:b=0.5:k=0.01:a=10",
         {{"@display-gamma", "2.6"}, {"@display-peak", "500"}, {"@display-black", "0.5"},
          {"@display-reflectivity", "0.01"}, {"@display-ambient", "10"}},
         {"@display-reflectivity", "@display-ppd", "@display-distance", "--white-y", "--fps", "--output-tone-curve"}},
        {EPfsToneMappingProvider::Drago03, "Drago \u2019""03", "2003 \u00b7 adaptive log",
         "pfstmo_drago03 -b 0.8 | pfsgamma -g 1.7", {{"--bias", "0.8"}}, {}},
        {EPfsToneMappingProvider::Durand02, "Durand \u2019""02", "2002 \u00b7 bilateral", "", {}, {}},
        {EPfsToneMappingProvider::Fattal02, "Fattal \u2019""02", "2002 \u00b7 gradient domain",
         "pfstmo_fattal02 -b 0.85 -g 0.7 -w 2.0 (solver kept)",
         {{"--beta", "0.85"}, {"--gamma", "0.7"}, {"--white-point", "2.0"}},
         {"--noise", "--detail-level", "-k", "--multigrid"}},
        {EPfsToneMappingProvider::Pattanaik00, "Pattanaik \u2019""00", "2000 \u00b7 visual adaptation", "", {},
         {"--fps", "--time-dependence"}},
        {EPfsToneMappingProvider::Ferradans11, "Ferradans \u2019""11", "2011 \u00b7 retina model",
         "pfstmo_ferradans11 --rho -3 --inv_alpha 10", {{"--rho", "-3"}, {"--inv_alpha", "10"}}, {}},
        {EPfsToneMappingProvider::Mai11, "Mai \u2019""11", "2011 \u00b7 backward compatible", "", {}, {}},
        {EPfsToneMappingProvider::Ward97, "Ward \u2019""97", "1997 \u00b7 Radiance pcond", "", {},
         {"-a", "-v", "-s", "-c", "-w", "-l"}},
    };
    for (auto& spec : specs) {
        const auto entry = std::find_if(entries.begin(), entries.end(),
            [&](const PresentationEntry& e) { return e.provider == spec.provider; });
        if (entry == entries.end()) {
            spec.shortName = spec.label;
            continue;
        }
        spec.shortName = entry->shortName;
        spec.detail = entry->detail;
        spec.exampleCommand = entry->exampleCommand;
        const auto isAdvanced = [&](const std::string& option) {
            return std::find(entry->advanced.begin(), entry->advanced.end(), option) != entry->advanced.end();
        };
        for (auto& parameter : spec.parameters) {
            parameter.advanced = isAdvanced(parameter.option);
            for (const auto& [option, value] : entry->examples) {
                if (option == parameter.option) parameter.exampleValue = value;
            }
        }
        for (auto& toggleSpec : spec.toggles) {
            toggleSpec.advanced = isAdvanced(toggleSpec.option);
            for (const auto& [option, value] : entry->examples) {
                if (option == toggleSpec.option) toggleSpec.exampleValue = value == "1" ? 1 : 0;
            }
        }
    }
    return specs;
}

const std::vector<PfsToneMappingProviderSpec> kProviderSpecs = withPresentation({
    {
        EPfsToneMappingProvider::NativeInteractive,
        "Native interactive (gaze)",
        "",
        "native",
        "hdrspace's real-time global exposure with uniform, center-weighted, or Gaussian gaze metering.",
        {},
        {},
    },
    {
        EPfsToneMappingProvider::Reinhard02,
        "Reinhard 2002 (Photographic)",
        "pfstmo_reinhard02",
        "reinhard02",
        "Original pfstools photographic operator. Global by default; Scales enables the local version.",
        {
            parameter("Key a (alpha)", "--key", "0.18", "Maps the log-average luminance to this key value (0 < a <= 1)."),
            parameter("Phi", "--phi", "1.0", "Sharpening parameter used by the local, scale-based version."),
            parameter("Scale range", "--range", "8", "Number of local scales."),
            parameter("Lower scale", "--lower", "1", "Smallest local scale in pixels."),
            parameter("Upper scale", "--upper", "43", "Largest local scale in pixels."),
            parameter("Post gamma", kPostGammaOption, "2.2", "Original pfstools workflow applies pfsgamma after this operator."),
        },
        {
            toggle("Use local scales", "--scales", false, "Enable the local Reinhard 2002 operator. Phi and scale controls then matter."),
            toggle("Temporal coherent", "--temporal-coherent", false, "Limit average and maximum luminance changes between frames. Intended for sequences."),
        },
    },
    {
        EPfsToneMappingProvider::Reinhard05,
        "Reinhard 2005",
        "pfstmo_reinhard05",
        "reinhard05",
        "Original pfstools Reinhard and Devlin photoreceptor-inspired operator.",
        {
            parameter("Brightness f", "--brightness", "0", "Overall brightness, accepted range -8 to 8."),
            parameter("Chromatic c", "--chromatic", "0.5", "Chromatic adaptation, accepted range 0 to 1."),
            parameter("Light adaptation l", "--light", "0.75", "Local versus global light adaptation, accepted range 0 to 1."),
            parameter("Post gamma", kPostGammaOption, "2.2", "Original pfstools example applies pfsgamma after this operator."),
        },
        {},
    },
    {
        EPfsToneMappingProvider::Mantiuk06,
        "Mantiuk 2006",
        "pfstmo_mantiuk06",
        "mantiuk06",
        "Original pfstools contrast mapping/equalization operator. Factor and equalization are mutually exclusive.",
        {
            parameter("Contrast factor", "--factor", "0.1", "Contrast mapping factor, accepted range 0 to 1."),
            parameter("Saturation", "--saturation", "0.8", "Color saturation factor, accepted range 0 to 2."),
            parameter("Equalize amount", "@equalize-amount", "0.5", "Used only when contrast equalization is enabled."),
            parameter("Max iterations", "--itmax", "200", "Maximum conjugate-gradient iterations."),
            parameter("Tolerance", "--tol", "0.001", "Convergence tolerance, accepted range 0 to 1."),
            parameter("Post gamma", kPostGammaOption, "2.2", "Original pfstools workflow applies pfsgamma after this operator."),
        },
        {
            toggle("Use contrast equalization", "@use-equalize", false, "Use --equalize-contrast instead of --factor."),
            toggle("Use Biconjugate Gradient", "--bcg", false, "Use the original BCG solver switch."),
        },
    },
    {
        EPfsToneMappingProvider::Mantiuk08,
        "Mantiuk 2008 (Display adaptive)",
        "pfstmo_mantiuk08",
        "mantiuk08",
        "Original display-adaptive operator. It performs its own frame analysis and display modeling; no post gamma is required.",
        {
            parameter("Display gamma", "@display-gamma", "2.2", "Gamma in the gamma-gain-black-ambient display model."),
            parameter("Peak luminance", "@display-peak", "200", "Display peak luminance l in cd/m2."),
            parameter("Black luminance", "@display-black", "0.8", "Display black level b in cd/m2."),
            parameter("Ambient illuminance", "@display-ambient", "60", "Ambient illuminance a in lux."),
            parameter("Screen reflectivity", "@display-reflectivity", "0.01", "Diffuse screen reflectivity k."),
            parameter("Pixels per degree", "@display-ppd", "30", "Display angular resolution in pixels per visual degree."),
            parameter("Viewing distance (m)", "@display-distance", "0.5", "Viewing distance attached to the display-size model."),
            parameter("Color saturation", "--color-saturation", "1.0", "Color saturation factor, accepted range 0 to 2."),
            parameter("Contrast enhancement", "--contrast-enhancement", "1.0", "Contrast enhancement factor; 1 preserves contrast."),
            parameter("Reference white Y", "--white-y", "", "Optional luminance factor for reference white; blank uses image metadata or none.", true),
            parameter("Frames per second", "--fps", "25", "Temporal filter rate. The implementation accepts 25, 30, or 60."),
            parameter("Tone-curve CSV", "--output-tone-curve", "", "Optional path for the generated tone curve CSV.", true),
        },
        {},
    },
    {
        EPfsToneMappingProvider::Drago03,
        "Drago 2003",
        "pfstmo_drago03",
        "drago03",
        "Original adaptive logarithmic mapping operator.",
        {
            parameter("Bias", "--bias", "0.85", "Bias parameter, accepted range 0 to 1."),
            parameter("Post gamma", kPostGammaOption, "1.7", "The original pfstools man-page example uses pfsgamma 1.7."),
        },
        {},
    },
    {
        EPfsToneMappingProvider::Durand02,
        "Durand 2002 (Bilateral)",
        "pfstmo_durand02",
        "durand02",
        "Original bilateral-filtering operator with the pfstools color-correction extension enabled by default.",
        {
            parameter("Spatial sigma", "--sigma-s", "40", "Spatial bilateral-filter sigma in pixels."),
            parameter("Range sigma", "--sigma-r", "0.4", "Range bilateral-filter sigma."),
            parameter("Base contrast", "--base-contrast", "5.0", "Maximum contrast of the compressed base layer."),
            parameter("Post gamma", kPostGammaOption, "2.2", "Original pfstools workflow applies pfsgamma after this operator."),
        },
        {
            toggle("Original paper colors", "--original", false, "Disable the pfstools color-correction extension."),
        },
    },
    {
        EPfsToneMappingProvider::Fattal02,
        "Fattal 2002 (Gradient domain)",
        "pfstmo_fattal02",
        "fattal02",
        "Original gradient-domain operator. These defaults match the installed multigrid build.",
        {
            parameter("Alpha", "--alpha", "1.0", "Gradient threshold parameter. The upstream implementation marks its global-gamma side effect as deprecated."),
            parameter("Beta", "--beta", "0.9", "Gradient attenuation strength."),
            parameter("Internal gamma", "--gamma", "1.0", "Internal Fattal gamma; this is not the display post-gamma stage."),
            parameter("Saturation", "--saturation", "0.8", "Color saturation, accepted range 0 to 1."),
            parameter("Noise floor", "--noise", "0.01", "Gradient noise floor."),
            parameter("Detail level", "--detail-level", "0", "Detail level, accepted range 0 to 9."),
            parameter("White point (%)", "--white-point", "0.5", "Percentage clipped at the white end, accepted range 0 to 50."),
            parameter("Black point (%)", "-k", "0.1", "Percentage clipped at the black end. -k avoids an upstream long-option parsing bug."),
        },
        {
            toggle("Use multigrid solver", "--multigrid", true, "The installed binary already defaults to multigrid; this keeps the command explicit."),
        },
    },
    {
        EPfsToneMappingProvider::Pattanaik00,
        "Pattanaik 2000",
        "pfstmo_pattanaik00",
        "pattanaik00",
        "Original visual-adaptation operator. Cone and rod adaptation are auto-estimated when left blank.",
        {
            parameter("Multiplier", "--mul", "1.0", "Input luminance multiplier."),
            parameter("Cone adaptation", "--cone", "", "Optional cone adaptation luminance; blank selects the log-average automatically.", true),
            parameter("Rod adaptation", "--rod", "", "Optional rod adaptation luminance; blank follows the automatic/cone value.", true),
            parameter("Frames per second", "--fps", "16", "Temporal adaptation rate for image sequences."),
            parameter("Post gamma", kPostGammaOption, "2.2", "Original pfstools workflow applies pfsgamma after this operator."),
        },
        {
            toggle("Time dependence", "--time-dependence", false, "Enable temporal adaptation for an image sequence."),
            toggle("Local adaptation", "--local", false, "Use local adaptation. This cancels the time-dependent effect."),
        },
    },
    {
        EPfsToneMappingProvider::Ferradans11,
        "Ferradans 2011",
        "pfstmo_ferradans11",
        "ferradans11",
        "Original visual-adaptation operator. No post gamma is required.",
        {
            parameter("Rho", "--rho", "-2", "Installed-code default is -2. The executable help recommends experimenting around 0."),
            parameter("Inverse alpha", "--inv_alpha", "5", "Installed-code default is 5. Larger values increase local contrast."),
        },
        {},
    },
    {
        EPfsToneMappingProvider::Mai11,
        "Mai 2011",
        "pfstmo_mai11",
        "mai11",
        "Backward-compatible HDR compression tone curve. It has no user tone parameters and requires no post gamma.",
        {},
        {},
    },
    {
        EPfsToneMappingProvider::Ward97,
        "Ward 1997 (Radiance pcond)",
        "pcond",
        "ward97",
        "Ward Larson, Rushmeier and Piatko visibility-matching histogram adjustment, run by Radiance pcond. Reads Radiance .hdr input.",
        {
            parameter("Display max", "-u", "", "Maximum display luminance in cd/m2 (pcond -u). Blank uses the pcond default.", true),
            parameter("Display range", "-d", "", "Display dynamic range, maximum over minimum (pcond -d). Blank uses the pcond default.", true),
            parameter("Post gamma", kPostGammaOption, "2.2", "pcond writes display-linear values; Radiance ra_tiff applies gamma 2.2 by default."),
        },
        {
            toggle("Human vision (-h)", "-h", false, "Human visual response: acuity loss, veiling glare, contrast sensitivity and colour loss in dim light (= -a -v -s -c)."),
            toggle("Acuity loss (-a)", "-a", false, "Lose detail in dark regions according to visual acuity."),
            toggle("Veiling glare (-v)", "-v", false, "Add veiling glare from bright sources."),
            toggle("Contrast sensitivity (-s)", "-s", false, "Use human contrast sensitivity in the histogram adjustment."),
            toggle("Colour loss (-c)", "-c", false, "Mesopic and scotopic loss of colour."),
            toggle("Centre-weighted (-w)", "-w", false, "Weight the histogram towards the image centre."),
            toggle("Linear only (-l)", "-l", false, "Use a linear response function instead of histogram adjustment."),
        },
    },
});

std::string parameterValue(
    const PfsToneMappingProviderSpec& spec,
    const PfsToneMappingRequest& request,
    size_t index
) {
    if (index < request.parameterValues.size()) {
        return trim(request.parameterValues[index]);
    }
    return spec.parameters[index].defaultValue;
}

bool toggleValue(const PfsToneMappingProviderSpec& spec, const PfsToneMappingRequest& request, size_t index) {
    if (index < request.toggleValues.size()) {
        return request.toggleValues[index];
    }
    return spec.toggles[index].defaultValue;
}

bool hasToggle(
    const PfsToneMappingProviderSpec& spec,
    const PfsToneMappingRequest& request,
    const std::string& option
) {
    for (size_t index = 0; index < spec.toggles.size(); ++index) {
        if (spec.toggles[index].option == option) {
            return toggleValue(spec, request, index);
        }
    }
    return false;
}

void appendOption(std::ostringstream& command, const std::string& option, const std::string& value) {
    command << " " << option;
    if (!value.empty()) {
        command << " " << shellQuote(value);
    }
}

} // namespace

const std::vector<PfsToneMappingProviderSpec>& pfsToneMappingProviderSpecs() {
    return kProviderSpecs;
}

const PfsToneMappingProviderSpec& pfsToneMappingProviderSpec(EPfsToneMappingProvider provider) {
    const auto it = std::find_if(
        kProviderSpecs.begin(),
        kProviderSpecs.end(),
        [provider](const auto& spec) { return spec.provider == provider; }
    );
    return it == kProviderSpecs.end() ? kProviderSpecs.front() : *it;
}

fs::path detectPfsToolsDirectory() {
    if (const char* configured = std::getenv("HDRSPACE_PFSTOOLS_BIN"); configured && *configured) {
        const fs::path candidate{configured};
        if (fs::exists(candidate / "pfsin") && fs::exists(candidate / "pfsout")) {
            return candidate;
        }
    }

    // An activated conda environment first, then every environment of the usual conda
    // installations (in name order, so the choice is stable), then system locations.
    std::vector<fs::path> candidates;
    if (const char* prefix = std::getenv("CONDA_PREFIX"); prefix && *prefix) {
        candidates.emplace_back(fs::path{prefix} / "bin");
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        for (const char* root : {"miniforge3", "miniconda3", "anaconda3", "mambaforge"}) {
            const fs::path base = fs::path{home} / root;
            std::vector<fs::path> environments;
            std::error_code ec;
            for (fs::directory_iterator it{base / "envs", ec}, end; !ec && it != end; it.increment(ec)) {
                environments.push_back(it->path() / "bin");
            }
            std::sort(environments.begin(), environments.end());
            candidates.insert(candidates.end(), environments.begin(), environments.end());
            candidates.push_back(base / "bin");
        }
    }
    candidates.emplace_back("/opt/homebrew/bin");
    candidates.emplace_back("/usr/local/bin");
    candidates.emplace_back("/usr/bin");

    for (const auto& candidate : candidates) {
        if (fs::exists(candidate / "pfsin") && fs::exists(candidate / "pfsout")) {
            return candidate;
        }
    }
    return {};
}

PfsToneMappingCommand buildPfsToneMappingCommand(const PfsToneMappingRequest& request) {
    PfsToneMappingCommand result;
    const auto& spec = pfsToneMappingProviderSpec(request.provider);
    if (request.provider == EPfsToneMappingProvider::NativeInteractive) {
        result.error = "Native interactive tone mapping does not use a pfstools command.";
        return result;
    }
    if (request.toolsDirectory.empty()) {
        result.error = "Choose a pfstools bin directory.";
        return result;
    }
    if (request.inputPath.empty() || !fs::exists(request.inputPath)) {
        result.error = "The source image must be a saved file.";
        return result;
    }
    if (request.outputPath.empty()) {
        result.error = "A preview output path is required.";
        return result;
    }

    if (request.provider == EPfsToneMappingProvider::Ward97) {
        // Radiance pcond reads a Radiance picture (optionally reduced with pfilt first), writes
        // display-linear RGBE, and pfstools turns that into the PNG preview.
        std::string extension = request.inputPath.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (extension != ".hdr" && extension != ".pic") {
            result.error = "Ward (pcond) reads Radiance pictures. Open the .hdr version of this image.";
            return result;
        }
        const fs::path pcond = request.radianceDirectory / "pcond";
        const fs::path pfilt = request.radianceDirectory / "pfilt";
        if (request.radianceDirectory.empty() || !fs::exists(pcond) || !fs::exists(pfilt)) {
            result.error = "Radiance pcond / pfilt were not found in the bundled Radiance tools.";
            return result;
        }
        for (const char* tool : {"pfsin", "pfsout", "pfsgamma"}) {
            if (!fs::exists(request.toolsDirectory / tool)) {
                result.error = "Missing pfstools executable: " + (request.toolsDirectory / tool).string();
                return result;
            }
        }
        const std::string wardWidth = trim(request.previewWidth);
        const bool reduce = !wardWidth.empty() && wardWidth != "full" && wardWidth != "Full";
        if (reduce && (!std::all_of(wardWidth.begin(), wardWidth.end(), [](unsigned char ch) { return std::isdigit(ch); }) ||
                       wardWidth.size() > 6 || std::stoul(wardWidth) < 16)) {
            result.error = "Preview width must be Full or an integer of at least 16 pixels.";
            return result;
        }
        std::ostringstream options;
        for (size_t index = 0; index < spec.toggles.size(); ++index) {
            if (toggleValue(spec, request, index)) {
                options << " " << spec.toggles[index].option;
            }
        }
        std::string wardGamma;
        for (size_t index = 0; index < spec.parameters.size(); ++index) {
            const auto& parameterSpec = spec.parameters[index];
            const std::string value = parameterValue(spec, request, index);
            if (parameterSpec.option == kPostGammaOption) {
                wardGamma = value;
            } else if (!value.empty()) {
                options << " " << parameterSpec.option << " " << shellQuote(value);
            }
        }
        std::ostringstream pipeline;
        pipeline << "set -o pipefail\n";
        pipeline << "export PATH=" << shellQuote(request.toolsDirectory.string()) << ":"
                 << shellQuote(request.radianceDirectory.string()) << ":\"$PATH\"\n";
        pipeline << "tmp=$(mktemp -d \"${TMPDIR:-/tmp}/hdrspace-pcond.XXXXXX\") || exit 1\n";
        pipeline << "trap 'rm -rf \"$tmp\"' EXIT\n";
        std::string source = shellQuote(request.inputPath.string());
        if (reduce) {
            // Keep the aspect ratio: pfilt takes both sizes, so derive the height from the
            // picture's resolution string ("-Y H +X W"). Larger widths keep the source size.
            pipeline << "set -- $(" << shellQuote((request.radianceDirectory / "getinfo").string()) << " -d < "
                     << source << ")\n";
            pipeline << "if [ \"$4\" -gt " << wardWidth << " ]; then\n";
            pipeline << "  " << shellQuote(pfilt.string()) << " -1 -x " << wardWidth << " -y $(( ($2 * " << wardWidth
                     << " + $4 / 2) / $4 )) " << source << " > \"$tmp/in.hdr\" || exit 1\n";
            pipeline << "else\n  cp " << source << " \"$tmp/in.hdr\" || exit 1\nfi\n";
            source = "\"$tmp/in.hdr\"";
        }
        pipeline << shellQuote(pcond.string()) << options.str() << " " << source << " > \"$tmp/out.hdr\" || exit 1\n";
        pipeline << shellQuote((request.toolsDirectory / "pfsin").string()) << " --quiet \"$tmp/out.hdr\"";
        if (!wardGamma.empty()) {
            pipeline << " | " << shellQuote((request.toolsDirectory / "pfsgamma").string()) << " -g " << shellQuote(wardGamma);
        }
        pipeline << " | " << shellQuote((request.toolsDirectory / "pfsout").string())
                 << " " << shellQuote(request.outputPath.string());
        result.ok = true;
        result.displayCommand = pipeline.str();
        result.command = "zsh -lc " + shellQuote(result.displayCommand);
        return result;
    }

    const std::array requiredTools{
        std::string{"pfsin"},
        std::string{"pfsout"},
        spec.executable,
    };
    for (const auto& executable : requiredTools) {
        if (!fs::exists(request.toolsDirectory / executable)) {
            result.error = "Missing pfstools executable: " + (request.toolsDirectory / executable).string();
            return result;
        }
    }

    const std::string width = trim(request.previewWidth);
    if (!width.empty() && width != "full" && width != "Full") {
        if (!fs::exists(request.toolsDirectory / "pfssize")) {
            result.error = "Missing pfstools executable: " + (request.toolsDirectory / "pfssize").string();
            return result;
        }
        unsigned long numericWidth = 0;
        try {
            if (!std::all_of(width.begin(), width.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
                throw std::invalid_argument{"not numeric"};
            }
            numericWidth = std::stoul(width);
        } catch (const std::exception&) {
            result.error = "Preview width must be Full or an integer of at least 16 pixels.";
            return result;
        }
        if (numericWidth < 16) {
            result.error = "Preview width must be Full or an integer of at least 16 pixels.";
            return result;
        }
    }

    std::ostringstream provider;
    provider << shellQuote((request.toolsDirectory / spec.executable).string());
    std::string postGamma;

    if (request.provider == EPfsToneMappingProvider::Mantiuk06) {
        const bool equalize = hasToggle(spec, request, "@use-equalize");
        appendOption(
            provider,
            equalize ? "--equalize-contrast" : "--factor",
            parameterValue(spec, request, equalize ? 2 : 0)
        );
        appendOption(provider, "--saturation", parameterValue(spec, request, 1));
        appendOption(provider, "--itmax", parameterValue(spec, request, 3));
        appendOption(provider, "--tol", parameterValue(spec, request, 4));
        postGamma = parameterValue(spec, request, 5);
        if (hasToggle(spec, request, "--bcg")) {
            appendOption(provider, "--bcg", "");
        }
    } else if (request.provider == EPfsToneMappingProvider::Mantiuk08) {
        const std::string displayFunction =
            "g=" + parameterValue(spec, request, 0) +
            ":l=" + parameterValue(spec, request, 1) +
            ":b=" + parameterValue(spec, request, 2) +
            ":k=" + parameterValue(spec, request, 4) +
            ":a=" + parameterValue(spec, request, 3);
        const std::string displaySize =
            "ppd=" + parameterValue(spec, request, 5) +
            ":d=" + parameterValue(spec, request, 6);
        appendOption(provider, "--display-function", displayFunction);
        appendOption(provider, "--display-size", displaySize);
        appendOption(provider, "--color-saturation", parameterValue(spec, request, 7));
        appendOption(provider, "--contrast-enhancement", parameterValue(spec, request, 8));
        const std::string whiteY = parameterValue(spec, request, 9);
        if (!whiteY.empty()) {
            appendOption(provider, "--white-y", whiteY);
        }
        appendOption(provider, "--fps", parameterValue(spec, request, 10));
        const std::string toneCurve = parameterValue(spec, request, 11);
        if (!toneCurve.empty()) {
            appendOption(provider, "--output-tone-curve", toneCurve);
        }
    } else {
        for (size_t index = 0; index < spec.parameters.size(); ++index) {
            const auto& parameterSpec = spec.parameters[index];
            const std::string value = parameterValue(spec, request, index);
            if (parameterSpec.option == kPostGammaOption) {
                postGamma = value;
                continue;
            }
            if (parameterSpec.optional && value.empty()) {
                continue;
            }
            appendOption(provider, parameterSpec.option, value);
        }
        for (size_t index = 0; index < spec.toggles.size(); ++index) {
            const auto& toggleSpec = spec.toggles[index];
            if (toggleSpec.option.starts_with('@')) {
                continue;
            }
            if (toggleValue(spec, request, index)) {
                appendOption(provider, toggleSpec.option, "");
            }
        }
    }

    if (!postGamma.empty() && !fs::exists(request.toolsDirectory / "pfsgamma")) {
        result.error = "Missing pfstools executable: " + (request.toolsDirectory / "pfsgamma").string();
        return result;
    }

    std::ostringstream pipeline;
    pipeline << "set -o pipefail\n";
    pipeline << "export PATH=" << shellQuote(request.toolsDirectory.string()) << ":\"$PATH\"\n";
    pipeline << shellQuote((request.toolsDirectory / "pfsin").string());
    std::string inputExtension = request.inputPath.extension().string();
    std::transform(
        inputExtension.begin(),
        inputExtension.end(),
        inputExtension.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); }
    );
    if (request.radianceAbsolute && (inputExtension == ".hdr" || inputExtension == ".pic")) {
        pipeline << " --radiance";
    }
    pipeline << " --quiet " << shellQuote(request.inputPath.string());
    if (!width.empty() && width != "full" && width != "Full") {
        pipeline << " | " << shellQuote((request.toolsDirectory / "pfssize").string())
                 << " --x " << shellQuote(width);
    }
    pipeline << " | " << provider.str();
    if (!postGamma.empty()) {
        pipeline << " | " << shellQuote((request.toolsDirectory / "pfsgamma").string())
                 << " -g " << shellQuote(postGamma);
    }
    pipeline << " | " << shellQuote((request.toolsDirectory / "pfsout").string())
             << " " << shellQuote(request.outputPath.string());

    result.ok = true;
    result.displayCommand = pipeline.str();
    result.command = "zsh -lc " + shellQuote(result.displayCommand);
    return result;
}

} // namespace native
