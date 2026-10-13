/* snd/snd_internal.h — shared by the snd/ sources only (docs/CODE_STANDARD.md §2, §8).
 *
 *  mixer.c        voices + the mixer lock, the only state the SDL audio callback touches
 *  snd_capture.c  WAV capture and level statistics of the mixed output (under the mixer lock)
 *  snd_asset.c    MixSound (refcounted PCM), NWA/SE/koe loading, the #BGM table
 *  snd_stream.c   streams: volume slots, fades, config categories, BGM ducking, the audio clock
 *  audio.c        public BGM/koe/pcmch API, device / null sink, init/frame/shutdown, save/load
 *  snd_elm.c      script elements and wait procs;  snd_test.c  the audio selftest
 *
 * Threads: everything here is main-thread only except the functions marked [audio thread]. */
#ifndef SND_SND_INTERNAL_H
#define SND_SND_INTERNAL_H
#include "core/base.h"
#include "core/str.h"
#include "fmt/nwa.h"
#include "snd/audio.h"

/* ---- voices: one mixer voice per stream ---------------------------------------------------------------------- */
enum { MIX_BGM0, MIX_BGM1, MIX_KOE, MIX_PCMCH0, MIX_NVOICE = MIX_PCMCH0 + PCMCH_CNT };
enum { SND_BLOCK = 512 };                           /* mixing granularity in frames (gain ramps per block) */
enum { SND_RATE_MAX = 192000, SND_CHANNELS_MAX = 2 };
enum { PROC_PCMCH_WAIT = 0x1f };                    /* pcmch play_wait/wait (FUN_005ddb40 pushes type 0x1f) */

/* ---- MixSound: decoded PCM shared with the mixer -------------------------------------------------------------
 * Interleaved s16, immutable once created. refs is touched by the main thread only; the mixer holds a borrowed
 * pointer while a voice plays it, and the stream that started the voice owns one reference until mixer_stop.
 * The last snd_sound_unref frees the PCM (asserting that no voice still points at it). */
typedef struct MixSound {
    const int16_t *pcm;
    uint32_t frames;
    int channels, rate;                             /* 1..SND_CHANNELS_MAX, 1..SND_RATE_MAX */
    int refs;
    NwaSound nwa;                                   /* owns pcm for NWA sounds (released with nwa_free) */
    int16_t *heap_pcm;                              /* owns pcm otherwise (MEM_PCM, mem_free) */
} MixSound;

void      snd_sound_unref(MixSound *s);             /* NULL is a no-op */

/* ---- assets (snd_asset.c): every loader returns a new reference or NULL (missing/corrupt, logged) ------------ */
void      snd_assets_init(void);
void      snd_assets_shutdown(void);                /* drops the caches' references */
void      snd_assets_trim(void);                    /* once per frame: SE cache LRU trim */
MixSound *snd_load_nwa(const char *sub, const char *name);
MixSound *snd_load_se(const char *name);            /* wav/<name>, cached */
MixSound *snd_load_koe(int koe_no);                 /* koe/z%04d.ovk entry (AST §3.2) */
bool      snd_koe_exists(int koe_no);

/* #BGM table (AST §2.3): registered name -> file and loop points. Strings live until the next load/free. */
typedef struct BgmEntry { const char *reg, *file; int start, end, repeat; } BgmEntry;
void bgm_table_load(void);                          /* from Gameexe #BGM.nnn (empty when Gameexe is not loaded) */
bool bgm_table_add(const char *reg, const char *file, int start, int end, int repeat);
int  bgm_table_count(void);
/* Case-insensitive lookup (FUN_005a7620). Without a table the regname is the file name (start 0, to EOF, no
 * repeat); out then points at reg. */
bool bgm_table_find(const char *reg, BgmEntry *out);

/* ---- mixer (mixer.c) ---------------------------------------------------------------------------------------- */
bool     mixer_init(void);
void     mixer_shutdown(void);
void     mixer_lock(void);                          /* [audio thread] leaf lock; recursive */
void     mixer_unlock(void);                        /* [audio thread] */
bool     mixer_lock_held(void);                     /* [audio thread] by the calling thread */
/* s stays referenced by the caller until mixer_stop(v). Positions are clamped to the sound. */
void     mixer_start(int v, MixSound *s, uint32_t start, uint32_t end, uint32_t loop_start, bool loop, float gain);
void     mixer_stop(int v);
void     mixer_set_gain(int v, float gain);         /* ramped over the next block */
void     mixer_set_paused(int v, bool paused);
bool     mixer_active(int v);                       /* false once a one-shot voice reached its end */
uint32_t mixer_pos(int v);                          /* current frame */
bool     mixer_uses(const MixSound *s);             /* any voice still points at s */
/* [audio thread] Mixes `frames` frames of interleaved stereo into out and feeds the capture. Takes the lock per
 * block, never allocates. */
void     mixer_render(int16_t *out, int frames);

/* ---- capture (snd_capture.c) -------------------------------------------------------------------------------- */
void snd_capture_feed(const int16_t *buf, int frames);   /* [audio thread] mixer lock held */
void snd_capture_stats(AudioStats *out);                 /* since audio_capture_start */
void snd_capture_window_reset(void);                     /* selftest measurement window */
double snd_capture_window_rms(void);

/* ---- streams (snd_stream.c) --------------------------------------------------------------------------------- */
enum { CAT_BGM, CAT_KOE, CAT_PCM, CAT_SE, CAT_MOV };     /* config categories (pcmch volume_type 0..4) */
enum { SLOT_CONFIG, SLOT_SCRIPT, SLOT_PLAY, SLOT_COUNT = 8 };   /* FUN_006aeb10 multiplies 8 slots */

void  snd_streams_reset(void);                      /* every stream idle, slots 255, ducking released */
void  snd_streams_tick(int32_t dt);                 /* clock, delays, ducking, fades, gains (audio_frame) */
void  snd_config_defaults(void);                    /* AUDIO_CONFIG_DEFAULT (until sav's configuration is pushed) */
void  snd_config_set(const AudioConfig *cfg);
float snd_gain_of(int v);                           /* 0..255 -> linear gain (SYS §2.2) */

void  snd_stream_play(int i, MixSound *snd, uint32_t start, uint32_t end, uint32_t loop_start, bool loop,
                      int fade_in, int delay);      /* takes over the caller's reference */
void  snd_stream_kill(int i);
void  snd_stream_fade_out(int i, int ms, bool pause);
void  snd_stream_resume(int i, int delay);
bool  snd_stream_active(int i);
bool  snd_stream_fading(int i);
void  snd_stream_set_category(int i, int cat);
const MixSound *snd_stream_sound(int i);
int   snd_stream_pos_ms(int i);                     /* -1 if idle */

void  snd_slot_set(int i, int slot, int v, int ms); /* FUN_006ae8f0: ramp from the previous target */
int   snd_slot_get(int i, int slot);                /* the target volume */
void  snd_bgm_volume_set(int v, int ms);            /* slot 1 shared by both BGM streams */
int   snd_bgm_volume_get(void);
int   snd_bgm_volume_now(void);                     /* the ramped value now (selftest) */
int   snd_duck_level(void);                         /* 0..255 applied to BGM (selftest) */

/* ---- audio.c probes for the selftest ------------------------------------------------------------------------ */
int  snd_bgm_voice(void);                           /* voice of the current BGM stream, -1 if none */

/* ---- snd_elm.c ---------------------------------------------------------------------------------------------- */
void snd_elm_init(void);                            /* registers procs 0x1b, 0x1c, 0x1d, 0x1f, 0x20 (audio_init) */

#endif
