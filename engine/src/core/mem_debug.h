/* core/mem_debug.h — private to core/mem*.c: debug-build switches and the Valgrind / AddressSanitizer hooks that
 * keep arenas and pools from hiding bugs from those tools. Every macro is a no-op in a plain release build. */
#ifndef CORE_MEM_DEBUG_H
#define CORE_MEM_DEBUG_H
#include "core/base.h"

#if defined(SAYO_DEBUG)
#define MEM_DEBUG 1
#else
#define MEM_DEBUG 0
#endif

enum {
    MEM_POISON_FRESH = 0xCD,    /* uninitialised allocations (debug) */
    MEM_POISON_FREED = 0xDD,    /* released memory (debug) */
    MEM_POISON_GUARD = 0xFD,    /* guard bytes / redzones (debug) */
    MEM_GUARD = 16              /* guard bytes after every heap block and arena allocation (debug) */
};

/* ---- Valgrind (memcheck) client requests: make -C . valgrind builds with SAYO_VALGRIND ---- */
#if defined(SAYO_VALGRIND)
#include <valgrind/memcheck.h>
#define MEM_ON_VALGRIND()             (RUNNING_ON_VALGRIND != 0)
#define MEM_VG_NOACCESS(p, n)         ((void)VALGRIND_MAKE_MEM_NOACCESS((p), (n)))
#define MEM_VG_UNDEFINED(p, n)        ((void)VALGRIND_MAKE_MEM_UNDEFINED((p), (n)))
#define MEM_VG_DEFINED(p, n)          ((void)VALGRIND_MAKE_MEM_DEFINED((p), (n)))
#define MEM_VG_POOL_CREATE(pool)      VALGRIND_CREATE_MEMPOOL((pool), 0, 0)
#define MEM_VG_POOL_DESTROY(pool)     VALGRIND_DESTROY_MEMPOOL(pool)
#define MEM_VG_POOL_ALLOC(pool, p, n) VALGRIND_MEMPOOL_ALLOC((pool), (p), (n))
#define MEM_VG_POOL_FREE(pool, p)     VALGRIND_MEMPOOL_FREE((pool), (p))
#define MEM_VG_POOL_TRIM(pool, p, n)  VALGRIND_MEMPOOL_TRIM((pool), (p), (n))
#else
#define MEM_ON_VALGRIND()             0
#define MEM_VG_NOACCESS(p, n)         ((void)0)
#define MEM_VG_UNDEFINED(p, n)        ((void)0)
#define MEM_VG_DEFINED(p, n)          ((void)0)
#define MEM_VG_POOL_CREATE(pool)      ((void)0)
#define MEM_VG_POOL_DESTROY(pool)     ((void)0)
#define MEM_VG_POOL_ALLOC(pool, p, n) ((void)0)
#define MEM_VG_POOL_FREE(pool, p)     ((void)0)
#define MEM_VG_POOL_TRIM(pool, p, n)  ((void)0)
#endif

/* ---- AddressSanitizer manual poisoning (SAN=1 builds) ---- */
#if defined(__SANITIZE_ADDRESS__)
#define MEM_HAVE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MEM_HAVE_ASAN 1
#endif
#endif
#if defined(MEM_HAVE_ASAN)
#include <sanitizer/asan_interface.h>
#define MEM_ASAN_POISON(p, n)   ASAN_POISON_MEMORY_REGION((p), (n))
#define MEM_ASAN_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#define MEM_ON_ASAN()           1
#else
#define MEM_ASAN_POISON(p, n)   ((void)0)
#define MEM_ASAN_UNPOISON(p, n) ((void)0)
#define MEM_ON_ASAN()           0
#endif

/* Bytes nobody may touch (unallocated arena space, guards) / bytes that become usable. Debug builds only. */
#define MEM_HIDE(p, n)     do { if (MEM_DEBUG) { MEM_VG_NOACCESS((p), (n)); MEM_ASAN_POISON((p), (n)); } } while (0)
#define MEM_EXPOSE(p, n)   do { if (MEM_DEBUG) { MEM_ASAN_UNPOISON((p), (n)); MEM_VG_UNDEFINED((p), (n)); } } while (0)
/* Software guard checks are skipped when a tool already checks every access precisely. */
#define MEM_SOFT_CHECKS()  (MEM_DEBUG && !MEM_ON_VALGRIND() && !MEM_ON_ASAN())

const char *mem_tag_name(int tag);
void       *mem_alloc_reserve(size_t size, const char *file, int line);   /* arena backing, not poisoned */
void        mem_arena_register(struct Arena *a);     /* for mem_report (lifetime arenas register themselves) */
uint64_t    mem_thread_id(void);

#endif
