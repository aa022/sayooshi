/* ui/ui_internal.h — shared by the ui/ sources only: screen stack, bounded UI text, drawing helpers, the screens.
 *
 *  ui.c           screen stack, frame/draw dispatch, actions, input hook, config sync, init/shutdown
 *  ui_draw.c      UiText (bounded UTF-16 labels), text/rect/image drawing, hit tests, the mouse cursor
 *  ui_menu.c      syscom menu and the yes/no confirm box
 *  ui_saveload.c  save/load slot dialog
 *  ui_settings.c  config dialog
 *  ui_backlog.c   backlog viewer
 *  ui_editbox.c   editbox (root 97)
 *  ui_test.c      ui_selftest
 *
 * Memory: every screen is a fixed slot of a static stack; labels are fixed UiText buffers; the only heap data
 * are the save dialog's thumbnails (released when the page changes or the dialog closes, and by ui_shutdown).
 * Drawing never allocates. */
#ifndef UI_UI_INTERNAL_H
#define UI_UI_INTERNAL_H
#include "core/str.h"
#include "gfx/canvas.h"
#include "ui/ui.h"

/* ---- bounded UI text ---------------------------------------------------------------------------------------- */
/* UI_TEXT_MAX units hold any label the 640 px screen can show (80 half-width cells at the smallest UI size 16);
 * longer text is cut at a code point and the dialogs clip what they draw anyway. */
enum { UI_TEXT_MAX = 128 };
typedef struct UiText { uint16_t u[UI_TEXT_MAX + 1]; int n; } UiText;
void ui_text_set(UiText *t, const char *utf8);                  /* NULL -> "" */
/* Gameexe string: the field-th comma field, else field 0, else the UTF-8 fallback. */
void ui_text_gx(UiText *t, const char *key, int field, const char *fallback_utf8);

/* ---- drawing (ui_draw.c) ------------------------------------------------------------------------------------- */
int  ui_u16_w(const uint16_t *s, int n, int size);             /* pen advance: half-width = CP932 single byte */
static inline int ui_text_w(const UiText *t, int size) { return ui_u16_w(t->u, t->n, size); }
/* ---- palette of the engine-drawn screens (0xRRGGBB) ---- */
enum {
    UI_COL_BLACK = 0x000000, UI_COL_WHITE = 0xFFFFFF,
    UI_COL_TEXT = 0xFFFFFF,              /* item and caption text */
    UI_COL_TEXT_HOVER = 0xFFFF00,        /* the item under the pointer */
    UI_COL_TEXT_OFF = 0x808080,          /* a disabled item */
    UI_COL_HILITE = 0x5060A0,            /* background of the hovered / selected item */
    UI_COL_FRAME = 0xC0C0C0,             /* dialog border */
    UI_COL_BUTTON_FRAME = 0xA0A0A0,
    UI_COL_BUTTON = 0x303050,
    UI_COL_MENU = 0x101028,              /* system menu panel */
    UI_COL_DIALOG = 0x181830,            /* confirmation dialog panel */
    UI_COL_BACKDROP = 0x0C0C1C,          /* full-screen dialogs (save/load, settings) */
    UI_COL_ROW = 0x202038, UI_COL_ROW_HOVER = 0x40508C, UI_COL_THUMB_FRAME = 0x606060,
    UI_COL_TAB = 0x282840, UI_COL_BACK = 0x402828,
    UI_COL_LANG_TAG = 0xA0C0E0,          /* [EN] / [JA] on a slot of the other language */
    UI_COL_REFUSED = 0xC08080,           /* why a slot cannot be loaded */
    UI_COL_MESSAGE = 0xD0D0D0, UI_COL_NO_DATA = 0x909090,
    UI_COL_SLIDER_TRACK = 0x606080, UI_COL_SLIDER_FILL = 0xA0A0E0,
    UI_COL_TOGGLE_ON = 0x4070C0, UI_COL_TOGGLE_OFF = 0x303040, UI_COL_TOGGLE_OFF_TEXT = 0x9090A0,
    UI_COL_ROW_SELECTED = 0x303060,      /* settings row under the pointer */
    UI_COL_EDIT_FOCUS = 0x3060C0,
    UI_COL_BACKLOG_NAME = 0xF0D8A0, UI_COL_BACKLOG_SHADE = 0x404040,
};

void ui_draw_u16(Canvas *fb, int x, int y, int size, uint32_t rgb, const uint16_t *s, int n);   /* outlined */
void ui_draw_u16_face(Canvas *fb, int face, int x, int y, int size, uint32_t rgb, const uint16_t *s, int n); /* plain */
static inline void ui_draw_text(Canvas *fb, int x, int y, int size, uint32_t rgb, const UiText *t)
{
    ui_draw_u16(fb, x, y, size, rgb, t->u, t->n);
}
void ui_draw_clipped(Canvas *fb, int x, int y, int size, uint32_t rgb, const uint16_t *s, int n, int maxw);
void ui_fill(Canvas *fb, Rect r, uint32_t rgb, int tr);
void ui_frame_rect(Canvas *fb, Rect r, uint32_t rgb);
void ui_image(Canvas *fb, const char *name, int pat, int x, int y, int tr);
bool ui_in(Rect r);                                            /* mouse inside [x0,x1)×[y0,y1) */
bool ui_lclick(void);
bool ui_rclick(void);                                          /* right click or Escape */
bool ui_enter_pressed(void);                                   /* Return or keypad Enter */
int  ui_cycle(int cur, int n, int step);                       /* (cur + step) mod n for keyboard focus; n > 0 */

/* ---- screens -------------------------------------------------------------------------------------------------- */
enum { UI_SYSCOM = 1, UI_SAVE, UI_LOAD, UI_CONFIG, UI_BACKLOG, UI_CONFIRM };
enum { UIA_NONE, UIA_RETURN_MENU, UIA_GAME_END, UIA_RETURN_SEL, UIA_SAVE, UIA_LOAD, UIA_LANG };
typedef struct UiScreen {
    int kind;
    int hover;                  /* item under the mouse / keyboard cursor, -1 none */
    int page, scroll, drag;     /* dialog state */
    int action, arg;            /* confirm: what "yes" does */
    UiText msg;                 /* confirm text */
} UiScreen;

/* ui.c */
UiScreen *ui_push(int kind);                    /* NULL when the stack is full */
UiScreen *ui_top(void);
void      ui_pop(void);
void      ui_do_action(int action, int arg);
bool      ui_inited(void);
bool      ui_engine_cursor(void);
void      ui_input_hook(void);                  /* end of input_frame: right click -> syscom, wheel up -> backlog */
void      ui_input_attach(void);                /* the editbox and ui_input_hook become plat/input's consumers */
void      ui_elm_register(void);                /* ui_elm.c: claims the system roots (ui_init) */
void      ui_set_selftest_running(bool on);     /* tests: the input hook stays out of the way */
bool      ui_test_set_inited(bool inited);      /* tests: returns the previous state */

/* ui_editbox.c: input routing from input.c, drawing from ui_draw */
bool ui_editbox_has_focus(void);
void ui_editbox_on_text(const char *utf8);
void ui_editbox_on_key(int sdl_scancode);
void ui_editbox_draw_all(Canvas *fb);
void ui_editbox_mouse(void);                    /* left press inside an editbox focuses it */

/* ui_menu.c */
void ui_menu_init(void);                        /* labels and switches from Gameexe / lang.h */
void ui_menu_frame(UiScreen *s);  void ui_menu_draw(Canvas *fb, const UiScreen *s);
void ui_confirm(const UiText *msg, int action, int arg);
void ui_confirm_frame(UiScreen *s);  void ui_confirm_draw(Canvas *fb, const UiScreen *s);
bool ui_menu_hidden(void);                      /* "hide window" was chosen and no click has ended it yet */
void ui_menu_unhide(void);
int  ui_menu_count(void);                       /* tests: visible items */
Rect ui_menu_row(int k);                        /* tests: the k-th visible row */
Rect ui_confirm_button(int k);                  /* tests: 0 = yes, 1 = no */

/* ui_saveload.c */
void ui_saveload_open(UiScreen *s);
void ui_saveload_frame(UiScreen *s);
void ui_saveload_draw(Canvas *fb, const UiScreen *s);
void ui_saveload_release(void);                 /* thumbnails and slot infos */

/* ui_settings.c */
void ui_settings_open(UiScreen *s);
void ui_settings_frame(UiScreen *s);
void ui_settings_draw(Canvas *fb, const UiScreen *s);

/* ui_backlog.c */
void ui_backlog_open(UiScreen *s);
void ui_backlog_frame(UiScreen *s);
void ui_backlog_draw(Canvas *fb, const UiScreen *s);

#endif
