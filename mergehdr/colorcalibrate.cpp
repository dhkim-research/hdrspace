#include "colorcalibrate.h"
#include "hdrimage.h"
#include "third_party/nlopt_slsqp/slsqp.h"

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct CellRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct CalibrationEntry {
    std::string key;
    std::string label;
    Eigen::Matrix3d xyzcam = Eigen::Matrix3d::Identity();
    double dlab = 0.0;
    double duv = 0.0;
    double dxy = 0.0;
    double dlum = 0.0;
    Eigen::MatrixXd dlabFull;
    Eigen::MatrixXd duvFull;
    Eigen::MatrixXd dxyFull;
};

struct CalibrationBundle {
    std::vector<CalibrationEntry> entries;
    Eigen::MatrixXd referenceData;
    Eigen::MatrixXd testData;
};

std::string trimCopy(const std::string &value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return value.substr(begin, end - begin);
}

std::string lowerCopy(const std::string &value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::vector<std::string> splitWhitespace(const std::string &value) {
    std::vector<std::string> tokens;
    std::istringstream iss(value);
    std::string token;
    while (iss >> token)
        tokens.push_back(token);
    return tokens;
}

std::vector<double> parseDoubleList(const std::string &value) {
    std::vector<double> values;
    std::vector<std::string> tokens = splitWhitespace(value);
    values.reserve(tokens.size());
    for (const std::string &token : tokens)
        values.push_back(std::stod(token));
    return values;
}

std::vector<CellRect> loadCellFile(const std::string &path) {
    if (path.empty())
        throw std::runtime_error("Cell files are required when calibrating from HDR images.");

    std::ifstream input(path.c_str());
    if (!input)
        throw std::runtime_error("Could not open cell file: " + path);

    std::vector<CellRect> cells;
    std::string line;
    while (std::getline(input, line)) {
        line = trimCopy(line);
        if (line.empty() || line[0] == '#')
            continue;

        std::istringstream iss(line);
        CellRect rect;
        if (!(iss >> rect.x >> rect.y >> rect.w >> rect.h))
            throw std::runtime_error("Invalid cell file row in " + path + ": " + line);
        if (rect.w <= 0 || rect.h <= 0)
            throw std::runtime_error("Cell size must be positive in " + path + ": " + line);
        cells.push_back(rect);
    }

    if (cells.empty())
        throw std::runtime_error("No calibration cells found in " + path);
    return cells;
}

Eigen::MatrixXd readTextData(const std::string &path) {
    std::ifstream input(path.c_str());
    if (!input)
        throw std::runtime_error("Could not open data file: " + path);

    std::vector<std::vector<double>> rows;
    std::string line;
    while (std::getline(input, line)) {
        line = trimCopy(line);
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> tokens = splitWhitespace(line);
        if (tokens.empty())
            continue;
        std::vector<double> row;
        row.reserve(tokens.size());
        for (const std::string &token : tokens)
            row.push_back(std::stod(token));
        rows.push_back(row);
    }

    if (rows.empty())
        throw std::runtime_error("No numeric rows found in data file: " + path);

    size_t columns = rows.front().size();
    if (!(columns == 1 || columns == 3))
        throw std::runtime_error("Data file must contain either 1 or 3 columns: " + path);

    for (size_t i = 1; i < rows.size(); ++i)
        if (rows[i].size() != columns)
            throw std::runtime_error("Inconsistent column count in data file: " + path);

    Eigen::MatrixXd data(static_cast<Eigen::Index>(columns), static_cast<Eigen::Index>(rows.size()));
    for (size_t i = 0; i < rows.size(); ++i)
        for (size_t j = 0; j < columns; ++j)
            data(static_cast<Eigen::Index>(j), static_cast<Eigen::Index>(i)) = rows[i][j];
    return data;
}

Eigen::MatrixXd sampleCells(const HdrImage &image, const std::vector<CellRect> &cells, double scale) {
    Eigen::MatrixXd data(3, static_cast<Eigen::Index>(cells.size()));
    for (size_t i = 0; i < cells.size(); ++i) {
        const CellRect &cell = cells[i];
        if (cell.x < 0 || cell.y < 0 || cell.w <= 0 || cell.h <= 0)
            throw std::runtime_error("Cell rectangle has invalid bounds.");
        if (static_cast<size_t>(cell.x + cell.w) > image.width ||
                static_cast<size_t>(cell.y + cell.h) > image.height)
            throw std::runtime_error("Cell rectangle falls outside the HDR image.");

        Eigen::Vector3d sum = Eigen::Vector3d::Zero();
        for (int y = cell.y; y < cell.y + cell.h; ++y) {
            for (int x = cell.x; x < cell.x + cell.w; ++x) {
                size_t index = (static_cast<size_t>(y) * image.width + static_cast<size_t>(x)) * 3;
                sum(0) += image.rgb[index + 0];
                sum(1) += image.rgb[index + 1];
                sum(2) += image.rgb[index + 2];
            }
        }
        double invArea = scale / static_cast<double>(cell.w * cell.h);
        data.col(static_cast<Eigen::Index>(i)) = sum * invArea;
    }
    return data;
}

void saveMatrixRows(const std::string &path, const Eigen::MatrixXd &matrix) {
    if (path.empty())
        return;
    std::ofstream output(path.c_str());
    if (!output)
        throw std::runtime_error("Could not write data file: " + path);

    output << std::setprecision(10);
    for (Eigen::Index c = 0; c < matrix.cols(); ++c) {
        for (Eigen::Index r = 0; r < matrix.rows(); ++r) {
            if (r)
                output << '\t';
            output << matrix(r, c);
        }
        output << '\n';
    }
}

Eigen::Matrix3d primariesToXYZ(const std::array<double, 6> &primaries, const std::array<double, 2> &white) {
    Eigen::Matrix3d pxyz;
    for (int c = 0; c < 3; ++c) {
        double x = primaries[2 * c + 0];
        double y = primaries[2 * c + 1];
        double z = 1.0 - x - y;
        pxyz(0, c) = x;
        pxyz(1, c) = y;
        pxyz(2, c) = z;
    }

    Eigen::Vector3d wxyz;
    wxyz(0) = white[0] / white[1];
    wxyz(1) = 1.0;
    wxyz(2) = (1.0 - white[0] - white[1]) / white[1];

    Eigen::Vector3d scales = pxyz.inverse() * wxyz;
    Eigen::Matrix3d rgbXYZ = pxyz;
    for (int c = 0; c < 3; ++c)
        rgbXYZ.col(c) *= scales(c);
    return rgbXYZ;
}

Eigen::Matrix3d parseReferenceColor(const std::string &refcol) {
    std::string lowered = lowerCopy(trimCopy(refcol));
    if (lowered == "xyz")
        return Eigen::Matrix3d::Identity();
    if (lowered == "rad")
        return primariesToXYZ({0.640, 0.330, 0.290, 0.600, 0.150, 0.060}, {0.3333, 0.3333});
    if (lowered == "srgb")
        return primariesToXYZ({0.640, 0.330, 0.300, 0.600, 0.150, 0.060}, {0.3127, 0.3290});

    std::vector<double> values = parseDoubleList(refcol);
    if (values.size() == 8) {
        std::array<double, 6> primaries;
        std::array<double, 2> white;
        for (int i = 0; i < 6; ++i)
            primaries[i] = values[i];
        white[0] = values[6];
        white[1] = values[7];
        return primariesToXYZ(primaries, white);
    }

    throw std::runtime_error("Unsupported reference colorspace. Use rad, srgb, xyz, or 8 numeric values.");
}

std::array<double, 6> parsePrimaries(const std::string &value) {
    std::vector<double> values = parseDoubleList(value);
    if (values.size() != 6)
        throw std::runtime_error("TargetPrimaries must contain 6 numbers.");
    std::array<double, 6> primaries;
    std::copy(values.begin(), values.end(), primaries.begin());
    return primaries;
}

std::array<double, 2> parseWhitePoint(const std::string &value) {
    std::vector<double> values = parseDoubleList(value);
    if (values.size() != 2)
        throw std::runtime_error("TargetWhitePoint must contain 2 numbers.");
    return { values[0], values[1] };
}

Eigen::Matrix3d parseMatrix3x3(const std::string &value, const std::string &label) {
    std::vector<double> values = parseDoubleList(value);
    if (values.size() != 9)
        throw std::runtime_error(label + " must contain 9 numbers.");
    Eigen::Matrix3d matrix;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            matrix(i, j) = values[3 * i + j];
    return matrix;
}

Eigen::Vector3d parsePremultipliers(const std::string &value) {
    std::vector<double> values = parseDoubleList(value);
    if (values.size() < 3)
        throw std::runtime_error("CAM_PREMULTIPLIERS must contain at least 3 numbers.");
    return Eigen::Vector3d(values[0], values[1], values[2]);
}

Eigen::MatrixXd xyzToXY(const Eigen::MatrixXd &xyz) {
    Eigen::MatrixXd out(2, xyz.cols());
    for (Eigen::Index c = 0; c < xyz.cols(); ++c) {
        double denom = xyz(0, c) + xyz(1, c) + xyz(2, c);
        if (std::abs(denom) < 1e-20)
            denom = 1.0;
        out(0, c) = xyz(0, c) / denom;
        out(1, c) = xyz(1, c) / denom;
    }
    return out;
}

Eigen::MatrixXd xyzToYuv(const Eigen::MatrixXd &xyz) {
    Eigen::MatrixXd out(3, xyz.cols());
    for (Eigen::Index c = 0; c < xyz.cols(); ++c) {
        double denom = xyz(0, c) + 15.0 * xyz(1, c) + 3.0 * xyz(2, c);
        if (std::abs(denom) < 1e-20)
            denom = 1.0;
        out(0, c) = xyz(1, c);
        out(1, c) = 4.0 * xyz(0, c) / denom;
        out(2, c) = 9.0 * xyz(1, c) / denom;
    }
    return out;
}

Eigen::MatrixXd xyzToLab(const Eigen::MatrixXd &xyz, const Eigen::Vector3d &wp) {
    const double e = 216.0 / 24389.0;
    const double k = 24389.0 / 27.0;
    Eigen::MatrixXd relative = xyz;
    for (int i = 0; i < 3; ++i)
        relative.row(i).array() /= wp(i);

    Eigen::MatrixXd f = relative;
    for (Eigen::Index c = 0; c < relative.cols(); ++c) {
        for (int r = 0; r < 3; ++r) {
            double value = relative(r, c);
            f(r, c) = (value > e) ? std::cbrt(value) : (k * value + 16.0) / 116.0;
        }
    }

    Eigen::MatrixXd lab(3, xyz.cols());
    lab.row(0) = 116.0 * f.row(1).array() - 16.0;
    lab.row(1) = 500.0 * (f.row(0).array() - f.row(1).array());
    lab.row(2) = 200.0 * (f.row(1).array() - f.row(2).array());
    return lab;
}

double rms(const Eigen::ArrayXd &values) {
    return std::sqrt(values.square().mean());
}

double relLumDiff(const Eigen::VectorXd &A, const Eigen::VectorXd &A2, bool signedAverage) {
    Eigen::ArrayXd diff = (A2.array() - A.array()) / A.array();
    return signedAverage ? diff.mean() : rms(diff);
}

double colorDiffUv(const Eigen::MatrixXd &A, const Eigen::MatrixXd &A2) {
    Eigen::MatrixXd uvA = xyzToYuv(A);
    Eigen::MatrixXd uvB = xyzToYuv(A2);
    double sumSq = 0.0;
    for (Eigen::Index c = 0; c < A.cols(); ++c) {
        double du = uvB(1, c) - uvA(1, c);
        double dv = uvB(2, c) - uvA(2, c);
        double result = std::sqrt(du * du + dv * dv);
        sumSq += result * result;
    }
    return std::sqrt(sumSq / static_cast<double>(A.cols()));
}

double colorDiffYuv(const Eigen::MatrixXd &A, const Eigen::MatrixXd &A2) {
    Eigen::MatrixXd uvA = xyzToYuv(A);
    Eigen::MatrixXd uvB = xyzToYuv(A2);
    double sumSq = 0.0;
    for (Eigen::Index c = 0; c < A.cols(); ++c) {
        double dl = (uvB(0, c) - uvA(0, c)) / uvA(0, c);
        double du = uvB(1, c) - uvA(1, c);
        double dv = uvB(2, c) - uvA(2, c);
        double duv = std::sqrt(du * du + dv * dv);
        sumSq += dl * dl + duv * duv;
    }
    return std::sqrt(sumSq / static_cast<double>(2 * A.cols()));
}

Eigen::MatrixXd colorDiffYuvFull(const Eigen::MatrixXd &A, const Eigen::MatrixXd &A2) {
    Eigen::MatrixXd uvA = xyzToYuv(A);
    Eigen::MatrixXd uvB = xyzToYuv(A2);
    Eigen::ArrayXd du = uvB.row(1).array() - uvA.row(1).array();
    Eigen::ArrayXd dv = uvB.row(2).array() - uvA.row(2).array();
    Eigen::MatrixXd out(2, A.cols());
    out.row(0) = ((uvB.row(0).array() - uvA.row(0).array()) / uvA.row(0).array()).matrix();
    out.row(1) = (du.square() + dv.square()).sqrt().matrix();
    return out;
}

double colorDiffLab(const Eigen::MatrixXd &A, const Eigen::MatrixXd &A2) {
    Eigen::Vector3d wp = (A(1, 0) > A2(1, 0)) ? A.col(0) : A2.col(0);
    Eigen::MatrixXd labA = xyzToLab(A, wp);
    Eigen::MatrixXd labB = xyzToLab(A2, wp);
    Eigen::ArrayXd delta = ((labB - labA).array().square().colwise().sum().sqrt()) / 2.3;
    return rms(delta);
}

Eigen::MatrixXd colorDiffLabFull(const Eigen::MatrixXd &A, const Eigen::MatrixXd &A2) {
    Eigen::Vector3d wp = (A(1, 0) > A2(1, 0)) ? A.col(0) : A2.col(0);
    Eigen::MatrixXd labA = xyzToLab(A, wp);
    Eigen::MatrixXd labB = xyzToLab(A2, wp);
    Eigen::MatrixXd out(1, A.cols());
    out.row(0) = (((labB - labA).array().square().colwise().sum().sqrt()) / 2.3).matrix();
    return out;
}

double colorDiffXY(const Eigen::MatrixXd &A, const Eigen::MatrixXd &A2) {
    Eigen::MatrixXd xyA = xyzToXY(A);
    Eigen::MatrixXd xyB = xyzToXY(A2);
    double sumSq = 0.0;
    for (Eigen::Index c = 0; c < A.cols(); ++c) {
        double dx = xyB(0, c) - xyA(0, c);
        double dy = xyB(1, c) - xyA(1, c);
        sumSq += dx * dx + dy * dy;
    }
    return std::sqrt(sumSq / static_cast<double>(2 * A.cols()));
}

Eigen::MatrixXd colorDiffXYFull(const Eigen::MatrixXd &A, const Eigen::MatrixXd &A2) {
    return xyzToXY(A2) - xyzToXY(A);
}

double scalarLeastSquares(const Eigen::VectorXd &x, const Eigen::VectorXd &y) {
    double denom = x.dot(x);
    if (std::abs(denom) < 1e-20)
        throw std::runtime_error("Least-squares fit failed: degenerate input.");
    return x.dot(y) / denom;
}

Eigen::Matrix3d matrixFromVector(const Eigen::VectorXd &v) {
    Eigen::Matrix3d m;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m(i, j) = v(3 * i + j);
    return m;
}

Eigen::VectorXd vectorFromMatrix(const Eigen::Matrix3d &m) {
    Eigen::VectorXd v(9);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            v(3 * i + j) = m(i, j);
    return v;
}

Eigen::MatrixXd loadSampledData(const std::string &path, const std::string &cellsPath,
        std::map<std::string, std::string> *headerOut) {
    if (isRadianceHDR(path)) {
        HdrImage image = readRadianceHDR(path);
        if (headerOut)
            *headerOut = image.header;
        return sampleCells(image, loadCellFile(cellsPath), 179.0);
    }
    if (headerOut)
        headerOut->clear();
    return readTextData(path);
}

Eigen::VectorXd residualVectorFromMatrix(const Eigen::Matrix3d &matrix, const Eigen::MatrixXd &A,
        const Eigen::MatrixXd &B, const std::string &minimizer) {
    Eigen::MatrixXd A2 = matrix * B;
    if (lowerCopy(minimizer) == "lab") {
        Eigen::Vector3d wp;
        if (A(1, 0) > A2(1, 0))
            wp = A.col(0);
        else
            wp = A2.col(0);
        Eigen::MatrixXd labA = xyzToLab(A, wp);
        Eigen::MatrixXd labB = xyzToLab(A2, wp);
        Eigen::MatrixXd delta = (labB - labA) / 2.3;
        Eigen::VectorXd residual(delta.size());
        for (Eigen::Index c = 0; c < delta.cols(); ++c)
            residual.segment<3>(3 * c) = delta.col(c);
        return residual;
    }

    Eigen::MatrixXd uvA = xyzToYuv(A);
    Eigen::MatrixXd uvB = xyzToYuv(A2);
    Eigen::VectorXd residual(3 * A.cols());
    for (Eigen::Index c = 0; c < A.cols(); ++c) {
        residual(3 * c + 0) = (uvB(0, c) - uvA(0, c)) / uvA(0, c);
        residual(3 * c + 1) = uvB(1, c) - uvA(1, c);
        residual(3 * c + 2) = uvB(2, c) - uvA(2, c);
    }
    return residual;
}

template <typename ResidualFunction>
Eigen::VectorXd finiteDifferenceJacobian(const Eigen::VectorXd &x, const Eigen::VectorXd &baseResidual,
        ResidualFunction residualFunction) {
    Eigen::MatrixXd J(baseResidual.size(), x.size());
    for (Eigen::Index i = 0; i < x.size(); ++i) {
        double step = 1e-6 * std::max(1.0, std::abs(x(i)));
        Eigen::VectorXd xp = x;
        xp(i) += step;
        Eigen::VectorXd rp = residualFunction(xp);
        J.col(i) = (rp - baseResidual) / step;
    }
    return J;
}

template <typename ResidualFunction>
Eigen::VectorXd levenbergMarquardtMinimize(const Eigen::VectorXd &start, ResidualFunction residualFunction, int maxIter) {
    Eigen::VectorXd x = start;
    Eigen::VectorXd residual = residualFunction(x);
    double value = 0.5 * residual.squaredNorm();
    double lambda = 1e-3;

    for (int iter = 0; iter < maxIter; ++iter) {
        Eigen::MatrixXd J = finiteDifferenceJacobian(x, residual, residualFunction);
        Eigen::MatrixXd JTJ = J.transpose() * J;
        Eigen::VectorXd gradient = J.transpose() * residual;
        if (gradient.norm() < 1e-8)
            break;

        if (iter == 0)
            lambda = std::max(1e-6, 1e-3 * JTJ.diagonal().cwiseAbs().maxCoeff());

        Eigen::VectorXd step;
        bool solved = false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            Eigen::MatrixXd system = JTJ;
            system.diagonal().array() += lambda;
            Eigen::LDLT<Eigen::MatrixXd> solver(system);
            if (solver.info() == Eigen::Success) {
                step = solver.solve(-gradient);
                if (solver.info() == Eigen::Success) {
                    solved = true;
                    break;
                }
            }
            lambda *= 10.0;
        }
        if (!solved || step.norm() < 1e-9)
            break;

        Eigen::VectorXd candidate = x + step;
        Eigen::VectorXd candidateResidual = residualFunction(candidate);
        double candidateValue = 0.5 * candidateResidual.squaredNorm();

        if (std::isfinite(candidateValue) && candidateValue < value) {
            x = candidate;
            residual = candidateResidual;
            value = candidateValue;
            lambda = std::max(1e-9, lambda * 0.3);
        } else {
            lambda *= 8.0;
        }
    }
    return x;
}

template <typename Objective>
Eigen::VectorXd lineMinimizePowell(const Eigen::VectorXd &base, const Eigen::VectorXd &direction,
        Objective objective) {
    double dirNorm = direction.norm();
    if (!(dirNorm > 1e-12))
        return base;
    Eigen::VectorXd dir = direction / dirNorm;

    auto lineObjective = [&](double t) {
        return objective(base + t * dir);
    };

    double f0 = lineObjective(0.0);
    double step = 0.05 * std::max(1.0, base.norm() / std::sqrt(static_cast<double>(base.size())));
    double fp = lineObjective(step);
    double fn = lineObjective(-step);

    if (!(fp < f0 || fn < f0))
        return base;

    double a = 0.0;
    double b = (fp < fn) ? step : -step;
    double fb = std::min(fp, fn);
    const double expand = 1.61803398875;

    for (int i = 0; i < 24; ++i) {
        double c = b + expand * (b - a);
        double fc = lineObjective(c);
        if (fc >= fb) {
            double left = std::min(a, c);
            double right = std::max(a, c);
            const double gr = 0.61803398875;
            double x1 = right - gr * (right - left);
            double x2 = left + gr * (right - left);
            double f1 = lineObjective(x1);
            double f2 = lineObjective(x2);
            for (int iter = 0; iter < 48; ++iter) {
                if (f1 < f2) {
                    right = x2;
                    x2 = x1;
                    f2 = f1;
                    x1 = right - gr * (right - left);
                    f1 = lineObjective(x1);
                } else {
                    left = x1;
                    x1 = x2;
                    f1 = f2;
                    x2 = left + gr * (right - left);
                    f2 = lineObjective(x2);
                }
                if (std::abs(right - left) < 1e-6)
                    break;
            }
            double tBest = (f1 < f2) ? x1 : x2;
            return base + tBest * dir;
        }
        a = b;
        b = c;
        fb = fc;
    }

    return base + b * dir;
}

template <typename Objective>
Eigen::VectorXd powellMinimize(const Eigen::VectorXd &start, Objective objective, int maxIter) {
    const Eigen::Index n = start.size();
    std::vector<Eigen::VectorXd> directions(static_cast<size_t>(n), Eigen::VectorXd::Zero(n));
    for (Eigen::Index i = 0; i < n; ++i)
        directions[static_cast<size_t>(i)](i) = 1.0;

    Eigen::VectorXd x = start;
    double fx = objective(x);
    for (int iter = 0; iter < maxIter; ++iter) {
        Eigen::VectorXd xBegin = x;
        double fBegin = fx;
        double bestDrop = 0.0;
        Eigen::Index bestIndex = 0;

        for (Eigen::Index i = 0; i < n; ++i) {
            Eigen::VectorXd xNext = lineMinimizePowell(x, directions[static_cast<size_t>(i)], objective);
            double fNext = objective(xNext);
            double drop = fx - fNext;
            if (drop > bestDrop) {
                bestDrop = drop;
                bestIndex = i;
            }
            x = xNext;
            fx = fNext;
        }

        if (2.0 * std::abs(fBegin - fx) <= 1e-8 * (std::abs(fBegin) + std::abs(fx)) + 1e-10)
            break;

        Eigen::VectorXd newDirection = x - xBegin;
        if (!(newDirection.norm() > 1e-9))
            break;

        Eigen::VectorXd xNext = lineMinimizePowell(x, newDirection, objective);
        double fNext = objective(xNext);
        if (fNext < fx) {
            directions[static_cast<size_t>(bestIndex)] = newDirection;
            x = xNext;
            fx = fNext;
        }
    }
    return x;
}

struct NloptObjectiveData {
    const Eigen::MatrixXd *A = NULL;
    const Eigen::MatrixXd *B = NULL;
    std::string minimizer;
};

double nloptObjective(unsigned n, const double *x, double *grad, void *fdata) {
    NloptObjectiveData *data = static_cast<NloptObjectiveData *>(fdata);
    Eigen::Matrix3d matrix;
    for (unsigned i = 0; i < n; ++i)
        matrix(static_cast<int>(i / 3), static_cast<int>(i % 3)) = x[i];

    Eigen::MatrixXd A2 = matrix * (*data->B);
    double value = (data->minimizer == "lab")
        ? colorDiffLab(*data->A, A2)
        : colorDiffYuv(*data->A, A2);

    if (grad) {
        for (unsigned i = 0; i < n; ++i) {
            double step = 1e-7 * std::max(1.0, std::abs(x[i]));
            std::vector<double> xp(x, x + n);
            std::vector<double> xm(x, x + n);
            xp[i] += step;
            xm[i] -= step;

            Eigen::Matrix3d mp;
            Eigen::Matrix3d mm;
            for (unsigned j = 0; j < n; ++j) {
                mp(static_cast<int>(j / 3), static_cast<int>(j % 3)) = xp[j];
                mm(static_cast<int>(j / 3), static_cast<int>(j % 3)) = xm[j];
            }

            Eigen::MatrixXd A2p = mp * (*data->B);
            Eigen::MatrixXd A2m = mm * (*data->B);
            double fp = (data->minimizer == "lab")
                ? colorDiffLab(*data->A, A2p)
                : colorDiffYuv(*data->A, A2p);
            double fm = (data->minimizer == "lab")
                ? colorDiffLab(*data->A, A2m)
                : colorDiffYuv(*data->A, A2m);
            grad[i] = (fp - fm) / (2.0 * step);
        }
    }

    return value;
}

Eigen::VectorXd nloptSlsqpMinimize(const Eigen::VectorXd &start, const Eigen::MatrixXd &A,
        const Eigen::MatrixXd &B, const std::string &minimizer) {
    std::vector<double> x(static_cast<size_t>(start.size()));
    for (Eigen::Index i = 0; i < start.size(); ++i)
        x[static_cast<size_t>(i)] = start(i);

    NloptObjectiveData data;
    data.A = &A;
    data.B = &B;
    data.minimizer = lowerCopy(minimizer);

    int evals = 0;
    int forceStop = 0;
    char *stopMsg = NULL;
    nlopt_stopping stop;
    stop.n = static_cast<unsigned>(x.size());
    stop.minf_max = -HUGE_VAL;
    stop.ftol_rel = 1e-9;
    stop.ftol_abs = 0.0;
    stop.xtol_rel = 1e-8;
    stop.xtol_abs = NULL;
    stop.x_weights = NULL;
    stop.nevals_p = &evals;
    stop.maxeval = 100;
    stop.maxtime = 0.0;
    stop.start = nlopt_seconds();
    stop.force_stop = &forceStop;
    stop.stop_msg = &stopMsg;

    std::vector<double> lb(x.size(), -HUGE_VAL);
    std::vector<double> ub(x.size(), +HUGE_VAL);
    double optValue = std::numeric_limits<double>::infinity();
    nlopt_result result = nlopt_slsqp(static_cast<unsigned>(x.size()),
        nloptObjective, &data, 0, NULL, 0, NULL,
        lb.data(), ub.data(), x.data(), &optValue, &stop);

    std::string errMsg;
    if (stopMsg && *stopMsg)
        errMsg = stopMsg;
    std::free(stopMsg);

    if (result < 0)
        throw std::runtime_error(errMsg.empty() ? "Vendored SLSQP optimization failed." : errMsg);

    Eigen::VectorXd out(start.size());
    for (Eigen::Index i = 0; i < out.size(); ++i)
        out(i) = x[static_cast<size_t>(i)];
    return out;
}

CalibrationBundle calibrateColor(const ColorCalibrateOptions &opts) {
    std::map<std::string, std::string> refHeader;
    std::map<std::string, std::string> testHeader;
    Eigen::MatrixXd refd = loadSampledData(opts.referencePath, opts.referenceCellsPath, &refHeader);
    Eigen::MatrixXd testd = loadSampledData(opts.testPath, opts.testCellsPath, &testHeader);

    Eigen::Matrix3d rgbXYZ = parseReferenceColor(opts.referenceColor);
    Eigen::Matrix3d xyzRGB = rgbXYZ.inverse();

    Eigen::Matrix3d camXYZ = rgbXYZ;
    if (!opts.xyzcamOverride.empty()) {
        if (opts.xyzcamOverride.size() != 9)
            throw std::runtime_error("--xyzcam must contain 9 numbers.");
        Eigen::Matrix3d xyzcam;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                xyzcam(i, j) = opts.xyzcamOverride[3 * i + j];
        camXYZ = xyzcam.inverse();
    } else if (isRadianceHDR(opts.testPath)) {
        std::map<std::string, std::string>::const_iterator primariesIt = testHeader.find("TargetPrimaries");
        std::map<std::string, std::string>::const_iterator whiteIt = testHeader.find("TargetWhitePoint");
        std::map<std::string, std::string>::const_iterator xyzcamIt = testHeader.find("XYZCAM");
        if (primariesIt != testHeader.end() && whiteIt != testHeader.end()) {
            camXYZ = primariesToXYZ(parsePrimaries(primariesIt->second), parseWhitePoint(whiteIt->second));
        } else if (xyzcamIt != testHeader.end()) {
            Eigen::Matrix3d xyzcam = parseMatrix3x3(xyzcamIt->second, "XYZCAM");
            std::map<std::string, std::string>::const_iterator premultIt = testHeader.find("CAM_PREMULTIPLIERS");
            if (premultIt != testHeader.end()) {
                Eigen::Vector3d premult = parsePremultipliers(premultIt->second);
                for (int i = 0; i < 3; ++i) {
                    testd.row(i).array() /= premult(i);
                    xyzcam.row(i) /= premult(i);
                }
            }
            camXYZ = xyzcam.inverse();
        }
    }

    CalibrationBundle bundle;
    bundle.referenceData = refd;
    bundle.testData = testd;

    if (refd.rows() == 1) {
        Eigen::VectorXd By = (camXYZ * testd).row(1).transpose();
        double lumScale = scalarLeastSquares(By, refd.row(0).transpose());

        CalibrationEntry start;
        start.key = "start";
        start.label = "Default Matrix";
        start.xyzcam = camXYZ.inverse();
        bundle.entries.push_back(start);

        CalibrationEntry lum;
        lum.key = "lum";
        lum.label = "Luminance Least Squares";
        lum.xyzcam = (camXYZ * lumScale).inverse();
        bundle.entries.push_back(lum);

        for (size_t i = 0; i < bundle.entries.size(); ++i) {
            Eigen::MatrixXd Bxyz = bundle.entries[i].xyzcam.inverse() * testd;
            bundle.entries[i].dlum = relLumDiff(refd.row(0).transpose(), Bxyz.row(1).transpose(), false);
        }
        return bundle;
    }

    Eigen::MatrixXd A = rgbXYZ * refd;
    bundle.referenceData = A;

    Eigen::MatrixXd Bxyz = camXYZ * testd;
    double lumScale = scalarLeastSquares(Bxyz.row(1).transpose(), A.row(1).transpose());

    Eigen::MatrixXd Brgb = xyzRGB * Bxyz;
    double rf = scalarLeastSquares(Brgb.row(0).transpose(), refd.row(0).transpose());
    double gf = scalarLeastSquares(Brgb.row(1).transpose(), refd.row(1).transpose());
    double bf = scalarLeastSquares(Brgb.row(2).transpose(), refd.row(2).transpose());
    Eigen::Vector3d rgbScale(rf, gf, bf);

    Eigen::Matrix3d BBt = testd * testd.transpose();
    Eigen::Matrix3d ABt = A * testd.transpose();
    Eigen::Matrix3d camXYZOpt = ABt * BBt.inverse();

    Eigen::VectorXd nmStart = vectorFromMatrix(camXYZOpt);
    Eigen::Matrix3d camXYZOpt2 = camXYZOpt;
    try {
        Eigen::VectorXd slsqpBest = nloptSlsqpMinimize(nmStart, A, testd, opts.minimizer);
        camXYZOpt2 = matrixFromVector(slsqpBest);
    } catch (const std::exception &) {
        auto residualFunction = [&](const Eigen::VectorXd &candidate) {
            return residualVectorFromMatrix(matrixFromVector(candidate), A, testd, opts.minimizer);
        };
        auto residualObjective = [&](const Eigen::VectorXd &candidate) {
            Eigen::VectorXd residual = residualFunction(candidate);
            return 0.5 * residual.squaredNorm();
        };
        Eigen::VectorXd nmBest = levenbergMarquardtMinimize(nmStart, residualFunction, 120);
        Eigen::VectorXd powellBest = powellMinimize(nmBest, residualObjective, 40);
        if (residualObjective(powellBest) < residualObjective(nmBest))
            nmBest = powellBest;
        camXYZOpt2 = matrixFromVector(nmBest);
    }

    CalibrationEntry start;
    start.key = "start";
    start.label = "Default Matrix";
    start.xyzcam = camXYZ.inverse();
    bundle.entries.push_back(start);

    CalibrationEntry lum;
    lum.key = "lum";
    {
        std::ostringstream label;
        label << "Luminance Least Squares (" << std::fixed << std::setprecision(3) << lumScale << ")";
        lum.label = label.str();
    }
    lum.xyzcam = (camXYZ * lumScale).inverse();
    bundle.entries.push_back(lum);

    CalibrationEntry rgb;
    rgb.key = "rgb";
    {
        std::ostringstream label;
        label << "RGB Least Squares (" << std::fixed << std::setprecision(3)
              << rf << "," << gf << "," << bf << ")";
        rgb.label = label.str();
    }
    rgb.xyzcam = (rgbScale.asDiagonal() * camXYZ).inverse();
    bundle.entries.push_back(rgb);

    CalibrationEntry lopt;
    lopt.key = "lopt";
    lopt.label = "Color Matrix Optimization";
    lopt.xyzcam = camXYZOpt.inverse();
    bundle.entries.push_back(lopt);

    CalibrationEntry nopt;
    nopt.key = "nopt";
    nopt.label =
        "Color Matrix SLSQP Minimization (" + lowerCopy(opts.minimizer) + ")";
    nopt.xyzcam = camXYZOpt2.inverse();
    bundle.entries.push_back(nopt);

    for (size_t i = 0; i < bundle.entries.size(); ++i) {
        Eigen::MatrixXd entryXYZ = bundle.entries[i].xyzcam.inverse() * testd;
        bundle.entries[i].dlab = colorDiffLab(A, entryXYZ);
        bundle.entries[i].duv = colorDiffUv(A, entryXYZ);
        bundle.entries[i].dxy = colorDiffXY(A, entryXYZ);
        bundle.entries[i].dlum = relLumDiff(A.row(1).transpose(), entryXYZ.row(1).transpose(), true);
        bundle.entries[i].dlabFull = colorDiffLabFull(A, entryXYZ);
        bundle.entries[i].duvFull = colorDiffYuvFull(A, entryXYZ);
        bundle.entries[i].dxyFull = colorDiffXYFull(A, entryXYZ);
    }

    return bundle;
}

std::string formatMatrixLine(const std::string &label, const Eigen::Matrix3d &matrix) {
    std::ostringstream oss;
    oss << std::left << std::setw(40) << (label + ":") << " ";
    oss << std::fixed << std::setprecision(6);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (i != 0 || j != 0)
                oss << ' ';
            oss << matrix(i, j);
        }
    }
    return oss.str();
}

} // namespace

int runColorCalibration(const ColorCalibrateOptions &opts, std::ostream &out, std::ostream &err) {
    CalibrationBundle bundle = calibrateColor(opts);

    saveMatrixRows(opts.referenceDataOut, bundle.referenceData);
    saveMatrixRows(opts.testDataOut, bundle.testData);

    if (bundle.referenceData.rows() == 1) {
        err << "************************* RESULTS *************************\n";
        for (const CalibrationEntry &entry : bundle.entries)
            err << "lum diff for " << entry.key << ":\t" << std::fixed << std::setprecision(5)
                << entry.dlum << "\n";
        err << "***********************************************************\n";
    } else {
        err << "********************************** RESULTS *********************************\n";
        for (const CalibrationEntry &entry : bundle.entries) {
            err << "diff for " << entry.key << ":\tlab:" << std::fixed << std::setprecision(5) << entry.dlab
                << "\tuv:" << entry.duv << "\txy:" << entry.dxy
                << "\tlumMSD:" << std::showpos << std::setprecision(2) << (entry.dlum * 100.0) << "%\n";
            err << std::noshowpos << std::setprecision(5);
        }
        err << "****************************************************************************\n";
    }

    if (opts.verbose && !bundle.entries.empty() && bundle.referenceData.rows() == 3) {
        const CalibrationEntry &finalEntry = bundle.entries.back();
        out << "final optimization results:\n";
        if (lowerCopy(opts.minimizer) == "luv") {
            out << " #        Dlum         Duv\n";
            for (Eigen::Index i = 0; i < finalEntry.duvFull.cols(); ++i) {
                out << std::setw(2) << std::setfill('0') << i << std::setfill(' ')
                    << "  " << std::showpos << std::fixed << std::setprecision(2)
                    << (finalEntry.duvFull(0, i) * 100.0) << "%  "
                    << std::noshowpos << std::setprecision(4) << finalEntry.duvFull(1, i) << "\n";
            }
        } else {
            out << " #        De\n";
            for (Eigen::Index i = 0; i < finalEntry.dlabFull.cols(); ++i) {
                out << std::setw(2) << std::setfill('0') << i << std::setfill(' ')
                    << "  " << std::fixed << std::setprecision(2) << finalEntry.dlabFull(0, i) << "\n";
            }
        }
    }

    out << "xyzcam matrix:\n";
    for (const CalibrationEntry &entry : bundle.entries)
        out << formatMatrixLine(entry.label, entry.xyzcam) << "\n";
    return 0;
}
