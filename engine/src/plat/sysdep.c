/* plat/sysdep.c — OS-specific helpers for the distributable build and plain OS files (see sysdep.h). */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L          /* isatty */
#endif
#include "plat/sysdep.h"
#include "core/log.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static bool g_console;

void plat_attach_console(bool open_new)
{
    if (g_console) return;
    /* stderr already goes somewhere (redirected to a file/pipe, or a console-subsystem build): keep it */
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) {
        g_console = true;
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS) || (open_new && AllocConsole())) {
        /* freopen re-points the CRT's own stream objects at the console: nothing new to close. A stream that
         * cannot be re-pointed keeps its old target (the console is a convenience). */
        bool out = freopen("CONOUT$", "w", stdout) != NULL;
        bool err = freopen("CONOUT$", "w", stderr) != NULL;
        bool in = freopen("CONIN$", "r", stdin) != NULL;
        g_console = out || err || in;
    }
}

void plat_message_box(const char *title, const char *msg)
{
    if (!g_console) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, NULL);
}

#else
#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
#include <unistd.h>
#endif

void plat_attach_console(bool open_new) { (void)open_new; }

void plat_message_box(const char *title, const char *msg)
{
#if defined(__APPLE__)
    if (!isatty(2)) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, NULL);   /* started from Finder */
#else
    (void)title; (void)msg;
#endif
}
#endif

/* ---- plain OS files ------------------------------------------------------------------------------------------- */

bool plat_path_is_dir(const char *path)
{
    SDL_PathInfo info;
    return path && SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

bool plat_path_is_file(const char *path)
{
    SDL_PathInfo info;
    return path && SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

/* Opens path and checks its size against max; NULL when missing or too large (logged). */
static SDL_IOStream *open_sized(const char *path, size_t max, size_t *size)
{
    *size = 0;
    if (!plat_path_is_file(path)) return NULL;
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return NULL;
    Sint64 sz = SDL_GetIOSize(io);
    if (sz < 0 || (uint64_t)sz > max) {
        log_msg(LOG_WARN, "file: %s is unreadable or larger than %zu bytes; ignored", path, max);
        (void)SDL_CloseIO(io);   /* read-only stream: nothing to flush */
        return NULL;
    }
    *size = (size_t)sz;
    return io;
}

/* Reads size bytes into d (size + 1 bytes) and closes io; false on a short read (logged). */
static bool read_all(SDL_IOStream *io, const char *path, uint8_t *d, size_t size)
{
    bool ok = size == 0 || SDL_ReadIO(io, d, size) == size;
    (void)SDL_CloseIO(io);       /* read-only stream: nothing to flush */
    if (!ok) log_msg(LOG_WARN, "file: short read of %s: %s", path, SDL_GetError());
    else d[size] = 0;
    return ok;
}

uint8_t *plat_read_file(const char *path, size_t max, MemTag tag, size_t *len)
{
    size_t size;
    *len = 0;
    SDL_IOStream *io = open_sized(path, max, &size);
    if (!io) return NULL;
    uint8_t *d = mem_alloc(size + 1, tag);
    if (!read_all(io, path, d, size)) { mem_free(d); return NULL; }
    *len = size;
    return d;
}

uint8_t *plat_read_file_arena(Arena *a, const char *path, size_t max, size_t *len)
{
    size_t size;
    *len = 0;
    SDL_IOStream *io = open_sized(path, max, &size);
    if (!io) return NULL;
    uint8_t *d = arena_try_alloc(a, size + 1, 1);
    if (!d) { (void)SDL_CloseIO(io); return NULL; }   /* read-only stream */
    if (!read_all(io, path, d, size)) return NULL;     /* the arena keeps the block until its reset */
    *len = size;
    return d;
}
