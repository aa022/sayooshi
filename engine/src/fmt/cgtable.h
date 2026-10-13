/* fmt/cgtable.h — dat/MODE.CGM: the CG-mode table, g00 name -> flag number (AST §4).
 * Reference module of docs/CODE_STANDARD.md: Rd parsing, scratch memory for temporaries, bounded static storage. */
#ifndef FMT_CGTABLE_H
#define FMT_CGTABLE_H
#include "core/base.h"

enum { CGT_MAX_ENTRIES = 512, CGT_NAME_MAX = 32 };

bool        cgt_load(const char *path);         /* replaces the table; false (empty table) on missing/corrupt data */
bool        cgt_load_mem(const uint8_t *data, size_t n, const char *what);   /* same from bytes (not modified) */
int         cgt_count(void);
const char *cgt_name(int i);                    /* g00 name as stored (cp932; ASCII in this game); "" if out of range */
int         cgt_flag_no(const char *name_ci);   /* ASCII case-insensitive lookup; -1 if not in the table */

#endif
