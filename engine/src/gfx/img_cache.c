/* gfx/img_cache.c — decoded g00 images by name: img_get / img_pat_count / img_exists (GFX §3.4, AST §1).
 *
 * Names are normalised to the cache key (ASCII upper case, "?N" tonecurve suffixes dropped per '|' part). A key
 * with '|' is a composite (gallery): its parts are blended in order onto the union canvas of each cut.
 *
 * Ownership: entries live in a core Cache (byte budget IMG_BUDGET, values on the tracked heap as MEM_IMG). The
 * pixels of a single file stay inside its G00 (released with g00_free), composites own one pixel block. Missing
 * names are cached as negative entries so a missing file is looked up (and logged) once. The cache only evicts in
 * img_cache_trim, which gfx_frame calls once per frame, so every pointer img_get returns stays valid until the
 * next frame starts. */
#include "gfx/canvas.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "fmt/g00.h"
#include "gfx/pixel.h"
#include "gfx/render.h"
#include "plat/fs.h"
#include <string.h>

enum {
    IMG_MAX_ENTRIES = 4096,   /* distinct names (the game has ~620 g00 files) */
    IMG_PARTS_MAX = 8,        /* composite parts used ("a|b|..."), as the original */
    IMG_SPILL_MAX = 16,       /* entries kept until the next trim when the cache table is full */
};
static const size_t IMG_BUDGET = (size_t)160 << 20;   /* LRU byte budget (M4: peak RSS < 512 MB) */

typedef struct ImgEntry {
    int ncut;                 /* -1: missing (negative entry) */
    Image *cut;               /* ncut images; px NULL for an empty cut */
    G00 *g00;                 /* owns the pixels of a single file */
    uint32_t *own;            /* composite pixels */
} ImgEntry;

static Cache g_cache;
static bool g_ready;
static ImgEntry *g_spill[IMG_SPILL_MAX];   /* used this frame but not in the table (see img_store) */
static int g_nspill;

/* ---------------------------------------------------------------- entries */

static void entry_free(void *value)
{
    ImgEntry *e = value;
    if (!e) return;
    g00_free(e->g00);
    mem_free(e->own);
    mem_free(e->cut);
    mem_free(e);
}

static ImgEntry *entry_new(int ncut)
{
    ImgEntry *e = mem_zalloc(sizeof *e, MEM_IMG);
    e->ncut = ncut;
    if (ncut > 0) e->cut = mem_alloc_array((size_t)ncut, sizeof *e->cut, MEM_IMG);
    return e;
}

static void cache_ready(void)
{
    if (g_ready) return;
    cache_init(&g_cache, "g00 images", IMG_BUDGET, IMG_MAX_ENTRIES, entry_free);
    g_ready = true;
}

static void spill_release(void)
{
    for (int i = 0; i < g_nspill; i++) entry_free(g_spill[i]);
    g_nspill = 0;
}

/* ---------------------------------------------------------------- loading */

/* Cache key: ASCII upper case without the "?N" suffix of each '|' part. False when it does not fit a cache key. */
static bool make_key(const char *name, StrBuf *key)
{
    bool skip = false;
    for (const char *p = name; *p; p++) {
        if (*p == '|') skip = false;
        else if (*p == '?') skip = true;
        if (skip) continue;
        if (!sb_put_char(key, str_ascii_upper(*p))) break;
    }
    return sb_ok(key);
}

static G00 *read_g00(const char *name)
{
    char *path = fs_path("g00", name, "g00");
    if (!path) return NULL;
    size_t len = 0;
    uint8_t *data = fs_read(path, &len);
    fs_free(path);
    if (!data) return NULL;
    G00 *g = g00_decode(data, len);
    fs_free(data);
    return g;
}

/* One file: cuts premultiplied in place inside the G00, which the entry then owns. NULL if missing/corrupt. */
static ImgEntry *load_single(const char *name, size_t *bytes)
{
    G00 *g = read_g00(name);
    if (!g) return NULL;
    if (g->ncut < 0 || (g->ncut > 0 && !g->cut)) { g00_free(g); return NULL; }
    ImgEntry *e = entry_new(g->ncut);
    e->g00 = g;
    for (int i = 0; i < g->ncut; i++) {
        G00Cut *c = &g->cut[i];
        if (!c->px || c->w <= 0 || c->h <= 0) continue;
        size_t np = (size_t)c->w * (size_t)c->h;
        for (size_t k = 0; k < np; k++) c->px[k] = px_premul(c->px[k]);
        e->cut[i] = (Image){ c->px, c->w, c->h, c->cx, c->cy };
        *bytes += np * sizeof(uint32_t);
    }
    return e;
}

typedef struct Parts { ImgEntry *e[IMG_PARTS_MAX]; size_t bytes; int n; } Parts;

static void parts_free(Parts *p)
{
    for (int k = 0; k < p->n; k++) entry_free(p->e[k]);
    p->n = 0;
}

/* Decode the parts of "a|b|..." (empty and missing parts skipped, at most IMG_PARTS_MAX loaded). */
static void parts_load(const char *key, Parts *p)
{
    memset(p, 0, sizeof *p);
    Str rest = str_c(key), part;
    while (p->n < IMG_PARTS_MAX && str_split(&rest, '|', &part)) {
        char name[CACHE_KEY_MAX + 1];
        if (!part.n || !str_copy(name, sizeof name, part)) continue;   /* part of a cache key: always fits */
        ImgEntry *e = load_single(name, &p->bytes);
        if (e) p->e[p->n++] = e;
    }
}

static const Image *part_cut(const Parts *p, int k, int i)
{
    const ImgEntry *e = p->e[k];
    return (i < e->ncut && e->cut[i].px) ? &e->cut[i] : NULL;
}

/* Union size of cut i over all parts. */
static void union_size(const Parts *p, int i, int *w, int *h)
{
    *w = *h = 0;
    for (int k = 0; k < p->n; k++) {
        const Image *im = part_cut(p, k, i);
        if (!im) continue;
        if (im->w > *w) *w = im->w;
        if (im->h > *h) *h = im->h;
    }
}

/* Pixels of all union canvases, false when the total does not fit the cache budget. */
static bool union_pixels(const Parts *p, int ncut, size_t *total)
{
    *total = 0;
    for (int i = 0; i < ncut; i++) {
        int w, h;
        union_size(p, i, &w, &h);
        size_t np;
        if (!core_mul_sz((size_t)w, (size_t)h, &np) || !core_add_sz(*total, np, total)) return false;
    }
    return *total <= IMG_BUDGET / sizeof(uint32_t);
}

/* "a|b|..." composite (GFX §3.4, gallery only): parts alpha-blended in order onto the union canvas. */
static ImgEntry *load_composite(const char *key, size_t *bytes)
{
    Parts p;
    parts_load(key, &p);
    size_t total = 0;
    if (!p.n || !union_pixels(&p, p.e[0]->ncut, &total)) { parts_free(&p); return NULL; }
    ImgEntry *e = entry_new(p.e[0]->ncut);
    e->own = mem_alloc_array(total ? total : 1, sizeof *e->own, MEM_IMG);   /* zeroed = transparent */
    size_t at = 0;
    for (int i = 0; i < e->ncut; i++) {
        Canvas cv = { e->own + at, 0, 0, 0 };
        union_size(&p, i, &cv.w, &cv.h);
        cv.pitch = cv.w;
        for (int k = 0; k < p.n; k++) {
            const Image *im = part_cut(&p, k, i);
            if (im) canvas_blit(&cv, im, rect_make(0, 0, im->w, im->h), 0, 0, 255, NULL);
        }
        if (cv.w && cv.h) e->cut[i] = (Image){ cv.px, cv.w, cv.h, 0, 0 };
        at += (size_t)cv.w * (size_t)cv.h;
    }
    *bytes = total * sizeof(uint32_t);
    parts_free(&p);
    return e;
}

/* Hand e to the cache. The table refuses only when all IMG_MAX_ENTRIES entries were used this frame; e is then
 * kept in the spill list until the next trim, or released (and the image drawn as missing) if that is full. */
static ImgEntry *img_store(const char *key, ImgEntry *e, size_t bytes)
{
    if (cache_put(&g_cache, key, e, bytes)) return e;
    if (g_nspill < IMG_SPILL_MAX) { g_spill[g_nspill++] = e; return e; }
    static bool once;
    if (!once) { once = true; log_msg(LOG_WARN, "gfx: image cache full this frame, '%s' not drawn", key); }
    entry_free(e);
    return NULL;
}

static ImgEntry *img_entry(const char *name)
{
    if (!name || !*name) return NULL;
    SB_ON_STACK(key, CACHE_KEY_MAX + 1);
    if (!make_key(name, &key)) {
        static bool once;
        if (!once) { once = true; log_msg(LOG_WARN, "gfx: image name longer than %d bytes ignored", CACHE_KEY_MAX); }
        return NULL;
    }
    cache_ready();
    ImgEntry *e = cache_get(&g_cache, sb_cstr(&key));
    if (!e) {
        size_t bytes = 0;
        e = strchr(sb_cstr(&key), '|') ? load_composite(sb_cstr(&key), &bytes) : load_single(sb_cstr(&key), &bytes);
        if (!e) {
            log_msg(LOG_WARN, "gfx: image '%s' not found", sb_cstr(&key));
            e = entry_new(0);
            e->ncut = -1;
        }
        e = img_store(sb_cstr(&key), e, bytes);
    }
    return (e && e->ncut >= 0) ? e : NULL;
}

/* ---------------------------------------------------------------- API */

const Image *img_get(const char *name, int patno)
{
    const ImgEntry *e = img_entry(name);
    if (!e || patno < 0 || patno >= e->ncut || !e->cut[patno].px) return NULL;
    return &e->cut[patno];
}

const Image *img_get_ws(const WStr *name, int patno)
{
    if (!name || name->len <= 0) return NULL;
    Scratch sc = mem_scratch_begin(NULL);
    const Image *im = img_get(ws_utf8_arena(sc.arena, name, WS_UTF8_SHORT_MAX), patno);
    mem_scratch_end(&sc);
    return im;
}

int img_pat_count(const char *name) { const ImgEntry *e = img_entry(name); return e ? e->ncut : 0; }
bool img_exists(const char *name) { return img_entry(name) != NULL; }

void img_cache_trim(void)
{
    if (g_ready) cache_trim(&g_cache);
    spill_release();
}

void img_cache_clear(void)
{
    if (g_ready) cache_clear(&g_cache);
    spill_release();
}

void img_cache_shutdown(void)
{
    if (g_ready) cache_destroy(&g_cache);
    g_ready = false;
    spill_release();
}
