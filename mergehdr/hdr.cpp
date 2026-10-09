#include "hdrmerge.h"
#include <string.h>
#include <limits>
#include <unordered_set>
#include <Eigen/QR>

#if defined(_OPENMP)
#  include <omp.h>
#else
inline int omp_get_max_threads() { return 1; }
inline int omp_get_thread_num() { return 0; }
#endif

namespace {

inline int raw_fc_value(int filter, int x, int y) {
    return (filter >> (((y << 1 & 14) + (x & 1)) << 1)) & 3;
}

inline int shift_filter_pattern(int filter, int offs_x, int offs_y) {
    int shifted = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 2; ++x) {
            int color = raw_fc_value(filter, x + offs_x, y + offs_y);
            shifted |= color << (((y << 1 & 14) + (x & 1)) << 1);
        }
    }
    return shifted;
}

inline void sample_bilinear_rgb(const float3 *src, size_t width, size_t height,
        double x_center, double y_center, float *out) {
    x_center = std::max(0.5, std::min((double) width  - 0.5, x_center));
    y_center = std::max(0.5, std::min((double) height - 0.5, y_center));

    double xi = x_center - 0.5;
    double yi = y_center - 0.5;

    int x0 = (int) std::floor(xi);
    int y0 = (int) std::floor(yi);
    int x1 = std::min<int>(x0 + 1, (int) width  - 1);
    int y1 = std::min<int>(y0 + 1, (int) height - 1);

    double tx = xi - x0;
    double ty = yi - y0;

    const float *p00 = src[y0 * width + x0];
    const float *p10 = src[y0 * width + x1];
    const float *p01 = src[y1 * width + x0];
    const float *p11 = src[y1 * width + x1];

    for (int c=0; c<3; ++c) {
        double a = p00[c] * (1.0 - tx) + p10[c] * tx;
        double b = p01[c] * (1.0 - tx) + p11[c] * tx;
        out[c] = (float) (a * (1.0 - ty) + b * ty);
    }
}

inline uint64_t pixel_key(int x, int y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32) |
        static_cast<uint32_t>(x);
}

} // namespace

float compute_weight(uint16_t value, uint16_t blacklevel, float saturation) {
    const float alpha = -1.0f / 10.0f;
    const float beta = 1.0f / std::exp(4.0f*alpha);

    float scaled = (value - blacklevel) / (float) (saturation - blacklevel);

    if (scaled <= 0 || scaled >= 1)
        return 0;

    return beta * std::exp(alpha * (1/scaled + 1/(1-scaled)));
}

static inline float compute_weight_pylinear(float scaled, float saturation_offset) {
    if (scaled < 0.0f || scaled > 1.0f - saturation_offset)
        return 0.0f;
    return std::exp(-0.01f/scaled - 0.01f/(1.0f - saturation_offset - scaled)) + 1e-6f;
}

void ExposureSeries::initTables(float saturation, EMergeMethod method, float underexposed) {
    this->merge_method = method;
    this->underexposed = std::max(0.0f, underexposed);
    this->saturation = saturation;

    /* Compute relative exposure table */
    for (int i=0; i<0x10000; ++i) {
        float scaled = (float) (i - blacklevel) / (float) (whitepoint - blacklevel);
        if (method == EMergePyLinear)
            scaled = clamp(scaled, 0.0f, 1.0f);
        value_tbl[i] = scaled;
    }

    if (size() == 1 && method != EMergePyLinear)
        return; /* The calibrated fast path below does not need weights. */

    if (saturation == 0) {
        /* Determine the value of a pixel considered to be overexposured */
        size_t npix = width*height;
        uint16_t *temp = new uint16_t[npix];
        memcpy(temp, exposures[size()-1].image, npix*sizeof(uint16_t));
        size_t percentile = (size_t) (npix*0.999);
        std::nth_element(temp, temp+percentile, temp+npix);
        saturation = (*(temp+percentile)-blacklevel) / (float) (whitepoint-blacklevel);
        delete[] temp;

        cerr << endl
             << "*******************************************************************************" << endl
             << "Warning: The HDR merging step needs to know the sensor's saturation threshold." << endl
             << "This is the percentage of the sensor's theoretical dynamic range, at which" << endl
             << "saturation occurs in practice. Based on the brightest image region in your " << endl
             << "longest exposure, this was estimated to be around " << saturation*100 << "\% (for Canon cameras," << endl
             << "this number is usually around 80\%). This estimatimation of course only" << endl
             << "works if your longest exposure does indeed contain overexposed pixels..." << endl
             << endl
             << "If you are going to process a larger set of measurements, it is advisable to" << endl
             << "lock this parameter in a config or profile file with \"saturation=" << saturation << "\"" << endl
             << "*******************************************************************************" << endl
             << endl;
    }

    saturation = saturation * (whitepoint-blacklevel) + blacklevel;
    float saturation_offset = std::max(0.0f, 1.0f - this->saturation);

    /* Precompute weight table */
    for (int i=0; i<0x10000; ++i) {
        if (method == EMergePyLinear)
            weight_tbl[i] = compute_weight_pylinear(value_tbl[i], saturation_offset);
        else
            weight_tbl[i] = compute_weight((uint16_t) i, blacklevel, saturation);
    }
}

void ExposureSeries::repairBadPixels() {
    if (badpixels.empty() || exposures.empty())
        return;

    std::vector<std::pair<int, int>> coords = badpixels;
    std::sort(coords.begin(), coords.end());
    coords.erase(std::unique(coords.begin(), coords.end()), coords.end());

    std::unordered_set<uint64_t> badset;
    badset.reserve(coords.size() * 2);
    for (const auto &coord : coords)
        badset.insert(pixel_key(coord.first, coord.second));

    size_t repaired = 0;
    size_t skipped = 0;

    auto repair_single = [&](uint16_t *image, int x, int y) -> bool {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height))
            return false;

        int channel = fc(x, y);
        std::vector<uint16_t> samples;
        samples.reserve(24);

        auto gather = [&](int radius) {
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (dx == 0 && dy == 0)
                        continue;
                    int xs = x + dx;
                    int ys = y + dy;
                    if (xs < 0 || ys < 0 || xs >= static_cast<int>(width) || ys >= static_cast<int>(height))
                        continue;
                    if (fc(xs, ys) != channel)
                        continue;
                    if (badset.find(pixel_key(xs, ys)) != badset.end())
                        continue;
                    samples.push_back(image[xs + ys * width]);
                }
            }
        };

        gather(2);
        if (samples.size() < 3) {
            samples.clear();
            gather(4);
        }
        if (samples.empty())
            return false;

        size_t mid = samples.size() / 2;
        std::nth_element(samples.begin(), samples.begin() + mid, samples.end());
        image[x + y * width] = samples[mid];
        return true;
    };

    cout << "Repairing " << coords.size() << " bad pixel"
         << (coords.size() == 1 ? "" : "s") << " ..";
    cout.flush();

    for (size_t i = 0; i < exposures.size(); ++i) {
        for (const auto &coord : coords) {
            if (repair_single(exposures[i].image, coord.first, coord.second))
                ++repaired;
            else
                ++skipped;
        }
    }

    cout << " done (" << repaired << " repaired";
    if (skipped)
        cout << ", " << skipped << " skipped";
    cout << ")" << endl;
}

void ExposureSeries::applyRawMultipliers(const float multipliers[3]) {
    const float lowest = std::min(multipliers[0], std::min(multipliers[1], multipliers[2]));
    if (!(lowest > 0.0f) || !std::isfinite(multipliers[0]) || !std::isfinite(multipliers[1]) ||
            !std::isfinite(multipliers[2]))
        throw std::runtime_error("Raw multipliers must be positive numbers.");

    /* LibRaw: pre_mul[c] /= min(pre_mul) (highlight mode 0), value = CLIP((raw - black) * pre_mul[c] * 65535 / (white - black)) */
    double factor[3];
    bool identity = true;
    for (int c = 0; c < 3; ++c) {
        factor[c] = static_cast<double>(multipliers[c]) / static_cast<double>(lowest);
        identity = identity && factor[c] == 1.0;
    }
    cout << "Applying raw multipliers " << multipliers[0] << ", " << multipliers[1] << ", " << multipliers[2]
         << " to the RAW data" << endl;
    if (identity)
        return;

    const double black = blacklevel, white = whitepoint;
    for (size_t img = 0; img < size(); ++img) {
        uint16_t *image = exposures[img].image;
        if (!image)
            continue;
        #pragma omp parallel for
        for (int y = 0; y < static_cast<int>(height); ++y) {
            uint16_t *row = image + static_cast<size_t>(y) * width;
            for (int x = 0; x < static_cast<int>(width); ++x) {
                const double f = factor[fc(x, y)];
                if (f == 1.0)
                    continue;
                const double scaled = black + (static_cast<double>(row[x]) - black) * f;
                row[x] = static_cast<uint16_t>(std::clamp(scaled, 0.0, white) + 0.5);
            }
        }
    }
}

void ExposureSeries::merge() {
    image_merged = new float[width * height];

    /* Preserve the inexpensive one-frame path for the standard merger, but
       put the normalized sensor value on the same exposure-calibrated scale
       as the multi-frame path.  EMergePyLinear intentionally falls through
       so its neighborhood-based saturation/bloom handling is retained. */
    if (size() == 1 && merge_method != EMergePyLinear) {
        const float exposure = exposures[0].exposure;
        if (!(std::isfinite(exposure) && exposure > 0.0f))
            throw std::runtime_error(
                "A single exposure must have a positive, finite effective exposure.");

        cout << "Calibrating one exposure .." << endl;
        #pragma omp parallel for
        for (int y=0; y<height; ++y) {
            uint16_t *src = exposures[0].image + y * width;
            float *dst = image_merged + y * width;
            for (int x=0; x<width; ++x)
                *dst++ = value_tbl[*src++] / exposure;
        }
        exposures[0].release();
        return;
    }

    cout << "Merging " << size() << " exposures .." << endl;
    #pragma omp parallel for
    for (int y=0; y<height; ++y) {
        uint32_t offset = y * width;
        for (int x=0; x<width; ++x) {
            if (merge_method == EMergePyLinear) {
                float value = 0.0f, total_exposure = 0.0f;
                float high = 0.0f, low = std::numeric_limits<float>::infinity();
                bool all_under = true, all_over = true;
                float saturation_offset = std::max(0.0f, 1.0f - saturation);
                int color = fc(x, y);
                float wsp = white_saturation[color];

                for (size_t img=0; img<size(); ++img) {
                    uint16_t pxvalue = exposures[img].image[offset];
                    float scaled = value_tbl[pxvalue];
                    bool saturated_exp = scaled >= saturation;
                    bool under_exp = scaled < underexposed;
                    float saturation_same = 0.0f, saturation_other = 0.0f, under = 1.0f;

                    int y0 = std::max(0, y - 2), y1 = std::min((int) height - 1, y + 2);
                    int x0 = std::max(0, x - 2), x1 = std::min((int) width - 1, x + 2);
                    for (int ys = y0; ys <= y1; ++ys) {
                        uint32_t row = ys * width;
                        for (int xs = x0; xs <= x1; ++xs) {
                            float neighbor = value_tbl[exposures[img].image[row + xs]];
                            if (fc(xs, ys) == color)
                                saturation_same = std::max(saturation_same, neighbor);
                            else
                                saturation_other = std::max(saturation_other, neighbor);
                            under = std::min(under, neighbor);
                        }
                    }

                    saturated_exp = saturation_same >= saturation || saturation_other >= saturation;
                    high = std::max(high, std::min(scaled, wsp) / exposures[img].exposure);
                    low = std::min(low, scaled / exposures[img].exposure);

                    all_under &= under_exp;
                    all_over &= saturated_exp;

                    if (!(saturated_exp || under_exp)) {
                        float weight = std::min(
                            compute_weight_pylinear(std::max(saturation_same, saturation_other), saturation_offset),
                            compute_weight_pylinear(under, saturation_offset)
                        );
                        value += scaled * weight;
                        total_exposure += exposures[img].exposure * weight;
                    }
                }

                if (all_under)
                    value = std::isfinite(low) ? low : 0.0f;
                else if (all_over)
                    value = high;
                else if (total_exposure > 0.0f)
                    value /= total_exposure;
                else
                    value = high;

                image_merged[offset++] = value;
                continue;
            }

            float value = 0, total_exposure = 0;
            float high = 0.0f, low = std::numeric_limits<float>::infinity();
            bool all_under = true, all_over = true;
            int color = fc(x, y);
            float wsp = white_saturation[color];

            /* Pass 1: Compute pixel intensity based on a simple
               Poisson model of arriving photons. Use weighting
               to discard over/under-exposed pixels */
            for (int img=0; img<size(); ++img) {
                uint16_t pxvalue = exposures[img].image[offset];
                float scaled = value_tbl[pxvalue];
                float weight = weight_tbl[pxvalue];
                value += scaled * weight;
                total_exposure += exposures[img].exposure * weight;
                high = std::max(high, std::min(scaled, wsp) / exposures[img].exposure);
                low = std::min(low, scaled / exposures[img].exposure);
                all_under &= scaled < underexposed;
                all_over &= scaled >= saturation;
            }
            if (total_exposure > 0) {
                value /= total_exposure;
            } else {
                /* No good exposures for this pixel! */
                if (all_under && std::isfinite(low))
                    value = low;
                else if (all_over)
                    value = high;
                else {
                    /* As in the original hdrmerge: the unweighted sum is kept undivided */
                    for (size_t img=0; img<size(); ++img) {
                        uint16_t pxvalue = exposures[img].image[offset];
                        value += value_tbl[pxvalue];
                        total_exposure += exposures[img].exposure;
                    }
                }
            }

            float reference = value;
            value = total_exposure = 0;

            /* To reduce bias, the above estimation is carried out once
               more -- but this times, the weight values are computed
               using intensities predicted by the first estimate */
            float blacklevel = this->blacklevel, scale = this->whitepoint - blacklevel;
            for (size_t img=0; img<size(); ++img) {
                float predicted = reference * exposures[img].exposure * scale + blacklevel;
                uint16_t pxvalue = exposures[img].image[offset];

                if (predicted <= 0 || predicted >= 65535.0f)
                    continue;

                float weight = weight_tbl[(uint16_t) (predicted + 0.5f)];
                value += value_tbl[pxvalue] * weight;
                total_exposure += exposures[img].exposure * weight;
            }

            if (total_exposure > 0)
                value /= total_exposure;
            else if (all_under && std::isfinite(low))
                value = low;
            else if (all_over)
                value = high;
            else
                value = reference;

            image_merged[offset++] = value;
        }
    }
    for (size_t i=0; i<exposures.size(); ++i)
        exposures[i].release();
}

void ExposureSeries::mergeDemosaicFirst(const float *cam2rgb, bool median) {
    /* Port of pylinearhdr --interpfirst (rawconvert -q DHT per exposure, then linearhdr -B, i.e.
       linearhdr.cpp merge_rgb). Default mergehdr behaviour (merge on the mosaic, then demosaic)
       is unchanged; this path is used only with --demosaicfirst. */
    if (merge_method != EMergePyLinear)
        throw std::runtime_error("--demosaicfirst requires the linearhdr merge style.");
    const size_t npix = (size_t) width * height;
    const float sat_off = std::max(0.0f, 1.0f - saturation);
    const float sat_lim = 1.0f - sat_off;
    const float blk_off = underexposed;
    /* linearhdr.cpp get_weight() */
    auto lin_weight = [sat_off](float x) {
        return std::exp(-0.01f / x - 0.01f / (1.0f - sat_off - x)) + 1e-6f;
    };
    std::vector<float> acc(3 * npix, 0.0f), high(3 * npix, 0.0f),
        low(3 * npix, std::numeric_limits<float>::infinity()), div(npix, 0.0f);
    std::vector<uint8_t> all_under(npix, 1), all_over(npix, 1);

    cout << "Merging " << size() << " exposures (demosaic first, linearhdr RGB merge) .." << endl;
    for (size_t img = 0; img < size(); ++img) {
        /* demosaicDHT() frees image_merged after interpolation, so allocate it per exposure */
        if (image_merged)
            delete[] image_merged;
        image_merged = new float[npix];
        const uint16_t *src = exposures[img].image;
        #pragma omp parallel for
        for (ptrdiff_t i = 0; i < (ptrdiff_t) npix; ++i)
            image_merged[i] = value_tbl[src[i]];
        exposures[img].release();
        demosaicDHT(*this, median, false);
        const float ec = exposures[img].exposure;
        #pragma omp parallel for
        for (ptrdiff_t k = 0; k < (ptrdiff_t) npix; ++k) {
            float irgb[3], orgb[3];
            for (int c = 0; c < 3; ++c)   /* rawconvert writes a clipped 16-bit TIFF */
                irgb[c] = clamp(image_demosaiced[k][c], 0.0f, 1.0f);
            float satv = std::max(irgb[0], std::max(irgb[1], irgb[2]));
            float under = std::min(irgb[0], std::min(irgb[1], irgb[2]));
            bool saturated_exp = satv >= sat_lim;
            bool under_exp = under < blk_off;
            for (int i = 0; i < 3; ++i)   /* linearhdr apply_color_transform() */
                orgb[i] = std::max(0.0f, cam2rgb[3*i] * irgb[0] + cam2rgb[3*i+1] * irgb[1] + cam2rgb[3*i+2] * irgb[2]);
            saturated_exp |= std::max(orgb[0], std::max(orgb[1], orgb[2])) >= sat_lim;
            under_exp |= std::min(orgb[0], std::min(orgb[1], orgb[2])) < blk_off;
            all_under[k] &= under_exp;
            all_over[k] &= saturated_exp;
            const bool in_range = !(saturated_exp || under_exp);
            const float w = std::min(lin_weight(satv), lin_weight(under));
            if (in_range)
                div[k] += w * ec;
            for (int c = 0; c < 3; ++c) {
                size_t kc = 3 * (size_t) k + c;
                high[kc] = std::max(high[kc], std::min(irgb[c], white_saturation[c]) / ec);
                low[kc] = std::min(low[kc], irgb[c] / ec);
                if (in_range)
                    acc[kc] += w * irgb[c];
            }
        }
    }
    #pragma omp parallel for
    for (ptrdiff_t k = 0; k < (ptrdiff_t) npix; ++k) {
        const bool over = all_over[k] || div[k] == 0.0f;
        for (int c = 0; c < 3; ++c) {
            size_t kc = 3 * (size_t) k + c;
            if (all_under[k])
                image_demosaiced[k][c] = std::isfinite(low[kc]) ? low[kc] : 0.0f;
            else if (over)
                image_demosaiced[k][c] = high[kc];
            else
                image_demosaiced[k][c] = acc[kc] / div[k];
        }
    }
    if (image_merged)
        delete[] image_merged;
    image_merged = NULL;
}

RawChannelSampleStats ExposureSeries::sampleRawChannel(int channel) {
    if (channel < 0 || channel > 2)
        throw std::runtime_error("Raw sample channel must be 0 (r), 1 (g), or 2 (b).");

    RawChannelSampleStats stats;

    if (size() == 0)
        return stats;

    #pragma omp parallel
    {
        double localSum = 0.0;
        size_t localValid = 0;
        size_t localTotal = 0;

        #pragma omp for
        for (int y = 0; y < static_cast<int>(height); ++y) {
            uint32_t offset = y * width;
            for (int x = 0; x < static_cast<int>(width); ++x) {
                if (fc(x, y) != channel) {
                    ++offset;
                    continue;
                }

                ++localTotal;

                if (size() == 1) {
                    float scaled = value_tbl[exposures[0].image[offset]];
                    bool valid = scaled >= underexposed && scaled < saturation;
                    if (valid && exposures[0].exposure > 0.0f) {
                        localSum += scaled / exposures[0].exposure;
                        ++localValid;
                    }
                } else if (merge_method == EMergePyLinear) {
                    float value = 0.0f, total_exposure = 0.0f;
                    bool all_under = true, all_over = true;
                    float saturation_offset = std::max(0.0f, 1.0f - saturation);
                    float wsp = white_saturation[channel];

                    for (size_t img = 0; img < size(); ++img) {
                        uint16_t pxvalue = exposures[img].image[offset];
                        float scaled = value_tbl[pxvalue];
                        bool under_exp = scaled < underexposed;
                        float saturation_same = 0.0f, saturation_other = 0.0f, under = 1.0f;

                        int y0 = std::max(0, y - 2), y1 = std::min((int) height - 1, y + 2);
                        int x0 = std::max(0, x - 2), x1 = std::min((int) width - 1, x + 2);
                        for (int ys = y0; ys <= y1; ++ys) {
                            uint32_t row = ys * width;
                            for (int xs = x0; xs <= x1; ++xs) {
                                float neighbor = value_tbl[exposures[img].image[row + xs]];
                                if (fc(xs, ys) == channel)
                                    saturation_same = std::max(saturation_same, neighbor);
                                else
                                    saturation_other = std::max(saturation_other, neighbor);
                                under = std::min(under, neighbor);
                            }
                        }

                        bool saturated_exp = saturation_same >= saturation || saturation_other >= saturation;
                        (void) wsp;
                        all_under &= under_exp;
                        all_over &= saturated_exp;

                        if (!(saturated_exp || under_exp)) {
                            float weight = std::min(
                                compute_weight_pylinear(std::max(saturation_same, saturation_other), saturation_offset),
                                compute_weight_pylinear(under, saturation_offset)
                            );
                            value += scaled * weight;
                            total_exposure += exposures[img].exposure * weight;
                        }
                    }

                    if (!(all_under || all_over) && total_exposure > 0.0f) {
                        localSum += value / total_exposure;
                        ++localValid;
                    }
                } else {
                    float value = 0.0f, total_exposure = 0.0f;
                    bool all_under = true, all_over = true;

                    for (size_t img = 0; img < size(); ++img) {
                        uint16_t pxvalue = exposures[img].image[offset];
                        float scaled = value_tbl[pxvalue];
                        float weight = weight_tbl[pxvalue];
                        value += scaled * weight;
                        total_exposure += exposures[img].exposure * weight;
                        all_under &= scaled < underexposed;
                        all_over &= scaled >= saturation;
                    }

                    if (!(all_under || all_over) && total_exposure > 0.0f) {
                        localSum += value / total_exposure;
                        ++localValid;
                    }
                }

                ++offset;
            }
        }

        #pragma omp critical
        {
            stats.average += localSum;
            stats.validSamples += localValid;
            stats.totalSamples += localTotal;
        }
    }

    if (stats.validSamples > 0)
        stats.average /= static_cast<double>(stats.validSamples);
    if (stats.totalSamples > 0)
        stats.fraction = static_cast<double>(stats.validSamples) / static_cast<double>(stats.totalSamples);

    return stats;
}

void ExposureSeries::demosaic(float *sensor2xyz, EDemosaicMethod method) {
    if (method == EDemosaicDHT) {
        demosaicDHT(*this, true);
        return;
    }

    /* This function is based on the AHD code from dcraw, which in turn
       builds on work by Keigo Hirakawa, Thomas Parks, and Paul Lee. */
    const int G = 1, tsize = 256;

    struct DemosaicBuffer {
        /* Horizontally and vertically interpolated sensor colors */
        float3 rgb[2][tsize][tsize];

        /* CIElab color values */
        float3 cielab[2][tsize][tsize];

        /* Homogeneity map */
        uint8_t homo[2][tsize][tsize];
    };

    /* Temporary tile storage */
    DemosaicBuffer *buffers = new DemosaicBuffer[omp_get_max_threads()];

    cout << "AHD demosaicing .." << endl;

    /* Allocate a big buffer for the interpolated colors */
    image_demosaiced = new float3[width*height];

    size_t offset = 0;
    float maxvalue = 0;
    for (size_t y=0; y<height; ++y) {
        for (size_t x=0; x<width; ++x) {
            float value = image_merged[offset];
            image_demosaiced[offset][fc(x, y)] = value;
            if (value > maxvalue)
                maxvalue = value;
            offset++;
        }
    }

    /* The AHD implementation below doesn't interpolate colors on a 5-pixel wide
       boundary region -> use a naive averaging method on this region instead. */
    const size_t border = 5;
    for (size_t y=0; y<height; ++y) {
        for (size_t x=0; x<width; ++x) {
            if (x == border && y >= border && y < height-border)
                x = width-border; /* Jump over the center part of the image */

            float binval[3] = {0, 0, 0};
            int bincount[3] = {0, 0, 0};

            for (size_t ys=y-1; ys != y+2; ++ys) {
                for (size_t xs=x-1; xs != x+2; ++xs) {
                    if (ys < height && xs < width) {
                        int col = fc(xs, ys);
                        binval[col] += image_demosaiced[ys*width+xs][col];
                        ++bincount[col];
                    }
                }
            }

            int col = fc(x, y);
            for (int c=0; c<3; ++c) {
                if (col != c)
                    image_demosaiced[y*width+x][c] = bincount[c] ? (binval[c]/bincount[c]) : 1.0f;
            }
        }
    }

    /* Matrix that goes from sensor to normalized XYZ tristimulus values */
    float sensor2xyz_n[3][3], sensor2xyz_n_maxvalue = 0;
    const float d65_white[3] = { 0.950456, 1, 1.088754 };
    for (int i=0; i<3; ++i) {
        for (int j=0; j<3; ++j) {
            sensor2xyz_n[i][j] = sensor2xyz[i*3+j] / d65_white[i];
            sensor2xyz_n_maxvalue = std::max(sensor2xyz_n_maxvalue, sensor2xyz_n[i][j]);
        }
    }

    /* Scale factor that is guaranteed to push XYZ values into the range [0, 1] */
    float scale = 1.0 / (maxvalue * sensor2xyz_n_maxvalue);

    /* Precompute a table for the nonlinear part of the CIELab conversion */
    const int cielab_table_size = 0xFFFF;
    float cielab_table[cielab_table_size];
    for (int i=0; i<cielab_table_size; ++i) {
        float r = i * 1.0f / (cielab_table_size-1);
        cielab_table[i] = r > 0.008856 ? std::pow(r, 1.0f / 3.0f) : 7.787f*r + 4.0f/29.0f;
    }

    /* Process the image in tiles */
    std::vector<std::pair<size_t, size_t>> tiles;
    for (size_t top = 2; top < height - 5; top += tsize - 6)
        for (size_t left = 2; left < width - 5; left += tsize - 6)
            tiles.push_back(std::make_pair(left, top));

    #pragma omp parallel for /* Parallelize over tiles */
    for (int tile=0; tile<tiles.size(); ++tile) {
        DemosaicBuffer &buf = buffers[omp_get_thread_num()];
        size_t left = tiles[tile].first, top = tiles[tile].second;

        for (size_t y=top; y<top+tsize && y<height-2; ++y) {
            /* Interpolate green horizontally and vertically, starting
               at the first position where it is missing */
            size_t x = left + (fc(left, y) & 1), color = fc(x, y);

            for (; x<left+tsize && x<width-2; x += 2) {
                float3 *pix = image_demosaiced + y*width + x;

                float interp_h = 0.25f * ((pix[-1][G] + pix[0][color] + pix[1][G]) * 2
                      - pix[-2][color] - pix[2][color]);
                float interp_v = 0.25*((pix[-width][G] + pix[0][color] + pix[width][G]) * 2
                      - pix[-2*width][color] - pix[2*width][color]);

                /* Don't allow the interpolation to create new local maxima / minima */
                buf.rgb[0][y-top][x-left][G] = clamp(interp_h, pix[-1][G], pix[1][G]);
                buf.rgb[1][y-top][x-left][G] = clamp(interp_v, pix[-width][G], pix[width][G]);
            }
        }

        /* Interpolate red and blue, and convert to CIELab */
        for (int dir=0; dir<2; ++dir) {
            for (size_t y=top+1; y<top+tsize-1 && y<height-3; ++y) {
                for (size_t x = left+1; x<left+tsize-1 && x<width-3; ++x) {
                    float3 *pix = image_demosaiced + y*width + x;
                    float3 *interp = &buf.rgb[dir][y-top][x-left];
                    float3 *lab = &buf.cielab[dir][y-top][x-left];

                    /* Determine the color at the current pixel */
                    int color = fc(x, y);

                    if (color == G) {
                        color = fc(x, y+1);
                        /* Interpolate both red and green */
                        interp[0][2-color] = std::max(0.0f, pix[0][G] + (0.5f*(
                            pix[-1][2-color] + pix[1][2-color] - interp[-1][G] - interp[1][G])));

                        interp[0][color] = std::max(0.0f,  pix[0][G] + (0.5f*(
                            pix[-width][color] + pix[width][color] - interp[-tsize][1] - interp[tsize][1])));
                    } else {
                        /* Interpolate the other color */
                        color = 2 - color;
                        interp[0][color] = std::max(0.0f, interp[0][G] + (0.25f * (
                                pix[-width-1][color] + pix[-width+1][color]
                              + pix[+width-1][color] + pix[+width+1][color]
                              - interp[-tsize-1][G] - interp[-tsize+1][G]
                              - interp[+tsize-1][G] - interp[+tsize+1][G])));
                    }

                    /* Forward the color at the current pixel with out modification */
                    color = fc(x, y);
                    interp[0][color] = pix[0][color];

                    /* Convert to CIElab */
                    float xyz[3] = { 0, 0, 0 };
                    for (int i=0; i<3; ++i)
                        for (int j=0; j<3; ++j)
                            xyz[i] += sensor2xyz_n[i][j] * interp[0][j];

                    for (int i=0; i<3; ++i)
                        xyz[i] = cielab_table[std::max(0, std::min(cielab_table_size-1,
                                (int) (xyz[i] * scale * cielab_table_size)))];

                    lab[0][0] = (116.0f * xyz[1] - 16);
                    lab[0][1] = 500.0f * (xyz[0] - xyz[1]);
                    lab[0][2] = 200.0f * (xyz[1] - xyz[2]);
                }
            }
        }

        /*  Build homogeneity maps from the CIELab images: */
        const int offset_table[4] = { -1, 1, -tsize, tsize };
        memset(buf.homo, 0, 2*tsize*tsize);
        for (size_t y=top+2; y < top+tsize-2 && y < height-4; ++y) {
            for (size_t x=left+2; x< left+tsize-2 && x < width-4; ++x) {
                float ldiff[2][4], abdiff[2][4];

                for (int dir=0; dir < 2; dir++) {
                    float3 *lab = &buf.cielab[dir][y-top][x-left];

                    for (int i=0; i < 4; i++) {
                        int offset = offset_table[i];

                        /* Luminance and chromaticity differences in 4 directions,
                           for each of the two interpolated images */
                        ldiff[dir][i] = std::abs(lab[0][0] - lab[offset][0]);
                        abdiff[dir][i] = square(lab[0][1] - lab[offset][1])
                           + square(lab[0][2] - lab[offset][2]);
                    }
                }

                float leps  = std::min(std::max(ldiff[0][0], ldiff[0][1]),
                                       std::max(ldiff[1][2], ldiff[1][3]));
                float abeps = std::min(std::max(abdiff[0][0], abdiff[0][1]),
                                       std::max(abdiff[1][2], abdiff[1][3]));

                /* Count the number directions in which the above thresholds can
                   be maintained, for each of the two interpolated images */
                for (int dir=0; dir < 2; dir++)
                    for (int i=0; i < 4; i++)
                        if (ldiff[dir][i] <= leps && abdiff[dir][i] <= abeps)
                            buf.homo[dir][y-top][x-left]++;
            }
        }

        /*  Combine the most homogenous pixels for the final result */
        for (size_t y=top+3; y < top+tsize-3 && y < height-5; ++y) {
            for (size_t x=left+3; x < left+tsize-3 && x < width-5; ++x) {
                /* Look, which of the to images is more homogeneous in a 3x3 neighborhood */
                int hm[2] = {0, 0};
                for (int dir=0; dir < 2; dir++)
                    for (size_t i=y-top-1; i <= y-top+1; i++)
                        for (size_t j=x-left-1; j <= x-left+1; j++)
                            hm[dir] += buf.homo[dir][i][j];

                if (hm[0] != hm[1]) {
                    /* One of the images was more homogeneous */
                    for (int col=0; col<3; ++col)
                        image_demosaiced[y*width+x][col] = buf.rgb[hm[1] > hm[0] ? 1 : 0][y-top][x-left][col];
                } else {
                    /* No clear winner, blend */
                    for (int col=0; col<3; ++col)
                        image_demosaiced[y*width+x][col] = 0.5f*(buf.rgb[0][y-top][x-left][col]
                            + buf.rgb[1][y-top][x-left][col]);
                }
            }
        }
    }

    delete[] buffers;
    delete[] image_merged;
    image_merged = NULL;
}

void ExposureSeries::rawgrid() {
    if (image_demosaiced) {
        delete[] image_demosaiced;
        image_demosaiced = NULL;
    }

    image_demosaiced = new float3[width * height];

    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(height); ++y) {
        size_t row = static_cast<size_t>(y) * width;
        for (size_t x = 0; x < width; ++x) {
            size_t idx = row + x;
            image_demosaiced[idx][0] = 0.0f;
            image_demosaiced[idx][1] = 0.0f;
            image_demosaiced[idx][2] = 0.0f;
            image_demosaiced[idx][fc(static_cast<int>(x), y)] = image_merged[idx];
        }
    }
}

void ExposureSeries::transform_color(float *sensor2xyz, bool xyz) {
    const float xyz2rgb[3][3] = {
        { 3.240479f, -1.537150f, -0.498535f },
        {-0.969256f, +1.875991f, +0.041556f },
        { 0.055648f, -0.204043f, +1.057311f }
    };
    float M[3][3];

    if (xyz) {
        cout << "Transforming to XYZ color space .." << endl;
    } else {
        cout << "Transforming to sRGB color space .." << endl;
    }

    for (int i=0; i<3; ++i) {
        for (int j=0; j<3; ++j) {
            if (xyz) {
                M[i][j] = sensor2xyz[3*i+j];
            } else {
                float accum = 0;
                for (int k=0; k<3; ++k)
                    accum += xyz2rgb[i][k] * sensor2xyz[3*k+j];
                M[i][j] = accum;
            }
        }
    }

    #pragma omp parallel for
    for (int y=0; y<height; ++y) {
        float3 *ptr = image_demosaiced + y*width;
        for (size_t x=0; x<width; ++x) {
            float accum[3] = {0, 0, 0};
            for (int i=0; i<3; ++i)
                for (int j=0; j<3; ++j)
                    accum[i] += M[i][j] * ptr[0][j];
            for (int i=0; i<3; ++i)
                ptr[0][i] = accum[i];
            ++ptr;
        }
    }
}

void ExposureSeries::transform_color_matrix(float *matrix, const std::string &label) {
    cout << "Transforming to " << label << " color space .." << endl;

    #pragma omp parallel for
    for (int y=0; y<height; ++y) {
        float3 *ptr = image_demosaiced + y*width;
        for (size_t x=0; x<width; ++x) {
            float accum[3] = {0, 0, 0};
            for (int i=0; i<3; ++i)
                for (int j=0; j<3; ++j)
                    accum[i] += matrix[3*i+j] * ptr[0][j];
            for (int i=0; i<3; ++i)
                ptr[0][i] = accum[i];
            ++ptr;
        }
    }
}

void ExposureSeries::solid2ang() {
    if (!image_demosaiced)
        throw std::runtime_error("solid2ang(): the image must be demosaiced first");
    if (width != height)
        throw std::runtime_error("solid2ang(): only square fisheye images are currently supported");

    cout << "Converting equisolid fisheye to equiangular .." << endl;

    const double sqrt2 = std::sqrt(2.0);
    const double pi = std::acos(-1.0);
    float3 *source = image_demosaiced;
    float3 *result = new float3[width * height];

    #pragma omp parallel for
    for (int y=0; y<(int) height; ++y) {
        for (size_t x=0; x<width; ++x) {
            float *dst = result[y * width + x];
            dst[0] = dst[1] = dst[2] = 0.0f;

            double dx = ((double) x + 0.5) / (double) width - 0.5;
            double dy = ((double) y + 0.5) / (double) height - 0.5;
            double radius = std::sqrt(dx*dx + dy*dy);

            if (radius > 0.5)
                continue;

            double src_radius = 0.0;
            if (radius > 1e-12)
                src_radius = std::sin(0.5 * pi * radius) / sqrt2;

            double scale = radius > 1e-12 ? src_radius / radius : 1.0;
            double src_u = 0.5 + dx * scale;
            double src_v = 0.5 + dy * scale;
            sample_bilinear_rgb(source, width, height, src_u * width, src_v * height, dst);
        }
    }

    delete[] image_demosaiced;
    image_demosaiced = result;
}

void ExposureSeries::scale(float factor) {
    cout << "Scaling the image by a factor of " << factor << " .." << endl;

    if (image_merged) {
        #pragma omp parallel for
        for (int y=0; y<height; ++y) {
            float *ptr = image_merged + y*width;
            for (size_t x=0; x<width; ++x)
                *ptr++ *= factor;
        }
    }

    if (image_demosaiced) {
        #pragma omp parallel for
        for (int y=0; y<height; ++y) {
            float3 *ptr = image_demosaiced + y*width;
            for (size_t x=0; x<width; ++x) {
                for (int i=0; i<3; ++i)
                    ptr[0][i] *= factor;
                ptr++;
            }
        }
    }
}

void ExposureSeries::crop(int offs_x, int offs_y, int w, int h) {
    cout << "Cropping to " << w << "x" << h << " .." << endl;
    if (offs_x < 0 || offs_y < 0 || w <= 0 || h <= 0 || offs_x+w > (int) width || offs_y+h > (int) height)
        throw std::runtime_error("crop(): selected an invalid rectangle!");

    if (image_merged) {
        float *temp = new float[w*h];

        for (int y=0; y<h; ++y) {
            float *dst = temp + w * y;
            float *src = image_merged + width * (y+offs_y) + offs_x;

            for (int x=0; x<w; ++x)
                *dst++ = *src++;
        }
        delete[] image_merged;
        image_merged = temp;
    }

    if (image_demosaiced) {
        float3 *temp = new float3[w*h];

        for (int y=0; y<h; ++y) {
            float3 *dst = temp + w * y;
            float3 *src = image_demosaiced + width * (y+offs_y) + offs_x;

            for (int x=0; x<w; ++x) {
                for (int c=0; c<3; ++c)
                    (*dst)[c] = (*src)[c];
                ++dst;
                ++src;
            }
        }
        delete[] image_demosaiced;
        image_demosaiced = temp;
    }

    width = w;
    height = h;
}

void ExposureSeries::crop_raw(int offs_x, int offs_y, int w, int h) {
    if (offs_x < 0 || offs_y < 0 || w <= 0 || h <= 0 || offs_x + w > (int) width || offs_y + h > (int) height)
        throw std::runtime_error("crop_raw(): selected an invalid rectangle!");

    if (image_merged || image_demosaiced) {
        crop(offs_x, offs_y, w, h);
        return;
    }

    cout << "Pre-cropping RAW mosaic to " << w << "x" << h << " .." << endl;

    for (size_t i = 0; i < exposures.size(); ++i) {
        uint16_t *temp = new uint16_t[w * h];
        for (int y = 0; y < h; ++y) {
            uint16_t *dst = temp + w * y;
            uint16_t *src = exposures[i].image + width * (y + offs_y) + offs_x;
            memcpy(dst, src, sizeof(uint16_t) * w);
        }
        delete[] exposures[i].image;
        exposures[i].image = temp;
    }

    filter = shift_filter_pattern(filter, offs_x, offs_y);
    width = w;
    height = h;
}

void ExposureSeries::whitebalance(int offs_x, int offs_y, int w, int h) {
    if (offs_x < 0 || offs_y < 0 || w <= 0 || h <= 0 || offs_x+w > (int) width || offs_y+h > (int) height)
        throw std::runtime_error("crop(): selected an invalid rectangle!");

    float scale[3] = { 0, 0, 0 };
    for (int y=0; y<h; ++y) {
        float3 *ptr = image_demosaiced + (offs_y+y) * width + offs_x;
        for (int x=0; x<w; ++x) {
            for (int c=0; c<3; ++c)
                scale[c] += (*ptr)[c];
            ++ptr;
        }
    }

    for (int c=0; c<3; ++c)
        scale[c] = 1.0f / scale[c];

    float normalization = 3.0f / (scale[0] + scale[1] + scale[2]);

    for (int c=0; c<3; ++c)
        scale[c] = normalization * scale[c];

    whitebalance(scale);
}

void ExposureSeries::whitebalance(float *scale) {
    cout << "Applying white balance (multipliers = " << scale[0] << ", " << scale[1] << ", " << scale[2] << ")" << endl;
    for (size_t y=0; y<height; ++y) {
        float3 *ptr = image_demosaiced + y*width;
        for (size_t x=0; x<width; ++x) {
            for (int c=0; c<3; ++c)
                (*ptr)[c] *= scale[c];
            ptr++;
        }
    }
}

void ExposureSeries::vcal() {
    /* Simplistic vignetting correction -- assumes that vignetting is radially symmetric
       around the image center and least-squares-fits a 6-th order polynomial. Probably
       good enough for most purposes though.. */
    double center_x = width / 2.0, center_y = height / 2.0;
    size_t skip = 10, nPixels = ((width+skip-1)/skip) * ((height+skip-1)/skip);
    double size_scale = 1.0 / std::max(width, height);

    Eigen::MatrixXd A(nPixels, 4);
    Eigen::VectorXd b(nPixels);

    cout << "Fitting a 6-th order polynomial to the vignetting profile .." << endl;
    size_t idx = 0;

    for (size_t y=0; y<height; y += skip) {
        float3 *ptr = image_demosaiced + y*width;
        double dy = ((y + 0.5f) - center_y)*size_scale, dy2 = dy*dy;
        for (size_t x=0; x<width; x += skip) {
            double luminance = ptr[0][0] * 0.212671 + ptr[0][1] * 0.715160 + ptr[0][2] * 0.072169;
            double dx = ((x + 0.5f) - center_x) * size_scale, dx2 = dx*dx;
            double dist2 = dx2+dy2, dist4 = dist2*dist2, dist6 = dist4*dist2;
            A(idx, 0) = 1.0f;
            A(idx, 1) = dist2;
            A(idx, 2) = dist4;
            A(idx, 3) = dist6;
            b(idx) = luminance;
            ptr += skip;
            idx++;
        }
    }
    if (nPixels != idx) {
        cout << idx << " vs " << nPixels << endl;
        exit(-1);
    }

    Eigen::VectorXd result = A.colPivHouseholderQr().solve(b);
    result /= result(0);

    cout << "Done. Pass --vcorr \"" << result[1] << ", " << result[2] << ", " << result[3]
         << "\" in future runs (or add it to a config or profile file)" << endl;

    vcorr((float) result[1], (float) result[2], (float) result[3]);
}

void ExposureSeries::vcorr(float a, float b, float c) {
    double center_x = width / 2.0, center_y = height / 2.0;
    double size_scale = 1.0 / std::max(width, height);

    cout << "Correcting for vignetting .." << endl;

    #pragma omp parallel for
    for (int y=0; y<height; ++y) {
        float3 *ptr = image_demosaiced + y*width;
        double dy = ((y + 0.5f) - center_y)*size_scale, dy2 = dy*dy;
        for (int x=0; x<width; ++x) {
            double dx = ((x + 0.5f) - center_x)*size_scale, dx2 = dx*dx;
            double dist2 = dx2+dy2, dist4 = dist2*dist2, dist6 = dist4*dist2;
            float corr = 1.0f / (1.0f + dist2*a + dist4*b + dist6*c);
            for (int c=0; c<3; ++c)
                ptr[0][c] *= corr;
            ++ptr;
        }
    }
}
