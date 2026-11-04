#pragma once

#include <cstdint>

namespace qm {

// RGBA_8888 in memory is R,G,B,A bytes, i.e. little-endian 0xAABBGGRR.
constexpr uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return 0xFF000000u | (b << 16) | (g << 8) | r; }

// Clipped drawing into an RGBA_8888 buffer with a built-in 5x7 bitmap font (upper case,
// digits and common punctuation; lower case is drawn as upper case).
struct Canvas {
    uint32_t* px;
    int32_t stride;  // in pixels
    int32_t w;
    int32_t h;

    void fill(int32_t x, int32_t y, int32_t fw, int32_t fh, uint32_t c) const;

    // Each glyph cell is 6x8 units (5x7 glyph + 1 unit spacing); a unit is `scale` pixels.
    static int32_t textWidth(const char* s, int32_t scale);
    void text(int32_t x, int32_t y, int32_t scale, const char* s, uint32_t fg) const;
};

}  // namespace qm
