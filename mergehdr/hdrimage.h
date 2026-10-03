#ifndef MERGEHDR_HDRIMAGE_H
#define MERGEHDR_HDRIMAGE_H

#include <cstddef>
#include <map>
#include <string>
#include <vector>

// Radiance resolution-line orientation flags (resolu.h).
constexpr int kRadianceXDecr = 1;
constexpr int kRadianceYDecr = 2;
constexpr int kRadianceYMajor = 4;
constexpr int kRadiancePixStandard = kRadianceYMajor | kRadianceYDecr; // "-Y <h> +X <w>"

struct HdrImage {
    size_t width = 0;
    size_t height = 0;
    std::vector<float> rgb; // Stored bottom-up so cell coordinates match linearhdr files.
    std::vector<std::string> headerLines;
    std::map<std::string, std::string> header;
    // Added for Radiance-faithful consumers (evalglare). Defaults describe a standard file.
    std::string magicLine;                    // first header line verbatim (e.g. "#?RADIANCE")
    std::vector<std::string> rawHeaderLines;  // every information-header line verbatim (magic line,
                                              // '#' lines and lines without '='), without the '\n'
    // Radiance orientation flags of the stored pixel grid, normalised like Radiance pictool's
    // pict_read(): kRadianceYMajor is always set; for an X-major file ("+X w +Y h" etc.) width and
    // height are swapped and the scanlines become rows. XDECR/YDECR are kept from the file.
    int radianceOrientation = kRadiancePixStandard;
};

bool isRadianceHDR(const std::string &path);
// Reads a Radiance picture. All eight resolution-line orientations are accepted and mapped exactly
// like Radiance fgetsresolu() + pictool pict_read(): scanline number s (0 = first in the file) becomes
// Radiance row y = (YDECR ? height-1-s : s) (row 0 = bottom for standard files), pixels keep their
// order within the scanline (XDECR is recorded in radianceOrientation, not applied), and an X-major
// file is transposed (width/height swapped). flipY=true (default) stores rgb with row 0 = Radiance
// row 0 (bottom-up); flipY=false stores row 0 = Radiance row height-1 (top-down). Standard
// "-Y h +X w" files are read exactly as before.
HdrImage readRadianceHDR(const std::string &path, bool addHalfStep = true, bool flipY = true);
// Writes `image` (rgb stored bottom-up, as returned by readRadianceHDR(path, true, true)) with the
// given orientation flags exactly like Radiance pictool's pict_write(): "#?RADIANCE", the header
// lines verbatim, "FORMAT=32-bit_rle_rgbe", the resolution line for `orientation` and the scanlines
// in the corresponding order (truncating RGBE quantisation like Radiance setcolr()).
void writeRadianceHDROriented(const std::string &path,
        const HdrImage &image,
        const std::vector<std::string> &verbatimHeaderLines,
        int orientation = kRadiancePixStandard);
void writeRadianceHDR(const std::string &path,
        const HdrImage &image,
        const std::vector<std::string> &headerLines = std::vector<std::string>(),
        bool radianceCompatibility = false,
        bool topDownInput = false);
void writeRadianceHDRMatlabCompatible(const std::string &path,
        const HdrImage &image,
        const std::vector<std::string> &headerLines = std::vector<std::string>(),
        bool radianceCompatibility = false,
        bool topDownInput = false);
void quantizeRadianceRgbRoundTrip(std::vector<float> &rgb);
float quantizeRadianceGrayRoundTrip(float value);
float quantizeRadianceGrayRoundTripMatlabCompatible(float value);

#endif
