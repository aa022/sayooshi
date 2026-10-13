/* plat/fs.h — layered, case-insensitive, read-only game data file system (ENGINE_SPEC §2.1 #4, §2.3; data layer
 * contract of the self-contained build, docs/PAYLOAD.md).
 *
 * Game data is a stack of read-only layers. A layer is a directory or a SAYOPAK archive (a payload appended to
 * the executable, or a stand-alone game.pak). Every layer is mounted at a prefix of one canonical tree:
 *
 *     ja/Scene.pck  ja/Gameexe.dat  ja/dat/MODE.CGM  ja/g00/ ja/bgm/  ja/wav/  ja/koe/   original GameData
 *     en/Scene.pck  en/Gameexe.dat  en/g00/        en/font/sayooshieng-Regular.ttf          EN patch overrides
 *     manifest.txt                                                                             meta (optional)
 *
 * Lookups are case-insensitive by relative path ('\\' counts as '/'); the topmost (last mounted) layer wins.
 * The VISIBLE tree that the engine reads ("Scene.pck", "g00/BG_RD.g00") is ja/ with <lang>/ overlaid
 * (fs_set_language; "ja" = no overlay). Paths that start with '/' (after an optional "vfs:") address the canonical
 * tree directly ("/en/Scene.pck").
 *
 * Archived files have no OS path: read them with fs_read_all (or fs_read on an fs_path() result).
 * Memory: fs owns its index (arena); buffers it returns are released with fs_free / fs_list_free only. */
#ifndef PLAT_FS_H
#define PLAT_FS_H
#include "core/base.h"

typedef struct SDL_IOStream SDL_IOStream;

/* ---- layers ---- */
bool  fs_mounted(void);                                 /* at least one layer with files */
const char *fs_describe(void);                          /* "payload:/x/sayooshi + dir:/y" (for logs); owned by fs */

/* Startup data discovery (docs/PAYLOAD.md §Discovery):
 *  (1) explicit (--gamedata DIR / positional): a plain GameData dir (= ja/), a data tree (ja/ + en/), or a .pak;
 *  (2) payload appended to the executable, then game.pak next to it (macOS: Contents/Resources/game.pak);
 *  (3) data/ next to the executable;
 *  (4) dev fallback: <exe>/../data, <exe>/../../build/data, ./build/data (the tree `make data` writes).
 * An explicit source that cannot be mounted is an error (no fallback). */
bool  fs_discover(const char *explicit_source);

/* ---- language overlay ---- */
#define FS_LANG_BASE "ja"                                /* the original data: no overlay */
bool  fs_set_language(const char *lang);                /* "ja" or a code with <lang>/ in the tree; false if absent */
const char *fs_language(void);                          /* "ja" by default */
bool  fs_has_language(const char *lang);                /* <lang>/Scene.pck exists ("ja": ja/Scene.pck) */

/* ---- file access (visible tree; "vfs:" prefix accepted; leading '/' = canonical tree) ---- */
bool  fs_exists(const char *rel);
uint8_t *fs_read_all(const char *rel,
                     size_t *len); /* whole file (+1 NUL byte), release with fs_free; NULL if missing */
/* The two strings below are owned by fs and stay valid until the next mount or reset. */
const char *fs_layer_of(const char *rel);               /* fs_describe()-style name of the layer serving rel */
const char *fs_canonical(const char *rel);              /* canonical path serving rel ("en/Scene.pck"), or NULL */

/* ---- legacy helpers (kept for existing callers) ---- */
bool  fs_init(const char *gamedata_dir);                /* fs_reset + fs_mount_dir(dir, NULL) (or a .pak) */
/* Case-insensitive name lookup in the visible tree. Returns a VIRTUAL path "vfs:<subdir>/<Name.ext>" (NOT an OS
 * path) that fs_read accepts, released with fs_free; NULL if missing. subdir may be NULL/"" (root); '\\' in
 * name counts as '/'; ext (without '.') is appended unless name already ends in ".ext"; if name+ext is missing,
 * name as given is tried too. */
char *fs_path(const char *subdir, const char *name, const char *ext);
/* "vfs:..." -> fs_read_all, else an OS file. Release with fs_free. Files over 512 MiB are refused (NULL). */
uint8_t *fs_read(const char *path, size_t *len);
/* Base names (original case, sorted case-insensitively, deduplicated over ja/ and the overlay) of the visible
 * files directly in subdir whose extension matches ext (case-insensitive; NULL = all). Returns the count;
 * release *names with fs_list_free. */
int   fs_list(const char *subdir, const char *ext, char ***names);
void  fs_list_free(char **names, int n);
/* Releases a buffer returned by fs_read, fs_read_all or fs_path (tracked heap blocks, MEM_PLAT). NULL ok. */
void  fs_free(void *data);

/* ---- host helpers ---- */
/* Verify every archived file against its CRC-32 (slow: reads all data). Returns the number of bad entries. */
int   fs_verify(void);

#endif
