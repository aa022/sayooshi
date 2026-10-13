/* gfx/wipe.c — wipe compositing by type (GFX §4.3).
 * Corrections to GFX §4.3 checked in the exe:
 *  - 102: both direction pairs use a gradient of length bs repeated every bs px (FUN_00591c30 passes bs as the
 *    texture length for d 0/1 and for d 2/3), so d=2 gives 16-px vertical blinds, not a full-length wipe.
 *  - 214 (0xd6 in FUN_0056b720): OLD's UV window side s = 1 - 0.667*P and OLD alpha = 255 -> 0
 *    (FUN_0056d490(0xff, p, 0)); the 1.998 in-out formula of the spec is type 212 (0xd4).
 * OLD and NEW are full-screen canvases of the wipe (gfx_wipe.c); every loop is bounded by the smaller of the
 * canvases involved, and per-call tables come from scratch memory. */
#include "gfx/render.h"
#include "core/log.h"
#include "core/mem.h"
#include "gfx/pixel.h"
#include <SDL3/SDL.h>
#include <math.h>

enum { WIPE_P_ONE = 65536, WIPE_BLOCK_MIN = 2, WIPE_BLOCK_MAX = 128, WIPE_SOFT_MAX = 8 };

static int clamp_progress(int p) { return p < 0 ? 0 : p > WIPE_P_ONE ? WIPE_P_ONE : p; }
static Image img_of(const Canvas *c) { Image im = { c->px, c->w, c->h, 0, 0 }; return im; }   /* pitch == w */
static int opt(const int *opts, int nopts, int k) { return (opts && k < nopts) ? opts[k] : 0; }
static bool canvases_ok(const Canvas *a, const Canvas *b)
{
    return a && b && a->px && b->px && a->w > 0 && a->h > 0 && b->w > 0 && b->h > 0;
}

static int block_size(const int *opts, int nopts)
{
    int bs = opt(opts, nopts, 2);
    return bs < WIPE_BLOCK_MIN ? WIPE_BLOCK_MIN : bs > WIPE_BLOCK_MAX ? WIPE_BLOCK_MAX : bs;
}

/* FUN_00591820 / FUN_005919d0 / FUN_00591c30 mask textures (the mask runs along y for d 0/1, x otherwise). */
uint8_t wipe_mask_value(int type, const int *opts, int nopts, int x, int y, int w, int h)
{
    int d = opt(opts, nopts, 1);
    bool vert = d == 0 || d == 1;
    int L = vert ? h : w, k = vert ? y : x;
    bool invert = !(d == 1 || d == 3);   /* called with param_2 = 0; d 1/3 flip it */
    int v;
    if (type == 101) {
        int bs = block_size(opts, nopts);
        int nb = (L + bs - 1) / bs, b = k / bs, j = k % bs;
        int base = nb - 1 == 0 ? 127 : b * 127 / (nb - 1);
        v = 128 * j / (bs - 1) + base;
    } else if (type == 102) {
        int bs = block_size(opts, nopts);
        v = (k % bs) * 255 / (bs - 1);   /* wrap addressing [I] (ENGINE_SPEC §1.6) */
    } else {
        v = L - 1 <= 0 ? 255 : (int)((int64_t)k * 255 / (L - 1));
    }
    v = v < 0 ? 0 : v > 255 ? 255 : v;   /* only out-of-canvas k can leave 0..255 */
    return (uint8_t)(invert ? 255 - v : v);
}

/* Shader 0x6b2604 + preshader: alpha = clamp(C0*(m/255 - 1) + C1, 0, 1), C0 = 255 - 254 s, C1 = (256 - 254 s) P;
 * soft level s from option[0] (FUN_00568ca0). */
int wipe_mask_alpha256(int m, int progress, int soft_opt)
{
    double s = soft_opt <= 0 ? 0.0 : soft_opt >= WIPE_SOFT_MAX ? 1.0 : 1.0 - ldexp(1.0, -soft_opt);
    double P = clamp_progress(progress) / 65536.0;
    double a = (255.0 - 254.0 * s) * (m / 255.0 - 1.0) + (256.0 - 254.0 * s) * P;
    a = a < 0 ? 0 : a > 1 ? 1 : a;
    return (int)(a * 256.0 + 0.5);
}

static void draw_full(Canvas *dst, const Canvas *src, int alpha255)
{
    Image im = img_of(src);
    canvas_blit(dst, &im, rect_make(0, 0, src->w, src->h), 0, 0, alpha255, NULL);
}

/* 100/101/102: NEW over OLD through a 1-D mask texture. */
static void composite_mask(Canvas *dst, const Canvas *old_c, const Canvas *new_c, int type, int p, const int *opts,
                           int nopts)
{
    draw_full(dst, old_c, 255);
    bool vert = opt(opts, nopts, 1) == 0 || opt(opts, nopts, 1) == 1;
    int w = core_min_i32(dst->w, new_c->w), h = core_min_i32(dst->h, new_c->h), L = vert ? h : w;
    Scratch s = mem_scratch_begin(NULL);
    int *a = ARENA_NEW_ARRAY(s.arena, int, (size_t)L);
    for (int k = 0; k < L; k++)
        a[k] = wipe_mask_alpha256(wipe_mask_value(type, opts, nopts, vert ? 0 : k, vert ? k : 0, w, h), p,
                                  opt(opts, nopts, 0));
    for (int y = 0; y < h; y++) {
        uint32_t *dr = dst->px + (size_t)y * (size_t)dst->pitch;
        const uint32_t *nr = new_c->px + (size_t)y * (size_t)new_c->pitch;
        for (int x = 0; x < w; x++) {
            int al = a[vert ? y : x];
            if (al) dr[x] = px_over(dr[x], px_scale256(nr[x], (uint32_t)al));
        }
    }
    mem_scratch_end(&s);
}

/* 200 slide (mode 1): the layer pushed in/out along dir. */
static void slide_layer(Canvas *dst, const Image *im, bool is_new, int dir, int p)
{
    int W = im->w, H = im->h;
    int offy = (int)((int64_t)H * p / WIPE_P_ONE), offx = (int)((int64_t)W * p / WIPE_P_ONE);
    int x = 0, y = 0;
    switch (dir) {
    case 0: y = is_new ? -H + offy : offy; break;
    case 1: y = is_new ? H - offy : -offy; break;
    case 2: x = is_new ? -W + offx : offx; break;
    default: x = is_new ? W - offx : -offx; break;
    }
    canvas_blit(dst, im, rect_make(0, 0, W, H), x, y, 255, NULL);
}

/* 200 stretch (mode 2): anchored at the entering (NEW) / leaving (OLD) edge. */
static void stretch_layer(Canvas *dst, const Image *im, bool is_new, int dir, int p)
{
    double P = p / 65536.0, s = is_new ? P : 1.0 - P;
    if (s <= 0) return;
    float W = (float)im->w, H = (float)im->h, sx = 1, sy = 1, x = 0, y = 0;
    if (dir == 0 || dir == 1) sy = (float)s; else sx = (float)s;
    switch (dir) {
    case 0: y = is_new ? 0 : (float)(H * P); break;
    case 1: y = is_new ? (float)(H - H * P) : 0; break;
    case 2: x = is_new ? 0 : (float)(W * P); break;
    default: x = is_new ? (float)(W - W * P) : 0; break;
    }
    canvas_blit_ex(dst, im, rect_make(0, 0, im->w, im->h), x, y, sx, sy, 255, NULL, NULL);
}

/* 200: opt0 direction, opt1 NEW mode, opt2 OLD mode (0 static, 1 slide, 2 stretch) (FUN_00569ce0) */
static void draw_200_layer(Canvas *dst, const Canvas *c, bool is_new, int dir, int mode, int p)
{
    Image im = img_of(c);
    if (mode == 1) slide_layer(dst, &im, is_new, dir, p);
    else if (mode == 2) stretch_layer(dst, &im, is_new, dir, p);
    else canvas_blit(dst, &im, rect_make(0, 0, c->w, c->h), 0, 0, 255, NULL);
}

static void composite_200(Canvas *dst, const Canvas *old_c, const Canvas *new_c, int p, const int *opts, int nopts)
{
    int dir = opt(opts, nopts, 0), nm = opt(opts, nopts, 1), om = opt(opts, nopts, 2);
    if (nm == 0) { draw_200_layer(dst, new_c, true, dir, nm, p); draw_200_layer(dst, old_c, false, dir, om, p); }
    else { draw_200_layer(dst, old_c, false, dir, om, p); draw_200_layer(dst, new_c, true, dir, nm, p); }
}

/* 210/211 pivot (opt0) and which axes scale. */
typedef struct Pivot { float x, y; bool sx_on, sy_on; } Pivot;

static Pivot squash_pivot(int kind, float W, float H, const int *opts, int nopts)
{
    Pivot v = { 0, 0, true, true };
    switch (kind) {
    case 0: v.x = W / 2; v.y = H / 2; break;
    case 1: break;
    case 2: v.x = W; break;
    case 3: v.y = H; break;
    case 4: v.x = W; v.y = H; break;
    case 5: v.x = W / 2; v.y = H / 2; v.sx_on = false; break;
    case 6: v.x = W / 2; v.y = H / 2; v.sy_on = false; break;
    case 7: v.sx_on = false; break;
    case 8: v.y = H; v.sx_on = false; break;
    case 9: v.sy_on = false; break;
    case 10: v.x = W; v.sy_on = false; break;
    case 11: {
        float ox = (float)opt(opts, nopts, 2), oy = (float)opt(opts, nopts, 3);
        v.x = ox < 0 ? 0 : ox > W ? W : ox;
        v.y = oy < 0 ? 0 : oy > H ? H : oy;
        break;
    }
    default: break;
    }
    return v;
}

/* 210/211: FUN_0056b410 — pivot by opt0, scale 1->0 (211, OLD on top) or 0->1 (210, NEW on top); opt1==1 fades. */
static void composite_squash(Canvas *dst, const Canvas *old_c, const Canvas *new_c, int type, int p, const int *opts,
                             int nopts)
{
    bool grow = type == 210;
    const Canvas *under = grow ? old_c : new_c, *top = grow ? new_c : old_c;
    draw_full(dst, under, 255);
    int a0 = grow ? 0 : 255, a1 = grow ? 255 : 0;
    int alpha = opt(opts, nopts, 1) == 1 ? a0 + (a1 - a0) * p / WIPE_P_ONE : 255;
    float s = grow ? (float)p / 65536.0f : 1.0f - (float)p / 65536.0f;
    Pivot v = squash_pivot(opt(opts, nopts, 0), (float)top->w, (float)top->h, opts, nopts);
    float sx = v.sx_on ? s : 1.0f, sy = v.sy_on ? s : 1.0f;
    if (sx <= 0 || sy <= 0) return;
    Image im = img_of(top);
    canvas_blit_ex(dst, &im, rect_make(0, 0, top->w, top->h), v.x - v.x * sx, v.y - v.y * sy, sx, sy, alpha, NULL,
                   NULL);
}

/* 214 UV window [u0,u1) x [v0,v1) of OLD for side s (opt0 picks the anchor). */
static void zoom_window(int kind, float s, float uv[4])
{
    float s2 = s * 0.51f + 0.49f, u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    switch (kind) {
    case 1: u1 = v1 = s; break;
    case 2: u0 = 1 - s; v1 = s; break;
    case 3: u1 = s; v0 = 1 - s; break;
    case 4: u0 = v0 = 1 - s; break;
    case 5: v1 = s2; v0 = 1 - s2; break;
    case 6: u1 = s2; u0 = 1 - s2; break;
    case 7: v1 = s; break;
    case 8: v0 = 1 - s; break;
    case 9: u1 = s; break;
    case 10: u0 = 1 - s; break;
    default: u0 = v0 = 0.5f - s * 0.5f; u1 = v1 = 0.5f + s * 0.5f; break;   /* 0 and unknown: centre */
    }
    uv[0] = u0; uv[1] = v0; uv[2] = u1; uv[3] = v1;
}

/* Texel of destination coordinate i (pixel centre) through the window [t0, t1) of a texture of n texels. */
static int zoom_texel(float t0, float t1, int i, int n)
{
    float t = SDL_floorf((t0 + (t1 - t0) * ((float)i + 0.5f) / (float)n) * (float)n);
    return !(t > 0) ? 0 : t >= (float)n ? n - 1 : (int)t;
}

/* 214: FUN_0056b720 case 0xd6 — OLD drawn full-screen through a UV window of side s, fading out. */
static void composite_zoom(Canvas *dst, const Canvas *old_c, const Canvas *new_c, int p, const int *opts, int nopts)
{
    draw_full(dst, new_c, 255);
    int alpha = 255 - 255 * p / WIPE_P_ONE;
    if (alpha <= 0) return;
    float uv[4];
    zoom_window(opt(opts, nopts, 0), 1.0f - (float)p * 0.667f / 65536.0f, uv);
    int W = old_c->w, H = old_c->h, w = core_min_i32(dst->w, W), h = core_min_i32(dst->h, H);
    uint32_t k = (uint32_t)(alpha * 256 / 255);
    Scratch s = mem_scratch_begin(NULL);
    int *ux = ARENA_NEW_ARRAY(s.arena, int, (size_t)w);
    for (int x = 0; x < w; x++) ux[x] = zoom_texel(uv[0], uv[2], x, W);
    for (int y = 0; y < h; y++) {
        uint32_t *dr = dst->px + (size_t)y * (size_t)dst->pitch;
        const uint32_t *sr = old_c->px + (size_t)zoom_texel(uv[1], uv[3], y, H) * (size_t)old_c->pitch;
        for (int x = 0; x < w; x++) {
            uint32_t s0 = sr[ux[x]];
            if (s0) dr[x] = px_over(dr[x], px_scale256(s0, k));
        }
    }
    mem_scratch_end(&s);
}

static void unknown_type(int type)
{
    static bool once;
    if (!once) { once = true; log_msg(LOG_WARN, "gfx: wipe type %d not implemented, using crossfade", type); }
}

void wipe_composite(Canvas *dst, const Canvas *old_c, const Canvas *new_c, int type,
                    int32_t progress, const int *opts, int nopts)
{
    if (!canvases_ok(dst, old_c) || !canvases_ok(dst, new_c)) return;
    int p = clamp_progress(progress);
    switch (type) {
    case 1: draw_full(dst, new_c, 255); break;   /* FUN_005686e0: only NEW */
    case 2: draw_full(dst, old_c, 255); break;   /* FUN_00568740: only OLD until the end */
    case 100: case 101: case 102: composite_mask(dst, old_c, new_c, type, p, opts, nopts); break;
    case 200: composite_200(dst, old_c, new_c, p, opts, nopts); break;
    case 210: case 211: composite_squash(dst, old_c, new_c, type, p, opts, nopts); break;
    case 214: composite_zoom(dst, old_c, new_c, p, opts, nopts); break;
    default:   /* 0 crossfade (FUN_005687a0): NEW over OLD with a = p*255/65536 (FUN_0056d440) */
        if (type != 0) unknown_type(type);
        draw_full(dst, old_c, 255);
        draw_full(dst, new_c, (int)((int64_t)p * 255 / WIPE_P_ONE));
        break;
    }
}
