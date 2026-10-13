/* vm/vm_save.c — the VM's save chunks (ENGINE_SPEC §2.11, docs/SAVE_FORMAT.md §9.1): VM02 (stacks, frames, position
 * by scene NAME, local banks, non-default scene props) and VMG2 (global banks G Z M ＊namae). Format-1 files
 * (VM01/VMG1) are still read. The byte layout is pinned by the golden save fixtures.
 *
 * Every count is bounded by the VM's own limits and by the bytes left in the chunk; positions are validated
 * against the loaded scenes; a failed load leaves an empty call stack (never a half-built one) and global banks
 * are only replaced when the whole chunk decoded. Temporary decode buffers come from a scratch arena. */
#include "vm/vm_internal.h"
#include "core/log.h"
#include "sav/sav.h"
#include <string.h>

enum { LIST_MAX = ELM_LIST_MAX, PROP_LIST_MAX = 1 << 16, PROPS_PER_SCENE_MAX = 4096, SCENES_MAX = 100000 };
enum { CHUNK_VM1 = FOURCC('V', 'M', '0', '1'), CHUNK_VM2 = FOURCC('V', 'M', '0', '2'),
       CHUNK_VMG1 = FOURCC('V', 'M', 'G', '1'), CHUNK_VMG2 = FOURCC('V', 'M', 'G', '2') };

static void list_take(IntList *l, int n)       /* resizable lists follow the save; fixed ones keep their size */
{
    if (n != l->n && (l->resizable || !l->w)) elm_intlist_resize(l, n);
}

static void strlist_take(StrList *l, int n)
{
    if (n != l->n && (l->resizable || !l->s)) elm_strlist_resize(l, n);
}

/* ---- format 1 (VM01/VMG1) readers ----------------------------------------------------------------------------- */

static bool r_intlist(SavReader *r, IntList *l)
{
    int n = sav_get_count(r, LIST_MAX, 4);
    if (!sav_ok(r)) return false;
    list_take(l, n);
    for (int i = 0; i < n; i++) { int32_t v = sav_get_i32(r); if (i < l->n) l->w[i] = v; }
    return sav_ok(r);
}

static bool r_strlist(SavReader *r, StrList *l)
{
    int n = sav_get_count(r, LIST_MAX, 4);
    if (!sav_ok(r)) return false;
    strlist_take(l, n);
    WStr spill = { 0 };                        /* entries beyond a fixed list's size are read and dropped */
    bool ok = true;
    for (int i = 0; i < n && ok; i++) ok = sav_get_wstr(r, i < l->n ? &l->s[i] : &spill);
    ws_free(&spill);
    return ok && sav_ok(r);
}

/* The fields shared by both prop formats; the lists follow. */
static bool r_prop_head(SavReader *r, Prop *p)
{
    int form = sav_get_i32(r), name = sav_get_i32(r);
    if (!sav_ok(r)) return false;
    vm_prop_reset(p, form, 0, name);
    p->i = sav_get_i32(r);
    if (!sav_get_wstr(r, &p->s)) return false;
    p->nchain = sav_get_count(r, VM_CHAIN_MAX, 4);
    for (int k = 0; k < p->nchain; k++) p->chain[k] = sav_get_i32(r);
    return sav_ok(r);
}

static bool r_prop(SavReader *r, Prop *p)
{
    if (!r_prop_head(r, p)) return false;
    if (p->form == ELM_FORM_INTLIST && !r_intlist(r, &p->il)) return false;
    if (p->form == ELM_FORM_STRLIST && !r_strlist(r, &p->sl)) return false;
    return sav_ok(r);
}

/* ---- format 2: zero-run-coded int lists, sparse string lists, non-default props only -------------------------- */

static void w_intlist2(SavWriter *w, const IntList *l) { sav_put_i32_rle(w, l->w, l->n); }

static bool r_intlist2(SavReader *r, IntList *l, int max)
{
    Scratch sc = mem_scratch_begin(NULL);
    int32_t *v = NULL;
    int n = 0;
    bool ok = sav_get_i32_rle(r, sc.arena, &v, &n, max);
    if (ok) {
        list_take(l, n);
        int m = n < l->n ? n : l->n;
        if (m > 0) memcpy(l->w, v, (size_t)m * sizeof *v);
        if (l->n > m) memset(l->w + m, 0, (size_t)(l->n - m) * sizeof *v);
    }
    mem_scratch_end(&sc);
    return ok;
}

static void w_strlist2(SavWriter *w, const StrList *l)
{
    int m = 0;
    for (int i = 0; i < l->n; i++) if (l->s[i].len > 0) m++;
    sav_put_u32(w, (uint32_t)l->n);
    sav_put_u32(w, (uint32_t)m);
    for (int i = 0; i < l->n; i++) if (l->s[i].len > 0) { sav_put_u32(w, (uint32_t)i); sav_put_wstr(w, &l->s[i]); }
}

static bool r_strlist2(SavReader *r, StrList *l, int max)
{
    int n = sav_get_count(r, max, 0);
    int m = sav_get_count(r, n, 8);
    if (!sav_ok(r)) return false;
    strlist_take(l, n);
    elm_strlist_clear(l);
    WStr spill = { 0 };
    int last = -1;
    for (int k = 0; k < m; k++) {
        int idx = sav_get_i32(r);
        if (!sav_ok(r) || idx <= last || idx >= n) { sav_fail(r); break; }   /* strictly increasing, in range */
        last = idx;
        if (!sav_get_wstr(r, idx < l->n ? &l->s[idx] : &spill)) break;
    }
    ws_free(&spill);
    return sav_ok(r);
}

static void w_prop2(SavWriter *w, const Prop *p)
{
    sav_put_i32(w, p->form); sav_put_i32(w, p->name); sav_put_i32(w, p->i); sav_put_wstr(w, &p->s);
    sav_put_u32(w, (uint32_t)p->nchain);
    for (int k = 0; k < p->nchain; k++) sav_put_i32(w, p->chain[k]);
    if (p->form == ELM_FORM_INTLIST) w_intlist2(w, &p->il);
    if (p->form == ELM_FORM_STRLIST) w_strlist2(w, &p->sl);
}

static bool r_prop2(SavReader *r, Prop *p)
{
    if (!r_prop_head(r, p)) return false;
    if (p->form == ELM_FORM_INTLIST && !r_intlist2(r, &p->il, PROP_LIST_MAX)) return false;
    if (p->form == ELM_FORM_STRLIST && !r_strlist2(r, &p->sl, PROP_LIST_MAX)) return false;
    return sav_ok(r);
}

static bool prop_is_default(const Prop *p, const PropDecl *d, int k)
{
    if (p->form != d->form || p->name != k || p->i != 0 || p->s.len != 0 || p->nchain != 0) return false;
    if (p->form == ELM_FORM_INTLIST) {
        if (p->il.n != d->size) return false;
        for (int i = 0; i < p->il.n; i++) if (p->il.w[i]) return false;
    }
    if (p->form == ELM_FORM_STRLIST) {
        if (p->sl.n != d->size) return false;
        for (int i = 0; i < p->sl.n; i++) if (p->sl.s[i].len) return false;
    }
    return true;
}

static bool scn_prop_default(int s, int k) { return prop_is_default(vm_scene_prop(s, k), vm_scene_prop_decl(s, k), k); }

static int scn_props_changed(int s)
{
    int m = 0;
    for (int k = 0; k < vm_scene_prop_count(s); k++) if (!scn_prop_default(s, k)) m++;
    return m;
}

/* ---- scene names and positions -------------------------------------------------------------------------------- */

static void w_scene_name(SavWriter *w, int scene) { sav_put_wstr(w, pack_scene_name(vm_pack(), scene)); }

static int r_scene_name(SavReader *r)
{
    WStr n = { 0 };
    int idx = sav_get_wstr(r, &n) ? vm_find_scene(&n) : -1;
    if (idx < 0 && sav_ok(r)) {
        Scratch sc = mem_scratch_begin(NULL);
        log_msg(LOG_ERR, "vm_load: unknown scene '%s'", ws_utf8_arena(sc.arena, &n, VM_UTF8_MAX));
        mem_scratch_end(&sc);
    }
    ws_free(&n);
    return idx;
}

static bool pc_ok(int scene, uint32_t pc)
{
    return scene >= 0 && scene < vm_pack()->nscene && pc <= vm_pack()->scene[scene].code_size;
}

/* ---- VM02 ----------------------------------------------------------------------------------------------------- */

static void save_frame(SavWriter *w, const Frame *f)
{
    sav_put_i32(w, f->type); sav_put_i32(w, f->ret_form);
    w_scene_name(w, f->rs.scene); sav_put_u32(w, f->rs.pc); sav_put_i32(w, f->rs.line);
    sav_put_i32(w, f->base.isp); sav_put_i32(w, f->base.ssp); sav_put_i32(w, f->base.nmk);
    w_intlist2(w, &f->L); w_strlist2(w, &f->K);
    sav_put_u32(w, (uint32_t)f->nloc);
    for (int k = 0; k < f->nloc; k++) w_prop2(w, vm_frame_local(f, k));
}

/* Scene props: only scenes with a non-default prop, only those props; keyed by scene name + code hash. */
static void save_scene_props(SavWriter *w)
{
    int ns = 0, nscene = vm_pack()->nscene;
    for (int s = 0; s < nscene; s++) if (scn_props_changed(s)) ns++;
    sav_put_u32(w, (uint32_t)ns);
    for (int s = 0; s < nscene; s++) {
        int m = scn_props_changed(s);
        if (!m) continue;
        w_scene_name(w, s);
        sav_put_u64(w, sav_scene_hash(s));
        sav_put_u32(w, (uint32_t)vm_scene_prop_count(s));
        sav_put_u32(w, (uint32_t)m);
        for (int k = 0; k < vm_scene_prop_count(s); k++)
            if (!scn_prop_default(s, k)) { sav_put_u32(w, (uint32_t)k); w_prop2(w, vm_scene_prop(s, k)); }
    }
}

void vm_save(SavWriter *w)
{
    if (!vm_pack()) return;
    VmPos pos = vm_pos();
    sav_put_chunk(w, CHUNK_VM2);
    w_scene_name(w, pos.scene); sav_put_u32(w, pos.pc); sav_put_i32(w, pos.line);
    vm_stacks_save(w);
    sav_put_u32(w, (uint32_t)vm_depth());
    for (int d = 0; d < vm_depth(); d++) save_frame(w, vm_frame_at(d));
    for (int b = 0; b < VM_LOCAL_INT_BANKS; b++) w_intlist2(w, vm_int_bank(b));   /* local banks A..F, X */
    w_strlist2(w, vm_str_bank(VM_BANK_S)); w_strlist2(w, vm_str_bank(VM_BANK_NAMAE_LOCAL));
    save_scene_props(w);
}

static bool load_locals(SavReader *r, bool v2, Frame *f)
{
    int nl = sav_get_count(r, VM_LOCAL_MAX, 20);
    for (int k = 0; k < nl; k++) {
        Prop *p = vm_frame_add_local(f);
        if (!p) { sav_fail(r); return false; }
        if (!(v2 ? r_prop2(r, p) : r_prop(r, p))) return false;
    }
    return sav_ok(r);
}

static bool load_frame(SavReader *r, bool v2, int d, int depth)
{
    Frame *f = vm_frame_load(d);
    if (!f) { sav_fail(r); return false; }
    f->type = sav_get_i32(r); f->ret_form = sav_get_i32(r);
    f->rs.scene = r_scene_name(r); f->rs.pc = sav_get_u32(r); f->rs.line = sav_get_i32(r);
    if (!sav_ok(r)) return false;
    if (d + 1 < depth && !pc_ok(f->rs.scene, f->rs.pc)) return false;   /* a resume point must be valid */
    if (f->rs.scene < 0) { f->rs.scene = 0; f->rs.pc = 0; }
    f->base.isp = sav_get_i32(r); f->base.ssp = sav_get_i32(r); f->base.nmk = sav_get_i32(r);
    if (v2 ? !r_intlist2(r, &f->L, LIST_MAX) || !r_strlist2(r, &f->K, LIST_MAX)
           : !r_intlist(r, &f->L) || !r_strlist(r, &f->K)) return false;
    return load_locals(r, v2, f);
}

static bool load_local_banks(SavReader *r, bool v2)
{
    for (int b = 0; b < VM_LOCAL_INT_BANKS; b++)
        if (!(v2 ? r_intlist2(r, vm_int_bank(b), LIST_MAX) : r_intlist(r, vm_int_bank(b)))) return false;
    StrList *s = vm_str_bank(VM_BANK_S), *nm = vm_str_bank(VM_BANK_NAMAE_LOCAL);
    return v2 ? r_strlist2(r, s, LIST_MAX) && r_strlist2(r, nm, LIST_MAX) : r_strlist(r, s) && r_strlist(r, nm);
}

/* Shared by both formats: position, stacks, frames, local banks. */
static bool load_stacks(SavReader *r, bool v2, VmPos *pos)
{
    pos->scene = r_scene_name(r);
    pos->pc = sav_get_u32(r);
    pos->line = sav_get_i32(r);
    if (!sav_ok(r) || pos->scene < 0 || !pc_ok(pos->scene, pos->pc)) return false;
    vm_reset_stacks();
    if (!vm_stacks_load(r)) return false;
    int depth = sav_get_count(r, VM_FRAME_MAX, 40);
    if (!sav_ok(r) || depth < 1) return false;
    for (int d = 0; d < depth; d++) if (!load_frame(r, v2, d, depth)) return false;
    return load_local_banks(r, v2) && sav_ok(r);
}

/* One VM02 scene-prop record; props of scene code that differs from the save are read and dropped. */
static bool load_scene_props2(SavReader *r, Prop *spill)
{
    int idx = r_scene_name(r);
    uint64_t h = sav_get_u64(r);
    int np = sav_get_count(r, PROPS_PER_SCENE_MAX, 0);
    int m = sav_get_count(r, np, 24);
    bool use = idx >= 0 && h == sav_scene_hash(idx) && np == vm_scene_prop_count(idx);
    if (idx >= 0 && !use && sav_ok(r))
        log_msg(LOG_WARN, "vm_load: props of scene %s are from different scene code; left at defaults",
                vm_scene_name_utf8(idx));
    int last = -1;
    for (int k = 0; k < m && sav_ok(r); k++) {
        int pi = sav_get_i32(r);
        if (!sav_ok(r) || pi <= last || pi >= np) { sav_fail(r); break; }
        last = pi;
        if (!r_prop2(r, use ? vm_scene_prop(idx, pi) : spill)) break;
    }
    return sav_ok(r);
}

static void reset_scene_props(void)
{
    for (int s = 0; s < vm_pack()->nscene; s++)
        for (int k = 0; k < vm_scene_prop_count(s); k++) vm_scene_prop_redeclare(s, k);
}

static bool load_v2(SavReader *r, VmPos *pos)
{
    if (!load_stacks(r, true, pos)) return false;
    reset_scene_props();
    int ns = sav_get_count(r, SCENES_MAX, 20);
    Prop spill = { 0 };
    for (int s = 0; s < ns && sav_ok(r); s++) (void)load_scene_props2(r, &spill);   /* failure is sticky in r */
    vm_prop_free(&spill);
    return sav_ok(r);
}

static bool load_v1(SavReader *r, VmPos *pos)
{
    if (!load_stacks(r, false, pos)) return false;
    int ns = sav_get_count(r, SCENES_MAX, 8);
    Prop spill = { 0 };
    for (int s = 0; s < ns && sav_ok(r); s++) {
        int idx = r_scene_name(r);
        int np = sav_get_count(r, PROPS_PER_SCENE_MAX, 20);
        for (int k = 0; k < np; k++)
            if (!r_prop(r, idx >= 0 && k < vm_scene_prop_count(idx) ? vm_scene_prop(idx, k) : &spill)) break;
    }
    vm_prop_free(&spill);
    return sav_ok(r);
}

/* A prop restored with another form than its declaration would confuse later accesses: re-declare it. */
static void redeclare_mismatched_props(void)
{
    for (int s = 0; s < vm_pack()->nscene; s++)
        for (int k = 0; k < vm_scene_prop_count(s); k++) {
            const Prop *p = vm_scene_prop(s, k);
            if (p->form != vm_scene_prop_decl(s, k)->form || p->name != k) vm_scene_prop_redeclare(s, k);
        }
}

bool vm_load(SavReader *r)
{
    if (!vm_pack()) return false;
    VmPos pos = { 0, 0, 0 };
    bool ok;
    if (sav_find_chunk(r, CHUNK_VM2)) ok = load_v2(r, &pos);
    else if (sav_legacy(r) && sav_find_chunk(r, CHUNK_VM1)) ok = load_v1(r, &pos);
    else return false;
    redeclare_mismatched_props();
    if (!ok) { vm_reset_stacks(); sav_fail(r); return false; }   /* never leave a half-built call stack behind */
    vm_set_pos(pos);
    vm_clear_stop();
    return sav_ok(r);
}

/* ---- VMG2: global banks --------------------------------------------------------------------------------------- */

void vm_save_global(SavWriter *w)
{
    sav_put_chunk(w, CHUNK_VMG2);
    w_intlist2(w, vm_int_bank(VM_BANK_G)); w_intlist2(w, vm_int_bank(VM_BANK_Z));
    w_strlist2(w, vm_str_bank(VM_BANK_M)); w_strlist2(w, vm_str_bank(VM_BANK_NAMAE_GLOBAL));
}

typedef struct GlobalBanks { IntList g, z; StrList m, nm; } GlobalBanks;

static bool load_global_banks(SavReader *r, GlobalBanks *t)
{
    if (sav_find_chunk(r, CHUNK_VMG2))
        return r_intlist2(r, &t->g, LIST_MAX) && r_intlist2(r, &t->z, LIST_MAX) &&
               r_strlist2(r, &t->m, LIST_MAX) && r_strlist2(r, &t->nm, LIST_MAX) && sav_ok(r);
    if (sav_legacy(r) && sav_find_chunk(r, CHUNK_VMG1))
        return r_intlist(r, &t->g) && r_intlist(r, &t->z) && r_strlist(r, &t->m) && r_strlist(r, &t->nm) && sav_ok(r);
    return false;
}

static void copy_intlist(IntList *dst, const IntList *src)   /* same size by construction */
{
    if (dst->n) memcpy(dst->w, src->w, (size_t)dst->n * sizeof *dst->w);
}

static void copy_strlist(StrList *dst, const StrList *src)
{
    for (int i = 0; i < dst->n; i++) ws_copy(&dst->s[i], &src->s[i]);
}

bool vm_load_global(SavReader *r)
{
    /* Decode into temporary fixed-size lists first so a damaged chunk leaves the live global banks untouched. */
    IntList *g = vm_int_bank(VM_BANK_G), *z = vm_int_bank(VM_BANK_Z);
    StrList *m = vm_str_bank(VM_BANK_M), *nm = vm_str_bank(VM_BANK_NAMAE_GLOBAL);
    GlobalBanks t;
    memset(&t, 0, sizeof t);
    elm_intlist_init(&t.g, g->n, false); elm_intlist_init(&t.z, z->n, false);
    elm_strlist_init(&t.m, m->n, false); elm_strlist_init(&t.nm, nm->n, false);
    bool ok = load_global_banks(r, &t);
    if (ok) { copy_intlist(g, &t.g); copy_intlist(z, &t.z); copy_strlist(m, &t.m); copy_strlist(nm, &t.nm); }
    elm_intlist_free(&t.g); elm_intlist_free(&t.z); elm_strlist_free(&t.m); elm_strlist_free(&t.nm);
    return ok;
}

/* ---- call-stack scenes (save slot thumbnails / resume points) ------------------------------------------------- */

int vm_stack_scenes(int *out, int max)
{
    int n = 0;
    if (!vm_pack() || max <= 0) return 0;
    int nscene = vm_pack()->nscene, scene = vm_scene();
    for (int d = 0; d + 1 < vm_depth() && n < max - 1; d++) {
        int s = vm_frame_at(d)->rs.scene;
        if (s >= 0 && s < nscene) out[n++] = s;
    }
    if (scene >= 0 && scene < nscene) out[n++] = scene;
    return n;
}
