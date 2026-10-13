/* vm/vm.h — interpreter, stacks, frames, banks (VM spec, ENGINE_SPEC §2.4.2). Cross-module API
 * (docs/CODE_STANDARD.md §12); module internals are in vm/vm_internal.h.
 *
 * Untrusted bytecode never crashes the engine: a bad index, stack under/overflow or truncated code stops the script
 * with VM_HALT_ERROR (reason "VM ERROR ...", vm_halt_reason) and the frame driver pushes PROC_HALT. Main thread
 * only. */
#ifndef VM_VM_H
#define VM_VM_H
#include "core/str.h"
#include "sav/sav_stream.h"
#include "fmt/pack.h"
#include "vm/elements.h"
#include <stdio.h>

/* Observers the application installs (the headless harness); every member is optional. They keep the vm free of
 * any dependency on the modules above it. */
typedef struct VmHooks {
    bool (*on_step)(void);                     /* before every instruction of vm_run; true = stop for this frame */
    void (*on_text)(int text_no);              /* after every TEXT opcode (1-based count) */
    void (*on_stop)(const char *reason);       /* vm_run found the script halted */
    void (*on_error)(const char *reason);      /* ... and it was VM_HALT_ERROR */
    void (*on_unknown)(const char *chain);     /* an element chain no module handles (outside the stub list) */
} VmHooks;
void  vm_set_hooks(const VmHooks *h);            /* copied; NULL = none */
/* The text opcodes TEXT (0x31) and NAME (0x32) belong to the text module, which installs them (each pops the
 * string from the stack); without them the vm drops the string. */
typedef struct VmTextOps { void (*text)(int read_flag); void (*name)(void); } VmTextOps;
void  vm_set_text_ops(const VmTextOps *ops);     /* copied; NULL = none */
const VmHooks *vm_hooks(void);                   /* never NULL (members may be) */

bool  vm_init(const Pack *pk);                   /* binds pk (re-binding frees the previous state); false if empty */
void  vm_shutdown(void);                         /* frees everything the vm module owns (vm, proc, sysstate) */
bool  vm_start(int scene, int z);                /* fresh call stack (depth 1), clears stacks; false (nothing changed)
                                                    for a scene outside the bound pack */
void  vm_run(void);                              /* runs until proc_top()->type != PROC_SCRIPT */
void  vm_push_int(int32_t v);  void vm_push_str(const WStr *s);
int32_t vm_pop_int(void);      void vm_pop_str(WStr *out);
bool vm_str_pop_swap(WStr *out);   /* pop by swapping buffers (no copy); false and out = "" on underflow */
int   vm_scene(void); int vm_line(void); uint32_t vm_pc(void);
bool  vm_restart_at(int scene, int z);           /* proc 8: local banks reset + vm_start; false = bad scene, no reset */
void  vm_restart_menu(void);                     /* proc 8: the same at #MENU_SCENE z0 (scene 0 if it is missing) */
int32_t *vm_bank_int(int root, int idx);         /* 25..32,137; NULL + log if out of range */
WStr    *vm_bank_str(int root, int idx);         /* 34,35,106,107 */
void vm_save(SavWriter *w);
bool vm_load(SavReader *r); /* stacks, frames, pc, scene NAME, banks(local), scene props */
void  vm_save_global(SavWriter *w); bool vm_load_global(SavReader *r);   /* G, Z, M, namae */
int   vm_stack_scenes(int *out, int max);        /* scenes on the call stack (resume points, then current); count */

const Pack *vm_pack(void);
int   vm_find_scene(const WStr *name);           /* ASCII-folded; -1 if missing */
const char *vm_scene_name_utf8(int scene);       /* cached; "" if out of range */
int   vm_depth(void);                            /* call-stack depth (>= 1) */
int64_t vm_steps(void);                          /* executed instructions since vm_init */
int64_t vm_text_count(void);                     /* TEXT opcodes executed since vm_init */
/* Why the script stopped: VM_HALT_STOP for an end the script or the runner chose (EOF, opcode 0, empty RETURN,
 * missing scene, limits), VM_HALT_ERROR for corrupt bytecode or state (vm_fault). The reason text is for logs. */
typedef enum { VM_HALT_NONE, VM_HALT_STOP, VM_HALT_ERROR } VmHaltKind;
bool  vm_halted(void);  const char *vm_halt_reason(void);
VmHaltKind vm_halt_kind(void);
int64_t vm_run_steps(int64_t max);                 /* like vm_run but at most max instructions; returns the count */
void vm_err(const char *fmt, ...) CORE_PRINTF(1, 2); /* non-fatal script error (exe FUN_00538220): logged + counted */
int   vm_err_count(void);                         /* legacy WP2 drivers only (the harness counts its own) */

/* vm_ref.py-compatible runner (M1): used by the harness for --vm-stubs runs and --sweep.
 * Output goes to `out` in exactly the vm_ref.py stdout format. Returns 0 if no VM error. */
typedef struct VmRefOpts {
    const char *scene;
    int z;             /* NULL = "_start" */
    int64_t max_steps; /* default 2000000 (sweep: 300000 as sweep.txt) */
    int64_t max_texts; /* 0 = unlimited */
    int sel;           /* answer for every selmsg (default 1) */
    bool quiet, stop_on_jump;
} VmRefOpts;
int   vm_ref_run(const Pack *pk, const VmRefOpts *o, FILE *out);   /* out NULL = stdout */
int   vm_ref_sweep(const Pack *pk, int64_t max_steps, int sel, FILE *out);

#endif
