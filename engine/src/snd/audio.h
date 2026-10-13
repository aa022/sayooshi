/* snd/audio.h — sound: SDL output device or null sink, BGM / koe / pcmch streams, WAV capture, save/load and the
 * audio selftest (SYS §2, AST §2–3, ENGINE_SPEC §2.10, WP5).
 * Output: AUDIO_RATE Hz stereo s16. Fade/ramp times are wall-clock ms advanced by audio_frame(real_dt).
 * Volumes are 0..255. Everything here is main-thread only. */
#ifndef SND_AUDIO_H
#define SND_AUDIO_H
#include "core/str.h"
#include "sav/sav_stream.h"

enum { AUDIO_RATE = 44100, PCMCH_CNT = 16 };

/* The user's audio settings (part of sav's SavConfig, pushed by ui_config_apply). Until the first push the engine
 * defaults below apply (they equal the game's Gameexe #CONFIG values). */
typedef struct AudioConfig {
    int  vol_all, vol_bgm, vol_koe, vol_pcm, vol_se;    /* 0..255 (clamped on use) */
    bool on_all, on_bgm, on_koe, on_pcm, on_se;
    bool bgm_fade;                                      /* BGMFADE_ONOFF: duck BGM while a voice plays */
    int  bgm_fade_vol;                                  /* BGMFADE_VOLUME: the ducked BGM level 0..255 */
} AudioConfig;
#define AUDIO_CONFIG_DEFAULT { 255, 255, 255, 255, 255, true, true, true, true, true, true, 192 }

/* ---- lifetime ---- */
bool audio_init(bool real_device);                      /* false -> null sink (headless); mixing still runs */
void audio_shutdown(void);                              /* frees everything audio_init and playback allocated */
void audio_frame(int32_t real_dt);                      /* fades, ducking, BGM-end detection, null-sink mixing */
void audio_set_config(const struct AudioConfig *cfg);   /* volumes/on-off from config */
void audio_stop_all(int fade_ms);                       /* return-to-menu, game end */

/* ---- BGM (two streams, crossfaded) ---- */
void  bgm_play(const WStr *regname, bool loop, bool wait, int fade_in, int fade_out_old, int start_pos, int delay);
void  bgm_stop(int fade_ms);
void  bgm_pause(int fade_ms);
void  bgm_resume(int delay);
void  bgm_set_volume(int v, int ms);
int   bgm_get_volume(void);
bool  bgm_playing(void);
bool  bgm_fading(void);                                 /* any BGM stream in a fade-in/out */
WStr *bgm_current_name(void);                           /* registered name of the current BGM ("" if none) */
int   bgm_play_pos_ms(void);                            /* position of the current BGM in ms (-1 if none) */

/* ---- koe (voice) ---- */
bool koe_play(int koe_no, int chara, int vol /* 100 = full */);
void koe_stop(int fade_ms);
bool koe_playing(void);
void koe_set_volume(int v, int ms);
int  koe_get_volume(void);

/* ---- pcmch (16 effect channels) ---- */
typedef struct {
    bool loop, wait;
    int fade_in, volume_type, chara;
    int koe_no;
    WStr name;
    /* alternative sources, used only when name is empty (FUN_006660d0 order: name, bgm regname, koe_no >= 0,
     * se_no >= 0). Zero-init leaves them inert. */
    WStr bgm_name;
    int se_no;
    bool has_koe;
} PcmchArgs;
void pcmch_play(int ch, const PcmchArgs *a);            /* ch outside 0..PCMCH_CNT-1: no-op */
void pcmch_stop(int ch, int fade_ms);
void pcmch_pause(int ch, int fade_ms);
void pcmch_resume(int ch, int delay);
void pcmch_set_volume(int ch, int v, int ms);
int  pcmch_get_volume(int ch);
bool pcmch_playing(int ch);
bool pcmch_fading(int ch);

/* ---- save / load: BGM name/loop + slot-1 volumes, looping pcmch channels only ---- */
void audio_save(SavWriter *w);
bool audio_load(SavReader *r);

/* ---- capture of the mixed output to a 16-bit stereo WAV (--audio-capture). In null-sink mode audio_frame mixes
 * exactly real_dt ms per frame, so the capture is deterministic with --fixed-dt. ---- */
bool audio_capture_start(const char *wav_path);
void audio_capture_stop(void);
typedef struct { uint64_t frames; int peak; double rms; } AudioStats;   /* peak 0..32767, rms 0..1 (full scale) */

/* Self-test (M0 + WP5 checks): decode every NWA and OVK entry, check sample counts, loop/fade/duck rules, and
 * render a short bgm+voice mix to wav_out. koe_ref: optional file with one koe number per line (last
 * tab-separated column) that must all resolve. Returns 0 on success. */
int audio_selftest_ex(const char *gamedata, const char *wav_out, const char *koe_ref);

#endif
