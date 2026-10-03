#include "hdrmerge.h"
#include "dht_include/pfs.h"

#ifdef _OPENMP
#include <omp.h>
#endif

extern void dht_interpolate(pfs::Array2D *imgdata[], bool median);

namespace {

class InterleavedPlane final : public pfs::Array2D {
public:
    InterleavedPlane(float3 *data, std::size_t cols, std::size_t rows, int channel)
        : m_data(data), m_cols(cols), m_rows(rows), m_channel(channel) { }

    std::size_t getRows() const override { return m_rows; }
    std::size_t getCols() const override { return m_cols; }

    float &operator()(std::size_t index) override {
        return m_data[index][m_channel];
    }

    const float &operator()(std::size_t index) const override {
        return m_data[index][m_channel];
    }

    float &operator()(std::size_t col, std::size_t row) override {
        return m_data[row * m_cols + col][m_channel];
    }

    const float &operator()(std::size_t col, std::size_t row) const override {
        return m_data[row * m_cols + col][m_channel];
    }

private:
    float3 *m_data;
    std::size_t m_cols;
    std::size_t m_rows;
    int m_channel;
};

}

void demosaicDHT(ExposureSeries &series, bool median) {
    cout << "DHT demosaicing .." << endl;

    if (series.image_demosaiced) {
        delete[] series.image_demosaiced;
        series.image_demosaiced = NULL;
    }

    const std::size_t total = series.width * series.height;
    series.image_demosaiced = new float3[total];

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (ptrdiff_t idx = 0; idx < static_cast<ptrdiff_t>(total); ++idx) {
        series.image_demosaiced[idx][0] = 0.f;
        series.image_demosaiced[idx][1] = 0.f;
        series.image_demosaiced[idx][2] = 0.f;
    }

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (ptrdiff_t y = 0; y < static_cast<ptrdiff_t>(series.height); ++y) {
        for (std::size_t x = 0; x < series.width; ++x) {
            std::size_t idx = static_cast<std::size_t>(y) * series.width + x;
            series.image_demosaiced[idx][series.fc(static_cast<int>(x), static_cast<int>(y))] = series.image_merged[idx];
        }
    }

    InterleavedPlane red(series.image_demosaiced, series.width, series.height, 0);
    InterleavedPlane green(series.image_demosaiced, series.width, series.height, 1);
    InterleavedPlane blue(series.image_demosaiced, series.width, series.height, 2);
    pfs::Array2D *planes[3] = { &red, &green, &blue };
    dht_interpolate(planes, median);

    delete[] series.image_merged;
    series.image_merged = NULL;
}
