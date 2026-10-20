// Check that mergehdr's RAW decode with fixed profile levels gives the same normalized mosaic
// as linearhdr's rawconvert -disinterp -k BLACK -S WHITE -r 1 1 1 1 (LibRaw dcraw_process,
// adjust_maximum_thr = 0, raw colour, linear 16-bit), pixel by pixel.
// Also reports what the previous behaviour (LibRaw's own black, profile black as baseline only) gave.
//
// Build (no Boost/Exiv2 needed), from the mergehdr folder:
//   g++ -std=c++17 -O2 -I. -I<LibRaw source dir> tools/compare_rawconvert_levels.cpp camera_detect.cpp
//       <LibRaw source dir>/lib/libraw.a -lz -lpthread -o compare_rawconvert_levels
// Run: compare_rawconvert_levels BLACK WHITE_ABOVE_BLACK file.CR2 [file2 ...]
// Exit status 0 only if every frame agrees within one 16-bit output code of rawconvert.
#include "../raw_decode.h"
#include "../raw_normalization.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s BLACK WHITE_ABOVE_BLACK file.raw [...]\n", argv[0]);
        return 2;
    }
    const int black = std::atoi(argv[1]), white = std::atoi(argv[2]);
    bool ok = true;
    for (int a = 3; a < argc; ++a) {
        const char *f = argv[a];
        // linearhdr: rawconvert emulation (same LibRaw calls rawconvert makes)
        LibRaw r;
        auto &O = r.imgdata.params;
        for (int c = 0; c < 4; ++c) O.user_mul[c] = 1;
        O.adjust_maximum_thr = 0; O.output_color = 0; O.gamm[0] = O.gamm[1] = O.no_auto_bright = 1;
        O.output_bps = 16; O.no_interpolation = 1; O.user_black = black; O.user_sat = white; O.user_flip = 0;
        if (r.open_file(f) != LIBRAW_SUCCESS || r.unpack() != LIBRAW_SUCCESS || r.dcraw_process() != LIBRAW_SUCCESS) {
            std::fprintf(stderr, "%s: LibRaw failed\n", f); return 2;
        }
        int err = 0;
        libraw_processed_image_t *im = r.dcraw_make_mem_image(&err);
        if (!im || im->bits != 16) { std::fprintf(stderr, "%s: no 16-bit image\n", f); return 2; }

        // mergehdr: new fixed-level decode, and the previous decode (LibRaw black)
        DecodedRawFrame fixedFrame = decodeWithLibRaw(f, black);
        DecodedRawFrame oldFrame = decodeWithLibRaw(f, -1);
        if (fixedFrame.width != im->width || fixedFrame.height != im->height) {
            std::fprintf(stderr, "%s: size %zux%zu vs rawconvert %ux%u\n", f, fixedFrame.width, fixedFrame.height, im->width, im->height);
            return 2;
        }
        const int W = black + white;
        double maxNew = 0, maxOld = 0, sumOld = 0, loNew = 1e9, hiNew = -1e9; size_t n = 0, nSat = 0;
        for (size_t y = 0; y < fixedFrame.height; ++y)
            for (size_t x = 0; x < fixedFrame.width; ++x) {
                int c = r.COLOR(static_cast<int>(y), static_cast<int>(x)); if (c == 3) c = 1;
                const unsigned short lin = reinterpret_cast<unsigned short *>(im->data)[(y * im->width + x) * im->colors + c];
                const double nLin = lin / 65535.0;
                const size_t q = y * fixedFrame.width + x;
                const double nNew = (normalizeRawSample(fixedFrame.image[q], black, W, black, W) - black) / double(white);
                // previous load(): frame LibRaw levels -> common (profile black, max frame white); merge white = profile
                const int oldCommonWhite = std::max(oldFrame.whitepoint, W);
                const double nOld = (normalizeRawSample(oldFrame.image[q], oldFrame.blacklevel, oldFrame.whitepoint, black, oldCommonWhite) - black) / double(white);
                maxNew = std::max(maxNew, std::fabs(nNew - nLin));
                loNew = std::min(loNew, (nNew - nLin) * 65535.0); hiNew = std::max(hiNew, (nNew - nLin) * 65535.0);
                const double dOld = (std::min(nOld, 1.0) - nLin) * white;   // in raw counts
                maxOld = std::max(maxOld, std::fabs(dOld)); sumOld += dOld; ++n;
                nSat += lin == 65535;
            }
        const bool frameOk = maxNew * 65535.0 <= 1.0;
        ok = ok && frameOk;
        std::printf("%s  LibRaw black %d white %d | fixed decode - rawconvert: %+.3f to %+.3f codes (%s) | "
                    "previous decode vs rawconvert: mean %+.2f, max %.2f counts | clipped %.4f %%\n",
                    f, fixedFrame.decoderBlack, fixedFrame.decoderWhite, loNew, hiNew, frameOk ? "OK" : "DIFFERENT",
                    sumOld / n, maxOld, 100.0 * nSat / n);
        LibRaw::dcraw_clear_mem(im);
    }
    std::printf(ok ? "PASS: fixed-level decode matches rawconvert -k/-S on every frame\n"
                   : "FAIL: fixed-level decode differs from rawconvert\n");
    return ok ? 0 : 1;
}
