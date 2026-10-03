#if !defined(__HDRMERGE_H)
#define __HDRMERGE_H

#include "platform.h"
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <stdint.h>
#include <memory>
#include <utility>

#ifndef MERGEHDR_ENABLE_OPENEXR
#define MERGEHDR_ENABLE_OPENEXR 1
#endif

using std::cout;
using std::cerr;
using std::endl;

/// String map for metadata
typedef std::map<std::string, std::string> StringMap;

/// Rgb color type
typedef float float3[3];

enum EDemosaicMethod {
    EDemosaicAHD,
    EDemosaicDHT
};

enum EMergeMethod {
    EMergeHDRMerge,
    EMergePyLinear
};

/// Abstract reconstruction filter
class ReconstructionFilter {
public:
    virtual float getRadius() const = 0;
    virtual float eval(float x) const = 0;
};

/// Records a single RAW exposure
struct Exposure {
    std::string filename;
    float exposure;
    float shown_exposure;
    float iso;
    float aperture;
    uint16_t *image;

    inline Exposure(const std::string &filename)
     : filename(filename), exposure(-1), shown_exposure(-1), iso(-1), aperture(-1), image(NULL) { }

    inline ~Exposure() {
        release();
    }

    inline void release() {
        if (image) {
            delete[] image;
            image = NULL;
        }
    }

    /// Return the exposure has a human-readable string
    std::string toString() const {
        char buf[10];
        if (exposure < 1)
            snprintf(buf, sizeof(buf), "1/%.4g", 1/exposure);
        else
            snprintf(buf, sizeof(buf), "%.4g", exposure);
        return buf;
    }
};

struct RawChannelSampleStats {
    double average = 0.0;
    double fraction = 0.0;
    size_t validSamples = 0;
    size_t totalSamples = 0;
};

/// Stores a series of exposures, manages demosaicing and subsequent steps
struct ExposureSeries {
    std::vector<Exposure> exposures;
    StringMap metadata;

    /* Width and height of the cropped RAW images */
    size_t width, height;

    /* Black level and whitepoint as reported by the active RAW decoder */
    int blacklevel, whitepoint;

    /* Merged high dynamic range image (no demosaicing yet) */
    float *image_merged;

    /* Merged and demosaiced image */
    float3 *image_demosaiced;

    /* dcraw-style color filter array description */
    int filter;

    /* Bad pixel coordinates in full RAW mosaic coordinates */
    std::vector<std::pair<int, int>> badpixels;

    /* Saturation threshold */
    float saturation;

    /* Merge style + underexposure threshold */
    EMergeMethod merge_method;
    float underexposed;

    /* Per-channel white saturation hint */
    float white_saturation[3];

    /* Tables for transforming from sensor values to exposures / weights */
    float weight_tbl[0x10000], value_tbl[0x10000];

    inline ExposureSeries() : 
        image_merged(NULL), image_demosaiced(NULL),
        merge_method(EMergeHDRMerge), underexposed(0.0f) {
        white_saturation[0] = white_saturation[1] = white_saturation[2] = 1.0f;
    }

    ~ExposureSeries() {
        if (image_merged)
            delete[] image_merged;
        if (image_demosaiced)
            delete[] image_demosaiced;
    }

    /// Return the color at position (x, y)
    inline int fc(int x, int y) const {
        int color = (filter >> (((y << 1 & 14) + (x & 1)) << 1) & 3);
        return color == 3 ? 1 : color;
    }

    /**
     * Add a file to the exposure series (or, optionally, a sequence
     * such as file_%03i.png expressed using the printf-style format)
     */
    void add(const std::string &filename);

    /**
     * Check that all exposures are valid, and that they satisfy
     * a few basic requirements such as:
     *  - all images use the same ISO speed and aperture setting, and carry
     *    exposure mode metadata (both relaxed for linearhdr effective exposures,
     *    which include ISO and aperture per frame)
     *  - there are no duplicate exposures.
     *
     * This also sorts the exposures in case they weren't ordered already
     */
    void check(bool allowVariableIso = false);

    /**
     * Run dcraw on an entire exposure series (in parallel)
     * and fill the exposure series with a normalized RGB floating
     * point image representation
     */
    void load();

    /// Initialize the exposure / weight table
    void initTables(float saturation, EMergeMethod method = EMergeHDRMerge,
        float underexposed = 0.0f);

    /// Repair a known list of bad pixels in the RAW mosaic before merging.
    void repairBadPixels();

    /**
     * Scale the RAW channels by camera premultipliers (r, g, b; the second green uses g) the way
     * LibRaw's scale_colors() does for linearhdr's rawconvert -r without highlight recovery:
     * relative to the smallest multiplier, about the black level, clipped at the white point.
     */
    void applyRawMultipliers(const float multipliers[3]);

    /// Merge all exposures into a single HDR image and release the RAW data
    void merge();

    /// Compute average value and in-range fraction for a Bayer channel without writing an image.
    RawChannelSampleStats sampleRawChannel(int channel);

    /// Estimate the exposure times in case the EXIF tags can't be trusted
    void fitExposureTimes();

    /// Perform demosaicing
    void demosaic(float *sensor2xyz, EDemosaicMethod method = EDemosaicAHD);

    /// Expand the merged Bayer mosaic into a sparse 3-channel raw grid
    void rawgrid();

    /// Transform the image into the right color space
    void transform_color(float *sensor2xyz, bool xyz);

    /// Apply an arbitrary 3x3 color transform in-place
    void transform_color_matrix(float *matrix, const std::string &label);

    /// Convert a 180-degree equisolid fisheye image to equiangular
    void solid2ang();

    /// Scale the image brightness by a given factor
    void scale(float factor);

    /// Resample the image to a different resolution
    void resample(const ReconstructionFilter &filter, size_t w, size_t h);

    /// Crop a rectangular region
    void crop(int x, int y, int w, int h);

    /// Crop the RAW mosaic before merging and shift CFA parity accordingly
    void crop_raw(int x, int y, int w, int h);

    /// Apply white balancing
    void whitebalance(float *scale);

    /// Apply white balancing based on a grey patch
    void whitebalance(int xoffs, int yoffs, int w, int h);

    /// Remove vignetting / calibration routine
    void vcal();

    /// Correct for vignetting using a radial polynomial 1+ax^2+bx^4+cx^6
    void vcorr(float a, float b, float c);

    /// Return the number of exposures
    inline size_t size() const {
        return exposures.size();
    }

    /// Evaluate a pixel in one of the images
    float eval(int img, int x, int y) const {
        return value_tbl[exposures[img].image[x + y*width]];
    }
};

/// Windowed Lanczos filter
class LanczosSincFilter : public ReconstructionFilter {
public:
    LanczosSincFilter(float radius = 3) : m_radius(radius) { }

    float getRadius() const { return m_radius; }

    float eval(float x) const;
private:
    float m_radius;
};

/// Tent filter
class TentFilter : public ReconstructionFilter {
public:
    TentFilter(float radius = 1) : m_radius(radius) { }

    float getRadius() const { return m_radius; }

    float eval(float x) const;
private:
    float m_radius;
};



/// Return the number of processors available for multithreading
extern int getProcessorCount();

/**
 * Write a lossless floating point OpenEXR file using either half or
 * single precision (grayscale or RGB)
 */
extern void writeOpenEXR(const std::string &filename, size_t w, size_t h,
    int channels, float *data, const StringMap &metadata, bool writeHalf);

void writeJPEG(const std::string &filename, size_t w, size_t h, float *data, int quality = 100);

void writeRGBE(const std::string &filename, size_t w, size_t h, float *data,
    const std::vector<std::string> &headerLines = std::vector<std::string>(),
    bool radianceCompatibility = true);

/// Generate a uniformly distributed random number in [0, 1)
inline float randf() {
    #define RS_SCALE (1.0f / (1.0f + RAND_MAX))
    float f;
    do {
       f = (((rand () * RS_SCALE) + rand ()) * RS_SCALE + rand()) * RS_SCALE;
    } while (f >= 1); /* Round off */

    return f;
}

inline float clamp(float value, float min, float max) {
    if (min > max)
        std::swap(min, max);
    return std::min(std::max(value, min), max);
}

inline float square(float value) {
    return value*value;
}

/// check if a file exists
bool fexists(const std::string& name);

enum ERotateFlipType {
    ERotateNoneFlipNone = 0,
    ERotate180FlipXY    = ERotateNoneFlipNone,
    ERotate90FlipNone   = 1,
    ERotate270FlipXY    = ERotate90FlipNone,
    ERotate180FlipNone  = 2,
    ERotateNoneFlipXY   = ERotate180FlipNone,
    ERotate270FlipNone  = 3,
    ERotate90FlipXY     = ERotate270FlipNone,
    ERotateNoneFlipX    = 4,
    ERotate180FlipY     = ERotateNoneFlipX,
    ERotate90FlipX      = 5,
    ERotate270FlipY     = ERotate90FlipX,
    ERotate180FlipX     = 6,
    ERotateNoneFlipY    = ERotate180FlipX,
    ERotate270FlipX     = 7,
    ERotate90FlipY      = ERotate270FlipX
};

// Rotate and/or flip an arbitrary image
extern void rotateFlip(
        uint8_t *src,  size_t  s_width, size_t  s_height,
        uint8_t *&dst, size_t &t_width, size_t &t_height,
        int bypp, ERotateFlipType type);

extern ERotateFlipType flipTypeFromString(int rotation, std::string axes);

enum EColorMode {
    ENative,
    ESRGB,
    EXYZ
};

extern std::istream& operator>>(std::istream& in, EColorMode& unit);
extern std::istream& operator>>(std::istream& in, EDemosaicMethod& unit);
extern std::istream& operator>>(std::istream& in, EMergeMethod& unit);

/// Native DHT demosaicing port used by hdrmerge.
extern void demosaicDHT(ExposureSeries &series, bool median);

#endif /* __HDRMERGE_H */
