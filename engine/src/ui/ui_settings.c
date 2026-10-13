/* ui/ui_settings.c — the config dialog (ENGINE_SPEC §2.12): volumes and speeds as sliders, switches, the text
 * shadow style, fullscreen and the language. Every change goes straight to sav_config() + ui_config_apply().
 *
 * Memory: fixed static labels; nothing is allocated. */
#include "ui/ui_internal.h"
#include "core/str.h"
#include "sav/sav.h"
#include "plat/input.h"
#include "txt/lang.h"
#include <SDL3/SDL.h>
#include <string.h>

enum { CF_VOL_ALL, CF_VOL_BGM, CF_VOL_KOE, CF_VOL_PCM, CF_VOL_SE, CF_MSG_SPEED, CF_AUTO_SPEED, CF_SKIP_UNREAD,
       CF_KOE_DONT_STOP, CF_WHEEL, CF_SHADOW, CF_WINDOW, CF_LANG, CF_BACK, CF_N };
enum { CF_Y0 = 56, CF_ROW = 30, CF_LX = 60, CF_CX0 = 330, CF_CX1 = 580, CF_TEXT = 18, CF_KEY_STEP = 16,
       MSG_MS_MAX = 100, AUTO_MS_MAX = 200 };

static const LangStrId CF_LABEL[CF_N] = {
    LANG_STR_CF_VOL_ALL, LANG_STR_CF_VOL_BGM, LANG_STR_CF_VOL_KOE, LANG_STR_CF_VOL_PCM, LANG_STR_CF_VOL_SE,
    LANG_STR_CF_MSG_SPEED, LANG_STR_CF_AUTO_SPEED, LANG_STR_CF_SKIP_UNREAD, LANG_STR_CF_KOE_DONT_STOP,
    LANG_STR_CF_WHEEL, LANG_STR_CF_SHADOW, LANG_STR_CF_WINDOW, LANG_STR_CF_LANGUAGE, LANG_STR_BACK,
};
static const LangStrId SHADOW_LABEL[4] = { LANG_STR_SHADOW_0, LANG_STR_SHADOW_1, LANG_STR_SHADOW_2, LANG_STR_SHADOW_3 };

static UiText g_title, g_lang_question;

/* Visible rows (indices into CF_*): the language row only when the data offers both languages. */
static int visible_rows(int rows[CF_N])
{
    int n = 0;
    for (int r = 0; r < CF_N; r++)
        if (r != CF_LANG || lang_available_count() > 1) rows[n++] = r;
    return n;
}

static bool is_slider(int r) { return r <= CF_AUTO_SPEED; }

static int slider_get(int r)
{
    const SavConfig *c = sav_config();
    switch (r) {
    case CF_VOL_ALL: return c->audio.vol_all;
    case CF_VOL_BGM: return c->audio.vol_bgm;
    case CF_VOL_KOE: return c->audio.vol_koe;
    case CF_VOL_PCM: return c->audio.vol_pcm;
    case CF_VOL_SE:  return c->audio.vol_se;
    case CF_MSG_SPEED:  return 255 - SDL_clamp(c->msg_speed, 0, MSG_MS_MAX) * 255 / MSG_MS_MAX;   /* right = faster */
    case CF_AUTO_SPEED: return 255 - SDL_clamp(c->auto_moji_wait, 0, AUTO_MS_MAX) * 255 / AUTO_MS_MAX;
    default: return 0;
    }
}

static void slider_set(int r, int v)
{
    SavConfig *c = sav_config();
    v = SDL_clamp(v, 0, 255);
    switch (r) {
    case CF_VOL_ALL: c->audio.vol_all = v; break;
    case CF_VOL_BGM: c->audio.vol_bgm = v; break;
    case CF_VOL_KOE: c->audio.vol_koe = v; break;
    case CF_VOL_PCM: c->audio.vol_pcm = v; break;
    case CF_VOL_SE:  c->audio.vol_se = v; break;
    case CF_MSG_SPEED:  c->msg_speed = (255 - v) * MSG_MS_MAX / 255; break;
    case CF_AUTO_SPEED: c->auto_moji_wait = (255 - v) * AUTO_MS_MAX / 255; break;
    default: return;
    }
    ui_config_apply();
}

/* The language row asks first: the switch reloads the game data in the other language and restarts at the title
 * (main.c game_reboot). The question is asked in the current language. */
static void lang_toggle(void)
{
    bool to_en = !lang_is_en();
    if (!lang_available(to_en ? LANG_EN : LANG_JA)) return;
    ui_text_set(&g_lang_question, lang_str(to_en ? LANG_STR_LANG_SWITCH_TO_EN : LANG_STR_LANG_SWITCH_TO_JA));
    ui_confirm(&g_lang_question, UIA_LANG, to_en ? 1 : 0);
}

static void toggle(int r)
{
    SavConfig *c = sav_config();
    switch (r) {
    case CF_SKIP_UNREAD:   c->skip_unread = !c->skip_unread; break;
    case CF_KOE_DONT_STOP: c->koe_dont_stop = !c->koe_dont_stop; break;
    case CF_WHEEL:         c->wheel_next = !c->wheel_next; break;
    case CF_SHADOW:        c->font_shadow = (c->font_shadow + 1) & 3; break;
    case CF_WINDOW:        c->window_mode = c->window_mode == 1 ? 0 : 1; break;
    case CF_LANG:          lang_toggle(); return;
    default: return;
    }
    ui_config_apply();
}

static bool switch_on(int r)
{
    const SavConfig *c = sav_config();
    return r == CF_SKIP_UNREAD ? c->skip_unread : r == CF_KOE_DONT_STOP ? c->koe_dont_stop :
           r == CF_WHEEL ? c->wheel_next : c->window_mode == 1;
}

static int value_from_x(int x) { return (x - CF_CX0) * 255 / (CF_CX1 - CF_CX0); }
static Rect row_rect(int k) { return (Rect){ CF_LX - 10, CF_Y0 + k * CF_ROW, CF_CX1 + 20, CF_Y0 + (k + 1) * CF_ROW }; }

void ui_settings_open(UiScreen *s)
{
    (void)s;
    ui_text_gx(&g_title, "SYSCOMMENU.CONFIG", 2, lang_str(LANG_STR_SETTINGS));
}

/* Mouse drag on a slider row (s->drag = row + 1 while held), Left/Right on the focused slider. */
static void slider_input(UiScreen *s, const int *rows, int hit, int cur)
{
    const InputBtn *l = input_btn_mouse(0);
    if (l->on_down && hit >= 0 && is_slider(rows[hit]) && ui_in((Rect){ CF_CX0 - 8, 0, CF_CX1 + 8, SCREEN_H }))
        s->drag = rows[hit] + 1;
    if (s->drag > 0) {
        slider_set(s->drag - 1, value_from_x(input_mouse_x()));
        if (!l->is_down) s->drag = 0;
    }
    if (cur >= 0 && is_slider(cur)) {
        int step = input_key_pressed(SDL_SCANCODE_RIGHT) ? CF_KEY_STEP
                 : input_key_pressed(SDL_SCANCODE_LEFT)  ? -CF_KEY_STEP : 0;
        if (step) slider_set(cur, slider_get(cur) + step);
    }
}

void ui_settings_frame(UiScreen *s)
{
    int rows[CF_N], nr = visible_rows(rows);
    int hit = -1;                                    /* index into rows[] */
    for (int k = 0; k < nr; k++)
        if (ui_in(row_rect(k))) hit = s->hover = k;
    if (input_key_pressed(SDL_SCANCODE_DOWN)) s->hover = ui_cycle(s->hover, nr, 1);
    if (input_key_pressed(SDL_SCANCODE_UP)) s->hover = ui_cycle(s->hover, nr, -1);
    if (s->hover >= nr) s->hover = nr - 1;
    if (ui_rclick()) { ui_pop(); return; }
    int cur = s->hover >= 0 ? rows[s->hover] : -1;
    slider_input(s, rows, hit, cur);
    bool go = (ui_lclick() && hit >= 0) || ui_enter_pressed();
    if (!go || cur < 0 || is_slider(cur)) return;
    if (cur == CF_BACK) ui_pop();
    else toggle(cur);
}

static void draw_slider(Canvas *fb, int r, int y)
{
    int v = slider_get(r), kx = CF_CX0 + v * (CF_CX1 - CF_CX0) / 255;
    ui_fill(fb, (Rect){ CF_CX0, y + 12, CF_CX1, y + 16 }, UI_COL_SLIDER_TRACK, 255);
    ui_fill(fb, (Rect){ CF_CX0, y + 12, kx, y + 16 }, UI_COL_SLIDER_FILL, 255);
    ui_fill(fb, (Rect){ kx - 4, y + 5, kx + 4, y + 23 }, UI_COL_WHITE, 255);
}

/* Both language names, each in its own script; the active one lit. */
static void draw_languages(Canvas *fb, int y)
{
    int x = CF_CX0;
    static const LangId LANGS[] = { LANG_JA, LANG_EN };
    for (size_t l = 0; l < COUNT_OF(LANGS); l++) {
        bool on = lang_id() == LANGS[l];
        UiText u;
        ui_text_set(&u, lang_native_name(LANGS[l]));
        int w = ui_text_w(&u, CF_TEXT);
        ui_fill(fb, (Rect){ x - 6, y + 4, x + w + 6, y + 26 }, on ? UI_COL_TOGGLE_ON : UI_COL_TOGGLE_OFF, 255);
        ui_draw_text(fb, x, y + 5, CF_TEXT, on ? UI_COL_TEXT : UI_COL_TOGGLE_OFF_TEXT, &u);
        x += w + 24;
    }
}

static void draw_value(Canvas *fb, int r, int y, uint32_t col)
{
    UiText u;
    if (is_slider(r)) {
        draw_slider(fb, r, y);
    } else if (r == CF_SHADOW) {
        ui_text_set(&u, lang_str(SHADOW_LABEL[sav_config()->font_shadow & 3]));
        ui_draw_text(fb, CF_CX0, y + 5, CF_TEXT, col, &u);
    } else if (r == CF_LANG) {
        draw_languages(fb, y);
    } else if (r != CF_BACK) {
        bool on = switch_on(r);
        ui_fill(fb, (Rect){ CF_CX0, y + 4, CF_CX0 + 60, y + 26 }, on ? UI_COL_TOGGLE_ON : UI_COL_TOGGLE_OFF, 255);
        ui_text_set(&u, lang_str(on ? LANG_STR_ON : LANG_STR_OFF));
        ui_draw_text(fb, CF_CX0 + 30 - ui_text_w(&u, CF_TEXT) / 2, y + 5, CF_TEXT, UI_COL_TEXT, &u);
    }
}

void ui_settings_draw(Canvas *fb, const UiScreen *s)
{
    int rows[CF_N], nr = visible_rows(rows);
    ui_fill(fb, (Rect){ 0, 0, SCREEN_W, SCREEN_H }, UI_COL_BACKDROP, 235);
    ui_draw_text(fb, 30, 14, 24, UI_COL_TEXT, &g_title);
    for (int k = 0; k < nr; k++) {
        int r = rows[k], y = CF_Y0 + k * CF_ROW;
        uint32_t col = k == s->hover ? UI_COL_TEXT_HOVER : UI_COL_TEXT;
        if (k == s->hover) ui_fill(fb, (Rect){ CF_LX - 10, y, CF_CX1 + 20, y + CF_ROW - 2 }, UI_COL_ROW_SELECTED, 200);
        UiText label;
        ui_text_set(&label, lang_str(CF_LABEL[r]));
        ui_draw_text(fb, CF_LX, y + 5, CF_TEXT, col, &label);
        draw_value(fb, r, y, col);
    }
}
