/* core/mem_arena.c — bump arenas with a fixed budget, marks, zeroed allocations and debug redzones.
 *
 * Layout of one allocation in a debug build:   [chunk header][payload][MEM_GUARD redzone bytes]
 * The chunk headers form a backward list (Arena.last, Chunk.prev) so arena_reset_to can verify every redzone it
 * releases. Release builds store only the payloads. Under Valgrind each payload is a memcheck mempool chunk (so
 * reports name the allocating call) and every other byte is NOACCESS; under ASan the same bytes are poisoned. */
#include "core/mem.h"
#include "core/check.h"
#include "core/mem_debug.h"
#include <string.h>

enum { ARENA_LIVE = 0x41524e41u, CHUNK_MAGIC = 0x43484b31u, ARENA_MAX_ALIGN = 4096, ARENA_BASE_ALIGN = 64 };

typedef struct { uint32_t magic, pad; size_t size, prev; } Chunk;   /* prev: offset + 1 of the previous Chunk */
#define CHUNK_HDR  ((size_t)(MEM_DEBUG ? 32 : 0))
#define REDZONE    ((size_t)(MEM_DEBUG ? MEM_GUARD : 0))
CORE_STATIC_ASSERT(sizeof(Chunk) <= 32, "a chunk header fits its 32-byte slot");

static void check_live(const Arena *a)
{
    ASSERT_MSG(a && a->magic == ARENA_LIVE, "arena used before arena_init or after arena_destroy");
    ASSERT_MSG(a->self == a, "arena '%s' was copied by value; pass a pointer", a->name);
    ASSERT_MSG(a->owner == mem_thread_id(), "arena '%s' used by a thread that does not own it", a->name);
}

static void setup(Arena *a, const char *name, uint8_t *base, size_t cap, bool owns, bool vg_pool)
{
    memset(a, 0, sizeof *a);
    a->magic = ARENA_LIVE;
    a->name = name ? name : "?";
    a->base = base;
    a->cap = cap;
    a->owns = owns;
    a->vg_pool = vg_pool;
    a->owner = mem_thread_id();
    a->self = a;
    if (vg_pool) MEM_VG_POOL_CREATE(a);
    MEM_HIDE(base, cap);
}

void arena_init(Arena *a, const char *name, size_t cap)
{
    ASSERT(a && cap > 0);
    setup(a, name, mem_alloc_reserve(cap, __FILE__, __LINE__), cap, true, true);
}

void arena_init_buffer(Arena *a, const char *name, void *buf, size_t cap)
{
    ASSERT(a && buf && cap > 0);
    setup(a, name, buf, cap, false, true);
}

void arena_init_child(Arena *a, const char *name, Arena *parent, size_t cap)
{
    ASSERT(a && cap > 0);
    uint8_t *base = arena_alloc_uninit(parent, cap, ARENA_BASE_ALIGN);
    setup(a, name, base, cap, false, false);   /* no nested mempool inside the parent's chunk */
}

void arena_destroy(Arena *a)
{
    if (!a || a->magic != ARENA_LIVE) return;
    check_live(a);
    arena_reset(a);   /* verifies the redzones one last time */
    if (a->vg_pool) MEM_VG_POOL_DESTROY(a);
    MEM_EXPOSE(a->base, a->cap);
    if (a->owns) mem_free(a->base);
    memset(a, 0, sizeof *a);
}

/* Chunk headers are copied in and out with memcpy: no alignment or aliasing assumptions. */
static Chunk chunk_load(const Arena *a, size_t off_plus1)
{
    Chunk c;
    memcpy(&c, a->base + off_plus1 - 1, sizeof c);
    return c;
}

/* Offset of the payload for a request at the current fill level; false when it does not fit. */
static bool place(const Arena *a, size_t size, size_t align, size_t *p_off, size_t *end)
{
    size_t addr, after;
    if (!core_add_sz((size_t)(uintptr_t)a->base, a->used + CHUNK_HDR, &addr) || !core_align_up(addr, align, &addr))
        return false;
    *p_off = addr - (size_t)(uintptr_t)a->base;
    return core_add_sz(*p_off, size, &after) && core_add_sz(after, REDZONE, end) && *end <= a->cap;
}

static void debug_chunk(Arena *a, uint8_t *p, size_t size)
{
    uint8_t *at = p - CHUNK_HDR;
    Chunk c = { CHUNK_MAGIC, 0, size, a->last };
    MEM_EXPOSE(at, CHUNK_HDR);
    memcpy(at, &c, sizeof c);
    MEM_HIDE(at, CHUNK_HDR);
    a->last = (size_t)(at - a->base) + 1;
    MEM_EXPOSE(p + size, MEM_GUARD);
    memset(p + size, MEM_POISON_GUARD, MEM_GUARD);
    MEM_HIDE(p + size, MEM_GUARD);
}

static void *bump(Arena *a, size_t size, size_t align, bool fatal)
{
    check_live(a);
    ASSERT_MSG(core_is_pow2(align) && align <= ARENA_MAX_ALIGN, "arena '%s': bad alignment %zu", a->name, align);
    size_t p_off, end;
    if (!place(a, size, align, &p_off, &end)) {
        if (!fatal) return NULL;
        core_fatal("arena '%s' out of budget: request of %zu bytes (align %zu) with %zu of %zu bytes used (peak %zu)",
                   a->name, size, align, a->used, a->cap, a->peak);
    }
    uint8_t *p = a->base + p_off;
    if (MEM_DEBUG) debug_chunk(a, p, size);
    MEM_EXPOSE(p, size);
    if (a->vg_pool) MEM_VG_POOL_ALLOC(a, p, size);
    a->used = end;
    a->allocs++;
    if (a->used > a->peak) a->peak = a->used;
    return p;
}

void *arena_alloc_uninit(Arena *a, size_t size, size_t align)
{
    void *p = bump(a, size, align, true);
    if (MEM_DEBUG) memset(p, MEM_POISON_FRESH, size);
    return p;
}

void *arena_alloc(Arena *a, size_t size, size_t align)
{
    void *p = bump(a, size, align, true);
    memset(p, 0, size);
    return p;
}

void *arena_try_alloc(Arena *a, size_t size, size_t align)
{
    void *p = bump(a, size, align, false);
    if (p) memset(p, 0, size);
    return p;
}

void *arena_alloc_array(Arena *a, size_t count, size_t size, size_t align)
{
    size_t n;
    if (!core_mul_sz(count, size, &n))
        core_fatal("arena '%s': array size overflow (%zu x %zu bytes)", a ? a->name : "?", count, size);
    return arena_alloc(a, n, align);
}

static void *arena_dup(Arena *a, const void *p, size_t n)
{
    void *q = arena_alloc_uninit(a, n, 1);
    if (n) memcpy(q, p, n);
    return q;
}

char *arena_strdup(Arena *a, const char *s)
{
    if (!s) s = "";
    return arena_dup(a, s, strlen(s) + 1);
}

ArenaMark arena_mark(const Arena *a)
{
    check_live(a);
    ArenaMark m = { a->used, a->last, a->resets };
    return m;
}

size_t arena_remaining(const Arena *a)
{
    check_live(a);
    return a->cap - a->used;
}

/* Debug: every chunk released by a reset must still have its header and redzone intact. */
static void verify_chunks(const Arena *a, size_t down_to)
{
    size_t off = a->last;
    for (uint32_t guard = 0; off > down_to; guard++) {
        ASSERT_MSG(guard <= a->allocs, "arena '%s': chunk list loops", a->name);
        Chunk c = chunk_load(a, off);
        if (c.magic != CHUNK_MAGIC || c.size > a->cap)
            core_fatal("arena '%s': chunk header at offset %zu overwritten (buffer underrun)", a->name, off - 1);
        const uint8_t *z = a->base + off - 1 + CHUNK_HDR + c.size;
        for (size_t i = 0; i < MEM_GUARD; i++)
            if (z[i] != MEM_POISON_GUARD)
                core_fatal("arena '%s': redzone after a %zu-byte allocation at offset %zu overwritten (buffer overrun)",
                           a->name, c.size, off - 1 + CHUNK_HDR);
        off = c.prev;
    }
}

void arena_reset_to(Arena *a, ArenaMark m)
{
    check_live(a);
    ASSERT_MSG(m.used <= a->used, "arena '%s': reset to a mark above the fill level (mark already released)", a->name);
    if (MEM_SOFT_CHECKS()) verify_chunks(a, m.last);
    if (a->vg_pool) MEM_VG_POOL_TRIM(a, a->base, m.used);
    if (MEM_DEBUG && a->used > m.used) {
        size_t n = a->used - m.used;
        MEM_EXPOSE(a->base + m.used, n);
        memset(a->base + m.used, MEM_POISON_FREED, n);
        MEM_HIDE(a->base + m.used, n);
    }
    a->used = m.used;
    a->last = m.last;
    a->resets++;
}

bool arena_release_top(Arena *a, ArenaMark start, ArenaMark end)
{
    check_live(a);
    ASSERT_MSG(start.used <= end.used, "arena '%s': release of an inverted range", a->name);
    if (a->used != end.used || a->last != end.last || a->resets != end.resets) return false;   /* not ours any more */
    arena_reset_to(a, start);
    return true;
}

void arena_reset(Arena *a)
{
    ArenaMark zero = { 0, 0, 0 };
    arena_reset_to(a, zero);
}
