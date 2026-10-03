#include "hdrmerge.h"

#include <boost/format.hpp>
#include <cstdio>
#include <vector>

#if MERGEHDR_ENABLE_OPENEXR
#include <ImfOutputFile.h>
#include <ImfChannelList.h>
#include <ImfStringAttribute.h>
#include <ImfHeader.h>
#include <ImfFrameBuffer.h>
#include <Imath/half.h>
#endif

extern "C" {
    #include <jpeglib.h>
    #include <jerror.h>
};

namespace {

const float WHITE_EFFICACY = 179.0f;

typedef unsigned char Trgbe;
struct TrgbePixel {
    Trgbe r;
    Trgbe g;
    Trgbe b;
    Trgbe e;
};

void rgb2rgbe(float r, float g, float b, TrgbePixel &rgbe) {
    r = std::max(0.0f, r);
    g = std::max(0.0f, g);
    b = std::max(0.0f, b);

    double v = r;
    if (v < g)
        v = g;
    if (v < b)
        v = b;

    if (v < 1e-32) {
        rgbe.r = rgbe.g = rgbe.b = rgbe.e = 0;
    } else {
        int e;
        v = frexp(v, &e) * 256.0 / v;
        const auto quantizeMantissa = [](double value) -> Trgbe {
            return Trgbe(std::max(0, std::min(255, static_cast<int>(value))));
        };
        rgbe.r = quantizeMantissa(v * r);
        rgbe.g = quantizeMantissa(v * g);
        rgbe.b = quantizeMantissa(v * b);
        rgbe.e = Trgbe(e + 128);
    }
}

int RLEWrite(FILE *file, Trgbe *scanline, int size) {
    Trgbe *scanend = scanline + size;
    while (scanline < scanend) {
        int run_start = 0;
        int peek = 0;
        int run_len = 0;
        while (run_len <= 4 && peek < 128 && scanline + peek < scanend) {
            run_start = peek;
            run_len = 0;
            while (run_len < 127 && run_start + run_len < 128 &&
                   scanline + peek < scanend &&
                   scanline[run_start] == scanline[peek]) {
                peek++;
                run_len++;
            }
        }

        if (run_len > 4) {
            if (run_start > 0) {
                std::vector<Trgbe> buf(run_start + 1);
                buf[0] = run_start;
                for (int i=0; i<run_start; ++i)
                    buf[i+1] = scanline[i];
                fwrite(buf.data(), sizeof(Trgbe), run_start + 1, file);
            }

            Trgbe buf[2];
            buf[0] = 128 + run_len;
            buf[1] = scanline[run_start];
            fwrite(buf, sizeof(Trgbe), 2, file);
        } else {
            std::vector<Trgbe> buf(peek + 1);
            buf[0] = peek;
            for (int i=0; i<peek; ++i)
                buf[i+1] = scanline[i];
            fwrite(buf.data(), sizeof(Trgbe), peek + 1, file);
        }
        scanline += peek;
    }

    if (scanline != scanend)
        throw std::runtime_error("RGBE: difference in size while writing RLE scanline");

    return 0;
}

} // namespace

void writeOpenEXR(const std::string &filename, size_t w, size_t h, int nChannels, float *data, const StringMap &metadata, bool writeHalf) {
#if !MERGEHDR_ENABLE_OPENEXR
    (void) filename;
    (void) w;
    (void) h;
    (void) nChannels;
    (void) data;
    (void) metadata;
    (void) writeHalf;
    throw std::runtime_error("This mergehdr build was compiled without OpenEXR support.");
#else
    Imf::setGlobalThreadCount(getProcessorCount());

    Imf::Header header(w, h);
    for (StringMap::const_iterator it = metadata.begin(); it != metadata.end(); ++it)
        header.insert(it->first.c_str(), Imf::StringAttribute(it->second.c_str()));

    Imf::ChannelList &channels = header.channels();

    cout << "Writing " << filename << " (" << w << "x" << h << ", " << nChannels
         << " channels, " << (writeHalf ? "half" : "single") << " precision) .. " << endl;
    if (nChannels == 3) {
        if (writeHalf) {
            channels.insert("R", Imf::Channel(Imf::HALF));
            channels.insert("G", Imf::Channel(Imf::HALF));
            channels.insert("B", Imf::Channel(Imf::HALF));

            /* Though it would be nicer to do the conversion scanline by scanline,
               this would prevent us from using OpenEXR's multithreading abilities.
               Hence, convert everything at once with a full-sized buffer */
            half *buffer = new half[3*w*h];
            for (size_t j=0; j<3*w*h; ++j)
                buffer[j] = *data++;

            Imf::FrameBuffer frameBuffer;
            frameBuffer.insert("R", Imf::Slice(Imf::HALF, (char *) buffer,   6, 6*w));
            frameBuffer.insert("G", Imf::Slice(Imf::HALF, (char *) buffer+2, 6, 6*w));
            frameBuffer.insert("B", Imf::Slice(Imf::HALF, (char *) buffer+4, 6, 6*w));

            Imf::OutputFile file(filename.c_str(), header);
            file.setFrameBuffer(frameBuffer);
            file.writePixels(h);
            delete[] buffer;
        } else {
            channels.insert("R", Imf::Channel(Imf::FLOAT));
            channels.insert("G", Imf::Channel(Imf::FLOAT));
            channels.insert("B", Imf::Channel(Imf::FLOAT));
            Imf::FrameBuffer frameBuffer;
            frameBuffer.insert("R", Imf::Slice(Imf::FLOAT, (char *) data,   12, 12*w));
            frameBuffer.insert("G", Imf::Slice(Imf::FLOAT, (char *) data+4, 12, 12*w));
            frameBuffer.insert("B", Imf::Slice(Imf::FLOAT, (char *) data+8, 12, 12*w));
            Imf::OutputFile file(filename.c_str(), header);
            file.setFrameBuffer(frameBuffer);
            file.writePixels(h);
        }
    } else if (nChannels == 1) {
        if (writeHalf) {
            channels.insert("Y", Imf::Channel(Imf::HALF));

            /* Though it would be nicer to do the conversion scanline by scanline,
               this would prevent us from using OpenEXR's multithreading abilities.
               Hence, convert everything at once with a full-sized buffer */
            half *buffer = new half[w*h];
            for (size_t j=0; j<w*h; ++j)
                buffer[j] = *data++;

            Imf::FrameBuffer frameBuffer;
            frameBuffer.insert("Y", Imf::Slice(Imf::HALF, (char *) buffer,   2, 2*w));

            Imf::OutputFile file(filename.c_str(), header);
            file.setFrameBuffer(frameBuffer);
            file.writePixels(h);
            delete[] buffer;
        } else {
            channels.insert("Y", Imf::Channel(Imf::FLOAT));
            Imf::FrameBuffer frameBuffer;
            frameBuffer.insert("Y", Imf::Slice(Imf::FLOAT, (char *) data,   4, 4*w));
            Imf::OutputFile file(filename.c_str(), header);
            file.setFrameBuffer(frameBuffer);
            file.writePixels(h);
        }
    } else {
        throw std::runtime_error("writeOpenEXR(): unknown number of channels!");
    }
#endif
}

extern "C" {
    METHODDEF(void) jpeg_error_exit (j_common_ptr cinfo) noexcept(false) {
        char msg[JMSG_LENGTH_MAX];
        (*cinfo->err->format_message) (cinfo, msg);
        throw std::runtime_error((boost::format("Critcal libjpeg error: %1%") % msg).str());
    }
};

void writeJPEG(const std::string &filename, size_t w, size_t h, float *data, int quality) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    FILE *file = fopen(filename.c_str(), "w");
    if (!file)
        throw std::runtime_error("Unable to open output file");

    cout << "Writing " << filename << " (" << w << "x" << h << ", "
         << "3 channels, low dynamic range) .. " << endl;

    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = jpeg_error_exit;
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, file);

    cinfo.image_width = (int) w;
    cinfo.image_height = (int) h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    uint8_t *buffer = new uint8_t[w * h * 3];
    uint8_t **scanlines = new uint8_t*[h];

    #pragma omp parallel for
    for (int i=0; i<h; ++i) {
        float *in_ptr = data + w * i * 3;
        uint8_t *out_ptr = buffer + w * i * 3;
        scanlines[i] = out_ptr;

        for (int j=0; j<3*w; ++j) {
            float value = *in_ptr++;
            if (value <= 0.0031308f)
                value = 12.92f * value;
            else
                value = 1.055f * std::pow(value, 1.0f/2.4f) - 0.055f;

            *out_ptr ++ = (uint8_t) std::max(std::min(255.0f, std::round(value * 255.0f)), 0.0f);
        }
    }
    jpeg_write_scanlines(&cinfo, scanlines, (int) h);

    /* Release the libjpeg data structures */
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    delete[] buffer;
    delete[] scanlines;
    fclose(file);
}

void writeRGBE(const std::string &filename, size_t w, size_t h, float *data,
               const std::vector<std::string> &headerLines, bool radianceCompatibility) {
    FILE *file = fopen(filename.c_str(), "wb");
    if (!file)
        throw std::runtime_error("Unable to open output file");

    cout << "Writing " << filename << " (" << w << "x" << h
         << ", 3 channels, Radiance RGBE) .. " << endl;

    fprintf(file, "#?RADIANCE\n");
    for (const std::string &line : headerLines) {
        if (line.rfind("VIEW=", 0) != 0)
            fprintf(file, "%s\n", line.c_str());
    }
    for (const std::string &line : headerLines) {
        if (line.rfind("VIEW=", 0) == 0)
            fprintf(file, "%s\n", line.c_str());
    }
    fprintf(file, "FORMAT=32-bit_rle_rgbe\n");
    fprintf(file, "\n");
    fprintf(file, "-Y %d +X %d\n", (int) h, (int) w);

    std::vector<Trgbe> scanlineR(w * h), scanlineG(w * h), scanlineB(w * h), scanlineE(w * h);

    #pragma omp parallel for
    for (int y=0; y<(int) h; ++y) {
        const float *row = data + y * w * 3;
        Trgbe *rowR = scanlineR.data() + y * w;
        Trgbe *rowG = scanlineG.data() + y * w;
        Trgbe *rowB = scanlineB.data() + y * w;
        Trgbe *rowE = scanlineE.data() + y * w;

        for (size_t x=0; x<w; ++x) {
            TrgbePixel pixel;
            float r = row[3*x + 0];
            float g = row[3*x + 1];
            float b = row[3*x + 2];
            if (radianceCompatibility) {
                r /= WHITE_EFFICACY;
                g /= WHITE_EFFICACY;
                b /= WHITE_EFFICACY;
            }
            rgb2rgbe(r, g, b, pixel);
            rowR[x] = pixel.r;
            rowG[x] = pixel.g;
            rowB[x] = pixel.b;
            rowE[x] = pixel.e;
        }
    }

    for (size_t y=0; y<h; ++y) {
        unsigned char header[4];
        header[0] = 2;
        header[1] = 2;
        header[2] = (unsigned char) (w >> 8);
        header[3] = (unsigned char) (w & 0xFF);
        fwrite(header, sizeof(header), 1, file);

        RLEWrite(file, scanlineR.data() + y * w, (int) w);
        RLEWrite(file, scanlineG.data() + y * w, (int) w);
        RLEWrite(file, scanlineB.data() + y * w, (int) w);
        RLEWrite(file, scanlineE.data() + y * w, (int) w);
    }

    fclose(file);
}
