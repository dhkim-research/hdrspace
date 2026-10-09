#include <boost/program_options.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/tokenizer.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <fstream>
#include <Eigen/LU>
#include <array>
#include "hdrmerge.h"
#include "camera_detect.h"

namespace po = boost::program_options;

namespace {

struct ColorSpaceSpec {
    bool raw = false;
    bool xyz = false;
    std::array<float, 6> primaries{};
    std::array<float, 2> white{};
};

ColorSpaceSpec parseColorSpaceSpec(const std::string &value) {
    std::string lowered = boost::to_lower_copy(value);
    ColorSpaceSpec spec;
    if (lowered == "raw" || lowered == "native") {
        spec.raw = true;
        return spec;
    }
    if (lowered == "xyz") {
        spec.xyz = true;
        return spec;
    }
    if (lowered == "rad") {
        spec.primaries = { 0.640f, 0.330f, 0.290f, 0.600f, 0.150f, 0.060f };
        spec.white = { 0.3333f, 0.3333f };
        return spec;
    }
    if (lowered == "srgb") {
        spec.primaries = { 0.6400f, 0.3300f, 0.3000f, 0.6000f, 0.1500f, 0.0600f };
        spec.white = { 0.3127f, 0.3290f };
        return spec;
    }
    throw std::runtime_error("Unsupported --colorspace argument (expected raw/native, xyz, rad, or srgb)");
}

Eigen::Matrix3f primariesToXYZ(const ColorSpaceSpec &spec) {
    if (spec.xyz)
        return Eigen::Matrix3f::Identity();

    Eigen::Matrix3f pxyz;
    for (int c = 0; c < 3; ++c) {
        float x = spec.primaries[2 * c + 0];
        float y = spec.primaries[2 * c + 1];
        float z = 1.0f - x - y;
        pxyz(0, c) = x;
        pxyz(1, c) = y;
        pxyz(2, c) = z;
    }

    Eigen::Vector3f wxyz;
    wxyz(0) = spec.white[0] / spec.white[1];
    wxyz(1) = 1.0f;
    wxyz(2) = (1.0f - spec.white[0] - spec.white[1]) / spec.white[1];

    Eigen::Vector3f scales = pxyz.inverse() * wxyz;
    Eigen::Matrix3f rgb_xyz = pxyz;
    for (int c = 0; c < 3; ++c)
        rgb_xyz.col(c) *= scales(c);
    return rgb_xyz;
}

float roundToNearestThirdStop(float value) {
    if (!(value > 0.0f))
        return value;
    return std::pow(2.0f, std::round(std::log2(value) * 3.0f) / 3.0f);
}

void applyPylinearEffectiveExposure(ExposureSeries &es, bool nominal,
        float shutterc, bool hasShutterCorrection,
        const std::map<float, float> &apertureOverrides,
        float exposureScale) {
    if (!(exposureScale > 0.0f))
        throw std::runtime_error("The --exposurescale argument must be greater than zero.");

    for (size_t i = 0; i < es.exposures.size(); ++i) {
        Exposure &exp = es.exposures[i];
        float etime = exp.exposure;
        float aperture = exp.aperture;

        if (!nominal) {
            etime = roundToNearestThirdStop(etime);
            if (hasShutterCorrection && etime > 0.0f) {
                float shutterRecip = 1.0f / etime;
                shutterRecip *= std::exp(shutterc * shutterRecip);
                if (shutterRecip > 0.0f)
                    etime = 1.0f / shutterRecip;
            }

            std::map<float, float>::const_iterator it = apertureOverrides.end();
            for (std::map<float, float>::const_iterator curr = apertureOverrides.begin();
                    curr != apertureOverrides.end(); ++curr) {
                float denom = std::max(std::abs(curr->first), 1e-6f);
                if (std::abs(curr->first - aperture) / denom < 1e-5f) {
                    it = curr;
                    break;
                }
            }
            if (it != apertureOverrides.end())
                aperture = it->second;
            else if (std::isfinite(aperture))
                aperture = std::pow(2.0f, std::round(std::log2(aperture * aperture) * 3.0f) / 6.0f);
        }

        if (!std::isfinite(aperture) || aperture <= 0.0f)
            aperture = 1.0f;
        if (!(etime > 0.0f))
            throw std::runtime_error("Encountered an invalid exposure time while computing effective exposures.");
        if (!(exp.iso > 0.0f))
            throw std::runtime_error("Encountered an invalid ISO value while computing effective exposures.");

        exp.exposure = (etime * exp.iso) / (100.0f * aperture * aperture * exposureScale);
    }
}

int parseRawChannelToken(const std::string &value) {
    std::string token = boost::to_lower_copy(boost::trim_copy(value));
    if (token == "r" || token == "red" || token == "0")
        return 0;
    if (token == "g" || token == "green" || token == "1")
        return 1;
    if (token == "b" || token == "blue" || token == "2")
        return 2;
    throw std::runtime_error("Unsupported sample channel (expected r, g, or b).");
}

std::vector<std::pair<int, int>> parseBadPixelText(const std::string &value) {
    std::vector<std::pair<int, int>> result;
    std::vector<std::string> tokens;
    boost::split(tokens, value, boost::is_any_of(" \t\r\n"), boost::token_compress_on);
    for (const std::string &tokenRaw : tokens) {
        std::string token = boost::trim_copy(tokenRaw);
        if (token.empty())
            continue;
        std::vector<std::string> xy;
        boost::split(xy, token, boost::is_any_of(","), boost::token_compress_off);
        if (xy.size() != 2)
            throw std::runtime_error("Unable to parse the 'badpixels' argument (expected x,y pairs).");
        try {
            result.emplace_back(boost::lexical_cast<int>(boost::trim_copy(xy[0])),
                boost::lexical_cast<int>(boost::trim_copy(xy[1])));
        } catch (const boost::bad_lexical_cast &) {
            throw std::runtime_error("Unable to parse the 'badpixels' argument (expected integer x,y pairs).");
        }
    }
    return result;
}

} // namespace

template <typename T> std::vector<T> parse_list(const po::variables_map &vm,
        const std::string &name, const std::vector<size_t> &nargs,
        const char *sepstr = " ,") {
    std::vector<T> result;

    if (!vm.count(name))
        return result;

    std::string argument = vm[name].as<std::string>();
    boost::char_separator<char> sep(sepstr);
    boost::tokenizer<boost::char_separator<char>> tokens(argument, sep);

    try {
        for (auto it = tokens.begin(); it != tokens.end(); ++it)
            result.push_back(boost::lexical_cast<T>(*it));
    } catch (const boost::bad_lexical_cast &) {
        throw std::runtime_error((boost::format("Unable to parse the '%1%' argument!") % name).str());
    }

    bool good = nargs.empty();
    std::ostringstream oss;
    for (size_t i=0; i<nargs.size(); ++i) {
        if (result.size() == nargs[i]) {
            good = true;
            break;
        }
        oss << nargs[i];
        if (i+1 < nargs.size())
            oss << " or ";
    }

    if (!good)
        throw std::runtime_error((boost::format("Unable to parse the '%1%'"
            " argument -- expected %2% values!") % name % oss.str()).str());

    return result;
}

void help(char **argv, const po::options_description &desc) {
    cout << "mergehdrcore" << endl
        << endl
        << "Syntax: " << argv[0] << " [options] <RAW file format string / list of multiple files>" << endl
        << endl
        << "Merge a RAW exposure sequence into a linear HDR image." << endl
        << endl
        << "Summary:"<< endl
        << "  This program takes an exposure series of DNG/CR2/.. RAW files and merges it" << endl
        << "  into a high dynamic-range EXR image. Given a printf-style format expression" << endl
        << "  for the input file names, the program automatically figures out both the" << endl
        << "  number of images and their exposure times. Any metadata (e.g. lens data)" << endl
        << "  present in the input RAW files is also copied over into the output EXR file." << endl
        << "  The program automatically checks for common mistakes like duplicate exposures," << endl
        << "  leaving autofocus or auto-ISO turned on by accident, and it can do useful " << endl
        << "  operations like cropping, resampling, and removing vignetting. Used with " << endl
        << "  just a single image, it works a lot like a hypothetical 'dcraw' in floating" << endl
        << "  point mode. OpenMP is used wherever possible to accelerate image processing." << endl
        << "  Note that this program makes the assumption that the input frames are well-" << endl
        << "  aligned so that no alignment correction is necessary." << endl
        << endl
        << "  The order of operations is as follows (all steps except 1 and 10 are" << endl
        << "  optional; brackets indicate steps that disabled by default):" << endl << endl
        << "    1. Load RAWs -> 2. HDR Merge -> 3. Demosaic -> 4. Transform colors -> " << endl
        << "    5. [White balance] -> 6. [Scale] -> 7. [Remove vignetting] -> 8. [Crop] -> " << endl
        << "    9. [Resample] -> 10. [Flip/rotate] -> 11. Write image" << endl
        << endl
        << "The following sections contain additional information on some of these steps." << endl
        << endl
        << "Step 1: Load RAWs" << endl
        << "  mergehdrcore reads RAW data through LibRaw. For simplicity, HDR" << endl
        << "  processing is currently restricted to sensors having a standard RGB" << endl
        << "  Bayer grid." << endl
        << endl
        << "Step 2: Merge" << endl
        << "  Exposures are merged based on a simple Poisson noise model. In other words," << endl
        << "  the exposures are simply summed together and divided by the total exposure." << endl
        << "  time. To avoid problems with over- and under-exposure, each pixel is" << endl
        << "  furthermore weighted such that only well-exposed pixels contribute to this" << endl
        << "  summation." << endl
        << endl
        << "  For this procedure, it is crucial that mergehdrcore knows the correct exposure" << endl
        << "  time for each image. Many cameras today use exposure values that are really" << endl
        << "  fractional powers of two rather than common rounded values (i.e. 1/32 as " << endl
        << "  opposed to 1/30 sec). mergehdrcore will try to retrieve the true exposure value" << endl
        << "  from the EXIF tag. Unfortunately, some cameras \"lie\" in their EXIF tags" << endl
        << "  and use yet another set of exposure times, which can seriously throw off" << endl
        << "  the HDR merging process. If your camera does this, pass the parameter " << endl
        << "  --fitexptimes to manually estimate the actual exposure times from the " << endl
        << "  input set of images." << endl
        << endl
        << "  A subtle issue that one should be aware of is that even professional-grade" << endl
        << "  lenses from the big two SLR manufactorers tend to have rather inaccurate " << endl
        << "  apertures. Take a photo sequence of a still scene at identical camera" << endl
        << "  settings, and you will notice that there is a perceptible amount of flicker" << endl
        << "  when turning it into a movie. This is because the aperture radius in each" << endl
        << "  shot may vary by a random amount that could be as large as 5%. This is not" << endl
        << "  not much of an issue if you're just doing video capture or still" << endl
        << "  photography, hence lens manufacturers don't correct for it. But it can" << endl
        << "  cause significant headaches in long capture sessions, where it introduces" << endl
        << "  random intensity scale factors from image to image. There are two" << endl
        << "  workarounds: 1. shoot wide open, or 2. use a trick used by time-lapse" << endl
        << "  photographers that is referred to as 'lens twist' or 'aperture twist'" << endl
        << "  (search for these keywords online to find videos that demonstrate" << endl
        << "  how it works)." << endl
        << endl
        << "Step 3: Demosaic" << endl
        << "  Demosaicing is done after HDR merging, on the merged floating point Bayer grid." << endl
        << endl
        << "Step 7: Vignetting correction" << endl
        << "  To remove vignetting from your photographs, take a single well-exposed " << endl
        << "  picture of a uniformly colored object. Ideally, take a picture through " << endl
        << "  the opening of an integrating sphere, if you have one. Then run mergehdrcore" << endl
        << "  on this picture using the --vcal parameter. This fits a radial polynomial" << endl
        << "  of the form 1 + ax^2 + bx^4 + cx^6 to the image and prints out the" << endl
        << "  coefficients. These can then be passed using the --vcorr parameter" << endl
        << endl
        << "Step 9: Resample" << endl
        << "  This program can do high quality Lanczos resampling to get lower resolution" << endl
        << "  output if desired. This can sometimes cause ringing on high frequency edges," << endl
        << "  in which case a tent filter may be preferable (selectable via --rfilter)." << endl
        << endl
        << desc << endl
        << "Options can also be stored in a local config file using key=value format." << endl
        << endl
        << "Examples:" << endl
        << "  Create an OpenEXR file from files specified in printf format." << endl
        << "    $ mergehdrcore --output scene.exr scene_%02i.cr2" << endl
        << endl
        << "  As above, but explicitly specify the files (in any order):" << endl
        << "    $ mergehdrcore --output scene.exr scene_001.cr2 scene_002.cr2 scene_003.cr2" << endl;
}

int main(int argc, char **argv) {
    po::options_description options("Command line options");
    po::options_description hidden_options("Hiden options");
    po::variables_map vm, vm_temp;

    options.add_options()
        ("help", "Print information on how to use this program\n")
        ("config", po::value<std::string>(),
            "Load the configuration file 'arg' as an additional source of command line parameters. "
            "Should contain one parameter per line in key=value format. The command line takes precedence "
            "when an argument is specified multiple times.\n")
        ("blacklevel", po::value<int>(),
            "Set the common RAW black baseline after per-frame LibRaw black correction.\n")
        ("whitepoint", po::value<int>(),
            "Override the sensor white point used to normalize RAW values.\n")
        ("saturation", po::value<float>(),
            "Saturation threshold of the sensor: the ratio of the sensor's theoretical dynamic "
            "range, at which saturation occurs in practice (in [0,1]). Estimated automatically if not specified.\n")
        ("range", po::value<float>()->default_value(0.0f),
            "Underexposure threshold in normalized sensor units for the neighborhood-aware merge mode.\n")
        ("fitexptimes", "On some cameras, the exposure times in the EXIF tags can't be trusted. Use "
            "this parameter to estimate them automatically for the current image sequence\n")
        ("exptimes", po::value<std::string>(),
            "Override the EXIF exposure times with a manually specified sequence of the "
            "format 'time1,time2,time3,..'\n")
        ("nodemosaic", "If specified, the raw Bayer grid is exported as a grayscale EXR file\n")
        ("rawgrid", "Export the merged Bayer grid as a sparse 3-channel raw image without interpolation\n")
        ("demosaic", po::value<EDemosaicMethod>()->default_value(EDemosaicAHD, "ahd"),
            "Demosaicing algorithm to use when demosaicing is enabled ('ahd' or 'dht')\n")
        ("mergestyle", po::value<EMergeMethod>()->default_value(EMergeHDRMerge, "hdrmerge"),
            "HDR merge style to use ('hdrmerge' or 'linearhdr').\n")
        ("linearhdrexposure", "Compute effective exposure times internally as etime * ISO / (100 * f^2 * scale).\n")
        ("pylinearexposure", "Backward-compatible alias for --linearhdrexposure.\n")
        ("exposurescale", po::value<float>()->default_value(1.0f),
            "Scale factor used together with --linearhdrexposure.\n")
        ("nominal", "Disable shutter/aperture correction when using --linearhdrexposure.\n")
        ("shutterc", po::value<float>(),
            "Optional shutter correction used with --linearhdrexposure.\n")
        ("fo", po::value<std::string>(),
            "Optional aperture override pairs nominal,exact nominal,exact ... used with --linearhdrexposure.\n")
        ("colormode", po::value<EColorMode>()->default_value(ESRGB, "sRGB"),
            "Output color space (one of 'native'/'sRGB'/'XYZ')\n")
        ("colorspace", po::value<std::string>(),
            "Output color space ('rad', 'srgb', 'xyz', or 'raw'/'native')\n")
        ("xyzcam", po::value<std::string>(),
            "Matrix that transforms from XYZ tristimulus values to camera RGB (row-major 3x3)\n")
        ("badpixels", po::value<std::string>(),
            "Repair the listed RAW bad pixels before merging. Format: x,y x,y ...\n")
        ("rawmultipliers", po::value<std::string>(),
            "Camera premultipliers applied to XYZCAM rows before computing Camera2RGB (r,g,b[,g2])\n")
        ("rgbcal", po::value<std::string>(),
            "Additional output RGB calibration multipliers applied after Camera2RGB (r,g,b)\n")
        ("sensor2xyz", po::value<std::string>(),
            "Matrix that transforms from the sensor color space to XYZ tristimulus values\n")
        ("cam2rgb", po::value<std::string>(),
            "Matrix that transforms from camera RGB to the target RGB color space\n")
        ("solid2ang", "Convert a demosaiced 180-degree equisolid fisheye image to equiangular before writing\n")
        ("demosaicfirst", "Demosaic every exposure (DHT) before merging and merge RGB as linearhdr -B "
            "(pylinearhdr --interpfirst). Requires --mergestyle linearhdr.\n")
        ("scale", po::value<float>(),
            "Optional scale factor that is applied to the image\n")
        ("crop", po::value<std::string>(),
            "Crop to a rectangular area. 'arg' should be specified in the form x,y,width,height\n")
        ("mergecrop", po::value<std::string>(),
            "Crop the RAW mosaic before HDR merging/demosaicing. 'arg' should be specified in the form x,y,width,height\n")
        ("resample", po::value<std::string>(),
            "Resample the image to a different resolution. 'arg' can be "
            "a pair of integers like 1188x790 or the max. resolution ("
            "maintaining the aspect ratio)\n")
        ("rfilter", po::value<std::string>()->default_value("lanczos"),
            "Resampling filter used by the --resample option (available choices: "
            "'tent' or 'lanczos')\n")
        ("wbalpatch", po::value<std::string>(),
            "White balance the image using a grey patch occupying the region "
            "'arg' (specified as x,y,width,height). Prints output suitable for --wbal\n")
        ("wbal", po::value<std::string>(),
            "White balance the image using floating point multipliers 'arg' "
            "specified as r,g,b\n")
        ("vcal", "Calibrate vignetting correction given a uniformly illuminated image\n")
        ("vcorr", po::value<std::string>(),
            "Apply the vignetting correction computed using --vcal\n")
        ("flip", po::value<std::string>()->default_value(""), "Flip the output image along the "
          "specified axes (one of 'x', 'y', or 'xy')\n")
        ("rotate", po::value<int>()->default_value(0), "Rotate the output image by 90, 180 or 270 degrees\n")
#if MERGEHDR_ENABLE_OPENEXR
        ("format", po::value<std::string>()->default_value("half"),
          "Choose the desired output file format -- one of 'half' (OpenEXR, 16 bit HDR / half precision), "
          "'single' (OpenEXR, 32 bit / single precision), 'hdr'/'rgbe' (Radiance RGBE), "
          "'jpeg' (libjpeg, 8 bit LDR for convenience)\n")
        ("output", po::value<std::string>()->default_value("output.exr"),
            "Name of the output file in OpenEXR format. When only a single RAW file is processed, its "
            "name is used by default (with the ending replaced by .exr/.jpeg");
#else
        ("format", po::value<std::string>()->default_value("hdr"),
          "Choose the desired output file format -- one of 'hdr'/'rgbe' (Radiance RGBE), "
          "'jpeg' (libjpeg, 8 bit LDR for convenience)\n")
        ("output", po::value<std::string>()->default_value("output.hdr"),
            "Name of the output file. When only a single RAW file is processed, its "
            "name is used by default (with the ending replaced by .hdr/.jpeg");
#endif

    hidden_options.add_options()
        ("headerline", po::value<std::vector<std::string>>(), "Extra HDR header line")
        ("samplechannel", po::value<std::string>(), "Compute raw channel sample statistics and exit")
        ("input-files", po::value<std::vector<std::string>>(), "Input files");

    po::options_description all_options;
    all_options.add(options).add(hidden_options);
    po::positional_options_description positional;
    positional.add("input-files", -1);

    try {
        /* Temporary command line parsing pass */
        po::store(po::command_line_parser(argc, argv)
            .options(all_options).positional(positional).run(), vm_temp);

        /* Is there a configuration file */
        std::string config = "hdrmerge.cfg";

        if (vm_temp.count("config"))
            config = vm_temp["config"].as<std::string>();

        if (fexists(config)) {
            std::ifstream settings(config, std::ifstream::in);
            po::store(po::parse_config_file(settings, all_options), vm);
            settings.close();
        }

        po::store(po::command_line_parser(argc, argv)
            .options(all_options).positional(positional).run(), vm);
        if (vm.count("help") || !vm.count("input-files")) {
            help(argv, options);
            return 0;
        }
        po::notify(vm);
    } catch (po::error &e) {
        cerr << "Error while parsing command line arguments: " << e.what() << endl << endl;
        help(argv, options);
        return -1;
    }

    try {
        EColorMode colormode = vm["colormode"].as<EColorMode>();
        EDemosaicMethod demosaic_method = vm["demosaic"].as<EDemosaicMethod>();
        EMergeMethod merge_method = vm["mergestyle"].as<EMergeMethod>();
        std::vector<int> wbalpatch      = parse_list<int>(vm, "wbalpatch", { 4 });
        std::vector<float> wbal         = parse_list<float>(vm, "wbal", { 3 });
        std::vector<int> resample       = parse_list<int>(vm, "resample", { 1, 2 }, ", x");
        std::vector<int> crop           = parse_list<int>(vm, "crop", { 4 });
        std::vector<int> mergecrop      = parse_list<int>(vm, "mergecrop", { 4 });
        std::vector<float> fo_v         = parse_list<float>(vm, "fo", { }, ", ");
        std::vector<float> xyzcam_v     = parse_list<float>(vm, "xyzcam", { 9 });
        std::vector<std::pair<int, int>> badpixels = vm.count("badpixels")
            ? parseBadPixelText(vm["badpixels"].as<std::string>())
            : std::vector<std::pair<int, int>>();
        std::vector<float> rawmult_v    = parse_list<float>(vm, "rawmultipliers", { 3, 4 });
        std::vector<float> rgbcal_v     = parse_list<float>(vm, "rgbcal", { 3 });
        std::vector<float> sensor2xyz_v = parse_list<float>(vm, "sensor2xyz", { 9 });
        std::vector<float> cam2rgb_v    = parse_list<float>(vm, "cam2rgb", { 9 });
        std::vector<float> vcorr        = parse_list<float>(vm, "vcorr", { 3 });
        std::vector<std::string> header_lines;
        if (vm.count("headerline"))
            header_lines = vm["headerline"].as<std::vector<std::string>>();

        if (!wbal.empty() && !wbalpatch.empty()) {
            cerr << "Cannot specify --wbal and --wbalpatch at the same time!" << endl;
            return -1;
        }
        if (!crop.empty() && !mergecrop.empty()) {
            cerr << "Cannot specify --crop and --mergecrop at the same time!" << endl;
            return -1;
        }
        if (!fo_v.empty() && (fo_v.size() % 2) != 0)
            throw std::runtime_error("Unable to parse the 'fo' argument -- expected nominal,exact aperture pairs.");

        std::map<float, float> aperture_overrides;
        for (size_t i = 0; i + 1 < fo_v.size(); i += 2)
            aperture_overrides[fo_v[i]] = fo_v[i + 1];

        float sensor2xyz[9] = {
            0.412453f, 0.357580f, 0.180423f,
            0.212671f, 0.715160f, 0.072169f,
            0.019334f, 0.119193f, 0.950227f
        };
        float cam2rgb[9] = {
            1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 1.0f
        };
        bool apply_cam2rgb = !cam2rgb_v.empty();
        bool pylinear_colorspace = vm.count("colorspace") || vm.count("xyzcam") || vm.count("rawmultipliers") || vm.count("rgbcal");
        std::array<float, 3> white_saturation_hint = { 1.0f, 1.0f, 1.0f };

        if (!sensor2xyz_v.empty()) {
            for (int i=0; i<9; ++i)
                sensor2xyz[i] = sensor2xyz_v[i];
        } else if (colormode != ENative) {
            cerr << "*******************************************************************************" << endl
                 << "Warning: no sensor2xyz matrix was specified -- this is necessary to get proper" << endl
                 << "sRGB / XYZ output. To acquire this matrix, convert any one of your RAW images" << endl
                 << "into a DNG file using Adobe's DNG converter on Windows / Mac (or on Linux," << endl
                 << "using the 'wine' emulator). The run" << endl
                 << endl
                 << "  $ exiv2 -pt the_image.dng 2> /dev/null | grep ColorMatrix2" << endl
                 << "  Exif.Image.ColorMatrix2 SRational 9  <sequence of ratios>" << endl
                 << endl
                 << "The sequence of rational numbers is a matrix in row-major order. Compute its" << endl
                 << "inverse using a tool like MATLAB or Octave and either pass it via" << endl
                 << "--sensor2xyz or add a matching entry to your config or profile:" << endl
                 << endl
                 << "# Sensor to XYZ color space transform (Canon EOS 50D)" << endl
                 << "sensor2xyz=1.933062 -0.1347 0.217175 0.880916 0.725958 -0.213945 0.089893 " << endl
                 << "-0.363462 1.579612" << endl
                 << endl
                 << "-> Providing output in the native sensor color space, as no matrix was given." << endl
                 << "*******************************************************************************" << endl
                 << endl;

            colormode = ENative;
        }

        if (apply_cam2rgb) {
            for (int i=0; i<9; ++i)
                cam2rgb[i] = cam2rgb_v[i];
        }

        std::vector<std::string> exposures = vm["input-files"].as<std::vector<std::string>>();

        if (pylinear_colorspace) {
            std::string colorspace_name = vm.count("colorspace")
                ? boost::to_lower_copy(vm["colorspace"].as<std::string>())
                : (colormode == ENative ? std::string("raw") : (colormode == EXYZ ? std::string("xyz") : std::string("srgb")));

            ColorSpaceSpec spec = parseColorSpaceSpec(colorspace_name);
            Eigen::Matrix3f xyzcam_m = Eigen::Matrix3f::Identity();
            bool have_xyzcam = false;

            if (!xyzcam_v.empty()) {
                have_xyzcam = true;
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        xyzcam_m(i, j) = xyzcam_v[3 * i + j];
            } else {
                CameraDetectInfo detected;
                std::string error;
                if (!detectXYZCamAuto(exposures[0], detected, &error))
                    throw std::runtime_error("Could not determine XYZCAM automatically" + (error.empty() ? std::string(".") : std::string(": ") + error));
                xyzcam_m = detected.xyzcam;
                have_xyzcam = true;
            }

            if (!rawmult_v.empty() && have_xyzcam) {
                for (int i = 0; i < 3; ++i)
                    xyzcam_m.row(i) *= rawmult_v[i];
            }

            colormode = ENative;
            apply_cam2rgb = false;

            if (have_xyzcam) {
                Eigen::Matrix3f sensor2xyz_m = xyzcam_m.inverse();
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        sensor2xyz[3 * i + j] = sensor2xyz_m(i, j);

                if (!spec.raw || !rgbcal_v.empty()) {
                    Eigen::Matrix3f cam2rgb_m = Eigen::Matrix3f::Identity();
                    if (spec.xyz) {
                        cam2rgb_m = sensor2xyz_m;
                    } else if (!spec.raw) {
                        Eigen::Matrix3f rgb_xyz = primariesToXYZ(spec);
                        Eigen::Matrix3f rgb_cam = xyzcam_m * rgb_xyz;
                        cam2rgb_m = rgb_cam.inverse();
                        Eigen::Vector3f wps = rgb_cam.rowwise().sum();
                        float max_wp = wps.maxCoeff();
                        if (max_wp > 0.0f) {
                            for (int i = 0; i < 3; ++i)
                                white_saturation_hint[i] = std::max(0.0f, wps(i) / max_wp);
                        }
                    }

                    if (!rgbcal_v.empty()) {
                        Eigen::Matrix3f rgbcal_m = Eigen::Matrix3f::Identity();
                        for (int i = 0; i < 3; ++i)
                            rgbcal_m(i, i) = rgbcal_v[i];
                        cam2rgb_m = rgbcal_m * cam2rgb_m;
                    }

                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            cam2rgb[3 * i + j] = cam2rgb_m(i, j);
                    apply_cam2rgb = true;
                }
            } else if (!rgbcal_v.empty()) {
                Eigen::Matrix3f rgbcal_m = Eigen::Matrix3f::Identity();
                for (int i = 0; i < 3; ++i)
                    rgbcal_m(i, i) = rgbcal_v[i];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        cam2rgb[3 * i + j] = rgbcal_m(i, j);
                apply_cam2rgb = true;
            }
        }

        float scale = 1.0f;
        if (vm.count("scale"))
            scale = vm["scale"].as<float>();

        /// Step 1: Load RAW
        ExposureSeries es;
        for (size_t i=0; i<exposures.size(); ++i)
            es.add(exposures[i]);
        es.badpixels = badpixels;
        const bool useLinearHdrExposure = vm.count("linearhdrexposure") || vm.count("pylinearexposure");
        es.check(useLinearHdrExposure);
        if (es.size() == 0)
            throw std::runtime_error("No input found / list of exposures to merge is empty!");

        std::vector<float> exptimes;
        std::map<float, float> exptimes_map;
        if (vm.count("exptimes")) {
            std::string value = vm["exptimes"].as<std::string>();

            if (value.find("->") == std::string::npos) {
                /* Normal list of exposure times, load directly */
                exptimes = parse_list<float>(vm, "exptimes", { es.size() });
            } else {
                /* Map of exposure time replacement values */
                std::vector<std::string> map_str = parse_list<std::string>(vm, "exptimes", { }, ",");
                for (size_t i=0; i<map_str.size(); ++i) {
                    std::vector<std::string> v;
                    boost::algorithm::iter_split(v, map_str[i], boost::algorithm::first_finder("->"));
                    if (v.size() != 2)
                        throw std::runtime_error("Unable to parse the 'exptimes' parameter");
                    try {
                        exptimes_map[boost::lexical_cast<float>(boost::trim_copy(v[0]))] = boost::lexical_cast<float>(boost::trim_copy(v[1]));
                    } catch (const boost::bad_lexical_cast &) {
                        throw std::runtime_error("Unable to parse the 'exptimes' argument!");
                    }
                }
            }
        }
        es.load(vm.count("blacklevel") ? vm["blacklevel"].as<int>() : -1);
        if (!badpixels.empty())
            es.repairBadPixels();

        if (pylinear_colorspace) {
            for (int i = 0; i < 3; ++i)
                es.white_saturation[i] = white_saturation_hint[i];
        } else if (apply_cam2rgb) {
            Eigen::Matrix3f cam2rgb_m;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    cam2rgb_m(i, j) = cam2rgb[3 * i + j];
            Eigen::Matrix3f rgb_cam = cam2rgb_m.inverse();
            float maxsum = 0.0f;
            for (int i = 0; i < 3; ++i) {
                float rowsum = rgb_cam(i, 0) + rgb_cam(i, 1) + rgb_cam(i, 2);
                es.white_saturation[i] = rowsum;
                maxsum = std::max(maxsum, rowsum);
            }
            if (maxsum > 0.0f) {
                for (int i = 0; i < 3; ++i)
                    es.white_saturation[i] = std::max(0.0f, es.white_saturation[i] / maxsum);
            } else {
                es.white_saturation[0] = es.white_saturation[1] = es.white_saturation[2] = 1.0f;
            }
        }

        if (vm.count("blacklevel")) {
            cout << "Using common black baseline: " << es.blacklevel << endl;
        }

        if (vm.count("whitepoint")) {
            es.whitepoint = vm["whitepoint"].as<int>();
            cout << "Overriding white point: " << es.whitepoint << endl;
        }

        if (es.whitepoint <= es.blacklevel)
            throw std::runtime_error("The sensor white point must be greater than the black level!");

        /* linearhdr passes the premultipliers to rawconvert (-r), so LibRaw scales the RAW data
           itself; the XYZCAM rows above carry the same multipliers. */
        if (!rawmult_v.empty()) {
            const float multipliers[3] = { rawmult_v[0], rawmult_v[1], rawmult_v[2] };
            es.applyRawMultipliers(multipliers);
        }

        if (!mergecrop.empty())
            es.crop_raw(mergecrop[0], mergecrop[1], mergecrop[2], mergecrop[3]);

        /// Precompute relative exposure + weight tables
        float saturation = 0;
        if (vm.count("saturation"))
            saturation = vm["saturation"].as<float>();
        es.initTables(saturation, merge_method, vm["range"].as<float>());

        if (!exptimes.empty()) {
            cout << "Overriding exposure times: [";

            for (size_t i=0; i<exptimes.size(); ++i) {
                cout << es.exposures[i].toString() << "->" << exptimes[i];
                es.exposures[i].exposure = exptimes[i];
                if (i+1 < exptimes.size())
                    cout << ", ";
            }
            cout << "]" << endl;
        }

        if (!exptimes_map.empty()) {
            cout << "Overriding exposure times: [";
            for (size_t i=0; i<es.exposures.size(); ++i) {
                float from = es.exposures[i].exposure, to = 0;
                for (std::map<float, float>::const_iterator it = exptimes_map.begin(); it != exptimes_map.end(); ++it) {
                    if (std::abs((it->first - from) / from) < 1e-5f) {
                        if (to != 0)
                            throw std::runtime_error("Internal error!");
                        to = it->second;
                    }
                }
                if (to == 0)
                    throw std::runtime_error((boost::format("Specified an exposure time replacement map, but couldn't find an entry for %1%") % from).str());

                cout << es.exposures[i].toString() << "->" << to;
                if (i+1 < es.exposures.size())
                    cout << ", ";
                es.exposures[i].exposure = to;
            }
            cout << "]" << endl;
        }


        if (vm.count("fitexptimes")) {
            es.fitExposureTimes();
            if (vm.count("exptimes"))
                cerr << "Note: you specified --exptimes and --fitexptimes at the same time. The" << endl
                     << "The test file exptime_showfit.m now compares these two sets of exposure" << endl
                     << "times, rather than the fit vs EXIF." << endl << endl;
        }

        if ((vm.count("linearhdrexposure") || vm.count("pylinearexposure")) && !vm.count("exptimes")) {
            applyPylinearEffectiveExposure(
                es,
                vm.count("nominal") != 0,
                vm.count("shutterc") ? vm["shutterc"].as<float>() : 0.0f,
                vm.count("shutterc") != 0,
                aperture_overrides,
                vm["exposurescale"].as<float>());
        }

        if (vm.count("samplechannel")) {
            int sample_channel = parseRawChannelToken(vm["samplechannel"].as<std::string>());
            RawChannelSampleStats stats = es.sampleRawChannel(sample_channel);
            cout << (boost::format("RAW_SAMPLE channel=%1% avg=%2$.12g fraction=%3$.12g valid=%4% total=%5%")
                % sample_channel % stats.average % stats.fraction % stats.validSamples % stats.totalSamples) << endl;
            for (size_t i = 0; i < es.exposures.size(); ++i)
                es.exposures[i].release();
            return 0;
        }

        bool rawgrid = vm.count("rawgrid") != 0;
        bool demosaic = vm.count("nodemosaic") == 0 && !rawgrid;
        bool rgb_output = demosaic || rawgrid;
        const bool demosaic_first = vm.count("demosaicfirst") != 0;
        if (demosaic_first) {
            /// Steps 1+3 in the other order: demosaic each exposure, then merge (pylinearhdr --interpfirst)
            if (!demosaic)
                throw std::runtime_error("--demosaicfirst cannot be combined with --rawgrid or --nodemosaic.");
            if (merge_method != EMergePyLinear)
                throw std::runtime_error("--demosaicfirst requires --mergestyle linearhdr.");
            if (demosaic_method != EDemosaicDHT)
                throw std::runtime_error("--demosaicfirst uses DHT (as pylinearhdr --interpfirst); pass --demosaic dht.");
            const float identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
            es.mergeDemosaicFirst(apply_cam2rgb ? cam2rgb : identity, false);
        } else {
            /// Step 1: HDR merge
            es.merge();

            /// Step 3: Demosaicing / raw-grid expansion
            if (demosaic)
                es.demosaic(sensor2xyz, demosaic_method);
            else if (rawgrid)
                es.rawgrid();
        }

        /// Step 4: Transform colors
        if (colormode != ENative) {
            if (!demosaic) {
                cerr << "Warning: you requested XYZ/sRGB output, but demosaicing was explicitly disabled! " << endl
                     << "Color processing is not supported in this case -- writing raw sensor colors instead." << endl;
            } else {
                es.transform_color(sensor2xyz, colormode == EXYZ);
            }
        }

        if (apply_cam2rgb) {
            if (!demosaic)
                throw std::runtime_error("The --cam2rgb option requires demosaicing.");
            es.transform_color_matrix(cam2rgb, "target RGB");
        }

        /// Step 5: White balancing
        if (!wbal.empty()) {
            float scale[3] = { wbal[0], wbal[1], wbal[2] };
            es.whitebalance(scale);
        } else if (wbalpatch.size()) {
            es.whitebalance(wbalpatch[0], wbalpatch[1], wbalpatch[2], wbalpatch[3]);
        }

        /// Step 6: Scale
        if (scale != 1.0f)
            es.scale(scale);

        /// Step 7: Remove vignetting
        if (vm.count("vcal")) {
            if (vm.count("vcorr")) {
                cerr << "Warning: only one of --vcal and --vcorr can be specified at a time. Ignoring --vcorr" << endl;
            }

            if (demosaic)
                es.vcal();
            else
                cerr << "Warning: Vignetting correction requires demosaicing. Ignoring.." << endl;
        } else if (!vcorr.empty()) {
            if (demosaic)
                es.vcorr(vcorr[0], vcorr[1], vcorr[2]);
            else
                cerr << "Warning: Vignetting correction requires demosaicing. Ignoring.." << endl;
        }

        /// Step 8: Crop
        if (!crop.empty())
            es.crop(crop[0], crop[1], crop[2], crop[3]);

        /// Step 9: Resample
        if (!resample.empty()) {
            int w, h;

            if (resample.size() == 1) {
                float factor = resample[0] / (float) std::max(es.width, es.height);
                w = (int) std::round(factor * es.width);
                h = (int) std::round(factor * es.height);
            } else {
                w = resample[0];
                h = resample[1];
            }

            if (demosaic) {
                std::string rfilter = boost::to_lower_copy(vm["rfilter"].as<std::string>());
                if (rfilter == "lanczos") {
                    es.resample(LanczosSincFilter(), w, h);
                } else if (rfilter == "tent") {
                    es.resample(TentFilter(), w, h);
                } else {
                    cout << "Invalid resampling filter chosen (must be 'lanczos' / 'tent')" << endl;
                    return -1;
                }
            } else {
                cout << "Warning: resampling a non-demosaiced image does not make much sense -- ignoring." << endl;
            }
        }

        /// Step 10: Flip / rotate
        ERotateFlipType flipType = flipTypeFromString(
            vm["rotate"].as<int>(), vm["flip"].as<std::string>());

        if (flipType != ERotateNoneFlipNone) {
            uint8_t *t_buf;
            size_t t_width, t_height;

            if (rgb_output) {
                rotateFlip((uint8_t *) es.image_demosaiced, es.width, es.height,
                    t_buf, t_width, t_height, 3*sizeof(float), flipType);
                delete[] es.image_demosaiced;
                es.image_demosaiced = (float3 *) t_buf;
                es.width = t_width;
                es.height = t_height;
            }
        }

        /// Step 10.5: Optional equisolid -> equiangular reprojection
        if (vm.count("solid2ang")) {
            if (demosaic)
                es.solid2ang();
            else
                cerr << "Warning: solid2ang requires demosaicing. Ignoring.." << endl;
        }

        /// Step 11: Write output
        std::string output = vm["output"].as<std::string>();
        std::string format = boost::to_lower_copy(vm["format"].as<std::string>());

        if (vm["output"].defaulted() && exposures.size() == 1 && exposures[0].find("%") == std::string::npos) {
            std::string fname = exposures[0];
            size_t spos = fname.find_last_of(".");
            if (spos != std::string::npos)
                output = fname.substr(0, spos)
#if MERGEHDR_ENABLE_OPENEXR
                    + ".exr";
#else
                    + ".hdr";
#endif
        }

        if (format == "jpg")
            format = "jpeg";

#if !MERGEHDR_ENABLE_OPENEXR
        if (format == "half" || format == "single")
            throw std::runtime_error("This mergehdr build was compiled without OpenEXR support. Use --format hdr or --format jpeg.");
#endif

        if ((format == "hdr" || format == "rgbe") && boost::ends_with(output,  ".exr"))
            output = output.substr(0, output.length()-4) + ".hdr";

        if (format == "jpeg" && boost::ends_with(output,  ".exr"))
            output = output.substr(0, output.length()-4) + ".jpg";

        if (rgb_output) {
            if (format == "half" || format == "single")
                writeOpenEXR(output, es.width, es.height, 3,
                    (float *) es.image_demosaiced, es.metadata, format == "half");
            else if (format == "hdr" || format == "rgbe")
                writeRGBE(output, es.width, es.height, (float *) es.image_demosaiced, header_lines, true);
            else if (format == "jpeg")
                if (rawgrid)
                    throw std::runtime_error("Tried to export the raw grid as a JPEG image -- this is not allowed.");
                else
                    writeJPEG(output, es.width, es.height, (float *) es.image_demosaiced);
            else
                throw std::runtime_error("Unsupported --format argument");
        } else {
            if (format == "half" || format == "single")
                writeOpenEXR(output, es.width, es.height, 1,
                    (float *) es.image_merged, es.metadata, format == "half");
            else if (format == "jpeg")
                throw std::runtime_error("Tried to export the raw Bayer grid "
                    "as a JPEG image -- this is not allowed.");
            else
                throw std::runtime_error("Unsupported --format argument");
        }
    } catch (const std::exception &ex) {
        cerr << "Encountered a fatal error: " << ex.what() << endl;
        return -1;
    }

    return 0;
}
