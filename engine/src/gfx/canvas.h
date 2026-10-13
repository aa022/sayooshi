/* gfx/canvas.h — premultiplied ARGB8888 canvas primitives and the image cache (GFX §3.5, ENGINE_SPEC §2.1 #2, WP3).
 * Pixels are 0xAARRGGBB in a uint32 (= BGRA bytes in memory on little-endian). */
#ifndef GFX_CANVAS_H
#define GFX_CANVAS_H
#include "core/base.h"

enum { SCREEN_W = 640, SCREEN_H = 480 };   /* #SCREEN_SIZE */

typedef struct { uint32_t *px; int w, h, pitch; } Canvas;          /* pitch in pixels */
typedef struct { uint32_t *px; int w, h; int cx, cy; } Image;      /* premultiplied */
typedef struct { int x0, y0, x1, y1; } Rect;                       /* right/bottom exclusive */

void canvas_clear(Canvas *c, uint32_t argb);
void canvas_fill(Canvas *c, Rect r, uint32_t argb_straight, int tr);
void canvas_blit(Canvas *dst, const Image *src, Rect src_r, int dx, int dy, int tr, const Rect *clip);
void canvas_blit_scaled(Canvas *dst, const Image *src, Rect src_r, float dx, float dy, float sx, float sy, int tr,
                        const Rect *clip);
/* Cached decoded image; NULL if missing; handles "a|b" composites. The pointer stays valid until the next
 * gfx_frame (the cache evicts only there); do not keep it longer. */
const Image *img_get(const char *name, int patno);

/* ---- canvas storage and rectangles ---- */
bool canvas_alloc(Canvas *c, int w, int h);       /* zeroed (transparent); false (c zeroed) for sides <= 0 or > 16384 */
void canvas_free(Canvas *c);                      /* only for canvases from canvas_alloc */
void canvas_copy(Canvas *dst, const Canvas *src, int dx, int dy);   /* replace (no blend), clipped */
/* Box filter of src (any size) onto all of dst: each dst pixel is the mean of its source box, opaque (thumbnails). */
void canvas_downscale_box(Canvas *dst, const Canvas *src);
static inline Rect rect_make(int x0, int y0, int x1, int y1) { Rect r = { x0, y0, x1, y1 }; return r; }
bool rect_intersect(Rect *r, const Rect *with);   /* false if empty afterwards */
/* {x, y, x+w, y+h} computed in 64 bits and clamped to +-2^24 per coordinate: script or save values cannot overflow,
 * and sums or differences of two coordinates stay far inside int. */
Rect rect_at(int64_t x, int64_t y, int64_t w, int64_t h);

/* GFX §3.5 tone, all fields 0..255 (rate/colour/add as in the shader). NULL/zero = identity. */
typedef struct { int mono, reverse, bright, dark, r, g, b, rate, add_r, add_g, add_b; } Tone;
bool tone_is_identity(const Tone *t);
uint32_t tone_apply_px(uint32_t premul, const Tone *t);              /* one premultiplied pixel */
void canvas_tone(Canvas *c, const Tone *t, const Rect *clip);       /* in-place post-process */
/* Full object blit: src rect scaled by (sx,sy) placed at (dx,dy), alpha tr, optional tone. */
void canvas_blit_ex(Canvas *dst, const Image *src, Rect src_r, float dx, float dy, float sx, float sy,
                    int tr, const Tone *tone, const Rect *clip);

int  img_pat_count(const char *name);             /* number of cuts; 0 if missing */
bool img_exists(const char *name);
void img_cache_clear(void);

#endif
