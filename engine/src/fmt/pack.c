/* fmt/pack.c — Scene.pck loader (FORMATS "Scene.pck header" / "Scene blob header"; exe FUN_0054b960).
 * File: 23 x i32 header of (offset, count) pairs, then tables and the scene data. Per scene:
 * [xor crypto_exe_key if exe_angou_mod] -> xor crypto_scene_table -> byte LZSS -> 33 x i32 header + tables.
 *
 * Every offset/count pair from the file becomes a checked sub-reader (table_at) before anything is read, so a
 * corrupt file fails with a message instead of reading out of range. Memory: all results go to the data arena
 * (pack.h); the encrypted copy of a scene is scratch memory of one call. On failure the data arena is rolled back
 * to where the load started. */
#include "fmt/pack.h"
#include "core/bin.h"
#include "core/log.h"
#include "fmt/crypto.h"
#include "fmt/lzss.h"
#include "plat/fs.h"
#include <string.h>

enum { PH_HEADER_SIZE, PH_INC_PROP, PH_INC_PROP_CNT, PH_INC_PROP_NAME_IDX, PH_INC_PROP_NAME_IDX_CNT,
       PH_INC_PROP_NAME, PH_INC_PROP_NAME_CNT, PH_INC_CMD, PH_INC_CMD_CNT, PH_INC_CMD_NAME_IDX,
       PH_INC_CMD_NAME_IDX_CNT, PH_INC_CMD_NAME, PH_INC_CMD_NAME_CNT, PH_SCN_NAME_IDX, PH_SCN_NAME_IDX_CNT,
       PH_SCN_NAME, PH_SCN_NAME_CNT, PH_SCN_DATA_IDX, PH_SCN_DATA_IDX_CNT, PH_SCN_DATA, PH_SCN_DATA_CNT,
       PH_EXE_ANGOU_MOD, PH_ORIG_SRC_HEADER_SIZE, PH_COUNT };

enum { SH_HEADER_SIZE, SH_SCN, SH_SCN_SIZE, SH_STR_IDX, SH_STR_IDX_CNT, SH_STR, SH_STR_CNT, SH_LABEL,
       SH_LABEL_CNT, SH_Z_LABEL, SH_Z_LABEL_CNT, SH_CMD_LABEL, SH_CMD_LABEL_CNT, SH_SCN_PROP, SH_SCN_PROP_CNT,
       SH_SCN_PROP_NAME_IDX, SH_SCN_PROP_NAME_IDX_CNT, SH_SCN_PROP_NAME, SH_SCN_PROP_NAME_CNT, SH_SCN_CMD,
       SH_SCN_CMD_CNT, SH_SCN_CMD_NAME_IDX, SH_SCN_CMD_NAME_IDX_CNT, SH_SCN_CMD_NAME, SH_SCN_CMD_NAME_CNT,
       SH_CALL_PROP_NAME_IDX, SH_CALL_PROP_NAME_IDX_CNT, SH_CALL_PROP_NAME, SH_CALL_PROP_NAME_CNT,
       SH_NAMAE, SH_NAMAE_CNT, SH_READ_FLAG, SH_READ_FLAG_CNT, SH_COUNT };

enum {
    ZLABEL_CNT = 1000,
    NAME_ENTRY = 8,                      /* name / string index entry: i32 offset, i32 length (UTF-16 units) */
    PAIR_ENTRY = 8,                      /* {i32, i32} records: props, inc_cmd, cmd_label, data index */
    SCN_MIN_DATA = LZSS_HEADER,
    STR_XOR = 0x7087                     /* string i: every unit XOR (0x7087 * i) (FUN_0054c9c0) */
};

/* ------------------------------------------------------------------------------------------------ tables */

static Rd rd_bad(void)
{
    Rd r = rd_make(NULL, 0);
    rd_fail(&r);
    return r;
}

/* Reader over cnt elements of esize bytes at ofs of whole; a failed reader when the table is not inside. */
static Rd table_at(const Rd *whole, int32_t ofs, int32_t cnt, size_t esize)
{
    size_t len;
    if (ofs < 0 || cnt < 0 || !core_mul_sz((size_t)cnt, esize, &len)) return rd_bad();
    return rd_sub_at(whole, (size_t)ofs, len);
}

/* Reader from ofs to the end of whole (string pools: entries say how much they use). */
static Rd pool_at(const Rd *whole, int32_t ofs)
{
    if (ofs < 0 || (size_t)ofs > whole->n) return rd_bad();
    return rd_sub_at(whole, (size_t)ofs, whole->n - (size_t)ofs);
}

/* A name table: count index entries {offset, length} into a UTF-16LE pool. */
typedef struct { Rd idx, pool; int count; } NameTab;

static NameTab name_tab(const Rd *whole, int32_t idx_ofs, int32_t cnt, int32_t pool_ofs)
{
    NameTab t = { table_at(whole, idx_ofs, cnt, NAME_ENTRY), pool_at(whole, pool_ofs), cnt };
    return t;
}

static bool name_tab_ok(const NameTab *t) { return rd_ok(&t->idx) && rd_ok(&t->pool); }

/* Units of entry i: *p = the bytes in the pool, *len = length in units; false when out of range. */
static bool name_view(const NameTab *t, int i, const uint8_t **p, int32_t *len)
{
    if (i < 0 || i >= t->count) return false;
    Rd e = rd_sub_at(&t->idx, (size_t)i * NAME_ENTRY, NAME_ENTRY);
    int32_t o = rd_i32le(&e), n = rd_i32le(&e);
    size_t off, bytes;
    if (!rd_ok(&e) || o < 0 || n < 0 || !core_mul_sz((size_t)o, 2, &off) || !core_mul_sz((size_t)n, 2, &bytes))
        return false;
    Rd s = rd_sub_at(&t->pool, off, bytes);
    if (!rd_ok(&s)) return false;
    *p = s.p;
    *len = n;
    return true;
}

/* Entry i as a read-only WStr view in arena memory (pack.h); "" when the entry is out of range. */
static bool name_to_arena(const NameTab *t, int i, Arena *a, WStr *out)
{
    static uint16_t empty[1];
    const uint8_t *p;
    int32_t n;
    *out = (WStr){ empty, 0, 0 };
    if (!name_view(t, i, &p, &n)) return false;
    uint16_t *u = arena_try_alloc(a, ((size_t)n + 1) * sizeof *u, _Alignof(uint16_t));
    if (!u) return false;
    for (int32_t k = 0; k < n; k++) u[k] = bin_load_le16(p + 2 * (size_t)k);
    *out = (WStr){ u, n, 0 };
    return true;
}

static void pack_warn_name(const char *what, int i) { log_msg(LOG_WARN, "pack: %s %d: bad name entry", what, i); }

/* ------------------------------------------------------------------------------------------------ scenes */

/* Reads count i32 from r into dst (count may be 0). */
static void read_ints(Rd r, int32_t *dst, int count)
{
    for (int i = 0; i < count; i++) dst[i] = rd_i32le(&r);
}

typedef struct { Rd label, zlabel, cmd_label, prop, scn_cmd, namae, read_flag; int nz; } SceneTabs;

/* Validates every table of the blob header h; false if one is outside the blob. */
static bool scene_tables(const Rd *b, const int32_t *h, SceneTabs *t)
{
    t->label = table_at(b, h[SH_LABEL], h[SH_LABEL_CNT], 4);
    t->zlabel = table_at(b, h[SH_Z_LABEL], h[SH_Z_LABEL_CNT], 4);
    t->cmd_label = table_at(b, h[SH_CMD_LABEL], h[SH_CMD_LABEL_CNT], PAIR_ENTRY);
    t->prop = table_at(b, h[SH_SCN_PROP], h[SH_SCN_PROP_CNT], PAIR_ENTRY);
    t->scn_cmd = table_at(b, h[SH_SCN_CMD], h[SH_SCN_CMD_CNT], 4);
    t->namae = table_at(b, h[SH_NAMAE], h[SH_NAMAE_CNT], 4);
    t->read_flag = table_at(b, h[SH_READ_FLAG], h[SH_READ_FLAG_CNT], 4);
    t->nz = h[SH_Z_LABEL_CNT] < ZLABEL_CNT ? h[SH_Z_LABEL_CNT] : ZLABEL_CNT;
    NameTab str = name_tab(b, h[SH_STR_IDX], h[SH_STR_IDX_CNT], h[SH_STR]);
    NameTab n0 = name_tab(b, h[SH_SCN_PROP_NAME_IDX], h[SH_SCN_PROP_NAME_IDX_CNT], h[SH_SCN_PROP_NAME]);
    NameTab n1 = name_tab(b, h[SH_SCN_CMD_NAME_IDX], h[SH_SCN_CMD_NAME_IDX_CNT], h[SH_SCN_CMD_NAME]);
    NameTab n2 = name_tab(b, h[SH_CALL_PROP_NAME_IDX], h[SH_CALL_PROP_NAME_IDX_CNT], h[SH_CALL_PROP_NAME]);
    Rd code = table_at(b, h[SH_SCN], h[SH_SCN_SIZE], 1);
    return rd_ok(&code) && rd_ok(&t->label) && rd_ok(&t->zlabel) && rd_ok(&t->cmd_label) && rd_ok(&t->prop) &&
           rd_ok(&t->scn_cmd) && rd_ok(&t->namae) && rd_ok(&t->read_flag) && name_tab_ok(&str) &&
           name_tab_ok(&n0) && name_tab_ok(&n1) && name_tab_ok(&n2);
}

/* Pointers into the blob for code, strings and name tables (validated by scene_tables). */
static void scene_views(Scene *sc, const int32_t *h)
{
    const uint8_t *d = sc->blob;
    sc->code = d + h[SH_SCN];
    sc->code_size = (uint32_t)h[SH_SCN_SIZE];
    sc->nstr = h[SH_STR_IDX_CNT];
    sc->priv_.str_idx = d + h[SH_STR_IDX];
    sc->priv_.str = d + h[SH_STR];
    sc->nlabel = h[SH_LABEL_CNT];
    sc->nscn_cmd = h[SH_SCN_CMD_CNT];
    sc->nread_flag = h[SH_READ_FLAG_CNT];
    sc->nnamae = h[SH_NAMAE_CNT];
    sc->ncmd_label = h[SH_CMD_LABEL_CNT];
    sc->ncall_prop_name = h[SH_CALL_PROP_NAME_IDX_CNT];
    sc->nprop = h[SH_SCN_PROP_CNT];
    sc->priv_.nm_idx[0] = d + h[SH_SCN_PROP_NAME_IDX]; sc->priv_.nm[0] = d + h[SH_SCN_PROP_NAME];
    sc->priv_.nm_idx[1] = d + h[SH_SCN_CMD_NAME_IDX];  sc->priv_.nm[1] = d + h[SH_SCN_CMD_NAME];
    sc->priv_.nm_idx[2] = d + h[SH_CALL_PROP_NAME_IDX]; sc->priv_.nm[2] = d + h[SH_CALL_PROP_NAME];
}

/* Aligned, endian-explicit copies of the int tables (z_label padded to 1000 entries) and the prop table. */
static bool scene_copy_tables(Scene *sc, const SceneTabs *t, Arena *a)
{
    size_t total = (size_t)sc->nlabel + ZLABEL_CNT + (size_t)sc->nscn_cmd + (size_t)sc->nread_flag +
                   (size_t)sc->nnamae + 2 * (size_t)sc->ncmd_label;   /* each <= blob_size / 4: no overflow */
    int32_t *w = arena_try_alloc(a, total * sizeof *w, _Alignof(int32_t));
    sc->prop = arena_try_alloc(a, (size_t)(sc->nprop ? sc->nprop : 1) * sizeof *sc->prop, _Alignof(PropDecl));
    if (!w || !sc->prop) return false;
    sc->priv_.tabs = w;
    sc->label = w;          read_ints(t->label, w, sc->nlabel);              w += sc->nlabel;
    sc->zlabel = w;         read_ints(t->zlabel, w, t->nz);                  w += ZLABEL_CNT;
    sc->scn_cmd = w;        read_ints(t->scn_cmd, w, sc->nscn_cmd);          w += sc->nscn_cmd;
    sc->read_flag_line = w; read_ints(t->read_flag, w, sc->nread_flag);      w += sc->nread_flag;
    sc->namae = w;          read_ints(t->namae, w, sc->nnamae);              w += sc->nnamae;
    sc->cmd_label = w;      read_ints(t->cmd_label, w, 2 * sc->ncmd_label);
    Rd p = t->prop;
    for (int i = 0; i < sc->nprop; i++) {
        sc->prop[i].form = rd_i32le(&p);
        sc->prop[i].size = rd_i32le(&p);
        sc->prop[i].name_id = i;                                   /* index into scn_prop_name */
    }
    return true;
}

static bool scene_parse(Scene *sc, Arena *a)
{
    Rd b = rd_make(sc->blob, sc->blob_size);
    int32_t h[SH_COUNT];
    for (int i = 0; i < SH_COUNT; i++) h[i] = rd_i32le(&b);
    SceneTabs t;
    if (!rd_ok(&b) || !scene_tables(&b, h, &t)) return false;
    scene_views(sc, h);
    return scene_copy_tables(sc, &t, a);
}

/* Decrypts a copy of the scene data (scratch) and decodes it into the data arena. */
static bool scene_decode(Scene *sc, const uint8_t *enc, size_t n, bool exe_angou, Arena *a)
{
    Scratch s = mem_scratch_begin(a);
    uint8_t *d = arena_try_alloc(s.arena, n, 1);
    bool ok = false;
    if (d) {
        memcpy(d, enc, n);
        if (exe_angou) crypto_xor_cycle(d, n, crypto_exe_key, sizeof crypto_exe_key, 0);
        crypto_xor_cycle(d, n, crypto_scene_table, sizeof crypto_scene_table, 0);
        uint32_t len = 0;
        sc->blob = lzss_decode_arena(a, d, n, &len);
        sc->blob_size = len;
        ok = sc->blob && scene_parse(sc, a);
    }
    mem_scratch_end(&s);
    return ok;
}

/* ------------------------------------------------------------------------------------------------ pack */

typedef struct { Rd whole; int32_t h[PH_COUNT]; Arena *a; const char *what; } PackCtx;

static bool pack_header(PackCtx *c, size_t n)
{
    for (int i = 0; i < PH_COUNT; i++) c->h[i] = rd_i32le(&c->whole);
    const int32_t *h = c->h;
    if (!rd_ok(&c->whole)) { log_msg(LOG_ERR, "pack: %s is truncated (%zu bytes)", c->what, n); return false; }
    Rd t[4] = { table_at(&c->whole, h[PH_INC_PROP], h[PH_INC_PROP_CNT], PAIR_ENTRY),
                table_at(&c->whole, h[PH_INC_CMD], h[PH_INC_CMD_CNT], PAIR_ENTRY),
                table_at(&c->whole, h[PH_SCN_DATA_IDX], h[PH_SCN_DATA_IDX_CNT], PAIR_ENTRY),
                pool_at(&c->whole, h[PH_SCN_DATA]) };
    NameTab nt[3] = { name_tab(&c->whole, h[PH_INC_PROP_NAME_IDX], h[PH_INC_PROP_NAME_IDX_CNT], h[PH_INC_PROP_NAME]),
                      name_tab(&c->whole, h[PH_INC_CMD_NAME_IDX], h[PH_INC_CMD_NAME_IDX_CNT], h[PH_INC_CMD_NAME]),
                      name_tab(&c->whole, h[PH_SCN_NAME_IDX], h[PH_SCN_NAME_IDX_CNT], h[PH_SCN_NAME]) };
    bool ok = h[PH_SCN_NAME_IDX_CNT] == h[PH_SCN_DATA_IDX_CNT];
    for (int i = 0; i < 4; i++) ok = ok && rd_ok(&t[i]);
    for (int i = 0; i < 3; i++) ok = ok && name_tab_ok(&nt[i]);
    if (!ok) log_msg(LOG_ERR, "pack: bad header in %s", c->what);
    return ok;
}

/* Arena array of count elements (count <= file size / 8, so no overflow); NULL (logged) when the data arena has
 * no room. */
static void *pack_array(PackCtx *c, int count, size_t size, size_t align)
{
    size_t bytes = (size_t)(count > 0 ? count : 1) * size;
    void *p = arena_try_alloc(c->a, bytes, align);
    if (!p) log_msg(LOG_ERR, "pack: %s: data arena exhausted (%zu more bytes)", c->what, bytes);
    return p;
}

static bool pack_inc(Pack *pk, PackCtx *c)
{
    const int32_t *h = c->h;
    pk->ninc_prop = h[PH_INC_PROP_CNT];
    pk->ninc_cmd = h[PH_INC_CMD_CNT];
    pk->inc_prop = pack_array(c, pk->ninc_prop, sizeof *pk->inc_prop, _Alignof(PropDecl));
    pk->inc_prop_name_ = pack_array(c, pk->ninc_prop, sizeof(WStr), _Alignof(WStr));
    pk->inc_cmd = pack_array(c, pk->ninc_cmd, sizeof *pk->inc_cmd, _Alignof(IncCmd));
    pk->inc_cmd_name_ = pack_array(c, pk->ninc_cmd, sizeof(WStr), _Alignof(WStr));
    if (!pk->inc_prop || !pk->inc_prop_name_ || !pk->inc_cmd || !pk->inc_cmd_name_) return false;
    Rd prop = table_at(&c->whole, h[PH_INC_PROP], pk->ninc_prop, PAIR_ENTRY);
    NameTab pn = name_tab(&c->whole, h[PH_INC_PROP_NAME_IDX], h[PH_INC_PROP_NAME_IDX_CNT], h[PH_INC_PROP_NAME]);
    for (int i = 0; i < pk->ninc_prop; i++) {
        pk->inc_prop[i].form = rd_i32le(&prop);
        pk->inc_prop[i].size = rd_i32le(&prop);
        pk->inc_prop[i].name_id = i;
        if (!name_to_arena(&pn, i, c->a, &pk->inc_prop_name_[i]) && i < pn.count) pack_warn_name("inc_prop", i);
    }
    Rd cmd = table_at(&c->whole, h[PH_INC_CMD], pk->ninc_cmd, PAIR_ENTRY);
    NameTab cn = name_tab(&c->whole, h[PH_INC_CMD_NAME_IDX], h[PH_INC_CMD_NAME_IDX_CNT], h[PH_INC_CMD_NAME]);
    for (int i = 0; i < pk->ninc_cmd; i++) {
        pk->inc_cmd[i].scene = rd_i32le(&cmd);
        pk->inc_cmd[i].ofs = rd_i32le(&cmd);
        if (!name_to_arena(&cn, i, c->a, &pk->inc_cmd_name_[i]) && i < cn.count) pack_warn_name("inc_cmd", i);
    }
    return true;
}

static bool pack_scene(Pack *pk, PackCtx *c, const NameTab *names, Rd *data_idx, const Rd *data, int i)
{
    Scene *sc = &pk->scene[i];
    if (!name_to_arena(names, i, c->a, &sc->name_)) pack_warn_name("scene", i);
    int32_t o = rd_i32le(data_idx), sz = rd_i32le(data_idx);
    Rd enc = o >= 0 && sz >= SCN_MIN_DATA ? rd_sub_at(data, (size_t)o, (size_t)sz) : rd_bad();
    if (!rd_ok(&enc)) {
        log_msg(LOG_ERR, "pack: scene %d: bad data range", i);
        return false;
    }
    if (!scene_decode(sc, enc.p, enc.n, pk->exe_angou_mod != 0, c->a)) {
        Scratch s = mem_scratch_begin(c->a);
        log_msg(LOG_ERR, "pack: scene %d (%s): decode failed", i,
                ws_utf8_arena(s.arena, &sc->name_, WS_UTF8_SHORT_MAX));
        mem_scratch_end(&s);
        return false;
    }
    return true;
}

static bool pack_scenes(Pack *pk, PackCtx *c)
{
    const int32_t *h = c->h;
    pk->nscene = h[PH_SCN_DATA_IDX_CNT];
    pk->scene = pack_array(c, pk->nscene, sizeof *pk->scene, _Alignof(Scene));
    if (!pk->scene) return false;
    NameTab names = name_tab(&c->whole, h[PH_SCN_NAME_IDX], h[PH_SCN_NAME_IDX_CNT], h[PH_SCN_NAME]);
    Rd data_idx = table_at(&c->whole, h[PH_SCN_DATA_IDX], pk->nscene, PAIR_ENTRY);
    Rd data = pool_at(&c->whole, h[PH_SCN_DATA]);
    for (int i = 0; i < pk->nscene; i++)
        if (!pack_scene(pk, c, &names, &data_idx, &data, i)) return false;
    for (int i = 0; i < pk->ninc_cmd; i++)
        if (pk->inc_cmd[i].scene < 0 || pk->inc_cmd[i].scene >= pk->nscene)
            log_msg(LOG_WARN, "pack: inc_cmd %d points to scene %d", i, pk->inc_cmd[i].scene);
    return true;
}

bool pack_load_mem(Pack *pk, const uint8_t *data, size_t n, const char *what)
{
    memset(pk, 0, sizeof *pk);
    PackCtx c = { rd_make(data, n), {0}, mem_data(), what ? what : "Scene.pck" };
    ArenaMark start = arena_mark(c.a);
    bool ok = pack_header(&c, n);
    if (ok) {
        pk->exe_angou_mod = c.h[PH_EXE_ANGOU_MOD];
        ok = pack_inc(pk, &c) && pack_scenes(pk, &c);
    }
    if (!ok) {
        arena_reset_to(c.a, start);                  /* everything since start is this load's */
        memset(pk, 0, sizeof *pk);
        return false;
    }
    pk->priv_.mem_start = start;
    pk->priv_.mem_end = arena_mark(c.a);
    pk->priv_.loaded = true;
    return true;
}

bool pack_load(Pack *pk, const char *scene_pck_path)
{
    memset(pk, 0, sizeof *pk);
    size_t n = 0;
    uint8_t *d = fs_read(scene_pck_path, &n);
    if (!d) return false;
    bool ok = pack_load_mem(pk, d, n, scene_pck_path);
    fs_free(d);
    return ok;
}

const WStr *pack_scene_name(const Pack *pk, int scene)
{
    static const WStr empty;
    return pk && scene >= 0 && scene < pk->nscene ? &pk->scene[scene].name_ : &empty;
}

const WStr *pack_inc_cmd_name(const Pack *pk, int i)
{
    static const WStr empty;
    return pk && i >= 0 && i < pk->ninc_cmd ? &pk->inc_cmd_name_[i] : &empty;
}

void pack_free(Pack *pk)
{
    if (!pk) return;
    if (pk->priv_.loaded)
        (void)arena_release_top(mem_data(), pk->priv_.mem_start, pk->priv_.mem_end); /* else: next reset */
    memset(pk, 0, sizeof *pk);
}

int pack_find_scene(const Pack *pk, const WStr *name)
{
    for (int i = 0; i < pk->nscene; i++)
        if (!ws_cmp_fold(&pk->scene[i].name_, name)) return i;
    return -1;
}

void scene_str(const Scene *sc, int idx, WStr *out)
{
    ws_set(out, NULL, 0);
    if (!sc || idx < 0 || idx >= sc->nstr) return;
    size_t pool_off = (size_t)(sc->priv_.str - sc->blob);
    NameTab t = { rd_make(sc->priv_.str_idx, (size_t)sc->nstr * NAME_ENTRY),
                  rd_make(sc->priv_.str, sc->blob_size - pool_off), sc->nstr };
    const uint8_t *p;
    int32_t n;
    if (!name_view(&t, idx, &p, &n)) return;
    uint16_t key = (uint16_t)(STR_XOR * idx);
    for (int32_t k = 0; k < n; k++) ws_push(out, (uint16_t)(bin_load_le16(p + 2 * (size_t)k) ^ key));
}

bool scene_name(const Scene *sc, int table, int idx, WStr *out)
{
    ws_set(out, NULL, 0);
    if (!sc || table < SCENE_NAMES_PROP || table > SCENE_NAMES_CALL_PROP) return false;
    const int count[3] = { sc->nprop, sc->nscn_cmd, sc->ncall_prop_name };
    const uint8_t *end = sc->blob + sc->blob_size;
    /* the index table is bounded by the blob (the header's own count of it is not kept) */
    NameTab t = { rd_make(sc->priv_.nm_idx[table], (size_t)(end - sc->priv_.nm_idx[table])),
                  rd_make(sc->priv_.nm[table], (size_t)(end - sc->priv_.nm[table])), count[table] };
    const uint8_t *p;
    int32_t n;
    if (!name_view(&t, idx, &p, &n)) return false;
    for (int32_t k = 0; k < n; k++) ws_push(out, bin_load_le16(p + 2 * (size_t)k));
    return true;
}
