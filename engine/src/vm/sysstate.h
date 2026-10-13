/* vm/sysstate.h — skip/auto/script flags, title, read flags, read-skip (TXT §6, SYS §4/§5). Main thread only. */
#ifndef VM_SYSSTATE_H
#define VM_SYSSTATE_H
#include "core/str.h"
#include "fmt/pack.h"
#include "sav/sav_stream.h"

typedef struct {
    bool skip_active, skip_request, skip_suspended, ctrl_skip; /* DAT_00a8ff5c[0..], ff08+0xb9, ff08+0xb5 */
    bool skip_disabled;                                        /* script 64.6/7  (L+0x339) — blocks even Ctrl skip */
    bool readskip_disabled;                                    /* L+0x338 */
    bool auto_mode;                                            /* config auto (fee8+0x135) or script (L+0x33d) */
    bool syscom_locked;                                        /* syscom 6/7 (L+0x334) */
    int auto_moji_wait, auto_min_wait;                         /* ms; defaults 70 / 300 (TXT §6) */
    int msg_speed;                                             /* ms per char, #CONFIG.MESSAGE_SPEED = 20 */
    WStr title;                                                /* root 74 */
} SysState;
SysState *sys_state(void);                     /* the live system state (never NULL) */
void sys_reset_modes(void);                     /* skip (all kinds), auto mode and the syscom lock off */
void sys_set_skip(bool on);                     /* the menu's read-skip toggle: request + active (+ un-suspend) */

bool sys_is_skipping(void);                     /* TXT §6 "Skipping" */
void sys_readskip_check(int scene, int rf);     /* TXT §6 at every TEXT/koe/sel; updates skip_active */
void sys_rf_set(int scene, int rf);   /* read flags (global data) */
void sys_save(SavWriter *w);
bool sys_load(SavReader *r);
void sys_save_global(SavWriter *w);
bool sys_load_global(SavReader *r);

void sys_bind_pack(const Pack *pk);             /* sizes the read-flag store (per scene); NULL frees it */
void sys_set_script_auto(bool on);
void sys_shutdown(void);                        /* frees the read flags, other-language records and the title */

/* CG flags (SYS §6.3, global data; vm/cgflags.c, which also implements root 78 cgtable). */
enum { CGTABLE_FLAG_MAX = 1000 };
void sys_cg_mark_flag(int flag_no);             /* by MODE.CGM flag number (the gfx_set_cg_hook target) */
void sys_cg_save_global(SavWriter *w); bool sys_cg_load_global(SavReader *r);   /* load merges (OR) */

#endif
