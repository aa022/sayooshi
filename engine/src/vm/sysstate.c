/* vm/sysstate.c — skip/auto/script flags, save title, read flags and the read-skip decision
 * (TXT §6, SYS §4/§5, ENGINE_SPEC §2.8).
 *
 * Memory: the read flags of the bound pack (one byte per flag, per scene) live in a private arena sized exactly
 * for the pack; sys_bind_pack(NULL) / sys_shutdown destroy it. Read-flag records of scene code that is not loaded
 * (the other language's scenes) are kept verbatim on the tracked heap, at most RF_OTHER_MAX of them, across
 * language switches. Save decoding uses scratch memory and applies nothing until the whole chunk is good. */
#include "vm/sysstate.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "vm/clock.h"
#include "plat/input.h"
#include "sav/sav.h"
#include <string.h>

enum { RF_OTHER_MAX = 4096, RF_MAX = 1 << 20, RF_SCENES_MAX = 100000, RF_ARENA_SLACK = 1024 };
enum { CHUNK_SYS1 = FOURCC('S', 'Y', 'S', '1'), CHUNK_RFL1 = FOURCC('R', 'F', 'L', '1'),
       CHUNK_RFL2 = FOURCC('R', 'F', 'L', '2') };

static SysState g_sys = { .auto_moji_wait = 70, .auto_min_wait = 300, .msg_speed = 20 };
SysState *sys_state(void) { return &g_sys; }

void sys_reset_modes(void)
{
    g_sys.skip_request = g_sys.skip_active = g_sys.skip_suspended = g_sys.ctrl_skip = false;
    g_sys.auto_mode = false;
    g_sys.syscom_locked = false;
}

void sys_set_skip(bool on)
{
    g_sys.skip_request = on;
    g_sys.skip_active = on;                    /* the next TEXT re-checks read flags (TXT §6) */
    if (on) g_sys.skip_suspended = false;
}

static bool g_script_auto;

static struct {
    const Pack *pk;
    Arena arena;
    bool live;
    int nscene;
    uint8_t **rf;                              /* [nscene][nrf[s]] read flags (0/1) */
    int *nrf;
} RF;

typedef struct { WStr name; uint64_t hash; int n; uint8_t *bits; } RfOther;   /* bits: (n + 7) / 8 bytes, heap */
static RfOther *g_other;                       /* [RF_OTHER_MAX] once used */
static int g_nother;

static size_t rf_bytes(int n) { return (size_t)(n + 7) / 8; }
static int rf_count(const Scene *sc) { return sc->nread_flag > 0 && sc->nread_flag <= RF_MAX ? sc->nread_flag : 0; }

/* ---- binding ------------------------------------------------------------------------------------------------- */

static void rf_unbind(void)
{
    if (RF.live) arena_destroy(&RF.arena);
    memset(&RF, 0, sizeof RF);
}

static size_t rf_arena_size(const Pack *pk)
{
    size_t total = (size_t)pk->nscene * (sizeof(uint8_t *) + sizeof(int)) + RF_ARENA_SLACK;
    for (int i = 0; i < pk->nscene; i++) total += (size_t)rf_count(&pk->scene[i]) + 1 + 64;   /* + align/guard */
    return total;
}

void sys_bind_pack(const Pack *pk)
{
    if (RF.live && RF.pk == pk) return;        /* keep the flags across vm_init on the same pack */
    rf_unbind();
    if (!pk || pk->nscene <= 0) return;
    arena_init(&RF.arena, "sys.readflags", rf_arena_size(pk));
    RF.live = true;
    RF.pk = pk;
    RF.nscene = pk->nscene;
    RF.rf = ARENA_NEW_ARRAY(&RF.arena, uint8_t *, (size_t)RF.nscene);
    RF.nrf = ARENA_NEW_ARRAY(&RF.arena, int, (size_t)RF.nscene);
    for (int i = 0; i < RF.nscene; i++) {
        RF.nrf[i] = rf_count(&pk->scene[i]);
        RF.rf[i] = ARENA_NEW_ARRAY(&RF.arena, uint8_t, (size_t)RF.nrf[i] + 1);
    }
}

void sys_shutdown(void)
{
    rf_unbind();
    for (int i = 0; i < g_nother; i++) { ws_free(&g_other[i].name); mem_free(g_other[i].bits); }
    mem_free(g_other);
    g_other = NULL;
    g_nother = 0;
    ws_free(&g_sys.title);
}

void sys_set_script_auto(bool on) { g_script_auto = on; }

/* ---- skipping ------------------------------------------------------------------------------------------------- */

/* FUN_005706e0: skip active or forced (Ctrl), unless all skipping is disabled (64.6, L+0x339);
 * the read-skip disable flag (L+0x338) blocks only normal skip. */
bool sys_is_skipping(void)
{
    if (g_sys.skip_disabled) return false;
    if (g_sys.ctrl_skip || input_ctrl_held()) return true;
    if (g_sys.readskip_disabled) return false;
    return g_sys.skip_active;
}

static bool rf_valid(int scene, int rf) { return scene >= 0 && scene < RF.nscene && rf >= 0 && rf < RF.nrf[scene]; }

static bool sys_rf_get(int scene, int rf) { return rf_valid(scene, rf) && RF.rf[scene][rf] != 0; }

void sys_rf_set(int scene, int rf)
{
    if (rf_valid(scene, rf)) RF.rf[scene][rf] = 1;
}

/* FUN_00570c40 + FUN_00570d00, at every TEXT / koe / sel. */
void sys_readskip_check(int scene, int rf)
{
    bool is_read = sys_rf_get(scene, rf);
    if (!is_read && !sav_config()->skip_unread) {   /* config SKIP_UNREAD_MESSAGE (sav.h) */
        g_sys.skip_active = false;
        g_sys.skip_suspended = true;
    } else if (g_sys.skip_request && !g_sys.skip_suspended) {
        g_sys.skip_active = true;
    }
}

/* ---- SYS1: local state (snapshot): script flags, title, counters ---------------------------------------------- */

void sys_save(SavWriter *w)
{
    sav_put_chunk(w, CHUNK_SYS1);
    sav_put_u8(w, g_sys.skip_disabled); sav_put_u8(w, g_sys.readskip_disabled); sav_put_u8(w, g_script_auto);
    sav_put_u8(w, g_sys.syscom_locked);
    sav_put_wstr(w, &g_sys.title);
    for (int i = 0; i < COUNTER_CNT; i++) {
        const Counter *c = clock_counter(i);
        sav_put_u8(w, c->running); sav_put_u8(w, c->real); sav_put_u8(w, c->frame_mode); sav_put_u8(w, c->frame_loop);
        sav_put_i32(w, c->frame_start);
        sav_put_i32(w, c->frame_end);
        sav_put_i32(w, c->frame_time);
        sav_put_i32(w, c->value);
    }
}

bool sys_load(SavReader *r)
{
    if (!sav_find_chunk(r, CHUNK_SYS1)) return false;
    g_sys.skip_disabled = sav_get_u8(r); g_sys.readskip_disabled = sav_get_u8(r); g_script_auto = sav_get_u8(r);
    g_sys.syscom_locked = sav_get_u8(r);
    if (!sav_get_wstr(r, &g_sys.title)) return false;
    for (int i = 0; i < COUNTER_CNT; i++) {
        Counter *c = clock_counter(i);
        c->running = sav_get_u8(r);
        c->real = sav_get_u8(r);
        c->frame_mode = sav_get_u8(r);
        c->frame_loop = sav_get_u8(r);
        c->frame_start = sav_get_i32(r);
        c->frame_end = sav_get_i32(r);
        c->frame_time = sav_get_i32(r);
        c->value = sav_get_i32(r);
    }
    return sav_ok(r);
}

/* ---- RFL2: read flags (global data, SAVE_FORMAT.md §9.3) ------------------------------------------------------
 * Keyed by scene name + scene code hash: a scene whose code is the same in two languages shares its flags; records
 * for scene code that is not loaded are kept verbatim and written back. */

static void rf_other_put(const WStr *name, uint64_t hash, int n, const uint8_t *bits)
{
    for (int i = 0; i < g_nother; i++) {
        RfOther *o = &g_other[i];
        if (o->hash == hash && o->n == n && !ws_cmp_fold(&o->name, name)) {
            for (size_t k = 0; k < rf_bytes(n); k++) o->bits[k] |= bits[k];
            return;
        }
    }
    if (g_nother >= RF_OTHER_MAX) return;
    if (!g_other) g_other = mem_alloc_array(RF_OTHER_MAX, sizeof *g_other, MEM_VM);
    RfOther *o = &g_other[g_nother++];
    *o = (RfOther){ { NULL, 0, 0 }, hash, n, mem_dup(bits, rf_bytes(n) + 1, MEM_VM) };
    ws_copy(&o->name, name);
}

static void pack_bits(uint8_t *bits, const uint8_t *flags, int n)
{
    memset(bits, 0, rf_bytes(n) + 1);
    for (int k = 0; k < n; k++) if (flags[k]) bits[k >> 3] |= (uint8_t)(1u << (k & 7));
}

void sys_save_global(SavWriter *w)
{
    sav_put_chunk(w, CHUNK_RFL2);
    sav_put_u32(w, (uint32_t)(RF.nscene + g_nother));
    Scratch sc = mem_scratch_begin(NULL);
    uint8_t *bits = ARENA_NEW_ARRAY(sc.arena, uint8_t, rf_bytes(RF_MAX) + 1);
    for (int i = 0; i < RF.nscene; i++) {
        int n = RF.nrf[i];
        pack_bits(bits, RF.rf[i], n);
        sav_put_wstr(w, pack_scene_name(RF.pk, i));
        sav_put_u64(w, sav_scene_hash(i));
        sav_put_u32(w, (uint32_t)n);
        sav_put_bytes(w, bits, rf_bytes(n));
    }
    mem_scratch_end(&sc);
    for (int i = 0; i < g_nother; i++) {
        const RfOther *o = &g_other[i];
        sav_put_wstr(w, &o->name);
        sav_put_u64(w, o->hash);
        sav_put_u32(w, (uint32_t)o->n);
        sav_put_bytes(w, o->bits, rf_bytes(o->n));
    }
}

static int find_scene_fold(const WStr *name)
{
    for (int s = 0; s < RF.nscene; s++) if (!ws_cmp_fold(pack_scene_name(RF.pk, s), name)) return s;
    return -1;
}

static bool load_rfl1(SavReader *r)            /* format 1: one byte per flag, by scene name (Japanese pack assumed) */
{
    int n = sav_get_count(r, RF_SCENES_MAX, 8);
    WStr name = { 0 };
    Scratch sc = mem_scratch_begin(NULL);
    bool ok = sav_ok(r);
    for (int i = 0; i < n && ok; i++) {
        if (!sav_get_wstr(r, &name)) { ok = false; break; }
        int cnt = sav_get_count(r, RF_MAX, 1); /* <= RF_MAX: the buffer below always fits the scratch arena */
        if (!sav_ok(r)) { ok = false; break; }
        ArenaMark m = arena_mark(sc.arena);
        uint8_t *buf = ARENA_NEW_ARRAY(sc.arena, uint8_t, (size_t)cnt + 1);
        ok = sav_get_bytes(r, buf, (size_t)cnt);
        int idx = ok ? find_scene_fold(&name) : -1;
        if (idx >= 0) for (int k = 0; k < cnt && k < RF.nrf[idx]; k++) if (buf[k]) RF.rf[idx][k] = 1;
        arena_reset_to(sc.arena, m);
    }
    mem_scratch_end(&sc);
    ws_free(&name);
    return ok && sav_ok(r);
}

typedef struct { WStr name; uint64_t hash; int n; const uint8_t *bits; } RfRec;

/* Decodes up to n records into rec (bits in a); returns the number decoded completely. */
static int rfl2_decode(SavReader *r, Arena *a, RfRec *rec, int n)
{
    int got = 0;
    while (got < n && sav_ok(r)) {
        RfRec *x = &rec[got];
        (void)sav_get_wstr(r, &x->name);       /* failure is sticky in r */
        x->hash = sav_get_u64(r);
        x->n = sav_get_count(r, RF_MAX, 0);
        size_t nb = rf_bytes(x->n);
        uint8_t *bits = sav_ok(r) && nb <= sav_remaining(r) ? arena_try_alloc(a, nb + 1, 1) : NULL;
        if (!bits) { sav_fail(r); ws_free(&x->name); break; }
        (void)sav_get_bytes(r, bits, nb);      /* nb <= remaining: cannot fail */
        x->bits = bits;
        got++;
    }
    return got;
}

static void rfl2_apply(const RfRec *x)
{
    int idx = find_scene_fold(&x->name);
    if (idx >= 0 && x->hash == sav_scene_hash(idx) && x->n == RF.nrf[idx]) {
        for (int k = 0; k < x->n; k++) if (x->bits[k >> 3] >> (k & 7) & 1) RF.rf[idx][k] = 1;
    } else {
        rf_other_put(&x->name, x->hash, x->n, x->bits);
    }
}

bool sys_load_global(SavReader *r)
{
    if (!sav_find_chunk(r, CHUNK_RFL2)) {
        if (sav_legacy(r) && sav_find_chunk(r, CHUNK_RFL1)) return load_rfl1(r);
        return false;
    }
    /* Decode everything first; apply only if the whole chunk is good (a damaged chunk changes nothing). */
    int n = sav_get_count(r, RF_SCENES_MAX + RF_OTHER_MAX, 16);
    Scratch sc = mem_scratch_begin(NULL);
    RfRec *rec = arena_try_alloc(sc.arena, (size_t)(n + 1) * sizeof *rec, _Alignof(RfRec));
    int got = rec ? rfl2_decode(r, sc.arena, rec, n) : 0;
    if (!rec) sav_fail(r);
    bool ok = sav_ok(r) && got == n;
    for (int i = 0; i < got; i++) {
        if (ok) rfl2_apply(&rec[i]);
        ws_free(&rec[i].name);
    }
    mem_scratch_end(&sc);
    return ok;
}
