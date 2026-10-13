/* gfx/gfx_event.c — IntEvent interpolation (GFX §3.2; exe FUN_005f4960 advance, FUN_005f50e0 / FUN_005ba120
 * evaluate). All fields are script integers: time advances with two's-complement wrap, the curve is evaluated in
 * 64-bit / double arithmetic, and a result outside int range becomes INT_MIN (what the x86 cvttsd2si of the
 * original yields), so no input can reach undefined behaviour. */
#include "gfx/gfx.h"
#include <limits.h>
#include <string.h>

void ievent_init(IntEvent *e, int def)
{
    memset(e, 0, sizeof *e);
    e->def = e->value = e->cur = e->start = e->end = def;
    e->loop = -1;
}

void ievent_assign(IntEvent *e, int v)
{
    e->value = e->cur = v;
    e->loop = -1;
}

/* sub 0 / 7 (FUN_00661040 cases 0/7): getters return the target right away */
void ievent_set(IntEvent *e, int value, int total, int delay, int speed, bool has_start, int start, bool real)
{
    if (has_start) e->value = start;
    e->start = e->cur = e->value;
    e->end = value;
    e->time = 0; e->total = total; e->delay = delay; e->speed = speed;
    e->loop = 0; e->real = real ? 1 : 0;
    e->value = value;
}

/* sub 1/8 (loop) and 2/9 (turn) via FUN_005ba0a0 / FUN_005ba0e0: E.value is unchanged */
void ievent_loop(IntEvent *e, int start, int end, int total, int delay, int speed, int loop, bool real)
{
    e->start = e->cur = start;
    e->end = end;
    e->time = 0; e->total = total; e->delay = delay; e->speed = speed;
    e->loop = loop; e->real = real ? 1 : 0;
}

void ievent_end(IntEvent *e) { e->loop = -1; e->cur = e->value; }

/* double -> int truncation; out of range (or NaN) gives INT_MIN like cvttsd2si. */
static int to_int(double v) { return (v > -2147483649.0 && v < 2147483648.0) ? (int)v : INT_MIN; }

/* Curve value at time t of total T (0 < t, T > 0). */
static int ev_curve(const IntEvent *e, int64_t t)
{
    double d = (double)e->end - e->start, T = e->total, tt = (double)t;
    switch (e->speed) {
    case 0: return to_int(d * tt / T + e->start);
    case 1: return to_int((d * tt * tt / T) / T + e->start);
    case 2: return to_int(e->end - (d * (tt - T) * (tt - T) / T) / T);
    default: return e->cur;   /* unknown speed: cur keeps value */
    }
}

/* FUN_005ba120 (CONFIRMED); called only while loop != -1, after cur = value */
static void ev_update(IntEvent *e)
{
    int64_t t = (int64_t)e->time - e->delay, total = e->total;
    if (e->loop == 0 && t - total >= 0) { e->loop = -1; return; }
    if (t < 1) { e->cur = e->start; return; }
    if (total <= 0) { e->cur = e->end; return; }   /* loop/turn with total 0: avoid div by 0 */
    if (e->loop == 1) t %= total;
    if (e->loop == 2) { t %= 2 * total; if (t - total > 0) t = 2 * total - t; }
    e->cur = ev_curve(e, t);
}

void ievent_frame(IntEvent *e, int32_t game_dt, int32_t real_dt)
{
    e->cur = e->value;
    if (e->loop == -1) return;
    e->time = core_wrap_add_i32(e->time, e->real ? real_dt : game_dt);
    ev_update(e);
}
