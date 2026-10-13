/* snd/snd_capture.c — capture of the mixed output to a 16-bit stereo WAV (--audio-capture) and level statistics.
 *
 * Threads: snd_capture_feed runs inside mixer_render with the mixer lock held (the device callback, or the main
 * thread in null-sink mode). Every other function takes the mixer lock around the shared fields below and does
 * its file opening/closing outside it, so the callback never waits for a file open. In null-sink mode
 * audio_frame mixes exactly real_dt ms per frame, so the capture is deterministic with --fixed-dt. */
#include "snd/snd_internal.h"
#include "core/bin.h"
#include "core/log.h"
#include <SDL3/SDL.h>
#include <math.h>

enum { WAV_HEADER = 44, IO_CHUNK = 4096 };
#define WAV_DATA_MAX 0xffffffd0u                     /* largest data size the 32-bit RIFF sizes can hold */

/* Shared with the audio thread: mixer lock. */
static SDL_IOStream *g_cap;
static bool g_cap_failed;                            /* a write came up short */
static uint64_t g_cap_frames;
static int g_cap_peak;
static double g_cap_sum;
static uint64_t g_win_frames;                        /* selftest window */
static double g_win_sum;

static void wav_header(uint8_t out[WAV_HEADER], uint32_t data_bytes)
{
    Wr w;
    wr_init_fixed(&w, out, WAV_HEADER);
    wr_bytes(&w, "RIFF", 4);
    wr_u32le(&w, 36 + data_bytes);
    wr_bytes(&w, "WAVEfmt ", 8);
    wr_u32le(&w, 16);                                /* fmt chunk size */
    wr_u16le(&w, 1);                                 /* PCM */
    wr_u16le(&w, 2);                                 /* channels */
    wr_u32le(&w, AUDIO_RATE);
    wr_u32le(&w, AUDIO_RATE * 4);                    /* byte rate */
    wr_u16le(&w, 4);                                 /* block align */
    wr_u16le(&w, 16);                                /* bits per sample */
    wr_bytes(&w, "data", 4);
    wr_u32le(&w, data_bytes);
    ASSERT(wr_ok(&w) && wr_len(&w) == WAV_HEADER);
}

static bool write_all(SDL_IOStream *io, const void *p, size_t n) { return SDL_WriteIO(io, p, n) == n; }

static void accumulate(const int16_t *buf, int frames)
{
    for (int i = 0; i < 2 * frames; i++) {
        int a = buf[i] < 0 ? -buf[i] : buf[i];
        double f = buf[i] / 32768.0;
        if (a > g_cap_peak) g_cap_peak = a;
        g_cap_sum += f * f;
        g_win_sum += f * f;
    }
    g_cap_frames += (uint64_t)frames;
    g_win_frames += (uint64_t)frames;
}

void snd_capture_feed(const int16_t *buf, int frames)
{
    ASSERT(mixer_lock_held());
    accumulate(buf, frames);
    if (!g_cap) return;
    uint8_t tmp[IO_CHUNK];
    size_t k = 0;
    for (int i = 0; i < 2 * frames; i++) {
        bin_store_le16(tmp + k, (uint16_t)buf[i]);
        k += 2;
        if (k == sizeof tmp) { g_cap_failed |= !write_all(g_cap, tmp, k); k = 0; }
    }
    if (k) g_cap_failed |= !write_all(g_cap, tmp, k);
}

bool audio_capture_start(const char *wav_path)
{
    audio_capture_stop();
    uint8_t h[WAV_HEADER];
    wav_header(h, 0);
    SDL_IOStream *io = wav_path ? SDL_IOFromFile(wav_path, "wb") : NULL;
    if (io && !write_all(io, h, sizeof h)) { (void)SDL_CloseIO(io); io = NULL; }   /* unusable: report below */
    mixer_lock();
    g_cap = io;
    g_cap_failed = false;
    g_cap_frames = 0;
    g_cap_peak = 0;
    g_cap_sum = 0;
    mixer_unlock();
    if (!io) log_msg(LOG_ERR, "audio: cannot write %s: %s", wav_path ? wav_path : "(null)", SDL_GetError());
    return io != NULL;
}

void audio_capture_stop(void)
{
    mixer_lock();
    SDL_IOStream *io = g_cap;
    uint64_t bytes = g_cap_frames * 4;
    bool failed = g_cap_failed;
    g_cap = NULL;
    mixer_unlock();
    if (!io) return;
    uint8_t h[WAV_HEADER];
    wav_header(h, bytes > WAV_DATA_MAX ? WAV_DATA_MAX : (uint32_t)bytes);
    if (SDL_SeekIO(io, 0, SDL_IO_SEEK_SET) != 0 || !write_all(io, h, sizeof h)) failed = true;
    if (!SDL_CloseIO(io)) failed = true;
    if (failed) log_msg(LOG_ERR, "audio: the capture file is incomplete: %s", SDL_GetError());
}

void snd_capture_stats(AudioStats *out)
{
    mixer_lock();
    out->frames = g_cap_frames;
    out->peak = g_cap_peak;
    out->rms = g_cap_frames ? sqrt(g_cap_sum / (double)(g_cap_frames * 2)) : 0.0;
    mixer_unlock();
}

void snd_capture_window_reset(void)
{
    mixer_lock();
    g_win_frames = 0;
    g_win_sum = 0;
    mixer_unlock();
}

double snd_capture_window_rms(void)
{
    mixer_lock();
    double rms = g_win_frames ? sqrt(g_win_sum / (double)(g_win_frames * 2)) : 0.0;
    mixer_unlock();
    return rms;
}
