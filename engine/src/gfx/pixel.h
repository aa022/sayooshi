/* gfx/pixel.h — premultiplied ARGB8888 pixel arithmetic shared by the canvas primitives and the wipe compositor
 * (GFX §3.5). Internal to gfx/. All functions are total: every channel stays in 0..255 for any input. */
#ifndef GFX_PIXEL_H
#define GFX_PIXEL_H
#include "core/base.h"

/* a*b/255 rounded, exact for a, b in 0..255 (the D3D fixed-function rounding). */
static inline uint32_t px_mul255(uint32_t a, uint32_t b) { uint32_t t = a * b + 128; return (t + (t >> 8)) >> 8; }

/* All four channels scaled by k/255 (k clamped to 0..255). */
static inline uint32_t px_scale(uint32_t p, uint32_t k)
{
    if (k >= 255) return p;
    if (k == 0) return 0;
    return px_mul255(p >> 24, k) << 24 | px_mul255((p >> 16) & 255, k) << 16 | px_mul255((p >> 8) & 255, k) << 8 |
           px_mul255(p & 255, k);
}

/* All four channels scaled by k/256 (k clamped to 0..256), truncating (shader alpha). */
static inline uint32_t px_scale256(uint32_t p, uint32_t k)
{
    if (k >= 256) return p;
    if (k == 0) return 0;
    return ((p >> 24) * k >> 8) << 24 | (((p >> 16) & 255) * k >> 8) << 16 | (((p >> 8) & 255) * k >> 8) << 8 |
           ((p & 255) * k >> 8);
}

/* Premultiplied src over dst: blend ONE, INV_SRC_ALPHA, saturated per channel (GFX §3.5). */
static inline uint32_t px_over(uint32_t d, uint32_t s)
{
    uint32_t sa = s >> 24;
    if (sa == 255) return s;
    if (s == 0) return d;
    uint32_t ia = 255 - sa, out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        uint32_t v = ((s >> sh) & 255) + px_mul255((d >> sh) & 255, ia);
        out |= (v > 255 ? 255 : v) << sh;
    }
    return out;
}

/* Straight-alpha 0xAARRGGBB to premultiplied. */
static inline uint32_t px_premul(uint32_t straight)
{
    uint32_t a = straight >> 24;
    if (a == 255) return straight;
    if (a == 0) return 0;
    return a << 24 | px_mul255((straight >> 16) & 255, a) << 16 | px_mul255((straight >> 8) & 255, a) << 8 |
           px_mul255(straight & 255, a);
}

static inline int px_clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

#endif
