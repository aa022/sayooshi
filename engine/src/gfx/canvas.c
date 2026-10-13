/* gfx/canvas.c — premultiplied ARGB8888 canvas primitives: rects, fill, blit (1:1 and scaled), tone (GFX §3.5).
 *
 * Safety: every primitive validates its canvases and clips every rect against the destination first, so any input
 * (script positions near INT_MAX, negative sizes, NaN scales) draws nothing out of range. Rect arithmetic that can
 * overflow is done in 64 bits and clamped to ±RECT_COORD_MAX, which keeps intersections with any canvas exact.
 * Canvas pixels from canvas_alloc are on the tracked heap (MEM_GFX); per-call tables come from scratch memory, so
 * drawing never touches the heap. */
#include "gfx/canvas.h"
#include "core/mem.h"
#include "gfx/pixel.h"
#include "gfx/render.h"
#include <SDL3/SDL.h>
#include <string.h>

enum {
    RECT_COORD_MAX = 1 << 24,    /* far outside any canvas, still exact as a float */
    CANVAS_DIM_MAX = 1 << 14,    /* canvas_alloc refuses larger sides (16k x 16k = 1 GiB) */
};

/* ---------------------------------------------------------------- rects */

static int coord_clamp(int64_t v)
{ return v < -RECT_COORD_MAX ? -RECT_COORD_MAX : v > RECT_COORD_MAX ? RECT_COORD_MAX : (int)v; }

/* ceil(v - 0.5) clamped: the first pixel whose centre is >= v (D3D rasterisation, no -0.5 offset). NaN -> min. */
static int cover_edge(float v)
{
    float c = SDL_ceilf(v - 0.5f);
    if (!(c > (float)-RECT_COORD_MAX)) return -RECT_COORD_MAX;
    if (c > (float)RECT_COORD_MAX) return RECT_COORD_MAX;
    return (int)c;
}

Rect rect_cover(float x0, float y0, float x1, float y1)
{
    return rect_make(cover_edge(x0), cover_edge(y0), cover_edge(x1), cover_edge(y1));
}

Rect rect_at(int64_t x, int64_t y, int64_t w, int64_t h)
{
    return rect_make(coord_clamp(x), coord_clamp(y), coord_clamp(x + w), coord_clamp(y + h));
}

bool rect_intersect(Rect *r, const Rect *w)
{
    if (w) {
        if (r->x0 < w->x0) r->x0 = w->x0;
        if (r->y0 < w->y0) r->y0 = w->y0;
        if (r->x1 > w->x1) r->x1 = w->x1;
        if (r->y1 > w->y1) r->y1 = w->y1;
    }
    return r->x0 < r->x1 && r->y0 < r->y1;
}

static bool canvas_ok(const Canvas *c) { return c && c->px && c->w > 0 && c->h > 0 && c->pitch >= c->w; }
static bool image_ok(const Image *im) { return im && im->px && im->w > 0 && im->h > 0; }

/* The canvas rect, intersected with an optional clip (possibly empty). */
static Rect dst_bounds(const Canvas *c, const Rect *clip)
{
    Rect r = { 0, 0, c->w, c->h };
    (void)rect_intersect(&r, clip);   /* an empty result makes the callers' own intersection fail */
    return r;
}

static uint32_t *canvas_row(const Canvas *c, int y) { return c->px + (size_t)y * (size_t)c->pitch; }
static const uint32_t *image_row(const Image *im, int y) { return im->px + (size_t)y * (size_t)im->w; }

/* ---------------------------------------------------------------- canvas */

bool canvas_alloc(Canvas *c, int w, int h)
{
    memset(c, 0, sizeof *c);
    if (w <= 0 || h <= 0 || w > CANVAS_DIM_MAX || h > CANVAS_DIM_MAX) return false;
    c->px = mem_alloc_array((size_t)w * (size_t)h, sizeof *c->px, MEM_GFX);   /* zeroed = transparent */
    c->w = w; c->h = h; c->pitch = w;
    return true;
}

void canvas_free(Canvas *c)
{
    if (!c) return;
    mem_free(c->px);
    memset(c, 0, sizeof *c);
}

void canvas_clear(Canvas *c, uint32_t argb)
{
    if (!canvas_ok(c)) return;
    for (int y = 0; y < c->h; y++) {
        uint32_t *row = canvas_row(c, y);
        if (argb == 0) memset(row, 0, (size_t)c->w * sizeof *row);
        else for (int x = 0; x < c->w; x++) row[x] = argb;
    }
}

void canvas_copy(Canvas *dst, const Canvas *src, int dx, int dy)
{
    if (!canvas_ok(dst) || !canvas_ok(src)) return;
    Rect r = rect_at(dx, dy, src->w, src->h);
    if (!rect_intersect(&r, &(Rect){ 0, 0, dst->w, dst->h })) return;
    for (int y = r.y0; y < r.y1; y++)
        memcpy(canvas_row(dst, y) + r.x0, canvas_row(src, y - dy) + (r.x0 - dx),
               (size_t)(r.x1 - r.x0) * sizeof(uint32_t));
}

void canvas_fill(Canvas *c, Rect r, uint32_t argb_straight, int tr)
{
    if (!canvas_ok(c)) return;
    uint32_t a = px_mul255(argb_straight >> 24, (uint32_t)px_clamp255(tr));
    uint32_t s = px_premul((argb_straight & 0xffffffu) | a << 24);
    if (!rect_intersect(&r, &(Rect){ 0, 0, c->w, c->h }) || s == 0) return;
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t *row = canvas_row(c, y);
        if ((s >> 24) == 255) for (int x = r.x0; x < r.x1; x++) row[x] = s;
        else for (int x = r.x0; x < r.x1; x++) row[x] = px_over(row[x], s);
    }
}

void canvas_blit(Canvas *dst, const Image *src, Rect sr, int dx, int dy, int tr, const Rect *clip)
{
    if (!canvas_ok(dst) || !image_ok(src) || tr <= 0) return;
    uint32_t k = (uint32_t)px_clamp255(tr);
    if (!rect_intersect(&sr, &(Rect){ 0, 0, src->w, src->h })) return;
    Rect r = rect_at(dx, dy, sr.x1 - sr.x0, sr.y1 - sr.y0);
    Rect b = dst_bounds(dst, clip);
    if (!rect_intersect(&r, &b)) return;
    /* r lies inside [dx, dx + w) x [dy, dy + h), so these source coordinates lie inside sr */
    int sx0 = (int)((int64_t)r.x0 - dx + sr.x0), sy0 = (int)((int64_t)r.y0 - dy + sr.y0), n = r.x1 - r.x0;
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t *d = canvas_row(dst, y) + r.x0;
        const uint32_t *s = image_row(src, sy0 + (y - r.y0)) + sx0;
        if (k == 255) for (int i = 0; i < n; i++) d[i] = px_over(d[i], s[i]);
        else for (int i = 0; i < n; i++) d[i] = px_over(d[i], px_scale(s[i], k));
    }
}

/* lo + floor(pos), clamped to [lo, hi): nearest-neighbour source index of a destination pixel centre. */
static int src_index(float pos, int lo, int hi)
{
    float k = SDL_floorf(pos);
    if (!(k > 0.0f)) return lo;                       /* also NaN */
    if (k >= (float)(hi - lo)) return hi - 1;
    return lo + (int)k;
}

static bool blit_is_plain(float dx, float dy, float sx, float sy, bool tone_on)
{
    return !tone_on && sx == 1.0f && sy == 1.0f && dx == SDL_floorf(dx) && dy == SDL_floorf(dy) &&
           SDL_fabsf(dx) < (float)RECT_COORD_MAX && SDL_fabsf(dy) < (float)RECT_COORD_MAX;
}

void canvas_blit_ex(Canvas *dst, const Image *src, Rect sr, float dx, float dy, float sx, float sy,
                    int tr, const Tone *tone, const Rect *clip)
{
    if (!canvas_ok(dst) || !image_ok(src) || tr <= 0 || !(sx > 0) || !(sy > 0)) return;
    uint32_t k = (uint32_t)px_clamp255(tr);
    if (!rect_intersect(&sr, &(Rect){ 0, 0, src->w, src->h })) return;
    bool tone_on = tone && !tone_is_identity(tone);
    if (blit_is_plain(dx, dy, sx, sy, tone_on)) {
        canvas_blit(dst, src, sr, (int)dx, (int)dy, (int)k, clip);
        return;
    }
    float w = (float)(sr.x1 - sr.x0) * sx, h = (float)(sr.y1 - sr.y0) * sy;
    Rect r = rect_cover(dx, dy, dx + w, dy + h);
    Rect b = dst_bounds(dst, clip);
    if (!rect_intersect(&r, &b)) return;
    Scratch s = mem_scratch_begin(NULL);
    int *col = ARENA_NEW_ARRAY(s.arena, int, (size_t)(r.x1 - r.x0));   /* source column of each destination x */
    for (int x = r.x0; x < r.x1; x++) col[x - r.x0] = src_index(((float)x + 0.5f - dx) / sx, sr.x0, sr.x1);
    for (int y = r.y0; y < r.y1; y++) {
        const uint32_t *srow = image_row(src, src_index(((float)y + 0.5f - dy) / sy, sr.y0, sr.y1));
        uint32_t *d = canvas_row(dst, y);
        for (int x = r.x0; x < r.x1; x++) {
            uint32_t p = srow[col[x - r.x0]];
            if (!p) continue;
            if (tone_on) p = tone_apply_px(p, tone);
            d[x] = px_over(d[x], px_scale(p, k));
        }
    }
    mem_scratch_end(&s);
}

void canvas_blit_scaled(Canvas *dst, const Image *src, Rect src_r, float dx, float dy, float sx, float sy, int tr,
                        const Rect *clip)
{
    canvas_blit_ex(dst, src, src_r, dx, dy, sx, sy, tr, NULL, clip);
}

/* ---------------------------------------------------------------- tone (GFX §3.5) */

bool tone_is_identity(const Tone *t)
{
    return !t || !(t->mono | t->reverse | t->bright | t->dark | t->rate | t->add_r | t->add_g | t->add_b);
}

static float unit(int v) { return (float)v / 255.f; }

/* The shader: reverse, mono towards the luminance, bright/dark, colour rate, add; on straight colour. */
uint32_t tone_apply_px(uint32_t p, const Tone *t)
{
    uint32_t a = p >> 24;
    if (!a || tone_is_identity(t)) return p;
    float fa = (float)a;
    float c[3] = { (float)((p >> 16) & 255) / fa, (float)((p >> 8) & 255) / fa, (float)(p & 255) / fa };
    float col[3] = { unit(t->r), unit(t->g), unit(t->b) }, add[3] = { unit(t->add_r), unit(t->add_g), unit(t->add_b) };
    float y = 0.2989f * c[0] + 0.5886f * c[1] + 0.1145f * c[2];   /* luminance of the colour before reverse */
    float rev = unit(t->reverse), mono = unit(t->mono), rate = unit(t->rate);
    uint32_t out = a << 24;
    for (int k = 0; k < 3; k++) {
        float v = c[k] + (1.f - 2.f * c[k]) * rev;
        v += (y - v) * mono;
        v += unit(t->bright) - unit(t->dark);
        v += (col[k] - v) * rate;
        v += add[k];
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        out |= (uint32_t)(v * fa + 0.5f) << (16 - 8 * k);
    }
    return out;
}

void canvas_tone(Canvas *c, const Tone *t, const Rect *clip)
{
    if (!canvas_ok(c) || tone_is_identity(t)) return;
    Rect r = dst_bounds(c, clip);
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t *d = canvas_row(c, y);
        for (int x = r.x0; x < r.x1; x++) d[x] = tone_apply_px(d[x], t);
    }
}

/* Mean of src over [x0,x1) x [y0,y1) (non-empty, inside src), opaque. */
static uint32_t box_mean(const Canvas *src, int x0, int y0, int x1, int y1)
{
    uint32_t acc[3] = { 0, 0, 0 }, n = 0;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++, n++) {
            uint32_t p = src->px[(size_t)yy * (size_t)src->pitch + (size_t)xx];
            acc[0] += (p >> 16) & 255; acc[1] += (p >> 8) & 255; acc[2] += p & 255;
        }
    return 0xff000000u | (acc[0] / n) << 16 | (acc[1] / n) << 8 | (acc[2] / n);
}

static int scale_div(int a, int b, int c) { return (int)((int64_t)a * b / c); }   /* a*b/c, c > 0 */

void canvas_downscale_box(Canvas *dst, const Canvas *src)
{
    if (!dst || !dst->px || !src || !src->px || dst->w <= 0 || dst->h <= 0 || src->w <= 0 || src->h <= 0) return;
    for (int y = 0; y < dst->h; y++) {
        int sy0 = scale_div(y, src->h, dst->h), sy1 = scale_div(y + 1, src->h, dst->h);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < dst->w; x++) {
            int sx0 = scale_div(x, src->w, dst->w), sx1 = scale_div(x + 1, src->w, dst->w);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            dst->px[(size_t)y * (size_t)dst->pitch + (size_t)x] = box_mean(src, sx0, sy0, sx1, sy1);
        }
    }
}
