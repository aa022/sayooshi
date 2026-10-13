/* fmt/pack_walk.c — static disassembly of a scene (VM §2, §9): decodes every instruction to EOF with the
 * deterministic inline-read rule and counts COMMAND / TEXT / NAME instructions. Used by the M0 self-test and the
 * fuzz harness; the VM itself does not use it.
 *
 * A simulated int stack tracks constants from PUSH 10 so that element chains can be read; computed values are
 * WALK_UNKNOWN placeholders. Statement boundaries (NL) reset the stacks (VM §2.3: every statement is balanced).
 * The code is untrusted: every read goes through Rd, stacks are fixed-size and saturate, nested argument lists
 * are capped at WALK_ARG_DEPTH, and every instruction consumes at least one byte. */
#include "fmt/pack.h"
#include "core/bin.h"
#include <string.h>

enum {
    WALK_STACK = 4096,                   /* simulated int stack entries */
    WALK_MARKS = 256,                    /* element-point marks */
    WALK_CHAIN = 32,                     /* element chain entries examined */
    WALK_ARG_DEPTH = 8,                  /* nested argument lists */
    WALK_ARGS_MAX = 4096,                /* forms per argument list */
    WALK_NAMED_MAX = 1024,               /* named arguments of a COMMAND */
    FORM_INT = 10, FORM_STR = 20, FORM_INTLIST_REF = 11, FORM_STRLIST_REF = 21, FORM_INTREF = 30,
    FORM_ARGLIST = -1
};
#define WALK_UNKNOWN ((int32_t)INT32_MIN)

typedef struct { int32_t is[WALK_STACK]; int isp; int marks[WALK_MARKS]; int nmark; int ssp; } WalkStk;
typedef struct { Rd r; WalkStk *s; SceneStats *st; bool eof; } Walk;

/* ---- simulated stacks ---- */

static int top_mark(const WalkStk *s) { return s->nmark ? s->marks[s->nmark - 1] : 0; }
static void ipush(WalkStk *s, int32_t v) { if (s->isp < WALK_STACK) s->is[s->isp++] = v; }
static void ipop(WalkStk *s) { if (s->isp > top_mark(s)) s->isp--; }
static void spop(WalkStk *s) { if (s->ssp > 0) s->ssp--; }
static void mark(WalkStk *s) { if (s->nmark < WALK_MARKS) s->marks[s->nmark++] = s->isp; }

/* Pops the element chain above the newest mark; copies up to max entries into chain; returns its length. */
static int pop_chain(WalkStk *s, int32_t *chain, int max)
{
    int m = s->nmark ? s->marks[--s->nmark] : 0;
    if (m > s->isp) m = s->isp;
    int n = s->isp - m;
    for (int i = 0; i < n && i < max; i++) chain[i] = s->is[m + i];
    s->isp = m;
    return n;
}

static void pop_form(WalkStk *s, int32_t form)
{
    int32_t chain[WALK_CHAIN];
    if (form == FORM_INT || form == FORM_INTREF) ipop(s);
    else if (form == FORM_STR) spop(s);
    else (void)pop_chain(s, chain, WALK_CHAIN);
}

/* Argument list: a count, then forms stored last-first; FORM_ARGLIST opens a nested list (VM §2.2). Values are
 * popped as they are read. Iterative with an explicit depth stack. */
static void arglist(Walk *w)
{
    int32_t left[WALK_ARG_DEPTH + 1];
    int d = 0;
    left[0] = rd_i32le(&w->r);
    if (left[0] < 0 || left[0] > WALK_ARGS_MAX) { rd_fail(&w->r); return; }
    while (d >= 0 && rd_ok(&w->r)) {
        if (left[d] == 0) { d--; continue; }
        left[d]--;
        int32_t f = rd_i32le(&w->r);
        if (f != FORM_ARGLIST) { pop_form(w->s, f); continue; }
        int32_t n = rd_i32le(&w->r);
        if (n < 0 || n > WALK_ARGS_MAX || d == WALK_ARG_DEPTH) { rd_fail(&w->r); return; }
        left[++d] = n;
    }
}

/* VM §9: commands whose last argument is read inline from the code stream. */
static bool inline_read_rule(const int32_t *ch, int n)
{
    if (n < 1) return false;
    switch (ch[0]) {
    case 12: case 18: case 19: case 76: case 77: case 100: case 101: case 102: case 127: return true;
    default: break;
    }
    if (n >= 3 && ch[0] == 49 && ch[1] == -1) { ch += 2; n -= 2; }   /* 49.-1.s -> stage s */
    if (n >= 5 && (ch[0] == 37 || ch[0] == 38 || ch[0] == 73) && ch[1] == 3 && ch[2] == -1) {
        switch (ch[4]) {
        case 4: case 5: case 9: case 50: case 51: case 52: return true;
        default: break;
        }
    }
    return false;
}

/* ---- instructions with operands ---- */

static void op_copy_elm(WalkStk *s)
{
    int m = top_mark(s), n = s->isp - m;
    mark(s);
    for (int i = 0; i < n; i++) ipush(s, s->is[m + i]);
}

static void op_op2(Walk *w)
{
    int32_t a = rd_i32le(&w->r), b = rd_i32le(&w->r);
    uint8_t o = rd_u8(&w->r);
    WalkStk *s = w->s;
    if (a == FORM_INT && b == FORM_INT) { ipop(s); ipop(s); ipush(s, WALK_UNKNOWN); }
    else if (a == FORM_STR && b == FORM_INT) { ipop(s); if (o != 3) spop(s); }
    else if (a == FORM_STR && b == FORM_STR) { spop(s); if (o != 1) { spop(s); ipush(s, WALK_UNKNOWN); } }
}

static void op_command(Walk *w)
{
    int32_t chain[WALK_CHAIN];
    (void)rd_i32le(&w->r);
    arglist(w);
    int n = pop_chain(w->s, chain, WALK_CHAIN);
    int32_t named = rd_i32le(&w->r);
    if (named < 0 || named > WALK_NAMED_MAX) { rd_fail(&w->r); return; }
    for (int32_t i = 0; i < named; i++) (void)rd_i32le(&w->r);
    int32_t ret_form = rd_i32le(&w->r);
    w->st->ncommand++;
    if (inline_read_rule(chain, n < WALK_CHAIN ? n : WALK_CHAIN)) { (void)rd_i32le(&w->r); w->st->ninline++; }
    if (ret_form == FORM_INT) ipush(w->s, WALK_UNKNOWN);
    else if (ret_form == FORM_STR) w->s->ssp++;
}

static void op_push(Walk *w)
{
    int32_t a = rd_i32le(&w->r), b = rd_i32le(&w->r);
    if (a == FORM_INT) ipush(w->s, b);
    else if (a == FORM_STR) w->s->ssp++;
}

static void op_copy(Walk *w)
{
    int32_t a = rd_i32le(&w->r);
    WalkStk *s = w->s;
    if (a == FORM_INT) ipush(s, s->isp ? s->is[s->isp - 1] : WALK_UNKNOWN);
    else if (a == FORM_STR) s->ssp++;
}

/* One instruction. Unknown opcodes desynchronise the walk (the reader fails). */
static void walk_step(Walk *w, uint8_t op)
{
    WalkStk *s = w->s;
    int32_t a, b, chain[WALK_CHAIN];
    switch (op) {
    case 0x01: (void)rd_i32le(&w->r); s->isp = s->ssp = s->nmark = 0; break;        /* NL */
    case 0x02: op_push(w); break;
    case 0x03: a = rd_i32le(&w->r); if (a == FORM_INT) ipop(s); else if (a == FORM_STR) spop(s); break;   /* POP */
    case 0x04: op_copy(w); break;
    case 0x05: (void)pop_chain(s, chain, WALK_CHAIN); ipush(s, WALK_UNKNOWN); break;  /* PROPERTY */
    case 0x06: op_copy_elm(s); break;
    case 0x07: a = rd_i32le(&w->r); (void)rd_i32le(&w->r);                            /* DEC_PROP */
               if (a == FORM_INTLIST_REF || a == FORM_STRLIST_REF) ipop(s);
               break;
    case 0x08: mark(s); break;                                                        /* ELM_POINT */
    case 0x09: case 0x33: case 0x34: break;                                           /* ARG, ... */
    case 0x10: (void)rd_i32le(&w->r); break;
    case 0x11: case 0x12: (void)rd_i32le(&w->r); ipop(s); break;
    case 0x13: case 0x14: (void)rd_i32le(&w->r); arglist(w); ipush(s, WALK_UNKNOWN); break;
    case 0x15: arglist(w); break;
    case 0x16: w->eof = true; break;
    case 0x20: (void)rd_i32le(&w->r); b = rd_i32le(&w->r); (void)rd_i32le(&w->r);   /* ASSIGN */
               pop_form(s, b);
               (void)pop_chain(s, chain, WALK_CHAIN);
               break;
    case 0x21: a = rd_i32le(&w->r); b = rd_u8(&w->r);                                 /* OP1 */
               if (a == FORM_INT && b != 1 && b != 2 && b != 0x30) ipop(s);
               break;
    case 0x22: op_op2(w); break;
    case 0x30: op_command(w); break;
    case 0x31: (void)rd_i32le(&w->r); spop(s); w->st->ntext++; break;                 /* TEXT */
    case 0x32: spop(s); w->st->nname++; break;                                        /* NAME */
    default: rd_fail(&w->r); break;
    }
}

void scene_walk(const Scene *sc, SceneStats *st)
{
    memset(st, 0, sizeof *st);
    if (!sc) return;
    Scratch tmp = mem_scratch_begin(NULL);
    Walk w = { rd_make(sc->code, sc->code_size), ARENA_NEW(tmp.arena, WalkStk), st, false };
    while (rd_left(&w.r) > 0 && !w.eof) {             /* every instruction consumes its opcode byte */
        uint8_t op = rd_u8(&w.r);
        st->nop++;
        walk_step(&w, op);
    }
    st->end_pc = rd_ok(&w.r) ? (uint32_t)w.r.pos : sc->code_size;
    st->ok = rd_ok(&w.r) && w.eof && w.r.pos == w.r.n;
    mem_scratch_end(&tmp);
}
