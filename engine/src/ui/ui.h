/* ui/ui.h — engine-drawn system UI: syscom menu, save/load/config dialogs, backlog viewer, editbox, language
 * selector, cursor (ENGINE_SPEC §2.12, WP6). Module-internal declarations are in ui/ui_internal.h. */
#ifndef UI_UI_H
#define UI_UI_H
#include "core/str.h"
#include "sav/sav_stream.h"
#include "gfx/canvas.h"
#include "txt/lang.h"

bool ui_active(void);           /* a modal system UI is open -> the script is paused */
void ui_frame(void);  void ui_draw(Canvas *fb);
void ui_open_syscom(void); void ui_open_save(void); void ui_open_load(void); void ui_open_config(void);
void ui_cursor_draw(Canvas *fb);   /* _mouse_00, 8 cuts at 160 ms, or the OS cursor */

/* editbox (root 97, ui_editbox.c) with SDL text input; input.c forwards text and key events. */
enum { UI_EDITBOX_CNT = 4 };    /* #EDITBOX.CNT: ids 0..3 */
void ui_editbox_create(int id, Rect r, int moji_size);
void ui_editbox_destroy(int id);
void ui_editbox_set_text(int id, const WStr *s);
bool ui_editbox_get_text(int id, WStr *out);   /* false (out = "") for an unused id */
bool ui_editbox_decided(int id);
bool ui_editbox_canceled(int id);
void ui_editbox_set_focus(int id);
void ui_editbox_clear_input(int id);

/* Boot-time language selector (langsel.c): a screen drawn with the compositor before the game data is loaded,
 * offering 日本語 and English (each in its own face) with `preselect` highlighted. Mouse, arrow keys + Enter, and
 * gamepad (D-pad + A, via platform.c) work; scripted input (--input) drives it in headless runs. The application
 * runs it in its frame loop: ui_langsel_begin returns the language when there is nothing to choose, else LANG_NONE;
 * then ui_langsel_frame (after the frame's input) draws one frame into fb until it returns false (a choice faded
 * out; after max_frames > 0 frames the highlighted language is chosen, so a headless run never blocks), and
 * ui_langsel_finish returns the choice (quit: the highlighted language). */
LangId ui_langsel_begin(LangId preselect);
bool   ui_langsel_frame(Canvas *fb, int dt, int max_frames);
LangId ui_langsel_finish(bool quit);

/* ---- lifetime, selftest, screens -------------------------------------------------------------------- */
void ui_init(void);             /* boot (§2.13 step 2, after gx_load): strings, config defaults, proc 0x10 */
/* Releases what the ui module holds (open screens, the save dialog's thumbnails, editboxes). Call once at exit,
 * after the last frame; ui_init starts over. */
void ui_shutdown(void);
int  ui_selftest(void);         /* edge-state table + objbtngroup flow + editbox; returns the failure count */
void ui_close_all(void);        /* drop every open screen (load, restart menu) */
void ui_set_engine_cursor(bool on);   /* false: the platform shows the OS cursor instead (default true) */

/* The user configuration is sav's SavConfig (config.sav; sav_config()), the single copy: the config dialog and the
 * syscom elements edit it and then call ui_config_apply, which pushes it to audio, sys_state(), the message windows
 * and the window mode. */
void ui_config_apply(void);

#endif
