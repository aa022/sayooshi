/* plat/platform.c — SDL3 window, streaming texture of the game's framebuffer, letterboxed scaling, vsync, event pump
 * (ENGINE_SPEC §2.1 #2, §2.7, WP7). Headless mode opens no window and translates no events. */
#include "plat/platform.h"
#include "core/log.h"
#include "plat/input.h"
#include <SDL3/SDL.h>

static struct {
    bool inited, headless, quit, vsync, fullscreen, focus, linear, text_on;
    bool game_cursor;                          /* the game draws its own cursor inside the frame */
    bool os_cursor_hidden;
    int w, h;                                  /* the frame size given to plat_init */
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    uint64_t last_present_ns;
} g_plat = { .focus = true, .linear = true };

bool plat_headless(void) { return g_plat.headless || !g_plat.win; }
void plat_request_quit(void) { g_plat.quit = true; }
uint32_t plat_ticks_ms(void) { return (uint32_t)SDL_GetTicks(); }

bool plat_init(bool headless, int w, int h, int scale)
{
    g_plat.headless = headless;
    g_plat.w = w > 0 ? w : 1;
    g_plat.h = h > 0 ? h : 1;
#if defined(SAYO_VALGRIND)
    /* The Valgrind build lets SDL_Quit shut libdbus down, so memcheck sees no blocks left by it. SDL advises against
     * this in production (another library may still use D-Bus after SDL_Quit), hence only in the tool build. */
    SDL_SetHint(SDL_HINT_SHUTDOWN_DBUS_ON_QUIT, "1");
#endif
    if (headless) {
        /* Events only (no video): keeps SDL_GetTicks/SDL_GetPrefPath etc. consistent. */
        if (!SDL_Init(0)) log_msg(LOG_WARN, "platform: SDL_Init(0): %s", SDL_GetError());
        g_plat.inited = true;
        return true;
    }
    SDL_SetHint(SDL_HINT_APP_NAME, "sayooshi");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        log_msg(LOG_ERR, "platform: SDL_Init(video): %s", SDL_GetError());
        return false;
    }
    g_plat.inited = true;
    /* Gamepads are optional: without the subsystem the game is played with mouse and keyboard. */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) log_msg(LOG_INFO, "platform: no gamepad support (%s)", SDL_GetError());
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    if (!SDL_CreateWindowAndRenderer("さよならを教えて", g_plat.w * scale, g_plat.h * scale,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &g_plat.win, &g_plat.ren)) {
        log_msg(LOG_ERR, "platform: cannot create window: %s", SDL_GetError());
        return false;
    }
    SDL_SetWindowMinimumSize(g_plat.win, g_plat.w / 2, g_plat.h / 2);
    g_plat.vsync = SDL_SetRenderVSync(g_plat.ren, 1);
    if (!g_plat.vsync) log_msg(LOG_INFO, "platform: no vsync (%s); frames are paced by a timer", SDL_GetError());
    /* Letterboxing + window->game mouse mapping are done by SDL's logical presentation. */
    SDL_SetRenderLogicalPresentation(g_plat.ren, g_plat.w, g_plat.h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    g_plat.tex =
        SDL_CreateTexture(g_plat.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, g_plat.w, g_plat.h);
    if (!g_plat.tex) {
        log_msg(LOG_ERR, "platform: cannot create texture: %s", SDL_GetError());
        return false;
    }
    SDL_SetTextureBlendMode(g_plat.tex, SDL_BLENDMODE_NONE);
    plat_set_linear(g_plat.linear);
    g_plat.focus = true;
    return true;
}

void plat_shutdown(void)
{
    if (g_plat.text_on && g_plat.win) SDL_StopTextInput(g_plat.win);
    if (g_plat.tex) SDL_DestroyTexture(g_plat.tex);
    if (g_plat.ren) SDL_DestroyRenderer(g_plat.ren);
    if (g_plat.win) SDL_DestroyWindow(g_plat.win);
    g_plat.tex = NULL; g_plat.ren = NULL; g_plat.win = NULL;
    if (g_plat.inited) SDL_Quit();
    g_plat.inited = false;
}

void plat_set_title(const char *utf8)
{
    if (g_plat.win && utf8 && *utf8) SDL_SetWindowTitle(g_plat.win, utf8);
}

/* Gamepad buttons drive the game through the keys it already understands: D-pad = arrow keys, A (south) = Enter
 * (decide), B (east) = Escape (cancel / system menu), Start = Escape, Y (north) = mouse-wheel up (backlog),
 * X (west) = Ctrl (skip while held). */
static void gamepad_button(int button, bool down)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP:    input_raw_key(SDL_SCANCODE_UP, down); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  input_raw_key(SDL_SCANCODE_DOWN, down); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  input_raw_key(SDL_SCANCODE_LEFT, down); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: input_raw_key(SDL_SCANCODE_RIGHT, down); break;
    case SDL_GAMEPAD_BUTTON_SOUTH:      input_raw_key(SDL_SCANCODE_RETURN, down); break;
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_START:      input_raw_key(SDL_SCANCODE_ESCAPE, down); break;
    case SDL_GAMEPAD_BUTTON_WEST:       input_raw_key(SDL_SCANCODE_LCTRL, down); break;
    case SDL_GAMEPAD_BUTTON_NORTH:      if (down) input_raw_wheel(1); break;
    default: break;
    }
}

void plat_set_fullscreen(bool on)
{
    g_plat.fullscreen = on;
    if (g_plat.win) SDL_SetWindowFullscreen(g_plat.win, on);
}

void plat_set_linear(bool linear)
{
    g_plat.linear = linear;
    if (g_plat.tex) SDL_SetTextureScaleMode(g_plat.tex, linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
}

static void mouse_to_game(SDL_Event *e, float *x, float *y)
{
    SDL_ConvertEventToRenderCoordinates(g_plat.ren, e);
    if (e->type == SDL_EVENT_MOUSE_MOTION) { *x = e->motion.x; *y = e->motion.y; }
    else if (e->type == SDL_EVENT_MOUSE_WHEEL) { *x = e->wheel.mouse_x; *y = e->wheel.mouse_y; }
    else { *x = e->button.x; *y = e->button.y; }
}

void plat_set_game_cursor(bool on) { g_plat.game_cursor = on; }

/* The OS cursor is hidden over the game frame while the game draws its own there, and shown over the letterbox
 * bars and outside the window, so there is always exactly one cursor. */
static void sync_os_cursor(int x, int y)
{
    bool hide = g_plat.game_cursor && x >= 0 && y >= 0 && x < g_plat.w && y < g_plat.h;
    if (hide == g_plat.os_cursor_hidden) return;
    if (hide ? SDL_HideCursor() : SDL_ShowCursor()) g_plat.os_cursor_hidden = hide;
}

static void mouse_event(SDL_Event *e)
{
    float x, y;
    mouse_to_game(e, &x, &y);
    int gx = (int)SDL_floorf(x), gy = (int)SDL_floorf(y);
    input_raw_mouse(gx, gy);
    sync_os_cursor(gx, gy);
    if (e->type == SDL_EVENT_MOUSE_MOTION) return;
    bool down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    log_trace("INPUT", "plat button %d %s at %d %d", (int)e->button.button, down ? "down" : "up", gx, gy);
    if (e->button.button == SDL_BUTTON_LEFT) input_raw_button(0, down);
    else if (e->button.button == SDL_BUTTON_RIGHT) input_raw_button(1, down);
}

static void wheel_event(const SDL_Event *e)
{
    float dy = e->wheel.y;
    if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) dy = -dy;
    int d = dy > 0 ? 1 : dy < 0 ? -1 : 0;   /* +1 = up (backlog), -1 = down (advance), §1.6 */
    if (d) input_raw_wheel(d);
}

static void key_event(const SDL_Event *e)
{
    bool down = e->type == SDL_EVENT_KEY_DOWN;
    SDL_Scancode sc = e->key.scancode;
    /* Alt+Enter / F11: fullscreen toggle, not forwarded (§4 M5). */
    if (down && !e->key.repeat &&
        (sc == SDL_SCANCODE_F11 || (sc == SDL_SCANCODE_RETURN && (e->key.mod & SDL_KMOD_ALT)))) {
        plat_set_fullscreen(!g_plat.fullscreen);
        return;
    }
    if (e->key.repeat) return;   /* edges only; the input module tracks held state */
    input_raw_key((int)sc, down);
}

static void gamepad_event(const SDL_Event *e)
{
    if (e->type == SDL_EVENT_GAMEPAD_ADDED) {
        if (!SDL_OpenGamepad(e->gdevice.which)) log_msg(LOG_WARN, "platform: cannot open gamepad: %s", SDL_GetError());
        else log_msg(LOG_INFO, "platform: gamepad %s", SDL_GetGamepadNameForID(e->gdevice.which));
    } else if (e->type == SDL_EVENT_GAMEPAD_REMOVED) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(e->gdevice.which);
        if (g) SDL_CloseGamepad(g);
    } else {
        gamepad_button(e->gbutton.button, e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
    }
}

static void window_event(SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: g_plat.quit = true; break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: g_plat.focus = true; break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: g_plat.focus = false; break;
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: mouse_event(e); break;
    case SDL_EVENT_MOUSE_WHEEL: wheel_event(e); break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: key_event(e); break;
    case SDL_EVENT_TEXT_INPUT: input_raw_text(e->text.text); break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: gamepad_event(e); break;
    default: break;
    }
}

bool plat_pump(void)
{
    SDL_Event e;
    if (!g_plat.win) {
        if (g_plat.inited && !g_plat.headless) return !g_plat.quit;
        /* Headless: still drain the queue (SDL may post quit on SIGINT). */
        while (g_plat.inited && SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT) g_plat.quit = true;
        return !g_plat.quit;
    }
    while (SDL_PollEvent(&e)) window_event(&e);
    return !g_plat.quit;
}

void plat_present(const uint32_t *argb, int pitch_px)
{
    if (!g_plat.win || !argb || pitch_px < g_plat.w) return;
    SDL_UpdateTexture(g_plat.tex, NULL, argb, pitch_px * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(g_plat.ren, 0, 0, 0, 255);
    SDL_RenderClear(g_plat.ren);
    SDL_RenderTexture(g_plat.ren, g_plat.tex, NULL, NULL);
    SDL_RenderPresent(g_plat.ren);
    if (!g_plat.vsync) {
        /* ~60 Hz pacing when vsync is unavailable. */
        const uint64_t frame_ns = 16666667;
        uint64_t now = SDL_GetTicksNS();
        if (g_plat.last_present_ns && now - g_plat.last_present_ns < frame_ns)
            SDL_DelayNS(frame_ns - (now - g_plat.last_present_ns));
        g_plat.last_present_ns = SDL_GetTicksNS();
    }
}
