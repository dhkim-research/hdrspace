#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>

// LibRaw has already subtracted each frame's channel/pattern black correction.
// The stored Bayer value has its frame's scalar black baseline added back.
// Move BOTH black and full-scale white to one common encoding before merging;
// moving only black makes a clipped high-black/low-bit-depth frame look valid.
inline uint16_t normalizeRawSample(uint16_t value, int frameBlack, int frameWhite,
        int commonBlack, int commonWhite) {
    if (frameBlack < 0 || frameWhite <= frameBlack || frameWhite > 65535 ||
            commonBlack < 0 || commonWhite <= commonBlack || commonWhite > 65535)
        throw std::runtime_error("Invalid per-frame RAW black/white normalization range.");
    const int signal = std::max(0, static_cast<int>(value) - frameBlack);
    const int sourceRange = frameWhite - frameBlack;
    const int targetRange = commonWhite - commonBlack;
    if (signal >= sourceRange)
        return static_cast<uint16_t>(commonWhite);
    const uint64_t numerator = static_cast<uint64_t>(signal) * targetRange;
    const int scaled = static_cast<int>((numerator + sourceRange / 2) / sourceRange);
    return static_cast<uint16_t>(commonBlack + scaled);
}
