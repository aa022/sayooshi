/* vm/elm_flow.c — flow roots: 0-6, 36 savepoint, 39 math, 40 counter, 54/55 waits, 64 script, 74/75 title, 110-113,
 * 129, 149/150; everything else of the flow group (banks, 4, 5, 83, 108, 131, 138-141, 158) goes to vm_elm.c
 * (ENGINE_SPEC §1.1, §2.2; TXT §3.11-3.17; SYS §4, §6.1).
 *
 * Script integers wrap like the original's 32-bit arithmetic; float-to-int conversions saturate like MSVC's
 * cvttsd2si (out of range -> INT32_MIN) instead of being undefined. */
#include "vm/elements.h"
#include "vm/vm_internal.h"
#include "core/log.h"
#include "vm/clock.h"
#include "vm/proc.h"
#include "vm/sysstate.h"
#include "sav/sav.h"
#include <math.h>

enum { NUM_TEXT_MAX = 64, NUM_WIDTH_MAX = NUM_TEXT_MAX - 1 };

static void push_proc(int type) { proc_push_wait(type, 0, false, false); }

static void push_wait(int type, int32_t ms, bool key)   /* 55 returns via the proc; 54 returns nothing */
{
    proc_push_wait(type, core_wrap_add_i32(clock_game_ms(), ms), key, key);
}

/* ---- 39 math (FUN_00662200) ----------------------------------------------------------------------------------- */

static uint32_t g_rand = 1;                    /* MSVC rand() LCG, deterministic */
static int msvc_rand(void) { g_rand = g_rand * 214013u + 2531011u; return (int)((g_rand >> 16) & 0x7FFF); }

static int32_t trunc_i32(double v)             /* cvttsd2si: NaN / out of range -> INT32_MIN */
{
    return (v > -2147483649.0 && v < 2147483648.0) ? (int32_t)v : INT32_MIN;
}


/* tostr variants: decimal n right-aligned in |width| (left-aligned when negative), padded with pad; zen converts to
 * full-width digits, minus and space. The width is clamped to the 63 characters the original buffer held. */
static void push_num(int32_t n, int width, char pad, bool zen)
{
    char buf[NUM_TEXT_MAX];
    int w = core_clamp_i32(width, -NUM_WIDTH_MAX, NUM_WIDTH_MAX);
    str_fmt_cut(buf, sizeof buf, pad == '0' ? "%0*d" : "%*d", w, n);   /* <= 63 + sign + digits: fits */
    WStr *d = vm_str_push_slot();
    if (!d) return;
    if (!zen) { ws_set_utf8(d, buf); return; }
    ws_set(d, NULL, 0);
    for (const char *p = buf; *p; p++) {
        uint16_t ch = (uint16_t)*p;
        if (ch >= '0' && ch <= '9') ch = (uint16_t)(0xFF10 + ch - '0');
        else if (ch == '-') ch = 0xFF0D;
        else if (ch == ' ') ch = 0x3000;
        ws_push(d, ch);
    }
}

static int32_t math_rand(int32_t lo, int32_t hi)   /* rand(a, b), inclusive, either order */
{
    if (lo > hi) { int32_t t = lo; lo = hi; hi = t; }
    uint32_t span = (uint32_t)hi - (uint32_t)lo + 1u, r = (uint32_t)msvc_rand();   /* span 0 = all of int32 */
    return core_wrap_add_i32(lo, (int32_t)(span ? r % span : r));
}

static int32_t math_trig(int fn, int32_t r, int32_t angle)   /* sin/cos/tan(r, angle in 1/10 deg) [I] */
{
    double a = angle * 3.14159265358979323846 / 1800.0;
    double f = fn == 6 ? sin(a) : fn == 7 ? cos(a) : tan(a);
    return trunc_i32(r * f);
}

static int32_t math_linear(int32_t x, int32_t x1, int32_t y1, int32_t x2, int32_t y2)   /* [I] */
{
    if (x2 == x1) return y1;
    int64_t q = (int64_t)core_wrap_sub_i32(x, x1) * core_wrap_sub_i32(y2, y1) / core_wrap_sub_i32(x2, x1);
    return core_wrap_add_i32(y1, (int32_t)(uint32_t)q);
}

static ElmResult elm_math(const int *rest, int nrest, ElmCall *c)
{
    if (nrest < 1 || c->op != OP_COMMAND) return ELM_UNKNOWN;
    int32_t a0 = elm_arg_int(c, 0, 0), a1 = elm_arg_int(c, 1, 0), a2 = elm_arg_int(c, 2, 0);
    switch (rest[0]) {
    case ELM_MATH_RAND: vm_push_int(math_rand(a0, a1)); return ELM_OK;
    case ELM_MATH_TOSTR: push_num(a0, c->al_id == 1 ? a1 : 0, ' ', false); return ELM_OK; /* tostr(n[, width]) */
    case ELM_MATH_MAX: vm_push_int(a0 > a1 ? a0 : a1); return ELM_OK;
    case ELM_MATH_MIN: vm_push_int(a0 < a1 ? a0 : a1); return ELM_OK;
    case ELM_MATH_ABS: vm_push_int(a0 < 0 ? core_wrap_sub_i32(0, a0) : a0); return ELM_OK;
    case ELM_MATH_SIN:
    case ELM_MATH_COS:
    case ELM_MATH_TAN: vm_push_int(math_trig(rest[0], a0, a1)); return ELM_OK;
    case ELM_MATH_LINEAR:
        vm_push_int(math_linear(a0, a1, a2, elm_arg_int(c, 3, 0), elm_arg_int(c, 4, 0)));
        return ELM_OK;
    case ELM_MATH_LIMIT: vm_push_int(a1 < a0 ? a0 : a1 > a2 ? a2 : a1); return ELM_OK; /* limit(min, v, max) */
    case ELM_MATH_TOSTR_ZERO: push_num(a0, a1, '0', false); return ELM_OK;             /* tostr_zero(n, width) */
    case ELM_MATH_TOSTR_ZEN: push_num(a0, c->al_id == 1 ? a1 : 0, ' ', true); return ELM_OK;
    case ELM_MATH_TOSTR_ZEN_ZERO: push_num(a0, a1, '0', true); return ELM_OK;
    case ELM_MATH_DISTANCE: { /* distance(x1, y1, x2, y2) */
        double dx = (double)elm_arg_int(c, 2, 0) - a0, dy = (double)elm_arg_int(c, 3, 0) - a1;
        vm_push_int(trunc_i32(sqrt(dx * dx + dy * dy)));
        return ELM_OK;
    }
    default: return ELM_UNKNOWN;               /* 2, 14, 16-23: unused (SYS §6.1) */
    }
}

/* ---- 40 counter[16] (FUN_006631d0 / FUN_00663310, ENGINE_SPEC §1.4.3) ------------------------------------------ */

static void counter_wait(int idx, int32_t value, bool key)   /* wait / wait_key -> proc 0x15 */
{
    Proc p = { .type = PROC_COUNTER_WAIT };
    p.iarg[0] = idx;
    p.iarg[1] = value;
    p.key_skip = key;
    p.push_result = key;
    proc_push(&p);
}

static void counter_start_frame(Counter *k, int sub, const ElmCall *c) /* start_frame[_real][_loop](start, end, time) */
{
    k->running = true;
    k->real = sub == ELM_COUNTER_START_FRAME_REAL || sub == ELM_COUNTER_START_FRAME_LOOP_REAL;
    k->frame_mode = true;
    k->frame_loop = sub == ELM_COUNTER_START_FRAME_LOOP || sub == ELM_COUNTER_START_FRAME_LOOP_REAL;
    k->frame_start = elm_arg_int(c, 0, 0);
    k->frame_end = elm_arg_int(c, 1, 0);
    k->frame_time = elm_arg_int(c, 2, 0);
    k->value = 0;
}

static ElmResult elm_counter(const int *rest, int nrest, ElmCall *c)
{
    if (nrest == 1 && rest[0] == 1) { vm_push_int(COUNTER_CNT); return ELM_OK; }   /* get_size */
    if (nrest < 3 || rest[0] != -1) return ELM_UNKNOWN;
    Counter *k = clock_counter(rest[1]);
    if (!k) { vm_err("counter index out of range [%d]", rest[1]); elm_push_default(c); return ELM_OK; }
    int32_t a0 = elm_arg_int(c, 0, 0);
    int sub = rest[2];
    switch (sub) {
    case ELM_COUNTER_SET: k->value = a0; break;
    case ELM_COUNTER_GET: vm_push_int(clock_counter_value(k)); break;
    case ELM_COUNTER_RESET: *k = (Counter){0}; break;
    case ELM_COUNTER_START:
    case ELM_COUNTER_START_REAL:
        k->running = true; k->real = sub == ELM_COUNTER_START_REAL; k->frame_mode = false; k->value = 0;
        break;
    case ELM_COUNTER_STOP: k->running = false; break;
    case ELM_COUNTER_RESUME: k->running = true; break;
    case ELM_COUNTER_WAIT:
    case ELM_COUNTER_WAIT_KEY: counter_wait(rest[1], a0, sub == ELM_COUNTER_WAIT_KEY); break;
    case ELM_COUNTER_CHECK_VALUE:                                          /* setns (asm 0x663544) */
        vm_push_int(core_wrap_sub_i32(clock_counter_value(k), a0) >= 0);
        break;
    case ELM_COUNTER_START_FRAME:
    case ELM_COUNTER_START_FRAME_REAL:
    case ELM_COUNTER_START_FRAME_LOOP:
    case ELM_COUNTER_START_FRAME_LOOP_REAL: counter_start_frame(k, sub, c); break;
    case ELM_COUNTER_CHECK_ACTIVE: vm_push_int(k->running); break;
    default: return ELM_UNKNOWN;
    }
    return ELM_OK;
}

/* ---- 64 script (FUN_00664c70): flags in the local state block -------------------------------------------------- */

static ElmResult elm_script(const int *rest, int nrest, ElmCall *c)
{
    if (nrest < 1) return ELM_UNKNOWN;
    SysState *ss = sys_state();
    switch (rest[0]) {
    case ELM_SCRIPT_START_AUTO_MODE: sys_set_script_auto(true); break;
    case ELM_SCRIPT_END_AUTO_MODE: sys_set_script_auto(false); break;
    case ELM_SCRIPT_SET_SKIP_DISABLE: ss->readskip_disabled = true; break; /* L+0x338 */
    case ELM_SCRIPT_SET_SKIP_ENABLE: ss->readskip_disabled = false; break;
    case ELM_SCRIPT_SET_CTRL_SKIP_DISABLE: ss->skip_disabled = true; break; /* L+0x339 */
    case ELM_SCRIPT_SET_CTRL_SKIP_ENABLE: ss->skip_disabled = false; break;
    case ELM_SCRIPT_CHECK_SKIP: vm_push_int(sys_is_skipping()); break;
    default: {                                                              /* unused flag setters/getters */
        static bool once;
        if (!once) log_msg(LOG_DEBUG, "script.%d: no behaviour (unused)", rest[0]);
        once = true;
        elm_push_default(c);
        break;
    }
    }
    return ELM_OK;
}

/* ---- the roots ------------------------------------------------------------------------------------------------ */

static void returnmenu(const ElmCall *c)       /* TXT §3.15; returnmenu(scene[, z]): FUN_0059df60 */
{
    sav_global_write();
    Proc p8 = { .type = PROC_RESTART_MENU };
    if (c->al_id >= 1) {
        int idx = vm_find_scene(elm_arg_str(c, 0));
        if (idx >= 0) { p8.iarg[0] = idx; p8.iarg[1] = c->al_id == 2 ? elm_arg_int(c, 1, 0) : 0; p8.iarg[2] = 1; }
    }
    proc_push(&p8);                            /* run order: 2 -> 4 -> 8 */
    push_proc(PROC_RETMENU_A);
    push_proc(PROC_YIELD);
}

static void log_once_129(void)
{
    static bool once;
    if (!once) log_msg(LOG_DEBUG, "global 129 (__iapp_dummy): invalid command, ignored");
    once = true;
}

static ElmResult flow_global(int root, const int *rest, int nrest, ElmCall *c)
{
    switch (root) {
    case ELM_ROOT_MESSAGE_BOX: return ELM_UNKNOWN;         /* debug only: whitelisted stub */
    case ELM_ROOT_SELPOINT: sav_selpoint(); return ELM_OK; /* TXT §3.17 */
    case ELM_ROOT_OWARI:
        push_proc(PROC_YIELD);
        push_proc(PROC_GAME_END);
        return ELM_OK; /* proc 2, 0x2c (TXT §3.16) */
    case ELM_ROOT_RETURNMENU: returnmenu(c); return ELM_OK;
    case ELM_ROOT_DISP: push_proc(PROC_YIELD); return ELM_OK;                 /* TXT §3.11 */
    case ELM_ROOT_SAVEPOINT: vm_push_int(sav_cmd_savepoint()); return ELM_OK; /* SYS §4 */
    case ELM_ROOT_MATH: return elm_math(rest, nrest, c);
    case ELM_ROOT_COUNTER: return elm_counter(rest, nrest, c);
    case ELM_ROOT_TIMEWAIT: push_wait(PROC_TIMEWAIT, elm_arg_int(c, 0, 0), false); return ELM_OK;    /* (ms) */
    case ELM_ROOT_TIMEWAIT_KEY: push_wait(PROC_TIMEWAIT, elm_arg_int(c, 0, 0), true); return ELM_OK; /* -> 0/1 */
    case ELM_ROOT_SCRIPT: return elm_script(rest, nrest, c);
    case ELM_ROOT_SET_TITLE: ws_copy(&sys_state()->title, elm_arg_str(c, 0)); return ELM_OK; /* TXT §3.13 */
    case ELM_ROOT_GET_TITLE: vm_push_str(&sys_state()->title); return ELM_OK;
    case ELM_ROOT_CHECK_SELPOINT:
    case ELM_ROOT_CHECK_SAVEPOINT: vm_push_int(0); return ELM_OK; /* unused */
    case ELM_ROOT_CLEAR_SELPOINT:
    case ELM_ROOT_CLEAR_SAVEPOINT: /* unused */
    case ELM_ROOT_STACK_SELPOINT:
    case ELM_ROOT_DROP_SELPOINT: return ELM_OK;
    case ELM_ROOT_IAPP_DUMMY:
        log_once_129();
        elm_push_default(c);
        return ELM_OK;                      /* invalid here (0x65aab4) */
    case ELM_ROOT_CALL: return ELM_UNKNOWN; /* call list: unused */
    default: return vm_elm_global(root, rest, nrest, c);
    }
}

void elm_flow_register(void)
{
    static const int ROOTS[] = {
        ELM_ROOT_MESSAGE_BOX,
        ELM_ROOT_SELPOINT,
        ELM_ROOT_OWARI,
        ELM_ROOT_RETURNMENU,
        ELM_ROOT_JUMP,
        ELM_ROOT_FARCALL,
        ELM_ROOT_DISP,
        ELM_ROOT_BANK_A,
        ELM_ROOT_BANK_B,
        ELM_ROOT_BANK_C,
        ELM_ROOT_BANK_D,
        ELM_ROOT_BANK_E,
        ELM_ROOT_BANK_F,
        ELM_ROOT_BANK_G,
        ELM_ROOT_BANK_Z,
        ELM_ROOT_BANK_S,
        ELM_ROOT_BANK_M,
        ELM_ROOT_SAVEPOINT,
        ELM_ROOT_MATH,
        ELM_ROOT_COUNTER,
        ELM_ROOT_TIMEWAIT,
        ELM_ROOT_TIMEWAIT_KEY,
        ELM_ROOT_SCRIPT,
        ELM_ROOT_SET_TITLE,
        ELM_ROOT_GET_TITLE,
        ELM_ROOT_CUR_CALL,
        ELM_ROOT_CALL,
        ELM_ROOT_NAMAE_LOCAL,
        ELM_ROOT_NAMAE_GLOBAL,
        ELM_ROOT_NAMAE,
        ELM_ROOT_CHECK_SELPOINT,
        ELM_ROOT_CLEAR_SELPOINT,
        ELM_ROOT_CHECK_SAVEPOINT,
        ELM_ROOT_CLEAR_SAVEPOINT,
        ELM_ROOT_IAPP_DUMMY,
        ELM_ROOT_GET_SCENE_NAME,
        ELM_ROOT_BANK_X,
        ELM_ROOT_DEL_CALL_STACK,
        ELM_ROOT_GET_CALL_STACK_CNT,
        ELM_ROOT_SET_CALL_STACK_CNT,
        ELM_ROOT_INIT_CALL_STACK,
        ELM_ROOT_STACK_SELPOINT,
        ELM_ROOT_DROP_SELPOINT,
        ELM_ROOT_GET_LINE_NO,
    };
    elm_register_roots(ROOTS, (int)COUNT_OF(ROOTS), flow_global);
}
