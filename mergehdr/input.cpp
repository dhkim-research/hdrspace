#include "mergehdrcore.h"
#include "camera_detect.h"
#include "raw_normalization.h"
#include "raw_decode.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <sys/stat.h>
#include <boost/format.hpp>

#include <libraw/libraw.h>

#include <exiv2/image.hpp>
#include <exiv2/easyaccess.hpp>

/**
 * Find all images in an exposure series and check that some sensible
 * base requirements are satisfied, i.e.
 *  - all images use the same ISO speed and aperture setting
 *  - the images were taken using the same camera settings
 */
void ExposureSeries::add(const std::string &fmt) {
    bool success = false;

    for (int exposure = 0; ; ++exposure) {
        char filename[1024];
        snprintf(filename, sizeof(filename), fmt.c_str(), exposure);
        Exposure exp(filename);

        if (access(filename, F_OK) != 0)
            break;

        if (exposure == 1 && strchr(fmt.c_str(), '%') == NULL)
            break; /* Just one image -- stop */

        success = true;
        exposures.push_back(exp);
    }

    if (!success) {
        /* Maybe the sequence starts at 1? */
        for (int exposure = 1; ; ++exposure) {
            char filename[1024];
            snprintf(filename, sizeof(filename), fmt.c_str(), exposure);
            Exposure exp(filename);

            if (access(filename, F_OK) != 0)
                break;

            exposures.push_back(exp);
        }
    }
}

float exposureTime(float shutterSpeedValue) {
    /* lifted from libexiv2 */
    double tmp = std::exp(std::log(2.0) * shutterSpeedValue);
    if (tmp > 1)
        return 1.0f / ((int) (tmp + 0.5));
    else
        return (int) (1/tmp + 0.5);
}

void ExposureSeries::check(bool allowVariableIso) {
    float isoSpeed = -1, aperture = -1;
    bool warnedVariableIso = false, warnedVariableAperture = false;

    for (size_t exposure=0; exposure<exposures.size(); ++exposure) {
        Exposure &exp = exposures[exposure];

        Exiv2::Image::UniquePtr image = Exiv2::ImageFactory::open(exp.filename);
        if (image.get() == 0)
            throw std::runtime_error("\"" + exp.filename + "\": could not open RAW file!");
        image->readMetadata();

        const Exiv2::ExifData &exifData = image->exifData();

        Exiv2::ExifData::const_iterator it;
        for (it = exifData.begin(); it != exifData.end(); ++it) {
            std::string value = it->toString();
            if (value.length() > 100) /* Ignore huge attributes */
                continue;
            /* Collect the remainder */
            if (metadata.find(it->key()) != metadata.end()) {
                std::string current = metadata[it->key()];
                if (value == current)
                    continue;
                metadata[it->key()] = current + std::string("; ") + value;
            } else {
                metadata[it->key()] = value;
            }
        }

        it = exifData.findKey(Exiv2::ExifKey("Exif.Photo.ShutterSpeedValue"));
        if (it != exifData.end()) {
            exp.exposure = std::pow(2, -it->toFloat());
        } else {
            it = exifData.findKey(Exiv2::ExifKey("Exif.Photo.ExposureTime"));
            if (it == exifData.end())
                throw std::runtime_error("\"" + exp.filename + "\": could not extract the exposure time!");
            exp.exposure = it->toFloat();
        }

        it = Exiv2::exposureTime(exifData);
        if (it == exifData.end())
            throw std::runtime_error("\"" + exp.filename + "\": could not extract the exposure time!");
        exp.shown_exposure = it->toFloat();

        /* Fail if the images use different ISO values */
        it = Exiv2::isoSpeed(exifData);
        if (it == exifData.end())
            throw std::runtime_error("\"" + exp.filename + "\": could not extract the ISO speed!");
        float currentIso = it->toFloat();
        exp.iso = currentIso;
        if (exposure == 0)
            isoSpeed = currentIso;
        else if (isoSpeed != currentIso) {
            if (!allowVariableIso)
                throw std::runtime_error("\"" + exp.filename + "\": detected an ISO speed that is different from the other images!");
            if (!warnedVariableIso) {
                std::cerr << "Warning: ISO values differ across the bracket; using effective exposure times that include ISO." << std::endl;
                warnedVariableIso = true;
            }
        }

        /* Different apertures are only allowed with effective exposures
           (t * ISO / (100 N^2) per frame), which account for them */
        it = Exiv2::fNumber(exifData);
        if (it == exifData.end())
            throw std::runtime_error("\"" + exp.filename + "\": could not extract the aperture setting!");
        float currentAperture = it->toFloat();
        exp.aperture = currentAperture;
        if (exposure == 0)
            aperture = currentAperture;
        else if (aperture != currentAperture) {
            if (!allowVariableIso)
                throw std::runtime_error("\"" + exp.filename + "\": detected an aperture setting that is different from the other images!");
            if (!warnedVariableAperture) {
                std::cerr << "Warning: aperture values differ across the bracket; using effective exposures that include the aperture." << std::endl;
                warnedVariableAperture = true;
            }
        }

        /* Plain EXIF exposure times also need the exposure mode metadata */
        if (!allowVariableIso) {
            it = Exiv2::exposureMode(exifData);
            if (it == exifData.end())
                throw std::runtime_error("\"" + exp.filename + "\": could not extract the exposure mode!");
        }
    }

    std::sort(exposures.begin(), exposures.end(),
        [](const Exposure &a, const Exposure &b) {
            return a.exposure < b.exposure;
        }
    );

    cout << "Found " << exposures.size() << " image" <<
        (exposures.size() > 1 ? "s" : "");
    cout  << " [ISO " << isoSpeed << ", ";

    if (warnedVariableAperture)
        cout << "f/variable";
    else if (aperture == 0)
        cout << "f/unknown";
    else
        cout << "f/" << aperture;

    cout << ", exposures times: ";
    for (size_t i=0; i<exposures.size(); ++i) {
        cout << exposures[i].toString();
        if (i+1 < exposures.size())
            cout << ", ";
    }

    cout << "]" << endl;

    cout << "Collected " << metadata.size() << " metadata entries." << endl;
}

bool fexists(const std::string& name) {
    struct stat buffer;
    return stat(name.c_str(), &buffer) == 0;
}

void ExposureSeries::load(int commonBlackOverride, int fixedWhiteOverride) {
    // With a profile (black and white given): one fixed black and white for every frame;
    // LibRaw's own levels are only checked.
    const bool fixedBlack = commonBlackOverride >= 0;
    const bool fixedLevels = fixedBlack && fixedWhiteOverride >= 0;
    if (fixedLevels && (fixedWhiteOverride <= commonBlackOverride || fixedWhiteOverride > 65535))
        throw std::runtime_error("The fixed RAW white point must exceed the black level and fit in 16 bits.");
    cout << "Loading raw image data ..";
    cout.flush();

    if (exposures.empty()) {
        cout << " done (0x0, using 0 MiB of memory)" << endl;
        return;
    }

    std::vector<DecodedRawFrame> decoded(exposures.size());
    std::vector<std::string> errors(exposures.size());

    DecodedRawFrame first = decodeWithLibRaw(exposures[0].filename, commonBlackOverride);
    this->width = first.width;
    this->height = first.height;
    this->blacklevel = commonBlackOverride >= 0 ? commonBlackOverride : first.blacklevel;
    this->whitepoint = first.whitepoint;
    this->filter = first.filter;
    decoded[0] = std::move(first);

    cout << ".";
    cout.flush();

    #pragma omp parallel for schedule(dynamic, 1)
    for (int i=1; i<(int) exposures.size(); ++i) {
        try {
            decoded[i] = decodeWithLibRaw(exposures[i].filename, commonBlackOverride);
        } catch (const std::exception &e) {
            errors[i] = e.what();
        }

        #pragma omp critical
        {
            cout << ".";
            cout.flush();
        }
    }

    for (size_t i = 0; i < exposures.size(); ++i) {
        if (!errors[i].empty())
            throw std::runtime_error(errors[i]);
        if (decoded[i].width != this->width || decoded[i].height != this->height)
            throw std::runtime_error((boost::format("\"%1%\": detected a RAW frame size that differs from the other images")
                % exposures[i].filename).str());
        if (decoded[i].filter != this->filter)
            throw std::runtime_error((boost::format("\"%1%\": detected a CFA pattern that differs from the other images")
                % exposures[i].filename).str());
    }

    // Detect decoder scale mismatches per frame, never against the first frame's
    // black/white pair: ISO and RAW bit depth can change within one bracket.
    // With fixed levels every frame uses the profile white.
    if (fixedLevels) {
        for (DecodedRawFrame &frame : decoded)
            frame.whitepoint = fixedWhiteOverride;
        this->whitepoint = fixedWhiteOverride;
    }
    for (size_t i = 0; i < decoded.size() && !fixedLevels; ++i) {
        DecodedRawFrame &frame = decoded[i];
        const uint16_t *image = frame.image.get();
        const size_t pixelCount = decoded[i].width * decoded[i].height;
        int observedMaximum = 0;
        size_t samplesAboveMismatch = 0;
        const int mismatchThreshold = std::min(65535,
            frame.blacklevel + 2 * (frame.whitepoint - frame.blacklevel));
        for (size_t j = 0; j < pixelCount; ++j) {
            int value = image[j];
            observedMaximum = std::max(observedMaximum, value);
            if (value > mismatchThreshold)
                ++samplesAboveMismatch;
        }
        frame.whitepoint = adjustRawWhitepointForObservedScale(frame.blacklevel,
            frame.whitepoint, observedMaximum, samplesAboveMismatch, pixelCount);
        this->whitepoint = std::max(this->whitepoint, frame.whitepoint);
    }

    if (this->whitepoint <= this->blacklevel)
        throw std::runtime_error("The common RAW white point must exceed the black baseline.");

    // Fixed levels: every frame already has the common black/white, so this only clips
    // at the white point.
    // Otherwise: each frame's LibRaw black/white is moved to the common full scale, so
    // every frame is scaled to its own maximum.
    std::ostringstream frameLevels;
    for (size_t i = 0; i < decoded.size(); ++i) {
        DecodedRawFrame &frame = decoded[i];
        uint16_t *image = frame.image.get();
        const size_t pixelCount = frame.width * frame.height;
        #pragma omp parallel for
        for (size_t j = 0; j < pixelCount; ++j)
            image[j] = normalizeRawSample(image[j], frame.blacklevel, frame.whitepoint,
                this->blacklevel, this->whitepoint);
        if (i) frameLevels << "; ";
        frameLevels << exposures[i].filename << ":" << frame.blacklevel << "," << frame.whitepoint;
    }
    if (fixedLevels)
        metadata["RAW_NORMALIZATION"] = "Fixed profile black/white on every frame; LibRaw levels checked only";
    else if (fixedBlack)
        metadata["RAW_NORMALIZATION"] = "Fixed profile black; per-frame LibRaw white to common full scale";
    else
        metadata["RAW_NORMALIZATION"] = "Per-frame LibRaw black/white to common full scale; saturation preserved";
    metadata["RAW_FRAME_LEVELS"] = frameLevels.str();

    // Reproducibility check: record LibRaw's own levels and warn when they disagree
    // with the fixed black or between frames (mixed ISO or bit depth).
    std::vector<std::string> names;
    std::vector<int> decoderBlack, decoderWhite;
    std::ostringstream decoderLevels;
    for (size_t i = 0; i < decoded.size(); ++i) {
        names.push_back(exposures[i].filename);
        decoderBlack.push_back(decoded[i].decoderBlack);
        decoderWhite.push_back(decoded[i].decoderWhite);
        if (i) decoderLevels << "; ";
        decoderLevels << exposures[i].filename << ":" << decoded[i].decoderBlack << "," << decoded[i].decoderWhite;
    }
    metadata["RAW_DECODER_LEVELS"] = decoderLevels.str();
    const std::vector<std::string> warnings = rawLevelWarnings(names, decoderBlack, decoderWhite, commonBlackOverride);
    if (!warnings.empty()) {
        std::ostringstream joined;
        for (size_t i = 0; i < warnings.size(); ++i) {
            std::cerr << std::endl << "WARNING: " << warnings[i];
            if (i) joined << "; ";
            joined << warnings[i];
        }
        std::cerr << std::endl;
        metadata["RAW_LEVEL_WARNINGS"] = joined.str();
    }
    metadata["RAW_COMMON_BLACK"] = std::to_string(this->blacklevel);
    metadata["RAW_COMMON_FULL_SCALE_WHITE"] = std::to_string(this->whitepoint);

    for (size_t i = 0; i < exposures.size(); ++i) {
        exposures[i].image = decoded[i].image.release();
    }

    cout << " done (" << width << "x" << height << ", using "
         << (width*height*sizeof(uint16_t) * exposures.size()) / (float) (1024*1024)
         << " MiB of memory)" << endl;
}
