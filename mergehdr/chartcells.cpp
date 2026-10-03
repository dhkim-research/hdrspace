#include "chartcells.h"

#include "hdrimage.h"
#include "hdrmerge.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

struct CellRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct CellStats {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double y = 0.0;
};

struct DetectionResult {
    std::vector<CellRect> cells;
    int whiteIndex = -1;
    int columns = 0;
    int rows = 0;
};

struct Segment {
    int begin = 0;
    int end = 0;
    double mean = 0.0;
    double score = 0.0;
};

double segmentWidth(const Segment &segment) {
    return static_cast<double>(segment.end - segment.begin + 1);
}

std::vector<double> luminanceImage(const HdrImage &image) {
    std::vector<double> y(image.width * image.height, 0.0);
    for (size_t i = 0; i < image.width * image.height; ++i) {
        y[i] = 0.265 * image.rgb[3 * i + 0] +
               0.670 * image.rgb[3 * i + 1] +
               0.065 * image.rgb[3 * i + 2];
    }
    return y;
}

std::vector<double> smoothProfile(const std::vector<double> &values, int radius) {
    std::vector<double> out(values.size(), 0.0);
    for (size_t i = 0; i < values.size(); ++i) {
        size_t begin = (i < static_cast<size_t>(radius)) ? 0 : i - static_cast<size_t>(radius);
        size_t end = std::min(values.size() - 1, i + static_cast<size_t>(radius));
        double sum = 0.0;
        for (size_t j = begin; j <= end; ++j)
            sum += values[j];
        out[i] = sum / static_cast<double>(end - begin + 1);
    }
    return out;
}

double percentile(std::vector<double> values, double p) {
    if (values.empty())
        return 0.0;
    if (p <= 0.0)
        return *std::min_element(values.begin(), values.end());
    if (p >= 1.0)
        return *std::max_element(values.begin(), values.end());
    size_t idx = static_cast<size_t>(std::floor(p * (values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(idx), values.end());
    return values[idx];
}

std::vector<double> columnProfile(const std::vector<double> &lum, size_t width, size_t height) {
    std::vector<double> profile(width, 0.0);
    for (size_t x = 0; x < width; ++x) {
        double sum = 0.0;
        for (size_t y = 0; y < height; ++y)
            sum += lum[y * width + x];
        profile[x] = sum / static_cast<double>(height);
    }
    return profile;
}

std::vector<double> rowProfile(const std::vector<double> &lum, size_t width, size_t height) {
    std::vector<double> profile(height, 0.0);
    for (size_t y = 0; y < height; ++y) {
        double sum = 0.0;
        for (size_t x = 0; x < width; ++x)
            sum += lum[y * width + x];
        profile[y] = sum / static_cast<double>(width);
    }
    return profile;
}

double overlapLength(const Segment &a, const Segment &b) {
    return static_cast<double>(std::max(0, std::min(a.end, b.end) - std::max(a.begin, b.begin) + 1));
}

double segmentContrast(const std::vector<double> &profile, int begin, int end) {
    int width = end - begin + 1;
    int radius = std::max(3, width * 2);
    int leftBegin = std::max(0, begin - radius);
    int leftEnd = begin - 1;
    int rightBegin = end + 1;
    int rightEnd = std::min(static_cast<int>(profile.size()) - 1, end + radius);
    auto average = [&](int a, int b) {
        if (b < a)
            return 0.0;
        double sum = 0.0;
        for (int i = a; i <= b; ++i)
            sum += profile[static_cast<size_t>(i)];
        return sum / static_cast<double>(b - a + 1);
    };
    double inner = average(begin, end);
    double left = average(leftBegin, leftEnd);
    double right = average(rightBegin, rightEnd);
    if (leftEnd < leftBegin)
        return std::max(0.0, right - inner);
    if (rightEnd < rightBegin)
        return std::max(0.0, left - inner);
    return std::max(0.0, 0.5 * (left + right) - inner);
}

std::vector<Segment> candidateLowSegments(const std::vector<double> &profile, int expectedCount) {
    std::vector<double> smooth = smoothProfile(profile, std::max(2, static_cast<int>(profile.size() / 80)));
    double p05 = percentile(smooth, 0.05);
    double p35 = percentile(smooth, 0.35);
    double p50 = percentile(smooth, 0.50);
    int minWidth = std::max(2, static_cast<int>(profile.size() / 250));
    int maxWidth = std::max(minWidth + 1, static_cast<int>(profile.size() / std::max(3, expectedCount)));

    std::vector<Segment> candidates;
    for (double mix = 0.0; mix <= 1.0; mix += 0.05) {
        double threshold = p05 + (p35 - p05) * mix;
        bool inside = false;
        int begin = 0;
        double sum = 0.0;
        int count = 0;
        for (int i = 0; i < static_cast<int>(smooth.size()); ++i) {
            bool low = smooth[static_cast<size_t>(i)] <= threshold;
            if (low && !inside) {
                inside = true;
                begin = i;
                sum = 0.0;
                count = 0;
            }
            if (low) {
                sum += smooth[static_cast<size_t>(i)];
                ++count;
            }
            if ((!low || i == static_cast<int>(smooth.size()) - 1) && inside) {
                int end = low ? i : (i - 1);
                int width = end - begin + 1;
                if (width >= minWidth && width <= maxWidth) {
                    Segment seg;
                    seg.begin = begin;
                    seg.end = end;
                    seg.mean = sum / static_cast<double>(count);
                    double darkness = std::max(0.0, p50 - seg.mean);
                    double contrast = segmentContrast(smooth, begin, end);
                    seg.score = darkness + 0.5 * contrast + 0.15 * static_cast<double>(width);
                    candidates.push_back(seg);
                }
                inside = false;
            }
        }
    }

    if (candidates.empty())
        throw std::runtime_error("Could not detect the full chart grid.");

    std::sort(candidates.begin(), candidates.end(), [](const Segment &a, const Segment &b) {
        if (a.score == b.score)
            return a.begin < b.begin;
        return a.score > b.score;
    });

    std::vector<Segment> unique;
    unique.reserve(candidates.size());
    for (const Segment &candidate : candidates) {
        bool duplicate = false;
        for (const Segment &kept : unique) {
            double overlap = overlapLength(candidate, kept);
            double minW = std::min(segmentWidth(candidate), segmentWidth(kept));
            if (overlap > 0.6 * minW) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            unique.push_back(candidate);
    }

    std::sort(unique.begin(), unique.end(), [](const Segment &a, const Segment &b) {
        return a.begin < b.begin;
    });

    if (static_cast<int>(unique.size()) < expectedCount)
        throw std::runtime_error("Could not detect the full chart grid.");
    return unique;
}

double meanValue(const std::vector<int> &values) {
    if (values.empty())
        return 0.0;
    return static_cast<double>(std::accumulate(values.begin(), values.end(), 0.0)) /
           static_cast<double>(values.size());
}

double coefficientOfVariation(const std::vector<int> &values) {
    if (values.size() < 2)
        return 0.0;
    double mean = meanValue(values);
    if (!(mean > 0.0))
        return std::numeric_limits<double>::infinity();
    double accum = 0.0;
    for (int value : values) {
        double delta = static_cast<double>(value) - mean;
        accum += delta * delta;
    }
    return std::sqrt(accum / static_cast<double>(values.size())) / mean;
}

double evaluateBandSelection(const std::vector<Segment> &bands, int axisLength, int cellCount) {
    if (static_cast<int>(bands.size()) != cellCount + 1)
        return -std::numeric_limits<double>::infinity();

    std::vector<int> cellSizes;
    std::vector<int> bandSizes;
    cellSizes.reserve(static_cast<size_t>(cellCount));
    bandSizes.reserve(bands.size());
    for (const Segment &band : bands)
        bandSizes.push_back(band.end - band.begin + 1);
    for (int i = 0; i < cellCount; ++i) {
        int begin = bands[static_cast<size_t>(i)].end + 1;
        int end = bands[static_cast<size_t>(i + 1)].begin - 1;
        int size = end - begin + 1;
        if (size <= 0)
            return -std::numeric_limits<double>::infinity();
        cellSizes.push_back(size);
    }

    double meanCell = meanValue(cellSizes);
    if (!(meanCell > 0.0))
        return -std::numeric_limits<double>::infinity();
    int minCell = *std::min_element(cellSizes.begin(), cellSizes.end());
    if (minCell < std::max(4, axisLength / (cellCount * 10)))
        return -std::numeric_limits<double>::infinity();

    double cvCell = coefficientOfVariation(cellSizes);
    double cvBand = coefficientOfVariation(bandSizes);
    double darkness = 0.0;
    for (const Segment &band : bands)
        darkness += band.score;
    darkness /= static_cast<double>(bands.size());

    double coverage = static_cast<double>(std::accumulate(cellSizes.begin(), cellSizes.end(), 0)) /
                      static_cast<double>(axisLength);
    double minRatio = static_cast<double>(minCell) / meanCell;
    double maxRatio = static_cast<double>(*std::max_element(cellSizes.begin(), cellSizes.end())) / meanCell;

    return 3.0 * darkness +
           2.0 * coverage -
           14.0 * cvCell -
           3.0 * cvBand -
           6.0 * std::max(0.0, 0.65 - minRatio) -
           2.0 * std::max(0.0, maxRatio - 1.45);
}

std::vector<Segment> selectRegularBands(const std::vector<Segment> &candidates, int expectedBandCount, int axisLength) {
    double bestScore = -std::numeric_limits<double>::infinity();
    std::vector<Segment> best;
    std::vector<Segment> current;

    std::function<void(size_t, int)> search = [&](size_t index, int remaining) {
        if (remaining == 0) {
            double score = evaluateBandSelection(current, axisLength, expectedBandCount - 1);
            if (score > bestScore) {
                bestScore = score;
                best = current;
            }
            return;
        }
        if (index >= candidates.size())
            return;
        if (static_cast<int>(candidates.size() - index) < remaining)
            return;

        for (size_t i = index; i + static_cast<size_t>(remaining) <= candidates.size(); ++i) {
            if (!current.empty() && candidates[i].begin <= current.back().end)
                continue;
            current.push_back(candidates[i]);
            search(i + 1, remaining - 1);
            current.pop_back();
        }
    };

    search(0, expectedBandCount);
    if (best.empty()) {
        if (static_cast<int>(candidates.size()) < expectedBandCount)
            throw std::runtime_error("Could not detect the full chart grid.");
        best.assign(candidates.begin(), candidates.begin() + expectedBandCount);
    }
    return best;
}

std::vector<std::pair<int, int>> cellSpansFromBands(const std::vector<Segment> &bands, int count) {
    if (static_cast<int>(bands.size()) != count + 1)
        throw std::runtime_error("Unexpected band count while building chart cells.");
    std::vector<std::pair<int, int>> spans;
    spans.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        int begin = bands[static_cast<size_t>(i)].end + 1;
        int end = bands[static_cast<size_t>(i + 1)].begin - 1;
        if (end <= begin)
            throw std::runtime_error("Detected chart bands overlap.");
        spans.push_back(std::make_pair(begin, end));
    }
    return spans;
}

CellRect insetRect(const std::pair<int, int> &xspan, const std::pair<int, int> &yspan, double inset) {
    int x0 = xspan.first;
    int x1 = xspan.second;
    int y0 = yspan.first;
    int y1 = yspan.second;
    int w = x1 - x0 + 1;
    int h = y1 - y0 + 1;
    int dx = std::max(1, static_cast<int>(std::round(w * inset)));
    int dy = std::max(1, static_cast<int>(std::round(h * inset)));
    CellRect rect;
    rect.x = x0 + dx;
    rect.y = y0 + dy;
    rect.w = std::max(1, w - 2 * dx);
    rect.h = std::max(1, h - 2 * dy);
    return rect;
}

CellStats cellStats(const HdrImage &image, const CellRect &cell) {
    CellStats stats;
    double inv = 1.0 / static_cast<double>(cell.w * cell.h);
    for (int y = cell.y; y < cell.y + cell.h; ++y)
        for (int x = cell.x; x < cell.x + cell.w; ++x) {
            size_t idx = (static_cast<size_t>(y) * image.width + static_cast<size_t>(x)) * 3;
            stats.r += image.rgb[idx + 0];
            stats.g += image.rgb[idx + 1];
            stats.b += image.rgb[idx + 2];
        }
    stats.r *= inv;
    stats.g *= inv;
    stats.b *= inv;
    stats.y = 0.265 * stats.r + 0.670 * stats.g + 0.065 * stats.b;
    return stats;
}

double whitePatchScore(const CellStats &stats) {
    double maxRgb = std::max(stats.r, std::max(stats.g, stats.b));
    double minRgb = std::min(stats.r, std::min(stats.g, stats.b));
    if (!(maxRgb > 0.0))
        return -std::numeric_limits<double>::infinity();
    double chroma = (maxRgb - minRgb) / maxRgb;
    return stats.y - 0.35 * chroma * maxRgb;
}

double cellChroma(const CellStats &stats) {
    double maxRgb = std::max(stats.r, std::max(stats.g, stats.b));
    double minRgb = std::min(stats.r, std::min(stats.g, stats.b));
    if (!(maxRgb > 0.0))
        return 1.0;
    return (maxRgb - minRgb) / maxRgb;
}

double monotonicLineScore(const std::vector<double> &values) {
    if (values.size() < 2)
        return 0.0;
    double minValue = *std::min_element(values.begin(), values.end());
    double maxValue = *std::max_element(values.begin(), values.end());
    double range = std::max(1e-8, maxValue - minValue);
    double violationAscending = 0.0;
    double violationDescending = 0.0;
    for (size_t i = 1; i < values.size(); ++i) {
        violationAscending += std::max(0.0, values[i - 1] - values[i]);
        violationDescending += std::max(0.0, values[i] - values[i - 1]);
    }
    double bestViolation = std::min(violationAscending, violationDescending);
    return std::max(0.0, 1.0 - bestViolation / (range * static_cast<double>(values.size() - 1) + 1e-8));
}

double grayStripScore(const std::vector<CellStats> &line) {
    if (line.size() < 3)
        return 0.0;
    std::vector<double> luminance;
    luminance.reserve(line.size());
    double chromaSum = 0.0;
    double maxY = 0.0;
    double minY = std::numeric_limits<double>::infinity();
    for (const CellStats &stats : line) {
        luminance.push_back(stats.y);
        chromaSum += cellChroma(stats);
        maxY = std::max(maxY, stats.y);
        minY = std::min(minY, stats.y);
    }
    double grayness = std::max(0.0, 1.0 - chromaSum / static_cast<double>(line.size()));
    double monotonic = monotonicLineScore(luminance);
    double spread = maxY > 0.0 ? std::max(0.0, (maxY - minY) / maxY) : 0.0;
    return 0.55 * grayness + 0.25 * monotonic + 0.20 * spread;
}

std::pair<int, int> inferGridDimensions(size_t width, size_t height, int patchCount) {
    if (patchCount <= 0)
        return std::make_pair(6, 4);
    double imageAspect = static_cast<double>(width) / std::max<size_t>(1, height);
    double bestScore = std::numeric_limits<double>::infinity();
    std::pair<int, int> best( patchCount, 1 );
    for (int rows = 1; rows <= patchCount; ++rows) {
        if (patchCount % rows != 0)
            continue;
        int cols = patchCount / rows;
        auto scorePair = [&](int c, int r) {
            double gridAspect = static_cast<double>(c) / std::max(1, r);
            double score = std::abs(std::log(gridAspect / imageAspect));
            if (c < r)
                score += 0.25;
            score += 0.03 * std::abs(c - r);
            return score;
        };
        for (auto candidate : {std::make_pair(cols, rows), std::make_pair(rows, cols)}) {
            double score = scorePair(candidate.first, candidate.second);
            if (score < bestScore) {
                bestScore = score;
                best = candidate;
            }
        }
    }
    return best;
}

DetectionResult detectChartCells(const HdrImage &image, int cols, int rows, double inset, bool whiteFirst) {
    std::vector<double> lum = luminanceImage(image);
    std::vector<Segment> xCandidates = candidateLowSegments(columnProfile(lum, image.width, image.height), cols + 1);
    std::vector<Segment> yCandidates = candidateLowSegments(rowProfile(lum, image.width, image.height), rows + 1);
    std::vector<Segment> xBands = selectRegularBands(xCandidates, cols + 1, static_cast<int>(image.width));
    std::vector<Segment> yBands = selectRegularBands(yCandidates, rows + 1, static_cast<int>(image.height));

    std::vector<std::pair<int, int>> xSpans = cellSpansFromBands(xBands, cols);
    std::vector<std::pair<int, int>> ySpans = cellSpansFromBands(yBands, rows);

    DetectionResult result;
    result.columns = cols;
    result.rows = rows;
    result.cells.reserve(static_cast<size_t>(cols * rows));
    for (int row = rows - 1; row >= 0; --row) {
        for (int col = 0; col < cols; ++col)
            result.cells.push_back(insetRect(xSpans[static_cast<size_t>(col)], ySpans[static_cast<size_t>(row)], inset));
    }

    if (!result.cells.empty()) {
        std::vector<CellStats> stats;
        stats.reserve(result.cells.size());
        for (const CellRect &cell : result.cells)
            stats.push_back(cellStats(image, cell));

        size_t brightest = 0;
        double best = -std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < result.cells.size(); ++i) {
            int row = static_cast<int>(i) / cols;
            int col = static_cast<int>(i) % cols;
            std::vector<CellStats> rowLine;
            std::vector<CellStats> colLine;
            rowLine.reserve(cols);
            colLine.reserve(rows);
            for (int c = 0; c < cols; ++c)
                rowLine.push_back(stats[static_cast<size_t>(row * cols + c)]);
            for (int r = 0; r < rows; ++r)
                colLine.push_back(stats[static_cast<size_t>(r * cols + col)]);
            bool rowBrightest = true;
            for (const CellStats &candidate : rowLine)
                rowBrightest = rowBrightest && stats[i].y >= candidate.y * 0.995;
            bool colBrightest = true;
            for (const CellStats &candidate : colLine)
                colBrightest = colBrightest && stats[i].y >= candidate.y * 0.995;
            double stripScore = std::max(rowBrightest ? grayStripScore(rowLine) : 0.0,
                                         colBrightest ? grayStripScore(colLine) : 0.0);
            double value = whitePatchScore(stats[i]) + 0.45 * stripScore;
            if (value > best) {
                best = value;
                brightest = i;
            }
        }
        result.whiteIndex = static_cast<int>(brightest);
        if (whiteFirst && brightest != 0) {
            CellRect white = result.cells[brightest];
            result.cells.erase(result.cells.begin() + static_cast<std::ptrdiff_t>(brightest));
            result.cells.insert(result.cells.begin(), white);
            result.whiteIndex = 0;
        }
    }

    return result;
}

void writeCells(const std::string &path, const std::vector<CellRect> &cells) {
    std::ofstream out(path.c_str());
    if (!out)
        throw std::runtime_error("Could not write cell file: " + path);
    for (const CellRect &cell : cells)
        out << cell.x << "\t" << cell.y << "\t" << cell.w << "\t" << cell.h << "\n";
}

void drawOverlay(std::vector<float> &rgb, size_t width, size_t height, const std::vector<CellRect> &cells, int highlightIndex) {
    for (size_t cellIndex = 0; cellIndex < cells.size(); ++cellIndex) {
        const CellRect &cell = cells[cellIndex];
        int x0 = cell.x;
        int y0 = cell.y;
        int x1 = cell.x + cell.w - 1;
        int y1 = cell.y + cell.h - 1;
        bool highlight = static_cast<int>(cellIndex) == highlightIndex;
        float red = highlight ? 0.0f : 1.0f;
        float green = highlight ? 0.0f : 0.05f;
        float blue = highlight ? 0.0f : 0.05f;
        int thickness = highlight ? 2 : 1;
        for (int t = 0; t < thickness; ++t) {
            int xt0 = x0 - t;
            int yt0 = y0 - t;
            int xt1 = x1 + t;
            int yt1 = y1 + t;
            for (int x = xt0; x <= xt1; ++x) {
                for (int y : {yt0, yt1}) {
                    if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height))
                        continue;
                    size_t idx = (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 3;
                    rgb[idx + 0] = red;
                    rgb[idx + 1] = green;
                    rgb[idx + 2] = blue;
                }
            }
            for (int y = yt0; y <= yt1; ++y) {
                for (int x : {xt0, xt1}) {
                    if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height))
                        continue;
                    size_t idx = (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 3;
                    rgb[idx + 0] = red;
                    rgb[idx + 1] = green;
                    rgb[idx + 2] = blue;
                }
            }
        }
    }
}

std::vector<float> tonemapPreview(const HdrImage &image) {
    std::vector<float> preview = image.rgb;
    std::vector<float> maxValues;
    maxValues.reserve(image.width * image.height);
    for (size_t i = 0; i < image.width * image.height; ++i)
        maxValues.push_back(std::max(preview[3 * i + 0], std::max(preview[3 * i + 1], preview[3 * i + 2])));
    std::nth_element(maxValues.begin(), maxValues.begin() + static_cast<std::ptrdiff_t>(maxValues.size() * 995 / 1000), maxValues.end());
    float scale = maxValues[static_cast<size_t>(maxValues.size() * 995 / 1000)];
    if (!(scale > 0.0f))
        scale = 1.0f;
    for (float &value : preview)
        value = std::max(0.0f, std::min(1.0f, value / scale));
    return preview;
}

} // namespace

int runChartCells(const ChartCellsOptions &opts, std::ostream &out, std::ostream &err) {
    if (opts.imagePath.empty())
        throw std::runtime_error("chartcells requires an input image.");
    if (opts.outputPath.empty())
        throw std::runtime_error("chartcells requires -o/--output.");
    if (opts.columns <= 0 || opts.rows <= 0)
        throw std::runtime_error("Chart dimensions must be positive.");
    if (!(opts.inset >= 0.0 && opts.inset < 0.45))
        throw std::runtime_error("--inset must be between 0 and 0.45.");

    HdrImage image = readRadianceHDR(opts.imagePath);
    int columns = opts.columns;
    int rows = opts.rows;
    if (opts.patchCount > 0)
        std::tie(columns, rows) = inferGridDimensions(image.width, image.height, opts.patchCount);
    DetectionResult detection = detectChartCells(image, columns, rows, opts.inset, opts.whiteFirst);
    writeCells(opts.outputPath, detection.cells);

    out << "grid: " << detection.columns << " x " << detection.rows << "\n";
    out << "wrote " << detection.cells.size() << " cells to " << opts.outputPath << "\n";
    if (!detection.cells.empty()) {
        const CellRect &first = detection.cells.front();
        out << "first cell: " << first.x << " " << first.y << " " << first.w << " " << first.h << "\n";
        if (detection.whiteIndex >= 0)
            out << "white cell index: " << detection.whiteIndex << "\n";
    }

    if (!opts.previewPath.empty()) {
        std::vector<float> preview = tonemapPreview(image);
        drawOverlay(preview, image.width, image.height, detection.cells, detection.whiteIndex);
        writeJPEG(opts.previewPath, image.width, image.height, preview.data(), 95);
        err << "preview: " << opts.previewPath << std::endl;
    }
    return 0;
}
