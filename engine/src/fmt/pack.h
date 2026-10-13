/* fmt/pack.h — Scene.pck (FORMATS "Scene.pck header", "Scene blob header"; exe FUN_0054b960).
 *
 * Memory: a loaded Pack lives in the data arena (mem_data(): its lifetime is the loaded language). Everything it
 * points to — scenes, decoded blobs, int tables, name strings — is read-only and owned by the Pack. The names are
 * views into that memory, read through pack_scene_name / pack_inc_cmd_name as `const WStr *` (copy them with
 * ws_copy; they cannot be ws_set or ws_free'd). Fields ending in '_' belong to fmt/pack*.c.
 * pack_free releases the memory at once when the Pack is still the newest data-arena allocation (a temporary
 * Pack), and otherwise leaves it to the next mem_data_reset(); call it before that reset either way. */
#ifndef FMT_PACK_H
#define FMT_PACK_H
#include "core/mem.h"
#include "core/str.h"

typedef struct { int form; int name_id; int size; } PropDecl;                    /* scene prop (form 10/20/11/21) */
typedef struct {
    WStr name_;    /* view (pack_scene_name) */
    uint8_t *blob; /* decrypted + decompressed scene blob */
    const uint8_t *code;
    uint32_t code_size;
    const int32_t *label;
    int nlabel;
    const int32_t *zlabel; /* 1000 entries, 0 = absent */
    const int32_t *scn_cmd;
    int nscn_cmd; /* code offsets of scene-local user commands */
    PropDecl *prop;
    int nprop;
    int nstr;
    int nread_flag;
    uint32_t blob_size;
    const int32_t *read_flag_line; /* [nread_flag]: source line of read flag i */
    const int32_t *namae;
    int nnamae; /* namae table (unused by this game: all 0) */
    const int32_t *cmd_label;
    int ncmd_label; /* {cmd_id, offset} pairs, [2*ncmd_label] */
    int ncall_prop_name;
    struct {                              /* fmt/pack.c only */
        int32_t *tabs;                    /* aligned copy of the int tables above */
        const uint8_t *str_idx, *str;     /* string index / pool inside blob */
        const uint8_t *nm_idx[3], *nm[3]; /* name tables: 0 scn_prop, 1 scn_cmd, 2 call_prop */
    } priv_;
} Scene;
typedef struct { int scene, ofs; } IncCmd;
typedef struct {
    Scene *scene;
    int nscene;
    IncCmd *inc_cmd;
    int ninc_cmd;
    int ninc_prop;
    PropDecl *inc_prop;                   /* [ninc_prop] (0 in this game) */
    WStr *inc_prop_name_, *inc_cmd_name_; /* views [ninc_prop], [ninc_cmd] (pack_inc_cmd_name) */
    int exe_angou_mod;
    struct {                          /* fmt/pack.c only */
        ArenaMark mem_start, mem_end; /* the Pack's range of the data arena */
        bool loaded;
    } priv_;
} Pack;

/* Reads and decodes Scene.pck. false (Pack zeroed, nothing kept) on missing or corrupt data. */
bool pack_load(Pack *pk, const char *scene_pck_path);
/* Same from bytes in memory (what = name for messages). The input is not modified and not referenced afterwards. */
bool pack_load_mem(Pack *pk, const uint8_t *data, size_t n, const char *what);
void pack_free(Pack *pk);                                     /* zeroes the Pack; no-op on a zeroed Pack */
const WStr *pack_scene_name(const Pack *pk, int scene);       /* "" for a scene outside the pack (never NULL) */
const WStr *pack_inc_cmd_name(const Pack *pk, int i);         /* "" outside [0, ninc_cmd) */
int  pack_find_scene(const Pack *pk, const WStr *name);       /* ASCII-case-folded; -1 if missing */
void scene_str(const Scene *sc, int idx, WStr *out);          /* string idx (XOR 0x7087*idx); "" if out of range */
/* Debug names of a scene: table 0 = scn_prop (nprop), 1 = scn_cmd (nscn_cmd), 2 = call_prop (ncall_prop_name).
 * false (and out = "") if idx is out of range or the entry is corrupt. */
enum { SCENE_NAMES_PROP, SCENE_NAMES_CMD, SCENE_NAMES_CALL_PROP };
bool scene_name(const Scene *sc, int table, int idx, WStr *out);

/* Static disassembly of one scene with the deterministic inline-read rule (VM §9). Counts only. */
typedef struct {
    int ncommand, ninline, ntext, nname, nop; /* nop = instructions decoded */
    bool ok;                                  /* reached EOF exactly at code_size, no unknown opcode */
    uint32_t end_pc;
} SceneStats;
void scene_walk(const Scene *sc, SceneStats *st);

/* M0 self-test (ENGINE_SPEC §4 M0, without the audio decoders): Scene.pck, Gameexe, g00, MODE.CGM.
 * Reference data: tests/ref/g00_crc.tsv, tests/ref/wp1_scenes.tsv. refdir NULL = $SAYOOSHI_REF_DIR, else
 * engine/tests/ref next to this source file, else ./tests/ref or ./engine/tests/ref.
 * Prints a report through log_msg (LOG_INFO); returns 0 when everything passes, 1 otherwise. */
int  fmt_selftest(const char *gamedata);
int  fmt_selftest_ref(const char *gamedata, const char *refdir);   /* refdir NULL = default lookup */

#endif
