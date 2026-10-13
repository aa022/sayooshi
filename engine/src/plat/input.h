/* plat/input.h — raw events -> per-button edge states, virtual keys, mouse in game coordinates (SYS §3.1).
 * The platform layer feeds the raw events; every module reads the edges. Consumers above plat (the editbox, the
 * system menu) get what they need through InputHooks, so this module depends on nothing above it. Main thread. */
#ifndef PLAT_INPUT_H
#define PLAT_INPUT_H
#include "core/base.h"

typedef struct InputHooks {
    void (*on_text)(const char *utf8);   /* queued text input, in order with on_key (for the focused editbox) */
    void (*on_key)(int sdl_scancode);    /* queued key presses, OS repeats included */
    void (*after_frame)(void);           /* end of input_frame (right click -> system menu, wheel -> backlog) */
    bool (*keyboard_captured)(void);     /* true mutes the keyboard members of DECIDE/CANCEL (editbox focus) */
} InputHooks;
void input_set_hooks(const InputHooks *h);     /* copied; NULL (or a NULL member) = no consumer */

enum { VK_DECIDE = 0x100, VK_CANCEL = 0x101 };
void input_raw_mouse(int win_x, int win_y);   /* platform converts window -> game coordinates first */
void input_raw_button(int which /*0 L,1 R*/, bool down);
void input_raw_key(int sdl_scancode, bool down);
void input_raw_wheel(int dy);  void input_raw_text(const char *utf8);
void input_frame(void);                        /* consume edges, then apply queued raw events (SYS §3.1 steps 1-2) */
int  input_query(int vk, int code);           /* codes 1/4/5/6/7 of SYS §3.1 -> 0/1 */
bool input_decide_clicked(void);  bool input_cancel_clicked(void);
void input_consume_decide(void);  void input_clear(void);  void input_next(void);
int  input_mouse_x(void); int input_mouse_y(void); int input_wheel(void);
bool input_ctrl_held(void);

/* ---- edge records and queries of the mouse/key elements (SYS §3) ---- */
enum { INPUT_CODE_ON_DOWN = 1, INPUT_CODE_ON_UP = 4, INPUT_CODE_CLICK = 5, INPUT_CODE_IS_DOWN = 6,
       INPUT_CODE_IS_UP = 7 };
/* Edge record of one button (SYS §3.1; IN+0x08 mouse L, +0x24 mouse R, +0x64 keys). */
typedef struct InputBtn { int is_down, on_down, on_up, down_up; } InputBtn;
int  input_query_mouse(int which /*0 L,1 R*/, int code);   /* mouse.6/.7 key state (SYS §3.3) */
int  input_query_key(int sdl_scancode, int code);          /* one keyboard key; vk < 0x100 in input_query too */
const InputBtn *input_btn_mouse(int which);                /* raw record (tests, UI) */
bool input_key_pressed(int sdl_scancode);                  /* on_down edge this frame (keyboard only) */
bool input_take_key_press(int sdl_scancode);               /* key_pressed, then used up incl. its release */
bool input_take_menu_toggle(void);                         /* right click or Escape down; used up */
void input_consume_cancel(void);                           /* mouse-R + cancel keys: down_up 2 -> 0 */
void input_consume_edges(void);                            /* clear current edges (not queued raw events) */
void input_mouse_clear(void);                              /* mouse.4: zero mouse edges (both banks) */
void input_reset(void);                                    /* full reset: states, queue, position */

#endif
