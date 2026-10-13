/* fmt/fmt_test.c — M0 self-test of the format decoders (ENGINE_SPEC §4 M0): Scene.pck against
 * tests/ref/wp1_scenes.tsv, Gameexe.dat, MODE.CGM, and every g00 against tests/ref/g00_crc.tsv.
 * Reference files are tab-separated text; they are parsed with Str/str_to_* like any other input. */
#include "fmt/pack.h"
#include "core/bin.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "fmt/cgtable.h"
#include "fmt/g00.h"
#include "fmt/gameexe.h"
#include "plat/fs.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

enum {
    REF_PATH_MAX = 1024,
    REF_FIELDS = 24,
    SCENE_FIELDS = 18,                   /* name + 17 values per scene line */
    REF_CHUNK = 64,                      /* ints of a table that the reference CRCs cover */
    G00_NAME_MAX = 63,
    G00_CHUNK_BYTES = 1024,
    ZLABELS = 1000
};

/* ------------------------------------------------------------------------------------------------ helpers */

/* Splits line at tabs into at most max fields; returns the count. */
static int split_tabs(Str line, Str *f, int max)
{
    int n = 0;
    Str field;
    while (n < max && str_split(&line, '\t', &field)) f[n++] = field;
    return n;
}

static int64_t fld_int(Str s)
{
    int64_t v;
    return str_to_i64(s, &v) ? v : INT64_MIN;           /* never equal to a count */
}

static uint64_t fld_hex(Str s)
{
    uint64_t v;
    return str_to_u64_hex(s, &v) ? v : UINT64_MAX;      /* never equal to a CRC-32 */
}

/* Next line of *rest without its newline (and a trailing CR); false at the end. */
static bool next_line(Str *rest, Str *line)
{
    while (str_split(rest, '\n', line)) {
        if (line->n && line->p[line->n - 1] == '\r') line->n--;
        if (line->n) return true;
    }
    return false;
}

static uint32_t crc_ints(const int32_t *v, int n)
{
    uint32_t c = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b[4];
        bin_store_le32(b, (uint32_t)v[i]);
        c = hash_crc32(c, b, 4);
    }
    return c;
}

static uint32_t crc_wstr(uint32_t c, const WStr *s)
{
    for (int i = 0; i <= s->len; i++) {      /* includes the terminating 0 unit */
        uint8_t b[2];
        bin_store_le16(b, i < s->len ? s->p[i] : 0);
        c = hash_crc32(c, b, 2);
    }
    return c;
}

/* ------------------------------------------------------------------------------------------------ Scene.pck */

typedef struct {
    int fail, scn_ok, scn_bad, walk_ok, ncmd, ninl, ntext, inl_mismatch, nref;
    bool *seen;
} PackTotals;

static bool check_pack_line(const Pack *pk, const Str *f)
{
    int32_t inc[2 * REF_CHUNK] = {0};
    int ni = pk->ninc_cmd < REF_CHUNK ? pk->ninc_cmd : REF_CHUNK;
    for (int i = 0, k = 0; i < ni; i++) { inc[k++] = pk->inc_cmd[i].scene; inc[k++] = pk->inc_cmd[i].ofs; }
    uint32_t nc = 0;
    for (int i = 0; i < pk->ninc_cmd; i++) nc = crc_wstr(nc, pack_inc_cmd_name(pk, i));
    bool ok = fld_int(f[1]) == pk->nscene && fld_int(f[2]) == pk->ninc_cmd && fld_int(f[3]) == pk->ninc_prop &&
              fld_hex(f[4]) == crc_ints(inc, 2 * ni) && fld_hex(f[5]) == nc;
    log_msg(ok ? LOG_INFO : LOG_ERR, "  Scene.pck: %d scenes, %d inc_cmd, %d inc_prop, inc_cmd table+names: %s",
            pk->nscene, pk->ninc_cmd, pk->ninc_prop, ok ? "ok" : "FAIL");
    return ok;
}

typedef struct { uint32_t str_crc, name_crc, props_crc; int nz; } SceneCrcs;

static SceneCrcs scene_crcs(const Scene *sc)
{
    SceneCrcs c = {0, 0, 0, 0};
    WStr w = {0};
    for (int i = 0; i < sc->nstr; i++) { scene_str(sc, i, &w); c.str_crc = crc_wstr(c.str_crc, &w); }
    const int count[3] = { sc->nprop, sc->nscn_cmd, sc->ncall_prop_name };
    for (int t = 0; t < 3; t++)
        for (int i = 0; i < count[t]; i++) { (void)scene_name(sc, t, i, &w); c.name_crc = crc_wstr(c.name_crc, &w); }
    ws_free(&w);
    for (int i = 0; i < ZLABELS; i++) c.nz += sc->zlabel[i] != 0;
    int32_t props[2 * REF_CHUNK] = {0};
    int np = sc->nprop < REF_CHUNK ? sc->nprop : REF_CHUNK;
    for (int i = 0, k = 0; i < np; i++) { props[k++] = sc->prop[i].form; props[k++] = sc->prop[i].size; }
    c.props_crc = crc_ints(props, 2 * np);
    return c;
}

/* The first table that disagrees with the reference line, or NULL. */
static const char *scene_mismatch(const Scene *sc, const Str *f, const SceneStats *st)
{
    SceneCrcs c = scene_crcs(sc);
    if (fld_int(f[1]) != sc->code_size) return "code_size";
    if (fld_int(f[2]) != sc->nstr || fld_hex(f[3]) != c.str_crc) return "strings";
    if (fld_int(f[4]) != sc->nlabel || fld_hex(f[5]) != crc_ints(sc->label, sc->nlabel)) return "labels";
    if (fld_int(f[6]) != c.nz || fld_hex(f[7]) != crc_ints(sc->zlabel, ZLABELS)) return "z-labels";
    if (fld_int(f[8]) != sc->nscn_cmd || fld_hex(f[9]) != crc_ints(sc->scn_cmd, sc->nscn_cmd)) return "scn_cmd";
    if (fld_int(f[10]) != sc->nprop || fld_hex(f[11]) != c.props_crc) return "props";
    if (fld_int(f[12]) != sc->nread_flag || fld_hex(f[13]) != crc_ints(sc->read_flag_line, sc->nread_flag))
        return "read flags";
    if (fld_hex(f[14]) != c.name_crc) return "name tables";
    if (!st->ok) return "disassembly desync";
    if (fld_int(f[15]) != st->ncommand || fld_int(f[17]) != st->ntext) return "COMMAND/TEXT counts";
    return NULL;
}

static int find_scene_utf8(const Pack *pk, const char *name)
{
    WStr w = {0};
    ws_set_utf8(&w, name);
    int r = pack_find_scene(pk, &w);
    ws_free(&w);
    return r;
}

static void check_scene_line(const Pack *pk, const Str *f, PackTotals *t)
{
    char name[REF_PATH_MAX];
    t->nref++;
    int si = str_copy(name, sizeof name, f[0]) ? find_scene_utf8(pk, name) : -1;
    if (si < 0) { log_msg(LOG_ERR, "  scene %.*s: missing", (int)f[0].n, f[0].p); t->scn_bad++; return; }
    t->seen[si] = true;
    const Scene *sc = &pk->scene[si];
    SceneStats st;
    scene_walk(sc, &st);
    const char *what = scene_mismatch(sc, f, &st);
    if (st.ok) t->walk_ok++;
    if (fld_int(f[16]) != st.ninline) t->inl_mismatch++;
    t->ncmd += st.ncommand; t->ninl += st.ninline; t->ntext += st.ntext;
    if (what) { t->scn_bad++; log_msg(LOG_ERR, "  scene %s: %s mismatch", name, what); }
    else t->scn_ok++;
}

static void check_ref_lines(const Pack *pk, Str text, PackTotals *t)
{
    Str line, f[REF_FIELDS] = {{NULL, 0}};
    while (next_line(&text, &line)) {
        int nf = split_tabs(line, f, REF_FIELDS);
        if (str_eq(f[0], STR("#pack")) && nf >= 6) { t->fail += !check_pack_line(pk, f); continue; }
        if (f[0].n && f[0].p[0] == '#') continue;
        if (nf >= SCENE_FIELDS) check_scene_line(pk, f, t);
    }
}

/* seen101 str 0 = the first TEXT line of seen101, checked by CRC-32 of its UTF-8 so that no script text is kept in
 * the source (run_m0.sh compares the line itself with the vm_ref.py reference). */
static bool check_seen101(const Pack *pk, bool en)
{
    static const uint32_t CRC_JA = 0xeeacf9a1u, CRC_EN = 0x94f79a8cu;
    int s101 = find_scene_utf8(pk, "SEEN101");
    WStr w = {0};
    if (s101 >= 0) scene_str(&pk->scene[s101], 0, &w);
    char *u = ws_utf8(&w);
    bool ok = hash_crc32(0, u, strlen(u)) == (en ? CRC_EN : CRC_JA);
    log_msg(ok ? LOG_INFO : LOG_ERR, "  seen101 str 0 = \"%s\": %s", u, ok ? "ok" : "FAIL");
    ws_utf8_free(u);
    ws_free(&w);
    return ok;
}

static int report_pack(const Pack *pk, const PackTotals *t, bool have_ref)
{
    int bad = t->scn_bad;
    for (int i = 0; i < pk->nscene && have_ref; i++)
        if (!t->seen[i]) {
            Scratch sc = mem_scratch_begin(NULL);
            log_msg(LOG_ERR, "  scene %s: not in ref",
                    ws_utf8_arena(sc.arena, pack_scene_name(pk, i), WS_UTF8_SHORT_MAX));
            mem_scratch_end(&sc);
            bad++;
        }
    if (have_ref)
        log_msg(bad ? LOG_ERR : LOG_INFO, "  scenes: %d/%d match the recon/tools reference (strings, labels, z-labels, "
                "scn_cmd, props, read flags, names)", t->scn_ok, pk->nscene);
    bool en = !strcmp(fs_language(), "en");       /* EN patch Scene.pck: _start/direction_mess98 have more code */
    bool tot = pk->nscene == 194 && pk->ninc_cmd == 24 && t->walk_ok == 194 && t->ncmd == (en ? 54906 : 54898) &&
               t->ninl == 2771 && !t->inl_mismatch;
    log_msg(tot ? LOG_INFO : LOG_ERR, "  disassembly: %d/%d scenes to EOF, %d COMMANDs, %d inline reads (VM §9 rule; "
            "%d scenes disagree with the tools), %d TEXT: %s", t->walk_ok, pk->nscene, t->ncmd, t->ninl,
            t->inl_mismatch, t->ntext, tot ? "ok" : "FAIL");
    bool s0 = check_seen101(pk, en);
    return t->fail + bad + !tot + !s0 + (have_ref && t->nref != pk->nscene);
}

/* Without a reference table: disassemble every scene for the totals check only. */
static void walk_all(const Pack *pk, PackTotals *t)
{
    for (int i = 0; i < pk->nscene; i++) {
        SceneStats st;
        scene_walk(&pk->scene[i], &st);
        t->walk_ok += st.ok;
        t->ncmd += st.ncommand; t->ninl += st.ninline; t->ntext += st.ntext;
    }
}

static bool ref_exists(const char *path)
{
    SDL_PathInfo info;
    return path && SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

static int pack_selftest(const Pack *pk, const char *ref_path)
{
    PackTotals t;
    memset(&t, 0, sizeof t);
    size_t rn = 0;
    char *txt = ref_exists(ref_path) ? (char *)fs_read(ref_path, &rn) : NULL;
    if (!txt) log_msg(LOG_INFO, "  scenes: no reference table (build-only tree): every scene is walked, not compared");
    Scratch s = mem_scratch_begin(NULL);
    t.seen = arena_alloc(s.arena, (size_t)pk->nscene + 1, 1);
    if (txt) check_ref_lines(pk, str_make(txt, rn), &t);
    else walk_all(pk, &t);
    int fails = report_pack(pk, &t, txt != NULL);
    mem_scratch_end(&s);
    fs_free(txt);
    return fails;
}

/* ------------------------------------------------------------------------------------------------ g00 */

typedef struct { char name[G00_NAME_MAX + 1]; int type, cut, w, h; bool empty; uint32_t crc; } RefCut;
typedef struct { RefCut *v; int n; } RefCuts;

static int cmp_ref(const void *a, const void *b)
{
    const RefCut *x = a, *y = b;
    int c = str_cmp_fold(str_c(x->name), str_c(y->name));
    return c ? c : x->cut - y->cut;
}

static bool parse_ref_cut(Str line, RefCut *r)
{
    Str f[6];
    memset(r, 0, sizeof *r);
    if (split_tabs(line, f, 6) != 6 || !str_copy(r->name, sizeof r->name, f[0])) return false;
    int32_t v[4];
    for (int i = 0; i < 4; i++) if (!str_to_i32(f[1 + i], &v[i])) return false;
    r->type = v[0]; r->cut = v[1]; r->w = v[2]; r->h = v[3];
    r->empty = f[5].n && f[5].p[0] == '-';
    uint64_t crc = 0;
    if (!r->empty && (!str_to_u64_hex(f[5], &crc) || crc > UINT32_MAX)) return false;
    r->crc = (uint32_t)crc;
    return true;
}

/* Reference cuts sorted by (name, cut), in arena a. */
static RefCuts load_ref_cuts(Arena *a, const char *ref_tsv)
{
    RefCuts rc = { NULL, 0 };
    size_t n = 0;
    char *txt = ref_exists(ref_tsv) ? (char *)fs_read(ref_tsv, &n) : NULL;
    if (!txt) { log_msg(LOG_INFO, "  g00: no reference table (build-only tree): every image is decoded, not compared"); return rc; }
    size_t lines = 1;
    for (size_t i = 0; i < n; i++) lines += txt[i] == '\n';
    rc.v = arena_try_alloc(a, lines * sizeof *rc.v, _Alignof(RefCut));   /* lines <= file size: no overflow */
    if (!rc.v) { log_msg(LOG_ERR, "  g00 reference %s is too large", ref_tsv); fs_free(txt); return rc; }
    Str rest = str_make(txt, n), line;
    while (next_line(&rest, &line) && (size_t)rc.n < lines)
        if (line.p[0] != '#' && parse_ref_cut(line, &rc.v[rc.n])) rc.n++;
    fs_free(txt);
    qsort(rc.v, (size_t)rc.n, sizeof *rc.v, cmp_ref);
    return rc;
}

/* CRC32 of the cut pixels as BGRA bytes (what g00.py's PIL image holds in BGRA raw mode). */
static uint32_t cut_crc(const G00Cut *c)
{
    uint32_t crc = 0;
    uint8_t buf[G00_CHUNK_BYTES] = {0};
    size_t n = (size_t)c->w * (size_t)c->h, k = 0;
    for (size_t i = 0; i < n; i++) {
        bin_store_le32(buf + k, c->px[i]);
        k += 4;
        if (k == sizeof buf) { crc = hash_crc32(crc, buf, k); k = 0; }
    }
    return hash_crc32(crc, buf, k);
}

static G00 *load_g00(const char *name)
{
    char *path = fs_path("g00", name, "g00");
    size_t n = 0;
    uint8_t *d = path ? fs_read(path, &n) : NULL;
    fs_free(path);
    G00 *g = d ? g00_decode(d, n) : NULL;
    fs_free(d);
    if (d && !g) log_msg(LOG_WARN, "g00: cannot decode %s", name);
    return g;
}

typedef struct { int ok, fail, bytype[3], slots, real, cut_ok, cut_bad, cut_noref; } G00Totals;

static void check_g00_cuts(const G00 *g, Str base, const RefCuts *rc, G00Totals *t)
{
    bool file_bad = false;
    for (int k = 0; k < g->ncut; k++) {
        const G00Cut *c = &g->cut[k];
        if (g->type == 2) { t->slots++; if (c->px) t->real++; }
        RefCut key;
        memset(&key, 0, sizeof key);
        key.cut = k;
        const RefCut *r = str_copy(key.name, sizeof key.name, base) && rc->n
                              ? bsearch(&key, rc->v, (size_t)rc->n, sizeof *rc->v, cmp_ref) : NULL;
        if (!r) { t->cut_noref++; file_bad = true; continue; }
        bool good =
            r->type == g->type && (r->empty ? !c->px : (c->px && r->w == c->w && r->h == c->h && r->crc == cut_crc(c)));
        if (good) { t->cut_ok++; continue; }
        t->cut_bad++;
        if (!file_bad) log_msg(LOG_ERR, "  g00 %s cut %d: mismatch (%dx%d)", key.name, k, c->w, c->h);
        file_bad = true;
    }
}

static int g00_selftest(const char *ref_tsv)
{
    Scratch s = mem_scratch_begin(NULL);
    RefCuts rc = load_ref_cuts(s.arena, ref_tsv);
    char **names;
    int nf = fs_list("g00", "g00", &names);
    G00Totals t;
    memset(&t, 0, sizeof t);
    for (int i = 0; i < nf; i++) {
        G00 *g = load_g00(names[i]);
        if (!g) { t.fail++; log_msg(LOG_ERR, "  g00 %s: decode FAILED", names[i]); continue; }
        t.ok++;
        if (g->type >= 0 && g->type <= 2) t.bytype[g->type]++;
        Str base = str_c(names[i]);
        size_t dot;
        if (str_rfind_char(base, '.', &dot)) base.n = dot;
        check_g00_cuts(g, base, &rc, &t);
        g00_free(g);
    }
    fs_list_free(names, nf);
    int ncut = t.cut_ok + t.cut_bad + t.cut_noref;
    log_msg(LOG_INFO, "  g00: %d/%d decoded, types %d/%d/%d, type-2 slots %d (%d non-empty)",
            t.ok, nf, t.bytype[0], t.bytype[1], t.bytype[2], t.slots, t.real);
    bool have_ref = rc.n > 0;
    if (have_ref)
        log_msg(LOG_INFO, "  g00: per-cut CRC %d/%d match (%d mismatch, %d not in ref; ref has %d cuts)",
                t.cut_ok, ncut, t.cut_bad, t.cut_noref, rc.n);
    mem_scratch_end(&s);
    int bad = t.fail + (have_ref ? t.cut_bad + t.cut_noref + (ncut != rc.n) : 0);
    bad += !(nf == 617 && t.bytype[0] == 468 && t.bytype[1] == 1 && t.bytype[2] == 148 && t.slots == 2840 &&
             t.real == 2315);
    return bad;
}

/* ------------------------------------------------------------------------------------------------ driver */

static bool ref_dir_has_data(Str dir)
{
    SB_ON_STACK(probe, REF_PATH_MAX);
    SDL_PathInfo info;
    return path_join(&probe, dir, STR("g00_crc.tsv")) && SDL_GetPathInfo(sb_cstr(&probe), &info) &&
           info.type == SDL_PATHTYPE_FILE;
}

/* $SAYOOSHI_REF_DIR, else <this file>/../../../tests/ref (= engine/tests/ref), else ./tests/ref, ./engine/tests/ref. */
static void default_refdir(StrBuf *out)
{
    const char *env = SDL_getenv("SAYOOSHI_REF_DIR");
    sb_reset(out);
    if (env && *env) { (void)sb_put_c(out, env); return; }
    Str engine = path_dir(path_dir(path_dir(str_c(__FILE__))));
    SB_ON_STACK(near_src, REF_PATH_MAX);
    bool have_src = engine.n && path_join(&near_src, engine, STR("tests/ref"));
    const Str cand[3] = { have_src ? sb_str(&near_src) : STR(""), STR("tests/ref"), STR("engine/tests/ref") };
    for (int i = 0; i < 3; i++)
        if (cand[i].n && ref_dir_has_data(cand[i])) { (void)sb_put(out, cand[i]); return; }
    (void)sb_put_c(out, "tests/ref");
}

static int test_pack(const char *refdir)
{
    char *path = fs_path(NULL, "scene", "pck");
    Pack pk;
    int fail = 0;
    if (path && pack_load(&pk, path)) {
        SB_ON_STACK(ref, REF_PATH_MAX);
        (void)path_join(&ref, str_c(refdir), STR("wp1_scenes.tsv"));   /* a cut path just fails to open */
        fail += pack_selftest(&pk, sb_cstr(&ref));
        pack_free(&pk);
    } else { log_msg(LOG_ERR, "  Scene.pck: load FAILED"); fail++; }
    fs_free(path);
    return fail;
}

/* ENGINE_SPEC §4 M0 Gameexe assertions. */
static int gx_check(bool cond, const char *what)
{
    log_msg(cond ? LOG_INFO : LOG_ERR, "  gameexe %-44s %s", what, cond ? "ok" : "FAIL");
    return cond ? 0 : 1;
}

static bool str_is(const char *key, int idx, const char *want)
{
    WStr w = {0};
    SB_ON_STACK(u, 256);
    bool ok = gx_str(key, idx, &w) && ws_to_utf8(&w, &u) && !strcmp(sb_cstr(&u), want);
    ws_free(&w);
    return ok;
}

static int test_gameexe(void)
{
    int fail = 0, t[64];
    /* The EN patch Gameexe (fs overlay "en") adds #CONFIG.FONT.NAME and widens MWND.002 (SPEC_ENPATCH §2). */
    bool en = !strcmp(fs_language(), "en");
    fail += gx_check(gx_line_count() == (en ? 500 : 499), en ? "500 setting lines (EN)" : "499 setting lines");
    fail += gx_check(gx_int("MWND.002.MOJI_SIZE", 0, -1) == 24, "MWND.002.MOJI_SIZE = 24");
    fail += gx_check(gx_int("#MWND.002.MOJI_CNT", 0, -1) == (en ? 27 : 26) &&
                         gx_int("MWND.002.MOJI_CNT", 1, -1) == 16 && gx_nfields("MWND.002.MOJI_CNT") == 2,
                     en ? "MWND.002.MOJI_CNT = 27,16 (EN)" : "MWND.002.MOJI_CNT = 26,16");
    if (en) fail += gx_check(gx_int("CONFIG.FONT.TYPE", 0, -1) == 3 && str_is("CONFIG.FONT.NAME", 0, "sayooshieng"),
                             "CONFIG.FONT.TYPE = 3, FONT.NAME = \"sayooshieng\" (EN)");
    int nt = gx_tuples("SHAKE.002", t, 64);
    fail += gx_check(nt == 9 && t[0] == 6 && t[1] == 32 && t[6] == -4 && t[8] == 32, "SHAKE.002 = 9 ints");
    fail += gx_check(str_is("BGM.001", 0, "RAIN") && str_is("BGM.001", 1, "RAIN") && gx_int("BGM.001", 2, -1) == 0 &&
                     gx_int("BGM.001", 3, -1) == 99999999 && gx_int("BGM.001", 4, -1) == 80000,
                     "BGM.001 = \"RAIN\",\"RAIN\",0,99999999,80000");
    fail += gx_check(gx_int("FLAG.CNT", 0, -1) == 2000, "FLAG.CNT = 2000");
    fail += gx_check(gx_int("OBJECT.042.SAVE", 0, -1) == 1 && gx_int("object.099.use", 0, -1) == 1 &&
                     !gx_has("OBJECT.100.SAVE"), "OBJECT.000-099 range expansion");
    fail +=
        gx_check(str_is("START_SCENE", 0, "_start") && gx_int("START_SCENE", 1, -1) == 0, "START_SCENE = \"_start\",0");
    fail += gx_check(
        str_is("GAMENAME", 0,
               en ? "Sayonara wo Oshiete"
                  : "\xe3\x81\x95\xe3\x82\x88\xe3\x81\xaa\xe3\x82\x89\xe3\x82\x92\xe6\x95\x99\xe3\x81\x88\xe3\x81\xa6"),
        "GAMENAME");
    return fail;
}

/* M0 MODE.CGM assertions on the loaded table. */
static int test_cgtable(void)
{
    int n = cgt_count(), mx = -1;
    for (int i = 0; i < n; i++) if (cgt_flag_no(cgt_name(i)) > mx) mx = cgt_flag_no(cgt_name(i));
    bool ok = n == 83 && !strcmp(cgt_name(0), "FG_RM1") && cgt_flag_no(cgt_name(0)) == 0 && mx == 89 &&
              cgt_flag_no("fg_rm1") == 0 && cgt_flag_no("NO_SUCH_CG") == -1;
    log_msg(ok ? LOG_INFO : LOG_ERR, "  MODE.CGM: %d entries, first %s -> flag %d, flag_no max %d: %s",
            n, cgt_name(0), cgt_flag_no(cgt_name(0)), mx, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static int test_tables(void)
{
    int fail = 0;
    char *path = fs_path(NULL, "gameexe", "dat");
    if (path && gx_load(path)) fail += test_gameexe();
    else { log_msg(LOG_ERR, "  Gameexe.dat: load FAILED"); fail++; }
    fs_free(path);
    path = fs_path("dat", "mode", "cgm");
    if (path && cgt_load(path)) fail += test_cgtable();
    else { log_msg(LOG_ERR, "  MODE.CGM: load FAILED"); fail++; }
    fs_free(path);
    return fail;
}

int fmt_selftest_ref(const char *gamedata, const char *refdir)
{
    SB_ON_STACK(rd, REF_PATH_MAX);
    if (refdir && *refdir) (void)sb_put_c(&rd, refdir);
    else default_refdir(&rd);
    log_msg(LOG_INFO, "formats self-test: gamedata %s, reference %s", gamedata, sb_cstr(&rd));
    /* "" or an already mounted fs (main.c discovery): use the mounted layers */
    if (gamedata && *gamedata && !fs_mounted() && !fs_init(gamedata)) {
        log_msg(LOG_ERR, "  fs: cannot index %s", gamedata);
        return 1;
    }
    int fail = test_pack(sb_cstr(&rd)) + test_tables();
    SB_ON_STACK(ref, REF_PATH_MAX);
    (void)path_join(&ref, sb_str(&rd), STR("g00_crc.tsv"));
    fail += g00_selftest(sb_cstr(&ref));
    log_msg(fail ? LOG_ERR : LOG_INFO, "formats self-test: %s (%d failures)", fail ? "FAIL" : "PASS", fail);
    return fail ? 1 : 0;
}

int fmt_selftest(const char *gamedata) { return fmt_selftest_ref(gamedata, NULL); }
