/* txt/lang.h — game language (ja / en): engine-drawn UI strings, speaker display names, language choice and runtime
 * switch requests (docs/SPEC_ENPATCH.md §1.4, §6; docs/PAYLOAD.md §2).
 *
 * The data language itself is the fs overlay (fs_set_language / fs_language): "ja" = the original data, "en" = the
 * English patch files overlaid on it. This module only reads it. Engine code passes languages as LangId; the
 * codes "ja"/"en" appear only at the fs path layer, in save file fields and in logs. */
#ifndef TXT_LANG_H
#define TXT_LANG_H
#include "core/str.h"

/* The values are those of config.sav "language" (SAVE_FORMAT.md §8.3): 0 = never chosen. */
typedef enum LangId { LANG_NONE = 0, LANG_JA = 1, LANG_EN = 2 } LangId;

LangId      lang_id(void);                          /* the active language: LANG_JA or LANG_EN */
const char *lang_code(void);                        /* its code, "ja" or "en" (static strings) */
bool        lang_is_en(void);
void        lang_test_override(LangId id);          /* unit tests: pretend this language is active (LANG_NONE = off) */
/* English text layout of the patched exe (SPEC_ENPATCH §1.3.2): word wrap at runs of 0x21..0x7e, no hanging
 * punctuation. True while the English data is active. */
bool        lang_latin_layout(void);
/* The language of user input or a stored code ("EN", "english", "jp", "日本語" ...); LANG_NONE if unknown. */
LangId      lang_parse(const char *s);
const char *lang_code_of(LangId id);                /* "ja" / "en" (static); NULL for LANG_NONE */
LangId      lang_from_config(int value);            /* config.sav "language" -> LangId (LANG_NONE for 0 / unknown) */
const char *lang_native_name(LangId id);            /* "日本語" / "English" (UTF-8), each in its own script */
bool        lang_available(LangId id);              /* the mounted data has <code>/Scene.pck (fs_has_language) */
int         lang_available_count(void);
/* Preselection when nothing was chosen before: the OS UI language if it is available (ja -> 日本語, anything
 * else -> English when the EN data exists), otherwise LANG_JA. */
LangId      lang_default(void);

/* Strings the engine draws itself (never taken from Gameexe, or used as fallbacks when a key is missing). */
typedef enum {
    LANG_STR_YES, LANG_STR_NO, LANG_STR_BACK, LANG_STR_NO_DATA, LANG_STR_SETTINGS, LANG_STR_SAVE, LANG_STR_LOAD,
    LANG_STR_SAVE_WARN, LANG_STR_LOAD_WARN, LANG_STR_MENU_READ_SKIP, LANG_STR_MENU_UNREAD_SKIP, LANG_STR_MENU_AUTO_SKIP,
    LANG_STR_MENU_AUTO_MODE, LANG_STR_MENU_HIDE_MWND, LANG_STR_MENU_RETURN_SEL, LANG_STR_MENU_RETURN_MENU,
    LANG_STR_MENU_GAME_END, LANG_STR_MENU_CANCEL, LANG_STR_WARN_RETURN_MENU, LANG_STR_WARN_RETURN_SEL,
    LANG_STR_WARN_GAME_END, LANG_STR_SLOT_OTHER_LANG, LANG_STR_SLOT_OTHER_DATA, LANG_STR_SLOT_OLD_FORMAT,
    LANG_STR_SLOT_DAMAGED, LANG_STR_SLOT_LANG_TAG_JA, LANG_STR_SLOT_LANG_TAG_EN, LANG_STR_CF_VOL_ALL,
    LANG_STR_CF_VOL_BGM, LANG_STR_CF_VOL_KOE, LANG_STR_CF_VOL_PCM, LANG_STR_CF_VOL_SE, LANG_STR_CF_MSG_SPEED,
    LANG_STR_CF_AUTO_SPEED, LANG_STR_CF_SKIP_UNREAD, LANG_STR_CF_KOE_DONT_STOP, LANG_STR_CF_WHEEL, LANG_STR_CF_SHADOW,
    LANG_STR_CF_WINDOW, LANG_STR_CF_LANGUAGE, LANG_STR_SHADOW_0, LANG_STR_SHADOW_1, LANG_STR_SHADOW_2,
    LANG_STR_SHADOW_3, LANG_STR_ON, LANG_STR_OFF, LANG_STR_LANG_SWITCH_TO_JA, LANG_STR_LANG_SWITCH_TO_EN, LANG_STR_COUNT
} LangStrId;
const char *lang_str(LangStrId id);                     /* UTF-8, in the current language */

/* Speaker names for the backlog. The English Scene.pck keeps the Japanese NAME strings (MWND.002 hides the name,
 * NAME_DISP_MODE 2); in English the backlog shows them translated. out = in for Japanese or unknown names. Written
 * into a caller buffer without allocating (drawing code); a name longer than the buffer is cut. */
void lang_display_name_buf(const WStr *in, WBuf *out);

/* Global names across languages (SPEC_ENPATCH §6.4 rule 3): the EN _start converts 人見/広介 to Hitomi/Hirosuke at
 * every boot; this is the reverse for Japanese. Call after global data is loaded while "ja" is active. */
void lang_migrate_global_names(void);

/* Runtime switch (config dialog): main reboots the game data in the new language at the next frame. */
void        lang_request_switch(LangId id);        /* ignored when the data lacks that language */
LangId      lang_pending_switch(void);              /* LANG_NONE if none */
void        lang_clear_switch(void);

#endif
