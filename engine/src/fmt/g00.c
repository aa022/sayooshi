/* fmt/g00.c — g00 decoder (AST §1; exe FUN_006c5860 parse, FUN_006c5960 cut, FUN_006c5af0 chunk,
 * FUN_006c5b70 pixels, FUN_0066ef90 cut composition). Files are not encrypted; pixels are BGRA straight alpha.
 *
 *   type 0: u8 0, u16 w, u16 h, pixel LZSS32 stream (one opaque cut)
 *   type 1: u8 1, u16 w, u16 h, byte LZSS of { u16 npal, npal x BGRA, w*h palette indices }
 *   type 2: u8 2, u16 w, u16 h, u32 nreg, nreg x 24-byte regions (skipped like the exe), byte LZSS of
 *           { u32 count, count x {u32 offset, u32 size}, cuts }; a cut is a 0x74-byte header and chunks of a
 *           0x5c-byte header + kw*kh BGRA pixels, drawn onto a transparent canvas of the cut's size.
 *
 * Memory: the whole result — the G00, its cuts and every pixel — is ONE tracked heap block (MEM_IMG), sized
 * exactly after a validating first pass, so g00_free is a single mem_free and a failed decode leaves nothing.
 * Decompressed intermediate data is scratch memory of the call (main thread). Decoded sizes are capped by
 * G00_MAX_PIXELS, so a tiny hostile file cannot request a huge canvas. */
#include "fmt/g00.h"
#include "core/bin.h"
#include "core/mem.h"
#include "fmt/lzss.h"
#include <stddef.h>
#include <string.h>

enum {
    G00_HEADER = 5,
    T0_SIZE_AT = 9,                      /* type 0: u32 unpacked size after the LZSS packed size */
    T2_REGIONS_AT = 5, T2_REGION = 24,
    T2_INDEX = 8,
    CUT_HEADER = 0x74, CHUNK_HEADER = 0x5c,
    CUT_DIM_MAX = 8192,
    OPAQUE = 0xFFu
};
#define G00_MAX_PIXELS ((size_t)1 << 24)  /* per file, all cuts: 64 MiB (the largest game file has 640x480 = 307200) */

/* ------------------------------------------------------------------------------------------------ block */

/* The single allocation behind a G00: the struct, its cuts, then the pixels (G00Cut's alignment covers uint32_t). */
typedef struct { G00 g; G00Cut cut[]; } G00Block;
CORE_STATIC_ASSERT(_Alignof(G00Cut) >= _Alignof(uint32_t) && sizeof(G00Cut) % _Alignof(uint32_t) == 0,
                   "pixels may follow the cut array");

/* A block for ncut cuts (zeroed) and npx pixels (uninitialised); NULL over G00_MAX_PIXELS. */
static G00 *g00_block(int type, uint32_t ncut, size_t npx, int w, int h, uint32_t **pixels)
{
    size_t head, total, slots = ncut ? ncut : 1;
    bool ok = npx <= G00_MAX_PIXELS && core_mul_sz(slots, sizeof(G00Cut), &head) &&
              core_add_sz(head, offsetof(G00Block, cut), &head) && core_add_sz(head, npx * sizeof(uint32_t), &total);
    if (!ok) return NULL;
    G00Block *b = mem_alloc(total, MEM_IMG);
    memset(b, 0, head);
    b->g.type = type;
    b->g.ncut = (int)ncut;
    b->g.cut = b->cut;
    b->g.sheet_w = w;
    b->g.sheet_h = h;
    *pixels = (uint32_t *)(b->cut + slots);
    return &b->g;                                             /* first member: the block's address */
}

void g00_free(G00 *g) { mem_free(g); }

static void whole_cut(G00Cut *c, int w, int h, uint32_t *px)
{
    c->w = c->x2 = w;
    c->h = c->y2 = h;
    c->nchunk = 1;
    c->px = px;
}

/* ------------------------------------------------------------------------------------------------ type 0 / 1 */

static G00 *decode_type0(const uint8_t *data, size_t n, int w, int h)
{
    Rd r = rd_make(data, n);
    uint64_t want = (uint64_t)w * (uint64_t)h * 4;
    if (!rd_seek(&r, T0_SIZE_AT) || rd_u32le(&r) != want) return NULL;
    uint32_t *px;
    G00 *g = g00_block(0, 1, (size_t)w * (size_t)h, w, h, &px);
    if (!g) return NULL;
    if (!lzss_decode32(data + G00_HEADER, n - G00_HEADER, (uint8_t *)px, (uint32_t)want)) { mem_free(g); return NULL; }
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) px[i] = bin_load_le32((const uint8_t *)&px[i]); /* BGRA bytes */
    whole_cut(&g->cut[0], w, h, px);
    return g;
}

static G00 *decode_type1(const uint8_t *data, size_t n, int w, int h)
{
    Scratch s = mem_scratch_begin(NULL);
    uint32_t len = 0;
    const uint8_t *b = lzss_decode_arena(s.arena, data + G00_HEADER, n - G00_HEADER, &len);
    Rd r = rd_make(b, len);
    uint16_t npal = rd_u16le(&r);
    const uint8_t *pal = rd_view(&r, (size_t)npal * 4);
    const uint8_t *idx = rd_view(&r, (size_t)w * (size_t)h);
    uint32_t *px;
    G00 *g = b && rd_ok(&r) ? g00_block(1, 1, (size_t)w * (size_t)h, w, h, &px) : NULL;
    if (g) {
        G00Cut *c = &g->cut[0];
        whole_cut(c, w, h, px);
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
            uint32_t v = idx[i] < npal ? bin_load_le32(pal + 4 * (size_t)idx[i]) : 0;
            if ((v >> 24) != OPAQUE) c->has_alpha = true;
            px[i] = v;
        }
    }
    mem_scratch_end(&s);
    return g;
}

/* ------------------------------------------------------------------------------------------------ type 2 */

typedef struct { Rd body; int32_t x, y, w, h, cx, cy, cw, ch; int nchunk; } CutHeader;

/* Cut i of the index: false (cut stays empty) for an empty entry; *bad on a corrupt one. */
static bool cut_header(const Rd *blk, uint32_t i, CutHeader *ch, bool *bad)
{
    Rd e = rd_sub_at(blk, 4 + (size_t)i * T2_INDEX, T2_INDEX);
    uint32_t off = rd_u32le(&e), size = rd_u32le(&e);
    *bad = !rd_ok(&e);
    if (*bad || !off || !size) return false;                 /* empty entry -> zero cut (6c591f) */
    Rd c = rd_sub_at(blk, off, size);
    (void)rd_u16le(&c);                                       /* not used by the exe either */
    ch->nchunk = rd_u16le(&c);
    ch->x = rd_i32le(&c); ch->y = rd_i32le(&c); ch->w = rd_i32le(&c); ch->h = rd_i32le(&c);
    ch->cx = rd_i32le(&c); ch->cy = rd_i32le(&c); ch->cw = rd_i32le(&c); ch->ch = rd_i32le(&c);
    *bad = size < CUT_HEADER || !rd_ok(&c) || ch->cw < 0 || ch->ch < 0 || ch->cw > CUT_DIM_MAX || ch->ch > CUT_DIM_MAX;
    ch->body = rd_sub_at(blk, (size_t)off + CUT_HEADER, size >= CUT_HEADER ? size - CUT_HEADER : 0);
    return !*bad;
}

/* Draws one chunk (header + kw*kh BGRA) onto the cut canvas, clipped. false on truncated data. */
static bool draw_chunk(G00Cut *c, Rd *body)
{
    Rd hd = rd_sub(body, CHUNK_HEADER);
    int kx = rd_u16le(&hd), ky = rd_u16le(&hd);
    uint8_t flag = rd_u8(&hd);
    (void)rd_u8(&hd);
    int kw = rd_u16le(&hd), kh = rd_u16le(&hd);
    const uint8_t *src = rd_view(body, (size_t)kw * (size_t)kh * 4);
    if (!rd_ok(&hd) || !src) return false;
    if (flag) c->has_alpha = true;
    for (int row = 0; row < kh; row++) {
        int yy = ky + row;
        if (yy >= c->h) break;
        int x0 = kx < c->w ? kx : c->w, ncol = kw < c->w - x0 ? kw : c->w - x0;
        for (int col = 0; col < ncol; col++)
            c->px[(size_t)yy * (size_t)c->w + (size_t)(x0 + col)] = bin_load_le32(src + ((size_t)row * kw + col) * 4);
    }
    return true;
}

static bool fill_cut(G00Cut *c, CutHeader *h, uint32_t *px)
{
    c->cx = h->cx; c->cy = h->cy;
    c->w = h->cw; c->h = h->ch;
    c->x1 = h->x; c->y1 = h->y;
    c->x2 = core_wrap_add_i32(h->x, h->w);           /* two's-complement wrap like the exe */
    c->y2 = core_wrap_add_i32(h->y, h->h);
    c->nchunk = h->nchunk;
    c->px = px;
    memset(px, 0, (size_t)h->cw * (size_t)h->ch * sizeof *px);   /* transparent black canvas */
    for (int k = 0; k < h->nchunk; k++)
        if (!draw_chunk(c, &h->body)) return false;
    return true;
}

/* Pass 1: validates every cut header and sums the canvas pixels. */
static bool count_pixels(const Rd *blk, uint32_t cnt, size_t *npx)
{
    *npx = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        CutHeader h;
        bool bad;
        if (!cut_header(blk, i, &h, &bad)) { if (bad) return false; continue; }
        *npx += (size_t)h.cw * (size_t)h.ch;                  /* <= cnt * 2^26: no overflow before the cap */
        if (*npx > G00_MAX_PIXELS) return false;
    }
    return true;
}

static G00 *build_type2(const Rd *blk, uint32_t cnt, int w, int h)
{
    size_t npx;
    uint32_t *px;
    G00 *g = count_pixels(blk, cnt, &npx) ? g00_block(2, cnt, npx, w, h, &px) : NULL;
    for (uint32_t i = 0; g && i < cnt; i++) {
        CutHeader ch;
        bool bad;
        if (!cut_header(blk, i, &ch, &bad)) continue;
        if (!fill_cut(&g->cut[i], &ch, px)) { mem_free(g); g = NULL; break; }
        px += (size_t)ch.cw * (size_t)ch.ch;
    }
    return g;
}

static G00 *decode_type2(const uint8_t *data, size_t n, int w, int h)
{
    Rd r = rd_make(data, n);
    (void)rd_seek(&r, T2_REGIONS_AT);
    uint32_t nreg = rd_count(&r, UINT32_MAX, T2_REGION);
    if (!rd_ok(&r)) return NULL;
    size_t lz = r.pos + (size_t)nreg * T2_REGION;             /* <= n: rd_count checked the room */
    Scratch s = mem_scratch_begin(NULL);
    uint32_t len = 0;
    const uint8_t *b = lzss_decode_arena(s.arena, data + lz, n - lz, &len);
    Rd blk = rd_make(b, len);
    uint32_t cnt = rd_count(&blk, UINT32_MAX, T2_INDEX);
    G00 *g = b && rd_ok(&blk) ? build_type2(&blk, cnt, w, h) : NULL;
    mem_scratch_end(&s);
    return g;
}

G00 *g00_decode(const uint8_t *data, size_t n)
{
    Rd r = rd_make(data, n);
    uint8_t type = rd_u8(&r);
    int w = rd_u16le(&r), h = rd_u16le(&r);
    if (!data || !rd_ok(&r)) return NULL;
    switch (type) {
    case 0: return decode_type0(data, n, w, h);
    case 1: return decode_type1(data, n, w, h);
    case 2: return decode_type2(data, n, w, h);
    default: return NULL;
    }
}
