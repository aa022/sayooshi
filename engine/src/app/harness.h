/* app/harness.h — headless runner: the options main parses, input scripts, auto-advance, choice policy, dumps,
 * save-at-text, the vm_ref sweep and the selftest drivers (ENGINE_SPEC §3, WP7). harness_init installs the vm
 * observers (VmHooks: steps, TEXT opcodes, halts, VM errors, unknown chains); everything else is called by main. */
#ifndef APP_HARNESS_H
#define APP_HARNESS_H
#include "core/base.h"
#include "gfx/canvas.h"
#include "fmt/pack.h"

enum { HARNESS_MAX_LIST = 64 };                               /* entries per repeatable option */
typedef struct { int n; const char *file; } HarnessDump;      /* N=FILE (TEXT or frame number) */
typedef struct { int text; int slot; } HarnessSaveAt;

/* Strings point into argv (they live for the whole run). Numbers are range-checked by main. */
typedef struct HarnessOpts {
    const char *gamedata;
    bool headless, vm_stubs, sweep, strict, selftest_assets;
    int fixed_dt;                      /* ms; 0 = wall clock (headless default 16) */
    int64_t frames, steps, until_text; /* 0 = unlimited */
    const char *scene;
    int z;
    int sel; /* default 1 */
    int choices[HARNESS_MAX_LIST];
    int nchoices;
    int auto_advance; /* -1 = off, else ms */
    const char *input_file, *trace_file, *trace_cats, *audio_capture, *save_dir, *font;
    HarnessDump dump_at_text[HARNESS_MAX_LIST];
    int ndump_at_text;
    HarnessDump dump_at_frame[HARNESS_MAX_LIST];
    int ndump_at_frame;
    int dump_every;
    const char *dump_dir;
    HarnessSaveAt save_at_text[HARNESS_MAX_LIST];
    int nsave_at_text;
    int load_slot; /* -1 = none */
    const char *set_global[HARNESS_MAX_LIST];
    int nset_global;
    const char *selftest; /* --selftest [LIST]: "all" or csv of core,formats,gfx,text,audio,ui,save */
    int scale;            /* --scale N (windowed size multiplier; 0 = config) */
    bool nearest;         /* --nearest: nearest-neighbour scaling */
    bool fullscreen;      /* --fullscreen */
    bool no_audio;        /* --no-audio: null sink even with a window */
    bool audio_dummy;     /* --audio-dummy: real SDL device on the "dummy" driver (its callback thread
                                         runs, also headless; for helgrind/drd) */
    bool skip;            /* --skip: force skip mode (Ctrl held) for route runs */
} HarnessOpts;

/* ---- main ---- */
bool harness_init(const HarnessOpts *o);      /* copies o; loads --input; false = unusable input script */
void harness_shutdown(void);
void harness_set_pack(const Pack *pack);      /* the loaded Pack (sweep, save selftest) */
const HarnessOpts *harness_opts(void);        /* the options given to harness_init (app/selftest.c) */
const Pack *harness_pack(void);
void harness_tick(uint32_t dt_ms);            /* once per frame before harness_frame (frame counter, clock) */
void harness_frame(void);                     /* scripted input + auto-advance (§2.7) */
void harness_after_present(const Canvas *fb); /* PNG dumps */
bool harness_should_quit(void);
int  harness_exit_code(void);                 /* 0, or 2 under --strict after UNK / VM errors */
int64_t harness_text_count(void);
int64_t harness_frame_no(void);
int  harness_sweep(void);                     /* --sweep; returns the process exit code */
/* app/selftest.c */
int  harness_selftest_assets(void);           /* M0 report; returns the process exit code */
int  harness_selftest(const char *list);      /* --selftest: module unit selftests; process exit code */

/* ---- hooks for other modules ---- */
void harness_input_frame(void);               /* scripted input only (no auto-advance): screens before the game */
void harness_request_quit(void);              /* e.g. proc 0x2c game end */

/* Module selftests return 0 on success and HARNESS_SELFTEST_ABSENT when they cannot run (no game data). */
enum { HARNESS_SELFTEST_ABSENT = 77 };

#endif
