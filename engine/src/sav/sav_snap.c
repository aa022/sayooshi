/* sav/sav_snap.c — the in-RAM local snapshot (taken at msg_block 84 and savepoint 36), the "previous choice"
 * history, module serialisation order, snapshot load with call-stack code-hash check and rollback
 * (docs/SAVE_FORMAT.md §5-6, ENGINE_SPEC §2.11), and the scene code hashes.
 *
 * Memory: snapshot bytes are tracked heap blocks (replaced at every msg_block, so an arena would only grow); the
 * scene hash table is a heap block tied to the bound Pack and dropped by sav_session_reset / sav_shutdown. */
#include "sav/sav_internal.h"
#include "core/bin.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "vm/vm.h"
#include "txt/lang.h"
#include "vm/proc.h"
#include "vm/sysstate.h"
#include "gfx/gfx.h"
#include "gfx/button.h"
#include "txt/mwnd.h"
#include "txt/backlog.h"
#include "snd/audio.h"
#include "plat/input.h"
#include "ui/ui.h"
#include "fmt/gameexe.h"
#include "plat/fs.h"
#include <string.h>

/* ================================================================== language, code hashes */

static uint64_t fnv_u32(uint64_t h, uint32_t v)
{
    uint8_t b[4];
    bin_store_le32(b, v);
    return hash_fnv64(h, b, 4);
}

static uint64_t fnv_i32s(uint64_t h, const int32_t *v, int n)
{
    h = fnv_u32(h, (uint32_t)(n > 0 ? n : 0));
    for (int i = 0; v && i < n; i++) h = fnv_u32(h, (uint32_t)v[i]);
    return h;
}

static uint64_t fnv_wstr(uint64_t h, const WStr *s)
{
    int n = s->p ? s->len : 0;
    h = fnv_u32(h, (uint32_t)n);
    for (int k = 0; k < n; k++) {
        uint8_t b[2];
        bin_store_le16(b, s->p[k]);
        h = hash_fnv64(h, b, 2);
    }
    return h;
}

/* Bytecode + label/prop/read-flag tables, no strings (SAVE_FORMAT.md §5). */
static uint64_t scene_code_hash(const Scene *sc)
{
    uint64_t h = hash_fnv64(HASH_FNV64_INIT, "SCN1", 4);
    h = fnv_u32(h, sc->code ? sc->code_size : 0);
    if (sc->code) h = hash_fnv64(h, sc->code, sc->code_size);
    h = fnv_i32s(h, sc->label, sc->nlabel);
    h = fnv_i32s(h, sc->zlabel, sc->zlabel ? 1000 : 0);
    h = fnv_i32s(h, sc->scn_cmd, sc->nscn_cmd);
    h = fnv_u32(h, (uint32_t)(sc->prop && sc->nprop > 0 ? sc->nprop : 0));
    for (int k = 0; sc->prop && k < sc->nprop; k++) {
        h = fnv_u32(h, (uint32_t)sc->prop[k].form);
        h = fnv_u32(h, (uint32_t)sc->prop[k].size);
    }
    h = fnv_u32(h, (uint32_t)(sc->nread_flag > 0 ? sc->nread_flag : 0));
    return fnv_i32s(h, sc->cmd_label, sc->cmd_label ? 2 * sc->ncmd_label : 0);
}

static struct {
    const Pack *pk;              /* the pack the table was computed for ... */
    int         nscene;          /* ... and its scene count */
    uint64_t   *scene;           /* [nscene] tracked heap */
    uint64_t    pack;
} g_hash;

static void hashes_free(void)
{
    mem_free(g_hash.scene);
    memset(&g_hash, 0, sizeof g_hash);
}

static bool hashes_ready(void)
{
    const Pack *pk = vm_pack();
    if (!pk || pk->nscene <= 0) return false;
    if (pk == g_hash.pk && pk->nscene == g_hash.nscene && g_hash.scene) return true;
    hashes_free();
    g_hash.scene = mem_alloc_array((size_t)pk->nscene, sizeof *g_hash.scene, MEM_SAV);
    uint64_t h = hash_fnv64(HASH_FNV64_INIT, "PCK1", 4);
    h = fnv_u32(h, (uint32_t)pk->nscene);
    for (int i = 0; i < pk->nscene; i++) {
        uint64_t sh = g_hash.scene[i] = scene_code_hash(&pk->scene[i]);
        h = fnv_wstr(h, pack_scene_name(pk, i));
        h = fnv_u32(h, (uint32_t)sh);
        h = fnv_u32(h, (uint32_t)(sh >> 32));
    }
    h = fnv_u32(h, (uint32_t)(pk->inc_cmd && pk->ninc_cmd > 0 ? pk->ninc_cmd : 0));
    for (int i = 0; pk->inc_cmd && i < pk->ninc_cmd; i++) {
        h = fnv_u32(h, (uint32_t)pk->inc_cmd[i].scene);
        h = fnv_u32(h, (uint32_t)pk->inc_cmd[i].ofs);
    }
    g_hash.pack = h;
    g_hash.pk = pk;
    g_hash.nscene = pk->nscene;
    return true;
}

uint64_t sav_scene_hash(int scene)
{
    if (!hashes_ready() || scene < 0 || scene >= g_hash.nscene) return 0;
    return g_hash.scene[scene];
}

uint64_t sav_pack_hash(void) { return hashes_ready() ? g_hash.pack : 0; }

/* ================================================================== snapshot store */

enum { SEL_HIST_MAX = 4 };
static SavSnap g_snap;                       /* latest msg_block / savepoint snapshot (format-2 chunk sequence) */
static SavWriter *g_snap_w;                  /* encoder reused by sav_snapshot; freed by sav_session_reset */
static SavSnap g_sel_hist[SEL_HIST_MAX];     /* "previous choice" history, newest first (#SEL_SAVE.CNT = 1) */
static int g_nsel;
static bool g_loading;                       /* suppress snapshots while restoring */

void sav_snap_free(SavSnap *s)
{
    mem_free(s->data);
    ws_free(&s->title);
    memset(s, 0, sizeof *s);
}

/* Replace the bytes of s with a copy of d (d may point into the old bytes). The old block is reused when it is
 * large enough: snapshots are taken at every savepoint and are about the same size each time. */
static void snap_store(SavSnap *s, const uint8_t *d, size_t n)
{
    if (s->data && n > 0 && mem_size(s->data) >= n) {
        memmove(s->data, d, n);
        s->len = n;
        return;
    }
    uint8_t *copy = mem_dup(d, n, MEM_SAV);
    mem_free(s->data);
    s->data = copy;
    s->len = n;
}

void sav_snap_copy(SavSnap *dst, const SavSnap *src)
{
    if (dst == src) return;
    sav_snap_free(dst);
    if (!src->data) return;
    snap_store(dst, src->data, src->len);
    dst->time = src->time;
    ws_copy(&dst->title, &src->title);
}

bool sav_snap_set(SavSnap *s, const SavWriter *w, int64_t t, const WStr *title)
{
    size_t len;
    const uint8_t *d = sav_writer_data(w, &len);
    if (!d) return false;
    snap_store(s, d, len);
    s->time = t;
    ws_copy(&s->title, title);
    return true;
}

static void sel_hist_clear(void)
{
    for (int i = 0; i < SEL_HIST_MAX; i++) sav_snap_free(&g_sel_hist[i]);
    g_nsel = 0;
}

void sav_session_reset(void)
{
    hashes_free();
    sav_writer_free(g_snap_w);
    g_snap_w = NULL;
    sav_snap_free(&g_snap);
    sel_hist_clear();
}

SavSnap *sav_snap_cur(void) { return &g_snap; }

/* Order of §2.11: vm, proc, sys, gfx, mwnd, audio; then backlog (last 100) and button groups. */
static const SavModuleIO MODULES[] = {
    { FOURCC('V', 'M', ' ', ' '), "vm",      vm_save,      vm_load,      true  },
    { FOURCC('P', 'R', 'O', 'C'), "proc",    proc_save,    proc_load,    true  },
    { FOURCC('S', 'Y', 'S', ' '), "sys",     sys_save,     sys_load,     false },
    { FOURCC('G', 'F', 'X', ' '), "gfx",     gfx_save,     gfx_load,     false },
    { FOURCC('M', 'W', 'N', 'D'), "mwnd",    mwnd_save,    mwnd_load,    false },
    { FOURCC('A', 'U', 'D', ' '), "audio",   audio_save,   audio_load,   false },
    { FOURCC('B', 'L', 'O', 'G'), "backlog", backlog_save, backlog_load, false },
    { FOURCC('B', 'T', 'N', 'G'), "buttons", btn_save,     btn_load,     false },
};
enum { NMODULES = (int)COUNT_OF(MODULES) };

const SavModuleIO *sav_modules(int *count)
{
    *count = NMODULES;
    return MODULES;
}

/* `u32 n, n x (str16 scene name, u64 code hash)` of the live call stack. */
static void put_stack(SavWriter *w)
{
    const Pack *pk = vm_pack();
    int sc[SAV_STACK_MAX];
    int n = pk ? vm_stack_scenes(sc, SAV_STACK_MAX) : 0;
    ASSERT(n >= 0 && n <= SAV_STACK_MAX);
    sav_put_u32(w, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        ASSERT(sc[i] >= 0 && sc[i] < pk->nscene);
        sav_put_wstr(w, pack_scene_name(pk, sc[i]));
        sav_put_u64(w, sav_scene_hash(sc[i]));
    }
}

static void snap_encode_into(SavWriter *w, int64_t t)
{
    sav_put_chunk(w, SAV_TAG_SHDR);
    sav_put_i64(w, t);
    sav_put_wstr(w, &sys_state()->title);
    sav_put_str8(w, lang_code());   /* the slot's language (LANG/SHDR, load rule SAVE_FORMAT.md §5) */
    put_stack(w);
    for (int i = 0; i < NMODULES; i++) {
        sav_put_chunk(w, MODULES[i].tag);
        sav_put_enter(w);
        MODULES[i].save(w);
        sav_put_leave(w);
    }
}

SavWriter *sav_snap_encode(int64_t t)
{
    SavWriter *w = sav_writer_new();
    snap_encode_into(w, t);
    return w;
}

void sav_snapshot(void)
{
    if (g_loading) return;
    int64_t t = sav_now_unix();
    if (g_snap_w) sav_writer_reset(g_snap_w); else g_snap_w = sav_writer_new();
    snap_encode_into(g_snap_w, t);
    if (!sav_snap_set(&g_snap, g_snap_w, t, &sys_state()->title))
        log_msg(LOG_ERR, "save: snapshot larger than %d bytes; the previous one is kept", SAV_MAX_FILE_BYTES);
    log_trace("SAVE", "snapshot bytes=%u", (unsigned)g_snap.len);
}

int sav_cmd_savepoint(void)
{
    /* SYS §4 (0x659a46): push 1, snapshot, pop, push 0 — the snapshot's int stack holds the 1, so after a load
     * the script continues with 36 returning 1. The caller pushes our return value. */
    vm_push_int(1);
    sav_snapshot();
    (void)vm_pop_int();   /* the 1 pushed above */
    return 0;
}

void sav_selpoint(void)
{
    if (!g_snap.data) return;
    int cap = core_clamp_i32(gx_int("SEL_SAVE.CNT", 0, 1), 1, SEL_HIST_MAX);
    /* Newest first; drop the oldest beyond capacity (TXT §3.17). */
    sav_snap_free(&g_sel_hist[cap - 1]);
    for (int i = cap - 1; i > 0; i--) g_sel_hist[i] = g_sel_hist[i - 1];
    memset(&g_sel_hist[0], 0, sizeof g_sel_hist[0]);
    sav_snap_copy(&g_sel_hist[0], &g_snap);
    g_nsel = g_nsel + 1 > cap ? cap : g_nsel + 1;
}

/* ================================================================== header and call-stack check */

static void stack_mismatch_note(char why[SAV_NOTE_CAP], const char *lang, bool other_lang, const WStr *nm, bool missing)
{
    SB_ON_STACK(name, 128);
    (void)ws_to_utf8(nm, &name);   /* an odd or overlong scene name only changes the note text */
    StrBuf note;
    sb_init(&note, why, SAV_NOTE_CAP);
    if (other_lang)
        (void)sb_fmt(&note, "made with the '%s' text; scene %s differs in the current '%s' text", lang, sb_cstr(&name),
                     lang_code());   /* a cut note is still a note */
    else
        (void)sb_fmt(&note, "made with different game data (scene %s %s)", sb_cstr(&name),
                     missing ? "is missing" : "has different code");
}

/* Why a save of language `lang` whose call stack does not match is refused (the dialog words it per language). */
SavSlotState sav_refusal_state(const char *lang)
{
    LangId id = lang_parse(lang);
    return id != LANG_NONE && id != lang_id() ? SAV_SLOT_OTHER_LANG : SAV_SLOT_OTHER_DATA;
}

/* Call-stack scene check (SAVE_FORMAT.md §6): every scene on the saved call stack must exist in the current pack
 * with the same code hash; the language may differ. Reads `u32 n, n x (str16 name, u64 hash)`. */
SavSlotState sav_check_stack(SavReader *r, const char *lang, char why[SAV_NOTE_CAP])
{
    why[0] = 0;
    int n = sav_get_count(r, SAV_STACK_MAX, 12);
    const Pack *pk = vm_pack();
    bool other_lang = lang && *lang && strcmp(lang, lang_code()) != 0;
    WStr nm = { 0 };
    bool ok = sav_ok(r);
    for (int i = 0; i < n && ok; i++) {
        (void)sav_get_wstr(r, &nm);   /* checked with sav_ok below */
        uint64_t h = sav_get_u64(r);
        ok = sav_ok(r);
        if (!ok || !pk) continue;     /* no game data bound (tools): cannot check */
        int idx = pack_find_scene(pk, &nm);
        if (idx >= 0 && sav_scene_hash(idx) == h) continue;
        stack_mismatch_note(why, lang, other_lang, &nm, idx < 0);
        ws_free(&nm);
        return sav_refusal_state(lang);
    }
    ws_free(&nm);
    if (!ok) str_fmt_cut(why, SAV_NOTE_CAP, "corrupt call-stack record");
    return ok ? SAV_SLOT_OK : SAV_SLOT_CORRUPT;
}

static bool head_v2(SavReader *r, SavSnapHead *h)
{
    bool ok = sav_find_chunk(r, SAV_TAG_SHDR);
    h->time = sav_get_i64(r);
    (void)sav_get_wstr(r, &h->title);
    (void)sav_get_str8(r, h->lang, sizeof h->lang);   /* both checked with sav_ok */
    ok = ok && sav_ok(r);
    h->stack_ok = ok && sav_check_stack(r, h->lang, h->why) == SAV_SLOT_OK;
    return ok && sav_ok(r);
}

static bool head_v1(SavReader *r, SavSnapHead *h)
{
    bool ok = sav_find_chunk(r, SAV_TAG_HEAD) && sav_get_u32(r) == SAV_OLD_MAGIC && sav_get_i32(r) == 1;
    h->time = sav_get_i64(r);
    (void)sav_get_wstr(r, &h->title);   /* checked with sav_ok */
    ok = ok && sav_ok(r);
    str_fmt_cut(h->lang, sizeof h->lang, "%s", lang_code_of(LANG_JA));   /* format 1: the Japanese release only */
    h->stack_ok = lang_id() == LANG_JA;
    if (!h->stack_ok) str_fmt_cut(h->why, sizeof h->why, "old-format save made with the 'ja' text");
    return ok;
}

/* Parses SHDR (format 2) or HEAD (format 1) of a snapshot without touching the engine state. h->title is owned
 * by the caller on success and empty on failure. */
bool sav_snap_head(const uint8_t *data, size_t len, bool legacy, SavSnapHead *h)
{
    memset(h, 0, sizeof *h);
    SavReader *r = sav_reader_new_ex(data, len, legacy);
    bool ok = legacy ? head_v1(r, h) : head_v2(r, h);
    sav_reader_free(r);
    if (!ok) ws_free(&h->title);
    return ok;
}

/* ================================================================== load */

static bool load_modules(const uint8_t *data, size_t len, bool legacy)
{
    SavReader *r = sav_reader_new_ex(data, len, legacy);
    bool ok = true;
    for (int i = 0; i < NMODULES && ok; i++) {
        const SavModuleIO *m = &MODULES[i];
        bool mok = sav_find_chunk(r, m->tag) && sav_get_enter(r) && m->load(r) && sav_ok(r);
        sav_get_leave(r);
        if (mok) continue;
        log_msg(m->critical ? LOG_ERR : LOG_WARN, "save: %s state did not load", m->name);
        if (m->critical) ok = false;
        sav_reset_fail(r);   /* non-critical: keep going with the other modules */
    }
    sav_reader_free(r);
    return ok;
}

/* Restore the modules; a load that fails half-way puts the running state back so no broken game is left. */
static bool restore_with_rollback(const uint8_t *data, size_t len, bool legacy)
{
    SavWriter *keep = vm_pack() ? sav_snap_encode(g_snap.time) : NULL;
    g_loading = true;
    ui_close_all();                      /* drop dialogs and button state before restoring */
    btn_reset_all();
    bool ok = load_modules(data, len, legacy);
    size_t kn = 0;
    const uint8_t *kd = keep && !ok ? sav_writer_data(keep, &kn) : NULL;
    if (kd && !load_modules(kd, kn, false)) log_msg(LOG_ERR, "save: could not restore the previous state either");
    g_loading = false;
    sav_writer_free(keep);
    return ok;
}

/* The loaded point becomes the latest snapshot, so saving right away writes the same point. Format-1 data is
 * re-encoded in format 2 (right after the restore, the live state is exactly the loaded one). */
static void adopt_loaded(const uint8_t *data, size_t len, bool legacy, const SavSnapHead *h)
{
    SavWriter *conv = legacy ? sav_snap_encode(h->time) : NULL;
    size_t cn = 0;
    const uint8_t *cd = conv ? sav_writer_data(conv, &cn) : NULL;
    if (cd) snap_store(&g_snap, cd, cn);
    else if (!legacy && data != g_snap.data) snap_store(&g_snap, data, len);
    sav_writer_free(conv);
    g_snap.time = h->time;
    ws_copy(&g_snap.title, &h->title);
}

bool sav_snap_load(const uint8_t *data, size_t len, bool legacy, bool check)
{
    SavSnapHead h;
    sav_clear_error();
    if (!sav_snap_head(data, len, legacy, &h)) {
        sav_set_error("snapshot header is missing or corrupt");
        log_msg(LOG_ERR, "save: %s", sav_last_error());
        return false;
    }
    bool ok = !check || h.stack_ok;
    if (!ok) {
        sav_set_error("%s", h.why);
        log_msg(LOG_WARN, "save: load refused: %s", sav_last_error());
    } else if (!restore_with_rollback(data, len, legacy)) {
        if (!sav_last_error()[0]) sav_set_error("save data is damaged (a required part did not load)");
        ok = false;
    } else {
        adopt_loaded(data, len, legacy, &h);   /* before the title: a format-1 conversion keeps the restored one */
        ws_copy(&sys_state()->title, &h.title);
        input_clear();
        /* #LOAD.WIPE = 0, 1000: crossfade from the current frame (§2.11). */
        gfx_start_load_wipe(gx_int("LOAD.WIPE", 0, 0), gx_int("LOAD.WIPE", 1, 1000));
        log_trace("SAVE", "snapshot loaded bytes=%u", (unsigned)len);
    }
    ws_free(&h.title);
    return ok;
}

bool sav_return_sel(void)
{
    if (g_nsel <= 0 || !g_sel_hist[0].data) return false;
    SavSnap s = { 0 };   /* a private copy: loading may run sav_selpoint, which rotates the history */
    sav_snap_copy(&s, &g_sel_hist[0]);
    bool ok = sav_snap_load(s.data, s.len, false, false);
    sav_snap_free(&s);
    return ok;
}

void sav_shutdown(void)
{
    sav_session_reset();
    sav_global_keep_clear();
    sav_global_set_locked(false);
    sav_dir_free();
    sav_clear_error();
}
