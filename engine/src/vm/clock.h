/* vm/clock.h — real/game clocks, deltas, skip multiplier, the counter[16] objects of global 40 (ENGINE_SPEC §2.7,
 * §1.4.3). All values are int32 milliseconds that wrap like the original's. Main thread only. */
#ifndef VM_CLOCK_H
#define VM_CLOCK_H
#include "core/base.h"

void    clock_frame(uint32_t real_dt_ms);    /* called once per frame by main */
int32_t clock_real_ms(void);  int32_t clock_game_ms(void);
int32_t clock_real_dt(void);  int32_t clock_game_dt(void);
void    clock_counters_tick(int32_t game_dt, int32_t real_dt);   /* global 40 counter[16] (§1.4.3) */

enum { COUNTER_CNT = 16 };
typedef struct {
    bool running, real, frame_mode, frame_loop;
    int32_t frame_start, frame_end, frame_time, value;
} Counter;
Counter *clock_counter(int i);                    /* NULL if out of range */
int32_t  clock_counter_value(const Counter *c);   /* get_value, FUN_005b32d0 */
void     clock_reset(void);           /* legacy WP2 test driver only (engine/tests/wp2_unit.c) */

#endif
