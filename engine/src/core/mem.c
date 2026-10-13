/* core/mem.c — tracked heap, lifetime arenas, scratch, no-allocation zones and the usage/leak report.
 * The only file (with the other core/mem*.c) that calls malloc/free. */
#include "core/mem.h"
#include "core/check.h"
#include "core/log.h"
#include "core/mem_debug.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

enum {
    HDR_LIVE  = 0x4d454d31u,                    /* "MEM1" */
    HDR_FREED = 0x46524545u,                    /* "FREE" */
    MAX_ARENAS = 32,
    LEAK_LIST_MAX = 40                          /* leaked blocks listed individually at exit */
};
#define MEM_MAX_ALLOC ((size_t)1 << 30)         /* larger requests are bugs (external sizes are checked first) */

typedef struct MemHdr {
    uint32_t magic;
    uint16_t tag, pad;
    size_t size;
    const char *file;
    int line;
    struct MemHdr *prev, *next;                 /* live-block list (leak report, guard sweep) */
} MemHdr;
typedef union { MemHdr h; max_align_t align; } MemSlot;   /* keeps the payload max-aligned */
#define HDR_SIZE  sizeof(MemSlot)
#define GUARD     ((size_t)(MEM_DEBUG ? MEM_GUARD : 0))

typedef struct { size_t cur, peak, live, total, budget; } TagStats;   /* budget 0 = unlimited */

/* The heap lock is a real mutex (not a spinlock) so Helgrind/DRD see the ordering. It exists between mem_init and
 * mem_shutdown; outside that window the program is single-threaded and the lock is skipped. */
static SDL_Mutex *g_lock;
static MemHdr *g_live;
static TagStats g_tag[MEM_TAG_COUNT];
static _Thread_local int t_noalloc;
static uint64_t g_main_thread;
static Arena *g_arenas[MAX_ARENAS];
static int g_narenas;
static bool g_inited;

static const char *const TAG_NAMES[MEM_TAG_COUNT] = {
    "misc", "core", "str", "fmt", "plat", "vm", "gfx", "obj", "img", "txt", "font", "snd", "pcm", "ui", "sav", "app",
    "arena"
};

const char *mem_tag_name(int tag) { return tag >= 0 && tag < MEM_TAG_COUNT ? TAG_NAMES[tag] : "?"; }
uint64_t mem_thread_id(void) { return (uint64_t)SDL_GetCurrentThreadID(); }
bool mem_on_main_thread(void) { return g_main_thread == 0 || g_main_thread == mem_thread_id(); }

/* ---------------------------------------------------------------------------------------------- heap */

static void heap_lock(void) { if (g_lock) SDL_LockMutex(g_lock); }
static void heap_unlock(void) { if (g_lock) SDL_UnlockMutex(g_lock); }

static MemHdr *hdr_of(void *p) { return &((MemSlot *)p)[-1].h; }
static uint8_t *payload_of(MemHdr *h) { return (uint8_t *)h + HDR_SIZE; }

static void guard_write(MemHdr *h)
{
    if (!MEM_DEBUG) return;
    memset(payload_of(h) + h->size, MEM_POISON_GUARD, MEM_GUARD);
    MEM_HIDE(payload_of(h) + h->size, MEM_GUARD);
}

static bool guard_ok(MemHdr *h)
{
    if (!MEM_SOFT_CHECKS()) return true;
    const uint8_t *g = payload_of(h) + h->size;
    for (size_t i = 0; i < MEM_GUARD; i++)
        if (g[i] != MEM_POISON_GUARD) return false;
    return true;
}

/* Would `size` more live bytes of tag exceed its budget? A try allocation keeps MEM_TRY_HEADROOM of the budget free
 * for the small allocations that cannot fail. Called with the heap lock held. */
static bool over_budget(const TagStats *t, size_t size, bool try_alloc)
{
    if (!t->budget) return false;
    size_t limit = t->budget;
    if (try_alloc) limit = limit > MEM_TRY_HEADROOM ? limit - MEM_TRY_HEADROOM : 0;
    return t->cur > limit || size > limit - t->cur;
}

/* Counts size bytes against the tag before the block exists; false (nothing counted) past the budget. */
static bool budget_take(MemTag tag, size_t size, bool try_alloc)
{
    heap_lock();
    TagStats *t = &g_tag[tag];
    bool ok = !over_budget(t, size, try_alloc);
    if (ok) {
        t->cur += size;
        if (t->cur > t->peak) t->peak = t->cur;
    }
    heap_unlock();
    return ok;
}

static void budget_give(MemTag tag, size_t size)
{
    heap_lock();
    g_tag[tag].cur -= size;
    heap_unlock();
}

/* Allocation counts per call site (file:line), for --mem-report: finds churn that belongs in an arena.
 * Open addressing over a fixed table; sites past capacity are counted in g_site_overflow. Heap lock held. */
enum { SITE_CAP = 1024 };
typedef struct { const char *file; int line; uint16_t tag; size_t count, bytes; } SiteCount;
static SiteCount g_sites[SITE_CAP];
static size_t g_site_overflow;

static void site_count(const MemHdr *h)
{
    uintptr_t k = (uintptr_t)h->file * 31u + (uintptr_t)h->line;
    for (size_t i = 0; i < SITE_CAP; i++) {
        SiteCount *c = &g_sites[(k + i) % SITE_CAP];
        if (!c->file) { c->file = h->file; c->line = h->line; c->tag = h->tag; }
        if (c->file == h->file && c->line == h->line) { c->count++; c->bytes += h->size; return; }
    }
    g_site_overflow++;
}

static void link_block(MemHdr *h)
{
    heap_lock();
    site_count(h);
    h->prev = NULL;
    h->next = g_live;
    if (g_live) g_live->prev = h;
    g_live = h;
    g_tag[h->tag].live++;
    g_tag[h->tag].total++;
    heap_unlock();
}

static void unlink_block(MemHdr *h)
{
    heap_lock();
    if (h->prev) h->prev->next = h->next; else g_live = h->next;
    if (h->next) h->next->prev = h->prev;
    TagStats *t = &g_tag[h->tag];
    t->cur -= h->size;
    t->live--;
    heap_unlock();
}

/* Header sanity: catches double free, foreign pointers and underflows that reached the header. */
static MemHdr *checked_hdr(const void *p, const char *what)
{
    MemHdr *h = hdr_of((void *)(uintptr_t)p);   /* the header is ours to update even for a const payload */
    if (h->magic != HDR_LIVE)
        core_fatal("mem: %s of %p: %s", what, p, h->magic == HDR_FREED ? "block already freed (double free)" :
                   "not a block from mem_alloc (foreign or corrupted pointer)");
    if (h->tag >= MEM_TAG_COUNT) core_fatal("mem: %s of %p: corrupted block header", what, p);
    return h;
}

/* Guard sanity before a block is released (debug builds). */
static void check_guard(MemHdr *h, const char *what)
{
    if (!MEM_DEBUG) return;
    uint8_t *g = payload_of(h) + h->size;
    MEM_ASAN_UNPOISON(g, MEM_GUARD);
    MEM_VG_DEFINED(g, MEM_GUARD);
    (void)g;
    if (!guard_ok(h))
        core_fatal("mem: %s of %p (%zu bytes, %s, allocated at %s:%d): guard bytes overwritten (buffer overrun)",
                   what, (void *)payload_of(h), h->size, mem_tag_name(h->tag), h->file, h->line);
}

typedef enum { ALLOC_POISON = 1, ALLOC_TRY = 2 } AllocFlags;

/* The one allocation path. Without ALLOC_TRY every failure is fatal; with it an impossible size, an exhausted tag
 * budget or a failed malloc return NULL. */
static void *alloc_impl(size_t size, MemTag tag, const char *file, int line, unsigned flags)
{
    ASSERT_MSG(t_noalloc == 0, "heap allocation of %zu bytes inside a no-allocation zone (audio thread?) at %s:%d",
               size, file, line);
    ASSERT((int)tag >= 0 && tag < MEM_TAG_COUNT);
    bool try_alloc = (flags & ALLOC_TRY) != 0;
    size_t total;
    if (size > MEM_MAX_ALLOC || !core_add_sz(size, HDR_SIZE + GUARD, &total)) {
        if (try_alloc) return NULL;
        core_fatal("mem: impossible allocation of %zu bytes (%s) at %s:%d", size, mem_tag_name(tag), file, line);
    }
    if (!budget_take(tag, size, try_alloc)) {
        if (try_alloc) return NULL;
        core_fatal("mem: heap budget of '%s' exhausted (%zu bytes) by %zu bytes at %s:%d", mem_tag_name(tag),
                   mem_budget(tag), size, file, line);
    }
    MemHdr *h = malloc(total);
    if (!h) {
        budget_give(tag, size);
        if (try_alloc) return NULL;
        core_fatal("mem: out of memory: %zu bytes (%s) at %s:%d", size, mem_tag_name(tag), file, line);
    }
    h->magic = HDR_LIVE;
    h->tag = (uint16_t)tag;
    h->pad = 0;
    h->size = size;
    h->file = file;
    h->line = line;
    if (MEM_DEBUG && (flags & ALLOC_POISON)) memset(payload_of(h), MEM_POISON_FRESH, size);
    guard_write(h);
    link_block(h);
    return payload_of(h);
}

void *mem_alloc_at(size_t size, MemTag tag, const char *file, int line)
{
    return alloc_impl(size, tag, file, line, ALLOC_POISON);
}

/* Arena reserves: not poisoned (the arena hides and poisons what it hands out), so pages stay untouched. */
void *mem_alloc_reserve(size_t size, const char *file, int line) { return alloc_impl(size, MEM_ARENA, file, line, 0); }

void *mem_try_alloc_array_at(size_t count, size_t size, MemTag tag, const char *file, int line)
{
    size_t n;
    if (!core_mul_sz(count, size, &n)) return NULL;
    void *p = alloc_impl(n, tag, file, line, ALLOC_TRY);
    if (p) memset(p, 0, n);
    return p;
}

void *mem_try_realloc_array_at(void *p, size_t count, size_t size, const char *file, int line)
{
    size_t n;
    if (!core_mul_sz(count, size, &n)) return NULL;
    MemTag tag = p ? (MemTag)checked_hdr(p, "mem_try_realloc")->tag : MEM_MISC;
    void *q = alloc_impl(n, tag, file, line, ALLOC_TRY | ALLOC_POISON);
    if (!q || !p) return q;
    size_t old = mem_size(p);
    memcpy(q, p, old < n ? old : n);
    mem_free(p);
    return q;
}

size_t mem_budget(MemTag tag)
{
    ASSERT((int)tag >= 0 && tag < MEM_TAG_COUNT);
    heap_lock();
    size_t b = g_tag[tag].budget;
    heap_unlock();
    return b;
}

void mem_budget_set(MemTag tag, size_t bytes)
{
    ASSERT((int)tag >= 0 && tag < MEM_TAG_COUNT);
    heap_lock();
    g_tag[tag].budget = bytes;
    heap_unlock();
}

void *mem_zalloc_at(size_t size, MemTag tag, const char *file, int line)
{
    void *p = mem_alloc_at(size, tag, file, line);
    memset(p, 0, size);
    return p;
}

void *mem_alloc_array_at(size_t count, size_t size, MemTag tag, const char *file, int line)
{
    size_t n;
    if (!core_mul_sz(count, size, &n))
        core_fatal("mem: array size overflow (%zu x %zu bytes, %s) at %s:%d", count, size, mem_tag_name(tag), file,
                   line);
    return mem_zalloc_at(n, tag, file, line);
}

void mem_free(void *p)
{
    if (!p) return;
    ASSERT_MSG(t_noalloc == 0, "mem_free inside a no-allocation zone (audio thread?)");
    MemHdr *h = checked_hdr(p, "mem_free");
    check_guard(h, "mem_free");
    unlink_block(h);
    h->magic = HDR_FREED;
    if (MEM_DEBUG) memset(payload_of(h), MEM_POISON_FREED, h->size + GUARD);
    free(h);
}

void *mem_realloc_at(void *p, size_t size, const char *file, int line)
{
    if (!p) return mem_alloc_at(size, MEM_MISC, file, line);
    MemHdr *h = checked_hdr(p, "mem_realloc");
    size_t old = h->size;
    MemTag tag = (MemTag)h->tag;
    /* Always move: a stale pointer to the old block is then caught by Valgrind/ASan instead of working by luck. */
    void *q = mem_alloc_at(size, tag, file, line);
    memcpy(q, p, old < size ? old : size);
    mem_free(p);
    return q;
}

void *mem_realloc_array_at(void *p, size_t count, size_t size, const char *file, int line)
{
    size_t n;
    if (!core_mul_sz(count, size, &n))
        core_fatal("mem: array size overflow (%zu x %zu bytes) at %s:%d", count, size, file, line);
    return mem_realloc_at(p, n, file, line);
}

void *mem_dup_at(const void *p, size_t n, MemTag tag, const char *file, int line)
{
    void *q = mem_alloc_at(n, tag, file, line);
    if (n) memcpy(q, p, n);
    return q;
}

char *mem_strdup_at(const char *s, MemTag tag, const char *file, int line)
{
    if (!s) s = "";
    return mem_dup_at(s, strlen(s) + 1, tag, file, line);
}

size_t mem_size(const void *p)
{
    return checked_hdr(p, "mem_size")->size;
}

bool mem_check_all(void)
{
    if (!MEM_SOFT_CHECKS()) return true;
    bool ok = true;
    heap_lock();
    for (MemHdr *h = g_live; h; h = h->next) {
        if (h->magic != HDR_LIVE || !guard_ok(h)) {
            log_msg(LOG_ERR, "mem: damaged block %p (%zu bytes, %s, %s:%d)", (void *)payload_of(h), h->size,
                    mem_tag_name(h->tag), h->file, h->line);
            ok = false;
        }
    }
    heap_unlock();
    return ok;
}

/* ------------------------------------------------------------------------------------- no-alloc zones */

void mem_noalloc_enter(void) { t_noalloc++; }
void mem_noalloc_leave(void) { ASSERT(t_noalloc > 0); t_noalloc--; }

/* ---------------------------------------------------------------------------------------- lifetimes */

static Arena g_perm, g_data, g_scratch[2];

void mem_arena_register(Arena *a)
{
    if (g_narenas < MAX_ARENAS) g_arenas[g_narenas++] = a;
}

static void arena_unregister(const Arena *a)
{
    for (int i = 0; i < g_narenas; i++)
        if (g_arenas[i] == a) { g_arenas[i] = g_arenas[--g_narenas]; return; }
}

void mem_init(const MemConfig *cfg)
{
    /* At least 2x the worst peaks over every scene in both languages (perm 4.0, data 6.1, scratch 1.2 MiB, from
     * --mem-report); engine/tests/run_mem.sh fails when a peak passes half its budget. */
    static const MemConfig DEFAULTS = { .perm = 12u << 20, .data = 16u << 20, .scratch = 4u << 20 };
    static const size_t BUDGETS[MEM_TAG_COUNT] = { [MEM_VM] = 128u << 20, [MEM_STR] = 128u << 20,
                                                   [MEM_OBJ] = 64u << 20 };   /* peaks in play: < 1 MiB each */
    ASSERT_MSG(!g_inited, "mem_init called twice");
    if (!cfg) cfg = &DEFAULTS;
    g_main_thread = mem_thread_id();
    g_lock = SDL_CreateMutex();
    if (!g_lock) core_fatal("mem: cannot create the heap lock: %s", SDL_GetError());
    arena_init(&g_perm, "perm", cfg->perm);
    arena_init(&g_data, "data", cfg->data);
    arena_init(&g_scratch[0], "scratch0", cfg->scratch);
    arena_init(&g_scratch[1], "scratch1", cfg->scratch);
    Arena *all[] = { &g_perm, &g_data, &g_scratch[0], &g_scratch[1] };
    for (size_t i = 0; i < COUNT_OF(all); i++) mem_arena_register(all[i]);
    for (int t = 0; t < MEM_TAG_COUNT; t++) mem_budget_set((MemTag)t, BUDGETS[t]);
    g_inited = true;
}

static Arena *lifetime(Arena *a)
{
    ASSERT_MSG(g_inited, "memory lifetimes used before mem_init");
    ASSERT_MSG(mem_on_main_thread(), "lifetime arena '%s' used off the main thread", a->name);
    return a;
}

Arena *mem_perm(void) { return lifetime(&g_perm); }
Arena *mem_data(void) { return lifetime(&g_data); }
void mem_data_reset(void) { arena_reset(lifetime(&g_data)); }

Scratch mem_scratch_begin(const Arena *avoid)
{
    Arena *a = lifetime(&g_scratch[0]);
    if (avoid == a) a = lifetime(&g_scratch[1]);
    Scratch s = { a, arena_mark(a) };
    return s;
}

void mem_scratch_end(Scratch *s)
{
    ASSERT(s && s->arena);
    arena_reset_to(s->arena, s->mark);
    s->arena = NULL;
}

/* ---------------------------------------------------------------------------------------- reports */

static void report_churn(void);

void mem_report(void)
{
    heap_lock();
    TagStats t[MEM_TAG_COUNT];
    memcpy(t, g_tag, sizeof t);
    heap_unlock();
    log_msg(LOG_INFO, "mem: heap by tag (current / peak bytes, live blocks, allocations)");
    for (int i = 0; i < MEM_TAG_COUNT; i++)
        if (t[i].total)
            log_msg(LOG_INFO, "mem:   %-6s %10zu / %10zu  %7zu  %9zu", TAG_NAMES[i], t[i].cur, t[i].peak, t[i].live,
                    t[i].total);
    for (int i = 0; i < g_narenas; i++) {
        const Arena *a = g_arenas[i];
        log_msg(LOG_INFO, "mem: arena %-10s budget %10zu  used %10zu  peak %10zu  allocs %u  resets %u", a->name,
                a->cap, a->used, a->peak, a->allocs, a->resets);
    }
    report_churn();
}

/* The allocation sites with the most allocations since boot, most first. */
static void report_churn(void)
{
    enum { TOP = 15 };
    SiteCount top[TOP];
    memset(top, 0, sizeof top);
    heap_lock();
    for (size_t i = 0; i < SITE_CAP; i++) {
        const SiteCount *c = &g_sites[i];
        if (!c->file || c->count <= top[TOP - 1].count) continue;
        size_t j = TOP - 1;
        while (j > 0 && top[j - 1].count < c->count) { top[j] = top[j - 1]; j--; }
        top[j] = *c;
    }
    size_t overflow = g_site_overflow;
    heap_unlock();
    log_msg(LOG_INFO, "mem: busiest allocation sites (allocations, bytes)");
    for (size_t i = 0; i < TOP && top[i].file; i++)
        log_msg(LOG_INFO, "mem:   %9zu %12zu  %-5s %s:%d", top[i].count, top[i].bytes, mem_tag_name(top[i].tag),
                top[i].file, top[i].line);
    if (overflow) log_msg(LOG_INFO, "mem:   %zu allocations from sites past the table", overflow);
}

void mem_heap_usage(size_t *blocks, size_t *bytes)
{
    size_t nb = 0, nbytes = 0;
    heap_lock();
    for (int i = 0; i < MEM_TAG_COUNT; i++) { nb += g_tag[i].live; nbytes += g_tag[i].cur; }
    heap_unlock();
    if (blocks) *blocks = nb;
    if (bytes) *bytes = nbytes;
}

/* Live blocks grouped by allocation site (file:line, tag), largest first. */
typedef struct { const char *file; int line; uint16_t tag; size_t blocks, bytes; } LeakSite;

static size_t leak_sites(LeakSite *site, size_t cap, size_t *nblocks, size_t *nbytes)
{
    size_t n = 0;
    *nblocks = *nbytes = 0;
    heap_lock();
    for (MemHdr *h = g_live; h; h = h->next) {
        (*nblocks)++;
        *nbytes += h->size;
        size_t k = 0;
        while (k < n && !(site[k].file == h->file && site[k].line == h->line && site[k].tag == h->tag)) k++;
        if (k == n && n < cap) { site[n].file = h->file; site[n].line = h->line; site[n].tag = h->tag; n++; }
        if (k < n) { site[k].blocks++; site[k].bytes += h->size; }
    }
    heap_unlock();
    return n;
}

static void report_leaks(void)
{
    static LeakSite site[LEAK_LIST_MAX * 4];   /* static: no allocation while reporting */
    size_t blocks, bytes, n = leak_sites(site, COUNT_OF(site), &blocks, &bytes);
    if (!blocks) return;
    for (size_t i = 0; i < n && i < LEAK_LIST_MAX; i++) {          /* selection of the largest sites */
        size_t best = i;
        for (size_t k = i + 1; k < n; k++) if (site[k].bytes > site[best].bytes) best = k;
        LeakSite t = site[i]; site[i] = site[best]; site[best] = t;
        log_msg(LOG_WARN, "mem: leak: %zu bytes in %zu blocks (%s) allocated at %s:%d", site[i].bytes, site[i].blocks,
                mem_tag_name(site[i].tag), site[i].file, site[i].line);
    }
    log_msg(LOG_WARN, "mem: %zu heap blocks (%zu bytes) still allocated at exit (Valgrind shows the call stacks)",
            blocks, bytes);
}

void mem_shutdown(void)
{
    if (!g_inited) return;
    bool report = MEM_DEBUG || SDL_getenv("SAYO_MEM_REPORT") != NULL;
    if (report) mem_report();
    Arena *all[] = { &g_scratch[1], &g_scratch[0], &g_data, &g_perm };
    for (size_t i = 0; i < COUNT_OF(all); i++) { arena_unregister(all[i]); arena_destroy(all[i]); }
    g_inited = false;
    if (report) report_leaks();
    SDL_Mutex *lock = g_lock;
    g_lock = NULL;                             /* single-threaded from here on */
    SDL_DestroyMutex(lock);
}
