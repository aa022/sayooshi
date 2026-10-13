/* vm/clock.c — real/game clocks (ENGINE_SPEC §2.7) and the counter[16] objects of global 40 (§1.4.3).
 * All arithmetic wraps like the original's 32-bit integers; nothing here allocates. */
#include "vm/clock.h"
#include "vm/sysstate.h"

enum { SKIP_SPEED = 32 };                      /* game clock x32 while skipping (GFX §3.2, FUN_00573000) */

static int32_t g_real_ms, g_game_ms, g_real_dt, g_game_dt;
static Counter g_cnt[COUNTER_CNT];


void clock_reset(void) { g_real_ms = g_game_ms = g_real_dt = g_game_dt = 0; }

void clock_frame(uint32_t real_dt_ms)
{
    g_real_dt = (int32_t)real_dt_ms;
    g_game_dt = sys_is_skipping() ? core_wrap_mul_i32(g_real_dt, SKIP_SPEED) : g_real_dt;
    g_real_ms = core_wrap_add_i32(g_real_ms, g_real_dt);   /* int32, keeps counting (wraps) */
    g_game_ms = core_wrap_add_i32(g_game_ms, g_game_dt);
}

int32_t clock_real_ms(void) { return g_real_ms; }
int32_t clock_game_ms(void) { return g_game_ms; }
int32_t clock_real_dt(void) { return g_real_dt; }
int32_t clock_game_dt(void) { return g_game_dt; }

Counter *clock_counter(int i) { return (i >= 0 && i < COUNTER_CNT) ? &g_cnt[i] : NULL; }

static int32_t trunc_i32(double v)             /* cvttsd2si: NaN / out of range -> INT32_MIN */
{
    return (v > -2147483649.0 && v < 2147483648.0) ? (int32_t)v : INT32_MIN;
}

/* FUN_005b32d0 */
int32_t clock_counter_value(const Counter *c)
{
    if (!c->frame_mode) return c->value;
    if (c->frame_time < 1) return c->frame_end;
    int32_t a = c->frame_start, b = c->frame_end;
    if (a == b) return b;
    int32_t span = core_wrap_sub_i32(b, a);             /* != 0 since a != b */
    int32_t v = trunc_i32((double)c->value * (double)span / (double)c->frame_time);
    if (c->frame_loop) return core_wrap_add_i32(span == -1 ? 0 : v % span, a);   /* INT32_MIN % -1 would trap */
    v = core_wrap_add_i32(v, a);
    if (b < a) { if (v < b) return b; if (v <= a) return v; }
    else if (a <= v) return v > b ? b : v;
    return a;
}

/* FUN_005b3280, once per frame: value += real ? real_dt : game_dt; a non-looping frame counter stops at frame_time. */
void clock_counters_tick(int32_t game_dt, int32_t real_dt)
{
    for (int i = 0; i < COUNTER_CNT; i++) {
        Counter *c = &g_cnt[i];
        if (!c->running) continue;
        c->value = core_wrap_add_i32(c->value, c->real ? real_dt : game_dt);
        if (c->frame_mode && !c->frame_loop && c->value >= c->frame_time) {
            c->value = c->frame_time;
            c->running = false;
        }
    }
}
