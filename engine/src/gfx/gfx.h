/* gfx/gfx.h — stages, objects, IntEvents, wipes, shake, screen effect (GFX §2–6, WP3).
 * The Obj field list follows GFX §3.1 (game-used props + button fields of ENGINE_SPEC §1.4.4);
 * ievent_* and gfx_resolve_obj (§2.5) are additions. WP6 owns the btn_* fields. */
#ifndef GFX_GFX_H
#define GFX_GFX_H
#include "core/str.h"
#include "sav/sav_stream.h"
#include "gfx/canvas.h"
#include "vm/elements.h"

/* GFX §3.1: loop -1 idle, 0 one-shot, 1 loop, 2 ping-pong; speed 0 linear, 1 ease-in, 2 ease-out. */
typedef struct IntEvent { int def, value, time, total, delay, start, cur, end, loop, speed, real; } IntEvent;

enum { GFX_STAGE_BACK = 0, GFX_STAGE_FRONT = 1, GFX_STAGE_NEXT = 2, GFX_STAGE_CNT = 3, GFX_OBJ_CNT = 256 };
enum { OBJ_NONE = 0, OBJ_RECT = 1, OBJ_IMAGE = 2 };
/* Tone fields in GFX §3.5 order (props 28,29,30,31,32,33,34,23,57,58,59). */
enum { TONE_MONO, TONE_REVERSE, TONE_BRIGHT, TONE_DARK, TONE_R, TONE_G, TONE_B, TONE_RATE,
       TONE_ADD_R, TONE_ADD_G, TONE_ADD_B, TONE_CNT };

typedef struct Obj Obj;
struct Obj {
    int stage, idx;              /* 0 back, 1 front, 2 next; idx = slot (child: index in parent) */
    int type;                    /* 0 none, 1 rect, 2 image */
    WStr file;                   /* image name (type 2) */
    uint32_t rect_rgb;           /* type 1 colour (straight 0xAARRGGBB, A = create_rect alpha) */
    bool disp;                   /* prop 0 */
    IntEvent patno;              /* prop 1, event 90 */
    int layer, order;            /* props 2, 55 */
    IntEvent x, y, z;            /* props 3/4/5, events 51/64/65 */
    IntEvent center_x, center_y; /* props 6/7, events 77/78 */
    IntEvent scale_x, scale_y;   /* props 12/13, 1/1000 */
    IntEvent rotate_z;           /* prop 17, 1/10 degree */
    bool clip_use;
    IntEvent clip_l, clip_t, clip_r, clip_b; /* props 18..22 */
    bool src_clip_use;
    IntEvent src_l, src_t, src_r, src_b; /* props 149..153 */
    IntEvent tr;                         /* prop 27, event 72; default 255 */
    IntEvent *x_rep, *y_rep, *tr_rep;
    int nx_rep, ny_rep, ntr_rep; /* rep lists 54/63/141 (summed) */
    bool wipe_copy, wipe_erase;  /* props 56/92 */
    Obj *child;
    int nchild;
    Obj *parent;
    /* buttons (WP6, ENGINE_SPEC §1.4.4) */
    bool is_button;
    int btn_no, btn_group, btn_action, btn_se, btn_state;
    /* properties the game does not use unless noted, kept so that get/set round-trips */
    IntEvent center_z, center_rep_x, center_rep_y, center_rep_z, scale_z, rotate_x, rotate_y;
    IntEvent tone[TONE_CNT]; /* GFX §3.5, rendered when non-zero */
    IntEvent *z_rep;
    int nz_rep; /* rep list 110 */
    int world, blend, tonecurve, click_disable, light_no, fog_use, mask_no, culling, alpha_test, alpha_blend;
    int rect_x, rect_y, rect_w, rect_h; /* type 1 local rect (create_rect) */
    char *img_key;                      /* upper-case UTF-8 image name for img_get (type 2); owned (tracked heap) */
};

void ievent_init(IntEvent *e, int def);
void ievent_frame(IntEvent *e, int32_t game_dt, int32_t real_dt);       /* GFX §3.2 interpolation */
/* GFX §3.2 sub-commands (additions). */
void ievent_set(IntEvent *e, int value, int total, int delay, int speed, bool has_start, int start, bool real);
void ievent_loop(IntEvent *e, int start, int end, int total, int delay, int speed, int loop /*1 loop, 2 turn*/,
                 bool real);
void ievent_end(IntEvent *e);
void ievent_assign(IntEvent *e, int v);   /* plain property set: value = cur = v, event stopped */
static inline bool ievent_active(const IntEvent *e) { return e->loop != -1; }

void gfx_init(void);
void gfx_shutdown(void);                            /* releases every object and image (call before mem_shutdown) */
Obj *gfx_obj(int stage /*0 back,1 front,2 next*/, int idx);
Obj *gfx_resolve_obj(const int *chain, int n);      /* object args (form 1310), ENGINE_SPEC §2.5 */
void gfx_frame(int32_t game_dt, int32_t real_dt);   /* image cache trim, events, wipe progress, shake, effects */
void gfx_render(Canvas *fb);                        /* objects (+wipe composite, effect tone, shake); not windows/UI */
bool gfx_wipe_active(void);   void gfx_wipe_finish(void);       /* for 103/109 and skip */
bool gfx_obj_hit(const Obj *o, int x, int y);       /* drawn-rect hit test for buttons */
void gfx_reset_all(void);                           /* used by proc 8 (restart menu) */
void gfx_start_load_wipe(int type, int ms);         /* #LOAD.WIPE after a load (old fb -> new) */
void gfx_save(SavWriter *w);
bool gfx_load(SavReader *r);     /* objects of stages 0/1 with #OBJECT.000-099.SAVE=1 [I] + screen effect */
void gfx_capture(Canvas *out);   /* save thumbnail: the stage scaled into out (any size, letterboxed) */
void gfx_forget_fb(const Canvas *fb);   /* fb is going away: drop it if it is the last presented frame */

/* ---- object model (gfx_obj.c) ---- */
void obj_init(Obj *o);                 /* 35: free everything, all params to default (FUN_005ee710) */
void obj_free(Obj *o);                 /* 36: drop image/type, keep params (FUN_005ee800) */
void obj_init_param(Obj *o);           /* 37: params only (FUN_005eeb30) */
void obj_copy(Obj *dst, const Obj *src);                               /* 41: deep copy (children, reps) */
bool obj_create_image(Obj *o, const WStr *name); /* 38 body: reset + load (al handled by caller) */
void obj_create_rect(Obj *o, int x, int y, int w, int h, int r, int g, int b, int a);
int  obj_pat_count(const Obj *o);                                      /* 169 */
/* 93.4 / rep lists 54/63/110/141 .2: true when the list now has the requested size; false (unchanged) for a size
 * out of range, children below GFX_OBJ_DEPTH_MAX, or a request past the MEM_OBJ heap budget. */
bool obj_children_resize(Obj *o, int n);
bool obj_rep_resize(IntEvent **list, int *n, int newn);
void obj_frame(Obj *o, int32_t game_dt, int32_t real_dt);             /* tick every IntEvent (recursive) */
bool obj_screen_rect(const Obj *o, Rect *out); /* drawn rect incl. parents; false if nothing drawn */
/* The stage message-window list `S.3.-1.n.<sub>` (S = 37/38/73) belongs to txt, which registers its handler here
 * (rest = [n, sub, ...]; VM §9, TXT §8). Without one the chain is unknown. */
typedef ElmResult (*GfxStageMwndFn)(int stage, const int *rest, int nrest, ElmCall *c);
void gfx_set_stage_mwnd_handler(GfxStageMwndFn fn);
void gfx_set_cg_hook(void (*fn)(int flag_no));
/* #CONFIG.SKIP_WIPE_ANIME (a wipe wait without key= is skippable by a click) and NO_WIPE_ANIME (wipes end at
 * once); pushed from the user configuration by ui_config_apply. */
void gfx_set_wipe_options(
    bool skip_wipe_anime,
    bool no_wipe_anime); /* SYS §6.3 auto CG unlock: called with cgt_flag_no(name) >= 0 on image create */

/* Wipe (gfx.c, GFX §4). */
typedef struct WipeArgs {
    int type, time, speed;
    int opt[8];
    int begin_order, end_order, begin_layer, end_layer;
    bool wait;
    int key; /* key: 0/1, -1 = config (#CONFIG.SKIP_WIPE_ANIME) */
    int start_time;
} WipeArgs;
void wipe_args_default(WipeArgs *a);
void gfx_wipe_start(const WipeArgs *a); /* transfer back -> front (§2.1), start progress; pushes proc 0x23 if a->wait */
void gfx_push_wipe_wait(int key /*0/1, -1 config*/, bool push_result);
/* Shake (GFX §5) and screen effects (GFX §6). */
bool gfx_shake_start(int no);              /* sets the table; caller pushes proc 0x24 (elm_gfx does) */
bool gfx_shake_active(void);
void gfx_shake_stop(void);
typedef struct Effect {
    IntEvent x, y, z;
    IntEvent tone[TONE_CNT];
    int begin_order, end_order, begin_layer, end_layer;
    bool wipe_copy, wipe_erase;
} Effect;
enum { GFX_EFFECT_CNT = 4 };
Effect *gfx_effect(int i);                 /* 70.31[i] */
Effect *gfx_screen_tone(void);             /* the screen root's own tone props (70.0..) */
void gfx_effect_init(Effect *e);

void gfx_elm_init(void);                   /* gfx_elm.c: registers procs 0x23/0x24/0x26 (called by gfx_init) */
void gfx_objects_reset(void);              /* gfx_obj.c: all three stages back to empty */

/* Window-less unit tests (wipe math, events, sort order, transfer). Returns the number of failures. */
int gfx_selftest(void);

#endif
