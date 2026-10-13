/* vm/proc.h — proc stack + frame driver (TXT §1; ENGINE_SPEC §1.4.5/6, §2.6). Cross-module API
 * (docs/CODE_STANDARD.md §12). The stack has a fixed capacity; the bottom entry is always PROC_SCRIPT. */
#ifndef VM_PROC_H
#define VM_PROC_H
#include "core/base.h"
#include "sav/sav_stream.h"

enum { PROC_HALT=0, PROC_SCRIPT=1, PROC_YIELD=2, PROC_RETMENU_A=4, PROC_RESTART_MENU=8, PROC_SEL=0x0e,
       PROC_BTNSEL=0x10, PROC_DEFER=0x12, PROC_TIMEWAIT=0x14, PROC_COUNTER_WAIT=0x15, PROC_MWND_OPEN=0x16,
       PROC_MWND_CLOSE=0x17, PROC_MSG_WAIT=0x19, PROC_KEY_WAIT=0x1a, PROC_BGM_WAIT=0x1b, PROC_KOE_WAIT=0x1d,
       PROC_WIPE_WAIT=0x23, PROC_SHAKE_WAIT=0x24, PROC_EVENT_WAIT=0x26, PROC_END_SAVE=0x2b, PROC_GAME_END=0x2c,
       PROC_YIELD2=0x2e };
enum { PROC_TYPE_MAX = 0x40 };   /* handler registry size: every proc type is < PROC_TYPE_MAX */
typedef struct Proc { int type; int chain[32]; int nchain; uint8_t key_skip, no_skip, push_result;
                      int32_t end_time; int32_t iarg[4]; } Proc;
typedef bool (*ProcFn)(Proc *p);             /* return true iff the proc finished AND popped itself */

void  proc_register(int type, ProcFn fn);    /* each module registers its proc types in its init */
void  proc_push(const Proc *p);              /* becomes the top (refused with an error when the stack is full) */
/* Pushes a proc with only these fields set (no chain, iargs 0): waits, yields, game end. end_time in game ms. */
void  proc_push_wait(int type, int32_t end_time, bool key_skip, bool push_result);
void  proc_pop(void);                        /* never pops the bottom script proc */
void  proc_finish(int32_t ret);              /* pop the top proc; push ret first when it asked for a result */
Proc *proc_top(void);                        /* never NULL (a HALT proc when empty) */
int   proc_depth(void);
void  proc_run_frame(void);                  /* the driver, ENGINE_SPEC §2.6 */
bool  proc_wait_click(void);                      /* §1.4.6 click test + consume (calls input.h) */
bool  proc_wait_skip(const Proc *p);              /* sys_is_skipping() && !p->no_skip */
void  proc_save(SavWriter *w); bool proc_load(SavReader *r);

void  proc_init(void);                       /* registers the vm procs (0x14/0x15) and resets the stack */
void  proc_reset(void);                      /* stack = [PROC_SCRIPT] */

#endif
