// CPU BGRA -> NV12 (BT.709 limited range) with nearest-neighbour scaling. Used as the fallback when the
// GPU video processor keeps failing mid-recording. Platform-independent so it can be unit-tested.
#pragma once
#include <cstdint>
#include <cstddef>

namespace cr {

inline void bgraToNv12(const uint8_t* src, unsigned sw, unsigned sh, size_t srcPitch, uint8_t* dst, unsigned W, unsigned H) {
    uint8_t* yP = dst;
    uint8_t* uv = dst + (size_t)W * H;
    auto px = [&](unsigned x, unsigned y) -> const uint8_t* {
        unsigned sx = (unsigned)((uint64_t)x * sw / W), sy = (unsigned)((uint64_t)y * sh / H);
        if (sx >= sw) sx = sw - 1;
        if (sy >= sh) sy = sh - 1;
        return src + (size_t)sy * srcPitch + (size_t)sx * 4;
    };
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            const uint8_t* p = px(x, y);
            int b = p[0], g = p[1], r = p[2];
            yP[(size_t)y * W + x] = (uint8_t)((47 * r + 157 * g + 16 * b + 128) / 256 + 16);
        }
    for (unsigned y = 0; y < H / 2; ++y)
        for (unsigned x = 0; x < W / 2; ++x) {
            int r = 0, g = 0, b = 0;
            for (unsigned k = 0; k < 4; ++k) { const uint8_t* p = px(2 * x + (k & 1), 2 * y + (k >> 1)); b += p[0]; g += p[1]; r += p[2]; }
            r /= 4; g /= 4; b /= 4;
            int u = (-26 * r - 87 * g + 112 * b + 128) / 256 + 128;
            int v = (112 * r - 102 * g - 10 * b + 128) / 256 + 128;
            uv[(size_t)y * W + 2 * x] = (uint8_t)(u < 0 ? 0 : u > 255 ? 255 : u);
            uv[(size_t)y * W + 2 * x + 1] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
}

}  // namespace cr
