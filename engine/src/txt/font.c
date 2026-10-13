/* txt/font.c — stb_truetype glyph cache, 65-level coverage ramp, dilation effects, shadow/outline layers
 * (AST §5.1–§5.3; exe FUN_0066ea50, FUN_006a9cd0, FUN_006aa2e0, FUN_005e5b10). Two faces with fallback: the
 * Japanese Gothic face and the English patch face (docs/SPEC_ENPATCH.md §1.3.1, §6.3.1).
 *
 * Memory: each face keeps its whole font file, released by the module it came from (SDL_free for SDL_LoadFile,
 * fs_free for the game data). Rendered glyphs live in a fixed open-addressing table whose bitmaps come from the
 * "glyphs" arena; when either is full the whole cache is flushed and refilled, so the cache is bounded and drawing
 * allocates nothing once the working set is cached. Coverage temporaries come from a scratch arena.
 *
 * Validation: stb_truetype trusts its input. A font file is accepted only after font_file_valid (font_sfnt.c) checked
 * its tables and glyph index, and every stb call goes through face_info(), which registers the face's file as the
 * only memory the (patched) stb readers may touch. */
#include "txt/font.h"
#include "txt/txt_internal.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "plat/fs.h"
#include <SDL3/SDL.h>
#include <string.h>
#include "stb_truetype.h"

#define EN_FONT_PATH "/en/font/sayooshieng-Regular.ttf"   /* canonical tree (docs/PAYLOAD.md §1) */

enum {
    GLYPH_SLOTS = 8192,                     /* hash table size (power of two) */
    GLYPH_LIVE_MAX = GLYPH_SLOTS / 2,       /* flush beyond this load, so probes stay short and always end */
    GLYPH_ARENA_BYTES = 8 << 20,            /* bitmaps of the cached glyphs */
    GLYPH_DIM_MAX = 1024,                   /* a larger glyph box means a broken font: drawn empty */
    GLYPH_SIZE_MAX = 255,
    EFFECT_MAX = 3,                         /* dilation 0..3 (FUN_006aa2e0) */
    PATH_MAX_BYTES = 1024,
};

typedef enum { SRC_NONE, SRC_SDL, SRC_FS, SRC_BUILTIN } FaceSrc;   /* BUILTIN: linked in, never freed */
typedef struct { stbtt_fontinfo info; uint8_t *data; size_t size; FaceSrc src; bool ok; int upem; } Face;
static Face g_face[FONT_FACES];
static int g_primary = FONT_JA;

typedef struct { uint32_t key; bool used; Glyph g; } GlyphSlot;
static GlyphSlot g_slot[GLYPH_SLOTS];
static int g_live;
static Arena g_glyph_arena;
static bool g_arena_ready;

/* ---- glyph cache ------------------------------------------------------------------------------------------- */
static void glyphs_flush(void)
{
    if (g_live == 0) return;
    memset(g_slot, 0, sizeof g_slot);
    g_live = 0;
    if (g_arena_ready) arena_reset(&g_glyph_arena);
}

static void glyphs_destroy(void)
{
    glyphs_flush();
    if (g_arena_ready) arena_destroy(&g_glyph_arena);
    g_arena_ready = false;
}

/* Zeroed bitmap memory for a glyph; flushes the cache once when the arena is full. NULL if it can never fit. */
static uint8_t *bitmap_alloc(size_t n, bool *flushed)
{
    if (!g_arena_ready) { arena_init(&g_glyph_arena, "glyphs", GLYPH_ARENA_BYTES); g_arena_ready = true; }
    uint8_t *p = arena_try_alloc(&g_glyph_arena, n, 1);
    if (!p && !*flushed) { glyphs_flush(); *flushed = true; p = arena_try_alloc(&g_glyph_arena, n, 1); }
    return p;
}

static uint32_t slot_of(uint32_t key) { return (key * 2654435761u) & (uint32_t)(GLYPH_SLOTS - 1); }

/* ---- faces -------------------------------------------------------------------------------------------------- */
static void release_file(uint8_t *data, FaceSrc src)
{
    if (src == SRC_SDL) SDL_free(data);
    else if (src == SRC_FS) fs_free(data);
}

static void face_free(int face)
{
    Face *f = &g_face[face];
    release_file(f->data, f->src);
    memset(f, 0, sizeof *f);
    stbtt_SetDataBounds(NULL, 0);                        /* no stb read may reach the released file */
}

/* The face's stb info, with its file registered as the bounds of every stb read (stbtt_SetDataBounds). Every stb
 * call on a face goes through here. */
static const stbtt_fontinfo *face_info(int face)
{
    const Face *f = &g_face[face];
    stbtt_SetDataBounds(f->data, f->size);
    return &f->info;
}

/* Takes ownership of data (released through src on every path). */
static bool face_load(int face, uint8_t *data, size_t size, FaceSrc src, const char *what)
{
    if (!data) return false;
    long ofs = font_sfnt_offset(data, size);
    stbtt_fontinfo info;
    stbtt_SetDataBounds(data, size);
    if (!font_file_valid(data, size) || !stbtt_InitFont(&info, data, (int)ofs)) {
        log_msg(LOG_WARN, "font: %s is not a usable TrueType/OpenType font", what);
        stbtt_SetDataBounds(NULL, 0);
        release_file(data, src);
        return false;
    }
    face_free(face);
    Face *f = &g_face[face];
    *f = (Face){ .info = info, .data = data, .size = size, .src = src, .ok = true };
    float em = stbtt_ScaleForMappingEmToPixels(face_info(face), 1.0f);   /* = 1 / unitsPerEm */
    f->upem = em > 0 ? (int)(1.0f / em + 0.5f) : 0;
    glyphs_flush();
    log_msg(LOG_INFO, "font: %s face = %s", face == FONT_EN ? "EN" : "JA", what);
    return true;
}

static bool try_load(const char *path)
{
    if (!path || !*path) return false;
    size_t size = 0;
    void *data = SDL_LoadFile(path, &size);
    return face_load(FONT_JA, data, size, SRC_SDL, path);
}

/* A font inside the game data (directory or SAYOPAK payload). */
static bool try_load_fs(int face, const char *rel)
{
    size_t len = 0;
    uint8_t *d = fs_read_all(rel, &len);
    if (!d) return false;
    const char *layer = fs_layer_of(rel);
    SB_ON_STACK(what, 320);
    (void)sb_fmt(&what, "%s (%s)", rel, layer ? layer : "game data");   /* a cut label is still a label */
    return face_load(face, d, len, SRC_FS, sb_cstr(&what));
}

static bool try_dir_pattern(const char *dir, const char *pattern)
{
    int n = 0;
    char **list = SDL_GlobDirectory(dir, pattern, 0, &n);
    if (!list) return false;
    bool ok = false;
    for (int i = 0; i < n && !ok; i++) {
        SB_ON_STACK(path, PATH_MAX_BYTES);
        if (path_join(&path, str_c(dir), str_c(list[i])) && sb_ok(&path)) ok = try_load(sb_cstr(&path));
        else log_msg(LOG_WARN, "font: path too long: %s/%s", dir, list[i]);
    }
    SDL_free((void *)list);                              /* SDL's memory */
    return ok;
}

static bool try_dir(const char *dir)
{
    static const char *const pats[] = { "*.ttf", "*.otf", "*.ttc", "*.TTF", "*.OTF", "*.TTC" };
    for (size_t p = 0; p < COUNT_OF(pats); p++)
        if (try_dir_pattern(dir, pats[p])) return true;
    return false;
}

static bool try_system_fonts(void)
{
    static const char *const sys[] = {
        "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
        "/usr/share/fonts/truetype/vlgothic/VL-Gothic-Regular.ttf",
        "C:\\Windows\\Fonts\\msgothic.ttc",
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\meiryo.ttc",
        ("/System/Library/Fonts/\xe3\x83\x92\xe3\x83\xa9\xe3\x82\xae\xe3\x83\x8e\xe8\xa7\x92" /* ヒラギノ角ゴシック */
         "\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf W3.ttc"),
        "/system/fonts/NotoSansCJK-Regular.ttc",
    };
    for (size_t i = 0; i < COUNT_OF(sys); i++)
        if (try_load(sys[i])) return true;
    return false;
}

/* The vendored Kosugi (engine/third_party/fonts, Apache-2.0), linked in by the Makefile: Japanese text renders
 * even on a system without a Japanese font. stb_truetype only reads the bytes. */
static bool try_builtin(void)
{
    return face_load(FONT_JA, (uint8_t *)(uintptr_t)font_fallback_ttf, font_fallback_ttf_size, SRC_BUILTIN,
                     "built-in Kosugi");
}

/* Selftest: the built-in face parses and maps every code point of s (UTF-8) to a glyph. */
bool font_builtin_covers(const char *s)
{
    stbtt_fontinfo info;
    size_t n = font_fallback_ttf_size;
    stbtt_SetDataBounds(font_fallback_ttf, n);
    bool ok = font_file_valid(font_fallback_ttf, n) && stbtt_InitFont(&info, font_fallback_ttf, 0);
    Str str = str_c(s);
    size_t pos = 0;
    uint32_t cp;
    while (ok && pos < str.n) ok = utf_decode8(str, &pos, &cp) && stbtt_FindGlyphIndex(&info, (int)cp) != 0;
    stbtt_SetDataBounds(NULL, 0);
    return ok;
}

/* ENGINE_SPEC §2.12: --font, bundled fonts dir, GameData/font.ttf, system Japanese Gothic fonts, then the built-in
 * fallback. */
static bool load_ja_face(const char *ttf_path)
{
    if (ttf_path && *ttf_path) {
        if (try_load(ttf_path)) return true;
        log_msg(LOG_WARN, "font: cannot load %s, searching defaults", ttf_path);
    }
    const char *base = SDL_GetBasePath();                /* SDL-owned, ends with a separator */
    if (base) {
        SB_ON_STACK(dir, PATH_MAX_BYTES);
        if (sb_fmt(&dir, "%sfonts", base) && try_dir(sb_cstr(&dir))) return true;
    }
    if (try_dir("engine/fonts") || try_dir("fonts")) return true;
    if (try_load_fs(FONT_JA, "/ja/font.ttf")) return true;   /* GameData/font.ttf (make data FONT=...) */
    return try_system_fonts() || try_builtin();
}

bool font_init(const char *ttf_path)
{
    glyphs_flush();
    face_free(FONT_JA);
    face_free(FONT_EN);
    bool ja = load_ja_face(ttf_path);
    bool en = fs_exists(EN_FONT_PATH) && try_load_fs(FONT_EN, EN_FONT_PATH);
    if (!ja && en) log_msg(LOG_INFO, "font: no Japanese system font; the EN patch face draws the Japanese text too");
    if (!ja && !en) { log_msg(LOG_ERR, "font: no Japanese font found (use --font PATH)"); return false; }
    g_primary = ja ? FONT_JA : FONT_EN;
    return true;
}

void font_shutdown(void)
{
    glyphs_destroy();
    face_free(FONT_JA);
    face_free(FONT_EN);
    g_primary = FONT_JA;
}

bool font_has_face(int face) { return face >= 0 && face < FONT_FACES && g_face[face].ok; }

void font_select_for_language(LangId lang)
{
    int want = lang == LANG_EN ? FONT_EN : FONT_JA;
    if (!g_face[want].ok) {
        if (want == FONT_EN)
            log_msg(LOG_WARN, "font: sayooshieng-Regular.ttf missing; English text uses the JA face "
                              "(no comma kerning)");
        want = want == FONT_EN ? FONT_JA : FONT_EN;
    }
    if (!g_face[want].ok) return;
    if (want != g_primary) glyphs_flush();
    g_primary = want;
}

bool font_kerning_active(void) { return g_primary == FONT_EN && g_face[FONT_EN].ok; }

int font_kern_px(uint16_t left, uint16_t right, int size)
{
    const Face *f = &g_face[FONT_EN];
    if (!f->ok || size <= 0) return 0;
    int v = stbtt_GetCodepointKernAdvance(face_info(FONT_EN), left, right);
    if (!v) return 0;
    int upem = f->upem > 0 ? f->upem : 1024;
    long t = (long)v * size * 1024 / upem;                  /* size * k in 1/1024 em, then the exe's rounding */
    return t >= 0 ? (int)((t + 512) >> 10) : -(int)((-t + 512) >> 10);
}

/* ---- rendering ---------------------------------------------------------------------------------------------- */
/* FUN_006a9cd0: 65-level GDI coverage g (0..64) -> alpha g*255/65 (g = 64 gives 251). */
static uint8_t ramp(uint8_t cov)
{
    int g = (cov * 64 + 127) / 255;
    return (uint8_t)(g * 255 / 65);
}

/* The face that draws `code` when `want` is asked for: want itself, else the other face if it has the glyph. */
static int pick_face(uint16_t code, int want, int *gi)
{
    *gi = 0;
    if (want >= 0 && want < FONT_FACES && g_face[want].ok && (*gi = stbtt_FindGlyphIndex(face_info(want), code)))
        return want;
    for (int f = 0; f < FONT_FACES; f++)
        if (f != want && g_face[f].ok && (*gi = stbtt_FindGlyphIndex(face_info(f), code))) return f;
    return -1;
}

/* FUN_006aa2e0: effect n stamps the coverage at every (dx,dy) in [0,n]² with source-over alpha into gl->a
 * (gl->w = bw + n, gl->h = bh + n). */
static void stamp_dilated(Glyph *gl, const uint8_t *cov, int bw, int bh, int n)
{
    for (int dy = 0; dy <= n; dy++)
        for (int dx = 0; dx <= n; dx++)
            for (int y = 0; y < bh; y++) {
                uint8_t *d = gl->a + (size_t)(y + dy) * (size_t)gl->w + (size_t)dx;
                const uint8_t *s = cov + (size_t)y * (size_t)bw;
                for (int x = 0; x < bw; x++) {
                    int sa = ramp(s[x]);
                    if (sa) d[x] = (uint8_t)(sa + d[x] * (255 - sa) / 255);
                }
            }
}

typedef struct { int fi, gi, x0, y0, bw, bh; float sc; int adv; } GlyphBox;

/* Outline box of `code` at `size` in the face that draws it; false for blanks and missing glyphs. */
static bool glyph_box(uint16_t code, int size, int want, GlyphBox *b)
{
    if (code == 0x20 || code == 0x3000) return false;
    b->fi = pick_face(code, want, &b->gi);
    if (b->fi < 0 || !b->gi) return false;
    const stbtt_fontinfo *font = face_info(b->fi);
    int x1, y1, lsb;
    b->sc = stbtt_ScaleForMappingEmToPixels(font, (float)size);
    stbtt_GetGlyphBitmapBox(font, b->gi, b->sc, b->sc, &b->x0, &b->y0, &x1, &y1);
    stbtt_GetGlyphHMetrics(font, b->gi, &b->adv, &lsb);
    b->bw = x1 - b->x0;
    b->bh = y1 - b->y0;
    return b->bw > 0 && b->bh > 0 && b->bw <= GLYPH_DIM_MAX && b->bh <= GLYPH_DIM_MAX;
}

/* Fills gl (adv already set). False only when the bitmap does not fit the glyph arena at all. */
static bool render_into(Glyph *gl, uint16_t code, int size, int effect, int want, bool *flushed)
{
    GlyphBox b;
    if (!glyph_box(code, size, want, &b)) return true;
    /* AST §5.2: baseline at round(0.86·size) below the cell top. Proportional JA fonts are centred in their fixed
     * MS-Gothic cell (full = size, half = size/2). The EN patch face is placed at its left bearing from the cell
     * origin, as GDI does (gmptGlyphOrigin.x, SPEC_ENPATCH §1.3.1): its Latin advances are exactly half a cell. */
    int fadv = (int)((float)b.adv * b.sc + 0.5f);
    int ox = b.fi == FONT_EN ? b.x0 : b.x0 + (gl->adv - fadv) / 2;
    int n = effect < 0 ? 0 : effect > EFFECT_MAX ? EFFECT_MAX : effect;
    uint8_t *a = bitmap_alloc((size_t)(b.bw + n) * (size_t)(b.bh + n), flushed);
    if (!a) return false;
    gl->w = b.bw + n; gl->h = b.bh + n;
    gl->ox = ox < 0 ? 0 : ox;
    gl->oy = (int)(size * 0.86 + 0.5) + b.y0;
    gl->a = a;
    Scratch s = mem_scratch_begin(NULL);
    uint8_t *cov = arena_alloc(s.arena, (size_t)b.bw * (size_t)b.bh, 1);   /* zeroed: an empty outline writes nothing */
    stbtt_MakeGlyphBitmap(face_info(b.fi), cov, b.bw, b.bh, b.bw, b.sc, b.sc, b.gi);
    stamp_dilated(gl, cov, b.bw, b.bh, n);
    mem_scratch_end(&s);
    return true;
}

static const Glyph *glyph_face(uint16_t code, int size, int effect, int face)
{
    if (size <= 0 || size > GLYPH_SIZE_MAX) return NULL;
    uint32_t key = (uint32_t)code | (uint32_t)size << 16 | (uint32_t)(effect & 3) << 24 | (uint32_t)(face & 3) << 26;
    uint32_t h = slot_of(key);
    for (int probe = 0; probe < GLYPH_SLOTS && g_slot[h].used; probe++) {
        if (g_slot[h].key == key) return &g_slot[h].g;
        h = (h + 1) & (uint32_t)(GLYPH_SLOTS - 1);
    }
    bool flushed = false;
    if (g_live >= GLYPH_LIVE_MAX) { glyphs_flush(); flushed = true; }
    Glyph gl = { .adv = msg_char_width(code, size) };
    if (!render_into(&gl, code, size, effect, face, &flushed)) {
        static bool logged;
        txt_cap_overflow(&logged, "glyph arena (one glyph)", GLYPH_ARENA_BYTES);
        gl = (Glyph){ .adv = gl.adv };
    }
    for (h = slot_of(key); g_slot[h].used; h = (h + 1) & (uint32_t)(GLYPH_SLOTS - 1)) {}   /* load <= 1/2 */
    g_slot[h] = (GlyphSlot){ .key = key, .used = true, .g = gl };
    g_live++;
    return &g_slot[h].g;
}

const Glyph *font_glyph(uint16_t code, int size, int effect) { return glyph_face(code, size, effect, g_primary); }

/* Coverage mask over a premultiplied ARGB canvas: src = rgb × a, out = src + dst × (1 − a). */
static void blend_mask(Canvas *c, const Glyph *g, int x, int y, uint32_t rgb, int tr)
{
    if (!g || !g->a || tr <= 0 || !c || !c->px) return;
    int px0 = x + g->ox, py0 = y + g->oy;
    for (int j = 0; j < g->h; j++) {
        int py = py0 + j;
        if (py < 0 || py >= c->h) continue;
        uint32_t *row = c->px + (size_t)py * (size_t)c->pitch;
        const uint8_t *m = g->a + (size_t)j * (size_t)g->w;
        for (int i = 0; i < g->w; i++) {
            int px = px0 + i;
            if (px < 0 || px >= c->w || !m[i]) continue;
            row[px] = txt_px_over(row[px], rgb, m[i] * tr / 255);
        }
    }
}

/* FUN_005e5b10 layers: shadow (mode 1/3), outline at (−1,−1) dilated (mode 2/3), then the main glyph. */
static void draw_char(Canvas *c, int face, uint16_t code, int size, int x, int y, uint32_t moji_rgb,
                      uint32_t fuchi_rgb, uint32_t shadow_rgb, int shadow_mode, bool bold, int tr)
{
    if (shadow_mode == 1 || shadow_mode == 3) {
        float s = (float)(size < 0 ? 0 : size > 32 ? 32 : size);
        float o = shadow_mode == 1 ? s * 1.5f / 32.0f + 0.5f : s / 32.0f + 0.5f - 1.0f;
        int eff = shadow_mode == 1 ? (bold ? 1 : 0) : (bold ? 3 : 2);
        int io = (int)(o + 0.5f);
        blend_mask(c, glyph_face(code, size, eff, face), x + io, y + io, shadow_rgb, tr);
    }
    if (shadow_mode == 2 || shadow_mode == 3)
        blend_mask(c, glyph_face(code, size, bold ? 3 : 2, face), x - 1, y - 1, fuchi_rgb, tr);
    blend_mask(c, glyph_face(code, size, bold ? 1 : 0, face), x, y, moji_rgb, tr);
}

void font_draw_char(Canvas *c, uint16_t code, int size, int x, int y, uint32_t moji_rgb, uint32_t fuchi_rgb,
                    uint32_t shadow_rgb, int shadow_mode, bool bold, int tr)
{
    draw_char(c, g_primary, code, size, x, y, moji_rgb, fuchi_rgb, shadow_rgb, shadow_mode, bold, tr);
}

void font_draw_char_face(Canvas *c, int face, uint16_t code, int size, int x, int y, uint32_t moji_rgb,
                         uint32_t fuchi_rgb, uint32_t shadow_rgb, int shadow_mode, int tr)
{
    draw_char(c, face, code, size, x, y, moji_rgb, fuchi_rgb, shadow_rgb, shadow_mode, false, tr);
}
