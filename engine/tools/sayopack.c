/* tools/sayopack.c — host tool for the self-contained build (docs/PAYLOAD.md). C11 + stdio, no SDL.
 *
 *   sayopack create OUT.pak ROOT LIST     pack the files named in LIST (one path per line, relative to ROOT)
 *   sayopack append EXE PAK OUT           OUT = EXE (minus any old payload) + PAK
 *   sayopack strip  EXE OUT               OUT = EXE without its payload
 *   sayopack list   FILE                  print the table of a .pak or of an executable with a payload
 *   sayopack verify FILE                  check every entry's CRC-32 (exit 1 on damage)
 *   sayopack icns   ICO OUT.icns          macOS icon (PNG-based icns) from the largest image in a Windows .ico
 *   sayopack png    ICO OUT.png           the largest image of a .ico as PNG
 *   sayopack cembed FILE OUT.c NAME       FILE as a C array `const unsigned char NAME[]` plus `NAME_size`
 *
 * The archive format is read and written by fmt/sayopak.c, the codec the engine uses, through core/bin.c (Rd/Wr).
 * The tool has no engine allocator: the three core/mem.h hooks that bin.c needs are defined here on top of libc,
 * and every other buffer goes through xalloc/xrealloc. Any error stops the tool with a message (exit 1); output
 * files are written to OUT.tmp and renamed into place, so a failed run never leaves a half-written OUT. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L          /* fseeko / ftello */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/bin.h"
#include "core/hash.h"
#include "core/mem.h"
#include "fmt/sayopak.h"

#if defined(_WIN32)
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#define fseek64 fseeko
#define ftell64 ftello
#endif

enum {
    COPY_CHUNK = 1 << 20,
    ICO_DIR = 6, ICO_ENTRY = 16,
    DIB_HEADER_MIN = 40,
    ICON_DIM_MAX = 1024,
    MAX_INPUT = 64 << 20                 /* .ico and list files */
};

static const char *g_prog = "sayopack";

static _Noreturn void die(const char *fmt, const char *a)
{
    (void)fprintf(stderr, "%s: ", g_prog);
    (void)fprintf(stderr, fmt, a ? a : "");
    (void)fputc('\n', stderr);
    exit(1);
}

/* ------------------------------------------------------------------------------------------------ memory */

static void *xalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory%s", NULL);
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory%s", NULL);
    return q;
}

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBIW_MALLOC(sz) xalloc(sz)                  /* never NULL, size 0 allowed */
#define STBIW_REALLOC(p, sz) xrealloc((p), (sz))
#define STBIW_FREE(p) free(p)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* core/mem.h hooks used by core/bin.c (Wr on the heap). */
void *mem_alloc_at(size_t size, MemTag tag, const char *file, int line)
{
    (void)tag; (void)file; (void)line;
    return xalloc(size);
}

void *mem_realloc_at(void *p, size_t size, const char *file, int line)
{
    (void)file; (void)line;
    return xrealloc(p, size);
}

void mem_free(void *p) { free(p); }

/* "a" + "b" + "c" in a new exact-size buffer. */
static char *concat3(const char *a, const char *b, const char *c)
{
    size_t n = strlen(a) + strlen(b) + strlen(c) + 1;
    char *s = xalloc(n);
    int k = snprintf(s, n, "%s%s%s", a, b, c);
    if (k < 0 || (size_t)k != n - 1) die("cannot build a path from %s", a);
    return s;
}

/* ------------------------------------------------------------------------------------------------ files */

static FILE *xopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (!f) die("cannot open %s", path);
    return f;
}

static void xwrite(FILE *f, const void *p, size_t n, const char *what)
{
    if (n && fwrite(p, 1, n, f) != n) die("write error on %s", what);
}

static void xclose(FILE *f, const char *what)
{
    if (fclose(f) != 0) die("cannot finish %s", what);
}

static void xseek(FILE *f, uint64_t off, const char *what)
{
    if (off > (uint64_t)INT64_MAX || fseek64(f, (int64_t)off, SEEK_SET) != 0) die("cannot seek in %s", what);
}

static uint64_t file_size(FILE *f, const char *what)
{
    if (fseek64(f, 0, SEEK_END) != 0) die("cannot seek in %s", what);
    int64_t n = (int64_t)ftell64(f);
    if (n < 0) die("cannot get the size of %s", what);
    xseek(f, 0, what);
    return (uint64_t)n;
}

static void commit(const char *tmp, const char *out)
{
    (void)remove(out);                                        /* may not exist */
    if (rename(tmp, out) != 0) die("cannot rename to %s", out);
}

static uint8_t g_buf[COPY_CHUNK];

/* copy n bytes from in (current position) to out (may be NULL), updating *crc if non-NULL */
static void copy_bytes(FILE *in, FILE *out, uint64_t n, uint32_t *crc, const char *what)
{
    while (n) {
        size_t want = n > sizeof g_buf ? sizeof g_buf : (size_t)n;
        size_t r = fread(g_buf, 1, want, in);
        if (r != want) die("read error on %s", what);
        if (crc) *crc = hash_crc32(*crc, g_buf, r);
        if (out) xwrite(out, g_buf, r, what);
        n -= r;
    }
}

/* Whole file (1 .. MAX_INPUT bytes) in a new buffer. */
static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = xopen(path, "rb");
    uint64_t n = file_size(f, path);
    if (n == 0 || n > MAX_INPUT) die("bad size: %s", path);
    uint8_t *d = xalloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) die("cannot read %s", path);
    xclose(f, path);
    *len = (size_t)n;
    return d;
}

/* ------------------------------------------------------------------------------------------------ payload */

typedef struct { uint64_t base, fsize; SayopakInfo info; } Payload;

/* false if f has no payload; stops on a damaged one */
static bool find_payload(FILE *f, Payload *p, const char *what)
{
    uint8_t tr[SAYOPAK_TRAILER_SIZE], hd[SAYOPAK_HEADER_SIZE];
    p->fsize = file_size(f, what);
    if (p->fsize < SAYOPAK_HEADER_SIZE + SAYOPAK_TRAILER_SIZE) return false;
    xseek(f, p->fsize - SAYOPAK_TRAILER_SIZE, what);
    if (fread(tr, 1, sizeof tr, f) != sizeof tr) return false;
    SayopakStatus st = sayopak_read_trailer(tr, p->fsize, &p->info);
    if (st == SAYOPAK_ABSENT) return false;
    if (st == SAYOPAK_DAMAGED) die("unsupported or damaged SAYOPAK trailer%s", NULL);
    p->base = p->fsize - p->info.payload_size;
    xseek(f, p->base, what);
    if (fread(hd, 1, sizeof hd, f) != sizeof hd || !sayopak_check_header(hd)) die("damaged SAYOPAK header in %s", what);
    return true;
}

/* One list line -> archive path in place ('\\' -> '/', leading "./" removed); NULL for blank/comment lines. */
static char *list_path(char *line, size_t n)
{
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = 0;
    char *rel = line;
    while (rel[0] == '.' && rel[1] == '/') rel += 2;
    if (!*rel || rel[0] == '#') return NULL;
    for (char *q = rel; *q; q++) if (*q == '\\') *q = '/';
    size_t len = strlen(rel);
    if (len >= SAYOPAK_MAX_PATH || len != (size_t)(line + n - rel) || !sayopak_path_safe(rel, len))
        die("bad path in list: %s", rel);
    return rel;
}

typedef struct { FILE *out; const char *tmp, *root; uint64_t pos, total; uint32_t count; Wr table; } Packer;

/* Appends one file (16-byte aligned) and its table entry. */
static void pack_file(Packer *pk, const char *rel)
{
    static const uint8_t zero[SAYOPAK_ALIGN];
    char *src = concat3(pk->root, "/", rel);
    FILE *in = xopen(src, "rb");
    uint64_t size = file_size(in, src);
    size_t pad = (size_t)((SAYOPAK_ALIGN - pk->pos % SAYOPAK_ALIGN) % SAYOPAK_ALIGN);
    xwrite(pk->out, zero, pad, pk->tmp);
    pk->pos += pad;
    SayopakEntry e = { pk->pos, size, 0, rel, strlen(rel) };
    copy_bytes(in, pk->out, size, &e.crc, src);
    xclose(in, src);
    sayopak_put_entry(&pk->table, &e);
    if (!wr_ok(&pk->table)) die("file table too large at %s", rel);
    pk->pos += size;
    pk->total += size;
    if (++pk->count > SAYOPAK_MAX_FILES) die("too many files%s", NULL);
    free(src);
}

static void pack_finish(Packer *pk)
{
    SayopakInfo info = { pk->count, hash_crc32(0, wr_data(&pk->table), wr_len(&pk->table)), pk->pos,
                         wr_len(&pk->table), 0 };
    xwrite(pk->out, wr_data(&pk->table), wr_len(&pk->table), pk->tmp);
    pk->pos += wr_len(&pk->table);
    info.payload_size = pk->pos + SAYOPAK_TRAILER_SIZE;
    uint8_t tr[SAYOPAK_TRAILER_SIZE];
    sayopak_put_trailer(tr, &info);
    xwrite(pk->out, tr, sizeof tr, pk->tmp);
    pk->pos += SAYOPAK_TRAILER_SIZE;
}

static int cmd_create(const char *out, const char *root, const char *listfile)
{
    size_t ln;
    char *list = (char *)read_file(listfile, &ln);
    list = xrealloc(list, ln + 1);
    list[ln] = 0;
    char *tmp = concat3(out, ".tmp", "");
    Packer pk = { xopen(tmp, "wb"), tmp, root, SAYOPAK_HEADER_SIZE, 0, 0, { NULL, 0, 0, 0, false, false } };
    wr_init_heap(&pk.table, SAYOPAK_MAX_TABLE);
    uint8_t hd[SAYOPAK_HEADER_SIZE];
    sayopak_put_header(hd);
    xwrite(pk.out, hd, sizeof hd, tmp);
    for (char *line = list; line < list + ln;) {              /* each pass consumes one line */
        char *nl = memchr(line, '\n', (size_t)(list + ln - line));
        size_t n = nl ? (size_t)(nl - line) : (size_t)(list + ln - line);
        line[n] = 0;
        const char *rel = list_path(line, n);
        if (rel) pack_file(&pk, rel);
        line += n + 1;
    }
    if (!pk.count) die("no files listed in %s", listfile);
    pack_finish(&pk);
    xclose(pk.out, tmp);
    commit(tmp, out);
    printf("%s: %u files, %.1f MiB of data, %.1f MiB payload\n", out, pk.count, (double)pk.total / 1048576.0,
           (double)pk.pos / 1048576.0);
    wr_free(&pk.table);
    free(list);
    free(tmp);
    return 0;
}

/* OUT = EXE without payload (+ PAK) */
static int cmd_append(const char *exe, const char *pak, const char *out)
{
    FILE *e = xopen(exe, "rb");
    Payload p;
    uint64_t keep = find_payload(e, &p, exe) ? p.base : p.fsize;
    xseek(e, 0, exe);
    char *tmp = concat3(out, ".tmp", "");
    FILE *o = xopen(tmp, "wb");
    copy_bytes(e, o, keep, NULL, exe);
    xclose(e, exe);
    if (pak) {
        FILE *k = xopen(pak, "rb");
        Payload kp;
        if (!find_payload(k, &kp, pak) || kp.base != 0) die("%s is not a SAYOPAK archive", pak);
        xseek(k, 0, pak);
        copy_bytes(k, o, kp.info.payload_size, NULL, pak);
        xclose(k, pak);
    }
    xclose(o, tmp);
    commit(tmp, out);
    free(tmp);
    return 0;
}

static uint8_t *read_table(FILE *f, const Payload *p, const char *what)
{
    size_t n = (size_t)p->info.table_size;                    /* <= SAYOPAK_MAX_TABLE */
    uint8_t *tab = xalloc(n);
    xseek(f, p->base + p->info.table_offset, what);
    if (fread(tab, 1, n, f) != n) die("cannot read the file table%s", NULL);
    if (hash_crc32(0, tab, n) != p->info.table_crc) die("file table CRC mismatch%s", NULL);
    return tab;
}

static int cmd_list_verify(const char *file, int verify)
{
    FILE *f = xopen(file, "rb");
    Payload p;
    if (!find_payload(f, &p, file)) die("%s has no SAYOPAK payload", file);
    uint8_t *tab = read_table(f, &p, file);
    Rd table = rd_make(tab, (size_t)p.info.table_size);
    int bad = 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < p.info.count; i++) {
        SayopakEntry e;
        if (!sayopak_next_entry(&table, &p.info, &e)) die("damaged table%s", NULL);
        total += e.size;
        if (verify) {
            uint32_t c = 0;
            xseek(f, p.base + e.offset, file);
            copy_bytes(f, NULL, e.size, &c, file);
            if (c != e.crc) {
                printf("BAD  %.*s (crc %08x, expected %08x)\n", (int)e.path_len, e.path, (unsigned)c, (unsigned)e.crc);
                bad++;
            }
        } else {
            printf("%12llu  %08x  %.*s\n", (unsigned long long)e.size, (unsigned)e.crc, (int)e.path_len, e.path);
        }
    }
    printf("%s: payload at offset %llu, %llu bytes, %u files, %llu data bytes%s\n", file,
           (unsigned long long)p.base, (unsigned long long)p.info.payload_size, (unsigned)p.info.count,
           (unsigned long long)total, verify ? (bad ? ", VERIFY FAILED" : ", all CRCs ok") : "");
    free(tab);
    xclose(f, file);
    return bad ? 1 : 0;
}

/* ------------------------------------------------------------------------------------------------ icons */

typedef struct { int w, h; uint8_t *rgba; } Image;
typedef struct { const uint8_t *p; size_t n; int w; } IcoImage;   /* one directory entry's bytes */

/* The largest image of an .ico (by width). */
static IcoImage ico_largest(const uint8_t *d, size_t n, const char *path)
{
    Rd r = rd_make(d, n);
    uint16_t reserved = rd_u16le(&r), type = rd_u16le(&r), cnt = rd_u16le(&r);
    if (!rd_ok(&r) || reserved || type != 1) die("%s is not a .ico file", path);
    IcoImage best = { NULL, 0, 0 };
    for (uint16_t i = 0; i < cnt; i++) {
        Rd e = rd_sub(&r, ICO_ENTRY);
        int w = rd_u8(&e);
        (void)rd_skip(&e, 7);
        uint32_t sz = rd_u32le(&e), off = rd_u32le(&e);
        if (!rd_ok(&e)) break;                                /* directory cut short: use what was listed */
        if (!w) w = 256;
        if (w <= best.w) continue;
        Rd img = rd_sub_at(&r, off, sz);
        if (!rd_ok(&img)) die("%s: truncated image", path);
        best = (IcoImage){ img.p, img.n, w };
    }
    if (!best.p) die("%s has no images", path);
    return best;
}

typedef struct { int w, h, bpp; uint32_t ncol; const uint8_t *pal, *xorp, *andp; size_t xstride, astride; } Dib;

static Dib dib_parse(const IcoImage *im, const char *path)
{
    Rd r = rd_make(im->p, im->n);
    Dib d;
    uint32_t hsz = rd_u32le(&r);
    d.w = rd_i32le(&r);
    d.h = rd_i32le(&r) / 2;
    (void)rd_u16le(&r);
    d.bpp = rd_u16le(&r);
    (void)rd_skip(&r, 16);
    d.ncol = rd_u32le(&r);
    if (!rd_ok(&r) || hsz < DIB_HEADER_MIN) die("%s: bad DIB", path);
    if (d.w <= 0 || d.h <= 0 || d.w > ICON_DIM_MAX || d.h > ICON_DIM_MAX) die("%s: bad DIB size", path);
    if (d.bpp != 1 && d.bpp != 4 && d.bpp != 8 && d.bpp != 24 && d.bpp != 32) die("%s: unsupported DIB depth", path);
    if (d.bpp <= 8 && !d.ncol) d.ncol = 1u << d.bpp;
    if (d.bpp > 8) d.ncol = 0;
    if (d.ncol > 256) die("%s: bad DIB palette", path);
    d.xstride = (size_t)((d.w * d.bpp + 31) / 32) * 4;
    d.astride = (size_t)((d.w + 31) / 32) * 4;
    Rd body = rd_sub_at(&r, hsz, im->n >= hsz ? im->n - hsz : 0);
    d.pal = rd_view(&body, (size_t)d.ncol * 4);
    d.xorp = rd_view(&body, d.xstride * (size_t)d.h);
    d.andp = rd_view(&body, d.astride * (size_t)d.h);
    if (!rd_ok(&body)) die("%s: truncated DIB", path);
    return d;
}

static void dib_pixel(const Dib *d, int x, int y, uint8_t px[4])
{
    const uint8_t *row = d->xorp + d->xstride * (size_t)(d->h - 1 - y);
    const uint8_t *arow = d->andp + d->astride * (size_t)(d->h - 1 - y);
    bool masked = (arow[x / 8] >> (7 - x % 8)) & 1;
    size_t ux = (size_t)x;
    if (d->bpp == 32) {
        const uint8_t *s = row + 4 * ux;
        px[2] = s[0];
        px[1] = s[1];
        px[0] = s[2];
        px[3] = s[3];
        return;
    }
    if (d->bpp == 24) { const uint8_t *s = row + 3 * ux; px[2] = s[0]; px[1] = s[1]; px[0] = s[2]; }
    else {
        uint32_t idx = d->bpp == 8 ? row[x] : d->bpp == 4 ? (row[x / 2] >> (x & 1 ? 0 : 4)) & 15u
                                                         : (row[x / 8] >> (7 - x % 8)) & 1u;
        if (idx >= d->ncol) idx = 0;
        const uint8_t *s = d->pal + 4 * (size_t)idx;
        px[2] = s[0]; px[1] = s[1]; px[0] = s[2];
    }
    px[3] = masked ? 0 : 255;
}

/* Largest BMP (DIB) image of an .ico -> RGBA. A PNG entry is returned raw in *png instead. */
static Image ico_decode(const IcoImage *im, const char *path, IcoImage *png)
{
    Image out = { 0, 0, NULL };
    *png = (IcoImage){ NULL, 0, 0 };
    if (im->n >= 8 && !memcmp(im->p, "\x89PNG", 4)) { *png = *im; out.w = out.h = im->w; return out; }
    Dib d = dib_parse(im, path);
    out.w = d.w;
    out.h = d.h;
    out.rgba = xalloc((size_t)d.w * (size_t)d.h * 4);
    bool any_alpha = false;
    for (int y = 0; y < d.h; y++)
        for (int x = 0; x < d.w; x++) {
            uint8_t *px = out.rgba + ((size_t)y * (size_t)d.w + (size_t)x) * 4;
            dib_pixel(&d, x, y, px);
            if (d.bpp == 32 && px[3]) any_alpha = true;
        }
    if (d.bpp == 32 && !any_alpha)                            /* 32-bit without alpha: use the AND mask */
        for (int y = 0; y < d.h; y++)
            for (int x = 0; x < d.w; x++) {
                const uint8_t *arow = d.andp + d.astride * (size_t)(d.h - 1 - y);
                out.rgba[((size_t)y * (size_t)d.w + (size_t)x) * 4 + 3] = (arow[x / 8] >> (7 - x % 8)) & 1 ? 0 : 255;
            }
    return out;
}

/* Alpha-weighted box filter over source pixels [x0,x1) x [y0,y1) -> one RGBA pixel. */
static void box_pixel(const Image *s, int x0, int x1, int y0, int y1, uint8_t o[4])
{
    double acc[4] = {0, 0, 0, 0};
    int cnt = 0;
    for (int yy = y0; yy < y1 && yy < s->h; yy++)
        for (int xx = x0; xx < x1 && xx < s->w; xx++) {
            const uint8_t *p = s->rgba + ((size_t)yy * (size_t)s->w + (size_t)xx) * 4;
            double a = p[3] / 255.0;
            acc[0] += p[0] * a; acc[1] += p[1] * a; acc[2] += p[2] * a; acc[3] += a;
            cnt++;
        }
    for (int c = 0; c < 3; c++) o[c] = (uint8_t)(acc[3] > 0 ? acc[c] / acc[3] + 0.5 : 0);
    o[3] = (uint8_t)(cnt ? acc[3] / cnt * 255.0 + 0.5 : 0);
}

static Image scale_to(const Image *s, int size)
{
    Image d = { size, size, xalloc((size_t)size * (size_t)size * 4) };
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int x0 = x * s->w / size, x1 = (x + 1) * s->w / size, y0 = y * s->h / size, y1 = (y + 1) * s->h / size;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            box_pixel(s, x0, x1, y0, y1, d.rgba + ((size_t)y * (size_t)size + (size_t)x) * 4);
        }
    return d;
}

typedef struct { uint8_t *p; size_t n, cap; } Buf;

static void buf_put(void *ctx, void *data, int size)
{
    Buf *b = ctx;
    if (size <= 0) return;
    if (b->n + (size_t)size > b->cap) {
        b->cap = (b->n + (size_t)size) * 2;
        b->p = xrealloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, data, (size_t)size);
    b->n += (size_t)size;
}

static Buf png_of(const Image *im)
{
    Buf b = { NULL, 0, 0 };
    if (!stbi_write_png_to_func(buf_put, &b, im->w, im->h, 4, im->rgba, im->w * 4)) die("PNG encoding failed%s", NULL);
    return b;
}

/* PNG-based icns entries: icp4 16, icp5 32, icp6 64, ic07 128, ic08 256, ic09 512 (sizes <= the source). */
static Buf icns_of(const Image *im)
{
    static const struct { const char *type; int size; } ty[] = {
        { "icp4", 16 }, { "icp5", 32 }, { "icp6", 64 }, { "ic07", 128 }, { "ic08", 256 }, { "ic09", 512 },
    };
    Buf all = { NULL, 0, 0 };
    uint8_t hdr[8] = { 'i', 'c', 'n', 's', 0, 0, 0, 0 };
    buf_put(&all, hdr, 8);
    for (size_t i = 0; i < sizeof ty / sizeof ty[0]; i++) {
        if (ty[i].size > im->w) continue;
        Image s = ty[i].size == im->w && im->w == im->h ? *im : scale_to(im, ty[i].size);
        Buf b = png_of(&s);
        uint8_t eh[8];
        memcpy(eh, ty[i].type, 4);
        for (int k = 0; k < 4; k++) eh[4 + k] = (uint8_t)((b.n + 8) >> (24 - 8 * k));   /* big-endian */
        buf_put(&all, eh, 8);
        buf_put(&all, b.p, (int)b.n);
        free(b.p);
        if (s.rgba != im->rgba) free(s.rgba);
    }
    for (int k = 0; k < 4; k++) all.p[4 + k] = (uint8_t)(all.n >> (24 - 8 * k));
    return all;
}

static int cmd_icon(const char *ico, const char *out, int icns)
{
    size_t n;
    uint8_t *file = read_file(ico, &n);
    IcoImage best = ico_largest(file, n, ico), png;
    Image im = ico_decode(&best, ico, &png);
    if (png.p && icns) die("%s: PNG-compressed .ico entries are not supported for icns; use a BMP-based .ico", ico);
    char *tmp = concat3(out, ".tmp", "");
    FILE *o = xopen(tmp, "wb");
    if (png.p) xwrite(o, png.p, png.n, tmp);
    else {
        Buf b = icns ? icns_of(&im) : png_of(&im);
        xwrite(o, b.p, b.n, tmp);
        free(b.p);
    }
    xclose(o, tmp);
    commit(tmp, out);
    free(im.rgba);
    free(file);
    free(tmp);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ cembed */

/* OUT.c = FILE as `const unsigned char NAME[]` and `const size_t NAME_size`: binary data linked into the engine
 * (the vendored fallback font) without a build dependency on xxd or an assembler's .incbin. */
static int cmd_cembed(const char *file, const char *out, const char *name)
{
    size_t n;
    uint8_t *d = read_file(file, &n);
    char *tmp = concat3(out, ".tmp", "");
    FILE *f = xopen(tmp, "wb");
    if (fprintf(f, "/* Generated by sayopack cembed; do not edit. */\n#include <stddef.h>\n"
                   "extern const unsigned char %s[];\nextern const size_t %s_size;\n"
                   "const unsigned char %s[] = {\n", name, name, name) < 0)
        die("cannot write %s", tmp);
    for (size_t i = 0; i < n; i++)
        if (fprintf(f, "%u,%s", (unsigned)d[i], i % 24 == 23 ? "\n" : "") < 0) die("cannot write %s", tmp);
    if (fprintf(f, "\n};\nconst size_t %s_size = %zu;\n", name, n) < 0) die("cannot write %s", tmp);
    xclose(f, tmp);
    commit(tmp, out);
    free(d);
    free(tmp);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ main */

static int usage(void)
{
    (void)fprintf(stderr,
                  "usage: sayopack create OUT.pak ROOT LIST   pack files listed in LIST (paths relative to ROOT)\n"
                  "       sayopack append EXE PAK OUT         OUT = EXE (old payload removed) + PAK\n"
                  "       sayopack strip  EXE OUT             OUT = EXE without payload\n"
                  "       sayopack list   FILE                show the payload table of a .pak / executable\n"
                  "       sayopack verify FILE                check all CRC-32s\n"
                  "       sayopack icns   ICO OUT.icns        macOS icon from a .ico\n"
                  "       sayopack png    ICO OUT.png         largest .ico image as PNG\n"
                  "       sayopack cembed FILE OUT.c NAME     FILE as a C byte array NAME (+ NAME_size)\n"
                  "format: docs/PAYLOAD.md\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    const char *c = argv[1];
    if (!strcmp(c, "create") && argc == 5) return cmd_create(argv[2], argv[3], argv[4]);
    if (!strcmp(c, "append") && argc == 5) return cmd_append(argv[2], argv[3], argv[4]);
    if (!strcmp(c, "strip") && argc == 4) return cmd_append(argv[2], NULL, argv[3]);
    if (!strcmp(c, "list") && argc == 3) return cmd_list_verify(argv[2], 0);
    if (!strcmp(c, "verify") && argc == 3) return cmd_list_verify(argv[2], 1);
    if (!strcmp(c, "icns") && argc == 4) return cmd_icon(argv[2], argv[3], 1);
    if (!strcmp(c, "png") && argc == 4) return cmd_icon(argv[2], argv[3], 0);
    if (!strcmp(c, "cembed") && argc == 5) return cmd_cembed(argv[2], argv[3], argv[4]);
    return usage();
}
