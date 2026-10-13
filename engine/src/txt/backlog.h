/* txt/backlog.h — backlog records (text, name, koe) and the viewer data API (TXT §7, WP4).
 * One entry per TEXT. A koe (18) and a NAME seen before the TEXT attach to that TEXT's entry. */
#ifndef TXT_BACKLOG_H
#define TXT_BACKLOG_H
#include "core/str.h"
#include "sav/sav_stream.h"

typedef struct BacklogEntry {
    WStr text, name;   /* text = expanded message; name = raw speaker (may be empty) */
    int koe_no, chara; /* koe_no -1 = no voice */
    int scene, line;
    WStr disp_name; /* speaker with ＊Ａ etc. expanded (empty if none) */
} BacklogEntry;

void backlog_add_text(const WStr *t, int scene, int line);
void backlog_add_name(const WStr *n, int scene, int line);
void backlog_add_koe(int koe_no, int chara, int scene, int line);
int  backlog_count(void);
const struct BacklogEntry *backlog_get(int i);       /* 0 = oldest */

/* ---- capacity, names, persistence ---- */
enum { BACKLOG_MAX = 1000 };
void backlog_set_disp_name(const WStr *n);           /* expanded form of the pending NAME */
void backlog_add_newline(int scene, int line);       /* MSG_BACK_INSERT_NL (FUN_005c1690): '\n' to the last entry */
void backlog_clear(void);
void backlog_save(SavWriter *w);
bool backlog_load(SavReader *r); /* last 100 entries (ENGINE_SPEC §2.11, optional) */

#endif
