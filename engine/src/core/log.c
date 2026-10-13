/* core/log.c — logging (via SDL_Log for portability incl. Android logcat) and the --trace file (§3.2). */
#include "core/log.h"
#include "core/str.h"
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <string.h>

enum { TRACE_LINE_MAX = 4096 };

static int g_log_level = LOG_INFO;
static SDL_IOStream *g_trace;
static char g_trace_cats[128];   /* ",VM,CMD," form; empty = all */

void log_set_level(int max_level) { g_log_level = max_level; }
int log_level(void) { return g_log_level; }

static void log_vmsg(int level, const char *fmt, va_list ap)
{
    static const SDL_LogPriority prio[] = { SDL_LOG_PRIORITY_ERROR, SDL_LOG_PRIORITY_WARN,
                                            SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_DEBUG };
    if (level > g_log_level) return;
    if (level < LOG_ERR) level = LOG_ERR;
    if (level > LOG_DEBUG) level = LOG_DEBUG;
    /* SDL filters by its own priorities; make sure ours always pass. */
    if (SDL_GetLogPriority(SDL_LOG_CATEGORY_APPLICATION) > prio[level])
        SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, prio[level]);
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, prio[level], fmt, ap);
}

void log_msg(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vmsg(level, fmt, ap);
    va_end(ap);
}

void log_once(bool *logged, int level, const char *fmt, ...)
{
    if (*logged) return;
    *logged = true;
    va_list ap;
    va_start(ap, fmt);
    log_vmsg(level, fmt, ap);
    va_end(ap);
}

bool log_trace_open(const char *path, const char *cats_csv)
{
    log_trace_close();
    g_trace = SDL_IOFromFile(path, "w");
    if (!g_trace) {
        log_msg(LOG_ERR, "trace: cannot open %s: %s", path, SDL_GetError());
        return false;
    }
    g_trace_cats[0] = 0;
    if (cats_csv && *cats_csv && !str_fmt(g_trace_cats, sizeof g_trace_cats, ",%s,", cats_csv)) {
        log_msg(LOG_ERR, "trace: category list too long: %s", cats_csv);
        g_trace_cats[0] = 0;
        log_trace_close();
        return false;
    }
    return true;
}

void log_trace_close(void)
{
    SDL_IOStream *t = g_trace;
    g_trace = NULL;                            /* first: the message below must not go to the closed stream */
    if (t && !SDL_CloseIO(t))                  /* the final flush failed: the trace file is incomplete */
        log_msg(LOG_ERR, "trace: closing the trace file failed (%s); the trace may be truncated", SDL_GetError());
}

bool log_trace_on(const char *cat)
{
    if (!g_trace) return false;
    if (!g_trace_cats[0]) return true;
    char key[16];
    if (!str_fmt(key, sizeof key, ",%s,", cat)) return false;   /* no category is that long */
    return SDL_strcasestr(g_trace_cats, key) != NULL;
}

void log_trace(const char *cat, const char *fmt, ...)
{
    if (!log_trace_on(cat)) return;
    SB_ON_STACK(line, TRACE_LINE_MAX);
    (void)sb_fmt(&line, "%-5s ", cat);
    va_list ap;
    va_start(ap, fmt);
    (void)sb_vfmt(&line, fmt, ap);         /* an over-long line is cut (and then has no newline, as before) */
    va_end(ap);
    if (sb_ok(&line)) (void)sb_put_char(&line, '\n');
    if (SDL_WriteIO(g_trace, line.p, line.n) != line.n) log_msg(LOG_ERR, "trace: write failed: %s", SDL_GetError());
}
