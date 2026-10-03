#include "hdrmerge.h"
#include "camera_detect.h"

#include <algorithm>
#include <cstdint>
#include <sys/stat.h>
#include <boost/format.hpp>

#include <libraw/libraw.h>

#include <exiv2/image.hpp>
#include <exiv2/easyaccess.hpp>

struct DecodedRawFrame {
    size_t width;
    size_t height;
    int blacklevel;
    int whitepoint;
    int filter;
    std::unique_ptr<uint16_t[]> image;
};

static int buildDcrawFilter(LibRaw &raw, int top, int left) {
    int filter = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 2; ++x) {
            int color = raw.COLOR(top + y, left + x);
            filter |= color << (((y << 1 & 14) + (x & 1)) << 1);
        }
    }
    return filter;
}

static DecodedRawFrame decodeWithLibRaw(const std::string &filename) {
    LibRaw raw;

    int err = raw.open_file(filename.c_str());
    if (err != LIBRAW_SUCCESS)
        throw std::runtime_error((boost::format("\"%1%\": LibRaw open_file failed: %2%")
            % filename % libraw_strerror(err)).str());

    err = raw.unpack();
    if (err != LIBRAW_SUCCESS)
        throw std::runtime_error((boost::format("\"%1%\": LibRaw unpack failed: %2%")
            % filename % libraw_strerror(err)).str());

    if (!raw.imgdata.rawdata.raw_image)
        throw std::runtime_error((boost::format("\"%1%\": LibRaw did not return a flat Bayer raw_image")
            % filename).str());

    const libraw_image_sizes_t &sizes = raw.imgdata.sizes;
    const libraw_colordata_t &color = raw.imgdata.color;

    int left = sizes.left_margin;
    int top = sizes.top_margin;
    size_t width = sizes.iwidth ? sizes.iwidth : sizes.width;
    size_t height = sizes.iheight ? sizes.iheight : sizes.height;

    if (width == 0 || height == 0)
        throw std::runtime_error((boost::format("\"%1%\": LibRaw reported an empty visible image")
            % filename).str());

    int blacklevel = effectiveRawBlackLevel(
        color.black, color.cblack, sizeof(color.cblack) / sizeof(color.cblack[0]));
    const int sourceWhitepoint = color.maximum ? static_cast<int>(color.maximum) : 0x3FFF;
    const int filter = buildDcrawFilter(raw, top, left);

    err = raw.raw2image();
    if (err != LIBRAW_SUCCESS)
        throw std::runtime_error((boost::format("\"%1%\": LibRaw raw2image failed: %2%")
            % filename % libraw_strerror(err)).str());
    err = raw.subtract_black();
    if (err != LIBRAW_SUCCESS)
        throw std::runtime_error((boost::format("\"%1%\": LibRaw subtract_black failed: %2%")
            % filename % libraw_strerror(err)).str());

    const libraw_image_sizes_t &normalizedSizes = raw.imgdata.sizes;
    const size_t normalizedWidth = normalizedSizes.iwidth ? normalizedSizes.iwidth : normalizedSizes.width;
    const size_t normalizedHeight = normalizedSizes.iheight ? normalizedSizes.iheight : normalizedSizes.height;
    if (normalizedWidth != width || normalizedHeight != height || !raw.imgdata.image)
        throw std::runtime_error((boost::format("\"%1%\": LibRaw black-normalized image geometry changed unexpectedly")
            % filename).str());

    const int usableWhitepoint = raw.imgdata.color.maximum
        ? static_cast<int>(raw.imgdata.color.maximum)
        : std::max(1, sourceWhitepoint - blacklevel);
    const int whitepoint = std::min(65535, blacklevel + usableWhitepoint);
    std::unique_ptr<uint16_t[]> image(new uint16_t[width * height]);

    // raw2image() lays the flat Bayer signal into the channel selected by
    // fcol(). subtract_black() then applies LibRaw's full channel and repeated
    // BlackLevel tables. Restore one scalar baseline so the unchanged merge
    // code can continue to normalize (value-black)/(white-black).
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const int channel = raw.fcol(static_cast<int>(y), static_cast<int>(x));
            if (channel < 0 || channel > 3)
                throw std::runtime_error((boost::format("\"%1%\": unsupported non-Bayer color layout")
                    % filename).str());
            const unsigned corrected = raw.imgdata.image[y * width + x][channel];
            image[y * width + x] = static_cast<uint16_t>(
                std::min<unsigned>(65535, corrected + static_cast<unsigned>(blacklevel)));
        }
    }

    DecodedRawFrame frame;
    frame.width = width;
    frame.height = height;
    frame.blacklevel = blacklevel;
    frame.whitepoint = whitepoint;
    frame.filter = filter;
    frame.image = std::move(image);
    return frame;
}

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

        /* Different aperture settings are only allowed with linearhdr effective exposures
           (t * ISO / (100 N^2) per frame), which account for them like linearhdr does */
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

        /* hdrmerge also requires exposure mode metadata; linearhdr does not */
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

void ExposureSeries::load() {
    cout << "Loading raw image data ..";
    cout.flush();

    if (exposures.empty()) {
        cout << " done (0x0, using 0 MiB of memory)" << endl;
        return;
    }

    std::vector<DecodedRawFrame> decoded(exposures.size());
    std::vector<std::string> errors(exposures.size());

    DecodedRawFrame first = decodeWithLibRaw(exposures[0].filename);
    this->width = first.width;
    this->height = first.height;
    this->blacklevel = first.blacklevel;
    this->whitepoint = first.whitepoint;
    this->filter = first.filter;
    decoded[0] = std::move(first);

    cout << ".";
    cout.flush();

    #pragma omp parallel for schedule(dynamic, 1)
    for (int i=1; i<(int) exposures.size(); ++i) {
        try {
            decoded[i] = decodeWithLibRaw(exposures[i].filename);
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

    /* Every frame had its own LibRaw black level (black + per-channel cblack, measured per
       frame) subtracted in decodeWithLibRaw() and its own scalar baseline added back. Re-base
       all frames to one common baseline, so that normalizing with that baseline removes each
       frame's own black exactly, as linearhdr's rawconvert does per frame. */
    for (size_t i = 0; i < decoded.size(); ++i) {
        const int delta = decoded[i].blacklevel - this->blacklevel;
        if (delta == 0)
            continue;
        uint16_t *image = decoded[i].image.get();
        const size_t pixelCount = decoded[i].width * decoded[i].height;
        for (size_t j = 0; j < pixelCount; ++j)
            image[j] = static_cast<uint16_t>(std::clamp(static_cast<int>(image[j]) - delta, 0, 65535));
    }

    int observedMaximum = 0;
    size_t samplesAboveMismatch = 0;
    size_t totalSamples = 0;
    const int range = this->whitepoint - this->blacklevel;
    const int mismatchThreshold = range > 0
        ? std::min(65535, this->blacklevel + 2 * range)
        : 65535;
    for (size_t i = 0; i < decoded.size(); ++i) {
        const uint16_t *image = decoded[i].image.get();
        const size_t pixelCount = decoded[i].width * decoded[i].height;
        totalSamples += pixelCount;
        for (size_t j = 0; j < pixelCount; ++j) {
            int value = image[j];
            observedMaximum = std::max(observedMaximum, value);
            if (value > mismatchThreshold)
                ++samplesAboveMismatch;
        }
    }
    this->whitepoint = adjustRawWhitepointForObservedScale(
        this->blacklevel,
        this->whitepoint,
        observedMaximum,
        samplesAboveMismatch,
        totalSamples);

    for (size_t i = 0; i < exposures.size(); ++i) {
        exposures[i].image = decoded[i].image.release();
    }

    cout << " done (" << width << "x" << height << ", using "
         << (width*height*sizeof(uint16_t) * exposures.size()) / (float) (1024*1024)
         << " MiB of memory)" << endl;
}
