/* ui/ui_test.c — ui_selftest (ENGINE_SPEC §5 WP6 self-check): the SYS §3.1 edge table, the objbtngroup flow,
 * the editbox, and the syscom menu / confirm / config screens driven by synthetic clicks (with the built-in
 * string fallbacks when Gameexe is absent). */
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "core/log.h"
#include "core/str.h"
#include "sav/sav.h"
#include "plat/input.h"
#include "gfx/button.h"
#include "vm/proc.h"
#include "vm/sysstate.h"
#include <SDL3/SDL_scancode.h>
#include <string.h>

static int g_fail;
#define CHECK(cond) \
    do { if (!(cond)) { g_fail++; log_msg(LOG_ERR, "ui_selftest: %s:%d: %s", __FILE__, __LINE__, #cond); } } while (0)

static void expect_btn(const InputBtn *b, int is_down, int on_down, int on_up, int down_up, int line)
{
    if (b->is_down != is_down || b->on_down != on_down || b->on_up != on_up || b->down_up != down_up) {
        g_fail++;
        log_msg(LOG_ERR, "ui_selftest: line %d: got {%d,%d,%d,%d} want {%d,%d,%d,%d}", line,
                b->is_down, b->on_down, b->on_up, b->down_up, is_down, on_down, on_up, down_up);
    }
}
#define EXPECT_L(d, od, ou, du) expect_btn(input_btn_mouse(0), d, od, ou, du, __LINE__)

static void click_raw(int which) { input_raw_button(which, true); input_raw_button(which, false); }
static void tap_key(int sc) { input_raw_key(sc, true); input_raw_key(sc, false); }

/* SYS §3.1 table: press -> {1,1,0,1}; hold -> {1,0,0,1}; release -> {0,0,1,2}; idle -> {0,0,0,0} */
static void test_edge_table(void)
{
    input_raw_button(0, true);  input_frame(); EXPECT_L(1, 1, 0, 1);
    CHECK(input_query(VK_DECIDE, 1) == 1 && input_query(VK_DECIDE, 5) == 0);
    CHECK(input_query(VK_DECIDE, 6) == 1 && input_query(VK_DECIDE, 7) == 0 && input_query(VK_DECIDE, 4) == 0);
    input_frame(); EXPECT_L(1, 0, 0, 1);
    input_raw_button(0, false); input_frame(); EXPECT_L(0, 0, 1, 2);
    CHECK(input_query(VK_DECIDE, 4) == 1 && input_query(VK_DECIDE, 5) == 1 && input_query(VK_DECIDE, 7) == 1);
    CHECK(input_decide_clicked() && !input_cancel_clicked());
    input_frame(); EXPECT_L(0, 0, 0, 0);
    CHECK(!input_decide_clicked() && input_query(VK_DECIDE, 7) == 1);
    /* press + release inside one frame: press applied first, then release (FUN_006a6d20) */
    click_raw(0); input_frame(); EXPECT_L(0, 1, 1, 2);
    input_frame(); EXPECT_L(0, 0, 0, 0);
    /* held, then release + re-press in one frame */
    input_raw_button(0, true); input_frame(); input_frame(); EXPECT_L(1, 0, 0, 1);
    input_raw_button(0, false); input_raw_button(0, true); input_frame(); EXPECT_L(1, 1, 1, 2);
    input_raw_button(0, false); input_frame(); input_frame(); EXPECT_L(0, 0, 0, 0);
    /* 86.2 clear: edges (current and queued) go, is_down stays */
    input_raw_button(0, true); input_frame(); input_raw_button(0, false);
    input_clear(); EXPECT_L(1, 0, 0, 0);
    input_frame(); EXPECT_L(1, 0, 0, 0);                 /* the queued release was dropped too */
    input_raw_button(0, false); input_frame(); EXPECT_L(0, 0, 1, 0);   /* down_up was 0, so no click */
    input_frame();
    /* 86.3 next: applies queued events now */
    click_raw(0); input_next(); EXPECT_L(0, 1, 1, 2);
    input_next(); EXPECT_L(0, 0, 0, 0);
    /* consume: one click satisfies one wait */
    click_raw(0); input_frame();
    CHECK(input_decide_clicked()); input_consume_decide(); CHECK(!input_decide_clicked());
    CHECK(input_query(VK_DECIDE, 1) == 1);               /* only down_up is used up */
    input_frame();
    /* a menu frame consumes its edges every frame (ui_frame): a press held into the next frame stays armed,
     * so the release still clicks (regression: menus ignored every real click) */
    input_raw_button(0, true); input_frame(); input_consume_edges(); EXPECT_L(1, 0, 0, 1);
    input_raw_button(0, false); input_frame(); EXPECT_L(0, 0, 1, 2);
    CHECK(input_decide_clicked());
    input_consume_edges(); EXPECT_L(0, 0, 0, 0);
    input_frame();
    /* Escape toggles the menu on key down; its release must not cancel/toggle again (regression: a second
     * Escape closed the menu on press and reopened it on release) */
    input_raw_key(SDL_SCANCODE_ESCAPE, true); input_frame();
    CHECK(input_take_menu_toggle()); CHECK(!input_take_menu_toggle());
    input_consume_edges();
    input_raw_key(SDL_SCANCODE_ESCAPE, false); input_frame();
    CHECK(!input_take_menu_toggle() && !input_cancel_clicked());
    input_frame();
    /* right click toggles on release (as the original) and is used up */
    input_raw_button(1, true); input_frame(); CHECK(!input_take_menu_toggle());
    input_raw_button(1, false); input_frame(); CHECK(input_take_menu_toggle() && !input_cancel_clicked());
    input_frame();
}

/* Keyboard members of DECIDE / CANCEL; auto-repeat makes no new edge; Ctrl, mouse and wheel latch per frame. */
static void test_keys(void)
{
    input_raw_key(SDL_SCANCODE_RETURN, true); input_frame();
    CHECK(input_query(VK_DECIDE, 1) == 1 && input_query(VK_DECIDE, 6) == 1);
    input_raw_key(SDL_SCANCODE_RETURN, true); input_frame();
    CHECK(input_query(VK_DECIDE, 1) == 0 && input_query(VK_DECIDE, 6) == 1);
    input_raw_key(SDL_SCANCODE_RETURN, false); input_frame();
    CHECK(input_decide_clicked());
    input_frame();
    tap_key(SDL_SCANCODE_SPACE); input_frame();
    CHECK(input_decide_clicked());
    click_raw(1); input_frame();
    CHECK(input_cancel_clicked() && !input_decide_clicked() && input_query_mouse(1, 5) == 1);
    input_consume_cancel(); CHECK(!input_cancel_clicked());
    tap_key(SDL_SCANCODE_ESCAPE); input_frame();
    CHECK(input_cancel_clicked());
    input_frame();
    input_raw_key(SDL_SCANCODE_LCTRL, true); CHECK(!input_ctrl_held()); input_frame(); CHECK(input_ctrl_held());
    input_raw_key(SDL_SCANCODE_LCTRL, false); input_frame(); CHECK(!input_ctrl_held());
    input_raw_mouse(100, 200); CHECK(input_mouse_x() != 100); input_frame();
    CHECK(input_mouse_x() == 100 && input_mouse_y() == 200);
    input_raw_wheel(1); input_raw_wheel(2); input_frame(); CHECK(input_wheel() == 3);
    input_frame(); CHECK(input_wheel() == 0);
    /* out-of-range ids are ignored, not stored */
    input_raw_key(-1, true); input_raw_key(1 << 20, true); input_raw_button(7, true); input_frame();
    CHECK(input_query_key(-1, 6) == 0 && input_query(0x7fff, 6) == 0);
}

static void test_edges(void)
{
    input_reset();
    test_edge_table();
    test_keys();
    input_reset();
}

/* ---- objbtngroup with synthetic mouse ------------------------------------------------------------------------- */
static Obj g_tobj[3];
static Rect g_trect[3];
static int g_tdone_val, g_tdone_cnt;
static Obj *t_src(int i) { return i >= 0 && i < 3 ? &g_tobj[i] : NULL; }
static bool t_hit(const Obj *o, int x, int y)
{
    int k = (int)(o - g_tobj);
    if (k < 0 || k >= 3) return false;
    Rect r = g_trect[k];
    return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
}
static void t_done(int v) { g_tdone_val = v; g_tdone_cnt++; }

static void t_frame(int mx, int my, int press /*1 down, 0 up, -1 none, 2 click*/)
{
    input_raw_mouse(mx, my);
    if (press == 1 || press == 2) input_raw_button(0, true);
    if (press == 0 || press == 2) input_raw_button(0, false);
    input_frame();
    btn_frame();
}

static BtnGroup *buttons_setup(void)
{
    input_reset();
    memset(g_tobj, 0, sizeof g_tobj);
    for (int i = 0; i < 3; i++) {
        g_tobj[i].stage = 1; g_tobj[i].idx = 20 + i; g_tobj[i].type = 2; g_tobj[i].disp = true;
    }
    g_trect[0] = (Rect){ 10, 10, 50, 30 };
    g_trect[1] = (Rect){ 10, 50, 50, 70 };
    g_trect[2] = (Rect){ 10, 90, 50, 110 };
    btn_set(&g_tobj[0], 0, 0, 8, 0);       /* _menu $name_input: set_button(n, 0, 8, 0) */
    btn_set(&g_tobj[1], 1, 0, 8, 0);
    btn_set(&g_tobj[2], 2, 0, 9, 0);
    btn_test_hooks(t_src, t_hit, t_done);
    btn_reset_all();
    BtnGroup *g = btn_group_get(1, 0);
    CHECK(g && btn_group_get(1, BTNGROUP_CNT) == NULL && btn_group_get(3, 0) == NULL);
    return g;
}

/* seen021 flow: start (non-blocking), poll hit/pushed/decided */
static void test_buttons_poll(BtnGroup *g)
{
    btn_group_init(g);                               /* init only: the group is inactive, so nothing is hit */
    t_frame(20, 20, -1);
    CHECK(g->hit_no == -1 && g_tobj[0].btn_state == BTN_NORMAL && g->decided == -2);
    btn_group_start(g);
    CHECK(g->active && !g->wait && g->decided == -2);
    t_frame(20, 20, -1);
    CHECK(g->hit_no == 0 && g->pushed_no == -1 && g_tobj[0].btn_state == BTN_HIT && g_tobj[1].btn_state == BTN_NORMAL);
    int pa, dx, dy, tr, br, dk;
    btn_visual(&g_tobj[0], &pa, &dx, &dy, &tr, &br, &dk);
    CHECK(pa == 1 && tr == 255);                    /* #BUTTON.ACTION.008.HIT = 1,0,0,255,0,0 */
    t_frame(300, 300, -1);
    CHECK(g->hit_no == -1 && g->pushed_no == -1 && g_tobj[0].btn_state == BTN_NORMAL);
    t_frame(20, 60, 1);                              /* press on button 1 */
    CHECK(g->pushed_no == 1 && g->hit_no == -1 && g_tobj[1].btn_state == BTN_PUSH);
    btn_visual(&g_tobj[1], &pa, &dx, &dy, &tr, &br, &dk);
    CHECK(pa == 2);
    t_frame(300, 300, -1);                           /* drag off while held */
    CHECK(g->pushed_no == -1 && g->hit_no == -1 && g_tobj[1].btn_state == BTN_NORMAL);
    t_frame(20, 60, -1);                             /* back on it */
    CHECK(g->pushed_no == 1);
    t_frame(20, 60, 0);                              /* release -> decide */
    CHECK(g->decided == 1 && !g->active && g->hit_no == -1 && g->pushed_no == -1 && g->result_no == 1);
    CHECK(!input_decide_clicked());                  /* the decide used up the click */
    CHECK(g_tdone_cnt == 0);                         /* start does not wait */
    t_frame(20, 20, -1);
    CHECK(g->decided == 1 && g->hit_no == -1);       /* inactive after the decide */
}

static void test_buttons_misses(BtnGroup *g)
{
    btn_group_start(g);                              /* press outside, release on a button: no decide */
    t_frame(300, 300, 1);
    t_frame(20, 20, 0);
    CHECK(g->decided == -2 && g->active);
    t_frame(20, 20, 1);                              /* press on 0, release on 1: no decide */
    t_frame(20, 60, 0);
    CHECK(g->decided == -2);
    g_tobj[0].disp = false;                          /* invisible objects are not hit */
    t_frame(20, 20, -1);
    CHECK(g->hit_no == -1);
    g_tobj[0].disp = true;
    t_frame(20, 100, -1);                            /* button 2 uses action 9 (pat + 10) */
    CHECK(g->hit_no == 2);
    int pa;
    btn_visual(&g_tobj[2], &pa, NULL, NULL, NULL, NULL, NULL);
    CHECK(pa == 10);
}

/* $name_input flow: sel (blocking) -> proc 0x10 resolves with the button number; sel_cancel: right click -> -1 */
static void test_buttons_sel(BtnGroup *g)
{
    btn_group_init(g);
    btn_group_sel(1, 0, false, -1);
    CHECK(g->active && g->wait && !g->cancel_ok && g->decided == -2);
    Proc p = { .type = PROC_BTNSEL };
    p.iarg[0] = 1; p.iarg[1] = 0; p.push_result = 1;
    CHECK(!btn_proc_sel(&p));
    t_frame(300, 300, -1);
    click_raw(1); input_frame();
    CHECK(!btn_proc_sel(&p));                         /* no cancel without sel_cancel */
    t_frame(20, 20, 2);                              /* click inside one frame */
    CHECK(g_tdone_cnt == 1 && g_tdone_val == 0 && g->decided == 0 && !g->active);
    btn_group_init(g);
    btn_group_sel(1, 0, true, -1);
    CHECK(g->cancel_ok);
    CHECK(!btn_proc_sel(&p));
    click_raw(1); input_frame();
    CHECK(btn_proc_sel(&p));
    CHECK(g_tdone_cnt == 2 && g_tdone_val == -1 && g->decided == -2 && !g->active);
    CHECK(!input_cancel_clicked());
    btn_group_start(g);
    btn_group_end(g);
    CHECK(!g->active && g->decided == -2);
}

static void test_buttons(void)
{
    g_tdone_cnt = 0;
    BtnGroup *g = buttons_setup();
    if (g) {
        test_buttons_poll(g);
        test_buttons_misses(g);
        test_buttons_sel(g);
    }
    btn_test_hooks(NULL, NULL, NULL);
    btn_reset_all();
    input_reset();
}

/* ---- editbox --------------------------------------------------------------------------------------------------- */
static void test_editbox(void)
{
    input_reset();
    ui_editbox_create(0, (Rect){ 164, 200, 164 + 144, 224 }, 24);
    ui_editbox_create(1, (Rect){ 332, 200, 332 + 144, 224 }, 24);
    WStr w = { 0 }, out = { 0 };
    ws_set_utf8(&w, "人見");
    ui_editbox_set_text(0, &w);
    ui_editbox_set_focus(0);
    CHECK(ui_editbox_has_focus());
    input_raw_text("ab");
    tap_key(SDL_SCANCODE_BACKSPACE);
    input_frame();
    CHECK(ui_editbox_get_text(0, &out));
    static const uint16_t WANT[] = { 0x4EBA, 0x898B, 'a' };
    CHECK(out.len == (w.len == 2 ? 3 : 1) && (w.len != 2 || memcmp(out.p, WANT, sizeof WANT) == 0));
    /* Return in a focused box decides it and is not a DECIDE key press */
    tap_key(SDL_SCANCODE_RETURN); input_frame();
    CHECK(ui_editbox_decided(0) && !input_decide_clicked());
    ui_editbox_clear_input(0);
    CHECK(!ui_editbox_decided(0));
    /* the capacity holds: 200 more characters leave the box at its limit */
    for (int i = 0; i < 20; i++) input_raw_text("0123456789");
    input_frame();
    CHECK(ui_editbox_get_text(0, &out) && out.len == 128);
    /* clicking box 1 moves the focus */
    input_raw_mouse(340, 210); click_raw(0); input_frame();
    input_raw_text("x"); input_frame();
    CHECK(ui_editbox_get_text(1, &out) && out.len == 1 && out.p[0] == 'x');
    ui_editbox_destroy(0);
    ui_editbox_destroy(1);
    CHECK(!ui_editbox_has_focus() && !ui_editbox_get_text(0, &out));
    ws_free(&w);
    ws_free(&out);
    input_reset();
}

/* ---- syscom menu / confirm / config ------------------------------------------------------------------------------ */
static void t_click(int x, int y, int which)
{
    input_raw_mouse(x, y);
    click_raw(which);
    input_frame();
    ui_frame();
}

static void t_click_rect(Rect r, int which) { t_click((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2, which); }

/* Every item exists when Gameexe is absent; menu rows follow the item order. */
static void test_menu(void)
{
    enum { ROW_AUTO_MODE = 3, ROW_RETURN_SEL = 7, ROW_CONFIG = 8, ITEMS = 12 };
    bool am = sys_state()->auto_mode;
    CHECK(ui_menu_count() == ITEMS);
    if (ui_menu_count() != ITEMS) return;
    ui_open_syscom();
    CHECK(ui_active() && ui_top()->kind == UI_SYSCOM);
    t_click_rect(ui_menu_row(ROW_AUTO_MODE), 0);
    CHECK(!ui_active() && sys_state()->auto_mode == !am);
    sys_state()->auto_mode = am;
    ui_open_syscom();
    t_click_rect(ui_menu_row(ROW_RETURN_SEL), 0);
    CHECK(ui_top() && ui_top()->kind == UI_CONFIRM && ui_top()->action == UIA_RETURN_SEL);
    t_click_rect(ui_confirm_button(1), 0);                    /* いいえ -> back to the menu */
    CHECK(ui_top() && ui_top()->kind == UI_SYSCOM);
    t_click_rect(ui_menu_row(ROW_CONFIG), 0);
    CHECK(ui_top() && ui_top()->kind == UI_CONFIG);
    t_click(10, 10, 1);                                       /* right click closes one level */
    CHECK(ui_top() && ui_top()->kind == UI_SYSCOM);
    t_click(10, 10, 1);
    CHECK(!ui_active());
}

static void test_ui_screens(void)
{
    bool was_inited = ui_test_set_inited(true), locked = sys_state()->syscom_locked;
    input_reset();
    ui_init();
    ui_close_all();
    sys_state()->syscom_locked = false;
    test_menu();
    sys_state()->syscom_locked = true;
    ui_open_syscom();
    CHECK(!ui_active());                                       /* title screen: 63.7 locks the menu */
    ui_open_config();
    bool wheel = sav_config()->wheel_next;
    t_click(100, 56 + 9 * 30 + 15, 0);                         /* config row 9: wheel toggle */
    CHECK(sav_config()->wheel_next == !wheel);
    sav_config()->wheel_next = wheel;
    t_click(10, 10, 1);
    CHECK(!ui_active());
    sys_state()->syscom_locked = locked;
    (void)ui_test_set_inited(was_inited);
    input_reset();
}

int ui_selftest(void)
{
    g_fail = 0;
    ui_set_selftest_running(true);
    ui_input_attach();
    for (int i = 0; i < UI_EDITBOX_CNT; i++) ui_editbox_destroy(i);
    test_edges();
    test_buttons();
    test_editbox();
    test_ui_screens();
    ui_set_selftest_running(false);
    log_msg(g_fail ? LOG_ERR : LOG_INFO, "ui_selftest: %s (%d failure%s)", g_fail ? "FAIL" : "ok", g_fail,
            g_fail == 1 ? "" : "s");
    return g_fail;
}
