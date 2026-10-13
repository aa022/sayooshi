/* txt/mwnd.c — message windows: config, mwnd sub-commands (FUN_0065c1d0), typing, open/close animation,
 * wait procs 0x16/0x17/0x19/0x1a, read-flag commits, save/load (TXT §3, §5, §6; ENGINE_SPEC §1.4.1–2, §2.9, §2.11).
 * Drawing is in mwnd_draw.c, choices in sel.c, the unit tests in txt_test.c.
 *
 * Memory: the MWND_CNT windows are static with fixed storage (txt_internal.h); the heap holds only their WStr
 * strings, released by mwnd_window_free (reset, load, shutdown). */
#include "txt/mwnd.h"
#include "txt/txt_internal.h"
#include "core/log.h"
#include "core/str.h"
#include "txt/backlog.h"
#include "fmt/gameexe.h"
#include "gfx/gfx.h"
#include "vm/proc.h"
#include "vm/sysstate.h"
#include "plat/input.h"
#include "snd/audio.h"
#include "sav/sav.h"
#include "vm/vm.h"
#include <inttypes.h>
#include <string.h>

enum {
    MWND_SAVE_VERSION = 1,
    MWND_LOAD_SIZE_MAX = 1000,      /* load-time sanity bound for character sizes (the game uses 24) */
    MWND_LOAD_COUNT_MAX = 1 << 20,  /* load-time bound for the read-flag and replay counts */
    PREVIEW_MAX = 256,              /* units of message text kept for the save slot preview */
    KOE_FADE_MS = 100,              /* TXT §3.4/§3.6 */
    CFG_SIZE_MIN = 1, CFG_SIZE_MAX = 200,
    CFG_COORD_MAX = 1 << 16,        /* Gameexe positions, sizes, spacing and counts are clamped to +-this */
    NAME_BANK_GLOBAL = 107, NAME_BANK_LOCAL = 106,
    SUB_SET_WAKU = 0, SUB_OPEN_WAIT = 1, SUB_CLOSE_WAIT = 2, SUB_CLEAR = 3, SUB_PRINT_FLAG = 4, SUB_SEL_OLD = 5,
    SUB_NL = 6, SUB_SIZE = 7, SUB_KOE = 9, SUB_CLOSE_WAIT2 = 0xd, SUB_CLOSE_NOWAIT = 0xe, SUB_OPEN_WAIT2 = 0xf,
    SUB_OPEN_NOWAIT = 0x10, SUB_MSG_WAIT = 0x12, SUB_PP = 0x14, SUB_MSG_BLOCK = 0x31, SUB_SEL = 0x32,
    SUB_SEL_CANCEL = 0x33, SUB_SEL_CANCEL2 = 0x34, SUB_PAGE = 0x36, SUB_NL_CONTINUE = 0x37, SUB_REMAINDER = 0x39,
    SUB_NAME_WINDOW = 0x3f, SUB_IS_OPEN = 0x41, SUB_LOG_MAX = 0x60,
    WAKU_FROM_CFG = -2,
};

static Mwnd g_mwnd[MWND_CNT];
static int g_cur = MWND_DEFAULT;
static bool g_user_hidden, g_wheel_used;
static WStr g_preview;

/* #CONFIG switches: the game's Gameexe values until ui_config_apply pushes the user's (mwnd_set_options) */
static bool g_no_anime, g_koe_dont_stop, g_wheel_next = true, g_speed_nowait;

/* ---- Gameexe ---------------------------------------------------------------------------------------------- */
void txt_gx_key(char key[TXT_GX_KEY_MAX], const char *group, int no, const char *field)
{
    bool fits = str_fmt(key, TXT_GX_KEY_MAX, "%s.%03d.%s", group, no, field);
    ASSERT_MSG(fits, "gameexe key %s.%d.%s too long", group, no, field);   /* literals + at most 11 digits */
}

int txt_gx_int(const char *group, int no, const char *field, int idx, int def)
{
    char k[TXT_GX_KEY_MAX];
    txt_gx_key(k, group, no, field);
    return gx_int(k, idx, def);
}

static int cfg_int(int no, const char *field, int idx, int def)
{
    return core_clamp_i32(txt_gx_int("MWND", no, field, idx, def), -CFG_COORD_MAX, CFG_COORD_MAX);
}

/* Engine defaults per TXT §5.1 (init loop @0x53f3f0). */
static void cfg_load(MwndCfg *c, int no)
{
    c->novel_mode = cfg_int(no, "NOVEL_MODE", 0, 0);
    c->win_x = cfg_int(no, "WINDOW_POS", 0, 50);    c->win_y = cfg_int(no, "WINDOW_POS", 1, 400);
    c->win_w = cfg_int(no, "WINDOW_SIZE", 0, 700);  c->win_h = cfg_int(no, "WINDOW_SIZE", 1, 150);
    c->msg_x = cfg_int(no, "MESSAGE_POS", 0, 20);   c->msg_y = cfg_int(no, "MESSAGE_POS", 1, 20);
    c->cols = cfg_int(no, "MOJI_CNT", 0, 26);       c->rows = cfg_int(no, "MOJI_CNT", 1, 3);
    c->size = core_clamp_i32(cfg_int(no, "MOJI_SIZE", 0, 25), CFG_SIZE_MIN, CFG_SIZE_MAX);
    c->space_x = cfg_int(no, "MOJI_SPACE", 0, -1);  c->space_y = cfg_int(no, "MOJI_SPACE", 1, 10);
    c->waku_no = cfg_int(no, "WAKU_NO", 0, 0);
    c->name_waku_no = cfg_int(no, "NAME_WAKU_NO", 0, -1);
    c->name_disp_mode = cfg_int(no, "NAME_DISP_MODE", 0, 0);
    c->name_bracket = cfg_int(no, "NAME_BRACKET", 0, 0);
    c->overflow_check = cfg_int(no, "OVERFLOW_CHECK_SIZE", 0, 0);
    c->back_insert_nl = cfg_int(no, "MSG_BACK_INSERT_NL", 0, 0);
    c->open_type = cfg_int(no, "OPEN_ANIME_TYPE", 0, 0);
    c->open_time = cfg_int(no, "OPEN_ANIME_TIME", 0, 0);
    c->close_type = cfg_int(no, "CLOSE_ANIME_TYPE", 0, 0);
    c->close_time = cfg_int(no, "CLOSE_ANIME_TIME", 0, 0);
}

/* §1.4.1 FUN_005ebbd0: copy #WAKU.n into the window frame (this game: filter colour + icons only). */
static void set_waku(Mwnd *w, int waku_no, int name_waku_no)
{
    w->waku = waku_no == WAKU_FROM_CFG ? w->cfg.waku_no : waku_no;
    w->name_waku = name_waku_no == WAKU_FROM_CFG ? w->cfg.name_waku_no : name_waku_no;
    int n = w->waku;
    for (int i = 0; i < 4; i++) {
        int margin = n >= 0 ? txt_gx_int("WAKU", n, "FILTER_MARGIN", i, 0) : 0;
        w->filter[i] = (uint8_t)(n >= 0 ? txt_gx_int("WAKU", n, "FILTER_COLOR", i, 0) : 0);
        w->filter_margin[i] = core_clamp_i32(margin, -CFG_COORD_MAX, CFG_COORD_MAX);
    }
    w->icon_no = n >= 0 ? txt_gx_int("WAKU", n, "ICON_NO", 0, 0) : 0;
    w->page_icon_no = n >= 0 ? txt_gx_int("WAKU", n, "PAGE_ICON_NO", 0, 1) : 1;
}

/* ---- per-window stores -------------------------------------------------------------------------------------- */
void mwnd_rp_clear(Mwnd *w)
{
    for (int i = 0; i < w->nrp; i++) ws_free(&w->rp[i].s);
    w->nrp = 0;
}

void mwnd_rp_add(Mwnd *w, int op, int a, const uint16_t *s, int n)
{
    static bool logged;
    if (w->nrp >= RP_OP_MAX) { txt_cap_overflow(&logged, "page replay log", RP_OP_MAX); return; }
    ReplayOp *o = &w->rp[w->nrp++];
    memset(o, 0, sizeof *o);
    o->op = op; o->a = a;
    if (s && n > 0) ws_set(&o->s, s, n);
}

static void rf_add(Mwnd *w, int scene, int rf)
{
    static bool logged;
    if (scene < 0 || rf < 0) return;
    if (w->nrf + 2 > 2 * RF_PEND_MAX) { txt_cap_overflow(&logged, "pending read flags", RF_PEND_MAX); return; }
    w->rf_pend[w->nrf++] = scene;
    w->rf_pend[w->nrf++] = rf;
}

/* FUN_005e3f70: the read flags of a page are set only when it is committed (TXT §3.2 step 5). */
static void commit_rf(Mwnd *w)
{
    for (int i = 0; i + 1 < w->nrf; i += 2) sys_rf_set(w->rf_pend[i], w->rf_pend[i + 1]);
    w->nrf = 0;
}

/* FUN_005e3d70: drop the glyphs, the koe link and the selection. */
static void page_clear(Mwnd *w)
{
    msg_reset(&w->msg);
    w->koe_no = -1; w->koe_linked = false;
    w->key_wait = false;
    sel_reset(w);
    w->clear_pending = false;
    mwnd_rp_clear(w);
    mwnd_rp_add(w, RP_SIZE, w->msg.size, NULL, 0);
    ws_set(&g_preview, NULL, 0);
}

/* ---- window lifetime ---------------------------------------------------------------------------------------- */
void mwnd_window_free(Mwnd *w)
{
    mwnd_rp_clear(w);
    ws_free(&w->remainder);
    ws_free(&w->name);
}

void mwnd_window_init(Mwnd *w, int no, const MwndCfg *cfg)
{
    mwnd_window_free(w);
    memset(w, 0, sizeof *w);
    w->no = no;
    w->cfg = *cfg;
    msg_bind(&w->msg, w->glyph, MSG_GLYPH_MAX);
    msg_config(&w->msg, &w->cfg);
    w->msg.def_moji = mwnd_style_moji_col();
    w->msg.def_fuchi = mwnd_style_fuchi_col();
    w->msg.def_shadow = mwnd_style_shadow_col();
    msg_reset(&w->msg);
    w->open_anim = w->close_anim = -1;
    w->koe_no = -1;
    sel_reset(w);
    set_waku(w, WAKU_FROM_CFG, WAKU_FROM_CFG);
    mwnd_rp_add(w, RP_SIZE, w->msg.size, NULL, 0);
}

static void reset_windows(void)
{
    for (int i = 0; i < MWND_CNT; i++) {
        MwndCfg cfg;
        cfg_load(&cfg, i);
        mwnd_window_init(&g_mwnd[i], i, &cfg);
    }
    ws_set(&g_preview, NULL, 0);
}

static bool proc_sel_wait(Proc *p);
static bool proc_open_wait(Proc *p);
static bool proc_close_wait(Proc *p);
static bool proc_msg_wait(Proc *p);
static bool proc_key_wait(Proc *p);

void mwnd_init(void)
{
    gfx_set_stage_mwnd_handler(txt_elm_mwnd);
    txt_elm_register();
    mwnd_style_load();
    g_cur = gx_int("MWND.DEFAULT_MWND_NO", 0, MWND_DEFAULT);
    if (g_cur < 0 || g_cur >= MWND_CNT) g_cur = MWND_DEFAULT;
    reset_windows();
    proc_register(PROC_SEL, proc_sel_wait);
    proc_register(PROC_MWND_OPEN, proc_open_wait);
    proc_register(PROC_MWND_CLOSE, proc_close_wait);
    proc_register(0x18, proc_close_wait);
    proc_register(PROC_MSG_WAIT, proc_msg_wait);
    proc_register(PROC_KEY_WAIT, proc_key_wait);
}

void mwnd_reset_all(void)
{
    reset_windows();
    g_user_hidden = false;
}

static WStr g_print;   /* cmd_print's expanded text, kept between lines (released by txt_shutdown) */

void txt_shutdown(void)
{
    for (int i = 0; i < MWND_CNT; i++) mwnd_window_free(&g_mwnd[i]);
    ws_free(&g_preview);
    backlog_clear();
    font_shutdown();
    txt_elm_shutdown();
    msg_shutdown();
    ws_free(&g_print);
}

Mwnd *mwnd_get(int no) { return &g_mwnd[no >= 0 && no < MWND_CNT ? no : g_cur]; }
Mwnd *mwnd_cur(void) { return &g_mwnd[g_cur]; }
void mwnd_set_user_hidden(bool hidden) { g_user_hidden = hidden; }
bool mwnd_user_hidden(void) { return g_user_hidden; }
void mwnd_set_options(bool no_anime, bool koe_dont_stop, bool wheel_next, bool speed_nowait)
{
    g_no_anime = no_anime; g_koe_dont_stop = koe_dont_stop; g_wheel_next = wheel_next; g_speed_nowait = speed_nowait;
}

void mwnd_test_swap_timing(bool *speed_nowait, bool *no_anime)
{
    bool s = g_speed_nowait, a = g_no_anime;
    g_speed_nowait = *speed_nowait; g_no_anime = *no_anime;
    *speed_nowait = s; *no_anime = a;
}

static const WStr *resolve_name(bool global, int idx)
{
    return vm_bank_str(global ? NAME_BANK_GLOBAL : NAME_BANK_LOCAL, idx);
}
static void expand_names(const WStr *in, WStr *out) { msg_expand_names(in, out, resolve_name); }

/* ---- open / close (FUN_005e2d10 / FUN_005e2d80) and typing ---------------------------------------------------- */
static void win_open(Mwnd *w)
{
    if (w->open) return;
    w->open = true;
    w->close_anim = -1;
    w->open_anim = (!g_no_anime && w->cfg.open_type != 0 && w->cfg.open_time > 0) ? 0 : -1;
}

static void win_close(Mwnd *w)
{
    if (!w->open) return;
    w->open = false;
    w->open_anim = -1;
    w->key_wait = false;
    w->close_anim = (!g_no_anime && w->cfg.close_type != 0 && w->cfg.close_time > 0) ? 0 : -1;
}

/* FUN_00570d50: ms per character. */
static int char_wait(void)
{
    if (g_speed_nowait) return 0;
    return sys_state()->msg_speed < 0 ? 0 : sys_state()->msg_speed;
}

bool mwnd_has_unrevealed(const Mwnd *w) { return w->msg.shown < w->msg.ng; }

/* FUN_005e2870: auto-open, then reveal glyphs by the window clock (not while the open animation runs). */
void mwnd_display_update(Mwnd *w)
{
    if (!w->printing) return;
    win_open(w);
    if (w->open_anim < 0) {
        Msg *m = &w->msg;
        if (!m->started) { m->started = true; m->t = w->clock; }
        while (m->shown < m->ng) {                  /* bounded by the glyph count */
            int wait = char_wait();
            if (w->clock - m->t - wait < 0) break;
            m->t += wait;
            m->shown++;
        }
    }
    if (!mwnd_has_unrevealed(w)) w->printing = false;
}

/* FUN_005e2ac0: finish the open animation and reveal everything. */
void mwnd_finish_printing(Mwnd *w)
{
    if (!w->printing) return;
    win_open(w);
    w->open_anim = -1;
    w->msg.started = true;
    w->msg.shown = w->msg.ng;
    w->printing = false;
}

bool mwnd_any_printing(void)
{
    for (int i = 0; i < MWND_CNT; i++)
        if (g_mwnd[i].printing && mwnd_has_unrevealed(&g_mwnd[i])) return true;
    return false;
}

static void finish_all(void)
{
    for (int i = 0; i < MWND_CNT; i++) mwnd_finish_printing(&g_mwnd[i]);
}

static void anim_step(int32_t *anim, int32_t dt, int duration)
{
    if (*anim >= 0 && (*anim += dt) >= duration) *anim = -1;
}

void mwnd_frame(int32_t game_dt, int32_t real_dt, bool screen_open)
{
    g_wheel_used = false;
    for (int i = 0; i < MWND_CNT; i++) {
        Mwnd *w = &g_mwnd[i];
        w->clock += game_dt;
        w->icon_time += real_dt;
        anim_step(&w->open_anim, game_dt, w->cfg.open_time);
        anim_step(&w->close_anim, game_dt, w->cfg.close_time);
        mwnd_display_update(w);
    }
    sel_frame(screen_open);
}

/* ---- procs -------------------------------------------------------------------------------------------------- */
static void push_proc(int type, bool push_result) { proc_push_wait(type, 0, false, push_result); }

static void push_defer(const Mwnd *w, int sub)
{
    Proc p = { .type = PROC_DEFER };
    p.chain[0] = MSG_DEFER_ROOT; p.chain[1] = sub; p.chain[2] = w->no;
    p.nchain = 3;
    proc_push(&p);
}

/* Key wait after the text is out: run order typing -> key wait -> the deferred sub-commands pushed before. */
static void push_key_wait(void)
{
    push_proc(PROC_KEY_WAIT, false);
    push_proc(PROC_MSG_WAIT, false);
}

static bool click_or_wheel(void)
{
    if (proc_wait_click()) return true;
    if (g_wheel_next && !g_wheel_used && input_wheel() < 0) { g_wheel_used = true; return true; }
    return false;
}

static bool proc_sel_wait(Proc *p) { (void)p; return false; }   /* resolved by sel_frame() */

static bool proc_open_wait(Proc *p)       /* FUN_005a10e0 */
{
    Mwnd *w = mwnd_cur();
    if (proc_wait_skip(p)) w->open_anim = -1;
    if (w->open_anim >= 0) return false;
    proc_pop();
    return true;
}

static bool proc_close_wait(Proc *p)      /* FUN_005a1170 */
{
    Mwnd *w = mwnd_cur();
    if (proc_wait_skip(p)) w->close_anim = -1;
    if (w->close_anim >= 0) return false;
    proc_pop();
    return true;
}

/* FUN_005a1200: wait until no window is printing; click/skip/wheel-down finishes the text at once. */
static bool proc_msg_wait(Proc *p)
{
    bool finish = false;
    if (g_user_hidden) { if (proc_wait_click()) g_user_hidden = false; }
    else finish = click_or_wheel();
    if (mwnd_any_printing()) {
        if (finish || proc_wait_skip(p)) finish_all();
        if (mwnd_any_printing()) return false;
    }
    proc_pop();
    return true;
}

/* Auto mode (TXT §6): with a linked voice, wait for it to end plus the minimum; otherwise per revealed glyph. */
static bool auto_mode_done(const Mwnd *w, Proc *p)
{
    const SysState *ss = sys_state();
    if (w->koe_no >= 0 && w->koe_linked) {
        if (koe_playing()) { p->iarg[1] = w->clock; return false; }
        return w->clock - p->iarg[1] > ss->auto_min_wait;
    }
    int n = w->msg.shown - w->glyph_mark;
    if (n < 0) n = w->msg.shown;
    return w->clock - p->end_time > ss->auto_moji_wait * n + ss->auto_min_wait;
}

/* Skip, click, wheel or auto mode ends the key wait; while the window is hidden a click only brings it back. */
static bool key_wait_done(const Mwnd *w, Proc *p)
{
    if (proc_wait_skip(p)) return true;
    if (g_user_hidden) {
        if (proc_wait_click()) g_user_hidden = false;
        return false;
    }
    if (click_or_wheel()) return true;
    return sys_state()->auto_mode && auto_mode_done(w, p);
}

/* FUN_005a15a0 + FUN_005a1840: key wait with icon (TXT §6). */
static bool proc_key_wait(Proc *p)
{
    Mwnd *w = mwnd_cur();
    if (!p->iarg[0]) { p->iarg[0] = 1; p->end_time = w->clock; p->iarg[1] = w->clock; }
    if (!key_wait_done(w, p)) { w->key_wait = true; return false; }
    w->key_wait = false;
    w->glyph_mark = w->msg.shown;           /* FUN_005a17e0: mwnd+0x15c = msg+0x108 */
    proc_pop();
    return true;
}

/* ---- commands ----------------------------------------------------------------------------------------------- */
static void stop_voice(void)
{
    if (!g_koe_dont_stop) koe_stop(KOE_FADE_MS);
}

static void preview_add(const WStr *s)
{
    for (int i = 0; i < s->len && g_preview.len < PREVIEW_MAX; i++) ws_push(&g_preview, s->p[i]);
}

/* Tests (run_en.sh): the pen end of every new glyph against the window limit. */
static void trace_layout(const Mwnd *w, int ng0, int carried)
{
    if (!log_trace_on("LAYOUT")) return;
    int max_end = 0;
    for (int k = ng0; k < w->msg.ng; k++) {
        const MGlyph *g = &w->msg.g[k];
        int e = g->x + msg_char_width(g->code, g->size) + w->msg.space_x;
        if (e > max_end) max_end = e;
    }
    log_trace("LAYOUT", "mwnd=%d glyphs=%d max_end=%d limit=%d y=%d limit_y=%d carried=%d", w->no, w->msg.ng - ng0,
              max_end, w->msg.limit_x, w->msg.y, w->msg.limit_y, carried);
}

/* FUN_00598a60: auto-page. Run order: typing -> key wait -> clear -> msg_block -> print the rest. */
static void push_overflow(Mwnd *w, const WStr *s, int used, int scene, int rf)
{
    log_msg(LOG_INFO, "mwnd: message overflow after TEXT %" PRId64 " (%d units carried to the next page)",
            vm_text_count(), s->len - used);
    ws_set(&w->remainder, s->p + used, s->len - used);
    w->rem_scene = scene; w->rem_rf = rf;
    w->page_icon = w->cfg.novel_mode == 1;
    push_defer(w, SUB_REMAINDER);
    push_defer(w, SUB_MSG_BLOCK);
    push_defer(w, SUB_CLEAR);
    push_key_wait();
}

/* FUN_005986b0: print. Returns after pushing proc 0x19 (or the overflow chain). */
static void cmd_print(Mwnd *w, const WStr *raw, int scene, int rf, bool to_backlog, bool expand)
{
    sys_readskip_check(scene, rf);
    WStr *s = &g_print;
    if (expand) expand_names(raw, s); else ws_copy(s, raw);
    preview_add(s);
    int ng0 = w->msg.ng;
    int used = msg_layout(&w->msg, s->p, s->len);
    trace_layout(w, ng0, s->len - used);
    if (used > 0) mwnd_rp_add(w, RP_TEXT, 0, s->p, used);
    rf_add(w, scene, rf);
    if (to_backlog) backlog_add_text(s, scene, vm_line());
    w->printing = true;
    mwnd_display_update(w);
    if (used < s->len) push_overflow(w, s, used, scene, rf);
    else push_proc(PROC_MSG_WAIT, false);
}

/* mwnd 0x31, FUN_00597dc0 */
static void cmd_msg_block(Mwnd *w)
{
    if (w->in_block) return;
    if (w->clear_pending || (w->cfg.overflow_check >= 1 && !msg_fits_lines(&w->msg, 1))) page_clear(w);
    /* FUN_005e3dd0 / FUN_005e3660: per-message state */
    w->msg.moji = w->msg.fuchi = w->msg.shadow = -1;
    w->koe_no = -1; w->koe_linked = false;
    w->msg.started = false;
    w->msg.t = w->clock;
    input_clear();                           /* FUN_0054b4c0: drop decide edges */
    w->in_block = true;
    sav_snapshot();                          /* FUN_00517e40: local save point */
}

/* mwnd 3, FUN_00599c30: deferred clear */
static void cmd_clear(Mwnd *w)
{
    w->clear_pending = true;
    commit_rf(w);
    stop_voice();
    w->in_block = false;
}

/* mwnd 0x37, FUN_00599cf0: newline-continue */
static void cmd_newline_continue(Mwnd *w)
{
    commit_rf(w);
    stop_voice();
    w->in_block = false;
    msg_newline(&w->msg);
    mwnd_rp_add(w, RP_NL, 0, NULL, 0);
}

/* mwnd 0x14, FUN_0059a310 */
static void cmd_pp(Mwnd *w)
{
    int next = SUB_CLEAR;
    if (w->cfg.novel_mode == 1) {
        bool full = w->cfg.overflow_check > 0 && !msg_fits_lines(&w->msg, 1);
        next = full ? SUB_CLEAR : SUB_NL_CONTINUE;
        w->page_icon = full;
    }
    push_defer(w, next);
    push_key_wait();
}

/* mwnd 0x36, FUN_0059a5b0 */
static void cmd_page(Mwnd *w)
{
    w->page_icon = w->cfg.novel_mode == 1;
    push_defer(w, SUB_CLEAR);
    push_key_wait();
}

/* mwnd 2/0xd/0xe, FUN_00599d80 */
static void cmd_close(Mwnd *w, bool wait)
{
    if (!w->open) return;
    commit_rf(w);
    win_close(w);
    if (wait) push_proc(PROC_MWND_CLOSE, false);
}

/* mwnd 6, FUN_00599230 */
static void cmd_nl(Mwnd *w)
{
    msg_newline(&w->msg);
    mwnd_rp_add(w, RP_NL, 0, NULL, 0);
    if (w->cfg.back_insert_nl) backlog_add_newline(vm_scene(), vm_line());
}

/* mwnd 7: size([n]) (§1.4.2) */
static void cmd_size(Mwnd *w, const ElmCall *c)
{
    msg_set_size(&w->msg, c && c->al_id == 1 && c->npos >= 1 ? elm_arg_int(c, 0, -1) : -1);
    mwnd_rp_add(w, RP_SIZE, w->msg.size, NULL, 0);
}

/* mwnd 0: set_waku([waku[, name_waku]]) (§1.4.1) */
static void cmd_set_waku(Mwnd *w, const ElmCall *c)
{
    if (!c || c->al_id == 0 || c->npos < 1) set_waku(w, WAKU_FROM_CFG, WAKU_FROM_CFG);
    else if (c->al_id == 1 || c->npos < 2) set_waku(w, elm_arg_int(c, 0, WAKU_FROM_CFG), WAKU_FROM_CFG);
    else set_waku(w, elm_arg_int(c, 0, WAKU_FROM_CFG), elm_arg_int(c, 1, WAKU_FROM_CFG));
}

/* mwnd 9, FUN_00597ea0: koe(no[, chara]) + inline read flag */
static void cmd_koe(Mwnd *w, const ElmCall *c)
{
    int koe_no = elm_arg_int(c, 0, 0);
    int chara = (c->al_id == 1 && c->npos > 1) ? elm_arg_int(c, 1, -1) : -1;
    int rf = vm_read_inline_i32();
    int scene = vm_scene();
    sys_readskip_check(scene, rf);
    koe_play(koe_no, chara, KOE_FADE_MS);
    w->koe_no = koe_no; w->koe_linked = true;
    rf_add(w, scene, rf);
    backlog_add_koe(koe_no, chara, scene, vm_line());
}

/* The choice arguments: each string is an item, the ints after it are its cond, then its type (meaning [U]). */
static void sel_add_args(Mwnd *w, const ElmCall *c)
{
    int nints = 0;
    for (int k = 0; k < c->npos; k++) {
        const Arg *a = &c->arg[k];
        if (a->form == ELM_FORM_STR) {
            WStr t = {0};
            expand_names(&a->s, &t);
            sel_add_item(w, &t, 1, 0);
            ws_free(&t);
            nints = 0;
        } else if (w->nsel > 0) {
            if (nints == 0) w->sel[w->nsel - 1].cond = a->i;
            else if (nints == 1) w->sel[w->nsel - 1].type = a->i;
            nints++;
        }
    }
}

/* mwnd 5/0x32–0x34, FUN_0065c1d0 case 0x32 -> FUN_00599530 */
static void cmd_sel(Mwnd *w, const ElmCall *c, bool cancel)
{
    int rf = vm_read_inline_i32();
    int scene = vm_scene();
    if (w->clear_pending) page_clear(w);
    sel_reset(w);
    if (w->msg.x != 0) { msg_newline(&w->msg); mwnd_rp_add(w, RP_NL, 0, NULL, 0); }   /* FUN_005e3980 */
    w->sel_x = w->msg.x; w->sel_y = w->msg.y;
    sel_add_args(w, c);
    w->sel_cancel = cancel;
    w->sel_active = w->nsel > 0;
    w->sel_hover = -1;
    rf_add(w, scene, rf);
    sys_readskip_check(scene, rf);
    w->printing = true;
    mwnd_display_update(w);
    push_defer(w, w->cfg.novel_mode == 1 ? SUB_NL_CONTINUE : SUB_CLEAR);
    push_proc(PROC_SEL, true);
}

/* mwnd 4: print-with-flag (unused by the game) */
static void cmd_print_flag(Mwnd *w, const ElmCall *c)
{
    WStr s = {0};
    ws_copy(&s, elm_arg_str(c, 0));
    int rf = vm_read_inline_i32();
    cmd_print(w, &s, vm_scene(), rf, true, true);
    ws_free(&s);
}

/* mwnd 0x39: print the overflow remainder */
static void cmd_remainder(Mwnd *w)
{
    WStr s = {0};
    ws_copy(&s, &w->remainder);
    ws_set(&w->remainder, NULL, 0);
    cmd_print(w, &s, w->rem_scene, w->rem_rf, false, false);
    ws_free(&s);
}

static void unimplemented_sub(int sub, ElmCall *c)
{
    static bool logged[SUB_LOG_MAX];
    if (sub >= 0 && sub < SUB_LOG_MAX && !logged[sub]) {
        logged[sub] = true;
        log_msg(LOG_DEBUG, "mwnd: sub 0x%x not implemented (unused by the game)", sub);
    }
    if (c) elm_push_default(c);
}

void mwnd_sub(Mwnd *w, int sub, ElmCall *c)
{
    switch (sub) {
    case SUB_SET_WAKU:      cmd_set_waku(w, c); break;
    case SUB_OPEN_WAIT: case SUB_OPEN_WAIT2: win_open(w); push_proc(PROC_MWND_OPEN, false); break;
    case SUB_OPEN_NOWAIT:   win_open(w); break;
    case SUB_CLOSE_WAIT: case SUB_CLOSE_WAIT2: cmd_close(w, true); break;
    case SUB_CLOSE_NOWAIT:  cmd_close(w, false); break;
    case SUB_CLEAR:         cmd_clear(w); break;
    case SUB_PRINT_FLAG:    cmd_print_flag(w, c); break;
    case SUB_SEL_OLD: case SUB_SEL: cmd_sel(w, c, false); break;
    case SUB_SEL_CANCEL: case SUB_SEL_CANCEL2: cmd_sel(w, c, true); break;
    case SUB_NL:            cmd_nl(w); break;
    case SUB_SIZE:          cmd_size(w, c); break;
    case SUB_KOE:           cmd_koe(w, c); break;
    case SUB_MSG_WAIT:      push_proc(PROC_MSG_WAIT, false); break;
    case SUB_PP:            cmd_pp(w); break;
    case SUB_MSG_BLOCK:     cmd_msg_block(w); break;
    case SUB_PAGE:          cmd_page(w); break;
    case SUB_NL_CONTINUE:   cmd_newline_continue(w); break;
    case SUB_REMAINDER:     cmd_remainder(w); break;
    case SUB_IS_OPEN:       vm_push_int(w->open ? 1 : 0); break;
    default:                unimplemented_sub(sub, c); break;
    }
}

/* TEXT opcode body (TXT §3.2). */
void mwnd_print_text(const WStr *s, int scene, int rf) { cmd_print(mwnd_cur(), s, scene, rf, true, true); }

/* 【name】 when the window config asks for brackets. */
static void bracket_name(WStr *disp)
{
    WStr b = {0};
    ws_push(&b, 0x3010);
    ws_cat(&b, disp);
    ws_push(&b, 0x3011);
    ws_copy(disp, &b);
    ws_free(&b);
}

/* NAME opcode body (TXT §3.3, FUN_00598350). */
void mwnd_set_name(const WStr *raw)
{
    Mwnd *w = mwnd_cur();
    ws_copy(&w->name, raw);
    WStr disp = {0};
    expand_names(raw, &disp);
    int mode = w->cfg.name_disp_mode;
    if (w->cfg.novel_mode == 1 && mode == 0) mode = 1;
    if (w->cfg.name_bracket == 1) bracket_name(&disp);
    if (mode == 1) {                         /* name written into the message text (unused by this game) */
        int used = msg_layout(&w->msg, disp.p, disp.len);
        if (used > 0) mwnd_rp_add(w, RP_TEXT, 0, disp.p, used);
    } else if (mode == 0) {
        unimplemented_sub(SUB_NAME_WINDOW, NULL);   /* separate name window: unused (mwnd 2 has mode 2) */
    }
    if (mode != 2) preview_add(&disp);
    backlog_add_name(raw, vm_scene(), vm_line());
    backlog_set_disp_name(&disp);
    ws_free(&disp);
}

/* ---- save / load (ENGINE_SPEC §2.11: page replay log) --------------------------------------------------------- */
static void save_window(SavWriter *wr, const Mwnd *w)
{
    sav_put_u8(wr, w->open); sav_put_u8(wr, w->in_block); sav_put_u8(wr, w->clear_pending);
    sav_put_i32(wr, w->waku); sav_put_i32(wr, w->name_waku);
    sav_put_i32(wr, w->koe_no); sav_put_u8(wr, w->koe_linked);
    sav_put_i32(wr, w->msg.size);
    sav_put_wstr(wr, &w->name);
    sav_put_i32(wr, w->nrf);
    for (int k = 0; k < w->nrf; k++) sav_put_i32(wr, w->rf_pend[k]);
    sav_put_i32(wr, w->nrp);
    for (int k = 0; k < w->nrp; k++) {
        sav_put_i32(wr, w->rp[k].op); sav_put_i32(wr, w->rp[k].a); sav_put_wstr(wr, &w->rp[k].s);
    }
}

void mwnd_save(SavWriter *wr)
{
    sav_put_chunk(wr, FOURCC('M', 'W', 'N', 'D'));
    sav_put_i32(wr, MWND_SAVE_VERSION);
    sav_put_i32(wr, g_cur);
    sav_put_wstr(wr, &g_preview);
    for (int i = 0; i < MWND_CNT; i++) save_window(wr, &g_mwnd[i]);
}

void mwnd_replay(Mwnd *w)
{
    for (int k = 0; k < w->nrp; k++) {
        const ReplayOp *o = &w->rp[k];
        if (o->op == RP_TEXT) (void)msg_layout(&w->msg, o->s.p, o->s.len);   /* a saved page fits by construction */
        else if (o->op == RP_NL) msg_newline(&w->msg);
        else if (o->op == RP_SIZE) msg_set_size(&w->msg, o->a);
    }
    w->msg.shown = w->msg.ng;
}

static bool load_size_ok(int size) { return size >= -1 && size <= MWND_LOAD_SIZE_MAX; }

static void load_read_flags(SavReader *r, Mwnd *w)
{
    int nrf = sav_get_count(r, MWND_LOAD_COUNT_MAX, 4);
    for (int k = 0; k + 1 < nrf; k += 2) {
        int scene = sav_get_i32(r);
        rf_add(w, scene, sav_get_i32(r));
    }
    if (nrf & 1) (void)sav_get_i32(r);       /* odd count: the stray value is skipped, as before */
}

static void load_replay(SavReader *r, Mwnd *w)
{
    int nrp = sav_get_count(r, MWND_LOAD_COUNT_MAX, 12);
    mwnd_rp_clear(w);
    for (int k = 0; k < nrp && sav_ok(r); k++) {
        int op = sav_get_i32(r), a = sav_get_i32(r);
        WStr s = {0};
        (void)sav_get_wstr(r, &s);           /* failure is sticky: checked below */
        bool bad = op < RP_TEXT || op > RP_NAME || (op == RP_SIZE && !load_size_ok(a));
        if (bad) sav_fail(r);
        else if (sav_ok(r)) mwnd_rp_add(w, op, a, s.p, s.len);
        ws_free(&s);
    }
}

static bool load_window(SavReader *r, Mwnd *w)
{
    w->open = sav_get_u8(r); w->in_block = sav_get_u8(r); w->clear_pending = sav_get_u8(r);
    int waku = sav_get_i32(r), name_waku = sav_get_i32(r);
    set_waku(w, waku, name_waku);
    w->koe_no = sav_get_i32(r); w->koe_linked = sav_get_u8(r);
    int size = sav_get_i32(r);
    if (!load_size_ok(size)) { sav_fail(r); return false; }   /* hostile sizes overflow the layout */
    (void)sav_get_wstr(r, &w->name);         /* failure is sticky: checked below */
    load_read_flags(r, w);
    load_replay(r, w);
    if (!sav_ok(r)) return false;
    mwnd_replay(w);
    msg_set_size(&w->msg, size);
    return true;
}

bool mwnd_load(SavReader *r)
{
    if (!sav_find_chunk(r, FOURCC('M', 'W', 'N', 'D')) || sav_get_i32(r) != MWND_SAVE_VERSION) return false;
    mwnd_reset_all();
    int cur = sav_get_i32(r);
    if (cur >= 0 && cur < MWND_CNT) g_cur = cur;
    (void)sav_get_wstr(r, &g_preview);       /* failure is sticky: checked below */
    for (int i = 0; i < MWND_CNT && sav_ok(r); i++)
        if (!load_window(r, &g_mwnd[i])) break;
    return sav_ok(r);
}
