/* sav/sav.h — saves: local snapshots, slot files, global data, config, export/import (SYS §10, ENGINE_SPEC §2.11).
 * On-disk format: docs/SAVE_FORMAT.md (format 2: little-endian, chunked with CRC-32, versioned, self-describing).
 * The stream API that modules serialise with is sav/sav_stream.h. */
#ifndef SAV_SAV_H
#define SAV_SAV_H
#include "core/str.h"
#include "sav/sav_stream.h"
#include "snd/audio.h"

#ifndef SAYO_ENGINE_VERSION
#define SAYO_ENGINE_VERSION "sayooshi-engine 0.4"   /* stored in every file (META/ENGV); informative only */
#endif

enum { SAV_LANG_CAP = 16, SAV_NOTE_CAP = 160 };   /* SavInfo text fields (longer notes are cut, never overflow) */

/* Whether a slot loads here (SAVE_FORMAT.md §5). EMPTY: no slot parsed. OLD_FORMAT: a format-1 save while the other
 * language is active. CORRUPT: the call-stack record is damaged. OTHER_LANG / OTHER_DATA: a scene on the saved call
 * stack is missing or has different code, in a save made with the other language / with different game data. */
typedef enum SavSlotState {
    SAV_SLOT_EMPTY, SAV_SLOT_OK, SAV_SLOT_OLD_FORMAT, SAV_SLOT_CORRUPT, SAV_SLOT_OTHER_LANG, SAV_SLOT_OTHER_DATA
} SavSlotState;

/* One slot as the load/save dialog shows it. Every owned part is released by sav_info_free. */
typedef struct SavInfo {
    bool used;
    int64_t unix_time;  /* seconds since 1970-01-01 (UTC clock; shown as local by the UI) */
    WStr title;         /* root 74 */
    WStr message;       /* first message line */
    uint8_t *thumb_rgb; /* thumb_w x thumb_h RGB888 (decoded) or NULL; owned (sav_info_free) */
    int thumb_w, thumb_h;
    /* load rule (SAVE_FORMAT.md §5) */
    SavSlotState state;      /* SAV_SLOT_OK: loads here; the other values say why not (the UI switches on it) */
    char lang[SAV_LANG_CAP]; /* language the save was made in ("ja", "en", ...) */
    char note[SAV_NOTE_CAP]; /* log/diagnostic detail of a refusal (English), "" otherwise; never parsed */
} SavInfo;
void sav_info_free(SavInfo *si);
static inline bool sav_info_loadable(const SavInfo *si) { return si->state == SAV_SLOT_OK; }

void sav_snapshot(void);                 /* at msg_block (84) and savepoint (36): serialise local state into RAM */
void sav_selpoint(void);                 /* root 1 + after every selection decision */
int  sav_cmd_savepoint(void);            /* root 36 body (see SYS §4): returns 0 now */
bool sav_slot_write(int slot);           /* writes the latest snapshot + header + thumbnail */
bool sav_slot_load(int slot);            /* restores; script resumes at the snapshot; LOAD.WIPE */
bool sav_slot_info(int slot, struct SavInfo *out);
bool sav_return_sel(void);               /* syscom 前の選択肢に戻る */
void sav_thumb_freeze(void);             /* keep the current game picture as the save thumbnail (UI opens) */
void sav_thumb_thaw(void);               /* thumbnails follow the live picture again (UI closed) */
void sav_global_write(void); bool sav_global_read(void);
void sav_config_write(void); bool sav_config_read(void);
/* --save-dir; NULL = portable dir (a 'saves' folder or 'portable.txt' next to the executable) or else
 * SDL_GetPrefPath("sayooshi","sayooshi"). */
void sav_set_dir(const char *dir);

enum { SAV_VERSION = 2,                  /* file format major version (format 1 = the pre-CRC WP7 format) */
       SAV_VERSION_MINOR = 0,
       SAV_SLOT_CNT = 100,               /* #SAVE.CNT: slots 0..99 */
       SAV_QUICK_BASE = 100, SAV_QUICK_CNT = 3,   /* #QUICK_SAVE.CNT: slots 100..102 */
       SAV_THUMB_W = 128, SAV_THUMB_H = 72 };     /* stored thumbnail (RGB565); the UI draws it at 64x34 */

/* User configuration (config.sav): the only copy. Defaults from Gameexe #CONFIG.* (SYS §10.1, sav_config_defaults,
 * the one reader of those keys). The config dialog and the syscom elements edit sav_config() and call
 * ui_config_apply, which pushes it to the modules; other modules may read fields directly. */
enum { SAV_AUTO_MOJI_WAIT_DEFAULT = 70, SAV_AUTO_MIN_WAIT_DEFAULT = 300 };   /* ms per glyph / minimum (TXT §6) */
typedef struct SavConfig {
    AudioConfig audio;                 /* volumes 0..255 + on/off + BGMFADE_ONOFF */
    int msg_speed;                     /* ms per char, #CONFIG.MESSAGE_SPEED = 20 */
    bool msg_nowait;                   /* #CONFIG.MESSAGE_SPEED_NOWAIT.ONOFF */
    int auto_moji_wait, auto_min_wait; /* ms; 70 / 300 (TXT §6) */
    int font_shadow;                   /* #CONFIG.FONT.SHADOW = 2 (0..3) */
    bool font_futoku;                  /* #CONFIG.FONT.FUTOKU */
    bool skip_unread;                  /* #CONFIG.SKIP_UNREAD_MESSAGE.ONOFF */
    bool koe_dont_stop;                /* #CONFIG.KOE_DONT_STOP.ONOFF */
    bool wheel_next;                   /* #CONFIG.WHEEL_NEXT_MESSAGE.ONOFF */
    bool skip_wipe_anime, no_wipe_anime, no_mwnd_anime;
    bool chrcolor;    /* #CONFIG.MESSAGE_CHRCOLOR.ONOFF (read text colour) */
    int window_mode;  /* 0 window, 1 fullscreen (#CONFIG.WINDOW_MODE) */
    int window_scale; /* 1..8, windowed size multiplier (clamped on load) */
    int language;     /* last language chosen (selector / config dialog): a LangId, 0 never */
} SavConfig;
SavConfig *sav_config(void);              /* the live user configuration (never NULL) */
void sav_config_defaults(void);          /* Gameexe defaults (gx_* must be loaded) */
void sav_config_apply(void);             /* push sav_config() to audio / sysstate / platform */

const char *sav_dir(void);               /* resolved directory, with a trailing separator */
int  sav_selftest(void);                 /* stream + module round trip + file round trip; 0 = pass */

/* ---- language, session reset, load rule ------------------------------------------------------------- */
/* Forget everything tied to the loaded Scene.pck: scene-hash cache, the msg_block snapshot and the "previous
 * choice" history. Called when the game data is reloaded in another language (main.c game_reboot). */
void sav_session_reset(void);
/* Releases everything the module owns (snapshots, hash tables, preserved global chunks, the save directory).
 * Called once at exit; the module may be used again afterwards (it re-resolves the directory). */
void sav_shutdown(void);
/* 64-bit code hash of scene `scene` of the loaded pack (bytecode + label/prop/read-flag tables, no strings;
 * SAVE_FORMAT.md §5). 0 if no pack is bound or the index is out of range. */
uint64_t sav_scene_hash(int scene);
const char *sav_last_error(void);        /* human-readable reason of the last refused/failed load ("" if none) */
/* Command-line save management, handled before anything else is initialised: --print-save-dir,
 * --export-saves FILE, --import-saves FILE (each honours --save-dir). Returns -1 when argv has none of them,
 * else the process exit code. */
bool sav_export(const char *bundle_path);
bool sav_import(const char *bundle_path);

#endif
