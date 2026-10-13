/* snd/mixer.c — software mixer: one voice per stream, linear-interpolation resampling to AUDIO_RATE, loop points,
 * per-voice gain ramped over each block (ENGINE_SPEC §2.10, AST §2.3).
 *
 * Threads (docs/CODE_STANDARD.md §8): the voice table is the only engine state the SDL audio callback touches.
 * Every access holds g_mtx, a leaf lock: nothing here allocates, logs or calls another module while holding it,
 * except the capture tap (snd_capture_feed), which is part of the same lock domain. The voices borrow MixSound
 * pointers whose PCM is immutable while shared; the main thread frees a sound only after mixer_stop. */
#include "snd/snd_internal.h"
#include <SDL3/SDL.h>
#include <string.h>

typedef struct Voice {
    const MixSound *snd;
    bool active, paused, loop;
    uint64_t pos, step;                             /* 32.32 frames */
    uint32_t end, loop_start;
    float gain, target;
} Voice;

static Voice g_v[MIX_NVOICE];
static SDL_Mutex *g_mtx;
static SDL_ThreadID g_owner;                        /* written only while g_mtx is held */
static int g_depth;

static void check_voice(int v) { ASSERT_MSG(v >= 0 && v < MIX_NVOICE, "mixer voice %d out of range", v); }

/* ------------------------------------------------------------------------------------------------ the lock */

/* Without a mutex (before mixer_init, or when it failed) there is no device thread, so locking is a no-op; the
 * owner bookkeeping still runs so mixer_lock_held stays meaningful. */
void mixer_lock(void)
{
    if (g_mtx) SDL_LockMutex(g_mtx);
    g_owner = SDL_GetCurrentThreadID();
    g_depth++;
}

void mixer_unlock(void)
{
    ASSERT_MSG(mixer_lock_held(), "mixer_unlock without the lock");
    if (--g_depth == 0) g_owner = 0;
    if (g_mtx) SDL_UnlockMutex(g_mtx);
}

bool mixer_lock_held(void) { return g_depth > 0 && g_owner == SDL_GetCurrentThreadID(); }

bool mixer_init(void)
{
    ASSERT(g_depth == 0);
    if (!g_mtx) g_mtx = SDL_CreateMutex();
    memset(g_v, 0, sizeof g_v);
    return g_mtx != NULL;
}

/* The device stream is destroyed before this runs (audio_shutdown), so no callback can hold the lock. */
void mixer_shutdown(void)
{
    ASSERT(g_depth == 0);
    memset(g_v, 0, sizeof g_v);
    if (g_mtx) SDL_DestroyMutex(g_mtx);
    g_mtx = NULL;
}

/* ------------------------------------------------------------------------------------------------ voice control */

void mixer_start(int v, MixSound *s, uint32_t start, uint32_t end, uint32_t loop_start, bool loop, float gain)
{
    check_voice(v);
    ASSERT(s && s->pcm && s->channels >= 1 && s->channels <= SND_CHANNELS_MAX && s->rate > 0 && s->refs > 0);
    if (end > s->frames) end = s->frames;
    if (start > end) start = end;
    if (loop_start > end) loop_start = end;
    mixer_lock();
    Voice *x = &g_v[v];
    x->snd = s;
    x->pos = (uint64_t)start << 32;
    x->step = ((uint64_t)(uint32_t)s->rate << 32) / AUDIO_RATE;
    x->end = end;
    x->loop_start = loop_start;
    x->loop = loop && loop_start < end;
    x->gain = x->target = gain;
    x->paused = false;
    x->active = start < end;
    mixer_unlock();
}

void mixer_stop(int v)
{
    check_voice(v);
    mixer_lock();
    g_v[v].active = false;
    g_v[v].snd = NULL;
    mixer_unlock();
}

void mixer_set_gain(int v, float gain)
{
    check_voice(v);
    mixer_lock();
    g_v[v].target = gain;
    mixer_unlock();
}

void mixer_set_paused(int v, bool paused)
{
    check_voice(v);
    mixer_lock();
    g_v[v].paused = paused;
    mixer_unlock();
}

bool mixer_active(int v)
{
    check_voice(v);
    mixer_lock();
    bool a = g_v[v].active && g_v[v].snd;
    mixer_unlock();
    return a;
}

uint32_t mixer_pos(int v)
{
    check_voice(v);
    mixer_lock();
    uint32_t p = (uint32_t)(g_v[v].pos >> 32);
    mixer_unlock();
    return p;
}

bool mixer_uses(const MixSound *s)
{
    bool used = false;
    mixer_lock();
    for (int v = 0; v < MIX_NVOICE && !used; v++) used = g_v[v].snd == s;
    mixer_unlock();
    return used;
}

/* ------------------------------------------------------------------------------------------------ mixing */

/* AST §2.3: on reaching end a looping voice continues at repeat_pos (keeping the fractional overshoot); a
 * one-shot voice ends. Returns the frame index to play, or false when the voice ended. */
static bool wrap_position(Voice *x, uint32_t *idx)
{
    *idx = (uint32_t)(x->pos >> 32);
    if (*idx < x->end) return true;
    if (!x->loop) { x->active = false; return false; }
    uint64_t over = x->pos - ((uint64_t)x->end << 32);
    uint64_t len = (uint64_t)(x->end - x->loop_start) << 32;   /* > 0: loop requires loop_start < end */
    x->pos = ((uint64_t)x->loop_start << 32) + over % len;
    *idx = (uint32_t)(x->pos >> 32);
    return true;
}

/* Adds n frames of voice x to acc (interleaved stereo float, full scale 32768). acc NULL only advances the
 * position, exactly as mixing would (silent voices keep their timing). */
static void mix_voice(Voice *x, float *acc, int n)
{
    const MixSound *s = x->snd;
    const int16_t *p = s->pcm;
    const size_t ch = (size_t)s->channels;
    const float g0 = x->gain, dg = (x->target - x->gain) / (float)n;
    for (int i = 0; i < n; i++) {
        uint32_t idx;
        if (!wrap_position(x, &idx)) break;
        uint32_t nx = idx + 1;
        if (nx >= x->end) nx = x->loop ? x->loop_start : idx;
        if (acc) {
            float fr = (float)(uint32_t)x->pos * (1.0f / 4294967296.0f);
            float g = g0 + dg * (float)(i + 1);
            const int16_t *a = p + (size_t)idx * ch, *b = p + (size_t)nx * ch;   /* idx, nx < end <= frames */
            float l0 = a[0], l1 = b[0];
            float l = l0 + (l1 - l0) * fr, r = l;
            if (ch == 2) {
                float r0 = a[1], r1 = b[1];
                r = r0 + (r1 - r0) * fr;
            }
            float *o = acc + 2 * (size_t)i;
            o[0] += l * g;
            o[1] += r * g;
        }
        x->pos += x->step;
    }
    x->gain = x->target;
}

static int16_t to_s16(float f)
{
    int s = (int)(f >= 0 ? f + 0.5f : f - 0.5f);
    return (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

/* One block (n <= SND_BLOCK) with the lock held. */
static void mix_block(int16_t *out, int n)
{
    float acc[2 * SND_BLOCK];
    memset(acc, 0, sizeof(float) * 2 * (size_t)n);
    for (int v = 0; v < MIX_NVOICE; v++) {
        Voice *x = &g_v[v];
        if (!x->active || !x->snd || x->paused) continue;
        bool silent = x->gain == 0.0f && x->target == 0.0f;
        mix_voice(x, silent ? NULL : acc, n);
    }
    for (int i = 0; i < 2 * n; i++) out[i] = to_s16(acc[i]);
}

void mixer_render(int16_t *out, int frames)
{
    if (!out || frames <= 0) return;
    while (frames > 0) {
        int n = frames < SND_BLOCK ? frames : SND_BLOCK;
        mixer_lock();
        mix_block(out, n);
        snd_capture_feed(out, n);
        mixer_unlock();
        out += 2 * (size_t)n;
        frames -= n;
    }
}
