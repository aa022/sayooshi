/* txt/mwnd.h — message windows, typing, key icons, in-window selection (TXT §3–§7, WP4), and the txt module's
 * lifetime (txt_shutdown). Module-internal declarations are in txt/txt_internal.h. */
#ifndef TXT_MWND_H
#define TXT_MWND_H
#include "core/str.h"
#include "sav/sav_stream.h"
#include "gfx/canvas.h"
#include "vm/elements.h"

/* ---- binding API (ENGINE_SPEC §2.4.4) ---- */
void mwnd_init(void);                               /* after gx_load: config, colours, icons, procs; windows reset */
/* Typing, open/close/icon animation, in-window choices (proc 0x0e). screen_open: a system screen owns the input, so
 * the choice ignores it this frame. */
void mwnd_frame(int32_t game_dt, int32_t real_dt, bool screen_open);
void mwnd_draw(Canvas *fb);                         /* all open windows (filter, glyph layers, key icon, choices) */
bool mwnd_any_printing(void);
bool mwnd_sel_item_rect(int k, Rect *out);          /* for the headless choice policy */
void mwnd_set_user_hidden(bool hidden);             /* syscom "hide window" */
void mwnd_reset_all(void);
/* Per window: open, waku, page replay log (§2.11), koe link, sizes. */
void mwnd_save(SavWriter *w); bool mwnd_load(SavReader *r);

/* ---- selftest, stage message windows, configuration ---- */
int  txt_selftest(void);                            /* unit tests (layout, names, typing, font, stores); 0 = pass */
/* Stage-mwnd chains `S.3.-1.n.<sub>` (S = 37/38/73): WP3 forwards rest = [n, sub, ...] here (VM §9, TXT §8). */
ElmResult txt_elm_mwnd(int stage, const int *rest, int nrest, ElmCall *c);
int  mwnd_sel_count(void);   /* items of the active in-window selection, 0 if none (legacy WP4 test driver only) */
void mwnd_set_font_style(int shadow_mode, bool futoku);    /* config dialog: FONT.SHADOW 0..3, FUTOKU */
/* Config switches NO_MWND_ANIME, KOE_DONT_STOP, WHEEL_NEXT_MESSAGE. */
void mwnd_set_options(bool no_anime, bool koe_dont_stop, bool wheel_next, bool speed_nowait);

/* Releases everything the txt module holds (window strings, backlog, glyph cache, font files). Call once at exit,
 * after the last frame; txt functions must not be used afterwards except font_init / mwnd_init, which start over. */
void txt_shutdown(void);

#endif
