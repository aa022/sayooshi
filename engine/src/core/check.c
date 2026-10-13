/* core/check.c — core_fatal / core_assert_fail. Static message buffer: no allocation on the fatal path. */
#include "core/check.h"
#include "core/log.h"
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

enum { FATAL_MSG_MAX = 1024 };

static char g_msg[FATAL_MSG_MAX];
static CoreFatalHook g_hook, g_trap;

void core_set_fatal_hook(CoreFatalHook hook) { g_hook = hook; }
void core_set_fatal_trap(CoreFatalHook trap) { g_trap = trap; }
const char *core_fatal_message(void) { return g_msg; }

static _Noreturn void fatal_stop(void)
{
    if (g_trap) g_trap(g_msg);   /* tests: longjmp out, nothing printed */
    log_msg(LOG_ERR, "FATAL: %s", g_msg);
    (void)fprintf(stderr, "sayooshi: FATAL: %s\n", g_msg);
    (void)fflush(stderr);
    if (g_hook) g_hook(g_msg);   /* may longjmp (tests) or show a message box and return */
    abort();
}

void core_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)SDL_vsnprintf(g_msg, sizeof g_msg, fmt, ap);
    va_end(ap);
    fatal_stop();
}

void core_assert_fail(const char *file, int line, const char *expr, const char *fmt, ...)
{
    int k = SDL_snprintf(g_msg, sizeof g_msg, "assertion failed at %s:%d: %s", file, line, expr);
    if (fmt && k > 0 && (size_t)k < sizeof g_msg - 3) {
        g_msg[k++] = ':';
        g_msg[k++] = ' ';
        va_list ap;
        va_start(ap, fmt);
        (void)SDL_vsnprintf(g_msg + k, sizeof g_msg - (size_t)k, fmt, ap);
        va_end(ap);
    }
    fatal_stop();
}
