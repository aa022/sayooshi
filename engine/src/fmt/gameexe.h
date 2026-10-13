/* fmt/gameexe.h — Gameexe.dat decrypt + parse (AST §6).
 * Keys are stored without '#', upper-case, e.g. "MWND.002.MOJI_SIZE".
 * Range keys such as "OBJECT.000-099.SAVE" are expanded so that gx_int("OBJECT.042.SAVE",0,0) works.
 * Fields are the top-level comma-separated parts of the value ("quoted" strings and (...) groups are atoms).
 * Lookups are case-insensitive and accept a leading '#'. A later line with the same key overrides.
 * Memory: the table lives in the data arena (lifetime: the loaded language); load it again after
 * mem_data_reset(). Main thread. */
#ifndef FMT_GAMEEXE_H
#define FMT_GAMEEXE_H
#include "core/str.h"

bool gx_load(const char *gameexe_dat_path);                   /* replaces the table; false = missing/corrupt (empty) */
bool gx_load_mem(const uint8_t *data, size_t n, const char *what);   /* same from bytes; data is not modified */
void gx_unload(void);                                         /* empty table; its memory goes back if still on top */
bool gx_has(const char *key);
int  gx_int(const char *key, int idx, int def);               /* idx-th comma field; def if missing/not a number */
bool gx_str(const char *key, int idx, WStr *out);             /* idx-th field, surrounding quotes removed */
int  gx_tuples(const char *key, int *out, int max_ints);      /* "(a,b,c)(d,e,f)" -> flat ints; returns count */

int  gx_nfields(const char *key);                             /* comma fields of key (0 if missing) */
int  gx_line_count(void);                                     /* setting lines of the loaded file */

#endif
