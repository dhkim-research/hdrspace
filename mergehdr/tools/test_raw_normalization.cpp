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
    std::cout << "PASS: mixed ISO/bit depth, saturation, custom black, uniform bracket, invalid range\n";
}
