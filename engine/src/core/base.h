/* core/base.h — the few definitions every engine file may need: fixed-width types, compile-time helpers and the
 * always-on assertion macros (docs/CODE_STANDARD.md §4, §6). No state, no functions with linkage. */
#ifndef CORE_BASE_H
#define CORE_BASE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- compiler hints (no-ops where unsupported) ---- */
#if defined(__GNUC__) || defined(__clang__)
#define CORE_NODISCARD      __attribute__((warn_unused_result))
#define CORE_PRINTF(f, a)   __attribute__((format(printf, f, a)))
#define CORE_UNLIKELY(x)    __builtin_expect(!!(x), 0)
#else
#define CORE_NODISCARD
#define CORE_PRINTF(f, a)
#define CORE_UNLIKELY(x)    (x)
#endif

/* Number of elements of a true array (not a pointer). */
#define COUNT_OF(a)         (sizeof(a) / sizeof((a)[0]))
#define CORE_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

/* ---- assertions: ALWAYS compiled in (never NDEBUG-dependent) -----------------------------------------------
 * ASSERT is for invariants of OUR code: a failure is a bug and stops the program with file:line through
 * core_fatal(). Never use it on external data (game files, saves, input); validate those and fail recoverably. */
#define ASSERT(cond) \
    (CORE_UNLIKELY(!(cond)) ? core_assert_fail(__FILE__, __LINE__, #cond, NULL) : (void)0)
#define ASSERT_MSG(cond, ...) \
    (CORE_UNLIKELY(!(cond)) ? core_assert_fail(__FILE__, __LINE__, #cond, __VA_ARGS__) : (void)0)

_Noreturn void core_assert_fail(const char *file, int line, const char *expr, const char *fmt, ...) CORE_PRINTF(4, 5);

/* ---- small checked helpers ---- */
static inline int32_t core_clamp_i32(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t core_min_i32(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t core_max_i32(int32_t a, int32_t b) { return a > b ? a : b; }
static inline size_t core_min_sz(size_t a, size_t b) { return a < b ? a : b; }
static inline size_t core_max_sz(size_t a, size_t b) { return a > b ? a : b; }

/* Two's-complement wrapping int32 arithmetic (script integers, millisecond clocks) without signed-overflow UB. */
static inline int32_t core_wrap_add_i32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t core_wrap_sub_i32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static inline int32_t core_wrap_mul_i32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

/* Overflow-checked size arithmetic: false (and *out untouched) when the exact result does not fit. */
static inline bool core_add_sz(size_t a, size_t b, size_t *out)
{
    if (b > SIZE_MAX - a) return false;
    *out = a + b;
    return true;
}
static inline bool core_mul_sz(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}
/* Round n up to a multiple of align (a power of two); false on overflow. */
static inline bool core_align_up(size_t n, size_t align, size_t *out)
{
    size_t m;
    if (!core_add_sz(n, align - 1, &m)) return false;
    *out = m & ~(align - 1);
    return true;
}
static inline bool core_is_pow2(size_t v) { return v != 0 && (v & (v - 1)) == 0; }

#endif
