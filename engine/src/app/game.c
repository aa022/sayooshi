/* app/game.c — game-level resets and procs (see game.h). These sequences touch gfx, txt, snd, ui and vm at once, so
 * they live in app, the only module that may call all of them. */
#include "app/game.h"
#include "app/harness.h"
#include "core/log.h"
#include "gfx/button.h"
#include "gfx/gfx.h"
#include "plat/platform.h"
#include "snd/audio.h"
#include "txt/mwnd.h"
#include "ui/ui.h"
#include "vm/proc.h"
#include "vm/sysstate.h"
#include "vm/vm.h"

void app_game_reset(AppReset kind)
{
    switch (kind) {
    case APP_RESET_RESTART:
        gfx_reset_all();
        mwnd_reset_all();
        btn_reset_all();
        ui_close_all();
        break;
    case APP_RESET_REBOOT:
        audio_stop_all(0);
        ui_close_all();
        for (int i = 0; i < UI_EDITBOX_CNT; i++) ui_editbox_destroy(i);
        btn_reset_all();
        sys_reset_modes();
        break;
    default:
        ASSERT_MSG(false, "unknown game reset %d", (int)kind);
    }
}

/* 4: return-to-menu A (FUN_005a0690): stop audio with the return fade, then wait for the return wipe [I]. */
static bool proc_retmenu_a(Proc *p)
{
    if (!p->iarg[0]) {
        p->iarg[0] = 1;
        audio_stop_all(APP_RETURN_FADE_MS);
    }
    if (gfx_wipe_active() && !proc_wait_skip(p)) return false;
    proc_finish(0);
    return true;
}

/* 8: restart (FUN_0059c6c0): reset stages and windows, clear the proc stack, restart #MENU_SCENE z0 (or the scene
 * given by returnmenu(scene[, z]): iarg[2] = 1, iarg[0] = scene, iarg[1] = z). The proc pops itself first, so the
 * driver always makes progress, and a scene the pack does not have restarts the menu instead (a restart that failed
 * used to leave proc 8 on top, which then ran PROC_FRAME_GUARD times per frame). */
static bool proc_restart_menu(Proc *p)
{
    int scene = p->iarg[0], z = p->iarg[1], custom = p->iarg[2];
    log_trace("PROC", "pop 0x8 restart");
    proc_pop();
    app_game_reset(APP_RESET_RESTART);
    if (!custom || !vm_restart_at(scene, z)) vm_restart_menu();   /* both reset the proc stack */
    return true;
}

/* 0x2b / 0x2c (§2.6): end-save (END_SAVE.CNT = 0, nothing to save) and game end. */
static bool proc_end_save(Proc *p) { (void)p; proc_pop(); return true; }

static bool proc_game_end(Proc *p)
{
    (void)p;
    audio_stop_all(APP_RETURN_FADE_MS);
    harness_request_quit();
    plat_request_quit();
    return false;   /* stays on top; the main loop exits */
}

void app_game_register_procs(void)
{
    proc_register(PROC_RETMENU_A, proc_retmenu_a);
    proc_register(PROC_RESTART_MENU, proc_restart_menu);
    proc_register(PROC_END_SAVE, proc_end_save);
    proc_register(PROC_GAME_END, proc_game_end);
}
