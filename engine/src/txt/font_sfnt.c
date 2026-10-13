/* txt/font_sfnt.c — structural validation of TrueType/OpenType files before stb_truetype sees them (OpenType spec:
 * table directory, head, hhea, maxp, hmtx, loca/glyf).
 *
 * Two layers keep a hostile font from crashing the engine: this check rejects files whose tables or glyph index do
 * not fit, and the patched stb_truetype bounds every byte it reads to the registered buffer (stbtt_SetDataBounds,
 * "sayooshi patch" in third_party/stb/stb_truetype.h), which covers the offsets inside cmap, kern, GPOS, CFF and the
 * glyph programs. Pure function of the bytes (no allocation), so it is fuzzed directly (`make fuzz`, target font). */
#include "txt/font.h"
#include "core/bin.h"

enum {
    FONT_FILE_MAX = 256 << 20,
    SFNT_TABLES_MAX = 1024,
    SFNT_HEADER = 12,
    SFNT_RECORD = 16,
    UPEM_MIN = 16, UPEM_MAX = 16384,                 /* OpenType 'head'.unitsPerEm range */
    HEAD_UPEM = 18, HEAD_LOCA_FORMAT = 50,           /* field offsets */
    HHEA_METRICS = 34, MAXP_GLYPHS = 4,
};

#define TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

typedef struct { uint32_t off, len; bool found; } SfntTable;
typedef struct { SfntTable head, hhea, maxp, cmap, hmtx, loca, glyf, cff; } SfntDir;

/* Offset of face 0 like stbtt_GetFontOffsetForIndex(data, 0), but bounds-checked; -1 if not a font. */
static long sfnt_offset(const uint8_t *d, size_t n)
{
    Rd r = rd_make(d, n);
    uint32_t tag = rd_u32be(&r);
    if (tag == TAG('t', 't', 'c', 'f')) {
        uint32_t ver = rd_u32be(&r), count = rd_u32be(&r), ofs = rd_u32be(&r);
        bool ok = rd_ok(&r) && (ver == 0x00010000u || ver == 0x00020000u) && count >= 1 && ofs < n;
        return ok ? (long)ofs : -1;
    }
    bool sfnt = tag == TAG('1', 0, 0, 0) || tag == TAG('t', 'y', 'p', '1') || tag == TAG('O', 'T', 'T', 'O') ||
                tag == 0x00010000u || tag == TAG('t', 'r', 'u', 'e');
    return rd_ok(&r) && sfnt ? 0 : -1;
}

/* Minimum length of the fixed-size tables stbtt_InitFont and the metrics calls read. */
static uint32_t table_min_len(uint32_t tag)
{
    switch (tag) {
    case TAG('h', 'e', 'a', 'd'): return 54;
    case TAG('h', 'h', 'e', 'a'): return 36;
    case TAG('m', 'a', 'x', 'p'): return 6;
    case TAG('c', 'm', 'a', 'p'): return 4;
    default:                      return 0;
    }
}

static SfntTable *dir_slot(SfntDir *t, uint32_t tag)
{
    switch (tag) {
    case TAG('h', 'e', 'a', 'd'): return &t->head;
    case TAG('h', 'h', 'e', 'a'): return &t->hhea;
    case TAG('m', 'a', 'x', 'p'): return &t->maxp;
    case TAG('c', 'm', 'a', 'p'): return &t->cmap;
    case TAG('h', 'm', 't', 'x'): return &t->hmtx;
    case TAG('l', 'o', 'c', 'a'): return &t->loca;
    case TAG('g', 'l', 'y', 'f'): return &t->glyf;
    case TAG('C', 'F', 'F', ' '): return &t->cff;
    default:                      return NULL;
    }
}

/* The table directory at ofs: every table lies inside the file and the fixed-size ones are long enough. The first
 * record of a tag wins, as in stb's linear search. */
static bool read_dir(const uint8_t *d, size_t n, long ofs, SfntDir *t)
{
    *t = (SfntDir){ 0 };
    Rd r = rd_make(d, n);
    if (ofs < 0 || !rd_seek(&r, (size_t)ofs) || rd_left(&r) < SFNT_HEADER) return false;
    (void)rd_u32be(&r);                                   /* sfnt version (checked by sfnt_offset / stb) */
    uint16_t ntables = rd_u16be(&r);
    (void)rd_skip(&r, 6);                                /* searchRange, entrySelector, rangeShift */
    if (ntables == 0 || ntables > SFNT_TABLES_MAX || rd_left(&r) < (size_t)ntables * SFNT_RECORD) return false;
    for (uint16_t i = 0; i < ntables; i++) {
        uint32_t tag = rd_u32be(&r);
        (void)rd_u32be(&r);                               /* checksum */
        uint32_t off = rd_u32be(&r), len = rd_u32be(&r);
        if (off > n || len > n - off || len < table_min_len(tag)) return false;
        SfntTable *s = dir_slot(t, tag);
        if (s && !s->found) *s = (SfntTable){ off, len, true };
    }
    return rd_ok(&r);
}

static uint16_t u16_at(const uint8_t *d, size_t n, const SfntTable *t, uint32_t at)
{
    Rd all = rd_make(d, n);
    Rd r = rd_sub_at(&all, t->off, t->len);
    return rd_seek(&r, at) ? rd_u16be(&r) : 0;
}

/* loca: numGlyphs + 1 entries that never decrease and stay inside glyf (stb trusts them). */
static bool loca_valid(const uint8_t *d, size_t n, const SfntDir *t, int format, uint32_t glyphs)
{
    size_t each = format ? 4 : 2;
    if (!t->loca.found || !t->glyf.found || (size_t)t->loca.len / each < (size_t)glyphs + 1) return false;
    Rd all = rd_make(d, n);
    Rd r = rd_sub_at(&all, t->loca.off, t->loca.len);
    uint32_t prev = 0;
    for (uint32_t g = 0; g <= glyphs; g++) {
        uint32_t at = format ? rd_u32be(&r) : (uint32_t)rd_u16be(&r) * 2u;
        if (at < prev || at > t->glyf.len) return false;
        prev = at;
    }
    return rd_ok(&r);
}

/* head/hhea/maxp/hmtx fields stb divides by or indexes with, and the glyph index of TrueType outlines. */
static bool tables_valid(const uint8_t *d, size_t n, const SfntDir *t)
{
    if (!t->head.found || !t->hhea.found || !t->maxp.found || !t->cmap.found) return false;
    uint16_t upem = u16_at(d, n, &t->head, HEAD_UPEM);
    uint16_t loca_format = u16_at(d, n, &t->head, HEAD_LOCA_FORMAT);
    uint16_t metrics = u16_at(d, n, &t->hhea, HHEA_METRICS);
    uint16_t glyphs = u16_at(d, n, &t->maxp, MAXP_GLYPHS);
    if (upem < UPEM_MIN || upem > UPEM_MAX || metrics == 0) return false;
    if (!t->hmtx.found || t->hmtx.len / 4u < metrics) return false;
    if (t->cff.found) return true;                       /* CFF outlines: read through stb's bounded buffer */
    return loca_format <= 1 && loca_valid(d, n, t, loca_format, glyphs);
}

long font_sfnt_offset(const uint8_t *d, size_t n)
{
    return d && n >= SFNT_HEADER && n <= FONT_FILE_MAX ? sfnt_offset(d, n) : -1;
}

bool font_file_valid(const uint8_t *d, size_t n)
{
    SfntDir t;
    long ofs = font_sfnt_offset(d, n);
    return read_dir(d, n, ofs, &t) && tables_valid(d, n, &t);
}
