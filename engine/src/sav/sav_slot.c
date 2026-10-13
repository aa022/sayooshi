/* sav/sav_slot.c — slot files (slot_NNN.sav, quick_N.sav): META/THMB/SNAP layout, RGB565 thumbnails, slot info
 * for the UI, load with format-1 conversion (docs/SAVE_FORMAT.md §5).
 *
 * Memory: thumbnails being written live in scratch arenas; SavInfo.thumb_rgb is a tracked heap block released by
 * sav_info_free; slot file bytes come from sav_file_read (sav_file_free). */
#include "sav/sav_internal.h"
#include "core/bin.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "txt/lang.h"
#include "gfx/canvas.h"
#include "gfx/gfx.h"
#include "txt/backlog.h"
#include <SDL3/SDL.h>
#include <string.h>

enum { THUMB_RGB565 = 0,                       /* THMB encoding id */
       THUMB_MAX_DIM = 512,                    /* larger stored thumbnails are ignored */
       V1_THUMB_W = 240, V1_THUMB_H = 135 };   /* format 1 stored RGB888 at this size */

/* ================================================================== thumbnails */

void sav_thumb_to_565(const uint8_t *rgb, int npx, uint8_t *out)
{
    for (size_t i = 0; i < (size_t)(npx > 0 ? npx : 0); i++) {
        unsigned r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        bin_store_le16(out + i * 2, (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)));
    }
}

static void rgb565_to_rgb(const uint8_t *in, int npx, uint8_t *rgb)
{
    for (size_t i = 0; i < (size_t)(npx > 0 ? npx : 0); i++) {
        unsigned v = bin_load_le16(in + i * 2), r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        rgb[i * 3] = (uint8_t)(r << 3 | r >> 2);
        rgb[i * 3 + 1] = (uint8_t)(g << 2 | g >> 4);
        rgb[i * 3 + 2] = (uint8_t)(b << 3 | b >> 2);
    }
}

static void canvas_to_rgb(const Canvas *c, uint8_t *rgb)   /* c->w * c->h pixels, opaque */
{
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            uint32_t p = c->px[(size_t)y * (size_t)c->pitch + (size_t)x];
            uint8_t *o = rgb + ((size_t)y * (size_t)c->w + (size_t)x) * 3;
            o[0] = (uint8_t)(p >> 16); o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)p;
        }
}

/* The stored thumbnail size of an RGB888 image (sw, sh >= 1): box filter (canvas_downscale_box). */
static void rgb_resize(const uint8_t *src, int sw, int sh, uint8_t dst[SAV_THUMB_RGB_BYTES])
{
    ASSERT(sw > 0 && sh > 0);
    Scratch s = mem_scratch_begin(NULL);
    Canvas in = { ARENA_NEW_ARRAY(s.arena, uint32_t, (size_t)sw * (size_t)sh), sw, sh, sw };
    Canvas out = { ARENA_NEW_ARRAY(s.arena, uint32_t, SAV_THUMB_PX), SAV_THUMB_W, SAV_THUMB_H, SAV_THUMB_W };
    for (size_t i = 0; i < (size_t)sw * (size_t)sh; i++)
        in.px[i] = 0xff000000u | (uint32_t)src[i * 3] << 16 | (uint32_t)src[i * 3 + 1] << 8 | src[i * 3 + 2];
    canvas_downscale_box(&out, &in);
    canvas_to_rgb(&out, dst);
    mem_scratch_end(&s);
}

/* The game picture taken when the first system screen opened (sav_thumb_freeze): a save made from the save screen
 * shows the game, not the save screen drawn over it. */
static uint8_t g_frozen[SAV_THUMB_RGB_BYTES];
static bool g_frozen_ok;

static void capture_live(uint8_t rgb[SAV_THUMB_RGB_BYTES])
{
    Scratch s = mem_scratch_begin(NULL);
    Canvas c = { ARENA_NEW_ARRAY(s.arena, uint32_t, SAV_THUMB_PX), SAV_THUMB_W, SAV_THUMB_H, SAV_THUMB_W };
    gfx_capture(&c);
    canvas_to_rgb(&c, rgb);
    mem_scratch_end(&s);
}

void sav_thumb_freeze(void)
{
    capture_live(g_frozen);
    g_frozen_ok = true;
}

void sav_thumb_thaw(void) { g_frozen_ok = false; }

void sav_thumb_capture(uint8_t rgb[SAV_THUMB_RGB_BYTES])
{
    if (g_frozen_ok) memcpy(rgb, g_frozen, SAV_THUMB_RGB_BYTES);
    else capture_live(rgb);
}

static void put_thumb(SavWriter *w, const uint8_t *rgb)
{
    sav_put_chunk(w, SAV_TAG_THMB);
    sav_put_u32(w, rgb ? SAV_THUMB_W : 0);
    sav_put_u32(w, rgb ? SAV_THUMB_H : 0);
    sav_put_u32(w, THUMB_RGB565);
    if (!rgb) return;
    Scratch s = mem_scratch_begin(NULL);
    uint8_t *p565 = arena_alloc_uninit(s.arena, (size_t)SAV_THUMB_PX * 2, 1);
    sav_thumb_to_565(rgb, SAV_THUMB_PX, p565);
    sav_put_bytes(w, p565, (size_t)SAV_THUMB_PX * 2);
    mem_scratch_end(&s);
}

/* ================================================================== encoding and writing */

static void current_message(WStr *out)
{
    /* Slot list preview: the latest backlog text (the message on screen when the slot is written). */
    ws_set(out, NULL, 0);
    for (int i = backlog_count() - 1; i >= 0; i--) {
        const BacklogEntry *e = backlog_get(i);
        if (e && e->text.len > 0) { ws_copy(out, &e->text); return; }
    }
}

/* The call-stack record that follows the snapshot header (SHDR: i64 time, str16 title, str8 lang, stack). */
static void put_stack_copy(SavWriter *w, const SavSnap *s)
{
    SavReader *r = sav_reader_new(s->data, s->len);
    Str lang;
    bool ok = sav_find_chunk(r, SAV_TAG_SHDR);
    (void)sav_get_i64(r);
    (void)sav_get_wstr(r, NULL);
    (void)sav_get_str8_view(r, &lang);   /* fields skipped; sav_ok covers them */
    if (ok && sav_ok(r)) sav_put_bytes(w, sav_cur(r), sav_remaining(r));
    sav_reader_free(r);
}

/* SLOT file = header, META{GAME,ENGV,LANG,TIME,PACK,TITL,MESG,STAK}, THMB, SNAP{snapshot chunks}, END. */
SavWriter *sav_slot_encode(const SavSnap *s, const WStr *msg, const uint8_t *thumb_rgb)
{
    SavSnapHead h;
    if (!s->data || !sav_snap_head(s->data, s->len, false, &h)) return NULL;
    ws_free(&h.title);
    SavWriter *w = sav_writer_new();
    sav_file_begin(w, SAV_KIND_SLOT);
    sav_put_chunk(w, SAV_TAG_META);
    sav_put_enter(w);
    sav_put_meta(w, h.lang, s->time);
    sav_put_chunk(w, SAV_TAG_TITL); sav_put_wstr(w, &s->title);
    sav_put_chunk(w, SAV_TAG_MESG); sav_put_wstr(w, msg);
    sav_put_chunk(w, SAV_TAG_STAK); put_stack_copy(w, s);
    sav_put_leave(w);
    put_thumb(w, thumb_rgb);
    sav_put_chunk(w, SAV_TAG_SNAP);
    sav_put_bytes(w, s->data, s->len);
    sav_file_end(w);
    return w;
}

bool sav_slot_write_snap(int slot, const SavSnap *s, const WStr *msg, const uint8_t *thumb_rgb)
{
    char name[SAV_NAME_CAP];
    if (!sav_slot_name(slot, name, false)) return false;
    SavWriter *w = sav_slot_encode(s, msg, thumb_rgb);
    if (!w) { log_msg(LOG_ERR, "save: snapshot of slot %d is corrupt; not written", slot); return false; }
    Scratch sc = mem_scratch_begin(NULL);
    const char *path = sav_path_in(sc.arena, name);
    size_t len = 0;
    (void)sav_writer_data(w, &len);   /* the size for the log line; sav_file_store checks the data */
    bool ok = sav_file_store(path, w, SAV_KIND_SLOT);
    if (ok) {
        log_msg(LOG_INFO, "save: slot %d written (%s, %u bytes)", slot, path, (unsigned)len);
        log_trace("SAVE", "save slot=%d bytes=%u", slot, (unsigned)len);
    }
    mem_scratch_end(&sc);
    sav_writer_free(w);
    return ok;
}

bool sav_slot_write(int slot)
{
    char name[SAV_NAME_CAP];
    if (!sav_slot_name(slot, name, false)) { log_msg(LOG_WARN, "save: bad slot %d", slot); return false; }
    if (!sav_snap_cur()->data) {
        log_msg(LOG_WARN, "save: no save point yet; slot %d not written", slot);
        return false;
    }
    WStr msg = { 0 };
    current_message(&msg);
    Scratch s = mem_scratch_begin(NULL);
    uint8_t *thumb = arena_alloc_uninit(s.arena, SAV_THUMB_RGB_BYTES, 1);
    sav_thumb_capture(thumb);
    bool ok = sav_slot_write_snap(slot, sav_snap_cur(), &msg, thumb);
    mem_scratch_end(&s);
    ws_free(&msg);
    if (ok) sav_global_write();   /* SYS §10.1: global data after every save */
    return ok;
}

/* ================================================================== parsing */

/* META of a format-2 slot: time, language, title, message, and whether its call stack loads here. */
static bool parse_meta_v2(SavReader *r, SavInfo *out)
{
    if (!sav_find_chunk(r, SAV_TAG_META) || !sav_get_enter(r)) return false;
    if (sav_find_chunk(r, SAV_TAG_TIME)) out->unix_time = sav_get_i64(r);
    if (sav_find_chunk(r, SAV_TAG_LANG)) (void)sav_get_str8(r, out->lang, sizeof out->lang);
    if (sav_find_chunk(r, SAV_TAG_TITL)) (void)sav_get_wstr(r, &out->title);
    if (sav_find_chunk(r, SAV_TAG_MESG)) (void)sav_get_wstr(r, &out->message);
    bool ok = sav_ok(r);   /* covers the reads above */
    sav_reset_fail(r);
    if (sav_find_chunk(r, SAV_TAG_STAK)) {
        out->state = sav_check_stack(r, out->lang, out->note);
    } else {
        out->state = sav_refusal_state(out->lang);
        str_fmt_cut(out->note, sizeof out->note, "no call-stack record");
    }
    sav_reset_fail(r);
    sav_get_leave(r);
    return ok;
}

/* Optional THMB: a damaged or odd thumbnail only means no thumbnail. */
static void parse_thumb_v2(SavReader *r, SavInfo *out)
{
    if (!sav_find_chunk(r, SAV_TAG_THMB)) return;
    uint32_t tw = sav_get_u32(r), th = sav_get_u32(r), enc = sav_get_u32(r);
    if (sav_ok(r) && enc == THUMB_RGB565 && tw && th && tw <= THUMB_MAX_DIM && th <= THUMB_MAX_DIM &&
        sav_remaining(r) >= (size_t)tw * th * 2) {
        out->thumb_rgb = mem_alloc((size_t)tw * th * 3, MEM_SAV);
        rgb565_to_rgb(sav_cur(r), (int)(tw * th), out->thumb_rgb);
        out->thumb_w = (int)tw;
        out->thumb_h = (int)th;
    }
    sav_reset_fail(r);
}

static bool parse_v1(SavReader *r, SavInfo *out)
{
    if (!sav_find_chunk(r, SAV_TAG_INFO) || sav_get_u32(r) != SAV_OLD_MAGIC || sav_get_i32(r) != 1) return false;
    out->unix_time = sav_get_i64(r);
    (void)sav_get_wstr(r, &out->title);
    (void)sav_get_wstr(r, &out->message);
    int32_t tw = sav_get_i32(r), th = sav_get_i32(r);
    bool ok = sav_ok(r);   /* covers the reads above */
    size_t tbytes = (size_t)V1_THUMB_W * V1_THUMB_H * 3;
    if (ok && tw == V1_THUMB_W && th == V1_THUMB_H && sav_remaining(r) >= tbytes) {
        out->thumb_rgb = mem_dup(sav_cur(r), tbytes, MEM_SAV);
        out->thumb_w = V1_THUMB_W;
        out->thumb_h = V1_THUMB_H;
    }
    str_fmt_cut(out->lang, sizeof out->lang, "%s", lang_code_of(LANG_JA));   /* format 1: the Japanese release only */
    out->state = lang_id() == LANG_JA ? SAV_SLOT_OK : SAV_SLOT_OLD_FORMAT;
    if (out->state != SAV_SLOT_OK) str_fmt_cut(out->note, sizeof out->note, "old-format save made with the 'ja' text");
    return ok;
}

/* Parse the info part of a slot file (either format). snap/snap_len: the embedded snapshot (points into d).
 * On failure *out is empty (nothing to release) and the error text says why. */
bool sav_slot_parse(const uint8_t *d, size_t len, SavInfo *out, const uint8_t **snap, size_t *snap_len, bool *legacy)
{
    memset(out, 0, sizeof *out);
    *snap = NULL;
    *snap_len = 0;
    *legacy = false;
    SavFileView v;
    if (!sav_file_view(d, len, &v)) return false;
    if (v.kind != SAV_KIND_SLOT) { sav_set_error("not a slot file"); return false; }
    SavReader *r = sav_view_reader(d, &v);
    *legacy = v.fmt == SAV_FMT_V1;
    bool ok = *legacy ? parse_v1(r, out) : parse_meta_v2(r, out);
    if (ok && !*legacy) parse_thumb_v2(r, out);
    ok = ok && sav_find_chunk(r, SAV_TAG_SNAP);
    if (ok) {
        *snap = sav_cur(r);
        *snap_len = sav_remaining(r);
    }
    sav_reader_free(r);
    if (!ok) {
        if (!sav_last_error()[0]) sav_set_error("slot file is damaged");
        sav_info_free(out);
        memset(out, 0, sizeof *out);
        return false;
    }
    out->used = true;
    return true;
}

void sav_info_free(SavInfo *si)
{
    if (!si) return;
    ws_free(&si->title);
    ws_free(&si->message);
    mem_free(si->thumb_rgb);
    si->thumb_rgb = NULL;
    si->thumb_w = si->thumb_h = 0;
    si->used = false;
}

/* ================================================================== reading and loading */

/* The best version of a slot file: the format-2 name, then the format-1 name. Release with sav_file_free. */
static uint8_t *slot_read(int slot, size_t *len)
{
    *len = 0;
    uint8_t *d = NULL;
    Scratch s = mem_scratch_begin(NULL);
    for (int k = 0; k < 2 && !d; k++) {
        char name[SAV_NAME_CAP];
        if (!sav_slot_name(slot, name, k == 1)) break;
        SavFileView v;
        d = sav_file_read_best(sav_path_in(s.arena, name), len, &v, NULL);
    }
    mem_scratch_end(&s);
    return d;
}

bool sav_slot_info(int slot, SavInfo *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    size_t len;
    uint8_t *d = slot_read(slot, &len);
    if (!d) return false;
    const uint8_t *snap;
    size_t snap_len;
    bool legacy;
    bool ok = sav_slot_parse(d, len, out, &snap, &snap_len, &legacy);
    sav_file_free(d);
    return ok;
}

/* A loaded format-1 slot is rewritten in format 2 under the new name; the old file is kept as <name>.v1. */
static void convert_v1_slot(int slot, const SavInfo *info)
{
    Scratch s = mem_scratch_begin(NULL);
    uint8_t *thumb = NULL;
    if (info->thumb_rgb && info->thumb_w > 0 && info->thumb_h > 0) {
        thumb = arena_alloc_uninit(s.arena, SAV_THUMB_RGB_BYTES, 1);
        rgb_resize(info->thumb_rgb, info->thumb_w, info->thumb_h, thumb);
    }
    char name[SAV_NAME_CAP];
    if (sav_slot_write_snap(slot, sav_snap_cur(), &info->message, thumb) && sav_slot_name(slot, name, true)) {
        const char *old = sav_path_in(s.arena, name), *keep = str_cat_arena(s.arena, old, ".v1", NULL);
        if (SDL_RenamePath(old, keep))
            log_msg(LOG_INFO, "save: slot %d converted to format %d (old file kept as %s)", slot, SAV_VERSION, keep);
    }
    mem_scratch_end(&s);
}

/* Restore from the bytes of a slot file; format-1 slots are converted when `convert`. */
bool sav_slot_load_bytes(int slot, const uint8_t *d, size_t len, bool convert)
{
    SavInfo info;
    const uint8_t *snap = NULL;
    size_t snap_len = 0;
    bool legacy = false;
    sav_clear_error();
    if (!sav_slot_parse(d, len, &info, &snap, &snap_len, &legacy)) {
        log_msg(LOG_ERR, "save: slot %d: %s", slot, sav_last_error());
        return false;
    }
    bool ok = sav_snap_load(snap, snap_len, legacy, true);
    if (ok && legacy && convert) convert_v1_slot(slot, &info);
    sav_info_free(&info);
    return ok;
}

bool sav_slot_load(int slot)
{
    size_t len;
    sav_clear_error();
    uint8_t *d = slot_read(slot, &len);
    if (!d) {
        sav_set_error("no save data in slot %d", slot);
        log_msg(LOG_WARN, "save: slot %d: no usable file in %s", slot, sav_dir());
        return false;
    }
    bool ok = sav_slot_load_bytes(slot, d, len, true);
    sav_file_free(d);
    if (ok) {
        log_msg(LOG_INFO, "save: slot %d loaded", slot);
        log_trace("SAVE", "load slot=%d", slot);
    }
    return ok;
}
