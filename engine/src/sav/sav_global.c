/* sav/sav_global.c — global.sav: G/Z/M banks and names, read flags, CG flags; unknown or unloadable chunks are
 * kept verbatim so a partial failure never erases data (docs/SAVE_FORMAT.md §7).
 *
 * Memory: preserved chunk bodies are tracked heap copies, replaced on every decode and released by
 * sav_global_keep_clear (also from sav_shutdown). */
#include "sav/sav_internal.h"
#include "core/log.h"
#include "core/mem.h"
#include "txt/lang.h"
#include "vm/vm.h"
#include "vm/sysstate.h"
#include "vm/elements.h"
#include "ui/ui.h"
#include <string.h>

/* Chunks of global.sav that are re-emitted verbatim: unknown chunks (written by a newer engine) and known chunks
 * that failed to load. At most KEEP_MAX distinct tags; more are dropped with a warning. */
enum { KEEP_MAX = 32 };
typedef struct { uint32_t tag; uint8_t *body; size_t len; } KeptChunk;
static KeptChunk g_keep[KEEP_MAX];
static int g_nkeep;
static bool g_global_locked;  /* global.sav is from a newer engine: never overwrite it */

int sav_global_kept(void) { return g_nkeep; }
bool sav_global_locked(void) { return g_global_locked; }
void sav_global_set_locked(bool locked) { g_global_locked = locked; }

void sav_global_keep_clear(void)
{
    for (int i = 0; i < g_nkeep; i++) mem_free(g_keep[i].body);
    memset(g_keep, 0, sizeof g_keep);
    g_nkeep = 0;
}

static bool keep_has(uint32_t tag)
{
    for (int i = 0; i < g_nkeep; i++) if (g_keep[i].tag == tag) return true;
    return false;
}

static void keep_add(uint32_t tag, const uint8_t *body, size_t n)
{
    if (keep_has(tag)) return;
    if (g_nkeep >= KEEP_MAX) {
        log_msg(LOG_WARN, "save: more than %d unknown global chunks; extra dropped", KEEP_MAX);
        return;
    }
    g_keep[g_nkeep++] = (KeptChunk){ tag, mem_dup(body, n, MEM_SAV), n };
}

typedef struct { uint32_t tag; const char *name; SavSaveFn save; SavLoadFn load; } GlobalIO;
static const GlobalIO GLOBALS[] = {
    { SAV_TAG_VMGL, "vm (G, Z, M, namae)", vm_save_global, vm_load_global },
    { SAV_TAG_SYSG, "read flags",          sys_save_global, sys_load_global },
    { SAV_TAG_CGTG, "CG flags",            sys_cg_save_global, sys_cg_load_global },
};
enum { NGLOBALS = (int)COUNT_OF(GLOBALS) };

static const GlobalIO *global_of(uint32_t tag)
{
    for (int i = 0; i < NGLOBALS; i++) if (GLOBALS[i].tag == tag) return &GLOBALS[i];
    return NULL;
}

SavWriter *sav_global_encode(void)
{
    SavWriter *w = sav_writer_new();
    sav_file_begin(w, SAV_KIND_GLOB);
    sav_put_chunk(w, SAV_TAG_META);
    sav_put_enter(w);
    sav_put_meta(w, lang_code(), sav_now_unix());
    sav_put_leave(w);
    for (int i = 0; i < NGLOBALS; i++) {
        if (keep_has(GLOBALS[i].tag)) continue;   /* emitted verbatim below */
        sav_put_chunk(w, GLOBALS[i].tag);
        sav_put_enter(w);
        GLOBALS[i].save(w);
        sav_put_leave(w);
    }
    (void)sav_writer_data(w, NULL);   /* close the chunks before the raw ones; failure stays sticky */
    for (int i = 0; i < g_nkeep; i++) sav_put_raw_chunk(w, g_keep[i].tag, g_keep[i].body, g_keep[i].len);
    sav_file_end(w);
    return w;
}

/* Format 1: GLOB{magic, version} followed by sibling chunks VMGL, SYSG, CGTG. */
static bool decode_v1(SavReader *r)
{
    if (!sav_find_chunk(r, SAV_KIND_GLOB) || sav_get_u32(r) != SAV_OLD_MAGIC || sav_get_i32(r) != 1) return false;
    for (int i = 0; i < NGLOBALS; i++) {
        bool a = sav_find_chunk(r, GLOBALS[i].tag) && sav_get_enter(r) && GLOBALS[i].load(r) && sav_ok(r);
        sav_get_leave(r);
        if (!a) log_msg(LOG_WARN, "save: old global.sav: %s did not load", GLOBALS[i].name);
        sav_reset_fail(r);
    }
    return true;
}

static void decode_v2(SavReader *r)
{
    uint32_t tag;
    while (sav_next_chunk(r, &tag)) {   /* bounded: every chunk advances the cursor */
        const uint8_t *at = sav_cur(r);
        size_t n = sav_remaining(r);
        if (tag == SAV_TAG_META) continue;
        const GlobalIO *g = global_of(tag);
        if (!g) { keep_add(tag, at, n); continue; }   /* from a newer engine: keep verbatim */
        bool a = sav_get_enter(r) && g->load(r) && sav_ok(r);
        sav_get_leave(r);
        sav_reset_fail(r);
        if (!a) {
            log_msg(LOG_ERR, "save: global %s did not load; the stored data is kept unchanged", g->name);
            keep_add(tag, at, n);
        }
    }
}

bool sav_global_decode(const uint8_t *d, size_t len)
{
    SavFileView v;
    if (!sav_file_view(d, len, &v) || v.kind != SAV_KIND_GLOB) return false;
    SavReader *r = sav_view_reader(d, &v);
    sav_global_keep_clear();
    bool ok = true;
    if (v.fmt == SAV_FMT_V1) ok = decode_v1(r);
    else decode_v2(r);
    sav_reader_free(r);
    return ok;
}

void sav_global_write(void)
{
    if (g_global_locked) { log_msg(LOG_WARN, "save: global.sav is from a newer engine; not overwritten"); return; }
    SavWriter *w = sav_global_encode();
    Scratch s = mem_scratch_begin(NULL);
    (void)sav_file_store(sav_path_in(s.arena, "global.sav"), w, SAV_KIND_GLOB);   /* failures are logged there */
    mem_scratch_end(&s);
    sav_writer_free(w);
}

/* A format-1 file loaded from the main copy is kept once as global.sav.v1, then rewritten in format 2. */
static void convert_v1(const char *path, int which)
{
    if (which == 0) sav_file_copy_once(path, ".v1");
    sav_global_write();
    log_msg(LOG_INFO, "save: global.sav converted to format %d", SAV_VERSION);
}

bool sav_global_read(void)
{
    Scratch s = mem_scratch_begin(NULL);
    const char *path = sav_path_in(s.arena, "global.sav");
    size_t len;
    SavFileView v;
    int which = 0;
    sav_file_clear_newer();
    uint8_t *d = sav_file_read_best(path, &len, &v, &which);
    bool ok = false;
    if (!d && sav_file_seen_newer()) {
        log_msg(LOG_ERR, "save: global.sav was written by a newer engine (%s); it is left untouched and global "
                "data will not be saved in this session", sav_last_error());
        g_global_locked = true;
    } else if (d) {
        ok = sav_global_decode(d, len);
        if (ok && v.fmt == SAV_FMT_V1) convert_v1(path, which);
    } else if (sav_file_exists_any(path)) {
        log_msg(LOG_ERR, "save: global.sav and its backups are unusable (%s)", sav_last_error());
        sav_file_quarantine(path);
    }
    sav_file_free(d);
    mem_scratch_end(&s);
    return ok;
}
