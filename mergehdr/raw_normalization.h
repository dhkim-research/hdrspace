#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

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

// Checks for fixed (profile) RAW levels. LibRaw's own per-frame levels are only compared,
// never used, so the merge stays reproducible; a difference is reported, not corrected.
// tolerance: counts by which LibRaw's black may jitter (masked-area estimate) before a warning.
inline std::vector<std::string> rawLevelWarnings(const std::vector<std::string> &names,
        const std::vector<int> &decoderBlack, const std::vector<int> &decoderWhite,
        int fixedBlack, int tolerance = 4) {
    if (names.size() != decoderBlack.size() || names.size() != decoderWhite.size())
        throw std::runtime_error("rawLevelWarnings: inconsistent frame lists.");
    std::vector<std::string> warnings;
    if (names.empty())
        return warnings;
    if (fixedBlack >= 0) {
        for (size_t i = 0; i < names.size(); ++i) {
            const int d = decoderBlack[i] - fixedBlack;
            if (d > tolerance || d < -tolerance)
                warnings.push_back(names[i] + ": LibRaw black " + std::to_string(decoderBlack[i]) +
                    " differs from the fixed black " + std::to_string(fixedBlack) + " by " + std::to_string(d) + " counts");
        }
    }
    const auto b = std::minmax_element(decoderBlack.begin(), decoderBlack.end());
    if (*b.second - *b.first > tolerance)
        warnings.push_back("LibRaw black differs between frames (" + std::to_string(*b.first) + " to " +
            std::to_string(*b.second) + "): mixed ISO or bit depth? One fixed black may not fit every frame");
    const auto w = std::minmax_element(decoderWhite.begin(), decoderWhite.end());
    if (*w.second != *w.first)
        warnings.push_back("LibRaw white differs between frames (" + std::to_string(*w.first) + " to " +
            std::to_string(*w.second) + "): mixed ISO or bit depth? One fixed white may not fit every frame");
    return warnings;
}
