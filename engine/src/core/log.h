/* core/log.h — logging (SDL_Log: stderr, Android logcat, ...) and the --trace file (ENGINE_SPEC §3.2).
 * log_msg is thread-safe. The trace file is written by the main thread only (docs/CODE_STANDARD.md §8). */
#ifndef CORE_LOG_H
#define CORE_LOG_H
#include "core/base.h"
#include <stdarg.h>

enum { LOG_ERR, LOG_WARN, LOG_INFO, LOG_DEBUG };
void log_set_level(int max_level);                      /* messages above this level are dropped (default LOG_INFO) */
int  log_level(void);
void log_msg(int level, const char *fmt, ...) CORE_PRINTF(2, 3);
/* Logs only the first time for *logged (the caller's static flag, owned by one thread): "store full" warnings. */
void log_once(bool *logged, int level, const char *fmt, ...) CORE_PRINTF(3, 4);

bool log_trace_open(const char *path, const char *cats_csv);   /* cats NULL/"" = all; categories as below */
void log_trace_close(void);
bool log_trace_on(const char *cat);                     /* categories: VM, CMD, TEXT, PROC, SND, GFX, UNK, INPUT, ... */
void log_trace(const char *cat, const char *fmt, ...) CORE_PRINTF(2, 3);   /* one line to the --trace file */

#endif
