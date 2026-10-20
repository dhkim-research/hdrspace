#include "../raw_normalization.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    // Same normalized light signal, different per-frame black/full-scale values.
    assert(normalizeRawSample(550, 100, 1000, 4095, 65535) ==
           normalizeRawSample(700, 400, 1000, 4095, 65535));
    // A clipped high-black ISO400 exposure must remain at full-scale white.
    const uint16_t clipped = normalizeRawSample(65535, 4096, 65535, 4095, 65535);
    assert(clipped == 65535);
    assert((clipped - 4095.0) / (65535.0 - 4095.0) >= 0.99);
    // Changing the profile black baseline must preserve zero and saturation.
    assert(normalizeRawSample(255, 255, 65535, 4095, 65535) == 4095);
    assert(normalizeRawSample(255, 255, 65535, 0, 65535) == 0);
    assert(normalizeRawSample(100, 255, 65535, 4095, 65535) == 4095);
    // 14- and 16-bit encodings represent equal fractions on the merge scale.
    assert(normalizeRawSample(7967, 62, 15872, 4095, 65535) == 34815);
    assert(normalizeRawSample(32895, 255, 65535, 4095, 65535) == 34815);
    // Existing uniform brackets incur no changes, including at black and white.
    for (int p = 255; p <= 65535; ++p)
        assert(normalizeRawSample(static_cast<uint16_t>(p), 255, 65535, 255, 65535) == p);
    bool rejected = false;
    try { normalizeRawSample(0, 5, 5, 0, 65535); }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    // Fixed profile levels (linearhdr rawconvert -k 2048 -S 11000): identity up to the white, clip above it.
    for (int p = 2048; p <= 13048; ++p)
        assert(normalizeRawSample(static_cast<uint16_t>(p), 2048, 13048, 2048, 13048) == p);
    assert(normalizeRawSample(15303, 2048, 13048, 2048, 13048) == 13048);
    assert(normalizeRawSample(2000, 2048, 13048, 2048, 13048) == 2048);
    // Level checks: LibRaw jitter of 1-2 counts is silent; a 12-bit frame or another white is reported.
    {
        const std::vector<std::string> n = { "a", "b", "c" };
        assert(rawLevelWarnings(n, { 2049, 2048, 2050 }, { 15303, 15303, 15303 }, 2048).empty());
        assert(rawLevelWarnings(n, { 2049, 2048, 2050 }, { 15303, 15303, 15303 }, -1).empty());
        const auto mixedBlack = rawLevelWarnings(n, { 2048, 2048, 512 }, { 15303, 15303, 15303 }, 2048);
        assert(mixedBlack.size() == 2);            // frame c vs fixed black, and spread between frames
        assert(mixedBlack[0].find("c: LibRaw black 512") == 0);
        const auto mixedWhite = rawLevelWarnings(n, { 2048, 2048, 2048 }, { 15303, 12000, 15303 }, 2048);
        assert(mixedWhite.size() == 1 && mixedWhite[0].find("LibRaw white differs") == 0);
        assert(rawLevelWarnings(n, { 2053, 2053, 2053 }, { 15303, 15303, 15303 }, 2048).size() == 3);
        assert(rawLevelWarnings({}, {}, {}, 2048).empty());
        bool bad = false;
        try { rawLevelWarnings(n, { 1 }, { 1, 2, 3 }, 0); } catch (const std::runtime_error &) { bad = true; }
        assert(bad);
    }
    std::cout << "PASS: mixed ISO/bit depth, saturation, custom black, uniform bracket, invalid range, "
                 "fixed profile levels, level warnings\n";
}
