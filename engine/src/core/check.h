/* core/check.h — fatal errors. ASSERT/ASSERT_MSG live in core/base.h; this header adds core_fatal() and the hook
 * the application (message box) and the core selftest (longjmp) install. */
#ifndef CORE_CHECK_H
#define CORE_CHECK_H
#include "core/base.h"

/* Logs "FATAL: <message>", runs the fatal hook, then abort()s. Never allocates (works when out of memory). */
_Noreturn void core_fatal(const char *fmt, ...) CORE_PRINTF(1, 2);

/* hook: called after the message is logged, e.g. to show a message box; the process aborts when it returns.
 * trap: tests only; called first, silently, and expected to leave by longjmp (the core selftest checks that
 * budget overruns, double frees and bad handles stop the program). NULL removes either. Set them at startup. */
typedef void (*CoreFatalHook)(const char *message);
void core_set_fatal_hook(CoreFatalHook hook);
void core_set_fatal_trap(CoreFatalHook trap);

/* The last fatal message (tests read it after their trap longjmp'ed out). */
const char *core_fatal_message(void);

/* Unit tests of the core layer (--selftest core): memory, strings, UTF, numbers, paths, Rd/Wr, hashes. 0 = pass. */
int core_selftest(void);

#endif
