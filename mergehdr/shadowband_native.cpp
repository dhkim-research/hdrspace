#include "shadowband_native.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fftw3.h>

namespace {

const double kPi = 3.14159265358979323846;
const std::array<double, 3> kRadianceLumWeights{{0.26507413, 0.67011463, 0.06481124}};

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct PixelGeometry {
    Vec3d dir;
    double omega = 0.0;
    bool valid = false;
};

double clampDot(double value) {
    return std::max(-1.0, std::min(1.0, value));
}

Vec3d makeVec(double x, double y, double z) {
    Vec3d v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

Vec3d operator+(const Vec3d &a, const Vec3d &b) {
    return makeVec(a.x + b.x, a.y + b.y, a.z + b.z);
}

Vec3d operator-(const Vec3d &a, const Vec3d &b) {
    return makeVec(a.x - b.x, a.y - b.y, a.z - b.z);
}

Vec3d operator*(const Vec3d &v, double s) {
    return makeVec(v.x * s, v.y * s, v.z * s);
}

Vec3d cross(const Vec3d &a, const Vec3d &b) {
    return makeVec(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    );
}

double dot(const Vec3d &a, const Vec3d &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

double norm(const Vec3d &v) {
    return std::sqrt(dot(v, v));
}

Vec3d normalize(const Vec3d &v) {
    const double n = norm(v);
    if (!(n > 0.0))
        return makeVec(0.0, 0.0, 0.0);
    return v * (1.0 / n);
}

Vec3d rotateAroundZ(const Vec3d &v, double degrees) {
    const double radians = degrees * kPi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    return makeVec(c * v.x - s * v.y, s * v.x + c * v.y, v.z);
}

Vec3d rotateAroundY(const Vec3d &v, double degrees) {
    const double radians = degrees * kPi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    return makeVec(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

using Mat3 = std::array<std::array<double, 3>, 3>;

Mat3 identity3() {
    return {{{{1.0, 0.0, 0.0}}, {{0.0, 1.0, 0.0}}, {{0.0, 0.0, 1.0}}}};
}

Vec3d mul(const Mat3 &m, const Vec3d &v) {
    return makeVec(
        m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
        m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
        m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z
    );
}

Mat3 transpose(const Mat3 &m) {
    Mat3 out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            out[r][c] = m[c][r];
    }
    return out;
}

bool almostEqualVec(const Vec3d &a, const Vec3d &b, double eps = 1e-9) {
    return std::abs(a.x - b.x) <= eps &&
           std::abs(a.y - b.y) <= eps &&
           std::abs(a.z - b.z) <= eps;
}

double wrapAngle2Pi(double angle) {
    const double twoPi = 2.0 * kPi;
    angle = std::fmod(angle, twoPi);
    if (angle < 0.0)
        angle += twoPi;
    return angle;
}

struct AngularViewMapper {
    Vec3d dxyz = makeVec(0.0, 1.0, 0.0);
    double viewAngle = 180.0;
    double chordFactor = 1.0;
    Mat3 ymtx = identity3();
    Mat3 pmtx = identity3();

    AngularViewMapper() = default;

    AngularViewMapper(const Vec3d &dir, double angleDegrees) {
        dxyz = normalize(dir);
        viewAngle = angleDegrees;

        const double cl = (2.0 * std::sin((kPi / 2.0) / 2.0)) / (kPi / 2.0);
        const double va = viewAngle * kPi / 360.0;
        const double clp = va > 0.0 ? (2.0 * std::sin(va / 2.0)) / va : 1.0;
        chordFactor = cl / clp;

        const Vec3d z = makeVec(0.0, 0.0, 1.0);
        const Vec3d negZ = makeVec(0.0, 0.0, -1.0);
        if (almostEqualVec(dxyz, z)) {
            ymtx = identity3();
            pmtx = identity3();
            return;
        }
        if (almostEqualVec(dxyz, negZ)) {
            ymtx = {{{{-1.0, 0.0, 0.0}}, {{0.0, -1.0, 0.0}}, {{0.0, 0.0, 1.0}}}};
            pmtx = {{{{1.0, 0.0, 0.0}}, {{0.0, -1.0, 0.0}}, {{0.0, 0.0, -1.0}}}};
            return;
        }

        const double theta = std::acos(clampDot(dxyz.z));
        double phi;
        if (std::abs(theta) <= 1e-10 || std::abs(theta - kPi) <= 1e-10)
            phi = kPi;
        else
            phi = wrapAngle2Pi(std::atan2(dxyz.y, dxyz.x));

        const double y = 3.0 * kPi / 2.0 - phi;
        const double cy = std::cos(y);
        const double sy = std::sin(y);
        ymtx = {{{{cy, sy, 0.0}}, {{-sy, cy, 0.0}}, {{0.0, 0.0, 1.0}}}};

        const double p = -theta;
        const double cp = std::cos(p);
        const double sp = std::sin(p);
        pmtx = {{{{1.0, 0.0, 0.0}}, {{0.0, cp, sp}}, {{0.0, -sp, cp}}}};
    }

    Vec3d worldToView(const Vec3d &xyz) const {
        return mul(pmtx, mul(ymtx, xyz));
    }

    Vec3d viewToWorld(const Vec3d &xyz) const {
        return mul(transpose(ymtx), mul(transpose(pmtx), xyz));
    }

    std::array<double, 2> xyz2vxy(const Vec3d &xyz) const {
        const Vec3d rxyz = worldToView(xyz);
        const double radial = std::atan2(std::sqrt(rxyz.x * rxyz.x + rxyz.y * rxyz.y), rxyz.z) / (kPi / 2.0);
        const double phi = std::atan2(rxyz.x, rxyz.y);
        double xx = radial * std::sin(phi);
        const double yy = radial * std::cos(phi);
        xx = -xx;
        const double scale = 180.0 / (viewAngle * chordFactor) / 2.0;
        return {{xx * scale + 0.5, yy * scale + 0.5}};
    }

    Vec3d vxy2xyz(const std::array<double, 2> &xy) const {
        double px = xy[0] - 0.5;
        double py = xy[1] - 0.5;
        const double scale = (viewAngle * chordFactor) / 180.0;
        px *= -scale;
        py *= scale;
        const double d = std::sqrt(px * px + py * py);
        const double z = std::cos(kPi * d);
        const double f = d <= 0.0 ? kPi : std::sqrt(std::max(0.0, 1.0 - z * z)) / d;
        return viewToWorld(makeVec(px * f, py * f, z));
    }

    Vec3d pixelRay(int x, int y, int res) const {
        return vxy2xyz({{(static_cast<double>(x) + 0.5) / static_cast<double>(res),
                         (static_cast<double>(y) + 0.5) / static_cast<double>(res)}});
    }

    Vec3d uvRay(int x, int y, int res) const {
        const double uvx = (static_cast<double>(x) + 0.5) / static_cast<double>(res);
        const double uvy = (static_cast<double>(y) + 0.5) / static_cast<double>(res);
        const double sf = viewAngle / 180.0;
        const double u = (0.5 - sf * 0.5) + uvx * sf;
        const double v = (0.5 - sf * 0.5) + uvy * sf;

        const double a = 2.0 * u - 1.0;
        const double b = 2.0 * v - 1.0;
        const bool cond = a * a > b * b;
        const double r = cond ? a : ((std::abs(b) <= 1e-15) ? 0.0 : b);
        const double phi = (cond ? (b / (2.0 * a))
                                 : ((std::abs(b) <= 1e-15) ? 0.0 : (1.0 - a / (2.0 * b)))) * kPi * 0.5;
        const double sphterm = r * std::sqrt(std::max(0.0, 2.0 - r * r));
        const Vec3d local = makeVec(-std::cos(phi) * sphterm, std::sin(phi) * sphterm, 1.0 - r * r);
        return normalize(viewToWorld(local));
    }

    std::array<double, 2> ray2pixel(const Vec3d &xyz, int res, bool integer) const {
        const std::array<double, 2> vxy = xyz2vxy(xyz);
        std::array<double, 2> pxy{{vxy[0] * static_cast<double>(res), vxy[1] * static_cast<double>(res)}};
        if (integer) {
            pxy[0] = std::floor(pxy[0]);
            pxy[1] = std::floor(pxy[1]);
        }
        return pxy;
    }

    double pixelOmega(const std::array<double, 2> &pxy, int res) const {
        const std::array<double, 2> ofx{{0.5, 0.0}};
        const std::array<double, 2> ofy{{0.0, 0.5}};
        const Vec3d xa = vxy2xyz({{(pxy[0] - ofx[0]) / static_cast<double>(res), (pxy[1] - ofx[1]) / static_cast<double>(res)}});
        const Vec3d xb = vxy2xyz({{(pxy[0] + ofx[0]) / static_cast<double>(res), (pxy[1] + ofx[1]) / static_cast<double>(res)}});
        const Vec3d ya = vxy2xyz({{(pxy[0] - ofy[0]) / static_cast<double>(res), (pxy[1] - ofy[1]) / static_cast<double>(res)}});
        const Vec3d yb = vxy2xyz({{(pxy[0] + ofy[0]) / static_cast<double>(res), (pxy[1] + ofy[1]) / static_cast<double>(res)}});
        return norm(cross(xb - xa, yb - ya));
    }

    double degrees(const Vec3d &xyz) const {
        return std::acos(clampDot(dot(dxyz, normalize(xyz)))) * 180.0 / kPi;
    }

    bool inView(const Vec3d &xyz, double tolDegrees = 0.0) const {
        const double ang = std::acos(clampDot(dot(dxyz, normalize(xyz)))) - tolDegrees * kPi / 180.0;
        return ang < (chordFactor * viewAngle * kPi / 360.0);
    }
};

struct RaytoolsExactViewMapper {
    Vec3d dxyz = makeVec(0.0, 1.0, 0.0);
    double viewAngle = 180.0;
    double aspect = 1.0;
    double chordFactor = 1.0;
    Mat3 ymtx = identity3();
    Mat3 pmtx = identity3();

    RaytoolsExactViewMapper() = default;

    RaytoolsExactViewMapper(const Vec3d &dir, double angleDegrees) {
        dxyz = normalize(dir);
        viewAngle = angleDegrees;
        aspect = (viewAngle == 360.0) ? 2.0 : 1.0;
        const double cl = (2.0 * std::sin((kPi / 2.0) / 2.0)) / (kPi / 2.0);
        const double va = viewAngle * kPi / 360.0;
        const double clp = va > 0.0 ? (2.0 * std::sin(va / 2.0)) / va : 1.0;
        chordFactor = cl / clp;

        const Vec3d z = makeVec(0.0, 0.0, 1.0);
        const Vec3d negZ = makeVec(0.0, 0.0, -1.0);
        if (almostEqualVec(dxyz, z)) {
            ymtx = identity3();
            pmtx = identity3();
            return;
        }
        if (almostEqualVec(dxyz, negZ)) {
            ymtx = {{{{-1.0, 0.0, 0.0}}, {{0.0, -1.0, 0.0}}, {{0.0, 0.0, 1.0}}}};
            pmtx = {{{{1.0, 0.0, 0.0}}, {{0.0, -1.0, 0.0}}, {{0.0, 0.0, -1.0}}}};
            return;
        }

        const double theta = std::acos(clampDot(dxyz.z));
        const double phi = (std::abs(theta) <= 1e-10 || std::abs(theta - kPi) <= 1e-10)
            ? kPi
            : wrapAngle2Pi(std::atan2(dxyz.y, dxyz.x));

        const double y = 3.0 * kPi / 2.0 - phi;
        const double cy = std::cos(y);
        const double sy = std::sin(y);
        ymtx = {{{{cy, -sy, 0.0}}, {{sy, cy, 0.0}}, {{0.0, 0.0, 1.0}}}};

        const double p = -theta;
        const double cp = std::cos(p);
        const double sp = std::sin(p);
        pmtx = {{{{1.0, 0.0, 0.0}}, {{0.0, cp, -sp}}, {{0.0, sp, cp}}}};
    }

    Vec3d worldToView(const Vec3d &xyz) const {
        return mul(pmtx, mul(ymtx, xyz));
    }

    Vec3d viewToWorld(const Vec3d &xyz) const {
        return mul(transpose(ymtx), mul(transpose(pmtx), xyz));
    }

    std::array<double, 2> xyz2xy(const Vec3d &xyz) const {
        const Vec3d rxyz = worldToView(xyz);
        const double radial = std::atan2(std::sqrt(rxyz.x * rxyz.x + rxyz.y * rxyz.y), rxyz.z) / (kPi / 2.0);
        const double phi = std::atan2(rxyz.x, rxyz.y);
        const double xx = -radial * std::sin(phi);
        const double yy = radial * std::cos(phi);
        return {{xx, yy}};
    }

    Vec3d vxy2xyz(const std::array<double, 2> &xy) const {
        double px = xy[0] - 0.5;
        double py = xy[1] - 0.5;
        px *= -1.0 * (viewAngle * chordFactor) / 180.0;
        py *= (viewAngle * chordFactor) / 180.0;
        const double d = std::sqrt(px * px + py * py);
        const double z = std::cos(kPi * d);
        const double f = d <= 0.0 ? kPi : std::sqrt(std::max(0.0, 1.0 - z * z)) / d;
        return viewToWorld(makeVec(px * f, py * f, z));
    }

    Vec3d pixelRay(int x, int y, int res) const {
        return vxy2xyz({{(static_cast<double>(x) + 0.5) / static_cast<double>(res),
                         (static_cast<double>(y) + 0.5) / static_cast<double>(res)}});
    }

    std::array<double, 2> ray2pixel(const Vec3d &xyz, int res, bool integer) const {
        const std::array<double, 2> xy = xyz2xy(xyz);
        std::array<double, 2> pxy{{
            (xy[0] * 180.0 / (viewAngle * chordFactor)) / 2.0 + 0.5,
            (xy[1] * 180.0 / (viewAngle * chordFactor)) / 2.0 + 0.5
        }};
        pxy[0] *= static_cast<double>(res);
        pxy[1] *= static_cast<double>(res);
        if (integer) {
            pxy[0] = std::floor(pxy[0]);
            pxy[1] = std::floor(pxy[1]);
        }
        return pxy;
    }

    double pixelOmega(const std::array<double, 2> &pxy, int res) const {
        const std::array<double, 2> ofx{{0.5, 0.0}};
        const std::array<double, 2> ofy{{0.0, 0.5}};
        const Vec3d xa = vxy2xyz({{(pxy[0] - ofx[0]) / static_cast<double>(res), (pxy[1] - ofx[1]) / static_cast<double>(res)}});
        const Vec3d xb = vxy2xyz({{(pxy[0] + ofx[0]) / static_cast<double>(res), (pxy[1] + ofx[1]) / static_cast<double>(res)}});
        const Vec3d ya = vxy2xyz({{(pxy[0] - ofy[0]) / static_cast<double>(res), (pxy[1] - ofy[1]) / static_cast<double>(res)}});
        const Vec3d yb = vxy2xyz({{(pxy[0] + ofy[0]) / static_cast<double>(res), (pxy[1] + ofy[1]) / static_cast<double>(res)}});
        return norm(cross(xb - xa, yb - ya));
    }

    double degrees(const Vec3d &xyz) const {
        return std::acos(clampDot(dot(dxyz, normalize(xyz)))) * 180.0 / kPi;
    }

    bool inView(const Vec3d &xyz, double tolDegrees = 0.0) const {
        const double ang = degrees(xyz) - tolDegrees;
        return ang < (chordFactor * viewAngle / 2.0);
    }
};

std::vector<double> extractChannel(const HdrImage &image, int channel) {
    std::vector<double> values(image.width * image.height, 0.0);
    for (size_t i = 0; i < values.size(); ++i)
        values[i] = image.rgb[i * 3 + static_cast<size_t>(channel)];
    return values;
}

double radianceLuminance(const float *rgb) {
    return rgb[0] * kRadianceLumWeights[0] +
           rgb[1] * kRadianceLumWeights[1] +
           rgb[2] * kRadianceLumWeights[2];
}

std::vector<double> computeLuminance(const HdrImage &image) {
    std::vector<double> luminance(image.width * image.height, 0.0);
    for (size_t i = 0; i < luminance.size(); ++i)
        luminance[i] = radianceLuminance(&image.rgb[i * 3]);
    return luminance;
}

HdrImage makeGrayHdr(size_t width, size_t height, const std::vector<double> &values) {
    HdrImage image;
    image.width = width;
    image.height = height;
    image.rgb.assign(width * height * 3, 0.0f);
    for (size_t i = 0; i < values.size(); ++i) {
        const float v = static_cast<float>(values[i]);
        image.rgb[i * 3 + 0] = v;
        image.rgb[i * 3 + 1] = v;
        image.rgb[i * 3 + 2] = v;
    }
    return image;
}

HdrImage cropTopDown(const HdrImage &image, int left, int top, int width, int height) {
    HdrImage out;
    out.width = static_cast<size_t>(width);
    out.height = static_cast<size_t>(height);
    out.header = image.header;
    out.headerLines = image.headerLines;
    out.rgb.assign(out.width * out.height * 3, 0.0f);

    for (int y = 0; y < height; ++y) {
        const size_t srcY = static_cast<size_t>(top + y);
        const size_t dstY = static_cast<size_t>(y);
        const size_t srcIndex = (srcY * image.width + static_cast<size_t>(left)) * 3;
        const size_t dstIndex = dstY * out.width * 3;
        std::copy_n(image.rgb.begin() + static_cast<std::ptrdiff_t>(srcIndex),
                    static_cast<std::ptrdiff_t>(out.width * 3),
                    out.rgb.begin() + static_cast<std::ptrdiff_t>(dstIndex));
    }
    return out;
}

inline void sampleBilinearHdr(const HdrImage &image, double xCenter, double yCenter, float *out) {
    xCenter = std::max(0.5, std::min(static_cast<double>(image.width) - 0.5, xCenter));
    yCenter = std::max(0.5, std::min(static_cast<double>(image.height) - 0.5, yCenter));

    const double xi = xCenter - 0.5;
    const double yi = yCenter - 0.5;
    const int x0 = static_cast<int>(std::floor(xi));
    const int y0 = static_cast<int>(std::floor(yi));
    const int x1 = std::min<int>(x0 + 1, static_cast<int>(image.width) - 1);
    const int y1 = std::min<int>(y0 + 1, static_cast<int>(image.height) - 1);
    const double tx = xi - x0;
    const double ty = yi - y0;

    const float *p00 = image.rgb.data() + (static_cast<size_t>(y0) * image.width + static_cast<size_t>(x0)) * 3;
    const float *p10 = image.rgb.data() + (static_cast<size_t>(y0) * image.width + static_cast<size_t>(x1)) * 3;
    const float *p01 = image.rgb.data() + (static_cast<size_t>(y1) * image.width + static_cast<size_t>(x0)) * 3;
    const float *p11 = image.rgb.data() + (static_cast<size_t>(y1) * image.width + static_cast<size_t>(x1)) * 3;

    for (int channel = 0; channel < 3; ++channel) {
        const double a = p00[channel] * (1.0 - tx) + p10[channel] * tx;
        const double b = p01[channel] * (1.0 - tx) + p11[channel] * tx;
        out[channel] = static_cast<float>(a * (1.0 - ty) + b * ty);
    }
}

HdrImage projectEquisolidToEquidistantTopDown(const HdrImage &image) {
    if (image.width != image.height)
        throw std::runtime_error("shadowband fisheye reprojection expects square HDR inputs.");

    HdrImage projected = image;
    projected.rgb.assign(image.width * image.height * 3, 0.0f);
    const double sqrt2 = std::sqrt(2.0);

    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(image.height); ++y) {
        for (size_t x = 0; x < image.width; ++x) {
            float *dst = projected.rgb.data() + (static_cast<size_t>(y) * image.width + x) * 3;
            dst[0] = dst[1] = dst[2] = 0.0f;

            const double dx = (static_cast<double>(x) + 0.5) / static_cast<double>(image.width) - 0.5;
            const double dy = (static_cast<double>(y) + 0.5) / static_cast<double>(image.height) - 0.5;
            const double radius = std::sqrt(dx * dx + dy * dy);
            if (radius > 0.5)
                continue;

            double sourceRadius = 0.0;
            if (radius > 1e-12)
                sourceRadius = std::sin(0.5 * kPi * radius) / sqrt2;

            const double scale = radius > 1e-12 ? sourceRadius / radius : 1.0;
            const double sourceU = 0.5 + dx * scale;
            const double sourceV = 0.5 + dy * scale;
            sampleBilinearHdr(image, sourceU * image.width, sourceV * image.height, dst);
        }
    }
    return projected;
}

int reflectIndex(int i, int n) {
    if (n <= 1)
        return 0;
    while (i < 0 || i >= n) {
        if (i < 0)
            i = -i - 1;
        else
            i = 2 * n - i - 1;
    }
    return i;
}

std::vector<double> uniformFilter(const std::vector<double> &image, size_t width, size_t height, double windowRadius) {
    const int size = std::max(1, static_cast<int>(windowRadius));
    const int left = size / 2;
    const int right = size - left - 1;

    std::vector<double> temp(width * height, 0.0);
    std::vector<double> out(width * height, 0.0);

    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(height); ++y) {
        std::vector<double> padded(width + static_cast<size_t>(left + right), 0.0);
        for (size_t i = 0; i < padded.size(); ++i) {
            const int sx = reflectIndex(static_cast<int>(i) - left, static_cast<int>(width));
            padded[i] = image[static_cast<size_t>(y) * width + static_cast<size_t>(sx)];
        }
        std::vector<double> prefix(padded.size() + 1, 0.0);
        for (size_t i = 0; i < padded.size(); ++i)
            prefix[i + 1] = prefix[i] + padded[i];
        for (size_t x = 0; x < width; ++x) {
            const double sum = prefix[x + static_cast<size_t>(size)] - prefix[x];
            temp[static_cast<size_t>(y) * width + x] = sum / static_cast<double>(size);
        }
    }

    #pragma omp parallel for
    for (int x = 0; x < static_cast<int>(width); ++x) {
        std::vector<double> padded(height + static_cast<size_t>(left + right), 0.0);
        for (size_t i = 0; i < padded.size(); ++i) {
            const int sy = reflectIndex(static_cast<int>(i) - left, static_cast<int>(height));
            padded[i] = temp[static_cast<size_t>(sy) * width + static_cast<size_t>(x)];
        }
        std::vector<double> prefix(padded.size() + 1, 0.0);
        for (size_t i = 0; i < padded.size(); ++i)
            prefix[i + 1] = prefix[i] + padded[i];
        for (size_t y = 0; y < height; ++y) {
            const double sum = prefix[y + static_cast<size_t>(size)] - prefix[y];
            out[y * width + static_cast<size_t>(x)] = sum / static_cast<double>(size);
        }
    }

    return out;
}

std::vector<double> maximumFilterBinary(const std::vector<uint8_t> &image, size_t width, size_t height, double windowRadius) {
    const int size = std::max(1, static_cast<int>(windowRadius));
    const int left = size / 2;
    const int right = size - left - 1;

    std::vector<double> temp(width * height, 0.0);
    std::vector<double> out(width * height, 0.0);

    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(height); ++y) {
        std::vector<uint8_t> padded(width + static_cast<size_t>(left + right), 0);
        for (size_t i = 0; i < padded.size(); ++i) {
            const int sx = reflectIndex(static_cast<int>(i) - left, static_cast<int>(width));
            padded[i] = image[static_cast<size_t>(y) * width + static_cast<size_t>(sx)];
        }
        std::vector<int> prefix(padded.size() + 1, 0);
        for (size_t i = 0; i < padded.size(); ++i)
            prefix[i + 1] = prefix[i] + static_cast<int>(padded[i]);
        for (size_t x = 0; x < width; ++x) {
            const int sum = prefix[x + static_cast<size_t>(size)] - prefix[x];
            temp[static_cast<size_t>(y) * width + x] = sum > 0 ? 1.0 : 0.0;
        }
    }

    #pragma omp parallel for
    for (int x = 0; x < static_cast<int>(width); ++x) {
        std::vector<uint8_t> padded(height + static_cast<size_t>(left + right), 0);
        for (size_t i = 0; i < padded.size(); ++i) {
            const int sy = reflectIndex(static_cast<int>(i) - left, static_cast<int>(height));
            padded[i] = temp[static_cast<size_t>(sy) * width + static_cast<size_t>(x)] > 0.0 ? 1 : 0;
        }
        std::vector<int> prefix(padded.size() + 1, 0);
        for (size_t i = 0; i < padded.size(); ++i)
            prefix[i + 1] = prefix[i] + static_cast<int>(padded[i]);
        for (size_t y = 0; y < height; ++y) {
            const int sum = prefix[y + static_cast<size_t>(size)] - prefix[y];
            out[y * width + static_cast<size_t>(x)] = sum > 0 ? 1.0 : 0.0;
        }
    }

    return out;
}

std::vector<double> maximumFilterFloat(const std::vector<double> &image, size_t width, size_t height, double windowRadius) {
    const int size = std::max(1, static_cast<int>(windowRadius));
    const int left = size / 2;
    const int right = size - left - 1;
    std::vector<double> temp(width * height, 0.0);
    std::vector<double> out(width * height, 0.0);

    for (size_t y = 0; y < height; ++y) {
        std::deque<int> dq;
        for (int x = 0; x < static_cast<int>(width); ++x) {
            while (!dq.empty() && dq.front() < x - left)
                dq.pop_front();
            while (!dq.empty() && image[y * width + static_cast<size_t>(dq.back())] <= image[y * width + static_cast<size_t>(x)])
                dq.pop_back();
            dq.push_back(x);
            const int outX = x - right;
            if (outX >= 0)
                temp[y * width + static_cast<size_t>(outX)] = image[y * width + static_cast<size_t>(dq.front())];
        }
        for (int x = std::max<int>(0, static_cast<int>(width) - right - 1); x < static_cast<int>(width); ++x) {
            while (!dq.empty() && dq.front() < x - left)
                dq.pop_front();
            temp[y * width + static_cast<size_t>(x)] = image[y * width + static_cast<size_t>(dq.front())];
        }
    }

    for (size_t x = 0; x < width; ++x) {
        std::deque<int> dq;
        for (int y = 0; y < static_cast<int>(height); ++y) {
            while (!dq.empty() && dq.front() < y - left)
                dq.pop_front();
            while (!dq.empty() && temp[static_cast<size_t>(dq.back()) * width + x] <= temp[static_cast<size_t>(y) * width + x])
                dq.pop_back();
            dq.push_back(y);
            const int outY = y - right;
            if (outY >= 0)
                out[static_cast<size_t>(outY) * width + x] = temp[static_cast<size_t>(dq.front()) * width + x];
        }
        for (int y = std::max<int>(0, static_cast<int>(height) - right - 1); y < static_cast<int>(height); ++y) {
            while (!dq.empty() && dq.front() < y - left)
                dq.pop_front();
            out[static_cast<size_t>(y) * width + x] = temp[static_cast<size_t>(dq.front()) * width + x];
        }
    }

    return out;
}

PixelGeometry geometryAt(size_t width, size_t height, int x, int y) {
    PixelGeometry sample;
    const double pixelOmega = (2.0 * kPi) / (static_cast<double>(width) * static_cast<double>(height));
    const double nx = (static_cast<double>(x) + 0.5) / static_cast<double>(width) - 0.5;
    const double ny = 0.5 - (static_cast<double>(y) + 0.5) / static_cast<double>(height);
    const double radius = std::sqrt(nx * nx + ny * ny);

    const double horizontalRadians = kPi;
    const double verticalRadians = kPi;
    const double u = nx * horizontalRadians;
    const double v = ny * verticalRadians;
    const double theta = std::sqrt(u * u + v * v);
    const Vec3d forward = makeVec(0.0, 1.0, 0.0);
    const Vec3d right = makeVec(1.0, 0.0, 0.0);
    const Vec3d up = makeVec(0.0, 0.0, 1.0);
    if (radius <= 1e-12) {
        sample.dir = forward;
        sample.omega = pixelOmega;
        sample.valid = true;
        return sample;
    }

    const double sinTheta = std::sin(theta);
    const double scale = sinTheta / std::max(theta, 1e-12);
    sample.dir = normalize(forward * std::cos(theta) + right * (u * scale) + up * (v * scale));
    sample.omega = pixelOmega;
    sample.valid = sample.dir.y >= 0.0;
    return sample;
}

std::vector<PixelGeometry> buildGeometry(size_t width, size_t height) {
    std::vector<PixelGeometry> geometry(width * height);
    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(height); ++y) {
        for (int x = 0; x < static_cast<int>(width); ++x)
            geometry[static_cast<size_t>(y) * width + static_cast<size_t>(x)] = geometryAt(width, height, x, y);
    }
    return geometry;
}

std::array<double, 2> profileAngles(const Vec3d &dir, double rh, double rv) {
    const Vec3d va = rv == 0.0 ? dir : rotateAroundY(dir, -rv);
    const Vec3d ha = rh == 0.0 ? dir : rotateAroundY(dir, -rh);
    const double a0 = std::atan2(va.x, va.y) * 180.0 / kPi;
    const double a1 = std::atan2(ha.z, ha.y) * 180.0 / kPi;
    return {{a0, a1}};
}

struct GeneratedMasks {
    std::vector<double> blendMask;
    std::vector<double> primarySourceMask;
    std::vector<double> angularDistanceDeg;
    std::vector<uint8_t> validMask;
};

GeneratedMasks generateMasks(const std::vector<PixelGeometry> &geometry, size_t width, size_t height,
        const Vec3d &sunDir, double roh, double rov, double bw) {
    const std::array<double, 2> sunAngles45 = profileAngles(sunDir, 45.0 + (roh + rov) * 0.5, 45.0 + (roh + rov) * 0.5);
    const std::array<double, 2> sunAngles = profileAngles(sunDir, roh, rov);

    std::vector<double> mask(width * height, 0.0);
    std::vector<uint8_t> initBlendMask(width * height, 0);
    std::vector<uint8_t> priSourceDiamond(width * height, 0);
    std::vector<uint8_t> secSourceBinary(width * height, 0);
    std::vector<double> angularDistance(width * height, 0.0);
    std::vector<uint8_t> validMask(width * height, 0);

    const double vangle = sunAngles[0] / 90.0;
    const double sv0 = std::abs(std::sin(sunAngles[0] * kPi / 180.0));
    const double sv1 = std::abs(std::sin(sunAngles[1] * kPi / 180.0));
    const double angrat = sv0 / std::max(1e-9, sv0 + sv1);
    const double band = bw / 180.0 * static_cast<double>(width);

    for (size_t i = 0; i < geometry.size(); ++i) {
        validMask[i] = geometry[i].valid ? 1 : 0;

        const std::array<double, 2> a45 = profileAngles(geometry[i].dir, 45.0 + (roh + rov) * 0.5, 45.0 + (roh + rov) * 0.5);
        const double d45x = sunAngles45[0] - a45[0];
        const double d45y = sunAngles45[1] - a45[1];
        if (d45x > 0.0 && d45y <= 0.0)
            mask[i] = 0.0;
        else if (d45x <= 0.0 && d45y <= 0.0)
            mask[i] = 1.0;
        else if (d45x > 0.0 && d45y > 0.0)
            mask[i] = 1.0;
        else
            mask[i] = 0.0;

        const std::array<double, 2> a = profileAngles(geometry[i].dir, roh, rov);
        const double pd0 = std::abs(sunAngles[0] - a[0]);
        const double pd1 = std::abs(sunAngles[1] - a[1]);
        if (pd0 < bw * 1.5 || pd1 < bw * 1.5)
            initBlendMask[i] = 1;

        const double dist = std::acos(clampDot(dot(geometry[i].dir, sunDir))) * 180.0 / kPi;
        angularDistance[i] = dist;
        if (pd0 < bw * 0.75 && pd1 < bw * 0.75 * (1.0 + std::abs(vangle)))
            priSourceDiamond[i] = 1;
        if (dist < bw * 1.5)
            secSourceBinary[i] = 1;
    }

    const std::vector<double> initBlend = maximumFilterBinary(initBlendMask, width, height, band);
    const std::vector<double> blurredMask = uniformFilter(mask, width, height, band);

    std::vector<double> thresholdMask(width * height, 0.0);
    for (size_t i = 0; i < thresholdMask.size(); ++i)
        thresholdMask[i] = blurredMask[i] > angrat ? 1.0 : 0.0;
    const std::vector<double> blurredThreshold = uniformFilter(thresholdMask, width, height, std::max(1.0, band / 4.0));

    std::vector<double> finalMask = blurredMask;
    for (size_t i = 0; i < finalMask.size(); ++i) {
        if (initBlend[i] > 0.5)
            finalMask[i] = blurredThreshold[i];
    }

    const std::vector<double> expandedPrimary = maximumFilterBinary(priSourceDiamond, width, height, band);
    std::vector<double> finalPrimary(width * height, 0.0);
    for (size_t i = 0; i < finalPrimary.size(); ++i)
        finalPrimary[i] = (expandedPrimary[i] > 0.0 || secSourceBinary[i] > 0) ? 1.0 : 0.0;
    finalPrimary = uniformFilter(finalPrimary, width, height, std::max(1.0, band / 2.0));

    GeneratedMasks out;
    out.blendMask.swap(finalMask);
    out.primarySourceMask.swap(finalPrimary);
    out.angularDistanceDeg.swap(angularDistance);
    out.validMask.swap(validMask);
    return out;
}

std::vector<double> blendChannels(const std::vector<double> &h, const std::vector<double> &v,
        const std::vector<double> &mask, const std::vector<double> &primaryMask) {
    std::vector<double> out(h.size(), 0.0);
    for (size_t i = 0; i < out.size(); ++i) {
        const double base = v[i] * mask[i] + h[i] * (1.0 - mask[i]);
        const double nearValue = std::max(v[i], h[i]);
        out[i] = base * (1.0 - primaryMask[i]) + nearValue * primaryMask[i];
    }
    return out;
}

void inpaintMaskedRegion(std::vector<double> &channel, size_t width, size_t height, const std::vector<double> &sourceMask) {
    std::vector<double> prev = channel;
    std::vector<double> next = channel;
    for (int iter = 0; iter < 160; ++iter) {
        for (int y = 0; y < static_cast<int>(height); ++y) {
            for (int x = 0; x < static_cast<int>(width); ++x) {
                const size_t idx = static_cast<size_t>(y) * width + static_cast<size_t>(x);
                if (sourceMask[idx] <= 0.01)
                    continue;
                double sum = 0.0;
                double weight = 0.0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = y + dy;
                    if (ny < 0 || ny >= static_cast<int>(height))
                        continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx;
                        if (dx == 0 && dy == 0)
                            continue;
                        if (nx < 0 || nx >= static_cast<int>(width))
                            continue;
                        const size_t nidx = static_cast<size_t>(ny) * width + static_cast<size_t>(nx);
                        const double w = (dx == 0 || dy == 0) ? 1.0 : 0.70710678118;
                        sum += prev[nidx] * w;
                        weight += w;
                    }
                }
                if (weight > 0.0)
                    next[idx] = sum / weight;
            }
        }
        prev.swap(next);
    }
    channel.swap(prev);
}

void interpolatePrimarySourceRegion(std::vector<double> &r, std::vector<double> &g, std::vector<double> &b,
        const std::vector<PixelGeometry> &geometry,
        const std::vector<double> &primaryMask,
        const std::vector<uint8_t> &validMask,
        double band) {
    const double ringLow = std::max(0.0, (band - 4.0) / (2.0 * std::max(1.0, band)));
    struct Seed {
        Vec3d dir;
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
    };

    std::vector<Seed> seeds;
    std::vector<size_t> targets;
    seeds.reserve(2048);
    targets.reserve(65536);

    for (size_t i = 0; i < primaryMask.size(); ++i) {
        if (!validMask[i])
            continue;
        if (primaryMask[i] > 0.01)
            targets.push_back(i);
        if (primaryMask[i] > ringLow && primaryMask[i] < 0.5) {
            Seed seed;
            seed.dir = geometry[i].dir;
            seed.r = r[i];
            seed.g = g[i];
            seed.b = b[i];
            seeds.push_back(seed);
        }
    }

    if (seeds.empty() || targets.empty())
        return;

    const size_t neighborCount = std::min<size_t>(300, std::max<size_t>(1, static_cast<size_t>(seeds.size() * 0.2)));

    #pragma omp parallel for
    for (int ti = 0; ti < static_cast<int>(targets.size()); ++ti) {
        const size_t idx = targets[static_cast<size_t>(ti)];
        const Vec3d targetDir = geometry[idx].dir;
        const double mask = std::max(0.0, std::min(1.0, primaryMask[idx]));

        std::vector<std::pair<double, size_t>> nearest;
        nearest.reserve(seeds.size());
        for (size_t si = 0; si < seeds.size(); ++si) {
            const Seed &seed = seeds[si];
            const double dx = targetDir.x - seed.dir.x;
            const double dy = targetDir.y - seed.dir.y;
            const double dz = targetDir.z - seed.dir.z;
            const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            nearest.emplace_back(dist, si);
        }

        const size_t k = std::min(neighborCount, nearest.size());
        std::nth_element(
            nearest.begin(),
            nearest.begin() + static_cast<std::ptrdiff_t>(k),
            nearest.end(),
            [](const std::pair<double, size_t> &a, const std::pair<double, size_t> &b) {
                return a.first < b.first;
            }
        );

        double dvar = 0.0;
        for (size_t ni = 0; ni < k; ++ni)
            dvar += nearest[ni].first * nearest[ni].first;
        dvar /= static_cast<double>(k);
        if (!(dvar > 0.0))
            dvar = 1.0;

        double sumW = 0.0;
        double sumR = 0.0;
        double sumG = 0.0;
        double sumB = 0.0;
        for (size_t ni = 0; ni < k; ++ni) {
            const double dist = nearest[ni].first;
            const Seed &seed = seeds[nearest[ni].second];
            const double w = std::exp(-(dist * dist) / (2.0 * dvar));
            sumW += w;
            sumR += seed.r * w;
            sumG += seed.g * w;
            sumB += seed.b * w;
        }
        if (!(sumW > 0.0))
            continue;

        const double interpR = sumR / sumW;
        const double interpG = sumG / sumW;
        const double interpB = sumB / sumW;
        r[idx] = r[idx] * (1.0 - mask) + interpR * mask;
        g[idx] = g[idx] * (1.0 - mask) + interpG * mask;
        b[idx] = b[idx] * (1.0 - mask) + interpB * mask;
    }
}

struct LinregResult {
    double slope = 0.0;
    double intercept = 0.0;
    double corr = 0.0;
    bool valid = false;
};

LinregResult linearRegression(const std::vector<double> &x, const std::vector<double> &y) {
    LinregResult out;
    if (x.size() != y.size() || x.size() < 2)
        return out;
    const double n = static_cast<double>(x.size());
    const double sumX = std::accumulate(x.begin(), x.end(), 0.0);
    const double sumY = std::accumulate(y.begin(), y.end(), 0.0);
    const double meanX = sumX / n;
    const double meanY = sumY / n;
    double sxx = 0.0;
    double syy = 0.0;
    double sxy = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double dx = x[i] - meanX;
        const double dy = y[i] - meanY;
        sxx += dx * dx;
        syy += dy * dy;
        sxy += dx * dy;
    }
    if (!(sxx > 0.0) || !(syy > 0.0))
        return out;
    out.slope = sxy / sxx;
    out.intercept = meanY - out.slope * meanX;
    out.corr = sxy / std::sqrt(sxx * syy);
    out.valid = true;
    return out;
}

void applyCircumsolarExtrapolation(std::vector<double> &r, std::vector<double> &g, std::vector<double> &b,
        size_t width, size_t height, const std::vector<uint8_t> &validMask, const std::vector<double> &distanceDeg,
        double bw, std::vector<std::string> &headerNotes) {
    std::vector<double> d;
    std::vector<double> rv;
    std::vector<double> gv;
    std::vector<double> bv;
    d.reserve(width * height / 16);
    rv.reserve(d.capacity());
    gv.reserve(d.capacity());
    bv.reserve(d.capacity());

    for (size_t i = 0; i < distanceDeg.size(); ++i) {
        if (!validMask[i])
            continue;
        if (distanceDeg[i] > bw * 1.5 && distanceDeg[i] < bw * 2.0) {
            d.push_back(distanceDeg[i]);
            rv.push_back(r[i]);
            gv.push_back(g[i]);
            bv.push_back(b[i]);
        }
    }

    const LinregResult rr = linearRegression(d, rv);
    const LinregResult rg = linearRegression(d, gv);
    const LinregResult rb = linearRegression(d, bv);
    if (!rr.valid || !rg.valid || !rb.valid)
        return;
    if (std::min(rr.corr, std::min(rg.corr, rb.corr)) >= -0.25)
        return;

    const double dn = bw * 1.5;
    for (size_t i = 0; i < distanceDeg.size(); ++i) {
        if (!validMask[i] || distanceDeg[i] >= dn)
            continue;
        const double df = std::max(0.0, std::min(1.0, distanceDeg[i] / dn));
        r[i] = std::max(r[i], r[i] * df + rr.intercept * (1.0 - df));
        g[i] = std::max(g[i], g[i] * df + rg.intercept * (1.0 - df));
        b[i] = std::max(b[i], b[i] * df + rb.intercept * (1.0 - df));
    }

    std::ostringstream oss;
    oss << "EXTRAPOLATED_CIRCUMSOLAR= "
        << std::setprecision(5) << (rr.intercept - std::accumulate(r.begin(), r.end(), 0.0) / std::max<size_t>(1, r.size()))
        << "," << (rg.intercept - std::accumulate(g.begin(), g.end(), 0.0) / std::max<size_t>(1, g.size()))
        << "," << (rb.intercept - std::accumulate(b.begin(), b.end(), 0.0) / std::max<size_t>(1, b.size()))
        << " / " << std::setprecision(2) << (bw * 1.5) << " degrees";
    headerNotes.push_back(oss.str());
}

void phaseCorrelateShift(const std::vector<double> &im0, const std::vector<double> &im1,
        int width, int height, int &shiftX, int &shiftY) {
    const size_t planeSize = static_cast<size_t>(width) * static_cast<size_t>(height);
    double *a = static_cast<double *>(fftw_malloc(sizeof(double) * planeSize));
    double *b = static_cast<double *>(fftw_malloc(sizeof(double) * planeSize));
    fftw_complex *fa = static_cast<fftw_complex *>(fftw_malloc(sizeof(fftw_complex) * static_cast<size_t>(height) * (static_cast<size_t>(width) / 2 + 1)));
    fftw_complex *fb = static_cast<fftw_complex *>(fftw_malloc(sizeof(fftw_complex) * static_cast<size_t>(height) * (static_cast<size_t>(width) / 2 + 1)));
    double *corr = static_cast<double *>(fftw_malloc(sizeof(double) * planeSize));
    if (!a || !b || !fa || !fb || !corr)
        throw std::runtime_error("shadowband alignment could not allocate FFT buffers.");

    fftw_plan pa = fftw_plan_dft_r2c_2d(height, width, a, fa, FFTW_ESTIMATE);
    fftw_plan pb = fftw_plan_dft_r2c_2d(height, width, b, fb, FFTW_ESTIMATE);
    fftw_plan pc = fftw_plan_dft_c2r_2d(height, width, fa, corr, FFTW_ESTIMATE);

    for (size_t i = 0; i < planeSize; ++i) {
        a[i] = im0[i];
        b[i] = im1[i];
    }
    fftw_execute(pa);
    fftw_execute(pb);

    const size_t freqSize = static_cast<size_t>(height) * (static_cast<size_t>(width) / 2 + 1);
    for (size_t i = 0; i < freqSize; ++i) {
        const double ar = fa[i][0];
        const double ai = fa[i][1];
        const double br = fb[i][0];
        const double bi = fb[i][1];
        const double pr = ar * br + ai * bi;
        const double pi = ai * br - ar * bi;
        const double mag = std::sqrt(pr * pr + pi * pi);
        if (mag > 0.0) {
            fa[i][0] = pr / mag;
            fa[i][1] = pi / mag;
        } else {
            fa[i][0] = 0.0;
            fa[i][1] = 0.0;
        }
    }

    fftw_execute(pc);
    size_t bestIndex = 0;
    double bestValue = -1.0;
    for (size_t i = 0; i < planeSize; ++i) {
        const double v = std::abs(corr[i]);
        if (v > bestValue) {
            bestValue = v;
            bestIndex = i;
        }
    }

    shiftY = static_cast<int>(bestIndex / static_cast<size_t>(width));
    shiftX = static_cast<int>(bestIndex % static_cast<size_t>(width));
    if (shiftX > width / 2)
        shiftX -= width;
    if (shiftY > height / 2)
        shiftY -= height;

    fftw_destroy_plan(pa);
    fftw_destroy_plan(pb);
    fftw_destroy_plan(pc);
    fftw_free(a);
    fftw_free(b);
    fftw_free(fa);
    fftw_free(fb);
    fftw_free(corr);
}

std::pair<int, int> alignImages(const std::vector<double> &im0, const std::vector<double> &im1,
        int width, int height, bool bottom, bool logval) {
    int croppedHeight = height;
    int yOffset = 0;
    if (bottom) {
        croppedHeight = height / 2;
        yOffset = height - croppedHeight;
    }

    std::vector<double> a(static_cast<size_t>(width) * static_cast<size_t>(croppedHeight), 0.0);
    std::vector<double> b(static_cast<size_t>(width) * static_cast<size_t>(croppedHeight), 0.0);
    for (int y = 0; y < croppedHeight; ++y) {
        const double fy = (static_cast<double>(y) + 0.5) / (static_cast<double>(croppedHeight) * 0.5) - 1.0;
        for (int x = 0; x < width; ++x) {
            const double fx = (static_cast<double>(x) + 0.5) / (static_cast<double>(width) * 0.5) - 1.0;
            const double window = std::max(0.0, 1.0 - std::sqrt(fx * fx + fy * fy));
            const size_t srcIndex = static_cast<size_t>(y + yOffset) * static_cast<size_t>(width) + static_cast<size_t>(x);
            const size_t dstIndex = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
            const double v0 = logval ? std::log10(im0[srcIndex] + 1.0) : im0[srcIndex];
            const double v1 = logval ? std::log10(im1[srcIndex] + 1.0) : im1[srcIndex];
            a[dstIndex] = v0 * window;
            b[dstIndex] = v1 * window;
        }
    }

    int shiftX = 0;
    int shiftY = 0;
    phaseCorrelateShift(a, b, width, croppedHeight, shiftX, shiftY);
    return std::make_pair(shiftX, shiftY);
}

HdrImage applyAlignment(const HdrImage &image, int shiftX, int shiftY, int margin) {
    const int outWidth = static_cast<int>(image.width) - margin * 2;
    const int outHeight = static_cast<int>(image.height) - margin * 2;
    if (outWidth <= 0 || outHeight <= 0)
        throw std::runtime_error("shadowband margin removed the whole image.");

    HdrImage out;
    out.width = static_cast<size_t>(outWidth);
    out.height = static_cast<size_t>(outHeight);
    out.header = image.header;
    out.headerLines = image.headerLines;
    out.rgb.assign(out.width * out.height * 3, 0.0f);

    const int srcX0 = std::max(0, margin + shiftX);
    const int srcY0 = std::max(0, margin + shiftY);
    const int srcX1 = std::min(static_cast<int>(image.width), shiftX + static_cast<int>(image.width) - margin);
    const int srcY1 = std::min(static_cast<int>(image.height), shiftY + static_cast<int>(image.height) - margin);

    const int dstX0 = 0;
    const int dstY0 = 0;
    const int copyWidth = std::max(0, srcX1 - srcX0);
    const int copyHeight = std::max(0, srcY1 - srcY0);
    if (copyWidth == 0 || copyHeight == 0)
        return out;

    for (int y = 0; y < copyHeight && y < outHeight; ++y) {
        const size_t srcIndex = (static_cast<size_t>(srcY0 + y) * image.width + static_cast<size_t>(srcX0)) * 3;
        const size_t dstIndex = (static_cast<size_t>(dstY0 + y) * out.width + static_cast<size_t>(dstX0)) * 3;
        const size_t count = static_cast<size_t>(std::min(copyWidth, outWidth)) * 3;
        std::copy_n(image.rgb.begin() + static_cast<std::ptrdiff_t>(srcIndex),
                    static_cast<std::ptrdiff_t>(count),
                    out.rgb.begin() + static_cast<std::ptrdiff_t>(dstIndex));
    }
    return out;
}

HdrImage cropMargin(const HdrImage &image, int margin) {
    if (margin <= 0)
        return image;
    return cropTopDown(image, margin, margin, static_cast<int>(image.width) - margin * 2, static_cast<int>(image.height) - margin * 2);
}

bool parseSolarSourceDirectionFromHeader(const HdrImage &image, Vec3d &direction) {
    std::string combined;
    for (const std::string &line : image.headerLines) {
        if (line.rfind("SOLARSOURCE=", 0) != 0)
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq + 1 >= line.size())
            continue;
        if (!combined.empty())
            combined.push_back(' ');
        combined += line.substr(eq + 1);
    }
    if (combined.empty())
        return false;

    std::vector<double> numbers;
    const char *ptr = combined.c_str();
    while (*ptr != '\0') {
        char *end = nullptr;
        const double value = std::strtod(ptr, &end);
        if (end != ptr) {
            numbers.push_back(value);
            ptr = end;
            continue;
        }
        ++ptr;
    }
    if (numbers.size() < 4)
        return false;

    const size_t n = numbers.size();
    const double outX = numbers[n - 4];
    const double outY = numbers[n - 3];
    const double outZ = numbers[n - 2];
    direction = normalize(makeVec(-outX, -outY, outZ));
    return norm(direction) > 0.0;
}

Vec3d findSunDirection(const std::vector<PixelGeometry> &geometry, const HdrImage &ndImage,
        bool haveSunloc, int sunX, int sunY,
        bool haveSunDirection, const std::array<double, 3> &sunDirection,
        double sourceSolidAngle) {
    if (haveSunloc) {
        if (sunX < 0 || sunY < 0 || sunX >= static_cast<int>(ndImage.width) || sunY >= static_cast<int>(ndImage.height))
            throw std::runtime_error("shadowband --sunloc pixel lies outside the HDR.");
        const PixelGeometry g = geometry[static_cast<size_t>(sunY) * ndImage.width + static_cast<size_t>(sunX)];
        if (!g.valid)
            throw std::runtime_error("shadowband --sunloc pixel lies outside the fisheye view.");
        return g.dir;
    }

    if (haveSunDirection) {
        const Vec3d parsed = normalize(makeVec(sunDirection[0], sunDirection[1], sunDirection[2]));
        if (norm(parsed) > 0.0)
            return parsed;
    }

    Vec3d headerDir;
    if (parseSolarSourceDirectionFromHeader(ndImage, headerDir))
        return headerDir;

    struct PeakSample {
        Vec3d dir;
        double omega = 0.0;
        double lum = 0.0;
        size_t flatIndex = 0;
    };

    const int res = static_cast<int>(ndImage.width);
    const RaytoolsExactViewMapper baseVm(makeVec(0.0, 1.0, 0.0), 180.0);
    std::vector<PeakSample> filtered;
    filtered.reserve(static_cast<size_t>(res * res / 32));

    Vec3d sunlocDir = makeVec(0.0, 0.0, 0.0);
    const bool useSunlocCone = haveSunloc;
    if (useSunlocCone) {
        sunlocDir = baseVm.pixelRay(sunX, sunY, res);
    }

    size_t flatIndex = 0;
    for (int x = 0; x < res; ++x) {
        for (int y = 0; y < res; ++y, ++flatIndex) {
            const Vec3d ray = baseVm.pixelRay(x, y, res);
            const bool up = useSunlocCone
                ? (std::acos(clampDot(dot(sunlocDir, ray))) * 180.0 / kPi) < 3.0
                : ray.z > 0.0;
            if (!up)
                continue;

            const size_t idx = (ndImage.height - 1 - static_cast<size_t>(y)) * ndImage.width + static_cast<size_t>(x);
            const float *rgb = ndImage.rgb.data() + idx * 3;
            const double lum = radianceLuminance(rgb);
            if (!(lum > 6000.0))
                continue;

            filtered.push_back({
                ray,
                baseVm.pixelOmega({{static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5}}, res),
                lum,
                flatIndex
            });
        }
    }
    if (filtered.empty())
        throw std::runtime_error("shadowband could not find a direct-sun peak in the ND image. Use --sunloc x y.");

    std::vector<PeakSample> candidates;
    candidates.reserve(filtered.size());
    for (const PeakSample &sample : filtered) {
        if (sample.lum > (1e5 / 179.0))
            candidates.push_back(sample);
    }
    if (candidates.empty())
        throw std::runtime_error("shadowband could not find a direct-sun peak in the ND image. Use --sunloc x y.");

    std::sort(candidates.begin(), candidates.end(), [](const PeakSample &a, const PeakSample &b) {
        if (a.lum != b.lum)
            return a.lum > b.lum;
        return a.flatIndex < b.flatIndex;
    });

    const double peaka = sourceSolidAngle;
    const double peakrad = 4.0;
    const double blurtol = 0.75;
    const double peakr = 4.0;
    const double cosrad = std::cos(std::sqrt(peaka / kPi) * peakrad);

    std::vector<PeakSample> pvol;
    pvol.reserve(candidates.size());
    const Vec3d peakDir = candidates.front().dir;
    for (const PeakSample &sample : candidates) {
        if (dot(peakDir, sample.dir) > cosrad)
            pvol.push_back(sample);
    }
    if (pvol.empty())
        return peakDir;

    const double peakLum = pvol.front().lum;
    std::vector<const PeakSample *> nearPeak;
    nearPeak.reserve(pvol.size());
    for (const PeakSample &sample : pvol) {
        if (peakLum * blurtol <= sample.lum)
            nearPeak.push_back(&sample);
    }

    double avgNearLum = 0.0;
    for (const PeakSample *sample : nearPeak)
        avgNearLum += sample->lum;
    avgNearLum /= std::max<size_t>(1, nearPeak.size());
    const double esun = avgNearLum * peaka;

    double cumulativeEnergy = 0.0;
    std::vector<double> cume;
    cume.reserve(pvol.size());
    for (const PeakSample &sample : pvol) {
        cumulativeEnergy += sample.omega * sample.lum;
        cume.push_back(cumulativeEnergy);
    }

    size_t stop = pvol.size();
    if (!cume.empty() && cume.back() > esun) {
        stop = 0;
        while (stop < cume.size() && !(cume[stop] > esun))
            ++stop;
        if (stop == 0)
            stop = cume.size();
    } else {
        stop = 0;
        while (stop < pvol.size() && !(pvol[stop].lum < (pvol.front().lum / peakr)))
            ++stop;
        if (stop == 0)
            stop = cume.size();
    }
    stop = std::max<size_t>(1, std::min(stop, pvol.size()));

    Vec3d weighted = makeVec(0.0, 0.0, 0.0);
    double weightSum = 0.0;
    for (size_t i = 0; i < stop; ++i) {
        const double w = pvol[i].omega * pvol[i].lum;
        weighted = weighted + pvol[i].dir * w;
        weightSum += w;
    }
    if (!(weightSum > 0.0))
        return peakDir;
    return normalize(weighted);
}

std::pair<int, int> directionToPixel(const Vec3d &dir, size_t width, size_t height, bool outputOrientation) {
    Vec3d useDir = dir;
    if (outputOrientation)
        useDir = makeVec(-dir.x, -dir.y, dir.z);

    const Vec3d forward = outputOrientation ? makeVec(0.0, -1.0, 0.0) : makeVec(0.0, 1.0, 0.0);
    const Vec3d right = makeVec(1.0, 0.0, 0.0);
    const Vec3d up = makeVec(0.0, 0.0, 1.0);

    const double theta = std::acos(clampDot(dot(useDir, forward)));
    if (theta > 0.5 * kPi)
        return std::make_pair(-1, -1);

    const Vec3d tangent = normalize(useDir - forward * dot(useDir, forward));
    const double uComp = dot(tangent, right);
    const double vComp = dot(tangent, up);
    const double radius = theta / kPi;
    const double nx = radius * uComp;
    const double ny = radius * vComp;
    const double fx = (nx + 0.5) * static_cast<double>(width) - 0.5;
    const double fy = (0.5 - ny) * static_cast<double>(height) - 0.5;
    return std::make_pair(static_cast<int>(std::floor(fx + 0.5)), static_cast<int>(std::floor(fy + 0.5)));
}

std::array<double, 3> integrateSourceRgb(const HdrImage &image, const std::vector<PixelGeometry> &geometry,
        const Vec3d &sunDir, double sfov, double solidAngle) {
    std::array<double, 3> sum{{0.0, 0.0, 0.0}};
    const double maxAngle = sfov * kPi / 360.0;
    for (size_t i = 0; i < geometry.size(); ++i) {
        if (!geometry[i].valid)
            continue;
        if (std::acos(clampDot(dot(geometry[i].dir, sunDir))) > maxAngle)
            continue;
        sum[0] += image.rgb[i * 3 + 0] * geometry[i].omega;
        sum[1] += image.rgb[i * 3 + 1] * geometry[i].omega;
        sum[2] += image.rgb[i * 3 + 2] * geometry[i].omega;
    }
    std::array<double, 3> out{{0.0, 0.0, 0.0}};
    if (solidAngle > 0.0) {
        out[0] = sum[0] / solidAngle;
        out[1] = sum[1] / solidAngle;
        out[2] = sum[2] / solidAngle;
    }
    return out;
}

std::array<double, 3> integrateSourceRgbRaytoolsExact(const HdrImage &image, const Vec3d &sunDir,
        double sfov, double solidAngle) {
    const int res = static_cast<int>(image.width);
    const RaytoolsExactViewMapper baseVm(makeVec(0.0, 1.0, 0.0), 180.0);
    const RaytoolsExactViewMapper sourceVm(sunDir, sfov);
    std::array<double, 3> sum{{0.0, 0.0, 0.0}};
    for (int y = 0; y < res; ++y) {
        for (int x = 0; x < res; ++x) {
            const Vec3d ray = baseVm.pixelRay(x, y, res);
            if (!sourceVm.inView(ray))
                continue;
            const double omega = baseVm.pixelOmega({{static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5}}, res);
            // raytools hdr2carray() converts Radiance scanlines to (3, x, y) by
            // applying both a transpose and a vertical flip:
            //     np.transpose(bytes[:, ::-1], (0, 2, 1))
            // Preserve that exact pairing here so pixelrays(res).reshape(-1, 3)
            // and image.reshape(3, -1) see the same pixels as pylinearhdr.
            const size_t idx = (image.height - 1 - static_cast<size_t>(y)) * image.width + static_cast<size_t>(x);
            sum[0] += image.rgb[idx * 3 + 0] * omega;
            sum[1] += image.rgb[idx * 3 + 1] * omega;
            sum[2] += image.rgb[idx * 3 + 2] * omega;
        }
    }
    std::array<double, 3> out{{0.0, 0.0, 0.0}};
    if (solidAngle > 0.0) {
        out[0] = sum[0] / solidAngle;
        out[1] = sum[1] / solidAngle;
        out[2] = sum[2] / solidAngle;
    }
    return out;
}

std::vector<double> gaussianBlurReflect(const std::vector<double> &image, size_t width, size_t height, double sigma) {
    if (!(sigma > 0.0))
        return image;

    const int radius = std::max(1, static_cast<int>(std::ceil(8.0 * sigma)));
    std::vector<double> kernel(static_cast<size_t>(radius * 2 + 1), 0.0);
    double sum = 0.0;
    for (int k = -radius; k <= radius; ++k) {
        const double w = std::exp(-0.5 * (static_cast<double>(k) * static_cast<double>(k)) / (sigma * sigma));
        kernel[static_cast<size_t>(k + radius)] = w;
        sum += w;
    }
    for (double &w : kernel)
        w /= sum;

    std::vector<double> temp(width * height, 0.0);
    std::vector<double> out(width * height, 0.0);

    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(height); ++y) {
        for (int x = 0; x < static_cast<int>(width); ++x) {
            double acc = 0.0;
            for (int k = -radius; k <= radius; ++k) {
                const int sx = reflectIndex(x + k, static_cast<int>(width));
                acc += image[static_cast<size_t>(y) * width + static_cast<size_t>(sx)] * kernel[static_cast<size_t>(k + radius)];
            }
            temp[static_cast<size_t>(y) * width + static_cast<size_t>(x)] = acc;
        }
    }

    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(height); ++y) {
        for (int x = 0; x < static_cast<int>(width); ++x) {
            double acc = 0.0;
            for (int k = -radius; k <= radius; ++k) {
                const int sy = reflectIndex(y + k, static_cast<int>(height));
                acc += temp[static_cast<size_t>(sy) * width + static_cast<size_t>(x)] * kernel[static_cast<size_t>(k + radius)];
            }
            out[static_cast<size_t>(y) * width + static_cast<size_t>(x)] = acc;
        }
    }
    return out;
}

std::vector<double> rasterizeSourceLuminance(const std::vector<PixelGeometry> &geometry, size_t width, size_t height,
        const Vec3d &sunDir, double sourceLuminance, double solidAngle) {
    std::vector<double> source(width * height, 0.0);
    if (!(solidAngle > 0.0) || !(sourceLuminance > 0.0))
        return source;

    const int rasterRes = 64;
    const double omegasp = solidAngle / static_cast<double>(rasterRes * rasterRes);
    const double sourceRadius = std::sqrt(solidAngle / kPi);
    const double viewAngleRadians = sourceRadius * 2.0;

    Vec3d worldUp = std::abs(sunDir.z) < 0.999 ? makeVec(0.0, 0.0, 1.0) : makeVec(1.0, 0.0, 0.0);
    Vec3d right = normalize(cross(worldUp, sunDir));
    Vec3d up = normalize(cross(sunDir, right));

    std::vector<int> hitCount(width * height, 0);
    for (int py = 0; py < rasterRes; ++py) {
        for (int px = 0; px < rasterRes; ++px) {
            const double nx = (static_cast<double>(px) + 0.5) / static_cast<double>(rasterRes) - 0.5;
            const double ny = 0.5 - (static_cast<double>(py) + 0.5) / static_cast<double>(rasterRes);
            const double u = nx * viewAngleRadians;
            const double v = ny * viewAngleRadians;
            const double theta = std::sqrt(u * u + v * v);
            Vec3d dir = sunDir;
            if (theta > 1e-12) {
                const double sinTheta = std::sin(theta);
                const double scale = sinTheta / theta;
                dir = normalize(sunDir * std::cos(theta) + right * (u * scale) + up * (v * scale));
            }
            const std::pair<int, int> pix = directionToPixel(dir, width, height, false);
            if (pix.first < 0 || pix.second < 0 || pix.first >= static_cast<int>(width) || pix.second >= static_cast<int>(height))
                continue;
            hitCount[static_cast<size_t>(pix.second) * width + static_cast<size_t>(pix.first)] += 1;
        }
    }

    for (size_t i = 0; i < hitCount.size(); ++i) {
        if (hitCount[i] <= 0 || !geometry[i].valid || !(geometry[i].omega > 0.0))
            continue;
        source[i] = sourceLuminance * static_cast<double>(hitCount[i]) * omegasp / geometry[i].omega;
    }

    const double sigma = static_cast<double>(width) / 180.0 * 0.0625;
    return gaussianBlurReflect(source, width, height, sigma);
}

void addSourceToBlend(HdrImage &blend, const std::vector<PixelGeometry> &geometry, const Vec3d &sunDir,
        const std::array<double, 3> &sourceRgb, double solidAngle,
        std::vector<double> *rawLumMap = nullptr, std::vector<double> *blurredLumMap = nullptr,
        std::vector<double> *countMapOut = nullptr, std::vector<double> *omegaMapOut = nullptr) {
    const double sourceLum =
        sourceRgb[0] * kRadianceLumWeights[0] +
        sourceRgb[1] * kRadianceLumWeights[1] +
        sourceRgb[2] * kRadianceLumWeights[2];
    if (!(sourceLum > 0.0))
        return;

    const double sourceRadius = std::sqrt(solidAngle / kPi);
    const double sourceViewAngle = sourceRadius * 360.0 / kPi;
    const AngularViewMapper sourceVm(sunDir, sourceViewAngle);
    const AngularViewMapper viewVm(makeVec(0.0, 1.0, 0.0), 180.0);
    if (viewVm.degrees(sunDir) > viewVm.viewAngle * 0.5)
        return;

    const int rasterRes = 64;
    const int imageRes = static_cast<int>(blend.width);
    const double omegasp = solidAngle / static_cast<double>(rasterRes * rasterRes);
    std::vector<double> lumMap(blend.width * blend.height, 0.0);
    std::vector<int> counts(blend.width * blend.height, 0);

    for (int sx = 0; sx < rasterRes; ++sx) {
        for (int sy = 0; sy < rasterRes; ++sy) {
            const Vec3d ray = sourceVm.uvRay(sx, sy, rasterRes);
            const std::array<double, 2> p = viewVm.ray2pixel(ray, imageRes, true);
            const int ix = static_cast<int>(p[0]);
            const int iy = static_cast<int>(p[1]);
            if (ix < 0 || iy < 0 || ix >= static_cast<int>(blend.width) || iy >= static_cast<int>(blend.height))
                continue;
            counts[static_cast<size_t>(iy) * blend.width + static_cast<size_t>(ix)] += 1;
        }
    }

    double rawEnergy = 0.0;
    for (int iy = 0; iy < static_cast<int>(blend.height); ++iy) {
        for (int ix = 0; ix < static_cast<int>(blend.width); ++ix) {
            const size_t idx = static_cast<size_t>(iy) * blend.width + static_cast<size_t>(ix);
            if (counts[idx] <= 0 || !geometry[idx].valid)
                continue;
            const double omega = viewVm.pixelOmega(
                {{static_cast<double>(ix) + 0.5, static_cast<double>(iy) + 0.5}}, imageRes);
            if (!(omega > 0.0))
                continue;
            lumMap[idx] = sourceLum * static_cast<double>(counts[idx]) * omegasp / omega;
            rawEnergy += lumMap[idx] * omega;
        }
    }

    if (countMapOut) {
        countMapOut->assign(counts.begin(), counts.end());
    }
    if (omegaMapOut) {
        omegaMapOut->assign(blend.width * blend.height, 0.0);
        for (int iy = 0; iy < static_cast<int>(blend.height); ++iy) {
            for (int ix = 0; ix < static_cast<int>(blend.width); ++ix) {
                const size_t idx = static_cast<size_t>(iy) * blend.width + static_cast<size_t>(ix);
                if (counts[idx] <= 0)
                    continue;
                (*omegaMapOut)[idx] = viewVm.pixelOmega(
                    {{static_cast<double>(ix) + 0.5, static_cast<double>(iy) + 0.5}}, imageRes);
            }
        }
    }

    if (rawLumMap)
        *rawLumMap = lumMap;

    const double sigma = static_cast<double>(imageRes) / viewVm.viewAngle * 0.0625;
    lumMap = gaussianBlurReflect(lumMap, blend.width, blend.height, sigma);

    // A normalized image-space Gaussian preserves the sum of pixel values, not
    // the solid-angle integral of an equidistant (-vta) image.  Renormalize the
    // blurred raster with each output pixel's actual omega so the source energy
    // remains exactly what was deposited by the subpixel source rasterizer.
    double blurredEnergy = 0.0;
    for (size_t i = 0; i < lumMap.size(); ++i) {
        if (!(lumMap[i] > 0.0) || !geometry[i].valid)
            continue;
        const int ix = static_cast<int>(i % blend.width);
        const int iy = static_cast<int>(i / blend.width);
        const double omega = viewVm.pixelOmega(
            {{static_cast<double>(ix) + 0.5, static_cast<double>(iy) + 0.5}}, imageRes);
        blurredEnergy += lumMap[i] * omega;
    }
    if (rawEnergy > 0.0 && blurredEnergy > 0.0) {
        const double energyScale = rawEnergy / blurredEnergy;
        for (double &value : lumMap)
            value *= energyScale;
    }

    if (blurredLumMap)
        *blurredLumMap = lumMap;

    const double colorR = static_cast<double>(sourceRgb[0]) / sourceLum;
    const double colorG = static_cast<double>(sourceRgb[1]) / sourceLum;
    const double colorB = static_cast<double>(sourceRgb[2]) / sourceLum;

    for (size_t i = 0; i < lumMap.size(); ++i) {
        const float srcR = static_cast<float>(lumMap[i] * colorR);
        const float srcG = static_cast<float>(lumMap[i] * colorG);
        const float srcB = static_cast<float>(lumMap[i] * colorB);
        blend.rgb[i * 3 + 0] += std::max(srcR - blend.rgb[i * 3 + 0], 0.0f);
        blend.rgb[i * 3 + 1] += std::max(srcG - blend.rgb[i * 3 + 1], 0.0f);
        blend.rgb[i * 3 + 2] += std::max(srcB - blend.rgb[i * 3 + 2], 0.0f);
    }
}

std::vector<std::string> checkHeaderLines(const std::string &prefix, const std::string &name) {
    std::vector<std::string> lines;
    lines.push_back("MERGEHDR_STEP_1= shadowband debug " + name);
    return lines;
}

} // namespace

ShadowbandMergeResult mergeShadowbandNative(
    const HdrImage &horizontalIn,
    const HdrImage &verticalIn,
    const HdrImage &noshadowIn,
    const ShadowbandMergeOptions &opts
) {
    if (horizontalIn.width == 0 || horizontalIn.height == 0)
        throw std::runtime_error("shadowband requires non-empty HDR inputs.");
    if (horizontalIn.width != verticalIn.width || horizontalIn.height != verticalIn.height ||
            horizontalIn.width != noshadowIn.width || horizontalIn.height != noshadowIn.height)
        throw std::runtime_error("shadowband requires H/V/ND HDR images to have identical resolution.");
    if (horizontalIn.width != horizontalIn.height)
        throw std::runtime_error("shadowband currently requires square fisheye HDR inputs.");

    HdrImage h = horizontalIn;
    HdrImage v = verticalIn;
    HdrImage s = noshadowIn;
    ShadowbandMergeResult result;

    if (opts.align) {
        const int margin = std::max(0, opts.margin);
        std::vector<double> hRef = extractChannel(h, 0);
        std::vector<double> vRef = extractChannel(v, 0);
        if (margin > 0) {
            const HdrImage hCrop = cropMargin(h, margin);
            const HdrImage vCrop = cropMargin(v, margin);
            hRef = extractChannel(hCrop, 0);
            vRef = extractChannel(vCrop, 0);
        }
        const int refWidth = static_cast<int>(h.width) - margin * 2;
        const int refHeight = static_cast<int>(h.height) - margin * 2;
        if (refWidth <= 0 || refHeight <= 0)
            throw std::runtime_error("shadowband margin is too large for the input resolution.");
        std::pair<int, int> shift = alignImages(hRef, vRef, refWidth, refHeight, true, true);
        if (shift.first != 0 || shift.second != 0) {
            h = applyAlignment(h, shift.first, shift.second, margin);
            result.headerNotes.push_back("SHADOWBAND_IMAGE_ALIGN= " + std::to_string(shift.first) + " " + std::to_string(shift.second));
        } else {
            h = cropMargin(h, margin);
        }
        v = cropMargin(v, margin);
        s = cropMargin(s, margin);
    }

    if (opts.fisheye) {
        h = projectEquisolidToEquidistantTopDown(h);
        v = projectEquisolidToEquidistantTopDown(v);
        s = projectEquisolidToEquidistantTopDown(s);
    }

    const std::vector<PixelGeometry> geometry = buildGeometry(h.width, h.height);
    const Vec3d sunDir = findSunDirection(
        geometry,
        s,
        opts.haveSunloc,
        opts.sunPixelX,
        opts.sunPixelY,
        opts.haveSunDirection,
        opts.sunDirection,
        opts.srcsize
    );
    const GeneratedMasks masks = generateMasks(geometry, h.width, h.height, sunDir, opts.roh, opts.rov, opts.bw);

    std::vector<double> hr = extractChannel(h, 0);
    std::vector<double> hg = extractChannel(h, 1);
    std::vector<double> hb = extractChannel(h, 2);
    std::vector<double> vr = extractChannel(v, 0);
    std::vector<double> vg = extractChannel(v, 1);
    std::vector<double> vb = extractChannel(v, 2);

    std::vector<double> br = blendChannels(hr, vr, masks.blendMask, masks.primarySourceMask);
    std::vector<double> bg = blendChannels(hg, vg, masks.blendMask, masks.primarySourceMask);
    std::vector<double> bb = blendChannels(hb, vb, masks.blendMask, masks.primarySourceMask);

    const double band = opts.bw / 180.0 * static_cast<double>(h.width);
    interpolatePrimarySourceRegion(br, bg, bb, geometry, masks.primarySourceMask, masks.validMask, band);
    applyCircumsolarExtrapolation(br, bg, bb, h.width, h.height, masks.validMask, masks.angularDistanceDeg, opts.bw, result.headerNotes);

    HdrImage blended = h;
    blended.rgb.assign(h.width * h.height * 3, 0.0f);
    for (size_t i = 0; i < br.size(); ++i) {
        blended.rgb[i * 3 + 0] = static_cast<float>(br[i]);
        blended.rgb[i * 3 + 1] = static_cast<float>(bg[i]);
        blended.rgb[i * 3 + 2] = static_cast<float>(bb[i]);
    }

    if (!opts.envmapPath.empty()) {
        result.skyOnly = blended;
        result.hasSkyOnly = true;
    }

    std::array<double, 3> solLumRgb = integrateSourceRgbRaytoolsExact(s, sunDir, opts.sfov, opts.srcsize);
    const std::array<double, 3> skyLumRgb = integrateSourceRgbRaytoolsExact(blended, sunDir, opts.sfov, opts.srcsize);
    const double solLum = solLumRgb[0] * kRadianceLumWeights[0] + solLumRgb[1] * kRadianceLumWeights[1] + solLumRgb[2] * kRadianceLumWeights[2];
    const double skyLum = skyLumRgb[0] * kRadianceLumWeights[0] + skyLumRgb[1] * kRadianceLumWeights[1] + skyLumRgb[2] * kRadianceLumWeights[2];
    if (solLum > 0.0) {
        const double cf = std::max(0.0, (solLum - skyLum) / solLum);
        result.sourceRgb[0] = solLumRgb[0] * cf;
        result.sourceRgb[1] = solLumRgb[1] * cf;
        result.sourceRgb[2] = solLumRgb[2] * cf;
        result.hasSource = true;
        result.sourceSolidAngle = opts.srcsize;
        addSourceToBlend(blended, geometry, sunDir, result.sourceRgb, result.sourceSolidAngle);
        result.outputSourceDirection = {{-sunDir.x, -sunDir.y, sunDir.z}};
        const RaytoolsExactViewMapper outputVm(makeVec(0.0, -1.0, 0.0), 180.0);
        const Vec3d outDir = makeVec(result.outputSourceDirection[0], result.outputSourceDirection[1], result.outputSourceDirection[2]);
        const std::array<double, 2> sourcePixel = outputVm.ray2pixel(outDir, static_cast<int>(h.width), false);
        result.sourcePixelX = sourcePixel[0];
        result.sourcePixelY = sourcePixel[1];
        std::ostringstream cp;
        cp << std::setprecision(15)
           << "CENTRAL_SOURCE_PIXEL= " << result.sourcePixelX << " " << result.sourcePixelY;
        result.headerNotes.push_back(cp.str());
    }

    result.blended = blended;

    if (!opts.checkPrefix.empty()) {
        writeRadianceHDR(opts.checkPrefix + "_mask.hdr", makeGrayHdr(h.width, h.height, masks.blendMask), checkHeaderLines(opts.checkPrefix, "mask"), false, true);
        writeRadianceHDR(opts.checkPrefix + "_srcmask.hdr", makeGrayHdr(h.width, h.height, masks.primarySourceMask), checkHeaderLines(opts.checkPrefix, "srcmask"), false, true);
        writeRadianceHDR(opts.checkPrefix + "_H.hdr", h, checkHeaderLines(opts.checkPrefix, "H"), false, true);
        writeRadianceHDR(opts.checkPrefix + "_V.hdr", v, checkHeaderLines(opts.checkPrefix, "V"), false, true);
        writeRadianceHDR(opts.checkPrefix + "_ND.hdr", s, checkHeaderLines(opts.checkPrefix, "ND"), false, true);
        if (result.hasSource) {
            HdrImage src = blended;
            src.rgb.assign(src.width * src.height * 3, 0.0f);
            std::vector<double> srcRaw;
            std::vector<double> srcBlurred;
            std::vector<double> srcCounts;
            std::vector<double> srcOmega;
            addSourceToBlend(src, geometry, sunDir, result.sourceRgb, result.sourceSolidAngle,
                &srcRaw, &srcBlurred, &srcCounts, &srcOmega);
            writeRadianceHDR(opts.checkPrefix + "_src.hdr", src, checkHeaderLines(opts.checkPrefix, "src"), false, true);
            writeRadianceHDR(opts.checkPrefix + "_src_raw.hdr", makeGrayHdr(src.width, src.height, srcRaw), checkHeaderLines(opts.checkPrefix, "src_raw"), false, true);
            writeRadianceHDR(opts.checkPrefix + "_src_blur.hdr", makeGrayHdr(src.width, src.height, srcBlurred), checkHeaderLines(opts.checkPrefix, "src_blur"), false, true);
            writeRadianceHDR(opts.checkPrefix + "_src_count.hdr", makeGrayHdr(src.width, src.height, srcCounts), checkHeaderLines(opts.checkPrefix, "src_count"), false, true);
            writeRadianceHDR(opts.checkPrefix + "_src_omega.hdr", makeGrayHdr(src.width, src.height, srcOmega), checkHeaderLines(opts.checkPrefix, "src_omega"), false, true);
        }
    }

    return result;
}
