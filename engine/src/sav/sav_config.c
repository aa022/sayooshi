/* sav/sav_config.c — config.sav: the user configuration as named, range-clamped keys (docs/SAVE_FORMAT.md §8);
 * defaults from Gameexe #CONFIG.*. Unknown keys are ignored, missing keys keep their value, values are clamped. */
#include "sav/sav_internal.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "vm/sysstate.h"
#include "snd/audio.h"
#include "fmt/gameexe.h"
#include "plat/platform.h"
#include <stddef.h>
#include <string.h>

static SavConfig g_config;
SavConfig *sav_config(void) { return &g_config; }

void sav_config_defaults(void)
{
    SavConfig *c = &g_config;
    memset(c, 0, sizeof *c);
    c->audio.vol_all = gx_int("CONFIG.VOLUME.ALL", 0, 255);
    c->audio.vol_bgm = gx_int("CONFIG.VOLUME.BGM", 0, 255);
    c->audio.vol_koe = gx_int("CONFIG.VOLUME.KOE", 0, 255);
    c->audio.vol_pcm = gx_int("CONFIG.VOLUME.PCM", 0, 255);
    c->audio.vol_se  = gx_int("CONFIG.VOLUME.SE", 0, 255);
    c->audio.on_all = c->audio.on_bgm = c->audio.on_koe = c->audio.on_pcm = c->audio.on_se = true;
    c->audio.bgm_fade = gx_int("CONFIG.BGMFADE_ONOFF", 0, 1) != 0;
    c->audio.bgm_fade_vol = gx_int("CONFIG.BGMFADE_VOLUME", 0, 192);
    c->msg_speed = gx_int("CONFIG.MESSAGE_SPEED", 0, 20);
    c->msg_nowait = gx_int("CONFIG.MESSAGE_SPEED_NOWAIT.ONOFF", 0, 0) != 0;
    c->auto_moji_wait = SAV_AUTO_MOJI_WAIT_DEFAULT;
    c->auto_min_wait = SAV_AUTO_MIN_WAIT_DEFAULT;
    c->font_shadow = gx_int("CONFIG.FONT.SHADOW", 0, 2);
    c->font_futoku = gx_int("CONFIG.FONT.FUTOKU", 0, 0) != 0;
    c->skip_unread = gx_int("CONFIG.SKIP_UNREAD_MESSAGE.ONOFF", 0, 0) != 0;
    c->koe_dont_stop = gx_int("CONFIG.KOE_DONT_STOP.ONOFF", 0, 0) != 0;
    c->wheel_next = gx_int("CONFIG.WHEEL_NEXT_MESSAGE.ONOFF", 0, 1) != 0;
    c->skip_wipe_anime = gx_int("CONFIG.SKIP_WIPE_ANIME.ONOFF", 0, 1) != 0;
    c->no_wipe_anime = gx_int("CONFIG.NO_WIPE_ANIME.ONOFF", 0, 0) != 0;
    c->no_mwnd_anime = gx_int("CONFIG.NO_MWND_ANIME.ONOFF", 0, 0) != 0;
    c->chrcolor = gx_int("CONFIG.MESSAGE_CHRCOLOR.ONOFF", 0, 1) != 0;
    c->window_mode = gx_int("CONFIG.WINDOW_MODE", 0, 0);
    c->window_scale = 1;
}

void sav_config_apply(void)
{
    audio_set_config(&g_config.audio);
    sys_state()->msg_speed = g_config.msg_nowait ? 0 : g_config.msg_speed;
    sys_state()->auto_moji_wait = g_config.auto_moji_wait;
    sys_state()->auto_min_wait = g_config.auto_min_wait;
    if (!plat_headless()) plat_set_fullscreen(g_config.window_mode == 1);
}

/* Config keys (SAVE_FORMAT.md §8): name, field, min, max. Booleans are 0/1. Values are clamped on load. */
typedef enum { CK_INT, CK_BOOL } CfgKind;
typedef struct { const char *key; CfgKind kind; size_t off; int lo, hi; } CfgKey;
#define CKI(k, f, lo, hi) { k, CK_INT, offsetof(SavConfig, f), lo, hi }
#define CKB(k, f)         { k, CK_BOOL, offsetof(SavConfig, f), 0, 1 }
static const CfgKey CFG_KEYS[] = {
    CKI("volume.all", audio.vol_all, 0, 255), CKI("volume.bgm", audio.vol_bgm, 0, 255),
    CKI("volume.koe", audio.vol_koe, 0, 255), CKI("volume.pcm", audio.vol_pcm, 0, 255),
    CKI("volume.se", audio.vol_se, 0, 255),
    CKB("on.all", audio.on_all), CKB("on.bgm", audio.on_bgm), CKB("on.koe", audio.on_koe),
    CKB("on.pcm", audio.on_pcm), CKB("on.se", audio.on_se), CKB("bgmfade.on", audio.bgm_fade),
    CKI("bgmfade.volume", audio.bgm_fade_vol, 0, 255),
    CKI("message.speed", msg_speed, 0, 1000), CKB("message.nowait", msg_nowait),
    CKI("auto.moji_wait", auto_moji_wait, 0, 10000), CKI("auto.min_wait", auto_min_wait, 0, 60000),
    CKI("font.shadow", font_shadow, 0, 3), CKB("font.futoku", font_futoku),
    CKB("skip.unread", skip_unread), CKB("koe.dont_stop", koe_dont_stop), CKB("wheel.next", wheel_next),
    CKB("anime.skip_wipe", skip_wipe_anime), CKB("anime.no_wipe", no_wipe_anime), CKB("anime.no_mwnd", no_mwnd_anime),
    CKB("message.chrcolor", chrcolor),
    CKI("window.mode", window_mode, 0, 1), CKI("window.scale", window_scale, 1, 8),
    CKI("language", language, 0, 2),
};
enum { NCFG_KEYS = (int)COUNT_OF(CFG_KEYS), CFG_MAX_ENTRIES = 4096 };

static int cfg_get(const SavConfig *c, const CfgKey *k)
{
    const char *p = (const char *)c + k->off;
    if (k->kind == CK_BOOL) { bool b; memcpy(&b, p, sizeof b); return b ? 1 : 0; }
    int v; memcpy(&v, p, sizeof v); return v;
}

static void cfg_set(SavConfig *c, const CfgKey *k, int32_t v)
{
    v = core_clamp_i32(v, k->lo, k->hi);
    char *p = (char *)c + k->off;
    if (k->kind == CK_BOOL) { bool b = v != 0; memcpy(p, &b, sizeof b); }
    else { int iv = (int)v; memcpy(p, &iv, sizeof iv); }
}

static const CfgKey *cfg_find(Str key)
{
    for (int k = 0; k < NCFG_KEYS; k++) if (str_eq(str_c(CFG_KEYS[k].key), key)) return &CFG_KEYS[k];
    return NULL;
}

SavWriter *sav_config_encode(const SavConfig *c, const char *engv, int64_t t)
{
    SavWriter *w = sav_writer_new();
    sav_file_begin(w, SAV_KIND_CONF);
    sav_put_chunk(w, SAV_TAG_META);
    sav_put_enter(w);
    sav_put_chunk(w, SAV_TAG_GAME); sav_put_str8(w, SAV_GAME_ID);
    sav_put_chunk(w, SAV_TAG_ENGV); sav_put_str8(w, engv);
    sav_put_chunk(w, SAV_TAG_TIME); sav_put_i64(w, t);
    sav_put_leave(w);
    sav_put_chunk(w, SAV_TAG_CFG2);
    sav_put_u32(w, NCFG_KEYS);
    for (int i = 0; i < NCFG_KEYS; i++) { sav_put_str8(w, CFG_KEYS[i].key); sav_put_i32(w, cfg_get(c, &CFG_KEYS[i])); }
    sav_file_end(w);
    return w;
}

/* Format 1: CONF{magic, version, fields in the fixed order of the old serialise_config}. */
static bool decode_v1(SavReader *r, SavConfig *t)
{
    static const char *const ORDER[] = { "volume.all", "volume.bgm", "volume.koe", "volume.pcm", "volume.se",
        "on.all", "on.bgm", "on.koe", "on.pcm", "on.se", "bgmfade.on", "bgmfade.volume", "message.speed",
        "message.nowait", "auto.moji_wait", "auto.min_wait", "font.shadow", "font.futoku", "skip.unread",
        "koe.dont_stop", "wheel.next", "anime.skip_wipe", "anime.no_wipe", "anime.no_mwnd", "message.chrcolor",
        "window.mode", "window.scale" };
    if (!sav_find_chunk(r, SAV_KIND_CONF) || sav_get_u32(r) != SAV_OLD_MAGIC || sav_get_i32(r) != 1) return false;
    for (size_t i = 0; i < COUNT_OF(ORDER); i++) {
        const CfgKey *k = cfg_find(str_c(ORDER[i]));
        ASSERT(k);   /* every format-1 field is a current key */
        cfg_set(t, k, k->kind == CK_BOOL ? sav_get_u8(r) : sav_get_i32(r));
    }
    return sav_ok(r);
}

/* Format 2: CFG2{u32 n, n x (str8 key, i32 value)}. */
static bool decode_v2(SavReader *r, SavConfig *t)
{
    bool ok = sav_find_chunk(r, SAV_TAG_CFG2);
    int n = sav_get_count(r, CFG_MAX_ENTRIES, 8);
    for (int i = 0; ok && i < n; i++) {
        Str key;
        (void)sav_get_str8_view(r, &key);   /* checked with sav_ok */
        int32_t val = sav_get_i32(r);
        ok = sav_ok(r);
        const CfgKey *k = ok ? cfg_find(key) : NULL;
        if (k) cfg_set(t, k, val);
    }
    return ok && sav_ok(r);
}

bool sav_config_decode(const uint8_t *d, size_t len, SavConfig *c)
{
    SavFileView v;
    if (!sav_file_view(d, len, &v) || v.kind != SAV_KIND_CONF) return false;
    SavReader *r = sav_view_reader(d, &v);
    SavConfig t = *c;
    bool ok = v.fmt == SAV_FMT_V1 ? decode_v1(r, &t) : decode_v2(r, &t);
    if (ok) *c = t;
    sav_reader_free(r);
    return ok;
}

void sav_config_write(void)
{
    SavWriter *w = sav_config_encode(&g_config, SAYO_ENGINE_VERSION, sav_now_unix());
    Scratch s = mem_scratch_begin(NULL);
    (void)sav_file_store(sav_path_in(s.arena, "config.sav"), w, SAV_KIND_CONF);   /* failures are logged there */
    mem_scratch_end(&s);
    sav_writer_free(w);
}

bool sav_config_read(void)
{
    Scratch s = mem_scratch_begin(NULL);
    const char *path = sav_path_in(s.arena, "config.sav");
    size_t len;
    SavFileView v;
    int which = 0;
    uint8_t *d = sav_file_read_best(path, &len, &v, &which);
    bool ok = d && sav_config_decode(d, len, &g_config);
    if (ok && v.fmt == SAV_FMT_V1) {
        if (which == 0) sav_file_copy_once(path, ".v1");
        sav_config_write();
        log_msg(LOG_INFO, "save: config.sav converted to format %d", SAV_VERSION);
    }
    if (!ok && sav_file_exists_any(path)) log_msg(LOG_WARN, "save: config.sav unreadable; using defaults");
    sav_file_free(d);
    mem_scratch_end(&s);
    return ok;
}
