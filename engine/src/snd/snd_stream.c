/* snd/snd_stream.c — streams: one per mixer voice, with 8 multiplied volume slots, fade-in/fade-out, delayed
 * start/resume, config category volumes and BGM ducking (SYS §2.2–2.6, AST §2.3, FUN_006aeb10 family).
 *
 * Time is the audio clock g_now: wall-clock ms advanced by snd_streams_tick(real_dt). Time differences use
 * two's-complement wrap (since()), so the clock may run for any length of time. Volumes are 0..255.
 * Each playing stream owns one MixSound reference, handed back after mixer_stop. Main thread only. */
#include "snd/snd_internal.h"
#include <math.h>
#include <string.h>

enum { VOL_MAX = 255, DUCK_IDLE = 1 << 20, DUCK_RAMP_MS = 1000, DUCK_HOLD_MS = 2000 };

typedef struct VolSlot { bool ramp; int vol; int32_t t0, t1; int from, to; } VolSlot;
typedef struct Fade { bool on; int32_t t0; int dur, from; } Fade;

typedef struct Stream {
    MixSound *snd;                                  /* owned reference while playing / pending / paused */
    VolSlot slot[SLOT_COUNT];                       /* 0 config, 1 script, 2 play volume (koe vol parameter) */
    Fade fin, fout;
    int fout_cur;                                   /* fade-out level reached (a shorter fade starts here) */
    int delay;                                      /* ms until start / resume */
    uint32_t p_start, p_end, p_loop;                /* play range for a delayed start */
    int cat;
    bool fout_pause;                                /* the fade-out pauses instead of stopping */
    bool pending;                                   /* voice not started yet (delay) */
    bool paused;
    bool p_looping;
} Stream;

static Stream g_st[MIX_NVOICE];
static int32_t g_now;
static AudioConfig g_cfg;
static const VolSlot SLOT_FULL = { false, VOL_MAX, 0, 0, VOL_MAX, VOL_MAX };
static VolSlot g_bgm_vol = { false, VOL_MAX, 0, 0, VOL_MAX, VOL_MAX };   /* slot 1 of both BGM streams */

/* ducking state ([a8ff08]+0x11d..0x128) */
static bool g_duck_on;
static int g_duck_t = DUCK_IDLE, g_duck_start = VOL_MAX, g_duck_cur = VOL_MAX;

static int32_t since(int32_t t) { return core_wrap_sub_i32(g_now, t); }
static int32_t later(int32_t ms) { return core_wrap_add_i32(g_now, ms); }

static Stream *stream_at(int i)
{
    ASSERT_MSG(i >= 0 && i < MIX_NVOICE, "stream %d out of range", i);
    return &g_st[i];
}

/* ------------------------------------------------------------------------------------------------ volume slots */

static void slot_set(VolSlot *s, int v, int ms)    /* FUN_006ae8f0 */
{
    v = core_clamp_i32(v, 0, VOL_MAX);
    s->ramp = false;
    if (ms > 0) { s->ramp = true; s->t0 = g_now; s->t1 = later(ms); s->from = s->vol; s->to = v; }
    s->vol = v;
}

static int slot_value(VolSlot *s)
{
    if (!s->ramp) return s->vol;
    int v = s->to;
    int32_t d = core_wrap_sub_i32(s->t1, s->t0);
    if (d != 0) {
        int32_t t = since(s->t0);
        t = t < 0 ? 0 : t > d ? d : t;
        v = (int)((int64_t)t * (s->to - s->from) / d) + s->from;
    }
    if (since(s->t1) >= 0) s->ramp = false;
    return v;
}

static VolSlot *slot_of(int i, int k)
{
    ASSERT(k >= 0 && k < SLOT_COUNT);
    return (i <= MIX_BGM1 && k == SLOT_SCRIPT) ? &g_bgm_vol : &stream_at(i)->slot[k];
}

void snd_slot_set(int i, int slot, int v, int ms) { slot_set(slot_of(i, slot), v, ms); }
int  snd_slot_get(int i, int slot) { return slot_of(i, slot)->vol; }
void snd_bgm_volume_set(int v, int ms) { slot_set(&g_bgm_vol, v, ms); }
int  snd_bgm_volume_get(void) { return g_bgm_vol.vol; }
int  snd_bgm_volume_now(void) { return slot_value(&g_bgm_vol); }

float snd_gain_of(int v)    /* SYS §2.2: DirectSound attenuation, (int)(5000*log10(v/255)) cB/100 */
{
    if (v <= 0) return 0.0f;
    if (v >= VOL_MAX) return 1.0f;
    int att = (int)(5000.0 * log10(v / 255.0));
    if (att <= -10000) return 0.0f;
    return (float)pow(10.0, att / 2000.0);
}

/* ------------------------------------------------------------------------------------------------ config */

static AudioConfig sanitized(const AudioConfig *c)
{
    AudioConfig s = *c;
    s.vol_all = core_clamp_i32(s.vol_all, 0, VOL_MAX);
    s.vol_bgm = core_clamp_i32(s.vol_bgm, 0, VOL_MAX);
    s.vol_koe = core_clamp_i32(s.vol_koe, 0, VOL_MAX);
    s.vol_pcm = core_clamp_i32(s.vol_pcm, 0, VOL_MAX);
    s.vol_se = core_clamp_i32(s.vol_se, 0, VOL_MAX);
    s.bgm_fade_vol = core_clamp_i32(s.bgm_fade_vol, 0, VOL_MAX);
    return s;
}

void snd_config_set(const AudioConfig *cfg) { g_cfg = sanitized(cfg); }

void snd_config_defaults(void)
{
    static const AudioConfig DEFAULTS = AUDIO_CONFIG_DEFAULT;
    snd_config_set(&DEFAULTS);
}

static int cat_volume(int cat)    /* FUN_005a7cd0 config mixing */
{
    int all = g_cfg.on_all ? g_cfg.vol_all : 0;
    int v, on;
    switch (cat) {
    case CAT_BGM: v = g_cfg.vol_bgm; on = g_cfg.on_bgm; break;
    case CAT_KOE: v = g_cfg.vol_koe; on = g_cfg.on_koe; break;
    case CAT_SE:  v = g_cfg.vol_se;  on = g_cfg.on_se;  break;
    case CAT_MOV: v = VOL_MAX; on = 1; break;
    default:      v = g_cfg.vol_pcm; on = g_cfg.on_pcm; break;
    }
    return on ? all * v / VOL_MAX : 0;
}

/* ------------------------------------------------------------------------------------------------ streams */

void snd_stream_kill(int i)
{
    Stream *s = stream_at(i);
    mixer_stop(i);                                  /* the voice lets go of the PCM before we release it */
    snd_sound_unref(s->snd);
    s->snd = NULL;
    s->fin.on = s->fout.on = s->fout_pause = s->pending = s->paused = false;
    s->delay = 0;
}

bool snd_stream_active(int i)
{
    const Stream *s = stream_at(i);
    return s->snd && (s->pending || mixer_active(i));
}

bool snd_stream_fading(int i) { return snd_stream_active(i) && (g_st[i].fin.on || g_st[i].fout.on); }

void snd_stream_set_category(int i, int cat) { stream_at(i)->cat = cat; }

const MixSound *snd_stream_sound(int i) { return stream_at(i)->snd; }

int snd_stream_pos_ms(int i)
{
    if (!snd_stream_active(i)) return -1;
    const Stream *s = &g_st[i];
    uint32_t pos = s->pending ? s->p_start : mixer_pos(i);
    return (int)((uint64_t)pos * 1000 / (uint32_t)s->snd->rate);
}

/* Fade-in level 0..255 (ends the fade when it is over). */
static int fade_in_level(Fade *f)
{
    int32_t e = since(f->t0);
    int64_t x = VOL_MAX;
    if (f->dur > 0) x = (int64_t)(VOL_MAX - f->from) * core_clamp_i32(e, 0, f->dur) / f->dur + f->from;
    if ((int64_t)e - f->dur >= 0) f->on = false;
    return (int)(x < 0 ? 0 : x > VOL_MAX ? VOL_MAX : x);
}

/* Fade-out level; at its end the stream pauses (fout_pause) or is killed. Returns -1 when killed. */
static int fade_out_level(int i, Stream *s)
{
    int32_t e = since(s->fout.t0);
    int d = s->fout.dur, x = 0;
    if (d > 0) x = s->fout.from - (int)((int64_t)core_clamp_i32(e, 0, d) * s->fout.from / d);
    s->fout_cur = x;
    if ((int64_t)e - d >= 0) {
        s->fout.on = false;
        if (!s->fout_pause) { snd_stream_kill(i); return -1; }
        s->fout_pause = false;
        s->paused = true;
        mixer_set_paused(i, true);
    }
    return core_clamp_i32(x, 0, VOL_MAX);
}

/* FUN_006aeb10: v = prod(slots)/255^n, then fade-in, then fade-out. 0 when the fade-out killed the stream. */
static int stream_eval(int i)
{
    Stream *s = stream_at(i);
    int v = VOL_MAX;
    for (int k = 0; k < SLOT_COUNT; k++) v = v * slot_value(slot_of(i, k)) / VOL_MAX;
    if (s->fin.on) v = v * fade_in_level(&s->fin) / VOL_MAX;
    if (s->fout.on) {
        int x = fade_out_level(i, s);
        if (x < 0) return 0;
        v = v * x / VOL_MAX;
    }
    return v;
}

void snd_stream_play(int i, MixSound *snd, uint32_t start, uint32_t end, uint32_t loop_start, bool loop,
                     int fade_in, int delay)
{
    ASSERT(snd);
    snd_stream_kill(i);
    Stream *s = &g_st[i];
    s->snd = snd;
    if (fade_in > 0) s->fin = (Fade){ true, later(delay > 0 ? delay : 0), fade_in, 0 };
    s->p_start = start; s->p_end = end; s->p_loop = loop_start; s->p_looping = loop;
    if (delay > 0) { s->pending = true; s->delay = delay; return; }
    mixer_start(i, snd, start, end, loop_start, loop, snd_gain_of(stream_eval(i)));
}

/* FUN_006ae5c0 (stop) / FUN_006ae4d0 (pause): a running fade is only replaced by one that ends sooner. */
void snd_stream_fade_out(int i, int ms, bool pause)
{
    Stream *s = stream_at(i);
    if (!snd_stream_active(i)) return;
    if (ms <= 0) {
        if (!pause) { snd_stream_kill(i); return; }
        s->paused = true; s->fout.on = false; mixer_set_paused(i, true);
        return;
    }
    if (!s->fout.on) { s->fout = (Fade){ true, g_now, ms, VOL_MAX }; s->fout_cur = VOL_MAX; }
    else if ((int64_t)s->fout.dur - since(s->fout.t0) - ms > 0) s->fout = (Fade){ true, g_now, ms, s->fout_cur };
    s->fout_pause = pause;
}

void snd_stream_resume(int i, int delay)
{
    Stream *s = stream_at(i);
    if (!s->snd || !s->paused) return;
    if (delay > 0) { s->delay = delay; return; }
    s->paused = false;
    mixer_set_paused(i, false);
}

/* ------------------------------------------------------------------------------------------------ frame */

/* BGM ducking while a voice plays (FUN_005a7cd0, [a8ff08]+0x11d..0x128): on a state change the counter
 * restarts from the current value; ducked: ramp to BGMFADE_VOLUME over 0..1000 ms; released: hold until
 * 2000 ms, then ramp back to 255 until 3000 ms. */
static void duck_update(int32_t dt)
{
    bool want = g_cfg.bgm_fade && cat_volume(CAT_KOE) > 0 && snd_stream_active(MIX_KOE);
    if (g_duck_t < DUCK_IDLE) g_duck_t += dt < DUCK_IDLE ? dt : DUCK_IDLE;
    if (want != g_duck_on) { g_duck_t = 0; g_duck_start = g_duck_cur; g_duck_on = want; }
    int t, target;
    if (g_duck_on) { t = core_clamp_i32(g_duck_t, 0, DUCK_RAMP_MS); target = g_cfg.bgm_fade_vol; }
    else { t = core_clamp_i32(g_duck_t, DUCK_HOLD_MS, DUCK_HOLD_MS + DUCK_RAMP_MS) - DUCK_HOLD_MS; target = VOL_MAX; }
    g_duck_cur = (target - g_duck_start) * t / DUCK_RAMP_MS + g_duck_start;
}

int snd_duck_level(void) { return g_duck_cur; }

/* Delayed starts and resumes whose delay ran out. */
static void tick_delay(int i, Stream *s, int32_t dt)
{
    if (!s->snd || s->delay <= 0) return;
    s->delay -= dt;
    if (s->delay > 0) return;
    s->delay = 0;
    if (s->pending) { s->pending = false; mixer_start(i, s->snd, s->p_start, s->p_end, s->p_loop, s->p_looping, 0.0f); }
    else if (s->paused) { s->paused = false; mixer_set_paused(i, false); }
}

static void tick_gain(int i, Stream *s)
{
    if (!s->snd) return;
    int cv = cat_volume(s->cat);
    if (i <= MIX_BGM1) cv = cv * g_duck_cur / VOL_MAX;
    s->slot[SLOT_CONFIG].vol = cv;
    s->slot[SLOT_CONFIG].ramp = false;
    int v = stream_eval(i);
    if (!s->snd) return;
    if (!s->pending && !mixer_active(i)) { snd_stream_kill(i); return; }   /* a one-shot reached its end */
    mixer_set_gain(i, snd_gain_of(v));
}

void snd_streams_tick(int32_t dt)
{
    ASSERT(dt >= 0);
    g_now = later(dt);
    for (int i = 0; i < MIX_NVOICE; i++) tick_delay(i, &g_st[i], dt);
    duck_update(dt);
    for (int i = 0; i < MIX_NVOICE; i++) tick_gain(i, &g_st[i]);
}

void snd_streams_reset(void)
{
    for (int i = 0; i < MIX_NVOICE; i++) {
        snd_stream_kill(i);
        memset(&g_st[i], 0, sizeof g_st[i]);
        for (int k = 0; k < SLOT_COUNT; k++) g_st[i].slot[k] = SLOT_FULL;
        g_st[i].cat = i <= MIX_BGM1 ? CAT_BGM : i == MIX_KOE ? CAT_KOE : CAT_PCM;
    }
    g_now = 0;
    g_bgm_vol = SLOT_FULL;
    g_duck_on = false;
    g_duck_t = DUCK_IDLE;
    g_duck_start = g_duck_cur = VOL_MAX;
}
