#ifndef MERGEHDR_CHARTCELLS_H
#define MERGEHDR_CHARTCELLS_H

#include <iosfwd>
#include <string>

struct ChartCellsOptions {
    std::string imagePath;
    std::string outputPath;
    std::string previewPath;
    int patchCount = 0;
    int columns = 6;
    int rows = 4;
    double inset = 0.15;
    bool whiteFirst = true;
};

int runChartCells(const ChartCellsOptions &opts, std::ostream &out, std::ostream &err);

#endif
