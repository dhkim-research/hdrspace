#ifndef HDRMERGE_MINI_PFS_H
#define HDRMERGE_MINI_PFS_H

#include <cstddef>

namespace pfs {

class Array2D {
public:
    virtual ~Array2D() = default;

    virtual std::size_t getRows() const = 0;
    virtual std::size_t getCols() const = 0;

    virtual float &operator()(std::size_t index) = 0;
    virtual const float &operator()(std::size_t index) const = 0;

    virtual float &operator()(std::size_t col, std::size_t row) = 0;
    virtual const float &operator()(std::size_t col, std::size_t row) const = 0;
};

}

#endif
