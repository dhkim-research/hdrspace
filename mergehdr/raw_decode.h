#pragma once

// Decode one RAW frame into a flat Bayer mosaic with LibRaw (no demosaicing, no
// LibRaw post-processing). Kept free of Boost/Exiv2 so that the decode can be
// checked on its own against linearhdr's rawconvert (tools/compare_rawconvert_levels.cpp).

#include "camera_detect.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include <libraw/libraw.h>

struct DecodedRawFrame {
    size_t width = 0;
    size_t height = 0;
    int blacklevel = 0;      // scalar black baseline stored in the mosaic
    int whitepoint = 0;      // absolute full-scale white on the same encoding
    int decoderBlack = 0;    // LibRaw's own black (scalar + mean channel correction), for checks only
    int decoderWhite = 0;    // LibRaw's own absolute white (color.maximum before black subtraction)
    int filter = 0;
    std::unique_ptr<uint16_t[]> image;
};

inline int buildDcrawFilter(LibRaw &raw, int top, int left) {
    int filter = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 2; ++x) {
            int color = raw.COLOR(top + y, left + x);
            filter |= color << (((y << 1 & 14) + (x & 1)) << 1);
        }
    }
    return filter;
}

/**
 * fixedBlack < 0: LibRaw subtracts its own black for this frame (scalar, channel and
 *   repeated BlackLevel tables), as rawconvert does without -k.
 * fixedBlack >= 0: the black is set the way linearhdr's rawconvert -k sets it
 *   (LibRaw params.user_black): the scalar black becomes fixedBlack, LibRaw keeps its
 *   per-channel residual and drops the repeated table. This is the same LibRaw code
 *   path (adjust_bl, subtract_black) that rawconvert reaches through dcraw_process.
 * The black-subtracted signal is stored with the scalar baseline added back, so the
 * merge can keep normalising (value - black) / (white - black).
 */
inline DecodedRawFrame decodeWithLibRaw(const std::string &filename, int fixedBlack = -1) {
    auto fail = [&](const std::string &what) {
        return std::runtime_error("\"" + filename + "\": " + what);
    };
    LibRaw raw;

    int err = raw.open_file(filename.c_str());
    if (err != LIBRAW_SUCCESS)
        throw fail(std::string("LibRaw open_file failed: ") + libraw_strerror(err));

    err = raw.unpack();
    if (err != LIBRAW_SUCCESS)
        throw fail(std::string("LibRaw unpack failed: ") + libraw_strerror(err));

    if (!raw.imgdata.rawdata.raw_image)
        throw fail("LibRaw did not return a flat Bayer raw_image");

    const libraw_image_sizes_t &sizes = raw.imgdata.sizes;
    const libraw_colordata_t &color = raw.imgdata.color;

    int left = sizes.left_margin;
    int top = sizes.top_margin;
    size_t width = sizes.iwidth ? sizes.iwidth : sizes.width;
    size_t height = sizes.iheight ? sizes.iheight : sizes.height;

    if (width == 0 || height == 0)
        throw fail("LibRaw reported an empty visible image");

    const int decoderBlack = effectiveRawBlackLevel(
        color.black, color.cblack, sizeof(color.cblack) / sizeof(color.cblack[0]));
    const int sourceWhitepoint = color.maximum ? static_cast<int>(color.maximum) : 0x3FFF;
    const int blacklevel = fixedBlack >= 0 ? fixedBlack : decoderBlack;
    const int filter = buildDcrawFilter(raw, top, left);

    if (fixedBlack >= 0)
        raw.imgdata.params.user_black = fixedBlack;

    err = raw.raw2image();
    if (err != LIBRAW_SUCCESS)
        throw fail(std::string("LibRaw raw2image failed: ") + libraw_strerror(err));
    err = raw.subtract_black();
    if (err != LIBRAW_SUCCESS)
        throw fail(std::string("LibRaw subtract_black failed: ") + libraw_strerror(err));

    const libraw_image_sizes_t &normalizedSizes = raw.imgdata.sizes;
    const size_t normalizedWidth = normalizedSizes.iwidth ? normalizedSizes.iwidth : normalizedSizes.width;
    const size_t normalizedHeight = normalizedSizes.iheight ? normalizedSizes.iheight : normalizedSizes.height;
    if (normalizedWidth != width || normalizedHeight != height || !raw.imgdata.image)
        throw fail("LibRaw black-normalized image geometry changed unexpectedly");

    // After subtract_black() color.maximum is the range above the subtracted black.
    const int usableWhitepoint = raw.imgdata.color.maximum
        ? static_cast<int>(raw.imgdata.color.maximum)
        : std::max(1, sourceWhitepoint - blacklevel);
    const int whitepoint = std::min(65535, blacklevel + usableWhitepoint);
    std::unique_ptr<uint16_t[]> image(new uint16_t[width * height]);

    // raw2image() lays the flat Bayer signal into the channel selected by fcol().
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const int channel = raw.fcol(static_cast<int>(y), static_cast<int>(x));
            if (channel < 0 || channel > 3)
                throw fail("unsupported non-Bayer color layout");
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
    frame.decoderBlack = decoderBlack;
    frame.decoderWhite = sourceWhitepoint;
    frame.filter = filter;
    frame.image = std::move(image);
    return frame;
}
