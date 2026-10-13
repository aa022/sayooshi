/* core/mem.h — the engine's memory model (docs/CODE_STANDARD.md §5). Nothing outside core/mem*.c calls
 * malloc/calloc/realloc/free directly.
 *
 *  Heap      mem_alloc/mem_free: tracked general-purpose blocks for data with an individual lifetime (strings that
 *            grow, decoded assets owned by a cache). Every block carries a header (magic, tag, size, allocation
 *            site): double free and foreign pointers are caught, per-tag usage is reported. Out of memory or an
 *            impossible size is a clean fatal error, so the functions never return NULL.
 *  Arena     arena_alloc: bump allocation from one fixed reserve with a name and a hard budget. Zeroed by default.
 *            arena_mark / arena_reset_to free everything allocated after the mark at once. Exhausting the budget
 *            is a fatal error naming the arena and the request, never a NULL pointer.
 *  Lifetime  the engine arenas: mem_perm (boot to exit), mem_data (per loaded game data / language: reset on a
 *            language switch) and two scratch arenas for temporary per-call memory (mem_scratch_begin/end).
 *  Budget    per-tag ceilings on live heap bytes for the data whose size scripts control (MEM_VM, MEM_STR), with
 *            try variants that let a module refuse a request instead of stopping the program.
 *  Cache     cache_get/cache_put: a byte-budgeted LRU map from names to heap values (decoded images ...). Values
 *            stay valid until the next cache_trim(), which the main loop calls once per frame.
 *
 * Debug builds (SAYO_DEBUG: make DEBUG=1, SAN=1 and the valgrind build) add guard bytes after heap blocks and
 * arena allocations (checked on free / reset), poison fresh memory with 0xCD and released memory with 0xDD, tell
 * Valgrind (SAYO_VALGRIND) and AddressSanitizer which bytes are not allocated, check that arenas are used by their
 * owner thread, and print a usage/leak report at exit.
 *
 * Threads: the heap is thread-safe. Arenas and caches belong to one thread (the main thread for the
 * lifetime arenas). The audio callback must not allocate at all: it brackets itself with
 * mem_noalloc_enter/leave and any allocation inside is a fatal error. */
#ifndef CORE_MEM_H
#define CORE_MEM_H
#include "core/base.h"

/* ---- heap ------------------------------------------------------------------------------------------------- */
typedef enum MemTag {
    MEM_MISC, MEM_CORE, MEM_STR, MEM_FMT, MEM_PLAT, MEM_VM, MEM_GFX, MEM_OBJ, MEM_IMG, MEM_TXT, MEM_FONT, MEM_SND,
    MEM_PCM, MEM_UI, MEM_SAV, MEM_APP, MEM_ARENA, MEM_TAG_COUNT
} MemTag;

/* size 0 is allowed (a unique minimal block). Fresh memory is uninitialised (0xCD in debug builds). */
#define mem_alloc(size, tag)              mem_alloc_at((size), (tag), __FILE__, __LINE__)
#define mem_zalloc(size, tag)             mem_zalloc_at((size), (tag), __FILE__, __LINE__)
/* count * size, overflow-checked, zeroed. */
#define mem_alloc_array(count, size, tag) mem_alloc_array_at((count), (size), (tag), __FILE__, __LINE__)
/* p may be NULL; keeps the block's tag; new bytes are uninitialised. size 0 keeps a minimal block. */
#define mem_realloc(p, size)              mem_realloc_at((p), (size), __FILE__, __LINE__)
#define mem_realloc_array(p, count, size) mem_realloc_array_at((p), (count), (size), __FILE__, __LINE__)
#define mem_dup(p, n, tag)                mem_dup_at((p), (n), (tag), __FILE__, __LINE__)
#define mem_strdup(s, tag)                mem_strdup_at((s), (tag), __FILE__, __LINE__)
void *mem_alloc_at(size_t size, MemTag tag, const char *file, int line);
void *mem_zalloc_at(size_t size, MemTag tag, const char *file, int line);
void *mem_alloc_array_at(size_t count, size_t size, MemTag tag, const char *file, int line);
void *mem_realloc_at(void *p, size_t size, const char *file, int line);
void *mem_realloc_array_at(void *p, size_t count, size_t size, const char *file, int line);
void *mem_dup_at(const void *p, size_t n, MemTag tag, const char *file, int line);
char *mem_strdup_at(const char *s, MemTag tag, const char *file, int line);   /* NULL s -> "" */
void  mem_free(void *p);                       /* NULL is a no-op; a foreign or freed pointer is fatal */
size_t mem_size(const void *p);                /* usable size of a heap block */

/* Budgets: a ceiling on the live heap bytes of one tag (0 = unlimited). mem_init sets them for the tags whose size
 * scripts and saves control: MEM_VM (list objects), MEM_STR (script strings), MEM_OBJ (graphics object trees and
 * their rep lists), 128/128/64 MiB, where play peaks below 1 MiB each. An ordinary allocation past the budget is a
 * fatal error naming the tag. The try variants return NULL instead, and already fail when less than
 * MEM_TRY_HEADROOM of the budget would remain, so the small allocations that cannot fail still fit after a refused
 * script request. NULL also for an impossible size or when the system is out of memory. try_alloc_array zeroes;
 * try_realloc_array keeps the tag (MEM_MISC for p == NULL), the old block stays valid when it fails, and new bytes
 * are uninitialised. */
enum { MEM_TRY_HEADROOM = 1 << 20 };
#define mem_try_alloc_array(count, size, tag) mem_try_alloc_array_at((count), (size), (tag), __FILE__, __LINE__)
#define mem_try_realloc_array(p, count, size) mem_try_realloc_array_at((p), (count), (size), __FILE__, __LINE__)
void *mem_try_alloc_array_at(size_t count, size_t size, MemTag tag, const char *file, int line);
void *mem_try_realloc_array_at(void *p, size_t count, size_t size, const char *file, int line);
void  mem_budget_set(MemTag tag, size_t bytes);   /* main thread, normally at boot */
size_t mem_budget(MemTag tag);                    /* the current budget (0 = unlimited) */
bool  mem_check_all(void);                     /* debug: verify every live block's guards; false + log on damage */

/* ---- arenas ----------------------------------------------------------------------------------------------- */
typedef struct Arena {
    uint32_t    magic;
    const char *name;
    uint8_t    *base;
    size_t      cap, used, peak;
    size_t      last;                          /* debug: offset + 1 of the newest chunk header (0 = none) */
    uint32_t    allocs, resets;
    bool        owns;                          /* base came from mem_alloc (freed by arena_destroy) */
    bool        vg_pool;                       /* registered as a Valgrind mempool */
    uint64_t    owner;                         /* SDL thread id of the owner (debug check) */
    const struct Arena *self;                  /* detects copies of an initialised arena */
} Arena;
typedef struct ArenaMark { size_t used, last; uint32_t resets; } ArenaMark;   /* resets: Arena.resets when taken */

void  arena_init(Arena *a, const char *name, size_t cap);                 /* one tracked heap block of cap bytes */
void  arena_init_buffer(Arena *a, const char *name, void *buf, size_t cap);   /* caller storage (static/stack) */
void  arena_init_child(Arena *a, const char *name, Arena *parent, size_t cap);  /* carved from parent */
void  arena_destroy(Arena *a);
/* size bytes aligned to align (a power of two <= 4096), ZEROED. Fatal when the budget is exhausted. */
void *arena_alloc(Arena *a, size_t size, size_t align);
void *arena_alloc_uninit(Arena *a, size_t size, size_t align);           /* 0xCD in debug builds */
void *arena_alloc_array(Arena *a, size_t count, size_t size, size_t align);   /* overflow-checked, zeroed */
/* Like arena_alloc but returns NULL instead of stopping when the request does not fit (sizes from external data). */
void *arena_try_alloc(Arena *a, size_t size, size_t align);
char *arena_strdup(Arena *a, const char *s);                             /* NULL -> "" */
ArenaMark arena_mark(const Arena *a);
void  arena_reset_to(Arena *a, ArenaMark m);   /* frees everything allocated after m (debug: guards checked) */
void  arena_reset(Arena *a);
/* Releases [start, end) when it is still the newest memory of the arena (the arena is exactly at `end`), e.g. a
 * table rebuilt in the data arena without a reset in between. Returns false and does nothing otherwise: the memory
 * then stays until the arena's next reset. */
bool  arena_release_top(Arena *a, ArenaMark start, ArenaMark end);
size_t arena_remaining(const Arena *a);
#define ARENA_NEW(a, T)            ((T *)arena_alloc((a), sizeof(T), _Alignof(T)))
#define ARENA_NEW_ARRAY(a, T, n)   ((T *)arena_alloc_array((a), (n), sizeof(T), _Alignof(T)))

/* ---- lifetimes -------------------------------------------------------------------------------------------- */
typedef struct MemConfig { size_t perm, data, scratch; } MemConfig;   /* arena budgets in bytes */
void   mem_init(const MemConfig *cfg);         /* NULL = defaults; call first thing in main (main thread) */
void   mem_shutdown(void);                     /* destroys the arenas; debug builds report usage and leaks */
bool   mem_on_main_thread(void);
Arena *mem_perm(void);
Arena *mem_data(void);
void   mem_data_reset(void);                   /* game data reloaded (language switch) */

/* Temporary memory for the duration of one call: everything allocated from s.arena is released by
 * mem_scratch_end. Pass the arena your caller gave you (or NULL) as `avoid` so nested scratch uses the other
 * scratch arena and never frees the caller's result. Main thread only. */
typedef struct Scratch { Arena *arena; ArenaMark mark; } Scratch;
Scratch mem_scratch_begin(const Arena *avoid);
void    mem_scratch_end(Scratch *s);

/* ---- no-allocation zones (the audio callback) ---- */
void mem_noalloc_enter(void);                  /* per thread, nestable */
void mem_noalloc_leave(void);

/* ---- caches ----------------------------------------------------------------------------------------------- */
typedef void (*CacheFreeFn)(void *value);
typedef struct CacheEntry CacheEntry;
typedef struct Cache {
    uint32_t    magic;
    const char *name;
    CacheEntry *e;                             /* open addressing, cap slots */
    uint32_t    cap, count;
    uint64_t    clock;                         /* LRU stamp, bumped on every get / put */
    uint64_t    frame_mark;                    /* clock at the last cache_trim: newer entries are in use */
    size_t      bytes, budget, peak;
    uint32_t    hits, misses, evictions;
    CacheFreeFn free_fn;
} Cache;
enum { CACHE_KEY_MAX = 255 };
void  cache_init(Cache *c, const char *name, size_t byte_budget, uint32_t max_entries, CacheFreeFn free_fn);
void  cache_destroy(Cache *c);                 /* frees every value */
void *cache_get(Cache *c, const char *key);    /* NULL if absent; marks the entry recently used */
/* Takes ownership of value (freed with free_fn on eviction or clear). Returns false and leaves ownership with the
 * caller when the key is already present, longer than CACHE_KEY_MAX, or every entry was used since the last trim
 * and the table is full. */
bool  cache_put(Cache *c, const char *key, void *value, size_t bytes);
void  cache_trim(Cache *c);                    /* evict least recently used entries until bytes <= budget */
void  cache_clear(Cache *c);

/* ---- reports ---------------------------------------------------------------------------------------------- */
void mem_report(void);                         /* per-tag heap usage and arena high-water marks, to the log */
void mem_heap_usage(size_t *blocks, size_t *bytes); /* live tracked heap blocks and bytes now (leak checks in tests) */

#endif
