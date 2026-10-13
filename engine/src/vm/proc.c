/* vm/proc.c — proc stack and per-frame driver (FUN_00572bf0; TXT §1; ENGINE_SPEC §1.4.5-6, §2.6), plus the vm procs:
 * 0x14 time wait (FUN_005a0cd0) and 0x15 counter wait. The game-level procs (4, 8, 0x2b, 0x2c) are app/game.c's.
 *
 * The stack is a fixed static array of PROC_STACK_MAX entries (the game uses fewer than 8); a push beyond it is
 * refused with an error. The bottom entry is always the script proc. */
#include "vm/proc.h"
#include "vm/vm_internal.h"
#include "core/log.h"
#include "vm/clock.h"
#include "vm/sysstate.h"
#include "plat/input.h"
#include "sav/sav_stream.h"

enum { PROC_STACK_MAX = 64, PROC_FRAME_GUARD = 100000, PROC_IARGS = 4 };
enum { CHUNK_PRC1 = FOURCC('P', 'R', 'C', '1') };
CORE_STATIC_ASSERT(sizeof(((Proc *)0)->chain) == VM_CHAIN_MAX * sizeof(int), "Proc.chain must hold VM_CHAIN_MAX");
CORE_STATIC_ASSERT(sizeof(((Proc *)0)->iarg) == PROC_IARGS * sizeof(int32_t), "Proc.iarg size");

static ProcFn g_reg[PROC_TYPE_MAX];
static Proc   g_st[PROC_STACK_MAX];
static int    g_n;
static Proc   g_halt = { .type = PROC_HALT };

void proc_register(int type, ProcFn fn)
{
    if (type < 0 || type >= PROC_TYPE_MAX) { log_msg(LOG_ERR, "proc_register: bad type 0x%x", type); return; }
    g_reg[type] = fn;
}

void proc_push(const Proc *p)
{
    if (g_n >= PROC_STACK_MAX) {
        log_msg(LOG_ERR, "proc_push: proc stack full (%d), proc 0x%x dropped", PROC_STACK_MAX, p->type);
        return;
    }
    g_st[g_n++] = *p;
    if (p->type != PROC_SCRIPT) log_trace("PROC", "push 0x%x end=%d key=%d", p->type, p->end_time, p->key_skip);
}

void proc_push_wait(int type, int32_t end_time, bool key_skip, bool push_result)
{
    Proc p = { .type = type, .end_time = end_time, .key_skip = key_skip, .push_result = push_result };
    proc_push(&p);
}

void proc_pop(void)
{
    if (g_n <= 1) {                            /* the bottom script proc always stays */
        log_msg(LOG_WARN, "proc_pop: stack would be empty");
        return;
    }
    g_n--;
}

void proc_finish(int32_t ret)
{
    if (g_n <= 0) return;
    const Proc *p = &g_st[g_n - 1];
    if (p->push_result) { vm_push_int(ret); log_trace("PROC", "pop 0x%x ret=%d", p->type, ret); }
    else log_trace("PROC", "pop 0x%x", p->type);
    proc_pop();
}

Proc *proc_top(void)
{
    if (g_n > 0) return &g_st[g_n - 1];
    g_halt = (Proc){ .type = PROC_HALT };      /* a caller may have written to the previous copy */
    return &g_halt;
}

int proc_depth(void) { return g_n; }

void proc_reset(void)
{
    g_n = 0;
    Proc s = { .type = PROC_SCRIPT };
    proc_push(&s);
}

/* §1.4.6: decide release edge, consumed so that one click satisfies one wait. */
bool proc_wait_click(void)
{
    if (!input_decide_clicked()) return false;
    input_consume_decide();
    return true;
}

bool proc_wait_skip(const Proc *p) { return sys_is_skipping() && !p->no_skip; }

static bool time_up(int32_t end) { return core_wrap_sub_i32(clock_game_ms(), end) >= 0; }

/* 0x14 (54 timewait / 55 timewait_key): pushes 0 on time or skip, 1 on click (key_skip) [C 0x5a0cf2 / 0x5a0e04]. */
static bool proc_timewait(Proc *p)
{
    if (proc_wait_skip(p)) { proc_finish(0); return true; }
    if (p->key_skip && proc_wait_click()) { proc_finish(1); return true; }
    if (time_up(p->end_time)) { proc_finish(0); return true; }
    return false;
}

/* 0x15 (counter.wait / wait_key; unused by the game): iarg[0] = counter, iarg[1] = target value. */
static bool proc_counter_wait(Proc *p)
{
    const Counter *c = clock_counter(p->iarg[0]);
    if (!c || proc_wait_skip(p)) { proc_finish(0); return true; }
    if (p->key_skip && proc_wait_click()) { proc_finish(1); return true; }
    if (core_wrap_sub_i32(clock_counter_value(c), p->iarg[1]) >= 0) { proc_finish(0); return true; }
    return false;
}

void proc_init(void)
{
    proc_register(PROC_TIMEWAIT, proc_timewait);
    proc_register(PROC_COUNTER_WAIT, proc_counter_wait);
    proc_reset();
}

static void run_deferred(void)                 /* 0x12: run the stored chain as a COMMAND (FUN_00659120) */
{
    Proc c = *proc_top();
    proc_pop();
    ElmCall k = { OP_COMMAND, 0, NULL, 0, 0, 0, NULL };
    elm_dispatch(c.chain, core_clamp_i32(c.nchain, 0, VM_CHAIN_MAX), &k);
}

/* One step of the driver: false when the top proc is still waiting (the frame is over). */
static bool run_top(void)
{
    Proc *p = proc_top();
    switch (p->type) {
    case PROC_SCRIPT: {
        int d = proc_depth();
        vm_run();
        return !(proc_top()->type == PROC_SCRIPT && proc_depth() == d);   /* halted without a proc: done */
    }
    case PROC_YIELD: case PROC_YIELD2:         /* resume next frame */
        log_trace("PROC", "pop 0x%x", p->type);
        proc_pop();
        return false;
    case PROC_SEL: return false;               /* resolved by mwnd_frame() */
    case PROC_DEFER: run_deferred(); return true;
    case PROC_HALT: return false;
    default: {
        ProcFn f = (p->type >= 0 && p->type < PROC_TYPE_MAX) ? g_reg[p->type] : NULL;
        if (!f) { log_msg(LOG_WARN, "proc 0x%x unhandled", p->type); proc_pop(); return true; }
        return f(p);
    }
    }
}

/* The driver (§1.4.5): keep resolving procs until one is still waiting. */
void proc_run_frame(void)
{
    for (int guard = 0; guard < PROC_FRAME_GUARD; guard++)
        if (!run_top()) return;
    log_msg(LOG_WARN, "proc_run_frame: too many proc transitions in one frame");
}

/* Save: the proc stack as it is at the snapshot (normally only the script proc, ENGINE_SPEC §2.11). */
void proc_save(SavWriter *w)
{
    sav_put_chunk(w, CHUNK_PRC1);
    sav_put_i32(w, g_n);
    for (int i = 0; i < g_n; i++) {
        const Proc *p = &g_st[i];
        sav_put_i32(w, p->type); sav_put_i32(w, p->nchain);
        for (int k = 0; k < p->nchain; k++) sav_put_i32(w, p->chain[k]);
        sav_put_u8(w, p->key_skip); sav_put_u8(w, p->no_skip); sav_put_u8(w, p->push_result);
        sav_put_i32(w, p->end_time);
        for (int k = 0; k < PROC_IARGS; k++) sav_put_i32(w, p->iarg[k]);
    }
}

/* Per-type checks of a proc read from a save (the generic fields are range-checked by load_proc). */
static bool proc_args_valid(const Proc *p)
{
    const Pack *pk = vm_pack();
    switch (p->type) {
    case PROC_RESTART_MENU: return !p->iarg[2] || (pk && p->iarg[0] >= 0 && p->iarg[0] < pk->nscene);
    default:                return true;      /* the other procs check their arguments when they run */
    }
}

static bool load_proc(SavReader *r, Proc *p)
{
    *p = (Proc){ 0 };
    p->type = sav_get_i32(r);
    p->nchain = sav_get_i32(r);
    if (p->type < 0 || p->type >= PROC_TYPE_MAX || p->nchain < 0 || p->nchain > VM_CHAIN_MAX) return false;
    for (int k = 0; k < p->nchain; k++) p->chain[k] = sav_get_i32(r);
    p->key_skip = sav_get_u8(r); p->no_skip = sav_get_u8(r); p->push_result = sav_get_u8(r);
    p->end_time = sav_get_i32(r);
    for (int k = 0; k < PROC_IARGS; k++) p->iarg[k] = sav_get_i32(r);
    return sav_ok(r) && proc_args_valid(p);
}

bool proc_load(SavReader *r)
{
    if (!sav_find_chunk(r, CHUNK_PRC1)) return false;
    int n = sav_get_count(r, PROC_STACK_MAX, 31);
    if (!sav_ok(r) || n < 1) return false;
    g_n = 0;
    for (int i = 0; i < n; i++) {
        Proc p;
        if (!load_proc(r, &p)) { proc_reset(); return false; }
        proc_push(&p);
    }
    if (g_st[0].type != PROC_SCRIPT) { proc_reset(); return false; }
    return true;
}
