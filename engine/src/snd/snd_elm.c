/* snd/snd_elm.c — script elements: roots 42 bgm, 44 pcmch, 85/99 koe waits, 87/88/89 exkoe, 152–155 koe volume,
 * 68/69 koe stop/check; wait procs 0x1b/0x1c (bgm), 0x1d (koe), 0x1f/0x20 (pcmch) (SYS §2.4–2.6, ENGINE_SPEC
 * §2.6, WP5). Sub numbers and overloads: origsrc/element_defs_profile0.tsv; argument handling FUN_006655a0 /
 * FUN_006660d0 / FUN_0065be10. Script values are passed through to audio.c, which clamps them. */
#include "snd/audio.h"
#include "snd/snd_internal.h"
#include "core/str.h"
#include "vm/elements.h"
#include "vm/proc.h"
#include "vm/vm.h"

enum { PROC_BGM_FADE_WAIT = 0x1c, PROC_PCMCH_FADE_WAIT = 0x20 };   /* FUN_005db5c0 / FUN_005ddc30 */
enum { DEFAULT_STOP_FADE_MS = 100, VOLUME_MAX = 255 };

/* ------------------------------------------------------------------------------------------------ wait procs
 * ENGINE_SPEC §1.4.6: skip ends the wait; a click ends it only with key_skip (the *_key variants return 1). */

static bool wait_finish(Proc *p, int v)
{
    bool push = p->push_result;
    proc_pop();
    if (push) vm_push_int(v);
    return true;
}

static bool wait_generic(Proc *p, bool done)
{
    if (done) return wait_finish(p, 0);
    if (proc_wait_skip(p)) return wait_finish(p, 0);
    if (p->key_skip && proc_wait_click()) return wait_finish(p, 1);
    return false;
}

static bool proc_bgm_wait(Proc *p)        { return wait_generic(p, !bgm_playing()); }
static bool proc_bgm_fade_wait(Proc *p)   { return wait_generic(p, !bgm_fading()); }
static bool proc_koe_wait(Proc *p)        { return wait_generic(p, !koe_playing()); }   /* [C 0x5a1e50] */
static bool proc_pcmch_wait(Proc *p)      { return wait_generic(p, !pcmch_playing(p->iarg[0])); }
static bool proc_pcmch_fade_wait(Proc *p) { return wait_generic(p, !pcmch_fading(p->iarg[0])); }

static ElmResult snd_elm_global(int root, const int *rest, int nrest, ElmCall *c);

void snd_elm_init(void)
{
    static const int ROOTS[] = {
        ELM_ROOT_MOV,
        ELM_ROOT_BGM,
        ELM_ROOT_PCM,
        ELM_ROOT_PCMCH,
        ELM_ROOT_SE,
        ELM_ROOT_PCMEVENT,
        ELM_ROOT_KOE_STOP,
        ELM_ROOT_KOE_CHECK,
        ELM_ROOT_KOE_ST,
        ELM_ROOT_KOE_WAIT,
        ELM_ROOT_EXKOE,
        ELM_ROOT_EXKOE_PLAY_WAIT,
        ELM_ROOT_EXKOE_PLAY_WAIT_KEY,
        ELM_ROOT_KOE_WAIT_KEY,
        ELM_ROOT_BGMTABLE,
        ELM_ROOT_KOE_SET_VOLUME,
        ELM_ROOT_KOE_SET_VOLUME_MAX,
        ELM_ROOT_KOE_SET_VOLUME_MIN,
        ELM_ROOT_KOE_GET_VOLUME,
        ELM_ROOT_KOE_CHECK_GET_KOE_NO,
        ELM_ROOT_KOE_CHECK_GET_CHARA_NO,
        ELM_ROOT_KOE_CHECK_IS_EX_KOE,
    };
    elm_register_roots(ROOTS, (int)COUNT_OF(ROOTS), snd_elm_global);
    proc_register(PROC_BGM_WAIT, proc_bgm_wait);
    proc_register(PROC_BGM_FADE_WAIT, proc_bgm_fade_wait);
    proc_register(PROC_KOE_WAIT, proc_koe_wait);
    proc_register(PROC_PCMCH_WAIT, proc_pcmch_wait);
    proc_register(PROC_PCMCH_FADE_WAIT, proc_pcmch_fade_wait);
}

static void push_wait(int type, bool key, const ElmCall *c, int ch)
{
    Proc p = { .type = type };
    p.key_skip = key;
    p.push_result = c->ret_form != ELM_FORM_NONE;        /* *_key variants return int via the proc (§2.5) */
    p.iarg[0] = ch;
    proc_push(&p);
}

/* ------------------------------------------------------------------------------------------------ arguments */

static void ret_int(const ElmCall *c, int32_t v)
{
    if (c->ret_form == ELM_FORM_NONE) return;
    if (c->ret_form == ELM_FORM_INT) vm_push_int(v);
    else elm_push_default(c);
}

static void ret_void(const ElmCall *c) { if (c->ret_form != ELM_FORM_NONE) elm_push_default(c); }

static int32_t pos_int(const ElmCall *c, int k, int32_t def) { return k < c->npos ? elm_arg_int(c, k, def) : def; }

static int32_t named_int(const ElmCall *c, int id, int32_t def)
{
    const Arg *a = elm_arg_named(c, id);
    return a ? a->i : def;
}

/* ------------------------------------------------------------------------------------------------ 42 bgm */

/* play / play_oneshot / play_wait: (name[, fade_in[, fade_out]]) + named 0 name, 1 loop, 2 wait, 3 start_pos,
 * 4 fade_in, 5 fade_out, 6 delay. */
static void bgm_play_cmd(int sub, const ElmCall *c)
{
    WStr name = {0};
    if (c->npos > 0) ws_copy(&name, elm_arg_str(c, 0));
    else ws_set_utf8(&name, "");
    const Arg *na = elm_arg_named(c, 0);
    if (na) ws_copy(&name, &na->s);
    bool loop = named_int(c, 1, sub == 0) != 0, wait = named_int(c, 2, sub == 2) != 0;
    int start = named_int(c, 3, -1), fade_in = named_int(c, 4, pos_int(c, 1, 0));
    int fade_out = named_int(c, 5, pos_int(c, 2, 0)), delay = named_int(c, 6, 0);
    bgm_play(&name, loop, wait, fade_in, fade_out, start, delay);
    ws_free(&name);
    ret_void(c);
}

static ElmResult bgm_cmd(int sub, ElmCall *c)
{
    switch (sub) {
    case ELM_BGM_PLAY:
    case ELM_BGM_PLAY_ONESHOT:
    case ELM_BGM_PLAY_WAIT: bgm_play_cmd(sub, c); return ELM_OK;
    case ELM_BGM_WAIT:
    case ELM_BGM_WAIT_KEY: push_wait(PROC_BGM_WAIT, sub == ELM_BGM_WAIT_KEY, c, 0); return ELM_OK;
    case ELM_BGM_WAIT_FADE:
    case ELM_BGM_WAIT_FADE_KEY: push_wait(PROC_BGM_FADE_WAIT, sub == ELM_BGM_WAIT_FADE_KEY, c, 0); return ELM_OK;
    case ELM_BGM_STOP:
        bgm_stop(pos_int(c, 0, DEFAULT_STOP_FADE_MS));
        ret_void(c);
        return ELM_OK;
    case ELM_BGM_SET_VOLUME:
        bgm_set_volume(pos_int(c, 0, VOLUME_MAX), pos_int(c, 1, 0));
        ret_void(c);
        return ELM_OK;
    case ELM_BGM_SET_VOLUME_MAX:
        bgm_set_volume(VOLUME_MAX, pos_int(c, 0, 0));
        ret_void(c);
        return ELM_OK;
    case ELM_BGM_SET_VOLUME_MIN:
        bgm_set_volume(0, pos_int(c, 0, 0));
        ret_void(c);
        return ELM_OK;
    case ELM_BGM_GET_VOLUME: ret_int(c, bgm_get_volume()); return ELM_OK;
    case ELM_BGM_PAUSE:
        bgm_pause(pos_int(c, 0, DEFAULT_STOP_FADE_MS));
        ret_void(c);
        return ELM_OK;
    case ELM_BGM_RESUME:
    case ELM_BGM_RESUME_WAIT:
        bgm_resume(named_int(c, 0, 0));
        if (sub == ELM_BGM_RESUME_WAIT) push_wait(PROC_BGM_WAIT, false, c, 0); else ret_void(c);
        return ELM_OK;
    case ELM_BGM_GET_PLAY_POS: ret_int(c, bgm_play_pos_ms()); return ELM_OK; /* [U] unit; unused by the game */
    case ELM_BGM_CHECK: ret_int(c, bgm_playing()); return ELM_OK;
    case ELM_BGM_GET_REGIST_NAME:
        if (c->ret_form == ELM_FORM_STR) vm_push_str(bgm_current_name()); else elm_push_default(c);
        return ELM_OK;
    default: return ELM_UNKNOWN;                    /* 16/17 ready: unused (§1.5) */
    }
}

/* ------------------------------------------------------------------------------------------------ 44 pcmch */

static void pcmch_named_arg(PcmchArgs *a, const Arg *g)
{
    switch (g->id) {
    case 0: a->loop = g->i != 0; break;
    case 1: a->wait = g->i != 0; break;
    case 2: a->fade_in = g->i; break;
    case 3: if (g->i >= -1 && g->i <= 31) a->volume_type = g->i; break;
    case 6: a->chara = g->i; break;
    case 7: ws_copy(&a->name, &g->s); break;
    case 8: a->koe_no = g->i; a->has_koe = true; break;
    case 9: a->se_no = g->i; break;
    case 10: ws_copy(&a->bgm_name, &g->s); break;
    default: break;                                 /* 4/5 bgm_fade targets, 11 ready, 12 random: unused */
    }
}

/* play / play_wait / play_loop (FUN_006660d0) */
static void pcmch_play_cmd(int ch, int sub, bool ok_ch, const ElmCall *c)
{
    PcmchArgs a = {.loop = sub == ELM_PCMCH_PLAY_LOOP,
                   .wait = sub == ELM_PCMCH_PLAY_WAIT,
                   .fade_in = pos_int(c, 1, 0),
                   .volume_type = CAT_PCM,
                   .chara = -1,
                   .koe_no = -1,
                   .se_no = -1};
    if (c->npos > 0) ws_copy(&a.name, elm_arg_str(c, 0));
    for (int k = c->npos; k < c->nall; k++) pcmch_named_arg(&a, &c->arg[k]);
    if (ok_ch) pcmch_play(ch, &a);
    ws_free(&a.name);
    ws_free(&a.bgm_name);
    ret_void(c);
}

static ElmResult pcmch_cmd(int ch, int sub, ElmCall *c)
{
    bool ok_ch = ch >= 0 && ch < PCMCH_CNT;         /* out of range -> no-op (FUN_006676a0) */
    switch (sub) {
    case ELM_PCMCH_PLAY:
    case ELM_PCMCH_PLAY_WAIT:
    case ELM_PCMCH_PLAY_LOOP: pcmch_play_cmd(ch, sub, ok_ch, c); return ELM_OK;
    case ELM_PCMCH_WAIT:
    case ELM_PCMCH_WAIT_KEY:
        if (ok_ch)
            push_wait(PROC_PCMCH_WAIT, sub == ELM_PCMCH_WAIT_KEY, c, ch);
        else
            ret_int(c, 0);
        return ELM_OK;
    case ELM_PCMCH_WAIT_FADE:
    case ELM_PCMCH_WAIT_FADE_KEY:
        if (ok_ch)
            push_wait(PROC_PCMCH_FADE_WAIT, sub == ELM_PCMCH_WAIT_FADE_KEY, c, ch);
        else
            ret_int(c, 0);
        return ELM_OK;
    case ELM_PCMCH_CHECK: ret_int(c, ok_ch && pcmch_playing(ch)); return ELM_OK;
    case ELM_PCMCH_STOP:
        if (ok_ch) pcmch_stop(ch, pos_int(c, 0, DEFAULT_STOP_FADE_MS));
        ret_void(c);
        return ELM_OK;
    case ELM_PCMCH_PAUSE:
        if (ok_ch) pcmch_pause(ch, pos_int(c, 0, DEFAULT_STOP_FADE_MS));
        ret_void(c);
        return ELM_OK;
    case ELM_PCMCH_RESUME:
    case ELM_PCMCH_RESUME_WAIT:
        if (ok_ch) pcmch_resume(ch, named_int(c, 0, 0));
        if (sub == ELM_PCMCH_RESUME_WAIT && ok_ch) push_wait(PROC_PCMCH_WAIT, false, c, ch); else ret_void(c);
        return ELM_OK;
    case ELM_PCMCH_GET_VOLUME: ret_int(c, ok_ch ? pcmch_get_volume(ch) : 0); return ELM_OK;
    case ELM_PCMCH_SET_VOLUME:
        if (ok_ch) pcmch_set_volume(ch, pos_int(c, 0, VOLUME_MAX), pos_int(c, 1, 0));
        ret_void(c);
        return ELM_OK;
    case ELM_PCMCH_SET_VOLUME_MAX:
        if (ok_ch) pcmch_set_volume(ch, VOLUME_MAX, pos_int(c, 0, 0));
        ret_void(c);
        return ELM_OK;
    case ELM_PCMCH_SET_VOLUME_MIN:
        if (ok_ch) pcmch_set_volume(ch, 0, pos_int(c, 0, 0));
        ret_void(c);
        return ELM_OK;
    default: return ELM_UNKNOWN;                    /* 11/16 ready: unused (§1.5) */
    }
}

static ElmResult pcmch_root(const int *rest, int nrest, ElmCall *c)
{
    if (nrest == 3 && rest[0] == -1) return pcmch_cmd(rest[1], rest[2], c);
    if (nrest == 1 && rest[0] == 1) {               /* pcmchlist.stop_all */
        for (int ch = 0; ch < PCMCH_CNT; ch++) pcmch_stop(ch, 0);
        ret_void(c);
        return ELM_OK;
    }
    return ELM_UNKNOWN;
}

/* ------------------------------------------------------------------------------------------------ koe */

/* 87/88/89 exkoe (FUN_0065be10): (koe_no, chara) + named 0 koe_no, 1 chara, 2 wait, 3 key. Named 4 (jitan)
 * selects the config voice volume (fee8+0x14c) [U]; 100 otherwise. */
static ElmResult exkoe(int root, ElmCall *c)
{
    int koe_no = named_int(c, 0, pos_int(c, 0, 0)), chara = named_int(c, 1, pos_int(c, 1, -1));
    bool wait = named_int(c, 2, root != ELM_ROOT_EXKOE) != 0,
         key = named_int(c, 3, root == ELM_ROOT_EXKOE_PLAY_WAIT_KEY) != 0;
    (void)koe_play(koe_no, chara, 100);             /* a missing voice is logged and the script goes on */
    if (wait) push_wait(PROC_KOE_WAIT, key, c, 0);
    else ret_int(c, 0);
    return ELM_OK;
}

static ElmResult koe_root(int root, int nrest, ElmCall *c)
{
    switch (root) {
    case ELM_ROOT_KOE_WAIT:
    case ELM_ROOT_KOE_WAIT_KEY: /* proc 0x1d; 99 returns 0 (end/skip) or 1 (click) */
        if (nrest) return ELM_UNKNOWN;
        push_wait(PROC_KOE_WAIT, root == ELM_ROOT_KOE_WAIT_KEY, c, 0);
        return ELM_OK;
    case ELM_ROOT_EXKOE:
    case ELM_ROOT_EXKOE_PLAY_WAIT:
    case ELM_ROOT_EXKOE_PLAY_WAIT_KEY: return nrest ? ELM_UNKNOWN : exkoe(root, c);
    case ELM_ROOT_KOE_STOP:
        koe_stop(pos_int(c, 0, DEFAULT_STOP_FADE_MS));
        ret_void(c);
        return ELM_OK;                                                 /* unused */
    case ELM_ROOT_KOE_CHECK: ret_int(c, koe_playing()); return ELM_OK; /* unused */
    case ELM_ROOT_KOE_SET_VOLUME:
        koe_set_volume(pos_int(c, 0, VOLUME_MAX), pos_int(c, 1, 0));
        ret_void(c);
        return ELM_OK;
    case ELM_ROOT_KOE_SET_VOLUME_MAX:
        koe_set_volume(VOLUME_MAX, pos_int(c, 0, 0));
        ret_void(c);
        return ELM_OK;
    case ELM_ROOT_KOE_SET_VOLUME_MIN:
        koe_set_volume(0, pos_int(c, 0, 0));
        ret_void(c);
        return ELM_OK;
    case ELM_ROOT_KOE_GET_VOLUME: ret_int(c, koe_get_volume()); return ELM_OK; /* unused */
    default: return ELM_UNKNOWN;
    }
}

static ElmResult snd_elm_global(int root, const int *rest, int nrest, ElmCall *c)
{
    if (root == ELM_ROOT_BGM) return nrest == 1 ? bgm_cmd(rest[0], c) : ELM_UNKNOWN;
    if (root == ELM_ROOT_PCMCH) return pcmch_root(rest, nrest, c);
    return koe_root(root, nrest, c);
}
