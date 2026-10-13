/* app/game.h — game-level orchestration across modules: the resets that span several modules (restart at the menu,
 * reload in another language) and the procs that drive them (ENGINE_SPEC §2.6: 4 return-to-menu A, 8 restart,
 * 0x2b end-save, 0x2c game end). Main thread. */
#ifndef APP_GAME_H
#define APP_GAME_H
#include "core/base.h"

enum { APP_RETURN_FADE_MS = 100 };   /* audio fade of return-to-menu and game end (FUN_005a0690) */

typedef enum AppReset {
    APP_RESET_RESTART,               /* proc 8 (FUN_0059c6c0): stages, windows, buttons and screens */
    APP_RESET_REBOOT,                /* before reloading the game data in another language: audio, screens, editboxes,
                                        buttons, skip/auto modes (the module inits reset the rest) */
} AppReset;
void app_game_reset(AppReset kind);
void app_game_register_procs(void);  /* procs 4, 8, 0x2b, 0x2c (after vm_init) */

#endif
