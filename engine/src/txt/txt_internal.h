/* txt/txt_internal.h — shared by the txt/ sources only: window and message objects, layout, selection, the
 * replay log, and the hooks the unit tests (txt_test.c) use.
 *
 * Memory: every window is a static object with fixed-capacity glyph, choice, read-flag and replay storage, so
 * laying out text never allocates. The only heap data are the WStr strings (replay texts, NAME, the overflow
 * remainder, the preview), released by mwnd_reset_all / txt_shutdown. Overfull storage drops the excess and logs
 * once (the bounds are far above anything the game produces). */
#ifndef TXT_TXT_INTERNAL_H
#define TXT_TXT_INTERNAL_H
#include "core/str.h"
#include "gfx/canvas.h"
#include "vm/elements.h"
#include "txt/mwnd.h"

enum { MWND_CNT = 16, MWND_DEFAULT = 2, MSG_DEFER_ROOT = 14 };
/* Deferred mwnd sub-commands (proc 0x12) are stored as chain [MSG_DEFER_ROOT, sub, mwnd_no]; txt_elm_global
 * routes a root-14 call with nrest == 2 to mwnd_sub(). The game's own `14` never has a rest. */

/* Capacities. A full page of the game is at most 16 rows x 35 glyphs; choices are a handful of short lines. */
enum {
    MSG_GLYPH_MAX = 4096,       /* glyphs of one window page */
    MSG_SIZE_MAX = 255,         /* character size (MGlyph.size is 8 bits; the font draws up to 255) */
    SEL_ITEM_MAX = 64,          /* choices of one in-window selection */
    SEL_GLYPH_MAX = 2048,       /* glyphs of all choices together */
    RF_PEND_MAX = 1024,         /* pending (scene, read flag) pairs of one page */
    RP_OP_MAX = 1024,           /* page replay log entries */
};

/* One laid-out glyph; moji/fuchi/shadow are COLOR_TABLE indices. */
typedef struct { uint16_t code; int16_t x, y; uint8_t size, moji, fuchi, shadow; } MGlyph;

typedef struct Msg {            /* one message object (TXT §5.3; msg+0x5c.. offsets in comments) */
    MGlyph *g; int ng, cap;     /* glyph storage owned by the caller (msg_bind) */
    int shown;                  /* glyphs revealed (+0x108) */
    int x, y;                   /* cursor (+0x5c/+0x60), relative to the text origin */
    int size, default_size;     /* +0x6c / +0x18 */
    int space_x, space_y;       /* +0x70 / +0x74 */
    int limit_x, limit_y;       /* +0xa8 / +0xac: cols*(size+space_x), rows*(size+space_y) [I] */
    int indent_x, depth;        /* +0xec / +0xf4 */
    uint16_t open_ch;           /* +0xf0 */
    bool at_line_start;         /* +0xfd */
    int moji, fuchi, shadow;    /* per-message colour override (-1 = default) */
    int def_moji, def_fuchi, def_shadow;
    int32_t t; bool started;    /* typing timer (+0x110/+0x114) */
} Msg;

typedef struct { int novel_mode, win_x, win_y, win_w, win_h, msg_x, msg_y, cols, rows, size, space_x, space_y,
                 waku_no, name_waku_no, name_disp_mode, name_bracket, overflow_check, back_insert_nl,
                 open_type, open_time, close_type, close_time; } MwndCfg;

typedef struct { int cond, type; int g0, g1; Rect r; } SelItem;   /* glyph range [g0,g1) in Mwnd.sel_g */

enum { RP_TEXT, RP_NL, RP_SIZE, RP_NAME };
typedef struct { int op; int a; WStr s; } ReplayOp;

typedef struct Mwnd {
    int no; MwndCfg cfg;
    bool open, in_block, printing, clear_pending;   /* +0x156, +0x154, +0x155, +0x158 */
    int32_t clock;              /* window clock (+0x150), game ms */
    int32_t open_anim, close_anim;  /* elapsed ms, -1 = idle (+0x180 / +0x18c) */
    int waku, name_waku;        /* current WAKU numbers (§1.4.1) */
    uint8_t filter[4];          /* r,g,b,a */
    int filter_margin[4];
    int icon_no, page_icon_no;
    bool page_icon, key_wait;   /* +0x2bc, +0x2b8 */
    int32_t icon_time;          /* real ms */
    int glyph_mark;             /* +0x15c: glyphs shown at the last key-wait end (auto mode) */
    int koe_no; bool koe_linked;    /* +0x178 / +0x17c */
    Msg msg;                    /* glyphs in .glyph */
    int nrf;                    /* rf_pend entries (2 per pending (scene, rf) pair, +0x16c) */
    int nsel; bool sel_active, sel_cancel; int sel_hover, sel_x, sel_y;
    int sel_kb, sel_mx, sel_my; /* keyboard/gamepad cursor (-1 = mouse), last mouse position (sel.c) */
    int nsel_g;
    WStr remainder; int rem_scene, rem_rf;      /* overflow remainder for sub 0x39 */
    int nrp;                    /* page replay log since the last page clear (§2.11) */
    WStr name;                  /* last NAME (raw) */
    MGlyph glyph[MSG_GLYPH_MAX];
    SelItem sel[SEL_ITEM_MAX];
    MGlyph sel_g[SEL_GLYPH_MAX];
    int rf_pend[2 * RF_PEND_MAX];
    ReplayOp rp[RP_OP_MAX];
} Mwnd;

/* Gameexe keys "<group>.<no>.<field>" with no as %03d (MWND.002.MOJI_SIZE, WAKU.000.ICON_NO, ICON.001.FILE). */
enum { TXT_GX_KEY_MAX = 96 };
void txt_gx_key(char key[TXT_GX_KEY_MAX], const char *group, int no, const char *field);
int  txt_gx_int(const char *group, int no, const char *field, int idx, int def);

/* Straight colour rgb at coverage a (1..255) over a premultiplied ARGB pixel: the blend of the text layers and the
 * window filter (kept separate from canvas_fill, whose rounding differs). */
static inline uint32_t txt_px_over(uint32_t d, uint32_t rgb, int a)
{
    int ia = 255 - a;
    int r = (int)((rgb >> 16) & 255), g = (int)((rgb >> 8) & 255), b = (int)(rgb & 255);
    int da = a + (int)(d >> 24) * ia / 255;
    int dr = r * a / 255 + (int)((d >> 16) & 255) * ia / 255;
    int dg = g * a / 255 + (int)((d >> 8) & 255) * ia / 255;
    int db = b * a / 255 + (int)(d & 255) * ia / 255;
    return (uint32_t)da << 24 | (uint32_t)dr << 16 | (uint32_t)dg << 8 | (uint32_t)db;
}

/* Log a capacity overflow once per process for the named store (*logged is the caller's static flag). */
void txt_cap_overflow(bool *logged, const char *what, int cap);

/* mwnd.c */
Mwnd *mwnd_cur(void);
Mwnd *mwnd_get(int no);
void  mwnd_sub(Mwnd *w, int sub, ElmCall *c);           /* mwnd element handler FUN_0065c1d0 (sub-code switch) */
void  mwnd_print_text(const WStr *s, int scene, int rf);   /* TEXT body (TXT §3.2) on the current window */
void  mwnd_set_name(const WStr *raw);                      /* NAME body (TXT §3.3) */
void  mwnd_window_init(Mwnd *w, int no, const MwndCfg *cfg);  /* fresh window (strings released first) */
void  mwnd_window_free(Mwnd *w);                        /* release the window's strings */
void  mwnd_display_update(Mwnd *w);                     /* typing step by the window clock (FUN_005e2870) */
void  mwnd_finish_printing(Mwnd *w);                    /* reveal everything now (FUN_005e2ac0) */
bool  mwnd_has_unrevealed(const Mwnd *w);
void  mwnd_rp_clear(Mwnd *w);
void  mwnd_rp_add(Mwnd *w, int op, int a, const uint16_t *s, int n);
void  mwnd_replay(Mwnd *w);                             /* re-layout the page from the replay log, all shown */
bool  mwnd_user_hidden(void);
/* Unit tests: override the typing speed switches; returns the previous values through the pointers. */
void  mwnd_test_swap_timing(bool *speed_nowait, bool *no_anime);

/* mwnd_draw.c: colour table, key icons, font style, drawing */
void  mwnd_style_load(void);                            /* #COLOR_TABLE, #MWND colours, #CONFIG.FONT, #ICON */
int   mwnd_style_moji_col(void);
int   mwnd_style_fuchi_col(void);
int   mwnd_style_shadow_col(void);

/* msglayout.c */
int  msg_char_width(uint16_t code, int size);           /* §1.6: half-width = CP932 single byte */
void msg_bind(Msg *m, MGlyph *storage, int cap);        /* glyph storage for m (cap >= 0); drops all glyphs */
void msg_config(Msg *m, const MwndCfg *cfg);            /* sizes, spacing and limits from the window config */
void msg_reset(Msg *m);                                 /* drop all glyphs, cursor home (size kept) */
int  msg_layout(Msg *m, const uint16_t *s, int n);      /* FUN_005e7690; returns units consumed (< n on overflow) */
void msg_newline(Msg *m);                               /* FUN_005e3070: explicit newline (indent reset) */
bool msg_fits_lines(const Msg *m, int lines);           /* FUN_005e2ee0 [I] */
bool msg_no_line_start(uint16_t c);                     /* FUN_005e72c0 */
typedef const WStr *(*NameResolver)(bool global, int idx);
void msg_expand_names(const WStr *in, WStr *out, NameResolver res);   /* FUN_00597fc0 */
void msg_set_size(Msg *m, int size);                    /* < 0 = default (§1.4.2); clamped to MSG_SIZE_MAX */

/* txt_elm.c */
void txt_elm_register(void);                            /* text roots and opcodes (mwnd_init) */
void txt_elm_shutdown(void);                            /* part of txt_shutdown */
void msg_shutdown(void);                                /* layout scratch (part of txt_shutdown) */

/* sel.c */
void sel_frame(bool screen_open); /* proc 0x0e hover/decide (TXT §4); idle while a screen is open */
void sel_reset(Mwnd *w);
void sel_add_item(Mwnd *w, const WStr *text, int cond, int type);

/* font.c */
void font_shutdown(void);                               /* faces and glyph cache (part of txt_shutdown) */
extern const unsigned char font_fallback_ttf[];         /* vendored Kosugi, generated by `sayopack cembed` */
extern const size_t font_fallback_ttf_size;


#endif
