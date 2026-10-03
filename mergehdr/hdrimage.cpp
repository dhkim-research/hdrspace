#include "hdrimage.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string trimCopy(const std::string &value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return value.substr(begin, end - begin);
}

float rgbeScale(unsigned char exponent) {
    if (exponent == 0)
        return 0.0f;
    return std::ldexp(1.0f, static_cast<int>(exponent) - (128 + 8));
}

constexpr float kWhiteEfficacy = 179.0f;

using Trgbe = unsigned char;

struct TrgbePixel {
    Trgbe r;
    Trgbe g;
    Trgbe b;
    Trgbe e;
};

template <typename QuantizeFn>
void rgb2rgbeImpl(float r, float g, float b, TrgbePixel &rgbe, QuantizeFn quantizeMantissa) {
    r = std::max(0.0f, r);
    g = std::max(0.0f, g);
    b = std::max(0.0f, b);

    double v = std::max({static_cast<double>(r), static_cast<double>(g), static_cast<double>(b)});
    if (v < 1e-32) {
        rgbe = {};
        return;
    }

    int e = 0;
    v = std::frexp(v, &e) * 256.0 / v;
    rgbe.r = quantizeMantissa(v * r);
    rgbe.g = quantizeMantissa(v * g);
    rgbe.b = quantizeMantissa(v * b);
    rgbe.e = static_cast<Trgbe>(e + 128);
}

void rgb2rgbe(float r, float g, float b, TrgbePixel &rgbe) {
    const auto quantizeMantissa = [](double value) -> Trgbe {
        return static_cast<Trgbe>(std::max(0, std::min(255, static_cast<int>(value))));
    };
    rgb2rgbeImpl(r, g, b, rgbe, quantizeMantissa);
}

void rgb2rgbeMatlabCompatible(float r, float g, float b, TrgbePixel &rgbe) {
    const auto quantizeMantissa = [](double value) -> Trgbe {
        const int rounded = static_cast<int>(std::floor(value + 0.5));
        return static_cast<Trgbe>(std::max(0, std::min(255, rounded)));
    };
    rgb2rgbeImpl(r, g, b, rgbe, quantizeMantissa);
}

void writeRleScanline(FILE* file, const Trgbe* scanline, int size) {
    const Trgbe* scanEnd = scanline + size;
    while (scanline < scanEnd) {
        int runStart = 0;
        int peek = 0;
        int runLen = 0;
        while (runLen <= 4 && peek < 128 && scanline + peek < scanEnd) {
            runStart = peek;
            runLen = 0;
            while (runLen < 127 && runStart + runLen < 128 &&
                   scanline + peek < scanEnd &&
                   scanline[runStart] == scanline[peek]) {
                ++peek;
                ++runLen;
            }
        }

        if (runLen > 4) {
            if (runStart > 0) {
                std::vector<Trgbe> buf(static_cast<size_t>(runStart + 1));
                buf[0] = static_cast<Trgbe>(runStart);
                for (int i = 0; i < runStart; ++i) {
                    buf[static_cast<size_t>(i + 1)] = scanline[i];
                }
                std::fwrite(buf.data(), sizeof(Trgbe), buf.size(), file);
            }

            Trgbe buf[2];
            buf[0] = static_cast<Trgbe>(128 + runLen);
            buf[1] = scanline[runStart];
            std::fwrite(buf, sizeof(Trgbe), 2, file);
        } else {
            std::vector<Trgbe> buf(static_cast<size_t>(peek + 1));
            buf[0] = static_cast<Trgbe>(peek);
            for (int i = 0; i < peek; ++i) {
                buf[static_cast<size_t>(i + 1)] = scanline[i];
            }
            std::fwrite(buf.data(), sizeof(Trgbe), buf.size(), file);
        }

        scanline += peek;
    }
}

std::string readAsciiLine(std::ifstream &input) {
    std::string line;
    std::getline(input, line);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    return line;
}

} // namespace

bool isRadianceHDR(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not open file: " + path);
    char magic[10] = {0};
    input.read(magic, sizeof(magic));
    return input.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
           std::string(magic, magic + sizeof(magic)) == "#?RADIANCE";
}

HdrImage readRadianceHDR(const std::string &path, bool addHalfStep, bool flipY) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not open HDR file: " + path);

    HdrImage image;
    std::string first = readAsciiLine(input);
    if (first != "#?RADIANCE")
        throw std::runtime_error("Unsupported HDR header in " + path);
    image.magicLine = first;
    image.rawHeaderLines.push_back(first);

    while (true) {
        std::string line = readAsciiLine(input);
        if (!input)
            throw std::runtime_error("Unexpected end of HDR header in " + path);
        if (line.empty())
            break;
        image.rawHeaderLines.push_back(line);
        if (line[0] == '#')
            continue;
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = trimCopy(line.substr(0, eq));
            std::string value = trimCopy(line.substr(eq + 1));
            image.headerLines.push_back(key + "= " + value);
            image.header[key] = value;
        }
    }

    // Resolution line, parsed like Radiance str2resolu(): the last 'X' and 'Y' in the line give the
    // sizes; the axis named last is the scanline (minor) axis; a '-' sign means decreasing order.
    const std::string resolution = readAsciiLine(input);
    const size_t xIndex = resolution.find_last_of('X');
    const size_t yIndex = resolution.find_last_of('Y');
    if (xIndex == std::string::npos || yIndex == std::string::npos)
        throw std::runtime_error("Could not parse HDR resolution line in " + path);
    int orientation = 0;
    if (xIndex > yIndex)
        orientation |= kRadianceYMajor;
    if (xIndex > 0 && resolution[xIndex - 1] == '-')
        orientation |= kRadianceXDecr;
    if (yIndex > 0 && resolution[yIndex - 1] == '-')
        orientation |= kRadianceYDecr;
    int width = std::atoi(resolution.c_str() + xIndex + 1);
    int height = std::atoi(resolution.c_str() + yIndex + 1);
    if (height <= 0 || width <= 0)
        throw std::runtime_error("Invalid HDR resolution in " + path);
    if (!(orientation & kRadianceYMajor)) {
        // pictool pict_read(): X-major files are stored transposed (scanlines become rows).
        std::swap(width, height);
        orientation |= kRadianceYMajor;
    }
    image.radianceOrientation = orientation;
    const bool scanlinesDecreaseY = (orientation & kRadianceYDecr) != 0;

    image.width = static_cast<size_t>(width);
    image.height = static_cast<size_t>(height);
    image.rgb.assign(image.width * image.height * 3, 0.0f);

    std::vector<unsigned char> rowR(image.width);
    std::vector<unsigned char> rowG(image.width);
    std::vector<unsigned char> rowB(image.width);
    std::vector<unsigned char> rowE(image.width);

    const float mantissaOffset = addHalfStep ? 0.5f : 0.0f;
    for (int fileY = 0; fileY < height; ++fileY) {
        unsigned char header[4];
        input.read(reinterpret_cast<char *>(header), 4);
        if (input.gcount() != 4)
            throw std::runtime_error("Unexpected end of HDR image data in " + path);

        const bool isNewRle = (header[0] == 2 && header[1] == 2 && !(header[2] & 0x80));
        if (isNewRle) {
            int scanWidth = (static_cast<int>(header[2]) << 8) | static_cast<int>(header[3]);
            if (scanWidth != width)
                throw std::runtime_error("HDR scanline width mismatch in " + path);

            std::vector<unsigned char> *channels[4] = { &rowR, &rowG, &rowB, &rowE };
            for (int c = 0; c < 4; ++c) {
                size_t x = 0;
                while (x < image.width) {
                    unsigned char code = 0;
                    input.read(reinterpret_cast<char *>(&code), 1);
                    if (!input)
                        throw std::runtime_error("Unexpected end of HDR RLE stream in " + path);

                    if (code > 128) {
                        size_t count = static_cast<size_t>(code - 128);
                        unsigned char value = 0;
                        input.read(reinterpret_cast<char *>(&value), 1);
                        if (!input)
                            throw std::runtime_error("Unexpected end of HDR RLE run in " + path);
                        if (x + count > image.width)
                            throw std::runtime_error("HDR RLE run exceeded scanline width in " + path);
                        std::fill_n(channels[c]->begin() + static_cast<std::ptrdiff_t>(x), count, value);
                        x += count;
                    } else {
                        size_t count = static_cast<size_t>(code);
                        if (count == 0 || x + count > image.width)
                            throw std::runtime_error("HDR RLE literal exceeded scanline width in " + path);
                        input.read(reinterpret_cast<char *>(&(*channels[c])[x]), static_cast<std::streamsize>(count));
                        if (input.gcount() != static_cast<std::streamsize>(count))
                            throw std::runtime_error("Unexpected end of HDR literal run in " + path);
                        x += count;
                    }
                }
            }
        } else {
            auto assignPixel = [&](size_t x, unsigned char r, unsigned char g, unsigned char b, unsigned char e) {
                rowR[x] = r;
                rowG[x] = g;
                rowB[x] = b;
                rowE[x] = e;
            };

            assignPixel(0, header[0], header[1], header[2], header[3]);
            size_t x = 1;
            int rshift = 0;
            while (x < image.width) {
                unsigned char pixel[4];
                input.read(reinterpret_cast<char *>(pixel), 4);
                if (input.gcount() != 4)
                    throw std::runtime_error("Unexpected end of HDR scanline data in " + path);

                if (pixel[0] == 1 && pixel[1] == 1 && pixel[2] == 1) {
                    size_t count = static_cast<size_t>(pixel[3]) << rshift;
                    if (x == 0 || x + count > image.width)
                        throw std::runtime_error("HDR old-style run exceeded scanline width in " + path);
                    while (count-- > 0) {
                        assignPixel(x, rowR[x - 1], rowG[x - 1], rowB[x - 1], rowE[x - 1]);
                        ++x;
                    }
                    rshift += 8;
                } else {
                    assignPixel(x, pixel[0], pixel[1], pixel[2], pixel[3]);
                    ++x;
                    rshift = 0;
                }
            }
        }

        // Radiance (pictool) row of this scanline: row 0 is the bottom row for "-Y" files and the
        // first scanline for "+Y" files. For standard files this is the previous mapping.
        const size_t radianceRow = scanlinesDecreaseY
            ? (image.height - 1 - static_cast<size_t>(fileY))
            : static_cast<size_t>(fileY);
        size_t y = flipY ? radianceRow : (image.height - 1 - radianceRow);
        for (size_t x = 0; x < image.width; ++x) {
            float scale = rgbeScale(rowE[x]);
            size_t index = (y * image.width + x) * 3;
            image.rgb[index + 0] = scale > 0.0f ? (static_cast<float>(rowR[x]) + mantissaOffset) * scale : 0.0f;
            image.rgb[index + 1] = scale > 0.0f ? (static_cast<float>(rowG[x]) + mantissaOffset) * scale : 0.0f;
            image.rgb[index + 2] = scale > 0.0f ? (static_cast<float>(rowB[x]) + mantissaOffset) * scale : 0.0f;
        }
    }

    return image;
}

void writeRadianceHDRInternal(const std::string &path,
        const HdrImage &image,
        const std::vector<std::string> &headerLines,
        bool radianceCompatibility,
        bool topDownInput,
        bool matlabCompatibleQuantization) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        throw std::runtime_error("Unable to open output HDR file: " + path);
    }

    std::fprintf(file, "#?RADIANCE\n");
    const auto& lines = headerLines.empty() ? image.headerLines : headerLines;
    for (const std::string &line : lines) {
        if (line.rfind("VIEW=", 0) != 0) {
            std::fprintf(file, "%s\n", line.c_str());
        }
    }
    for (const std::string &line : lines) {
        if (line.rfind("VIEW=", 0) == 0) {
            std::fprintf(file, "%s\n", line.c_str());
        }
    }
    std::fprintf(file, "FORMAT=32-bit_rle_rgbe\n");
    std::fprintf(file, "\n");
    std::fprintf(file, "-Y %d +X %d\n", static_cast<int>(image.height), static_cast<int>(image.width));

    std::vector<Trgbe> scanlineR(image.width * image.height);
    std::vector<Trgbe> scanlineG(image.width * image.height);
    std::vector<Trgbe> scanlineB(image.width * image.height);
    std::vector<Trgbe> scanlineE(image.width * image.height);

    for (size_t y = 0; y < image.height; ++y) {
        const size_t srcY = topDownInput ? y : (image.height - 1 - y);
        const float* row = image.rgb.data() + srcY * image.width * 3;
        Trgbe* rowR = scanlineR.data() + y * image.width;
        Trgbe* rowG = scanlineG.data() + y * image.width;
        Trgbe* rowB = scanlineB.data() + y * image.width;
        Trgbe* rowE = scanlineE.data() + y * image.width;

        for (size_t x = 0; x < image.width; ++x) {
            TrgbePixel pixel{};
            float r = row[x * 3 + 0];
            float g = row[x * 3 + 1];
            float b = row[x * 3 + 2];
            if (radianceCompatibility) {
                r /= kWhiteEfficacy;
                g /= kWhiteEfficacy;
                b /= kWhiteEfficacy;
            }
            if (matlabCompatibleQuantization)
                rgb2rgbeMatlabCompatible(r, g, b, pixel);
            else
                rgb2rgbe(r, g, b, pixel);
            rowR[x] = pixel.r;
            rowG[x] = pixel.g;
            rowB[x] = pixel.b;
            rowE[x] = pixel.e;
        }
    }

    for (size_t y = 0; y < image.height; ++y) {
        unsigned char header[4];
        header[0] = 2;
        header[1] = 2;
        header[2] = static_cast<unsigned char>(image.width >> 8);
        header[3] = static_cast<unsigned char>(image.width & 0xFF);
        std::fwrite(header, sizeof(header), 1, file);

        writeRleScanline(file, scanlineR.data() + y * image.width, static_cast<int>(image.width));
        writeRleScanline(file, scanlineG.data() + y * image.width, static_cast<int>(image.width));
        writeRleScanline(file, scanlineB.data() + y * image.width, static_cast<int>(image.width));
        writeRleScanline(file, scanlineE.data() + y * image.width, static_cast<int>(image.width));
    }

    std::fclose(file);
}

void writeRadianceHDR(const std::string &path,
        const HdrImage &image,
        const std::vector<std::string> &headerLines,
        bool radianceCompatibility,
        bool topDownInput) {
    writeRadianceHDRInternal(path, image, headerLines, radianceCompatibility, topDownInput, false);
}

void writeRadianceHDRMatlabCompatible(const std::string &path,
        const HdrImage &image,
        const std::vector<std::string> &headerLines,
        bool radianceCompatibility,
        bool topDownInput) {
    writeRadianceHDRInternal(path, image, headerLines, radianceCompatibility, topDownInput, true);
}

void writeRadianceHDROriented(const std::string &path,
        const HdrImage &image,
        const std::vector<std::string> &verbatimHeaderLines,
        int orientation) {
    if (!(orientation & kRadianceYMajor))
        throw std::runtime_error("writeRadianceHDROriented: orientation must be Y-major");
    if (image.rgb.size() != image.width * image.height * 3)
        throw std::runtime_error("writeRadianceHDROriented: pixel buffer does not match the image size");
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        throw std::runtime_error("Unable to open output HDR file: " + path);

    std::fprintf(file, "#?RADIANCE\n");
    for (const std::string &line : verbatimHeaderLines)
        std::fprintf(file, "%s\n", line.c_str());
    std::fprintf(file, "FORMAT=32-bit_rle_rgbe\n\n");
    // Radiance resolu2str() for a Y-major orientation.
    std::fprintf(file, "%cY %8d %cX %8d\n",
                 (orientation & kRadianceYDecr) ? '-' : '+', static_cast<int>(image.height),
                 (orientation & kRadianceXDecr) ? '-' : '+', static_cast<int>(image.width));

    std::vector<Trgbe> rowR(image.width), rowG(image.width), rowB(image.width), rowE(image.width);
    for (size_t fileY = 0; fileY < image.height; ++fileY) {
        // pictool pict_write(): scanline y holds row (YDECR ? height-1-y : y).
        const size_t row = (orientation & kRadianceYDecr) ? (image.height - 1 - fileY) : fileY;
        const float* src = image.rgb.data() + row * image.width * 3;
        for (size_t x = 0; x < image.width; ++x) {
            TrgbePixel pixel{};
            const double r = src[x * 3 + 0];
            const double g = src[x * 3 + 1];
            const double b = src[x * 3 + 2];
            // Radiance setcolr(): truncating mantissas, non-positive components become 0.
            double d = r > g ? r : g;
            if (b > d)
                d = b;
            if (d > 1e-32) {
                int e = 0;
                d = std::frexp(d, &e) * 256.0 / d;
                pixel.r = static_cast<Trgbe>((r > 0) * static_cast<int>(r * d));
                pixel.g = static_cast<Trgbe>((g > 0) * static_cast<int>(g * d));
                pixel.b = static_cast<Trgbe>((b > 0) * static_cast<int>(b * d));
                pixel.e = static_cast<Trgbe>(e + 128);
            }
            rowR[x] = pixel.r;
            rowG[x] = pixel.g;
            rowB[x] = pixel.b;
            rowE[x] = pixel.e;
        }
        if (image.width < 8 || image.width > 0x7fff) {
            // Radiance fwritecolrs(): scanlines outside [MINELEN, MAXELEN] are written flat.
            for (size_t x = 0; x < image.width; ++x) {
                const Trgbe flat[4] = {rowR[x], rowG[x], rowB[x], rowE[x]};
                std::fwrite(flat, sizeof(Trgbe), 4, file);
            }
            continue;
        }
        unsigned char header[4];
        header[0] = 2;
        header[1] = 2;
        header[2] = static_cast<unsigned char>(image.width >> 8);
        header[3] = static_cast<unsigned char>(image.width & 0xFF);
        std::fwrite(header, sizeof(header), 1, file);
        writeRleScanline(file, rowR.data(), static_cast<int>(image.width));
        writeRleScanline(file, rowG.data(), static_cast<int>(image.width));
        writeRleScanline(file, rowB.data(), static_cast<int>(image.width));
        writeRleScanline(file, rowE.data(), static_cast<int>(image.width));
    }
    std::fclose(file);
}

void quantizeRadianceRgbRoundTrip(std::vector<float> &rgb) {
    for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
        TrgbePixel pixel{};
        rgb2rgbeMatlabCompatible(rgb[i + 0], rgb[i + 1], rgb[i + 2], pixel);
        if (pixel.e == 0) {
            rgb[i + 0] = 0.0f;
            rgb[i + 1] = 0.0f;
            rgb[i + 2] = 0.0f;
            continue;
        }
        const float scale = rgbeScale(pixel.e);
        rgb[i + 0] = (static_cast<float>(pixel.r) + 0.5f) * scale;
        rgb[i + 1] = (static_cast<float>(pixel.g) + 0.5f) * scale;
        rgb[i + 2] = (static_cast<float>(pixel.b) + 0.5f) * scale;
    }
}

namespace {

float quantizeRadianceGrayRoundTripImpl(float value, bool matlabCompatible) {
    TrgbePixel pixel{};
    if (matlabCompatible)
        rgb2rgbeMatlabCompatible(value, value, value, pixel);
    else
        rgb2rgbe(value, value, value, pixel);
    if (pixel.e == 0)
        return 0.0f;
    const float scale = rgbeScale(pixel.e);
    return (static_cast<float>(pixel.r) + 0.5f) * scale;
}

} // namespace

float quantizeRadianceGrayRoundTrip(float value) {
    return quantizeRadianceGrayRoundTripImpl(value, false);
}

float quantizeRadianceGrayRoundTripMatlabCompatible(float value) {
    return quantizeRadianceGrayRoundTripImpl(value, true);
}
