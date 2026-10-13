/* plat/fs.c — layered, case-insensitive, read-only game data file system (ENGINE_SPEC §2.1 #4; docs/PAYLOAD.md).
 * Scripts say "bg_rd" while the file is "BG_RD.g00", so every file of every layer is indexed by its canonical path
 * ("ja/g00/BG_RD.g00") and looked up with ASCII case folding. Layers are directories (enumerated with SDL3) or
 * SAYOPAK archives (a payload appended to the executable or a stand-alone game.pak, read through a sub-range
 * SDL_IOStream); the archive table is decoded by fmt/sayopak.c.
 *
 * Memory: the whole index (layers, entries, paths, the sorted lookup table, the description) lives in one private
 * arena carved from mem_perm() on first use. index_reset() is arena_reset(), so a remount can never leak and nothing
 * is left at exit. A mount is a transaction: on any failure (unreadable layer, index budget exhausted) the arena
 * and the index return to their state before the mount. Main thread only (the arena checks its owner).
 *
 * Buffers handed to callers (fs_read*, fs_path, fs_list) are tracked heap blocks (MEM_PLAT), released with fs_free /
 * fs_list_free. */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L          /* readlink */
#endif
#include "plat/fs.h"
#include "plat/sysdep.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "fmt/sayopak.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#elif defined(__unix__) || defined(__linux__)
#include <unistd.h>
#endif

enum {
    FS_ARENA_BYTES = 4 << 20,            /* index budget: ~1000 files need ~250 KiB */
    FS_MAX_LAYERS = 16,
    FS_CHUNK = 256,                      /* entries per arena block */
    FS_DIR_DEPTH_MAX = 8,                /* directory nesting below a layer root */
    FS_PATH_MAX = SAYOPAK_MAX_PATH,      /* canonical paths are shorter than this */
    FS_PREFIX_MAX = 64,
    FS_LANG_MAX = 16,                    /* language codes are shorter than this */
    FS_SELF_MAX = 4096,                  /* OS path of the running executable */
    FS_VERIFY_CHUNK = 1 << 20
};
#define FS_MAX_READ ((uint64_t)512 << 20) /* larger files are refused (the game's largest is ~60 MB) */

typedef enum { LAYER_DIR, LAYER_ARCHIVE } LayerKind;
typedef struct { LayerKind kind; const char *path, *desc; } Layer;

typedef struct {
    const char *name;                    /* canonical path, original case, '/' separated: "ja/g00/BG_RD.g00" */
    const char *full;                    /* LAYER_DIR: OS path */
    int64_t off, size;                   /* LAYER_ARCHIVE: absolute file offset and size */
    uint32_t crc;
    int layer;
} FsEntry;

typedef struct FsChunk { struct FsChunk *next; int n; FsEntry e[FS_CHUNK]; } FsChunk;

/* Everything a mount changes; saved before and restored on failure. */
typedef struct {
    ArenaMark mark;
    FsChunk *chunks;
    int head_n;                          /* chunks->n: a failed mount may have added entries to an old chunk */
    int nentries, nlayer, nindex;
    const FsEntry **index;
    const char *desc;
} FsState;

static Arena g_arena;
static bool g_arena_ready;
static Layer g_layer[FS_MAX_LAYERS];
static FsState g_fs;                     /* the live index; g_fs.mark is unused here */
static char g_lang[FS_LANG_MAX] = FS_LANG_BASE;

/* ------------------------------------------------------------------------------------------------ arena */

static Arena *index_arena(void)
{
    if (!g_arena_ready) {
        arena_init_child(&g_arena, "fs", mem_perm(), FS_ARENA_BYTES);
        g_arena_ready = true;
    }
    return &g_arena;
}

static FsState index_snapshot(void)
{
    FsState s = g_fs;
    s.mark = arena_mark(index_arena());
    s.head_n = g_fs.chunks ? g_fs.chunks->n : 0;
    return s;
}

static void index_restore(const FsState *s)
{
    arena_reset_to(index_arena(), s->mark);
    g_fs = *s;
    if (g_fs.chunks) g_fs.chunks->n = s->head_n;
}

static void index_reset(void)
{
    if (g_arena_ready) arena_reset(&g_arena);
    memset(&g_fs, 0, sizeof g_fs);
    memset(g_layer, 0, sizeof g_layer);
}

/* Arena copy of s (n bytes, NUL added); NULL when the index budget is exhausted. */
static char *index_strdup(Str s)
{
    char *d = arena_try_alloc(index_arena(), s.n + 1, 1);
    if (d && s.n) memcpy(d, s.p, s.n);
    return d;
}

/* ------------------------------------------------------------------------------------------------ index */

static bool is_sep(char c) { return c == '/' || c == '\\'; }

/* Canonical form of a relative path: '\\' -> '/'. */
static void to_slashes(char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) if (s[i] == '\\') s[i] = '/';
}

static int add_layer(LayerKind kind, const char *path, const char *desc_prefix)
{
    if (g_fs.nlayer >= FS_MAX_LAYERS) { log_msg(LOG_ERR, "fs: more than %d layers", FS_MAX_LAYERS); return -1; }
    size_t need = strlen(desc_prefix) + 1 + strlen(path) + 1;
    char *desc = arena_try_alloc(index_arena(), need, 1);
    char *p = index_strdup(str_c(path));
    if (!desc || !p) return -1;
    StrBuf b;
    sb_init(&b, desc, need);
    (void)sb_fmt(&b, "%s:%s", desc_prefix, path);   /* sized exactly: cannot truncate */
    g_layer[g_fs.nlayer] = (Layer){ kind, p, desc };
    return g_fs.nlayer++;
}

/* New entry for canonical path `canon` (prefix + relative path); NULL when the budget is exhausted. */
static FsEntry *add_entry(Str canon, int layer)
{
    if (!g_fs.chunks || g_fs.chunks->n == FS_CHUNK) {
        FsChunk *c = arena_try_alloc(index_arena(), sizeof *c, _Alignof(FsChunk));
        if (!c) return NULL;
        c->next = g_fs.chunks;
        g_fs.chunks = c;
    }
    char *name = index_strdup(canon);
    if (!name) return NULL;
    to_slashes(name, canon.n);
    FsEntry *e = &g_fs.chunks->e[g_fs.chunks->n++];
    memset(e, 0, sizeof *e);
    e->name = name;
    e->layer = layer;
    g_fs.nentries++;
    return e;
}

/* name ascending (ASCII case folded), then newest layer first */
static int cmp_entry(const void *a, const void *b)
{
    const FsEntry *x = *(const FsEntry *const *)a, *y = *(const FsEntry *const *)b;
    int c = str_cmp_fold(str_c(x->name), str_c(y->name));
    return c ? c : (y->layer > x->layer) - (y->layer < x->layer);
}

/* Rebuilds the sorted lookup table over every entry; shadowed duplicates (older layers) are left out. */
static bool reindex(void)
{
    const FsEntry **ix = (const FsEntry **)arena_try_alloc(index_arena(), (size_t)(g_fs.nentries ? g_fs.nentries : 1) *
                                                           sizeof *ix, _Alignof(const FsEntry *));
    if (!ix) return false;
    int n = 0;
    for (FsChunk *c = g_fs.chunks; c; c = c->next)
        for (int i = 0; i < c->n; i++) ix[n++] = &c->e[i];
    ASSERT(n == g_fs.nentries);
    qsort((void *)ix, (size_t)n, sizeof *ix, cmp_entry);
    int w = 0;
    for (int i = 0; i < n; i++)
        if (!w || !str_eq_fold(str_c(ix[w - 1]->name), str_c(ix[i]->name))) ix[w++] = ix[i];
    g_fs.index = ix;
    g_fs.nindex = w;
    return true;
}

/* "dir:/y + payload:/x" (newest layer first), sized exactly in the arena. */
static bool update_desc(void)
{
    size_t need = 1;
    for (int i = 0; i < g_fs.nlayer; i++) need += strlen(g_layer[i].desc) + 3;
    char *mem = arena_try_alloc(index_arena(), need, 1);
    if (!mem) return false;
    StrBuf b;
    sb_init(&b, mem, need);
    for (int i = g_fs.nlayer - 1; i >= 0; i--) {
        if (b.n) (void)sb_put_c(&b, " + ");
        (void)sb_put_c(&b, g_layer[i].desc);
    }
    g_fs.desc = mem;
    return true;
}

static bool commit_mount(void) { return reindex() && update_desc(); }

bool fs_mounted(void) { return g_fs.nindex > 0; }
const char *fs_describe(void) { return g_fs.desc ? g_fs.desc : "(nothing mounted)"; }

/* first index whose name >= key (folded) */
static int lower_bound(Str key)
{
    int lo = 0, hi = g_fs.nindex;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (str_cmp_fold(str_c(g_fs.index[mid]->name), key) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static const FsEntry *lookup(Str key)
{
    int i = lower_bound(key);
    return i < g_fs.nindex && str_eq_fold(str_c(g_fs.index[i]->name), key) ? g_fs.index[i] : NULL;
}

/* Normalised prefix: "" or "xx/" (no leading separator, '/' separated). False if it does not fit. */
static bool norm_prefix(const char *in, StrBuf *out)
{
    sb_reset(out);
    if (!in) return true;
    while (is_sep(*in)) in++;
    bool ok = sb_put_c(out, in);
    to_slashes(out->p, out->n);
    if (out->n && out->p[out->n - 1] != '/') ok = sb_put_char(out, '/') && ok;
    return ok;
}

/* ------------------------------------------------------------------------------------------------ dir layers */

typedef struct {
    const char *prefix;                  /* canonical prefix of this directory: "" or "ja/g00/" */
    int depth, layer, added;
    bool failed;                         /* index budget exhausted */
} EnumCtx;

static SDL_EnumerationResult SDLCALL enum_cb(void *ud, const char *dirname, const char *fname);

/* One directory entry: a file is indexed, a directory is enumerated (depth-capped). */
static bool enum_entry(EnumCtx *ctx, const char *full, const char *fname, const SDL_PathInfo *info)
{
    SB_ON_STACK(rel, FS_PATH_MAX);
    if (!sb_fmt(&rel, "%s%s", ctx->prefix, fname)) {
        log_msg(LOG_WARN, "fs: skipped %s (path longer than %d bytes)", full, FS_PATH_MAX - 1);
        return true;
    }
    if (info->type == SDL_PATHTYPE_FILE) {
        FsEntry *e = add_entry(sb_str(&rel), ctx->layer);
        if (!e || !(e->full = index_strdup(str_c(full)))) return false;
        e->size = (int64_t)info->size;
        ctx->added++;
    } else if (info->type == SDL_PATHTYPE_DIRECTORY && ctx->depth < FS_DIR_DEPTH_MAX) {
        if (!sb_put_char(&rel, '/')) return true;
        EnumCtx sub = { sb_cstr(&rel), ctx->depth + 1, ctx->layer, 0, false };
        (void)SDL_EnumerateDirectory(full, enum_cb, &sub);   /* a partly unreadable subtree is skipped */
        ctx->added += sub.added;
        if (sub.failed) return false;
    }
    return true;
}

static SDL_EnumerationResult SDLCALL enum_cb(void *ud, const char *dirname, const char *fname)
{
    EnumCtx *ctx = ud;
    if (fname[0] == '.') return SDL_ENUM_CONTINUE;    /* ".", "..", stamps, .DS_Store */
    Scratch s = mem_scratch_begin(NULL);
    size_t dl = strlen(dirname), need = dl + strlen(fname) + 2;
    char *full = arena_try_alloc(s.arena, need, 1);
    SDL_EnumerationResult res = SDL_ENUM_CONTINUE;
    if (full) {
        StrBuf b;
        sb_init(&b, full, need);
        /* SDL passes dirname with a trailing separator */
        (void)sb_fmt(&b, (dl && is_sep(dirname[dl - 1])) ? "%s%s" : "%s/%s", dirname, fname);
        SDL_PathInfo info;
        if (SDL_GetPathInfo(full, &info) && !enum_entry(ctx, full, fname, &info)) res = SDL_ENUM_FAILURE;
    } else {
        res = SDL_ENUM_FAILURE;
    }
    mem_scratch_end(&s);
    if (res == SDL_ENUM_FAILURE) ctx->failed = true;
    return res;
}

static bool has_ci(const char *dir, const char *name)
{
    int n = 0;
    char **m = SDL_GlobDirectory(dir, name, SDL_GLOB_CASEINSENSITIVE, &n);
    SDL_free((void *)m);
    return n > 0;
}

/* Canonical prefix of a directory layer: "" for a data tree (ja/Scene.pck inside), else "ja/". */
static const char *dir_prefix(const char *root)
{
    Scratch s = mem_scratch_begin(NULL);
    size_t need = strlen(root) + 4;
    char *ja = arena_alloc(s.arena, need, 1);
    StrBuf b;
    sb_init(&b, ja, need);
    (void)sb_fmt(&b, "%s/ja", root);
    const char *pre = plat_path_is_dir(ja) && has_ci(ja, "Scene.pck") ? "" : "ja/";
    mem_scratch_end(&s);
    return pre;
}

static bool mount_dir_files(const char *root, const char *pre)
{
    int layer = add_layer(LAYER_DIR, root, *pre ? "dir(ja)" : "dir");
    if (layer < 0) return false;
    EnumCtx ctx = { pre, 0, layer, 0, false };
    if (!SDL_EnumerateDirectory(root, enum_cb, &ctx) && !ctx.failed)
        log_msg(LOG_ERR, "fs: cannot read %s: %s", root, SDL_GetError());
    if (ctx.failed) {
        log_msg(LOG_ERR, "fs: %s: index budget (%d bytes) exhausted", root, FS_ARENA_BYTES);
        return false;
    }
    log_msg(LOG_DEBUG, "fs: %d files from %s mounted at /%s", ctx.added, root, pre);
    return ctx.added > 0 && commit_mount();
}

static bool fs_mount_dir(const char *dir, const char *prefix_in_tree)
{
    if (!dir || !*dir || !plat_path_is_dir(dir)) {
        log_msg(LOG_ERR, "fs: %s is not a directory", dir ? dir : "(null)");
        return false;
    }
    FsState before = index_snapshot();
    Str r = str_c(dir);
    while (r.n > 1 && is_sep(r.p[r.n - 1])) r.n--;
    const char *root = index_strdup(r);
    SB_ON_STACK(pre, FS_PREFIX_MAX);
    bool ok = root && (prefix_in_tree ? norm_prefix(prefix_in_tree, &pre) : sb_put_c(&pre, dir_prefix(root)));
    ok = ok && mount_dir_files(root, sb_cstr(&pre));
    if (!ok) index_restore(&before);
    return ok;
}

/* ------------------------------------------------------------------------------------------------ archives */

static bool read_at(SDL_IOStream *f, int64_t off, void *buf, size_t n)
{
    return SDL_SeekIO(f, off, SDL_IO_SEEK_SET) == off && SDL_ReadIO(f, buf, n) == n;
}

/* Trailer + header of the payload at the end of f. *base = payload start. */
static SayopakStatus archive_info(SDL_IOStream *f, SayopakInfo *info, int64_t *base)
{
    uint8_t tr[SAYOPAK_TRAILER_SIZE], hd[SAYOPAK_HEADER_SIZE];
    int64_t fsize = SDL_GetIOSize(f);
    if (fsize < SAYOPAK_HEADER_SIZE + SAYOPAK_TRAILER_SIZE || !read_at(f, fsize - SAYOPAK_TRAILER_SIZE, tr, sizeof tr))
        return SAYOPAK_ABSENT;
    SayopakStatus st = sayopak_read_trailer(tr, (uint64_t)fsize, info);
    if (st != SAYOPAK_VALID) return st;
    *base = fsize - (int64_t)info->payload_size;
    if (!read_at(f, *base, hd, sizeof hd) || !sayopak_check_header(hd)) return SAYOPAK_DAMAGED;
    return SAYOPAK_VALID;
}

/* Indexes the table entries; returns how many were valid (the rest of a damaged table is ignored). */
static uint32_t index_table(Rd *table, const SayopakInfo *info, int64_t base, const char *pre, int layer, bool *full)
{
    uint32_t i;
    *full = false;
    for (i = 0; i < info->count; i++) {
        SayopakEntry se;
        if (!sayopak_next_entry(table, info, &se)) break;
        SB_ON_STACK(canon, FS_PREFIX_MAX + FS_PATH_MAX);
        (void)sb_put_c(&canon, pre);
        (void)sb_put(&canon, str_make(se.path, se.path_len));   /* sized for the longest valid entry */
        FsEntry *e = add_entry(sb_str(&canon), layer);
        if (!e) { *full = true; break; }
        e->off = base + (int64_t)se.offset;
        e->size = (int64_t)se.size;
        e->crc = se.crc;
    }
    return i;
}

/* Reads, checks and indexes the table of an archive whose trailer is valid. */
static bool mount_table(SDL_IOStream *f, const char *path, const SayopakInfo *info, int64_t base, const char *pre,
                        bool self)
{
    Scratch s = mem_scratch_begin(NULL);
    uint8_t *tab = arena_try_alloc(s.arena, info->table_size ? (size_t)info->table_size : 1, 1);
    bool ok = false;
    if (!tab) log_msg(LOG_ERR, "fs: %s: SAYOPAK file table too large (%llu bytes)", path,
                      (unsigned long long)info->table_size);
    else if (!read_at(f, base + (int64_t)info->table_offset, tab, (size_t)info->table_size) ||
             hash_crc32(0, tab, (size_t)info->table_size) != info->table_crc)
        log_msg(LOG_ERR, "fs: %s: SAYOPAK file table is damaged", path);
    else {
        int layer = add_layer(LAYER_ARCHIVE, path, self ? "payload" : "pak");
        Rd table = rd_make(tab, (size_t)info->table_size);
        bool full = false;
        uint32_t n = layer < 0 ? 0 : index_table(&table, info, base, pre, layer, &full);
        if (full) log_msg(LOG_ERR, "fs: %s: index budget (%d bytes) exhausted", path, FS_ARENA_BYTES);
        else if (n != info->count)
            log_msg(LOG_ERR, "fs: %s: bad SAYOPAK entry %u of %u (ignored the rest)", path, n, info->count);
        log_msg(LOG_DEBUG, "fs: %u files from %s (payload %llu bytes at %lld) mounted at /%s", n, path,
                (unsigned long long)info->payload_size, (long long)base, pre);
        ok = !full && n > 0 && commit_mount();
    }
    mem_scratch_end(&s);
    return ok;
}

static const char *fs_self_exe(void);

static bool fs_mount_archive(const char *path_or_self, const char *prefix_in_tree)
{
    bool self = !path_or_self || !*path_or_self;
    const char *path = self ? fs_self_exe() : path_or_self;
    if (!path) return false;
    SDL_IOStream *f = SDL_IOFromFile(path, "rb");
    if (!f) {
        if (!self) log_msg(LOG_ERR, "fs: cannot open %s: %s", path, SDL_GetError());
        return false;
    }
    FsState before = index_snapshot();
    SayopakInfo info;
    int64_t base = 0;
    SayopakStatus st = archive_info(f, &info, &base);
    SB_ON_STACK(pre, FS_PREFIX_MAX);
    bool ok = false;
    if (st == SAYOPAK_ABSENT) { if (!self) log_msg(LOG_ERR, "fs: %s has no SAYOPAK trailer", path); }
    else if (st == SAYOPAK_DAMAGED) log_msg(LOG_ERR, "fs: %s: bad SAYOPAK trailer or header", path);
    else if (!norm_prefix(prefix_in_tree, &pre)) log_msg(LOG_ERR, "fs: prefix too long: %s", prefix_in_tree);
    else ok = mount_table(f, path, &info, base, sb_cstr(&pre), self);
    (void)SDL_CloseIO(f);   /* read-only stream: nothing to flush */
    if (!ok) index_restore(&before);
    return ok;
}

/* ---- read-only sub-range stream over one archived file ---- */
typedef struct { SDL_IOStream *f; int64_t base, size, pos; } SubIO;

static Sint64 SDLCALL sub_size(void *u) { return ((SubIO *)u)->size; }

static Sint64 SDLCALL sub_seek(void *u, Sint64 off, SDL_IOWhence whence)
{
    SubIO *s = u;
    int64_t from = whence == SDL_IO_SEEK_SET ? 0 : whence == SDL_IO_SEEK_CUR ? s->pos : s->size;
    if ((off > 0 && from > INT64_MAX - off) || from + off < 0) { SDL_SetError("fs: bad seek"); return -1; }
    s->pos = from + off;
    return s->pos;
}

static size_t SDLCALL sub_read(void *u, void *ptr, size_t size, SDL_IOStatus *status)
{
    SubIO *s = u;
    if (s->pos >= s->size) { *status = SDL_IO_STATUS_EOF; return 0; }
    if ((uint64_t)size > (uint64_t)(s->size - s->pos)) size = (size_t)(s->size - s->pos);
    if (SDL_SeekIO(s->f, s->base + s->pos, SDL_IO_SEEK_SET) < 0) { *status = SDL_IO_STATUS_ERROR; return 0; }
    size_t r = SDL_ReadIO(s->f, ptr, size);
    s->pos += (int64_t)r;
    if (r < size) *status = SDL_GetIOStatus(s->f) == SDL_IO_STATUS_EOF ? SDL_IO_STATUS_EOF : SDL_IO_STATUS_ERROR;
    return r;
}

static bool SDLCALL sub_close(void *u)
{
    SubIO *s = u;
    bool ok = SDL_CloseIO(s->f);
    mem_free(s);
    return ok;
}

static SDL_IOStream *open_entry(const FsEntry *e)
{
    const Layer *L = &g_layer[e->layer];
    if (L->kind == LAYER_DIR) return SDL_IOFromFile(e->full, "rb");
    SDL_IOStream *f = SDL_IOFromFile(L->path, "rb");
    if (!f) return NULL;
    SubIO *s = mem_alloc(sizeof *s, MEM_PLAT);
    *s = (SubIO){ f, e->off, e->size, 0 };
    SDL_IOStreamInterface iface;
    SDL_INIT_INTERFACE(&iface);
    iface.size = sub_size;
    iface.seek = sub_seek;
    iface.read = sub_read;
    iface.close = sub_close;
    SDL_IOStream *io = SDL_OpenIO(&iface, s);
    if (!io) { (void)SDL_CloseIO(f); mem_free(s); }   /* read-only stream */
    return io;
}

/* ------------------------------------------------------------------------------------------------ lookup */

static const char *strip_vfs(const char *rel)
{
    return rel && !strncmp(rel, "vfs:", 4) ? rel + 4 : rel;
}

/* visible (or '/'-canonical) path -> entry */
static const FsEntry *resolve(const char *rel)
{
    rel = strip_vfs(rel);
    if (!rel || !*rel || !g_fs.nindex) return NULL;
    SB_ON_STACK(key, FS_LANG_MAX + FS_PATH_MAX);
    if (is_sep(*rel)) {
        if (!sb_put_c(&key, rel + 1)) return NULL;     /* longer than any indexed path */
        to_slashes(key.p, key.n);
        return lookup(sb_str(&key));
    }
    if (strcmp(g_lang, FS_LANG_BASE) != 0) {
        if (!sb_fmt(&key, "%s/%s", g_lang, rel)) return NULL;
        to_slashes(key.p, key.n);
        const FsEntry *e = lookup(sb_str(&key));
        if (e) return e;
        sb_reset(&key);
    }
    if (!sb_fmt(&key, "ja/%s", rel)) return NULL;
    to_slashes(key.p, key.n);
    return lookup(sb_str(&key));
}

bool fs_exists(const char *rel) { return resolve(rel) != NULL; }

/* Whole stream into a MEM_PLAT heap block (+1 NUL byte); closes io. NULL on a short read or an absurd size. */
static uint8_t *read_stream(SDL_IOStream *io, const char *what, size_t *len)
{
    Sint64 sz = SDL_GetIOSize(io);
    if (sz < 0 || (uint64_t)sz > FS_MAX_READ) {
        log_msg(LOG_WARN, "fs: %s: unsupported size %lld", what, (long long)sz);
        (void)SDL_CloseIO(io);   /* read-only stream: nothing to flush */
        return NULL;
    }
    uint8_t *out = mem_alloc((size_t)sz + 1, MEM_PLAT);
    size_t got = sz ? SDL_ReadIO(io, out, (size_t)sz) : 0;
    (void)SDL_CloseIO(io);   /* read-only stream: nothing to flush */
    if (got != (size_t)sz) {
        log_msg(LOG_WARN, "fs: short read of %s (%zu of %lld bytes)", what, got, (long long)sz);
        mem_free(out);
        return NULL;
    }
    out[got] = 0;
    if (len) *len = got;
    return out;
}

uint8_t *fs_read_all(const char *rel, size_t *len)
{
    if (len) *len = 0;
    const FsEntry *e = resolve(rel);
    if (!e) { SDL_SetError("fs: %s not found", rel ? rel : "(null)"); return NULL; }
    SDL_IOStream *io = open_entry(e);
    return io ? read_stream(io, rel, len) : NULL;
}

const char *fs_layer_of(const char *rel)
{
    const FsEntry *e = resolve(rel);
    return e ? g_layer[e->layer].desc : NULL;
}

const char *fs_canonical(const char *rel)
{
    const FsEntry *e = resolve(rel);
    return e ? e->name : NULL;
}

void fs_free(void *data) { mem_free(data); }

/* ------------------------------------------------------------------------------------------------ language */

static bool lang_ok(const char *lang)
{
    return lang && *lang && strlen(lang) < FS_LANG_MAX && !strchr(lang, '/') && !strchr(lang, '\\');
}

bool fs_has_language(const char *lang)
{
    if (!lang_ok(lang)) return false;
    SB_ON_STACK(key, FS_LANG_MAX + 16);
    return sb_fmt(&key, "%s/scene.pck", lang) && lookup(sb_str(&key)) != NULL;
}

bool fs_set_language(const char *lang)
{
    if (!lang || !*lang) lang = FS_LANG_BASE;
    char l[FS_LANG_MAX];
    if (!lang_ok(lang) || !str_copy_c(l, sizeof l, lang)) {
        log_msg(LOG_WARN, "fs: language '%s' is not in the game data", lang);
        return false;
    }
    for (char *q = l; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
    if (strcmp(l, FS_LANG_BASE) != 0 && !fs_has_language(l)) {
        log_msg(LOG_WARN, "fs: language '%s' is not in the game data", lang);
        return false;
    }
    memcpy(g_lang, l, sizeof g_lang);                 /* same capacity, NUL-terminated */
    log_msg(LOG_DEBUG, "fs: language %s", g_lang);
    return true;
}

const char *fs_language(void) { return g_lang; }

/* ------------------------------------------------------------------------------------------------ legacy API */

static bool mount_source(const char *src)
{
    if (plat_path_is_dir(src)) return fs_mount_dir(src, NULL);
    if (plat_path_is_file(src)) return fs_mount_archive(src, NULL);
    log_msg(LOG_ERR, "fs: %s does not exist", src);
    return false;
}

bool fs_init(const char *gamedata_dir)
{
    index_reset();
    if (!gamedata_dir || !*gamedata_dir) return false;
    return mount_source(gamedata_dir);
}

static bool ends_with_ext(Str name, const char *ext)
{
    Str e = str_c(ext);
    return name.n > e.n && name.p[name.n - e.n - 1] == '.' && str_ends_with_fold(name, e);
}

/* canonical "ja/g00/BG_RD.g00" -> visible "g00/BG_RD.g00" */
static const char *visible_name(const FsEntry *e)
{
    const char *s = strchr(e->name, '/');
    return s ? s + 1 : e->name;
}

char *fs_path(const char *subdir, const char *name, const char *ext)
{
    if (!name || !*name) return NULL;
    const char *sd = subdir ? subdir : "";
    const char *sep = *sd ? "/" : "";
    bool try_ext = ext && *ext && !ends_with_ext(str_c(name), ext);
    for (int pass = try_ext ? 0 : 1; pass < 2; pass++) {
        SB_ON_STACK(key, FS_PATH_MAX);
        bool fits = pass == 0 ? sb_fmt(&key, "%s%s%s.%s", sd, sep, name, ext) : sb_fmt(&key, "%s%s%s", sd, sep, name);
        const FsEntry *e = fits ? resolve(sb_cstr(&key)) : NULL;
        if (!e) continue;
        const char *v = visible_name(e);
        size_t n = strlen(v) + 5;
        char *out = mem_alloc(n, MEM_PLAT);
        StrBuf b;
        sb_init(&b, out, n);
        (void)sb_fmt(&b, "vfs:%s", v);                /* sized exactly */
        return out;
    }
    return NULL;
}

uint8_t *fs_read(const char *path, size_t *len)
{
    if (len) *len = 0;
    if (!path) return NULL;
    if (!strncmp(path, "vfs:", 4)) {
        uint8_t *d = fs_read_all(path, len);
        if (!d) log_msg(LOG_WARN, "fs: cannot read %s", path);
        return d;
    }
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) {
        log_msg(LOG_WARN, "fs: cannot read %s: %s", path, SDL_GetError());
        return NULL;
    }
    return read_stream(io, path, len);
}

static int cmp_name_fold(const void *a, const void *b)
{
    return str_cmp_fold(str_c(*(char *const *)a), str_c(*(char *const *)b));
}

/* Appends the base names of the visible files directly under `pre` ("ja/g00/") that match ext. */
static int list_layer(Str pre, const char *ext, char **out, int n, int cap)
{
    for (int i = lower_bound(pre); i < g_fs.nindex && n < cap; i++) {
        Str name = str_c(g_fs.index[i]->name);
        if (!str_eq_fold(str_sub(name, 0, pre.n), pre)) break;
        Str base = str_sub(name, pre.n, name.n);
        size_t slash;
        if (str_find_char(base, '/', &slash)) continue;
        if (ext && *ext && !ends_with_ext(base, ext)) continue;
        out[n++] = mem_strdup(base.p, MEM_PLAT);
    }
    return n;
}

int fs_list(const char *subdir, const char *ext, char ***names)
{
    const char *sd = subdir ? subdir : "";
    while (is_sep(*sd)) sd++;
    const char *langs[2] = { FS_LANG_BASE, strcmp(g_lang, FS_LANG_BASE) ? g_lang : NULL };
    int cap = g_fs.nindex, n = 0;
    char **out = (char **)mem_alloc_array((size_t)(cap ? cap : 1), sizeof *out, MEM_PLAT);
    for (int l = 0; l < 2; l++) {
        SB_ON_STACK(pre, FS_LANG_MAX + FS_PATH_MAX);
        if (!langs[l] || !sb_fmt(&pre, "%s/%s%s", langs[l], sd, *sd ? "/" : "")) continue;
        to_slashes(pre.p, pre.n);
        n = list_layer(sb_str(&pre), ext, out, n, cap);
    }
    qsort((void *)out, (size_t)n, sizeof *out, cmp_name_fold);
    int w = 0;   /* ja and overlay may both have the file: keep one */
    for (int i = 0; i < n; i++) {
        if (w && str_eq_fold(str_c(out[w - 1]), str_c(out[i]))) { mem_free(out[i]); continue; }
        out[w++] = out[i];
    }
    *names = out;
    return w;
}

void fs_list_free(char **names, int n)
{
    if (!names) return;
    for (int i = 0; i < n; i++) mem_free(names[i]);
    mem_free((void *)names);
}

/* ------------------------------------------------------------------------------------------------ host */

static const char *fs_self_exe(void)
{
    static char path[FS_SELF_MAX];
    static bool done;
    if (done) return path[0] ? path : NULL;
    done = true;
#if defined(_WIN32)
    wchar_t w[FS_SELF_MAX];
    DWORD n = GetModuleFileNameW(NULL, w, (DWORD)(sizeof w / sizeof w[0]));
    if (n == 0 || n >= sizeof w / sizeof w[0] ||
        !WideCharToMultiByte(CP_UTF8, 0, w, -1, path, (int)sizeof path, NULL, NULL)) path[0] = 0;
#elif defined(__APPLE__)
    char raw[FS_SELF_MAX];
    uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n) != 0 || !realpath(raw, path)) path[0] = 0;   /* PATH_MAX <= FS_SELF_MAX */
#elif defined(__unix__) || defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) n = readlink("/proc/curproc/file", path, sizeof path - 1);   /* *BSD with procfs */
    if (n >= (ssize_t)sizeof path - 1) n = 0;                                 /* possibly truncated: unusable */
    path[n > 0 ? n : 0] = 0;
#endif
    return path[0] ? path : NULL;
}

/* CRC-32 of one archived entry; false when it cannot be read completely. */
static bool entry_crc(const FsEntry *e, uint8_t *buf, uint32_t *crc)
{
    SDL_IOStream *io = open_entry(e);
    *crc = 0;
    int64_t left = e->size;
    while (io && left > 0) {                          /* each pass consumes > 0 bytes or stops */
        size_t want = left > FS_VERIFY_CHUNK ? FS_VERIFY_CHUNK : (size_t)left;
        size_t r = SDL_ReadIO(io, buf, want);
        if (!r) break;
        *crc = hash_crc32(*crc, buf, r);
        left -= (int64_t)r;
    }
    if (io) (void)SDL_CloseIO(io);   /* read-only stream: nothing to flush */
    return io && left == 0;
}

int fs_verify(void)
{
    int bad = 0;
    Scratch s = mem_scratch_begin(NULL);
    uint8_t *buf = arena_alloc_uninit(s.arena, FS_VERIFY_CHUNK, 1);
    for (int i = 0; i < g_fs.nindex; i++) {
        const FsEntry *e = g_fs.index[i];
        if (g_layer[e->layer].kind != LAYER_ARCHIVE) continue;
        uint32_t crc;
        if (!entry_crc(e, buf, &crc) || crc != e->crc) {
            log_msg(LOG_ERR, "fs: verify: %s is damaged (crc %08x, expected %08x)", e->name, crc, e->crc);
            bad++;
        }
    }
    mem_scratch_end(&s);
    return bad;
}

/* ------------------------------------------------------------------------------------------------ discovery */

/* (2)-(4) of fs_discover: game.pak / data dirs next to the executable, then ./build/data. */
static bool discover_near_exe(void)
{
    static const char *const rel[] = { "game.pak", "data", "../data", "../../build/data" };
    const char *base = SDL_GetBasePath();                              /* ends with a separator */
    for (size_t i = 0; base && i < COUNT_OF(rel); i++) {
        SB_ON_STACK(p, FS_SELF_MAX);
        if (!sb_fmt(&p, "%s%s", base, rel[i])) continue;
        const char *path = sb_cstr(&p);
        bool mounted = i == 0 ? plat_path_is_file(path) && fs_mount_archive(path, NULL)
                              : plat_path_is_dir(path) && fs_mount_dir(path, NULL);
        if (mounted && fs_has_language(FS_LANG_BASE)) return true;
        index_reset();
    }
    if (plat_path_is_dir("build/data") && fs_mount_dir("build/data", NULL) && fs_has_language(FS_LANG_BASE))
        return true;
    index_reset();
    return false;
}

bool fs_discover(const char *explicit_source)
{
    index_reset();
    bool found;
    if (explicit_source && *explicit_source) {
        found = mount_source(explicit_source);
        if (!found) log_msg(LOG_ERR, "fs: no game data in %s", explicit_source);
    } else {
        found = fs_mount_archive(NULL, NULL) && fs_has_language(FS_LANG_BASE);   /* (2) payload in the executable */
        if (!found) { index_reset(); found = discover_near_exe(); }
    }
    if (found)
        log_msg(LOG_INFO, "fs: game data: %s (%d files; languages: ja%s)", fs_describe(), g_fs.nindex,
                fs_has_language("en") ? ", en" : "");
    return found;
}
