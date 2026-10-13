/* snd/audio.c — the public sound API: BGM (two crossfading streams), koe, pcmch, the SDL output device or the
 * null sink, init/frame/shutdown and save/load (SYS §2, AST §2–3, ENGINE_SPEC §2.10–2.11).
 *
 * Threads: main thread only, except dev_callback, which runs on SDL's audio thread. The callback only calls
 * mixer_render (voices + capture under the mixer lock) inside a no-allocation zone and hands the samples to SDL.
 * Ownership: g_bgm_name and the pcmch names are WStr on the tracked heap, freed by audio_shutdown. */
#include "snd/audio.h"
#include "snd/snd_internal.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "fmt/gameexe.h"
#include "sav/sav_stream.h"
#include "vm/proc.h"
#include <SDL3/SDL.h>

enum { DEV_BLOCK = 1024, DEV_MAX_BLOCKS = 64, FRAME_BYTES = 4 };   /* stereo s16 */

typedef struct PlayRange { uint32_t start, end, loop; } PlayRange;

static bool g_inited;
static bool g_audio_subsystem;                      /* SDL_InitSubSystem(AUDIO) succeeded */
static SDL_AudioStream *g_dev;
static int64_t g_drain_acc;                         /* null sink: frames * 1000 not yet mixed */

/* BGM object (S+0x000): current stream (+0xd4), name/loop for save. */
static int g_bgm_cur = -1;                          /* 0/1 = MIX_BGM0/1, -1 none */
static WStr g_bgm_name;
static bool g_bgm_loop;
static struct { WStr name; int vt; bool loop; } g_pc[PCMCH_CNT];

static bool pcmch_ok(int ch) { return ch >= 0 && ch < PCMCH_CNT; }

/* ------------------------------------------------------------------------------------------------ output */

static void SDLCALL dev_callback(void *ud, SDL_AudioStream *st, int additional, int total)
{
    (void)ud; (void)total;
    int16_t buf[2 * DEV_BLOCK];
    mem_noalloc_enter();
    for (int k = 0; additional >= FRAME_BYTES && k < DEV_MAX_BLOCKS; k++) {
        int n = additional / FRAME_BYTES;
        if (n > DEV_BLOCK) n = DEV_BLOCK;
        mixer_render(buf, n);
        (void)SDL_PutAudioStreamData(st, buf, n * FRAME_BYTES);   /* a dropped block is only a glitch */
        additional -= n * FRAME_BYTES;
    }
    mem_noalloc_leave();
}

static void open_device(void)
{
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, AUDIO_RATE };
    g_audio_subsystem = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (g_audio_subsystem)
        g_dev = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, dev_callback, NULL);
    if (g_dev && !SDL_ResumeAudioStreamDevice(g_dev)) {
        SDL_DestroyAudioStream(g_dev);
        g_dev = NULL;
    }
    if (!g_dev) log_msg(LOG_WARN, "audio: no output device (%s); using the null sink", SDL_GetError());
}

static void close_device(void)
{
    if (g_dev) SDL_DestroyAudioStream(g_dev);       /* stops the callback before anything below is freed */
    g_dev = NULL;
    if (g_audio_subsystem) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    g_audio_subsystem = false;
}

/* Null sink: consume exactly dt ms of output so that captures and one-shot ends follow the frame clock. */
static void null_sink_drain(int32_t dt)
{
    int16_t buf[2 * DEV_BLOCK];
    g_drain_acc += (int64_t)dt * AUDIO_RATE;
    int64_t frames = g_drain_acc / 1000;
    g_drain_acc %= 1000;
    while (frames > 0) {
        int n = frames > DEV_BLOCK ? DEV_BLOCK : (int)frames;
        mixer_render(buf, n);
        frames -= n;
    }
}

/* ------------------------------------------------------------------------------------------------ init / frame */

static void names_reset(void)
{
    g_bgm_cur = -1;
    g_bgm_name.len = 0;
    g_bgm_loop = false;
    for (int c = 0; c < PCMCH_CNT; c++) { g_pc[c].name.len = 0; g_pc[c].vt = CAT_PCM; g_pc[c].loop = false; }
}

bool audio_init(bool real_device)
{
    if (g_inited) audio_shutdown();
    snd_elm_init();                            /* the sound roots and wait procs answer even without a mixer */
    if (!mixer_init()) return false;
    snd_assets_init();
    snd_streams_reset();
    names_reset();
    g_inited = true;
    g_drain_acc = 0;
    snd_config_defaults();
    bgm_table_load();
    if (!bgm_table_count()) log_msg(LOG_WARN, "audio: no #BGM table (Gameexe not loaded?); regname = file name");
    if (real_device) open_device();
    return g_dev != NULL;
}

void audio_shutdown(void)
{
    if (!g_inited) return;
    close_device();
    audio_capture_stop();
    snd_streams_reset();                            /* every voice stopped: the PCM is ours again */
    snd_assets_shutdown();
    mixer_shutdown();
    names_reset();
    ws_free(&g_bgm_name);
    for (int c = 0; c < PCMCH_CNT; c++) ws_free(&g_pc[c].name);
    g_inited = false;
}

void audio_frame(int32_t real_dt)
{
    if (!g_inited) return;
    if (real_dt < 0) real_dt = 0;
    snd_streams_tick(real_dt);
    snd_assets_trim();
    if (!g_dev) null_sink_drain(real_dt);
}

void audio_set_config(const struct AudioConfig *cfg) { if (cfg) snd_config_set(cfg); }

void audio_stop_all(int fade_ms)
{
    if (!g_inited) return;
    bgm_stop(fade_ms);
    for (int i = 0; i < MIX_NVOICE; i++) snd_stream_fade_out(i, fade_ms, false);
    for (int c = 0; c < PCMCH_CNT; c++) g_pc[c].loop = false;
}

/* ------------------------------------------------------------------------------------------------ BGM */

/* Clamp the table positions to the file (FUN_006ae780): end -1 or > frames -> frames; start/repeat in [0,end]. */
static PlayRange bgm_range(const BgmEntry *e, uint32_t frames, int start_pos)
{
    int64_t end = e->end;
    if (end == -1 || end > (int64_t)frames) end = frames;
    if (end < 0) end = 0;
    int64_t s = start_pos == -1 ? e->start : start_pos, r = e->repeat;
    s = s < 0 ? 0 : s > end ? end : s;
    r = r < 0 ? 0 : r > end ? end : r;
    return (PlayRange){ (uint32_t)s, (uint32_t)end, (uint32_t)r };
}

/* The table entry's file for regname reg (UTF-8), or NULL (logged). */
static MixSound *bgm_open(const char *reg, int start_pos, PlayRange *r)
{
    BgmEntry e;
    if (!bgm_table_find(reg, &e)) { log_msg(LOG_WARN, "audio: bgm '%s' not in #BGM table", reg); return NULL; }
    MixSound *snd = snd_load_nwa("bgm", e.file);
    if (snd) *r = bgm_range(&e, snd->frames, start_pos);
    return snd;
}

static int bgm_voice_of(int cur) { return MIX_BGM0 + cur; }

int snd_bgm_voice(void) { return g_bgm_cur >= 0 ? bgm_voice_of(g_bgm_cur) : -1; }

/* Replaying the current looping track with loop does not restart it. */
static bool bgm_same_loop(const WStr *regname, bool loop)
{
    return g_bgm_name.len && g_bgm_loop && loop && !ws_cmp_fold(regname, &g_bgm_name) && g_bgm_cur >= 0 &&
           snd_stream_active(bgm_voice_of(g_bgm_cur));
}

/* FUN_005db060 */
void bgm_play(const WStr *regname, bool loop, bool wait, int fade_in, int fade_out_old, int start_pos, int delay)
{
    if (!g_inited || !regname) return;
    Scratch sc = mem_scratch_begin(NULL);
    const char *reg = ws_utf8_arena(sc.arena, regname, WS_UTF8_SHORT_MAX);
    log_trace("SND", "bgm play %s loop=%d fade_in=%d", reg, loop ? 1 : 0, fade_in);
    if (bgm_same_loop(regname, loop)) { mem_scratch_end(&sc); return; }
    if (g_bgm_cur >= 0) {
        if (fade_out_old < 1) snd_stream_kill(bgm_voice_of(g_bgm_cur));
        else snd_stream_fade_out(bgm_voice_of(g_bgm_cur), fade_out_old, false);
    }
    g_bgm_name.len = 0;
    g_bgm_loop = false;
    PlayRange r = { 0, 0, 0 };
    MixSound *snd = bgm_open(reg, start_pos, &r);
    mem_scratch_end(&sc);
    if (!snd) return;
    int nxt = g_bgm_cur < 0 ? 0 : (g_bgm_cur + 1) & 1;
    snd_stream_play(bgm_voice_of(nxt), snd, r.start, r.end, r.loop, loop, fade_in, delay);
    g_bgm_cur = nxt;
    ws_copy(&g_bgm_name, regname);
    g_bgm_loop = loop;
    if (wait) {
        Proc p = { .type = PROC_BGM_WAIT };
        proc_push(&p);
    }
}

void bgm_stop(int fade_ms)
{
    if (!g_inited) return;
    log_trace("SND", "bgm stop fade=%d", fade_ms);
    if (g_bgm_cur >= 0) snd_stream_fade_out(bgm_voice_of(g_bgm_cur), fade_ms, false);
    g_bgm_name.len = 0;
    g_bgm_loop = false;
}

void bgm_set_volume(int v, int ms) { snd_bgm_volume_set(v, ms); }
int  bgm_get_volume(void) { return snd_bgm_volume_get(); }
void bgm_pause(int fade_ms)
{
    if (g_inited && g_bgm_cur >= 0) snd_stream_fade_out(bgm_voice_of(g_bgm_cur), fade_ms, true);
}
void bgm_resume(int delay) { if (g_inited && g_bgm_cur >= 0) snd_stream_resume(bgm_voice_of(g_bgm_cur), delay); }
bool bgm_playing(void) { return g_inited && g_bgm_cur >= 0 && snd_stream_active(bgm_voice_of(g_bgm_cur)); }
bool bgm_fading(void) { return snd_stream_fading(MIX_BGM0) || snd_stream_fading(MIX_BGM1); }
int  bgm_play_pos_ms(void) { return bgm_playing() ? snd_stream_pos_ms(bgm_voice_of(g_bgm_cur)) : -1; }

/* Read-only for callers. Before the first bgm_play (or after audio_shutdown) it is a static "" so that asking
 * for the name never allocates memory nobody would free. */
WStr *bgm_current_name(void)
{
    static uint16_t empty_units[1];
    static WStr empty = { empty_units, 0, 0 };
    return g_bgm_name.p ? &g_bgm_name : &empty;
}

/* ------------------------------------------------------------------------------------------------ koe */

bool koe_play(int koe_no, int chara, int vol)
{
    if (!g_inited) return false;
    (void)chara;                                    /* per-character voice volume (#CHRKOE) is not in this game */
    snd_stream_kill(MIX_KOE);
    MixSound *snd = snd_load_koe(koe_no);
    if (!snd) return false;
    int64_t v = (int64_t)vol * 255 / 100;
    snd_slot_set(MIX_KOE, SLOT_PLAY, v < 0 ? 0 : v > 255 ? 255 : (int)v, 0);
    snd_stream_play(MIX_KOE, snd, 0, snd->frames, 0, false, 0, 0);
    return true;
}

void koe_stop(int fade_ms) { if (g_inited) snd_stream_fade_out(MIX_KOE, fade_ms, false); }
bool koe_playing(void) { return g_inited && snd_stream_active(MIX_KOE); }
void koe_set_volume(int v, int ms) { snd_slot_set(MIX_KOE, SLOT_SCRIPT, v, ms); }
int  koe_get_volume(void) { return snd_slot_get(MIX_KOE, SLOT_SCRIPT); }

/* ------------------------------------------------------------------------------------------------ pcmch */

static MixSound *whole(MixSound *s, PlayRange *r)
{
    if (s) *r = (PlayRange){ 0, s->frames, 0 };
    return s;
}

static MixSound *source_bgm(const WStr *bgm_name, PlayRange *r)     /* FUN_005dd770 */
{
    Scratch sc = mem_scratch_begin(NULL);
    BgmEntry e;
    bool found = bgm_table_find(ws_utf8_arena(sc.arena, bgm_name, WS_UTF8_SHORT_MAX), &e);
    mem_scratch_end(&sc);
    MixSound *s = found ? snd_load_nwa("bgm", e.file) : NULL;
    if (s) *r = bgm_range(&e, s->frames, -1);
    return s;
}

static MixSound *source_se_no(int se_no, PlayRange *r)              /* FUN_005dd9a0: #SE.nnn (empty here) */
{
    char key[24];
    if (!str_fmt(key, sizeof key, "SE.%03d", se_no)) return NULL;   /* an int always fits */
    WStr s = {0};
    MixSound *snd = NULL;
    if (gx_str(key, 0, &s) && s.len) {
        Scratch sc = mem_scratch_begin(NULL);
        snd = whole(snd_load_se(ws_utf8_arena(sc.arena, &s, WS_UTF8_SHORT_MAX)), r);
        mem_scratch_end(&sc);
    }
    ws_free(&s);
    return snd;
}

/* FUN_006660d0 order: name (wav/), bgm regname, koe_no, se_no. name_utf8 is a->name as UTF-8 (or NULL). */
static MixSound *pcmch_source(const PcmchArgs *a, const char *name_utf8, PlayRange *r)
{
    if (name_utf8) return whole(snd_load_se(name_utf8), r);          /* FUN_005dd620 */
    if (a->bgm_name.len) return source_bgm(&a->bgm_name, r);
    if (a->has_koe && a->koe_no >= 0) return whole(snd_load_koe(a->koe_no), r);   /* FUN_005dd8b0 */
    if (a->se_no >= 0) return source_se_no(a->se_no, r);
    return NULL;
}

void pcmch_play(int ch, const PcmchArgs *a)
{
    if (!g_inited || !pcmch_ok(ch) || !a) return;
    Scratch sc = mem_scratch_begin(NULL);
    const char *name = a->name.len ? ws_utf8_arena(sc.arena, &a->name, WS_UTF8_SHORT_MAX) : NULL;
    PlayRange r = { 0, 0, 0 };
    MixSound *snd = pcmch_source(a, name, &r);
    log_trace("SND", "pcmch %d play %s loop=%d wait=%d fade_in=%d%s", ch, name ? name : "-", a->loop ? 1 : 0,
              a->wait ? 1 : 0, a->fade_in, snd ? "" : " MISSING");
    mem_scratch_end(&sc);
    if (!snd) return;
    int i = MIX_PCMCH0 + ch;
    snd_stream_play(i, snd, r.start, r.end, r.loop, a->loop, a->fade_in, 0);
    snd_stream_set_category(i, a->volume_type >= 0 && a->volume_type <= CAT_MOV ? a->volume_type : CAT_PCM);
    ws_copy(&g_pc[ch].name, &a->name);
    g_pc[ch].vt = a->volume_type;
    g_pc[ch].loop = a->loop;
    if (a->wait) {
        Proc p = { .type = PROC_PCMCH_WAIT };
        p.iarg[0] = ch;
        proc_push(&p);
    }
}

void pcmch_stop(int ch, int fade_ms)
{
    if (!g_inited || !pcmch_ok(ch)) return;
    snd_stream_fade_out(MIX_PCMCH0 + ch, fade_ms, false);
    g_pc[ch].loop = false;
}

void pcmch_pause(int ch, int fade_ms)
{
    if (g_inited && pcmch_ok(ch)) snd_stream_fade_out(MIX_PCMCH0 + ch, fade_ms, true);
}
void pcmch_resume(int ch, int delay) { if (g_inited && pcmch_ok(ch)) snd_stream_resume(MIX_PCMCH0 + ch, delay); }
void pcmch_set_volume(int ch, int v, int ms) { if (pcmch_ok(ch)) snd_slot_set(MIX_PCMCH0 + ch, SLOT_SCRIPT, v, ms); }
int  pcmch_get_volume(int ch) { return pcmch_ok(ch) ? snd_slot_get(MIX_PCMCH0 + ch, SLOT_SCRIPT) : 0; }
bool pcmch_playing(int ch) { return g_inited && pcmch_ok(ch) && snd_stream_active(MIX_PCMCH0 + ch); }
bool pcmch_fading(int ch) { return pcmch_playing(ch) && snd_stream_fading(MIX_PCMCH0 + ch); }

/* ------------------------------------------------------------------------------------------------ save / load */

/* Snapshot (ENGINE_SPEC §2.11): BGM name/loop + slot-1 volumes; looping pcmch channels only. */
void audio_save(SavWriter *w)
{
    sav_put_chunk(w, FOURCC('A', 'U', 'D', '1'));
    bool bgm = g_bgm_name.len && g_bgm_loop && bgm_playing();
    sav_put_u8(w, bgm);
    sav_put_wstr(w, bgm_current_name());
    sav_put_i32(w, snd_bgm_volume_get());
    sav_put_i32(w, koe_get_volume());
    for (int c = 0; c < PCMCH_CNT; c++) {
        bool on = g_pc[c].loop && pcmch_playing(c) && g_pc[c].name.len;
        sav_put_u8(w, on);
        sav_put_i32(w, pcmch_get_volume(c));
        if (on) { sav_put_wstr(w, &g_pc[c].name); sav_put_i32(w, g_pc[c].vt); }
    }
}

/* One pcmch record: volume, and a looping channel to restart. */
static void load_pcmch(SavReader *r, int c)
{
    bool on = sav_get_u8(r) != 0;
    pcmch_set_volume(c, sav_get_i32(r), 0);
    g_pc[c].loop = false;
    if (!on) return;
    PcmchArgs a = { .loop = true, .se_no = -1 };
    sav_get_wstr(r, &a.name);
    a.volume_type = sav_get_i32(r);
    if (sav_ok(r)) pcmch_play(c, &a);
    ws_free(&a.name);
}

bool audio_load(SavReader *r)
{
    if (!sav_find_chunk(r, FOURCC('A', 'U', 'D', '1'))) return false;
    WStr name = {0};
    bool bgm = sav_get_u8(r) != 0;
    sav_get_wstr(r, &name);
    int bv = sav_get_i32(r), kv = sav_get_i32(r);
    if (g_inited) for (int i = 0; i < MIX_NVOICE; i++) snd_stream_kill(i);
    g_bgm_name.len = 0;
    g_bgm_loop = false;
    snd_bgm_volume_set(bv, 0);
    koe_set_volume(kv, 0);
    if (bgm && sav_ok(r)) bgm_play(&name, true, false, 0, 0, -1, 0);
    for (int c = 0; c < PCMCH_CNT && sav_ok(r); c++) load_pcmch(r, c);
    ws_free(&name);
    return sav_ok(r);
}
