#ifndef MERGEHDR_COLORCALIBRATE_H
#define MERGEHDR_COLORCALIBRATE_H

#include <iosfwd>
#include <string>
#include <vector>

struct ColorCalibrateOptions {
    std::string referencePath;
    std::string testPath;
    std::string referenceCellsPath;
    std::string testCellsPath;
    std::string referenceColor = "rad";
    std::vector<float> xyzcamOverride;
    std::string referenceDataOut;
    std::string testDataOut;
    std::string minimizer = "luv";
    bool verbose = false;
};

int runColorCalibration(const ColorCalibrateOptions &opts, std::ostream &out, std::ostream &err);

#endif
