/* core/str.h — strings (docs/CODE_STANDARD.md §5.3). Nothing outside core/str*.c and core/mem*.c uses
 * strcpy/strcat/strncpy/sprintf/strtok/atoi or friends.
 *
 *  Str      read-only UTF-8 slice {p, n}; need not be NUL-terminated. STR("lit"), str_c(cstr).
 *  StrBuf   bounded UTF-8 builder over caller or arena storage; always NUL-terminated; overflow truncates at a
 *           code-point boundary and sets a sticky flag (sb_ok() false) instead of writing past the end.
 *  WStr     heap UTF-16 string (script strings, VM §5): grows on the tracked heap up to WSTR_MAX_LEN units.
 *  WBuf     bounded UTF-16 builder over caller storage, like StrBuf.
 *  utf_*    strict UTF-8 / UTF-16 decoding and encoding (overlong forms, surrogates, > U+10FFFF rejected).
 *  str_to_* number parsing with full-string and range checks (replaces atoi/strtol).
 *  path_*   join / normalise / split for '/'-separated paths ('\\' accepted as input).
 *  Fixed buffers: str_copy, str_cat, str_fmt always NUL-terminate and report truncation. */
#ifndef CORE_STR_H
#define CORE_STR_H
#include "core/base.h"
#include <stdarg.h>

/* ---- Str ---------------------------------------------------------------------------------------------------- */
typedef struct Str { const char *p; size_t n; } Str;
#define STR(lit) ((Str){ (lit), sizeof(lit) - 1 })
Str  str_c(const char *s);                          /* NULL -> empty */
Str  str_make(const char *p, size_t n);
bool str_eq(Str a, Str b);
bool str_eq_fold(Str a, Str b);                     /* ASCII case-insensitive */
int  str_cmp(Str a, Str b);                         /* bytewise; a prefix sorts first */
int  str_cmp_fold(Str a, Str b);
bool str_starts_with(Str s, Str prefix);
bool str_ends_with_fold(Str s, Str suffix);
Str  str_sub(Str s, size_t pos, size_t len);        /* clamped to s */
bool str_find_char(Str s, char c, size_t *pos);     /* first occurrence */
bool str_rfind_char(Str s, char c, size_t *pos);    /* last occurrence */
bool str_find(Str hay, Str needle, size_t *pos);
Str  str_trim(Str s);                               /* ASCII whitespace at both ends */
Str  str_skip_blanks(Str s);                        /* without its leading spaces and tabs */
Str  str_next_token(Str *rest);                     /* next space/tab separated token; *rest continues after it */
/* Up to `want` leading integers of s (blank-separated; junk after them is ignored); returns how many were read. */
int  str_scan_ints(Str s, int32_t *v, int want);
/* Tokenizer (replaces strtok): *field = text up to the next sep; *rest advances past it. False when *rest was
 * empty before the call. Empty fields are returned ("a,,b" -> "a", "", "b"). */
bool str_split(Str *rest, char sep, Str *field);

static inline char str_ascii_upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }

/* ---- arena strings: exact-size NUL-terminated copies (paths built per call in scratch memory) ---------------- */
struct Arena;
char *str_dup_arena(struct Arena *a, Str s);
char *str_cat_arena(struct Arena *a, const char *x, const char *y, const char *z);   /* x + y + z; z may be NULL */

/* ---- fixed buffers: always NUL-terminated (cap > 0); false = truncated ---------------------------------------- */
CORE_NODISCARD bool str_copy(char *dst, size_t cap, Str src);
CORE_NODISCARD bool str_copy_c(char *dst, size_t cap, const char *src);
CORE_NODISCARD bool str_cat_c(char *dst, size_t cap, const char *src);
CORE_NODISCARD bool str_fmt(char *dst, size_t cap, const char *fmt, ...) CORE_PRINTF(3, 4);
CORE_NODISCARD bool str_vfmt(char *dst, size_t cap, const char *fmt, va_list ap);
/* str_fmt for text where a cut result is acceptable (log lines, display notes): cut at a code point, nothing to
 * check. */
void str_fmt_cut(char *dst, size_t cap, const char *fmt, ...) CORE_PRINTF(3, 4);
void str_vfmt_cut(char *dst, size_t cap, const char *fmt, va_list ap);

/* ---- StrBuf ------------------------------------------------------------------------------------------------- */
typedef struct StrBuf { char *p; size_t n, cap; bool trunc; } StrBuf;   /* cap includes the NUL */
void        sb_init(StrBuf *b, char *storage, size_t cap);
void        sb_reset(StrBuf *b);
bool        sb_put(StrBuf *b, Str s);
bool        sb_put_c(StrBuf *b, const char *s);
bool        sb_put_char(StrBuf *b, char c);
bool        sb_put_cp(StrBuf *b, uint32_t cp);      /* one code point as UTF-8 (invalid -> U+FFFD) */
bool        sb_fmt(StrBuf *b, const char *fmt, ...) CORE_PRINTF(2, 3);
bool        sb_vfmt(StrBuf *b, const char *fmt, va_list ap);
static inline bool        sb_ok(const StrBuf *b) { return !b->trunc; }
static inline Str         sb_str(const StrBuf *b) { return str_make(b->p, b->n); }
static inline const char *sb_cstr(const StrBuf *b) { return b->p; }
#define SB_ON_STACK(name, size) char name##_mem[size]; StrBuf name; sb_init(&name, name##_mem, sizeof name##_mem)

/* ---- UTF ---------------------------------------------------------------------------------------------------- */
enum { UTF_REPLACEMENT = 0xFFFD, UTF_MAX_CP = 0x10FFFF };
/* Decode the code point at *pos and advance. Invalid input yields U+FFFD, advances by one unit, returns false. */
bool   utf_decode8(Str s, size_t *pos, uint32_t *cp);
bool   utf_decode16(const uint16_t *s, size_t n, size_t *pos, uint32_t *cp);
size_t utf_encode8(uint32_t cp, char out[4]);       /* bytes written; invalid cp -> U+FFFD */
size_t utf_encode16(uint32_t cp, uint16_t out[2]);  /* units written; invalid cp -> U+FFFD */
bool   utf_valid8(Str s);

/* ---- WStr: heap UTF-16 (p[len] == 0 kept for convenience) ---------------------------------------------------- */
typedef struct WStr { uint16_t *p; int len, cap; } WStr;
enum { WSTR_MAX_LEN = 1 << 24 };                    /* longer results are truncated (logged once) */
void  ws_set(WStr *s, const uint16_t *u, int n);    /* n < 0: u is 0-terminated (scanned up to WSTR_MAX_LEN) */
void  ws_set_utf8(WStr *s, const char *utf8);       /* lenient legacy decoder (see str_wide.c) */
void  ws_copy(WStr *dst, const WStr *src);
void  ws_cat(WStr *dst, const WStr *src);           /* src may alias dst */
void  ws_push(WStr *s, uint16_t c);
void  ws_free(WStr *s);
int   ws_cmp(const WStr *a, const WStr *b);         /* code-unit lexicographic, shorter prefix first */
int   ws_cmp_fold(const WStr *a, const WStr *b);    /* folds ASCII A-Z only (VM §5.2) */
char *ws_utf8(const WStr *s);                       /* release with ws_utf8_free; lone surrogates kept as WTF-8 */
void  ws_utf8_free(char *u);                        /* the only correct way to release a ws_utf8 result */
bool  ws_to_utf8(const WStr *s, StrBuf *out);       /* WTF-8 like ws_utf8; false only when out is truncated */
bool  ws_from_utf8(WStr *s, Str utf8);              /* strict: invalid input -> U+FFFD (false) */
/* WTF-8 copy of s in arena a (a scratch arena for a log line, a file name, a key): NUL-terminated, at most
 * max_bytes with the NUL (>= 1; cut at a code point beyond). The arena's lifetime is the result's. */
enum { WS_UTF8_SHORT_MAX = 1 << 16 };              /* names and log lines */
const char *ws_utf8_arena(struct Arena *a, const WStr *s, size_t max_bytes);

/* ---- WBuf: bounded UTF-16 builder ------------------------------------------------------------------------------ */
typedef struct WBuf { uint16_t *p; size_t n, cap; bool trunc; } WBuf;   /* cap includes the 0 terminator */
void wb_init(WBuf *b, uint16_t *storage, size_t cap);
bool wb_put(WBuf *b, const uint16_t *u, size_t n);
bool wb_put_cp(WBuf *b, uint32_t cp);
bool wb_put_utf8(WBuf *b, Str s);                   /* strict decode; false on invalid input or truncation */
static inline bool wb_ok(const WBuf *b) { return !b->trunc; }

/* ---- numbers: the whole string must be the number (no spaces, no junk); false on overflow ----------------------- */
CORE_NODISCARD bool str_to_i64(Str s, int64_t *out);     /* optional sign, decimal */
CORE_NODISCARD bool str_to_u64(Str s, uint64_t *out);    /* decimal, or hex with 0x */
CORE_NODISCARD bool str_to_u64_hex(Str s, uint64_t *out);/* hex digits only, no prefix ("cbf43926") */
CORE_NODISCARD bool str_to_i32(Str s, int32_t *out);
CORE_NODISCARD bool str_to_int_range(Str s, long long lo, long long hi, long long *out);
/* Scan a decimal integer (optional sign) starting at *pos and advance past it; false (pos unchanged) if none
 * there or out of int32 range. For tokenising formats such as "(1,2,3)". */
CORE_NODISCARD bool str_scan_i32(Str s, size_t *pos, int32_t *out);

/* ---- paths ('/' separated) ------------------------------------------------------------------------------------- */
bool path_join(StrBuf *out, Str dir, Str name);     /* dir + '/' + name (no doubled '/') */
Str  path_base(Str p);                              /* after the last '/' or '\\' */
Str  path_dir(Str p);                               /* up to (excluding) the last separator; "" if none */
Str  path_ext(Str p);                               /* after the last '.' of the base name, "" if none */
/* '\\' -> '/', collapse "//" and "/./", resolve "..". Fails on a ".." that would climb above the start (a
 * hostile archive path) when the input is relative. */
bool path_normalize(StrBuf *out, Str p);
bool path_is_safe_rel(Str p);                       /* relative, no "..", no drive letter, no NUL */

#endif
