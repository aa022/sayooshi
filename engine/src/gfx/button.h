/* gfx/button.h — object buttons, groups, proc 0x10 (ENGINE_SPEC §1.4.4). Their script elements (stage.6, obj.42)
 * are in gfx/button_elm.c. */
#ifndef GFX_BUTTON_H
#define GFX_BUTTON_H
#include "core/base.h"
#include "sav/sav_stream.h"
#include "gfx/canvas.h"
#include "gfx/gfx.h"
#include "vm/elements.h"
#include "vm/proc.h"

void btn_set(Obj *o, int no, int group, int action, int se);
void btn_frame(void);                      /* hit/push/decide for front-stage buttons */
void btn_visual(const Obj *o, int *pat_add, int *dx, int *dy, int *tr, int *bright, int *dark); /* used by render */
bool btn_rect(int group, int no, Rect *out);  /* headless policy */

/* ---- button states and object-button groups (SYS §3.2) ---- */
enum { BTN_NORMAL = 0, BTN_HIT = 1, BTN_PUSH = 2, BTN_SELECT = 3, BTN_DISABLE = 4 };   /* Obj.btn_state */
enum { BTNGROUP_CNT = 16, BTN_STAGES = 3 };                                          /* #OBJBTNGROUP.CNT */
typedef struct BtnGroup {
    bool active, wait, cancel_ok; /* +0x140 / +0x142 / +0x143 */
    int cancel_se;                /* +0x130 */
    int decided;                  /* +0x134, -2 = none */
    int result, result_no;        /* +0x138 / +0x13c (get_result / get_result_button_no) */
    int hit_no, pushed_no;        /* +0x1cc / +0x1d0, -1 = none; re-derived every frame */
} BtnGroup;
void      btn_init(void);                       /* registers proc 0x10 (idempotent) */
void      btn_reset_all(void);                  /* every group back to init state (restart menu, load) */
BtnGroup *btn_group_get(int stage, int idx);         /* NULL if out of range */
void      btn_group_init(BtnGroup *g);               /* sub 4 (FUN_005b94b0) */
void      btn_group_start(BtnGroup *g);              /* sub 2: non-blocking */
void      btn_group_end(BtnGroup *g);                /* sub 5 */
void      btn_group_sel(int stage, int idx, bool cancel_ok, int cancel_se);   /* subs 0/1: push proc 0x10 */
bool      btn_proc_sel(Proc *p);                     /* proc 0x10 handler (FUN_005a3080) */
void      btn_save(SavWriter *w); bool btn_load(SavReader *r);   /* group state (local snapshot) */

/* Test seams (ui_selftest): an object source replacing the front stage, a hit test replacing gfx_obj_hit,
 * and a sink for the "push result + pop proc 0x10" step. NULL restores the default. */
typedef Obj *(*BtnObjSource)(int i);                /* i-th root object of the front stage, NULL past the end */
typedef bool (*BtnHitFn)(const Obj *o, int x, int y);
typedef void (*BtnSelDone)(int value);
void btn_test_hooks(BtnObjSource src, BtnHitFn hit, BtnSelDone done);

#endif
