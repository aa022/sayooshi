/* vm/elements.h — global element dispatcher, root ownership, ElmCall/Arg helpers, list protocols (VM §2.2, §4.1;
 * ENGINE_SPEC §1.1, §1.5, §2.5). vm-internal list objects and formatting: vm/vm_internal.h. */
#ifndef VM_ELEMENTS_H
#define VM_ELEMENTS_H
#include "core/str.h"
#include "vm/elm_ids.h"

/* Value forms of the bytecode (VM §2.1): Arg.form, ElmCall.ret_form, list/ref declarations. */
enum {
    ELM_FORM_ARGLIST = -1, ELM_FORM_NONE = 0,
    ELM_FORM_INT = 10, ELM_FORM_INTLIST = 11, ELM_FORM_INTREF = 13, ELM_FORM_INTLISTREF = 14,
    ELM_FORM_STR = 20, ELM_FORM_STRLIST = 21, ELM_FORM_STRREF = 23, ELM_FORM_STRLISTREF = 24,
    ELM_FORM_LABEL = 30
};

typedef struct ArgList ArgList;
typedef struct Arg { int id; int form; int32_t i; WStr s; int chain[32]; int nchain; ArgList *sub; } Arg;
struct ArgList { Arg *v; int n; };
enum { OP_PROPERTY, OP_ASSIGN, OP_COMMAND };
typedef struct ElmCall {
    int op;    /* OP_PROPERTY (al 0, get) / OP_ASSIGN (al 1, args[0]=rhs) / OP_COMMAND */
    int al_id; /* overload id */
    Arg *arg;
    int npos;
    int nall;      /* positional = arg[0..npos), named = arg[npos..nall) (id field set) */
    int ret_form;  /* ELM_FORM_NONE, ELM_FORM_INT, ELM_FORM_STR, ELM_FORM_STRREF ... */
    void *ret_obj; /* set when a chain ends on an object (rare; copy/ref args) */
} ElmCall;
typedef enum { ELM_OK, ELM_UNKNOWN } ElmResult;

int32_t     elm_arg_int(const ElmCall *c, int k, int32_t def);            /* positional k of form int, else def */
const WStr *elm_arg_str(const ElmCall *c, int k);                         /* never NULL (empty string if absent) */
/* Form-agnostic reads, as the object handlers of the exe read their arguments: the int / string field of
 * positional k whatever its form (def / "" when absent). */
int32_t     elm_arg_raw_int(const ElmCall *c, int k, int32_t def);
const WStr *elm_arg_raw_str(const ElmCall *c, int k);
const Arg  *elm_arg_named(const ElmCall *c, int id);                      /* NULL if absent */
int32_t     vm_read_inline_i32(void);                                 /* VM §9: ONLY for 18 and 100 in this game */
ElmResult   elm_dispatch(const int *chain, int n, ElmCall *c);        /* the global dispatcher */
void        elm_push_default(const ElmCall *c);                       /* push 0 / "" by ret_form */
/* Results by the element contract (ENGINE_SPEC §2.5): exactly one value of ret_form, a PROPERTY read pushes by
 * the element's own type, an ASSIGN pushes nothing. */
void        elm_ret_int(const ElmCall *c, int32_t v);
void        elm_ret_str(const ElmCall *c, const WStr *s);
void        elm_ret_none(const ElmCall *c);

/* Root ownership (ENGINE_SPEC §1.1): each module registers the roots it implements when it initialises; the
 * dispatcher calls the owner with the rest of the chain. A root has one owner (a second owner is a bug). Roots
 * without an owner are unknown ("no global case", FUN_00659170 default). */
enum { ELM_ROOT_DISPATCH_MAX = 0xa3 };     /* the dispatcher's jump table covers roots 0..0xa3 */
typedef ElmResult (*ElmRootFn)(int root, const int *rest, int nrest, ElmCall *c);
void        elm_register_roots(const int *roots, int n, ElmRootFn fn);

/* Shared list protocol (VM §6.1/6.2) for modules that expose a fixed int list of n words (cgtable flags ...). */
ElmResult elm_intlist(int32_t *w, int n, const int *rest, int nrest, ElmCall *c);

/* Resizable list objects (VM §6.1/6.2) of the VM: banks, L/K, list locals and scene props. */
typedef struct { int32_t *w; int n, def; bool resizable; } IntList;   /* n = number of 32-bit words */
typedef struct { WStr *s; int n, def; bool resizable; } StrList;

int       elm_hw_len(const WStr *s);       /* str.len: half-width units (CP932 1/2 bytes, VM §6.2); exported for the
                                             legacy WP2 test driver */

/* --vm-stubs (ENGINE_SPEC §3.1, M1): every non-VM root is replaced by a vm_ref.py-compatible stub. */
void      elm_set_vm_stubs(bool on, int sel);
bool      elm_vm_stubs(void);
int       elm_unk_count(void);   /* UNK chains seen; legacy WP2 drivers only (--strict uses the harness's counter) */

#endif
