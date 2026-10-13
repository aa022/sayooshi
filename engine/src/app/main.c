/* app/main.c — command line (ENGINE_SPEC §3.1), boot sequence (§2.13), main loop (§2.7), shutdown.
 *
 * Every exit path after mem_init goes through app_shutdown(), which releases what the app started (the shutdown
 * functions it calls are safe to call when their module never started). Numeric options are range-checked against
 * the limits in OPTIONS; malformed or out-of-range values are rejected with a message and exit code 1. */
#include "core/check.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "app/game.h"
#include "app/harness.h"
#include "plat/fs.h"
#include "plat/platform.h"
#include "plat/sysdep.h"
#include "fmt/pack.h"
#include "fmt/gameexe.h"
#include "fmt/cgtable.h"
#include "vm/vm.h"
#include "vm/proc.h"
#include "vm/clock.h"
#include "vm/sysstate.h"
#include "gfx/gfx.h"
#include "gfx/button.h"
#include "gfx/canvas.h"
#include "txt/mwnd.h"
#include "txt/font.h"
#include "txt/lang.h"
#include "txt/backlog.h"
#include "snd/audio.h"
#include "plat/input.h"
#include "ui/ui.h"
#include "sav/sav.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char USAGE[] =
"usage: sayooshi [<GameData> | --gamedata DIR] [options]\n"
"  (no data argument)         use the game data built into this executable (or game.pak / data/ next to it)\n"
"  --gamedata DIR             original game files (Scene.pck, Gameexe.dat, g00/, bgm/, koe/, ...), a data tree\n"
"                             with ja/ and en/ (make data), or a SAYOPAK .pak file\n"
"  --lang ja|en               text language (en = the EN patch overlay). Without it a window run shows the\n"
"                             language selector (last choice preselected, from config.sav); a --headless run\n"
"                             uses the last choice, else ja\n"
"  --select-language          show the language selector even in --headless runs (driven by --input; it\n"
"                             picks the highlighted language after 600 frames)\n"
"  --verify-data              check every file of the built-in game data against its CRC-32, then exit\n"
"  --data-info                print where the game data comes from and which file serves each key path\n"
"  --console                  Windows: open a console window for the log (a parent console is used anyway)\n"
"  --headless                 no window; SDL video/audio not opened (audio mixed into a null sink)\n"
"  --fixed-dt MS              deterministic frame time (default 16 in headless)\n"
"  --frames N | --steps N     stop after N frames / N VM instructions\n"
"  --until-text N             stop after the N-th TEXT opcode (headless: implies --auto-advance 0 unless\n"
"                             --auto-advance or --input is given)\n"
"  --scene NAME [--z Z]       start at a scene instead of #START_SCENE\n"
"  --vm-stubs                 replace every non-VM element with vm_ref.py-compatible stubs\n"
"  --sweep                    run every scene from z0 with fresh state (vm_ref format)\n"
"  --sel K                    choice policy: always K (default 1)\n"
"  --choices 0,2,1,...        consume these indices for successive selections\n"
"  --auto-advance [MS]        inject a decide click MS ms (default 0) after a wait starts\n"
"  --input FILE               scripted raw input\n"
"  --trace FILE               trace log (categories via --trace-cats VM,CMD,TEXT,PROC,SND,GFX,UNK,INPUT)\n"
"  --dump-at-text N=FILE.png  write the framebuffer after TEXT N has finished typing (repeatable)\n"
"  --dump-every N DIR         PNG every N frames\n"
"  --dump-at-frame N=FILE.png write the framebuffer at frame N (repeatable)\n"
"  --audio-capture FILE.wav   write the mixed output (headless null sink)\n"
"  --strict                   exit 2 on any UNK element or VM error\n"
"  --save-at-text N=SLOT      write a save slot after the snapshot of the block containing TEXT N\n"
"  --load SLOT                boot by loading a slot\n"
"  --save-dir DIR             saves/global/config location (default: a 'saves' folder next to the executable\n"
"                             if it or 'portable.txt' exists there, else the per-user data directory)\n"
"  --print-save-dir           print the save directory and exit\n"
"  --mem-report               print heap and arena peaks at exit (sizing the arena budgets)\n"
"  --export-saves FILE        write every save file into one bundle FILE and exit\n"
"  --import-saves FILE        validate a bundle, back up the current files, copy its files in, and exit\n"
"  --set-global SPEC          debug: preset global data (e.g. Z=0..19:1)\n"
"  --selftest-assets          decode every asset and print the M0 counts/CRCs, then exit\n"
"  --font PATH                TTF/OTF/TTC for text\n"
"  --selftest [LIST]          run module unit selftests (all, or csv of core,formats,gfx,text,audio,ui,save)\n"
"  --scale N                  window size multiplier (default from config, 1)\n"
"  --nearest                  nearest-neighbour scaling instead of linear\n"
"  --fullscreen               start fullscreen (Alt+Enter / F11 toggle)\n"
"  --no-audio                 do not open an audio device (null sink)\n"
"  --audio-dummy              audio device on SDL's dummy driver (callback thread runs; for helgrind/drd)\n"
"  --skip                     hold forced skip (Ctrl) for the whole run\n"
"  -v, --verbose              debug logging\n"
"  -h, --help                 this help\n";

/* ================================================================== options */

enum {
    APP_COUNT_MAX = 2147483647,          /* frames, steps, TEXT numbers given on the command line */
    APP_SLOT_MAX = SAV_QUICK_BASE + SAV_QUICK_CNT - 1,
    APP_SCALE_MAX = 8,                   /* as window.scale in config.sav */
    APP_DT_MAX = 1000,                   /* ms per frame (--fixed-dt) */
    APP_FRAME_DT_MAX = 100,              /* wall-clock frame time is clamped to this (a stall does not jump ahead) */
    APP_DELAY_MAX = 600000,              /* ms, --auto-advance */
    APP_CHOICE_MAX = 999,                /* choice indices (the harness clamps to the items shown) */
    APP_Z_MAX = 999                      /* z labels 0..999 */
};

typedef struct AppOpts {
    HarnessOpts h;
    const char *lang;                    /* --lang */
    bool select_language;                /* --select-language */
    bool verify_data;                    /* --verify-data */
    bool data_info;                      /* --data-info */
    bool console;                        /* --console (Windows) */
    bool print_save_dir;                 /* --print-save-dir */
    bool mem_report;                     /* --mem-report */
    const char *export_saves, *import_saves;   /* --export-saves FILE / --import-saves FILE */
} AppOpts;

/* One line on stderr for the user (command-line and boot errors). */
static void cli_error(const char *fmt, ...) CORE_PRINTF(1, 2);
static void cli_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)fputs("sayooshi: ", stderr);   /* nothing sensible to do if stderr is gone */
    (void)vfprintf(stderr, fmt, ap);
    (void)fputc('\n', stderr);
    va_end(ap);
}

/* The argument being parsed: argv[*i] is the option; take_value() advances *i to its value. */
typedef struct ArgCursor { int argc; char **argv; int *i; const char *opt; } ArgCursor;

static bool arg_fail(const ArgCursor *c, const char *what, const char *value)
{
    cli_error("%s: %s%s%s%s", c->opt, what, value ? " '" : "", value ? value : "", value ? "'" : "");
    return false;
}

static const char *take_value(const ArgCursor *c)
{
    if (*c->i + 1 >= c->argc) return NULL;
    return c->argv[++*c->i];
}

/* "N=VALUE": N in [lo, hi]; *value points into s. */
static bool split_eq(const char *s, long long lo, long long hi, long long *n, const char **value)
{
    size_t eq;
    Str all = str_c(s);
    if (!str_find_char(all, '=', &eq) || eq + 1 >= all.n) return false;
    *value = s + eq + 1;
    return str_to_int_range(str_sub(all, 0, eq), lo, hi, n);
}

typedef bool (*OptFn)(AppOpts *o, const ArgCursor *c);
typedef enum { OPT_FLAG, OPT_STR, OPT_INT, OPT_I64, OPT_FN } OptKind;
typedef struct AppOpt { const char *name; OptKind kind; size_t off; long long lo, hi; OptFn fn; } AppOpt;

static bool opt_verbose(AppOpts *o, const ArgCursor *c) { (void)o; (void)c; log_set_level(LOG_DEBUG); return true; }

static bool opt_selftest(AppOpts *o, const ArgCursor *c)
{
    const char *v = *c->i + 1 < c->argc ? c->argv[*c->i + 1] : NULL;
    o->h.selftest = "all";
    if (v && v[0] != '-' && strspn(v, "abcdefghijklmnopqrstuvwxyz,") == strlen(v)) {   /* optional LIST */
        o->h.selftest = v;
        ++*c->i;
    }
    return true;
}

static bool opt_auto_advance(AppOpts *o, const ArgCursor *c)
{
    const char *v = *c->i + 1 < c->argc ? c->argv[*c->i + 1] : NULL;
    long long ms, any;
    o->h.auto_advance = 0;
    if (!v || !str_to_int_range(str_c(v), -APP_COUNT_MAX - 1LL, APP_COUNT_MAX, &any)) return true;   /* no MS */
    ++*c->i;
    if (!str_to_int_range(str_c(v), 0, APP_DELAY_MAX, &ms)) return arg_fail(c, "delay out of range (0..600000 ms)", v);
    o->h.auto_advance = (int)ms;
    return true;
}

static bool opt_choices(AppOpts *o, const ArgCursor *c)
{
    const char *v = take_value(c);
    if (!v || !*v) return arg_fail(c, "needs an argument", NULL);
    Str rest = str_c(v), field;
    while (str_split(&rest, ',', &field)) {   /* each field consumes input: bounded by strlen(v) */
        long long k;
        if (o->h.nchoices >= HARNESS_MAX_LIST) return arg_fail(c, "too many choices", NULL);
        if (!str_to_int_range(field, 0, APP_CHOICE_MAX, &k)) return arg_fail(c, "bad choice list (indices 0..999)", v);
        o->h.choices[o->h.nchoices++] = (int)k;
        if (!rest.n) break;
    }
    return true;
}

static bool opt_dump_at(AppOpts *o, const ArgCursor *c)
{
    bool text = !strcmp(c->opt, "--dump-at-text");
    HarnessDump *list = text ? o->h.dump_at_text : o->h.dump_at_frame;
    int *cnt = text ? &o->h.ndump_at_text : &o->h.ndump_at_frame;
    const char *v = take_value(c), *file;
    long long n;
    if (!v) return arg_fail(c, "needs an argument", NULL);
    if (*cnt >= HARNESS_MAX_LIST) return arg_fail(c, "given too often", NULL);
    if (!split_eq(v, text ? 1 : 0, APP_COUNT_MAX, &n, &file)) return arg_fail(c, "expects N=FILE", v);
    list[*cnt].n = (int)n;
    list[*cnt].file = file;
    ++*cnt;
    return true;
}

static bool opt_save_at_text(AppOpts *o, const ArgCursor *c)
{
    const char *v = take_value(c), *slot;
    long long n, s;
    if (!v) return arg_fail(c, "needs an argument", NULL);
    if (o->h.nsave_at_text >= HARNESS_MAX_LIST) return arg_fail(c, "given too often", NULL);
    if (!split_eq(v, 1, APP_COUNT_MAX, &n, &slot) || !str_to_int_range(str_c(slot), 0, APP_SLOT_MAX, &s))
        return arg_fail(c, "expects N=SLOT (slot 0..102)", v);
    o->h.save_at_text[o->h.nsave_at_text++] = (HarnessSaveAt){ (int)n, (int)s };
    return true;
}

static bool opt_dump_every(AppOpts *o, const ArgCursor *c)
{
    const char *n = take_value(c), *dir = take_value(c);
    long long k;
    if (!n || !dir) return arg_fail(c, "needs N DIR", NULL);
    if (!str_to_int_range(str_c(n), 1, APP_COUNT_MAX, &k)) return arg_fail(c, "bad frame interval", n);
    o->h.dump_every = (int)k;
    o->h.dump_dir = dir;
    return true;
}

static bool opt_set_global(AppOpts *o, const ArgCursor *c)
{
    const char *v = take_value(c);
    if (!v) return arg_fail(c, "needs an argument", NULL);
    if (o->h.nset_global >= HARNESS_MAX_LIST) return arg_fail(c, "given too often", NULL);
    o->h.set_global[o->h.nset_global++] = v;
    return true;
}

#define OF(f)                    offsetof(AppOpts, f)
#define FLAG(n, f)               { n, OPT_FLAG, OF(f), 0, 0, NULL }
#define TEXT(n, f)               { n, OPT_STR, OF(f), 0, 0, NULL }
#define INT(n, f, lo, hi)        { n, OPT_INT, OF(f), lo, hi, NULL }
#define I64(n, f, lo, hi)        { n, OPT_I64, OF(f), lo, hi, NULL }
#define FN(n, fn)                { n, OPT_FN, 0, 0, 0, fn }
static const AppOpt OPTIONS[] = {
    FN("-v", opt_verbose),
    FN("--verbose", opt_verbose),
    FLAG("--console", console),
    FLAG("--print-save-dir", print_save_dir),
    FLAG("--mem-report", mem_report),
    TEXT("--export-saves", export_saves),
    TEXT("--import-saves", import_saves),
    TEXT("--gamedata", h.gamedata),
    TEXT("--lang", lang),
    FLAG("--select-language", select_language),
    FLAG("--verify-data", verify_data),
    FLAG("--data-info", data_info),
    FLAG("--headless", h.headless),
    FLAG("--vm-stubs", h.vm_stubs),
    FLAG("--sweep", h.sweep),
    FLAG("--strict", h.strict),
    FLAG("--selftest-assets", h.selftest_assets),
    FN("--selftest", opt_selftest),
    INT("--scale", h.scale, 1, APP_SCALE_MAX),
    FLAG("--nearest", h.nearest),
    FLAG("--fullscreen", h.fullscreen),
    FLAG("--no-audio", h.no_audio),
    FLAG("--audio-dummy", h.audio_dummy),
    FLAG("--skip", h.skip),
    INT("--fixed-dt", h.fixed_dt, 0, APP_DT_MAX),
    I64("--frames", h.frames, 0, APP_COUNT_MAX),
    I64("--steps", h.steps, 0, APP_COUNT_MAX),
    I64("--until-text", h.until_text, 0, APP_COUNT_MAX),
    TEXT("--scene", h.scene),
    INT("--z", h.z, 0, APP_Z_MAX),
    INT("--sel", h.sel, 0, APP_CHOICE_MAX),
    INT("--load", h.load_slot, 0, APP_SLOT_MAX),
    TEXT("--input", h.input_file),
    TEXT("--trace", h.trace_file),
    TEXT("--trace-cats", h.trace_cats),
    TEXT("--audio-capture", h.audio_capture),
    TEXT("--save-dir", h.save_dir),
    TEXT("--font", h.font),
    FN("--set-global", opt_set_global),
    FN("--auto-advance", opt_auto_advance),
    FN("--choices", opt_choices),
    FN("--dump-at-text", opt_dump_at),
    FN("--dump-at-frame", opt_dump_at),
    FN("--save-at-text", opt_save_at_text),
    FN("--dump-every", opt_dump_every),
};
#undef OF
#undef FLAG
#undef TEXT
#undef INT
#undef I64
#undef FN

static const AppOpt *find_option(const char *name)
{
    for (size_t k = 0; k < COUNT_OF(OPTIONS); k++) if (!strcmp(OPTIONS[k].name, name)) return &OPTIONS[k];
    return NULL;
}

static bool apply_option(AppOpts *o, const AppOpt *opt, const ArgCursor *c)
{
    char *field = (char *)o + opt->off;
    if (opt->kind == OPT_FN) return opt->fn(o, c);
    if (opt->kind == OPT_FLAG) { bool t = true; memcpy(field, &t, sizeof t); return true; }
    const char *v = take_value(c);
    if (!v) return arg_fail(c, "needs an argument", NULL);
    if (opt->kind == OPT_STR) { memcpy(field, (const void *)&v, sizeof v); return true; }
    long long n;
    if (!str_to_int_range(str_c(v), opt->lo, opt->hi, &n)) {
        char range[64];
        bool fit = str_fmt(range, sizeof range, "bad number (allowed %lld..%lld)", opt->lo, opt->hi);
        return arg_fail(c, fit ? range : "bad number", v);
    }
    if (opt->kind == OPT_INT) { int iv = (int)n; memcpy(field, &iv, sizeof iv); }
    else { int64_t lv = (int64_t)n; memcpy(field, &lv, sizeof lv); }
    return true;
}

static void opts_defaults(AppOpts *o)
{
    memset(o, 0, sizeof *o);
    o->h.sel = 1;
    o->h.auto_advance = -1;
    o->h.load_slot = -1;
}

/* Returns -1 to continue, otherwise the exit code. */
static int parse_cli(int argc, char **argv, AppOpts *o)
{
    opts_defaults(o);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        ArgCursor c = { argc, argv, &i, a };
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { (void)fputs(USAGE, stdout); return 0; }
        const AppOpt *opt = find_option(a);
        if (opt) { if (!apply_option(o, opt, &c)) return 1; continue; }
        if (a[0] == '-' && a[1]) { cli_error("unknown option %s (try --help)", a); return 1; }
        if (o->h.gamedata) { cli_error("unexpected argument %s", a); return 1; }
        o->h.gamedata = a;   /* positional <GameData> (§3.1) */
    }
    if (o->h.headless && o->h.fixed_dt == 0) o->h.fixed_dt = 16;
    /* Headless --until-text without any input source would wait for a click forever: advance automatically. */
    if (o->h.headless && o->h.until_text > 0 && o->h.auto_advance < 0 && !o->h.input_file) o->h.auto_advance = 0;
    return -1;
}

/* ================================================================== boot helpers */

static Pack g_pack;

/* §2.13 step 1. The layered fs (fs_discover) is already mounted; fs_path gives "vfs:" paths. */
static bool load_formats(void)
{
    bool ok = true;
    char *p = fs_path(NULL, "Gameexe", "dat");
    if (!p || !gx_load(p)) { log_msg(LOG_ERR, "boot: cannot load Gameexe.dat"); ok = false; }
    fs_free(p);
    p = fs_path(NULL, "Scene", "pck");
    if (!p || !pack_load(&g_pack, p)) { log_msg(LOG_ERR, "boot: cannot load Scene.pck"); ok = false; }
    fs_free(p);
    p = fs_path("dat", "MODE", "CGM");
    if (!p || !cgt_load(p)) log_msg(LOG_WARN, "boot: cannot load dat/MODE.CGM (CG mode flags unavailable)");
    fs_free(p);
    return ok;
}

/* --set-global BANK=lo[..hi]:value | BANK[i]=value: parse into bank letter index, range and value. */
static bool parse_set_global(const char *spec, int *bank, int32_t *lo, int32_t *hi, int32_t *val)
{
    static const char BANKS[] = "ABCDEFGZX";
    Str s = str_c(spec);
    size_t pos = 1;
    const char *b = s.n ? strchr(BANKS, str_ascii_upper(s.p[0])) : NULL;
    if (!b || !*b || s.n < 2) return false;
    *bank = (int)(b - BANKS);
    char form = s.p[pos++];
    if (form != '[' && form != '=') return false;
    if (!str_scan_i32(s, &pos, lo)) return false;
    *hi = *lo;
    if (form == '[') {
        if (pos + 1 >= s.n || s.p[pos] != ']' || s.p[pos + 1] != '=') return false;
        pos += 2;
    } else {
        if (pos + 1 < s.n && s.p[pos] == '.' && s.p[pos + 1] == '.') {
            pos += 2;
            if (!str_scan_i32(s, &pos, hi)) return false;
        }
        if (pos >= s.n || s.p[pos] != ':') return false;
        pos++;
    }
    return str_scan_i32(s, &pos, val) && pos == s.n && *hi >= *lo;
}

/* Banks A B C D E F G Z X (§4 M4). */
static bool apply_set_global(const char *spec)
{
    static const char NAMES[] = "ABCDEFGZX";
    static const int ROOTS[] = { 25, 26, 27, 28, 29, 30, 31, 32, 137 };
    int bank;
    int32_t lo, hi, val;
    if (!parse_set_global(spec, &bank, &lo, &hi, &val)) return false;
    for (int64_t i = lo; i <= hi; i++) {   /* ends at the bank size: vm_bank_int is NULL past it */
        int32_t *w = vm_bank_int(ROOTS[bank], (int)i);
        if (!w) { log_msg(LOG_WARN, "set-global: %c[%lld] out of range", NAMES[bank], (long long)i); return false; }
        *w = val;
    }
    log_msg(LOG_INFO, "set-global: %c[%" PRId64 "..%" PRId64 "] = %" PRId64 "", NAMES[bank], (int64_t)lo, (int64_t)hi,
            (int64_t)val);
    return true;
}

static void log_scene_missing(const WStr *name)
{
    Scratch sc = mem_scratch_begin(NULL);
    log_msg(LOG_ERR, "boot: scene %s not found", ws_utf8_arena(sc.arena, name, WS_UTF8_SHORT_MAX));
    mem_scratch_end(&sc);
}

static bool start_scene(const char *scene, int z)
{
    WStr name = { 0 };
    if (scene) ws_set_utf8(&name, scene);
    else if (gx_str("START_SCENE", 0, &name) && name.len) z = gx_int("START_SCENE", 1, 0);
    else ws_set_utf8(&name, "_start");
    int idx = pack_find_scene(&g_pack, &name);
    if (idx < 0) log_scene_missing(&name);
    ws_free(&name);
    return idx >= 0 && vm_start(idx, z);
}

static void set_window_title(void)
{
    WStr t = { 0 };
    if (gx_str("GAMENAME", 0, &t) && t.len) {
        Scratch sc = mem_scratch_begin(NULL);
        plat_set_title(ws_utf8_arena(sc.arena, &t, WS_UTF8_SHORT_MAX));
        mem_scratch_end(&sc);
    }
    ws_free(&t);
}

/* Boot steps shared by the first start and a language switch (§2.13 steps 2-4 after the data is loaded). */
static void game_modules_init(void)
{
    gfx_init();
    gfx_set_cg_hook(sys_cg_mark_flag);   /* SYS §6.3 auto CG unlock */
    mwnd_init();
    if (!vm_init(&g_pack)) log_msg(LOG_WARN, "boot: vm_init failed");
    ui_init();                           /* after vm_init: proc 0x10, strings, #CONFIG defaults */
    app_game_register_procs();
    ui_config_apply();                   /* sav_config() -> audio, sys_state(), mwnd, window mode */
}

/* Language switch from the config dialog: write the shared global data, reload Scene.pck, Gameexe.dat, MODE.CGM,
 * the g00 cache and the font in the other language, reset every game module and restart at #START_SCENE (the EN
 * _start converts the default names; lang_migrate_global_names does the reverse for Japanese). */
static bool game_reboot(LangId id)
{
    const char *lang = lang_code_of(id);
    log_msg(LOG_INFO, "lang: switching %s -> %s (reloading the game data, restart at the title)", lang_code(), lang);
    sav_global_write();
    app_game_reset(APP_RESET_REBOOT);
    if (!lang || !fs_set_language(lang)) {
        log_msg(LOG_ERR, "lang: %s is not available", lang ? lang : "?");
        return true;
    }
    sav_config()->language = id;
    sav_config_write();
    pack_free(&g_pack);
    sys_bind_pack(NULL);                 /* read flags are re-read per (scene, code hash) from global.sav */
    gx_unload();                         /* nothing may point into the data arena once it is reset, even when */
    sav_session_reset();                 /* the new language's Gameexe then fails to load */
    mem_data_reset();                    /* everything tied to the old language's data */
    if (!load_formats()) return false;
    img_cache_clear();
    font_select_for_language(id);
    backlog_clear();
    game_modules_init();
    if (!sav_global_read()) log_msg(LOG_INFO, "lang: no global data");
    lang_migrate_global_names();
    set_window_title();
    return start_scene(NULL, 0);
}

/* ================================================================== app phases */

typedef enum { APP_GO = -1 } AppStep;   /* a phase returns APP_GO to continue, else the process exit code */

typedef struct App {
    AppOpts o;
    bool have_data;                      /* fs_discover found game data */
    bool unit_only;                      /* --selftest without game data */
    bool window;                         /* plat_init done (the selector may open it early) */
} App;

static uint32_t g_fb_px[SCREEN_W * SCREEN_H];   /* the frame buffer (static: 1.2 MB, lives for the process) */

static int no_data_exit(const App *a)
{
    if (a->o.h.gamedata) cli_error("cannot load the game data from %s", a->o.h.gamedata);
    else cli_error("no game data found. This executable has no built-in data and there is no\n"
                   "game.pak or data/ next to it. Pass the original GameData folder (sayooshi /path/to/GameData),\n"
                   "or build a self-contained executable with 'make build' (try --help)");
    plat_message_box("Sayonara o Oshiete", "No game data found. Run with the GameData folder as argument, or use "
                                          "the self-contained build (make build).");
    return 1;
}

static int print_data_info(void)
{
    static const char *const KEYS[] = { "Scene.pck", "Gameexe.dat", "dat/MODE.CGM", "g00/BG_RD.g00", "g00/ATTEN.g00",
                                        "font/sayooshieng-Regular.ttf", "font.ttf" };
    printf("data: %s\nlanguage: %s (available: ja%s)\n", fs_describe(), fs_language(),
           fs_has_language("en") ? " en" : "");
    for (size_t i = 0; i < COUNT_OF(KEYS); i++) {
        const char *c = fs_canonical(KEYS[i]);
        printf("  %-30s -> %s\n", KEYS[i], c ? c : "(missing)");
    }
    return 0;
}

/* Data discovery (docs/PAYLOAD.md), --lang, and the informational modes that exit right away. */
static int phase_data(App *a)
{
    const HarnessOpts *h = &a->o.h;
    a->have_data = fs_discover(h->gamedata);
    if (!a->have_data && (h->gamedata || !h->selftest)) return no_data_exit(a);
    a->unit_only = h->selftest && !a->have_data;
    if (a->o.lang) {
        const char *c = lang_code_of(lang_parse(a->o.lang));
        if (!c || !fs_set_language(c)) {
            cli_error("language '%s' is not available in %s", a->o.lang, fs_describe());
            return 1;
        }
    }
    if (a->o.data_info) return print_data_info();
    if (a->o.verify_data) {
        int bad = fs_verify();
        printf("verify-data: %s: %s (%d damaged files)\n", fs_describe(), bad ? "FAILED" : "ok", bad);
        return bad ? 1 : 0;
    }
    if (h->trace_file && !log_trace_open(h->trace_file, h->trace_cats)) return 1;
    sav_set_dir(h->save_dir);
    harness_set_pack(&g_pack);
    return harness_init(h) ? APP_GO : 1;
}

/* Opens the window (once) with the configured scale and filter, and the font. */
static bool open_window(App *a, int scale)
{
    if (!plat_init(a->o.h.headless, SCREEN_W, SCREEN_H, scale)) {
        cli_error("cannot open the window (try --headless)");
        return false;
    }
    a->window = true;
    if (a->o.h.nearest) plat_set_linear(false);
    return true;
}

/* The language selector in the application's frame loop and frame buffer; LANG_NONE when the window was closed. */
static LangId run_langsel(LangId preselect, int max_frames, int fixed_dt)
{
    LangId only = ui_langsel_begin(preselect);
    if (only != LANG_NONE) return only;
    Canvas fb = { g_fb_px, SCREEN_W, SCREEN_H, SCREEN_W };
    uint32_t last = plat_ticks_ms();
    for (bool running = true; running;) {
        if (!plat_pump()) { log_msg(LOG_INFO, "langsel: window closed"); return LANG_NONE; }
        uint32_t now = plat_ticks_ms();
        int dt = fixed_dt > 0 ? fixed_dt : (int)(now - last > APP_FRAME_DT_MAX ? APP_FRAME_DT_MAX : now - last);
        last = now;
        input_frame();
        harness_tick((uint32_t)dt);
        harness_input_frame();
        running = ui_langsel_frame(&fb, dt, max_frames);
        plat_present(fb.px, fb.pitch);
        harness_after_present(&fb);
        if (harness_should_quit()) return ui_langsel_finish(true);
    }
    return ui_langsel_finish(false);
}

/* Language before any game data is read. Game runs without --lang use the last choice from config.sav, shown
 * preselected in the selector of a window run; tools and tests (selftests, vm-stubs, sweep) stay on ja. */
static int phase_language(App *a)
{
    const HarnessOpts *h = &a->o.h;
    sav_config_defaults();               /* Gameexe is not loaded yet: engine defaults + config.sav */
    (void)sav_config_read();             /* no config.sav: the defaults */
    if (a->o.lang) return APP_GO;
    LangId saved = lang_from_config(sav_config()->language);
    LangId lang = saved != LANG_NONE && lang_available(saved) ? saved : h->headless ? LANG_JA : lang_default();
    if (lang_available_count() > 1 && (!h->headless || a->o.select_language)) {
        if (h->fullscreen) sav_config()->window_mode = 1;
        if (!open_window(a, h->scale > 0 ? h->scale : sav_config()->window_scale)) return 1;
        if (!h->headless && sav_config()->window_mode == 1) plat_set_fullscreen(true);
        if (!font_init(h->font)) log_msg(LOG_WARN, "langsel: no font");
        lang = run_langsel(lang, h->headless ? 600 : 0, h->fixed_dt);
        if (lang == LANG_NONE) return 0; /* window closed on the selector */
        sav_config()->language = lang;
        sav_config_write();
    }
    if (!fs_set_language(lang_code_of(lang))) log_msg(LOG_WARN, "lang: %s is not available", lang_code_of(lang));
    return APP_GO;
}

static int run_selftests(const App *a)
{
    int rc = 0;
    if (a->o.h.selftest_assets) rc = harness_selftest_assets();
    if (a->o.h.selftest) {
        sav_config_defaults();
        int r2 = harness_selftest(a->o.h.selftest);
        if (!rc) rc = r2;
    }
    return rc;
}

/* M1: the vm_ref-format runner; its log (incl. TEXT lines) goes to --trace FILE, else stdout. */
static int run_vm_stubs(const App *a)
{
    const HarnessOpts *h = &a->o.h;
    FILE *out = stdout;
    if (h->trace_file) {
        log_trace_close();
        out = fopen(h->trace_file, "w");
        if (!out) { cli_error("cannot write %s", h->trace_file); return 1; }
    }
    VmRefOpts vo = { h->scene, h->z, h->steps > 0 ? h->steps : 2000000, h->until_text, h->sel, false, false };
    int rc = vm_ref_run(&g_pack, &vo, out);
    if (out != stdout && fclose(out) != 0) { cli_error("cannot write %s", h->trace_file); rc = 1; }
    return rc;
}

/* Tool modes that need no window: selftests, the vm_ref runner and the sweep. */
static int phase_tools(const App *a, bool formats_ok)
{
    const HarnessOpts *h = &a->o.h;
    if (h->selftest_assets || h->selftest) return run_selftests(a);
    if (!formats_ok) { cli_error("cannot load the game data from %s", fs_describe()); return 1; }
    if (h->vm_stubs && !h->sweep) return run_vm_stubs(a);
    if (h->sweep) return harness_sweep();
    return APP_GO;
}

/* §2.13 steps 2-4: configuration, window, audio, game modules, global data, first scene or loaded slot. */
static int phase_boot(App *a)
{
    const HarnessOpts *h = &a->o.h;
    int chosen_lang = sav_config()->language;
    sav_config_defaults();
    (void)sav_config_read();             /* no config.sav: the Gameexe defaults */
    sav_config()->language = chosen_lang;
    if (h->fullscreen) sav_config()->window_mode = 1;
    if (!a->window) {
        if (!open_window(a, h->scale > 0 ? h->scale : sav_config()->window_scale)) return 1;
        if (!font_init(h->font)) {
            if (!h->headless) {
                cli_error("no usable Japanese font found; pass --font PATH (.ttf/.otf/.ttc)");
                return 1;
            }
            log_msg(LOG_WARN, "boot: no font; text will not be drawn");
        }
    }
    font_select_for_language(lang_id());
    set_window_title();
    if (h->audio_dummy && !SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"))
        log_msg(LOG_WARN, "boot: cannot select the dummy audio driver: %s", SDL_GetError());
    audio_init(h->audio_dummy || (!h->headless && !h->no_audio));
    if (h->audio_capture && !audio_capture_start(h->audio_capture))
        log_msg(LOG_ERR, "boot: cannot write %s", h->audio_capture);
    game_modules_init();
    if (h->headless) ui_set_engine_cursor(false);   /* keep PNG dumps free of the mouse cursor */
    plat_set_game_cursor(!h->headless);             /* the game draws its own cursor in a window */
    if (!sav_global_read()) log_msg(LOG_INFO, "boot: no global data yet");
    lang_migrate_global_names();
    for (int i = 0; i < h->nset_global; i++)
        if (!apply_set_global(h->set_global[i])) log_msg(LOG_WARN, "boot: bad --set-global %s", h->set_global[i]);
    if (h->load_slot >= 0 && !sav_slot_load(h->load_slot)) {
        cli_error("cannot load slot %d from %s", h->load_slot, sav_dir());
        return 1;
    }
    if (h->load_slot < 0 && !start_scene(h->scene, h->z)) return 1;
    return APP_GO;
}

/* One frame of §2.7. */
static void app_frame(const App *a, Canvas *fb, uint32_t *last)
{
    uint32_t now = plat_ticks_ms();
    uint32_t dt = a->o.h.fixed_dt > 0 ? (uint32_t)a->o.h.fixed_dt
                                       : (now - *last > APP_FRAME_DT_MAX ? APP_FRAME_DT_MAX : now - *last);
    *last = now;
    clock_frame(dt);
    input_frame();
    harness_tick(dt);
    harness_frame();
    if (ui_active()) ui_frame();
    else { proc_run_frame(); btn_frame(); }
    mwnd_frame(clock_game_dt(), clock_real_dt(), ui_active());
    gfx_frame(clock_game_dt(), clock_real_dt());
    clock_counters_tick(clock_game_dt(), clock_real_dt());
    audio_frame(clock_real_dt());
    gfx_render(fb);
    mwnd_draw(fb);
    ui_draw(fb);
    ui_cursor_draw(fb);
    plat_present(fb->px, fb->pitch);
    harness_after_present(fb);
}

/* The frame loop and §2.13 step 5. Returns the exit code. */
static int phase_loop(const App *a)
{
    Canvas fb = { g_fb_px, SCREEN_W, SCREEN_H, SCREEN_W };
    uint32_t last = plat_ticks_ms();
    bool running = true;
    while (running) {
        LangId want = lang_pending_switch();
        if (want != LANG_NONE) {
            lang_clear_switch();
            if (!game_reboot(want)) {
                cli_error("cannot reload the game data in '%s'", lang_code_of(want));
                return 1;                /* the old state is gone: do not write it */
            }
        }
        running = plat_pump();
        app_frame(a, &fb, &last);
        if (harness_should_quit()) running = false;
        /* Headless with nothing left to run (EOF / halt) and no frame budget: stop. */
        if (a->o.h.headless && a->o.h.frames <= 0 && harness_frame_no() > 1 && proc_top()->type == PROC_HALT)
            running = false;
    }
    sav_global_write();
    sav_config_write();
    int rc = harness_exit_code();
    log_msg(LOG_INFO, "sayooshi: exit after %" PRId64 " frames, %" PRId64 " TEXTs (rc %d)", harness_frame_no(),
            harness_text_count(), rc);
    return rc;
}

/* Releases everything the app started, in reverse order. Each call is a no-op for a module that never started. */
static void app_shutdown(void)
{
    audio_capture_stop();
    harness_shutdown();
    log_trace_close();
    ui_shutdown();
    txt_shutdown();
    vm_shutdown();
    gfx_shutdown();
    audio_shutdown();
    plat_shutdown();
    pack_free(&g_pack);
    sav_shutdown();
}

/* --print-save-dir / --export-saves / --import-saves: no game data needed. -1 when none was given. */
static int save_tools(const AppOpts *o)
{
    if (!o->print_save_dir && !o->export_saves && !o->import_saves) return -1;
    sav_set_dir(o->h.save_dir);
    int rc = 0;
    if (o->print_save_dir) printf("%s\n", sav_dir());
    if (o->export_saves && !sav_export(o->export_saves)) { cli_error("export to %s failed", o->export_saves); rc = 1; }
    if (o->import_saves && !sav_import(o->import_saves)) {
        cli_error("import of %s failed: %s", o->import_saves, sav_last_error());
        rc = 1;
    }
    return rc;
}

static int app_run(int argc, char **argv)
{
    App a;
    memset(&a, 0, sizeof a);
    plat_attach_console(false);          /* a parent console first, so parse errors are seen */
    int rc = parse_cli(argc, argv, &a.o);
    if (a.o.console) plat_attach_console(true);
    if (rc < 0) rc = save_tools(&a.o);
    if (rc < 0) rc = phase_data(&a);
    bool game_run = !a.unit_only && !a.o.h.selftest_assets && !a.o.h.selftest && !a.o.h.vm_stubs && !a.o.h.sweep;
    if (rc < 0 && game_run) rc = phase_language(&a);
    if (rc < 0) {
        log_msg(LOG_INFO, "sayooshi: data=%s lang=%s headless=%d saves=%s", fs_describe(), fs_language(),
                (int)a.o.h.headless, sav_dir());
        bool formats_ok = !a.unit_only && load_formats();   /* §2.13 step 1 */
        rc = phase_tools(&a, formats_ok);
    }
    if (rc < 0) rc = phase_boot(&a);
    if (rc < 0) rc = phase_loop(&a);
    if (a.o.mem_report) mem_report();
    app_shutdown();
    return rc;
}

/* Fatal errors (core_fatal / ASSERT): users who started the game without a console get a message box. */
static void app_fatal_hook(const char *message)
{
    plat_message_box("Sayonara o Oshiete - internal error", message);
}

int main(int argc, char **argv)
{
    mem_init(NULL);
    core_set_fatal_hook(app_fatal_hook);
    int rc = app_run(argc, argv);
    mem_shutdown();
    SDL_Quit();                          /* last: frees SDL's own state (log priorities, hints, subsystems) */
    return rc;
}
