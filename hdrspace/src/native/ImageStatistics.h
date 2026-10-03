#pragma once

#include <tev/Box.h>
#include <tev/Common.h>
#include <tev/Image.h>
#include <tev/ImageCanvas.h>
#include <tev/ProfileGraph.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace native {

enum class EImageStatisticsRegionType {
    Rectangle,
    Circle,
    SelectionMask,
};

enum class EImageStatisticsMode {
    Horizontal,
    Vertical,
    Both,
    Ranked,
};

enum class EImageStatisticsValueMode {
    Luminance,
    RGB,
};

enum class EImageStatisticsAxisMode {
    Pixels,
    AngleDegrees,
};

enum class EImageStatisticsGraphMode {
    Line,
    Scatter,
    LineScatter,
};

struct CircleStatisticsRegion {
    nanogui::Vector2i center = {0, 0};
    double radiusDegrees = 0.0;
};

struct ImageStatisticsGraph {
    std::string title;
    std::string subtitle;
    std::string xLabel;
    std::string yLabel;
    std::vector<tev::ProfileGraphSeries> series;
};

struct ImageStatisticsTable {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;
};

struct ImageStatisticsOptions {
    EImageStatisticsRegionType regionType = EImageStatisticsRegionType::Rectangle;
    EImageStatisticsMode mode = EImageStatisticsMode::Both;
    EImageStatisticsValueMode valueMode = EImageStatisticsValueMode::Luminance;
    EImageStatisticsAxisMode axisMode = EImageStatisticsAxisMode::Pixels;
    EImageStatisticsGraphMode graphMode = EImageStatisticsGraphMode::LineScatter;
    std::optional<tev::Box2i> rectangleRegion;
    std::optional<CircleStatisticsRegion> circleRegion;
    std::vector<uint8_t> selectionMask;
    std::optional<tev::Box2i> selectionBounds;
    std::string selectionLabel;
};

struct ImageStatisticsResult {
    bool ok = false;
    bool hasAngularAxis = false;
    std::string statusMessage;
    std::string regionSummary;
    std::string axisSummary;
    size_t pixelCount = 0;
    double solidAngle = 0.0;
    std::optional<tev::Box2i> rectangleRegion;
    std::optional<CircleStatisticsRegion> circleRegion;
    std::vector<ImageStatisticsGraph> graphs;
    ImageStatisticsTable table;
    std::shared_ptr<tev::PixelBuffer> overlayPixels;
    std::vector<uint8_t> selectionMask;
    std::optional<tev::Box2i> selectionBounds;
};

ImageStatisticsResult computeImageStatistics(
    const tev::Image& image,
    const tev::ImageCanvas& canvas,
    const std::shared_ptr<tev::Image>& reference,
    const ImageStatisticsOptions& options
);

std::vector<uint8_t> renderImageStatisticsGraphsToRgba(
    const ImageStatisticsResult& result,
    EImageStatisticsGraphMode graphMode,
    int width,
    int panelHeight
);

std::vector<uint8_t> renderGraphPanelsToRgba(
    std::span<const ImageStatisticsGraph> graphs,
    EImageStatisticsGraphMode graphMode,
    int width,
    int height,
    nanogui::Color backgroundColor = nanogui::Color(18, 22, 31, 255),
    const tev::ProfileGraphStyle* styleOverride = nullptr
);

void saveImageStatisticsCsv(const std::filesystem::path& path, const ImageStatisticsResult& result);

bool imageStatisticsSupportsAngularAxis(const tev::Image& image);
std::string imageStatisticsAngularAxisMessage(const tev::Image& image);

} // namespace native
