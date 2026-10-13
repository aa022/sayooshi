/* txt/backlog.c — backlog records (TXT §7: FUN_005c1550 text, FUN_005c1490 name, FUN_005c13e0 koe, FUN_005c1690 nl).
 *
 * Memory: a static ring of BACKLOG_MAX entries; the newest entry overwrites the oldest. Entry strings are WStr on
 * the tracked heap, released when the slot is reused and by backlog_clear (language switch, load, txt_shutdown). */
#include "txt/backlog.h"
#include "core/str.h"
#include "sav/sav_stream.h"
#include <string.h>

enum { BACKLOG_SAVE_MAX = 100, BACKLOG_ENTRY_MIN_BYTES = 28 };   /* 3 empty str16 + 4 i32 */

static BacklogEntry g_ring[BACKLOG_MAX];
static int g_first, g_count;

/* What the next TEXT picks up: the NAME (raw and expanded) and koe seen since the previous TEXT. */
static struct { WStr name, disp; int koe, chara; } g_pend = { .koe = -1, .chara = -1 };

static void entry_free(BacklogEntry *e)
{
    ws_free(&e->text); ws_free(&e->name); ws_free(&e->disp_name);
    memset(e, 0, sizeof *e);
}

/* Empties a ring slot for a new line but keeps its string buffers: the ring is reused 100 lines at a time. */
static void entry_reuse(BacklogEntry *e)
{
    WStr text = e->text, name = e->name, disp = e->disp_name;
    memset(e, 0, sizeof *e);
    WStr *keep[] = { &text, &name, &disp };
    for (size_t i = 0; i < COUNT_OF(keep); i++) {
        keep[i]->len = 0;
        if (keep[i]->p) keep[i]->p[0] = 0;
    }
    e->text = text; e->name = name; e->disp_name = disp;
    e->koe_no = -1; e->chara = -1;
}

static void pending_clear(void)
{
    ws_set(&g_pend.name, NULL, 0);
    ws_set(&g_pend.disp, NULL, 0);
    g_pend.koe = g_pend.chara = -1;
}

static BacklogEntry *push_entry(void)
{
    BacklogEntry *e;
    if (g_count < BACKLOG_MAX) e = &g_ring[(g_first + g_count++) % BACKLOG_MAX];
    else { e = &g_ring[g_first]; g_first = (g_first + 1) % BACKLOG_MAX; }
    entry_reuse(e);
    return e;
}

void backlog_add_text(const WStr *t, int scene, int line)
{
    BacklogEntry *e = push_entry();
    ws_copy(&e->text, t);
    if (g_pend.name.len) ws_copy(&e->name, &g_pend.name);
    if (g_pend.disp.len) ws_copy(&e->disp_name, &g_pend.disp);
    e->koe_no = g_pend.koe; e->chara = g_pend.chara;
    e->scene = scene; e->line = line;
    pending_clear();
}

void backlog_add_name(const WStr *n, int scene, int line)
{
    (void)scene; (void)line;
    ws_copy(&g_pend.name, n);
    ws_copy(&g_pend.disp, n);
}

void backlog_set_disp_name(const WStr *n) { ws_copy(&g_pend.disp, n); }

void backlog_add_koe(int koe_no, int chara, int scene, int line)
{
    (void)scene; (void)line;
    g_pend.koe = koe_no; g_pend.chara = chara;
}

void backlog_add_newline(int scene, int line)
{
    (void)scene; (void)line;
    if (g_count) ws_push(&g_ring[(g_first + g_count - 1) % BACKLOG_MAX].text, '\n');
}

int backlog_count(void) { return g_count; }

const struct BacklogEntry *backlog_get(int i)
{
    if (i < 0 || i >= g_count) return NULL;
    return &g_ring[(g_first + i) % BACKLOG_MAX];
}

void backlog_clear(void)
{
    for (int i = 0; i < BACKLOG_MAX; i++) entry_free(&g_ring[i]);
    g_first = g_count = 0;
    ws_free(&g_pend.name);
    ws_free(&g_pend.disp);
    g_pend.koe = g_pend.chara = -1;
}

void backlog_save(SavWriter *w)
{
    sav_put_chunk(w, FOURCC('B', 'L', 'O', 'G'));
    int n = g_count < BACKLOG_SAVE_MAX ? g_count : BACKLOG_SAVE_MAX;
    sav_put_i32(w, n);
    for (int i = g_count - n; i < g_count; i++) {
        const BacklogEntry *e = backlog_get(i);
        sav_put_wstr(w, &e->text); sav_put_wstr(w, &e->name); sav_put_wstr(w, &e->disp_name);
        sav_put_i32(w, e->koe_no); sav_put_i32(w, e->chara); sav_put_i32(w, e->scene); sav_put_i32(w, e->line);
    }
}

bool backlog_load(SavReader *r)
{
    if (!sav_find_chunk(r, FOURCC('B', 'L', 'O', 'G'))) return false;
    backlog_clear();
    int n = sav_get_count(r, BACKLOG_SAVE_MAX, BACKLOG_ENTRY_MIN_BYTES);
    if (!sav_ok(r)) return false;
    for (int i = 0; i < n && sav_ok(r); i++) {
        BacklogEntry *e = push_entry();
        (void)sav_get_wstr(r, &e->text);    /* failures are sticky: sav_ok below */
        (void)sav_get_wstr(r, &e->name);
        (void)sav_get_wstr(r, &e->disp_name);
        e->koe_no = sav_get_i32(r); e->chara = sav_get_i32(r); e->scene = sav_get_i32(r); e->line = sav_get_i32(r);
    }
    return sav_ok(r);
}
