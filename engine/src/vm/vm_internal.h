/* vm/vm_internal.h — declarations shared by the files of the vm module only (docs/CODE_STANDARD.md §2).
 *
 *  vm.c        interpreter: code stream, position, opcodes, errors/stop, run loops, init/start
 *  vm_stack.c  value stacks (int, str, element marks) and the per-instruction argument pool
 *  vm_store.c  call frames + locals, banks, scene/include props, the per-pack tables (one private arena)
 *  vm_elm.c    VM-level element roots: banks, call locals, jump/farcall, namae, user props/commands
 *  vm_save.c   VM02/VMG2 save chunks (VM01/VMG1 still read)
 *  vm_ref.c    the vm_ref.py-compatible --vm-stubs dispatcher, runner and sweep (M1)
 *  elements.c  global dispatcher, ElmCall argument helpers; elm_list.c / elm_str.c / elm_fmt.c: list objects,
 *              str methods, vm_ref/trace formatting
 *
 * Memory: every container of the interpreter has a fixed capacity (VM_*_MAX below) and lives in static storage;
 * overflowing one is a recoverable VM error (the script stops, the engine keeps running). Per-pack tables live in
 * one arena re-created by vm_init. Script strings (WStr) and resizable list objects use the tracked heap and are
 * released by vm_shutdown. Main thread only. */
#ifndef VM_VM_INTERNAL_H
#define VM_VM_INTERNAL_H
#include "core/bin.h"
#include "core/mem.h"
#include "core/str.h"
#include "fmt/pack.h"
#include "sav/sav_stream.h"
#include "vm/elements.h"
#include "vm/vm.h"
#include <stdarg.h>

/* The value forms of the bytecode are ELM_FORM_* (vm/elements.h). */

/* ---- fixed capacities ----------------------------------------------------------------------------------------
 * The game stays far below every limit (deepest call stack in the M1 sweep: 6 frames; 0 VM errors). Untrusted
 * bytecode or save data that would exceed one gets a recoverable VM error / load failure instead of growth. */
enum {
    VM_CHAIN_MAX = 32,            /* element chain length (= Arg.chain, Proc.chain) */
    VM_ISTACK_MAX = 1 << 16,      /* int stack entries */
    VM_SSTACK_MAX = 1 << 12,      /* str stack entries */
    VM_MARK_MAX = 1 << 14,        /* element marks */
    VM_FRAME_MAX = 256,           /* call frames */
    VM_LOCAL_MAX = 1 << 12,       /* declared call locals over all live frames */
    VM_ARG_MAX = 1024,            /* arguments of one instruction, nested lists included */
    VM_SUBLIST_MAX = 128,         /* nested argument lists of one instruction */
    VM_ARGLIST_LEN_MAX = 256,     /* arguments in one list */
    VM_ARGLIST_DEPTH_MAX = 8,     /* nesting of argument lists */
    VM_NAMED_MAX = 64,            /* named arguments of one command */
    VM_PROP_MAX = 1 << 16,        /* scene + include props of one pack */
    VM_UTF8_MAX = 1 << 20,        /* bytes of one script string converted for a log/trace line */
    ELM_LIST_MAX = 1 << 20,       /* words / strings of one list object (also the save limit) */
    ELM_DISPATCH_DEPTH_MAX = 16   /* nested elm_dispatch calls (reference arguments re-dispatch) */
};
CORE_STATIC_ASSERT(sizeof(((Arg *)0)->chain) == VM_CHAIN_MAX * sizeof(int), "Arg.chain must hold VM_CHAIN_MAX");

/* ---- vm.c: position, errors, stream ---------------------------------------------------------------------- */
typedef struct VmPos { int scene, line; uint32_t pc; } VmPos;
typedef struct VmStats { int64_t steps, texts; int nl_viol, errs; } VmStats;   /* 64-bit: long is 32-bit on Windows */

VmPos vm_pos(void);
void  vm_set_pos(VmPos p);                     /* p.scene must be a valid scene index */
void  vm_jump(int scene, uint32_t pc);         /* line kept; a bad pc faults at the next read */
bool  vm_goto_z(int scene, int32_t z);         /* z label of a valid scene; false if z >= 1000 */
const VmStats *vm_stats(void);
void  vm_step(void);                           /* one instruction */
void  vm_reset_stacks(void);                   /* empty value stacks, call stack = [frame 0] */
void  vm_clear_stop(void);
bool  vm_inline_pending(int32_t *v);           /* the inline read-flag word of the running command */

/* Offset tables point into the scene blob and may be unaligned: read them bytewise (little-endian). */
static inline int32_t vm_tab(const int32_t *t, int i)
{ return (int32_t)bin_load_le32((const uint8_t *)t + 4 * (size_t)i); }
/* Recoverable failures. vm_stop: the script cannot continue (vm_ref VMStop: EOF, RETURN at depth 1, missing scene).
 * vm_fault: corrupt bytecode or state (vm_ref VMError, "VM ERROR <type>: ..."). Both halt the script once. */
void  vm_stop(const char *fmt, ...) CORE_PRINTF(1, 2);
void  vm_fault(const char *type, const char *fmt, ...) CORE_PRINTF(2, 3);

/* ---- vm_stack.c: value stacks and the argument pool -------------------------------------------------------- */
typedef struct VmDepths { int isp, ssp, nmk; } VmDepths;

VmDepths vm_depths(void);
void  vm_stacks_clear(void);
WStr *vm_str_push_slot(void);                  /* new top entry (old contents); NULL after a fault on overflow */
const WStr *vm_str_top(void);                  /* NULL when empty */
bool  vm_str_drop(void);
bool  vm_dup_int(void);                        /* COPY: duplicate the top entry */
bool  vm_dup_str(void);
void  vm_mark_push(void);                      /* ELM_POINT: mark the current int stack depth */
void  vm_push_ref(const int *chain, int n);    /* mark + chain (FUN_005a8d10) */
int   vm_pop_chain(int out[VM_CHAIN_MAX]);     /* chain above the newest mark (FUN_005a8da0); 0 after a fault */
bool  vm_copy_elm(void);                       /* COPY_ELM (FUN_005a8e00) */
void  vm_stacks_save(SavWriter *w);
bool  vm_stacks_load(SavReader *r);            /* validates counts and marks; false = reader failed */
Arg     *vm_args_alloc(int n);                 /* n fresh args (id -1, int 0); NULL after a fault */
ArgList *vm_sublist_alloc(void);               /* NULL after a fault */
void  vm_args_release(void);                   /* end of the instruction */
Arg  *vm_rhs(void);                            /* the ASSIGN right-hand side, reset */
void  vm_stacks_shutdown(void);

/* ---- vm_store.c: frames, locals, banks, props, per-pack tables -------------------------------------------- */
typedef struct Prop {                          /* 0x16c-byte Prop record (VM §6.4) */
    int form, name;
    int32_t i;
    WStr s;
    int chain[VM_CHAIN_MAX], nchain;
    IntList il; StrList sl;                    /* forms 11 / 21 */
} Prop;

typedef struct Frame {                         /* 0x30c-byte call frame (VM §1.3) */
    int type, ret_form;                        /* +0x2c8, +0x2d0 */
    VmPos rs;                                  /* resume point saved when this frame calls (+0x2e0..+0x2fc) */
    IntList L; StrList K;                      /* +0x158 / +0x210 */
    int loc0, nloc;                            /* locals: the pool entries [loc0, loc0 + nloc) (+0xa0) */
    VmDepths base;                             /* stack depths at entry (NL balance check, VM §2.3) */
} Frame;

enum { VM_FRAME_BASE = 0, VM_FRAME_GOSUB = 1, VM_FRAME_FARCALL = 2, VM_FRAME_USERCMD = 3 };   /* Frame.type */
enum { VM_INT_BANKS = 9, VM_STR_BANKS = 4, VM_LOCAL_INT_BANKS = 7 };   /* A..F X | G Z ; S M ％namae ＊namae */
enum { VM_BANK_S = 0, VM_BANK_M = 1, VM_BANK_NAMAE_LOCAL = 2, VM_BANK_NAMAE_GLOBAL = 3, VM_BANK_G = 7, VM_BANK_Z = 8 };

bool  vm_store_bind(const Pack *pk);           /* per-pack tables, banks, frame 0; false (unbound) on bad data */
void  vm_store_unbind(void);                   /* frees everything bound (also the frames' and banks' heap data) */
int   vm_find_scene_utf8(const char *name);

void   vm_prop_reset(Prop *p, int form, int size, int name);
void   vm_prop_free(Prop *p);
Frame *vm_frame_top(void);
Frame *vm_frame_at(int d);                     /* 0 <= d < vm_depth() */
Frame *vm_frame_push(int type, int ret_form);  /* saves the current position as the caller's resume point */
void   vm_frame_set_depth(int depth);          /* 1 <= depth <= current depth */
Frame *vm_frame_load(int d);                   /* load: frame d (== depth, or 0) reset and on top; NULL at the limit */
void   vm_frames_reset(void);                  /* depth 1, frame 0 reset */
void   vm_frame_set_base(Frame *f);
void   vm_frame_pass_args(Frame *f, const Arg *a, int n);   /* int args -> L[0..], str args -> K[0..] */
Prop  *vm_frame_add_local(Frame *f);           /* f must be the top frame; NULL at VM_LOCAL_MAX (no fault) */
Prop  *vm_frame_local(const Frame *f, int i);  /* 0 <= i < f->nloc */
int    vm_frame_max_depth(void);
IntList *vm_int_bank(int b);                   /* 0 <= b < VM_INT_BANKS */
StrList *vm_str_bank(int b);
int    vm_int_bank_index(int root);            /* -1 if root is not an int bank */
int    vm_str_bank_index(int root);
void   vm_local_banks_reset(void);
int    vm_scene_prop_count(int scene);
Prop  *vm_scene_prop(int scene, int k);        /* 0 <= k < vm_scene_prop_count(scene) */
const PropDecl *vm_scene_prop_decl(int scene, int k);
void   vm_scene_prop_redeclare(int scene, int k);
Prop  *vm_inc_prop(int k);                     /* NULL if out of range */
int    vm_inc_prop_count(void);

/* ---- vm_ref.c: --vm-stubs runner hooks ----------------------------------------------------------------------- */
typedef enum { VM_REF_JUMP, VM_REF_FARCALL, VM_REF_USERCMD } VmRefEvent;
bool  vm_ref_logging(void);                    /* a vm_ref log stream is open */
void  vm_ref_log(const char *fmt, ...) CORE_PRINTF(1, 2);   /* "  [scene:line] ..." */
void  vm_ref_text(int rf, const char *speaker, bool has_speaker, const char *text);
void  vm_ref_count(VmRefEvent e);
void  vm_ref_note_err(const char *msg);
void  vm_ref_note_nl(uint32_t at, VmDepths now, VmDepths base);
void  vm_ref_reset(void);                      /* vm_init: per-run counters */
void  vm_ref_shutdown(void);

/* ---- element internals ----------------------------------------------------------------------------------------- */
ElmResult vm_elm_global(int root, const int *rest, int nrest, ElmCall *c);
void      elm_flow_register(void);             /* elm_flow.c: claims the vm's roots (vm_init) */
void      sys_cg_register(void);               /* cgflags.c: claims root 78 cgtable (vm_init) */
ElmResult vm_user_prop(int idx, const int *rest, int nrest, ElmCall *c);   /* 0x7F000000|idx (VM §6.5) */
ElmResult vm_user_cmd(int idx, ElmCall *c);                                /* 0x7E000000|idx (VM §7.3) */
ElmResult vm_stub_dispatch(const int *chain, int n, ElmCall *c);           /* --vm-stubs (vm_ref.py h_stub) */
void      vm_stub_set_sel(int sel);
void      vm_stub_restart(void);                                           /* vm_ref.py root 3 al0 */

void      elm_intlist_init(IntList *l, int n, bool resizable);   /* zero-filled; n clamped to [0, ELM_LIST_MAX] */
void      elm_intlist_free(IntList *l);
void      elm_intlist_resize(IntList *l, int n);
void      elm_intlist_zero(IntList *l);
void      elm_strlist_init(StrList *l, int n, bool resizable);
void      elm_strlist_free(StrList *l);
void      elm_strlist_resize(StrList *l, int n);
void      elm_strlist_clear(StrList *l);        /* every string emptied, size kept */
ElmResult elm_intlist_obj(IntList *l, const int *rest, int nrest, ElmCall *c);   /* FUN_00660880 */
ElmResult elm_strlist_obj(StrList *l, const int *rest, int nrest, ElmCall *c);   /* FUN_006614c0 */
ElmResult elm_str(WStr *s, const int *rest, int nrest, ElmCall *c);              /* str methods FUN_00661620 */

/* vm_ref.py / trace §3.2 text ("42.0 al0 args=['bgm11']"); truncated at a code point when b is full. */
void      elm_fmt_call(StrBuf *b, const int *chain, int nchain, const ElmCall *c);
void      elm_fmt_chain(StrBuf *b, const int *chain, int nchain);
void      elm_fmt_args(StrBuf *b, const Arg *a, int cnt);                 /* "[0, 'x', [1, 2]]" */
void      elm_repr_wstr(StrBuf *b, const WStr *s);                        /* Python repr() of a str */

#endif
