/* sav/sav_bundle.c — export/import of every save file as one validated bundle (with a backup of what an import
 * replaces), and the --print-save-dir / --export-saves / --import-saves command line (docs/SAVE_FORMAT.md §9).
 *
 * Safety: an entry name is only ever used as a path after it matched one of our own file names exactly, so a
 * hostile bundle cannot write outside the save directory. Paths live in scratch arenas. */
#include "sav/sav_internal.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "plat/sysdep.h"
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { SLOT_TOTAL = SAV_QUICK_BASE + SAV_QUICK_CNT };   /* slots 0..99 and the quick slots */

/* What a save file name is: its kind (0 = not one of ours), slot (-1 for global/config), old-format name. */
typedef struct { uint32_t kind; int slot; bool legacy_name; } SavName;

static SavName name_kind(Str n)
{
    SavName r = { 0, -1, false };
    if (str_eq(n, STR("global.sav"))) { r.kind = SAV_KIND_GLOB; return r; }
    if (str_eq(n, STR("config.sav"))) { r.kind = SAV_KIND_CONF; return r; }
    for (int s = 0; s < SLOT_TOTAL; s++)
        for (int legacy = 0; legacy < 2; legacy++) {
            char nm[SAV_NAME_CAP];
            if (!sav_slot_name(s, nm, legacy == 1) || !str_eq(n, str_c(nm))) continue;
            r.kind = SAV_KIND_SLOT;
            r.slot = s;
            r.legacy_name = legacy == 1;
            return r;
        }
    return r;
}

/* The i-th file name a bundle may hold (config, global, new-name slots, old-name slots); false past the end. */
static bool nth_name(int i, char out[SAV_NAME_CAP])
{
    if (i == 0 || i == 1) return str_copy_c(out, SAV_NAME_CAP, i == 0 ? "config.sav" : "global.sav");
    i -= 2;
    if (i < 0 || i >= 2 * SLOT_TOTAL) return false;
    return sav_slot_name(i % SLOT_TOTAL, out, i >= SLOT_TOTAL);
}

/* ================================================================== export */

static void put_bundle_meta(SavWriter *w)
{
    sav_put_chunk(w, SAV_TAG_META);
    sav_put_enter(w);
    sav_put_chunk(w, SAV_TAG_GAME); sav_put_str8(w, SAV_GAME_ID);
    sav_put_chunk(w, SAV_TAG_ENGV); sav_put_str8(w, SAYO_ENGINE_VERSION);
    sav_put_chunk(w, SAV_TAG_TIME); sav_put_i64(w, sav_now_unix());
    sav_put_leave(w);
}

/* An old-name slot is exported only while it has not been converted (no new-name file of the same slot). */
static bool superseded(Arena *ar, const char *name)
{
    SavName k = name_kind(str_c(name));
    char newer[SAV_NAME_CAP];
    return k.legacy_name && sav_slot_name(k.slot, newer, false) && sav_file_exists_any(sav_path_in(ar, newer));
}

/* FILE{NAME, DATA} with the best valid version of `name`; false if there is none. */
static bool put_file(SavWriter *w, Arena *ar, const char *name)
{
    size_t len;
    SavFileView v;
    uint8_t *d = sav_file_read_best(sav_path_in(ar, name), &len, &v, NULL);
    if (!d) return false;
    sav_put_chunk(w, SAV_TAG_FILE);
    sav_put_enter(w);
    sav_put_chunk(w, SAV_TAG_NAME); sav_put_str8(w, name);
    sav_put_chunk(w, SAV_TAG_DATA); sav_put_bytes(w, d, len);
    sav_put_leave(w);
    sav_file_free(d);
    return true;
}

static SavWriter *build_bundle(int *nfiles)
{
    SavWriter *w = sav_writer_new();
    sav_file_begin(w, SAV_KIND_BNDL);
    put_bundle_meta(w);
    *nfiles = 0;
    char name[SAV_NAME_CAP];
    for (int i = 0; nth_name(i, name); i++) {
        Scratch s = mem_scratch_begin(NULL);
        if (!superseded(s.arena, name) && put_file(w, s.arena, name)) (*nfiles)++;
        mem_scratch_end(&s);
    }
    sav_file_end(w);
    return w;
}

bool sav_export(const char *bundle_path)
{
    int nfiles;
    SavWriter *w = build_bundle(&nfiles);
    size_t len = 0;
    (void)sav_writer_data(w, &len);   /* the size for the log line; sav_file_store checks the data */
    bool ok = sav_file_store(bundle_path, w, SAV_KIND_BNDL);
    if (ok) log_msg(LOG_INFO, "save: exported %d files from %s to %s (%u bytes)", nfiles, sav_dir(), bundle_path,
                    (unsigned)len);
    sav_writer_free(w);
    return ok;
}

/* ================================================================== import */

/* One FILE entry: name and data present, the name is one of ours, the data is a valid file of that kind. */
static bool parse_entry(SavReader *r, int index, SavBundleFile *f)
{
    bool ok = sav_get_enter(r) && sav_find_chunk(r, SAV_TAG_NAME) && sav_get_str8_view(r, &f->name) &&
              sav_find_chunk(r, SAV_TAG_DATA);
    if (ok) { f->data = sav_cur(r); f->len = sav_remaining(r); }
    sav_get_leave(r);
    if (!ok) { sav_set_error("bundle entry %d is damaged", index); return false; }
    int nn = (int)core_min_sz(f->name.n, 64);
    uint32_t kind = name_kind(f->name).kind;
    if (!kind) { sav_set_error("bundle entry '%.*s' is not a save file name", nn, f->name.p); return false; }
    SavFileView fv;
    if (!sav_file_view(f->data, f->len, &fv) || fv.kind != kind) {
        sav_set_error("bundle entry '%.*s' is not a valid %s file", nn, f->name.p,
                      kind == SAV_KIND_SLOT ? "slot" : kind == SAV_KIND_GLOB ? "global.sav" : "config.sav");
        return false;
    }
    return true;
}

/* Validates a bundle completely; returns the number of entries in out[0..cap), or -1 (error text set). */
int sav_bundle_parse(const uint8_t *d, size_t len, SavBundleFile *out, int cap)
{
    SavFileView v;
    if (!sav_file_view(d, len, &v)) return -1;
    if (v.fmt != SAV_FMT_V2 || v.kind != SAV_KIND_BNDL) { sav_set_error("not a save bundle"); return -1; }
    SavReader *r = sav_view_reader(d, &v);
    int n = 0;
    uint32_t tag;
    bool ok = true;
    while (ok && sav_next_chunk(r, &tag)) {   /* bounded: every chunk advances the cursor */
        if (tag != SAV_TAG_FILE) continue;
        if (n >= cap) { sav_set_error("too many files in the bundle"); ok = false; break; }
        ok = parse_entry(r, n, &out[n]);
        for (int k = 0; ok && k < n; k++)
            if (str_eq(out[k].name, out[n].name)) {
                sav_set_error("duplicate entry '%.*s'", (int)out[n].name.n, out[n].name.p);   /* a known name */
                ok = false;
            }
        n++;
    }
    sav_reader_free(r);
    return ok ? n : -1;
}

/* NUL-terminated copy of an entry name (a validated one of ours, so it fits). */
static void entry_name(const SavBundleFile *f, char out[SAV_NAME_CAP])
{
    bool fit = str_copy(out, SAV_NAME_CAP, f->name);
    ASSERT(fit);
}

/* "<save dir>backup_YYYYMMDD_HHMMSS/" in ar. */
static const char *backup_dir(Arena *ar)
{
    SDL_Time now = 0;
    SDL_DateTime dt;
    memset(&dt, 0, sizeof dt);
    if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &dt, true)) log_msg(LOG_WARN, "save: no clock");
    char stamp[96];
    bool fit = str_fmt(stamp, sizeof stamp, "backup_%04d%02d%02d_%02d%02d%02d/", dt.year, dt.month, dt.day, dt.hour,
                       dt.minute, dt.second);
    ASSERT(fit);   /* six ints of at most 11 characters each + 10 fixed characters < 96 */
    return sav_path_in(ar, stamp);
}

/* Copy every local file the import replaces (both names of a slot) into bdir; false if that fails. */
static bool backup_existing(Arena *ar, const SavBundleFile *files, int n, const char *bdir, int *nbak)
{
    for (int i = 0; i < n; i++) {
        char cand[2][SAV_NAME_CAP];
        entry_name(&files[i], cand[0]);
        SavName k = name_kind(files[i].name);
        if (k.slot < 0 || !sav_slot_name(k.slot, cand[1], !k.legacy_name)) cand[1][0] = 0;   /* the other name */
        for (int c = 0; c < 2; c++) {
            if (!cand[c][0]) continue;
            const char *src = sav_path_in(ar, cand[c]);
            if (!plat_path_is_file(src)) continue;
            if (!*nbak && !SDL_CreateDirectory(bdir)) return false;
            if (!SDL_CopyFile(src, str_cat_arena(ar, bdir, cand[c], NULL))) return false;
            (*nbak)++;
        }
    }
    return true;
}

/* Write the entries; an imported old-name slot must not be shadowed by a local new-name file. */
static int write_entries(Arena *ar, const SavBundleFile *files, int n, bool *ok)
{
    int nw = 0;
    for (int i = 0; i < n; i++) {
        char name[SAV_NAME_CAP], alt[SAV_NAME_CAP];
        entry_name(&files[i], name);
        SavName k = name_kind(files[i].name);
        if (sav_file_write(sav_path_in(ar, name), files[i].data, files[i].len, k.kind)) nw++;
        else *ok = false;
        if (k.slot >= 0 && k.legacy_name && sav_slot_name(k.slot, alt, false)) {
            const char *q = sav_path_in(ar, alt);
            if (plat_path_is_file(q) && !SDL_RemovePath(q)) log_msg(LOG_WARN, "save: cannot remove %s", q);
        }
    }
    return nw;
}

static bool import_files(const SavBundleFile *files, int n)
{
    Scratch s = mem_scratch_begin(NULL);
    const char *bdir = backup_dir(s.arena);
    int nbak = 0;
    bool ok = backup_existing(s.arena, files, n, bdir, &nbak);
    if (!ok) {
        log_msg(LOG_ERR, "save: import aborted: cannot back up the current saves (%s)", SDL_GetError());
    } else {
        int nw = write_entries(s.arena, files, n, &ok);
        log_msg(ok ? LOG_INFO : LOG_ERR, "save: imported %d/%d files into %s%s%s", nw, n, sav_dir(),
                nbak ? "; previous files backed up in " : "", nbak ? bdir : "");
    }
    mem_scratch_end(&s);
    return ok;
}

bool sav_import(const char *bundle_path)
{
    size_t len;
    sav_clear_error();
    uint8_t *d = sav_file_read(bundle_path, &len);
    if (!d) { log_msg(LOG_ERR, "save: cannot read %s", bundle_path); return false; }
    Scratch s = mem_scratch_begin(NULL);
    SavBundleFile *files = ARENA_NEW_ARRAY(s.arena, SavBundleFile, SAV_BUNDLE_MAX);
    int n = sav_bundle_parse(d, len, files, SAV_BUNDLE_MAX);
    bool ok = n >= 0;
    if (!ok) log_msg(LOG_ERR, "save: %s rejected: %s (nothing was changed)", bundle_path, sav_last_error());
    else ok = import_files(files, n);
    mem_scratch_end(&s);
    sav_file_free(d);
    return ok;
}
