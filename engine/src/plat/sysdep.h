/* plat/sysdep.h — small OS helpers: the Windows GUI-subsystem console, the error box, and plain files outside the
 * game data (saves, harness scripts and reference tables, fonts) through SDL. */
#ifndef PLAT_SYSDEP_H
#define PLAT_SYSDEP_H
#include "core/base.h"
#include "core/mem.h"

/* Windows GUI-subsystem builds have no console: attach to the parent console (cmd/PowerShell) when there is one,
 * else open a new console window when open_new (--console), and route stdout/stderr there. Later calls only add
 * the new window. No-op elsewhere. */
void plat_attach_console(bool open_new);
/* Fatal startup error for users who started the game by double-click (Windows/macOS without a console);
 * elsewhere a no-op because the message was already printed to stderr. */
void plat_message_box(const char *title, const char *msg);

/* ---- plain OS files (not the game data: that is plat/fs) ---- */
bool plat_path_is_dir(const char *path);     /* false for NULL or a missing path */
bool plat_path_is_file(const char *path);
/* The whole file plus a NUL byte (not counted in *len). NULL, *len = 0, when it is missing, larger than max, or the
 * read is short; the last two are logged. plat_read_file: a tracked heap block of `tag` (release with mem_free).
 * plat_read_file_arena: allocated in `a` (refused, not fatal, when the arena has no room). */
uint8_t *plat_read_file(const char *path, size_t max, MemTag tag, size_t *len);
uint8_t *plat_read_file_arena(Arena *a, const char *path, size_t max, size_t *len);

#endif
