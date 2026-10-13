/* plat/platform.h — SDL window/renderer/texture, event pump, scaling, vsync (ENGINE_SPEC §2.1 #2, §2.7).
 * Main thread only. Events are translated into plat/input calls; headless mode opens no window. */
#ifndef PLAT_PLATFORM_H
#define PLAT_PLATFORM_H
#include "core/base.h"

/* The window shows a w x h frame (the game's screen), letterboxed, at `scale` times its size (1..8). */
bool plat_init(bool headless, int w, int h, int scale);
bool plat_pump(void);                 /* false = quit requested */
/* One w x h frame of 0xAARRGGBB pixels, rows pitch_px pixels apart (pitch_px >= w). No-op without a window. */
void plat_present(const uint32_t *argb, int pitch_px);
uint32_t plat_ticks_ms(void);
void plat_shutdown(void);

bool plat_headless(void);
void plat_set_fullscreen(bool on);    /* Alt+Enter / F11 toggle it too (§4 M5) */
void plat_set_game_cursor(bool on);   /* hide the OS cursor over the frame (the game draws one) */
void plat_set_linear(bool linear);    /* scaling filter: linear (default) or nearest */
void plat_request_quit(void);         /* plat_pump() returns false from now on */
void plat_set_title(const char *utf8);/* window title (#GAMENAME of the active language) */

#endif
