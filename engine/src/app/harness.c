/* app/harness.c — headless runner (ENGINE_SPEC §3, WP7): scripted input, auto-advance + choice policy, PNG dumps,
 * save-at-text, --until-text/--steps accounting, the vm_ref-format sweep and the selftest drivers.
 *
 * Memory: the input script and its events live in the perm arena (loaded once at boot, kept to exit); dumps,
 * reference tables and paths are built in scratch arenas. Input scripts are bounded (HARNESS_SCRIPT_MAX bytes)
 * and parsed without fixed-size buffers. */
#include "app/harness.h"
#include "core/bin.h"
#include "core/check.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "plat/sysdep.h"
#include "fmt/pack.h"
#include "fmt/gameexe.h"
#include "fmt/g00.h"
#include "fmt/cgtable.h"
#include "vm/vm.h"
#include "vm/proc.h"
#include "gfx/gfx.h"
#include "gfx/button.h"
#include "txt/mwnd.h"
#include "snd/audio.h"
#include "plat/input.h"
#include "ui/ui.h"
#include "sav/sav.h"
#include "plat/fs.h"
#include <SDL3/SDL.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stb_image_write.h"

enum {
    HARNESS_SCRIPT_MAX = 1 << 20,        /* bytes of an --input script */
    BTN_SCAN_MAX = 64,                   /* button numbers probed per group */
    BTN_GROUP_MAX = 4,                   /* button groups probed by the polling-loop policy */
    SEL_ITEM_MAX = 64,                   /* in-window choice items probed */
    SWEEP_STEPS = 300000,                /* sweep.txt was produced with --steps 300000 */
};

/* ================================================================== state */

enum { EV_MOVE, EV_BUTTON, EV_KEY, EV_WHEEL, EV_TEXT, EV_QUIT };
typedef struct { int64_t frame; int type; int a, b, order; const char *text; } InputEv;

static struct {
    HarnessOpts o;
    const Pack *pack;
    int64_t frame;
    uint32_t ms;                 /* harness clock (sum of frame dts) */
    int64_t texts, steps;
    int  next_choice;
    bool quit, stop_vm;
    int  unknowns, vm_errors;
    bool sweeping;
    int64_t typing_text, typing_from;
    bool dump_pending[HARNESS_MAX_LIST];   /* --dump-at-text entries waiting for the page to finish typing */
    InputEv *ev; int nev, iev;             /* scripted input (perm arena) */
    /* auto-advance / choice policy */
    int  wait_type, wait_depth; uint32_t wait_start;
    int  click_phase;            /* 0 idle, 1 = pressed (release next frame) */
    int  sel_k;                  /* choice picked for the current selection proc (-1 none) */
    int  btn_group, btn_no;      /* button picked in a polling loop (-1 none) */
    bool btn_clicked;
    uint32_t btn_seen;
    bool ctrl_down;
} g_h = { .o = { .sel = 1, .auto_advance = -1, .load_slot = -1 }, .sel_k = -1, .btn_group = -1, .btn_no = -1 };

int64_t harness_text_count(void) { return g_h.texts; }
const HarnessOpts *harness_opts(void) { return &g_h.o; }
const Pack *harness_pack(void) { return g_h.pack; }
int64_t harness_frame_no(void) { return g_h.frame; }
void harness_set_pack(const Pack *pack) { g_h.pack = pack; }
void harness_request_quit(void) { g_h.quit = true; g_h.stop_vm = true; }
int harness_exit_code(void) { return g_h.o.strict && (g_h.unknowns || g_h.vm_errors) ? 2 : 0; }

/* ================================================================== VM hooks */

static bool harness_on_step(void)
{
    if (g_h.stop_vm) return true;
    g_h.steps++;
    int64_t lim = g_h.o.steps;
    if (g_h.sweeping && lim <= 0) lim = SWEEP_STEPS;
    if (lim > 0 && g_h.steps >= lim) {
        g_h.stop_vm = true;                       /* this instruction still runs, like vm_ref */
        if (!g_h.sweeping) g_h.quit = true;
    }
    return false;
}

static void harness_on_vm_stop(const char *reason)
{
    (void)reason;   /* the vm_ref runner prints it; the harness only needs to know the VM stopped */
    if (!g_h.sweeping && g_h.o.headless) g_h.quit = true;
}

static void harness_note_vm_error(const char *what)
{
    g_h.vm_errors++;
    if (what) log_msg(LOG_WARN, "vm error: %s", what);
    if (g_h.o.strict && !g_h.sweeping) g_h.quit = true;
}

static void harness_note_unknown(const char *chain)
{
    g_h.unknowns++;
    if (g_h.o.strict && !g_h.sweeping) {
        log_msg(LOG_ERR, "strict: unknown element %s", chain ? chain : "?");
        g_h.quit = true;
    }
}

/* ================================================================== choice policy */

static int harness_choose(int nitems)
{
    int k = g_h.next_choice < g_h.o.nchoices ? g_h.o.choices[g_h.next_choice++] : g_h.o.sel;
    if (nitems > 0 && (k < 0 || k >= nitems)) {
        log_msg(LOG_WARN, "harness: choice %d out of range (%d items); using %d", k, nitems, nitems - 1);
        k = k < 0 ? 0 : nitems - 1;
    }
    log_trace("INPUT", "frame=%" PRId64 " choose %d of %d", g_h.frame, k, nitems);
    return k;
}

static int peek_choice(void) { return g_h.next_choice < g_h.o.nchoices ? g_h.o.choices[g_h.next_choice] : g_h.o.sel; }

/* ================================================================== scripted input (--input FILE)
 * Lines: "<frame> click X Y | rclick [X Y] | move X Y | down | up | key NAME | keydown NAME | keyup NAME |
 *         wheel DY | text UTF8 | quit". '#' starts a comment. A click/key is a press on <frame>, release on +1. */

enum { EV_PER_LINE_MAX = 3 };
typedef struct { InputEv *out; int n; int64_t frame; } EvSink;   /* out NULL: count only */

static void emit(EvSink *k, int64_t frame, int type, int a, int b, const char *text)
{
    if (k->out) k->out[k->n] = (InputEv){ frame, type, a, b, k->n, text };
    k->n++;
}

static bool parse_key(EvSink *k, Str cmd, Str rest, const char *where)
{
    char name[64];
    Str tok = str_next_token(&rest);
    if (!tok.n) return true;   /* no key name: ignored, as before */
    SDL_Scancode sc = str_copy(name, sizeof name, tok) ? SDL_GetScancodeFromName(name) : SDL_SCANCODE_UNKNOWN;
    if (sc == SDL_SCANCODE_UNKNOWN) {
        if (!k->out) log_msg(LOG_WARN, "harness: %s: unknown key %.*s", where, (int)core_min_sz(tok.n, 64), tok.p);
        return true;
    }
    bool up = str_eq(cmd, STR("keyup"));
    emit(k, k->frame, EV_KEY, (int)sc, up ? 0 : 1, NULL);
    if (str_eq(cmd, STR("key"))) emit(k, k->frame + 1, EV_KEY, (int)sc, 0, NULL);
    return true;
}

/* One command; false when it is not one we know (or lacks its required numbers). rest is NUL-terminated. */
static bool parse_command(EvSink *k, Str cmd, Str rest, const char *where)
{
    int v[2];
    int64_t fr = k->frame;
    if (str_eq(cmd, STR("click")) || str_eq(cmd, STR("rclick"))) {
        int btn = cmd.p[0] == 'r';
        if (str_scan_ints(rest, v, 2) == 2) emit(k, fr, EV_MOVE, v[0], v[1], NULL);
        emit(k, fr, EV_BUTTON, btn, 1, NULL);
        emit(k, fr + 1, EV_BUTTON, btn, 0, NULL);
    } else if (str_eq(cmd, STR("move")) && str_scan_ints(rest, v, 2) == 2) {
        emit(k, fr, EV_MOVE, v[0], v[1], NULL);
    } else if (str_eq(cmd, STR("down")) || str_eq(cmd, STR("up"))) {
        emit(k, fr, EV_BUTTON, 0, cmd.p[0] == 'd', NULL);
    } else if (str_eq(cmd, STR("key")) || str_eq(cmd, STR("keydown")) || str_eq(cmd, STR("keyup"))) {
        return parse_key(k, cmd, rest, where);
    } else if (str_eq(cmd, STR("wheel")) && str_scan_ints(rest, v, 1) == 1) {
        emit(k, fr, EV_WHEEL, v[0], 0, NULL);
    } else if (str_eq(cmd, STR("text"))) {
        emit(k, fr, EV_TEXT, 0, 0, rest.p);
    } else if (str_eq(cmd, STR("quit"))) {
        emit(k, fr, EV_QUIT, 0, 0, NULL);
    } else {
        return false;
    }
    return true;
}

/* One script line, already cut at '#' and trimmed (NUL-terminated at its end). */
static void parse_line(EvSink *k, Str line, const char *path, int lineno)
{
    char where[48];
    bool fit = str_fmt(where, sizeof where, "%.24s:%d", path, lineno);
    (void)fit;   /* a long script path is cut in the warning only */
    Str p = str_skip_blanks(line);
    if (!p.n) return;
    size_t pos = 0;
    int32_t fr;
    Str cmd = { NULL, 0 }, rest = p;
    if (str_scan_i32(p, &pos, &fr)) {
        rest = str_sub(p, pos, p.n - pos);
        cmd = str_next_token(&rest);
    }
    if (!cmd.n) {
        if (!k->out) log_msg(LOG_WARN, "harness: %s:%d: bad line", path, lineno);
        return;
    }
    k->frame = fr;
    rest = str_skip_blanks(rest);
    if (!parse_command(k, cmd, rest, where) && !k->out)
        log_msg(LOG_WARN, "harness: %s:%d: unknown command %.*s", path, lineno, (int)core_min_sz(cmd.n, 64), cmd.p);
}

/* Cut every line at '#', trim it, and hand it to parse_line. The counting pass leaves the buffer untouched; the
 * filling pass (the last one) NUL-terminates each line in place, so text events can point into the buffer. */
static void script_lines(char *d, size_t len, EvSink *k, const char *path)
{
    int lineno = 0;
    size_t at = 0;
    while (at < len) {   /* each line consumes at least its terminator */
        char *line = d + at;
        size_t n = 0;
        while (at + n < len && line[n] != '\n') n++;
        at += n + 1;
        lineno++;
        for (size_t i = 0; i < n; i++) if (line[i] == '#') { n = i; break; }
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
        if (k->out) line[n] = 0;   /* d[len] is the terminator read_text added */
        parse_line(k, str_make(line, n), path, lineno);
    }
}

static int ev_cmp(const void *pa, const void *pb)
{
    const InputEv *a = pa, *b = pb;
    if (a->frame != b->frame) return a->frame < b->frame ? -1 : 1;
    return (a->order > b->order) - (a->order < b->order);   /* file order within a frame */
}

static bool load_input_script(const char *path)
{
    size_t len;
    char *d = (char *)plat_read_file_arena(mem_perm(), path, HARNESS_SCRIPT_MAX, &len);
    if (!d) { log_msg(LOG_ERR, "harness: cannot read the input script %s", path); return false; }
    len = strlen(d);   /* an embedded NUL ends the script, as it always did */
    EvSink count = { NULL, 0, 0 };
    script_lines(d, len, &count, path);   /* pass 1: count (and warn) */
    EvSink fill = { NULL, 0, 0 };
    if (count.n > 0) {
        fill.out = arena_try_alloc(mem_perm(), (size_t)count.n * sizeof *fill.out, _Alignof(InputEv));
        if (!fill.out) { log_msg(LOG_ERR, "harness: %s has too many events (%d)", path, count.n); return false; }
        script_lines(d, len, &fill, path);   /* pass 2: the same lines again (idempotent in-place cuts) */
        ASSERT(fill.n == count.n);
        qsort(fill.out, (size_t)fill.n, sizeof *fill.out, ev_cmp);
    }
    g_h.ev = fill.out;
    g_h.nev = fill.n;
    log_msg(LOG_INFO, "harness: %d scripted input events from %s", g_h.nev, path);
    return true;
}

static void apply_event(const InputEv *e)
{
    switch (e->type) {
    case EV_MOVE:
        input_raw_mouse(e->a, e->b);
        log_trace("INPUT", "frame=%" PRId64 " move %d %d", g_h.frame, e->a, e->b);
        break;
    case EV_BUTTON:
        input_raw_button(e->a, e->b != 0);
        log_trace("INPUT", "frame=%" PRId64 " %s %s", g_h.frame, e->a ? "rbutton" : "lbutton", e->b ? "down" : "up");
        break;
    case EV_KEY:
        input_raw_key(e->a, e->b != 0);
        log_trace("INPUT", "frame=%" PRId64 " key %s %s", g_h.frame, SDL_GetScancodeName((SDL_Scancode)e->a),
                  e->b ? "down" : "up");
        break;
    case EV_WHEEL:
        input_raw_wheel(e->a);
        log_trace("INPUT", "frame=%" PRId64 " wheel %d", g_h.frame, e->a);
        break;
    case EV_TEXT:
        input_raw_text(e->text);
        log_trace("INPUT", "frame=%" PRId64 " text %s", g_h.frame, e->text);
        break;
    case EV_QUIT:
        g_h.quit = true;
        break;
    default:
        ASSERT_MSG(false, "harness: event type %d", e->type);
    }
}

static void apply_script_events(void)
{
    while (g_h.iev < g_h.nev && g_h.ev[g_h.iev].frame <= g_h.frame) apply_event(&g_h.ev[g_h.iev++]);
}

/* ================================================================== auto-advance (--auto-advance, --choices) */

static void start_click(int x, int y, bool move, const char *why)
{
    if (move) input_raw_mouse(x, y);
    input_raw_button(0, true);
    g_h.click_phase = 1;
    log_trace("INPUT", "frame=%" PRId64 " decide (%s) at %d %d", g_h.frame, why, move ? x : input_mouse_x(),
              move ? y : input_mouse_y());
}

static bool rect_center(const Rect *r, int *x, int *y)
{
    if (r->x1 <= r->x0 || r->y1 <= r->y0) return false;
    *x = (int)(((int64_t)r->x0 + r->x1) / 2);   /* the mean of two ints is an int; the sum may not be */
    *y = (int)(((int64_t)r->y0 + r->y1) / 2);
    return true;
}

/* Group number of a proc 0x10 chain (`stage .6 -1 group ...`, §1.4.4): the int after the last -1. */
static int chain_group(const Proc *p)
{
    for (int i = p->nchain - 2; i >= 0; i--)
        if (p->chain[i] == -1) return p->chain[i + 1];
    return 0;
}

static bool find_button(int group, int want, int *no, Rect *r)
{
    if (want >= 0 && btn_rect(group, want, r)) { *no = want; return true; }
    for (int k = 0; k < BTN_SCAN_MAX; k++)
        if (btn_rect(group, k, r)) { *no = k; return true; }
    return false;
}

/* In-window choice (TXT §4): click the chosen item's rect once the items are laid out. */
static void advance_sel(void)
{
    Rect r;
    int x, y;
    if (g_h.sel_k < 0) {
        int n = 0;
        while (n < SEL_ITEM_MAX && mwnd_sel_item_rect(n, &r)) n++;
        if (n == 0) return;
        g_h.sel_k = harness_choose(n);
    }
    if (mwnd_sel_item_rect(g_h.sel_k, &r) && rect_center(&r, &x, &y)) start_click(x, y, true, "selmsg");
}

/* Blocking objbtngroup.sel (§1.4.4). */
static void advance_btnsel(const Proc *p)
{
    Rect r;
    int x, y, no;
    if (g_h.sel_k < 0) g_h.sel_k = harness_choose(0);
    if (find_button(chain_group(p), g_h.sel_k, &no, &r) && rect_center(&r, &x, &y)) start_click(x, y, true, "button");
}

/* Polling loop (`disp`): seen021's place select polls objbtngroup.get_decided_no (§1.4.4). Click the chosen
 * button once it has been visible for the delay; one choice is consumed per button set. */
static void advance_poll(uint32_t delay)
{
    Rect r;
    int x, y, no = -1, g;
    bool any = false;
    for (g = 0; g < BTN_GROUP_MAX && !any; g++) any = find_button(g, peek_choice(), &no, &r);
    g--;
    if (!any) { g_h.btn_group = -1; g_h.btn_clicked = false; return; }
    if (g_h.btn_group != g) { g_h.btn_group = g; g_h.btn_seen = g_h.ms; g_h.btn_clicked = false; g_h.btn_no = -1; }
    if (g_h.btn_clicked && g_h.ms - g_h.btn_seen >= delay + 3000) {      /* click not taken: retry the same button */
        g_h.btn_clicked = false;
        g_h.btn_seen = g_h.ms - delay - 500;
    }
    if (g_h.btn_clicked || g_h.ms - g_h.btn_seen < delay + 500) return;  /* +500: let the fade-in finish */
    if (g_h.btn_no >= 0 && !btn_rect(g, g_h.btn_no, &r)) g_h.btn_no = -1;   /* a different button set */
    if (g_h.btn_no < 0) {
        g_h.btn_no = harness_choose(0);
        if (!find_button(g, g_h.btn_no, &no, &r)) return;
        g_h.btn_no = no;
    }
    if (rect_center(&r, &x, &y)) {
        start_click(x, y, true, "button-poll");
        g_h.btn_clicked = true;
        g_h.btn_seen = g_h.ms;
    }
}

/* Tracks the proc on top: a new wait type or depth restarts the delay and forgets the picked choice. */
static void track_wait(int type, int depth)
{
    if (type != PROC_SCRIPT && type != PROC_YIELD && type != PROC_YIELD2) {
        g_h.btn_group = -1;               /* a new polling loop later is a new button set */
        g_h.btn_clicked = false;
    }
    if (type == g_h.wait_type && depth == g_h.wait_depth) return;
    g_h.wait_type = type;
    g_h.wait_depth = depth;
    g_h.wait_start = g_h.ms;
    g_h.sel_k = -1;
}

static void auto_advance_frame(void)
{
    bool aa = g_h.o.auto_advance >= 0;
    uint32_t delay = aa ? (uint32_t)g_h.o.auto_advance : 0;
    if (!aa && g_h.o.nchoices <= 0) return;
    if (g_h.click_phase == 1) {           /* release: the press/release pair is the click edge */
        input_raw_button(0, false);
        g_h.click_phase = 0;
        g_h.wait_start = g_h.ms;            /* the next wait of the same type starts now */
        return;
    }
    Proc *p = proc_top();
    if (!p) return;
    track_wait(p->type, proc_depth());
    bool due = g_h.ms - g_h.wait_start >= delay;
    switch (p->type) {
    case PROC_SEL: if (due) advance_sel(); break;
    case PROC_BTNSEL: if (due) advance_btnsel(p); break;
    case PROC_SCRIPT: case PROC_YIELD: case PROC_YIELD2: advance_poll(delay); break;   /* yields pop in-frame */
    case PROC_KEY_WAIT: case PROC_WIPE_WAIT: case PROC_KOE_WAIT: case PROC_SHAKE_WAIT:
        if (aa && due) start_click(0, 0, false, "wait");
        break;
    case PROC_TIMEWAIT:
        if (aa && p->key_skip && due) start_click(0, 0, false, "wait_key");
        break;
    default:
        break;
    }
}

void harness_tick(uint32_t dt_ms)
{
    g_h.frame++;
    g_h.ms += dt_ms;
}

void harness_frame(void)
{
    if (g_h.o.skip && !g_h.ctrl_down) { input_raw_key(SDL_SCANCODE_LCTRL, true); g_h.ctrl_down = true; }
    apply_script_events();
    auto_advance_frame();
}

void harness_input_frame(void) { apply_script_events(); }

/* ================================================================== TEXT hook, dumps */

static void harness_on_text(int text_no)
{
    (void)text_no;   /* the harness counts calls; text_no is informational */
    g_h.texts++;
    if (g_h.sweeping) return;
    for (int i = 0; i < g_h.o.nsave_at_text; i++)
        if (g_h.o.save_at_text[i].text == g_h.texts) {
            /* The latest snapshot is the one taken by the 84 of the block containing this TEXT (§3.1). */
            bool ok = sav_slot_write(g_h.o.save_at_text[i].slot);
            log_msg(ok ? LOG_INFO : LOG_ERR, "harness: save-at-text %" PRId64 " -> slot %d %s", g_h.texts,
                    g_h.o.save_at_text[i].slot, ok ? "ok" : "FAILED");
        }
    for (int i = 0; i < g_h.o.ndump_at_text; i++)
        if (g_h.o.dump_at_text[i].n == g_h.texts) g_h.dump_pending[i] = true;
    g_h.typing_text = g_h.texts;          /* M2 typing-timing check: frames until the page stops printing */
    g_h.typing_from = g_h.frame;
    if (g_h.o.until_text > 0 && g_h.texts >= g_h.o.until_text) g_h.stop_vm = true;
}

static bool dump_png(const Canvas *fb, const char *path)
{
    if (!fb || !fb->px || !path || fb->w <= 0 || fb->h <= 0) return false;
    Scratch s = mem_scratch_begin(NULL);
    uint8_t *rgb = ARENA_NEW_ARRAY(s.arena, uint8_t, (size_t)fb->w * (size_t)fb->h * 3);
    for (int y = 0; y < fb->h; y++)
        for (int x = 0; x < fb->w; x++) {
            uint32_t v = fb->px[(size_t)y * (size_t)fb->pitch + (size_t)x];
            uint8_t *o = rgb + ((size_t)y * (size_t)fb->w + (size_t)x) * 3;
            o[0] = (uint8_t)(v >> 16); o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)v;
        }
    int ok = stbi_write_png(path, fb->w, fb->h, 3, rgb, fb->w * 3);
    mem_scratch_end(&s);
    if (!ok) log_msg(LOG_ERR, "harness: cannot write %s", path);
    else log_trace("GFX", "dump frame=%" PRId64 " text=%" PRId64 " %s", g_h.frame, g_h.texts, path);
    return ok != 0;
}

static void dump_periodic(const Canvas *fb)
{
    char name[48];
    bool fit = str_fmt(name, sizeof name, "/frame_%06" PRId64 ".png", g_h.frame);
    ASSERT(fit);   /* 7 + at most 20 digits + 4 */
    Scratch s = mem_scratch_begin(NULL);
    (void)dump_png(fb, str_cat_arena(s.arena, g_h.o.dump_dir, name, NULL));   /* failures are logged */
    mem_scratch_end(&s);
}

static bool any_text_dump_pending(void)
{
    for (int i = 0; i < g_h.o.ndump_at_text; i++) if (g_h.dump_pending[i]) return true;
    return false;
}

void harness_after_present(const Canvas *fb)
{
    for (int i = 0; i < g_h.o.ndump_at_frame; i++)
        if (g_h.o.dump_at_frame[i].n == g_h.frame) (void)dump_png(fb, g_h.o.dump_at_frame[i].file);   /* logged */
    if (g_h.o.dump_every > 0 && g_h.o.dump_dir && g_h.frame % g_h.o.dump_every == 0) dump_periodic(fb);
    bool printing = mwnd_any_printing();
    if (g_h.typing_text > 0 && !printing) {
        log_trace("HARN", "typed text=%" PRId64 " frames=%" PRId64 "", g_h.typing_text, g_h.frame - g_h.typing_from);
        g_h.typing_text = 0;
    }
    if (printing) return;                /* "after TEXT N has finished typing" (§3.1) */
    for (int i = 0; i < g_h.o.ndump_at_text; i++)
        if (g_h.dump_pending[i]) { (void)dump_png(fb, g_h.o.dump_at_text[i].file); g_h.dump_pending[i] = false; }
}

bool harness_should_quit(void)
{
    if (g_h.quit) return true;
    if (g_h.o.frames > 0 && g_h.frame >= g_h.o.frames) return true;
    return g_h.o.until_text > 0 && g_h.texts >= g_h.o.until_text && !any_text_dump_pending();
}

/* ================================================================== init / shutdown, sweep */

bool harness_init(const HarnessOpts *o)
{
    static const VmHooks HOOKS = { harness_on_step, harness_on_text, harness_on_vm_stop, harness_note_vm_error,
                                   harness_note_unknown };
    vm_set_hooks(&HOOKS);
    if (o) g_h.o = *o;
    g_h.wait_type = -1;
    if (g_h.o.input_file && !load_input_script(g_h.o.input_file)) return false;
    if (g_h.o.dump_every > 0 && g_h.o.dump_dir && !SDL_CreateDirectory(g_h.o.dump_dir))
        log_msg(LOG_WARN, "harness: cannot create %s: %s", g_h.o.dump_dir, SDL_GetError());
    return true;
}

void harness_shutdown(void)
{
    vm_set_hooks(NULL);
    g_h.ev = NULL;                         /* perm arena: released with it at exit */
    g_h.nev = g_h.iev = 0;
}

int harness_sweep(void)
{
    if (!g_h.pack || g_h.pack->nscene <= 0) {
        (void)fputs("sweep: no Scene.pck loaded\n", stderr);   /* nothing to do if stderr is gone */
        return 1;
    }
    /* WP2's vm_ref-compatible runner (byte-identical to vm_ref.py --sweep); the sweep always uses stubs. */
    g_h.sweeping = true;
    int rc = vm_ref_sweep(g_h.pack, g_h.o.steps > 0 ? g_h.o.steps : SWEEP_STEPS, g_h.o.sel, stdout);
    g_h.sweeping = false;
    return rc;
}
