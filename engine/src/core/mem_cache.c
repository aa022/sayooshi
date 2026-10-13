/* core/mem_cache.c — byte-budgeted LRU cache of heap values keyed by strings (decoded images, ...).
 * Linear-probing hash table with backward-shift deletion. Values handed out by cache_get stay valid until the
 * next cache_trim (once per frame), because only trim and a full table evict, and a full table never evicts an
 * entry used since the last trim. */
#include "core/mem.h"
#include "core/check.h"
#include "core/hash.h"
#include <string.h>

enum { CACHE_LIVE = 0x43414348u, CACHE_MAX_ENTRIES = 1u << 20 };

struct CacheEntry { char *key; uint32_t hash; void *value; size_t bytes; uint64_t stamp; };

static void check_cache(const Cache *c) { ASSERT_MSG(c && c->magic == CACHE_LIVE, "cache used before cache_init"); }
static uint32_t key_hash(const char *k, size_t n) { return (uint32_t)hash_fnv64(HASH_FNV64_INIT, k, n); }

void cache_init(Cache *c, const char *name, size_t byte_budget, uint32_t max_entries, CacheFreeFn free_fn)
{
    ASSERT(c && free_fn && max_entries > 0 && max_entries <= CACHE_MAX_ENTRIES);
    memset(c, 0, sizeof *c);
    c->magic = CACHE_LIVE;
    c->name = name ? name : "?";
    c->budget = byte_budget;
    c->free_fn = free_fn;
    uint32_t cap = 16;
    while (cap < 2 * max_entries) cap *= 2;                  /* load factor <= 1/2 */
    c->cap = cap;
    c->e = mem_alloc_array(cap, sizeof *c->e, MEM_CORE);
}

/* Slot holding key, or the empty slot where it would go. */
static uint32_t find(const Cache *c, const char *key, size_t n, uint32_t h)
{
    uint32_t mask = c->cap - 1, i = h & mask;
    for (uint32_t probes = 0; probes < c->cap; probes++, i = (i + 1) & mask) {
        const CacheEntry *e = &c->e[i];
        if (!e->key || (e->hash == h && strncmp(e->key, key, n + 1) == 0)) return i;
    }
    core_fatal("cache '%s': table full", c->name);       /* impossible: load factor is kept <= 1/2 */
}

static void remove_at(Cache *c, uint32_t i)
{
    CacheEntry *e = &c->e[i];
    c->free_fn(e->value);
    c->bytes -= e->bytes;
    c->count--;
    mem_free(e->key);
    memset(e, 0, sizeof *e);
    uint32_t mask = c->cap - 1;
    for (uint32_t j = (i + 1) & mask; c->e[j].key; j = (j + 1) & mask) {   /* backward-shift the probe run */
        uint32_t home = c->e[j].hash & mask;
        bool movable = (j > i) ? (home <= i || home > j) : (home <= i && home > j);
        if (!movable) continue;
        c->e[i] = c->e[j];
        memset(&c->e[j], 0, sizeof c->e[j]);
        i = j;
    }
}

/* Least recently used entry with stamp <= limit, or cap if none. */
static uint32_t lru(const Cache *c, uint64_t limit)
{
    uint32_t best = c->cap;
    for (uint32_t i = 0; i < c->cap; i++)
        if (c->e[i].key && c->e[i].stamp <= limit && (best == c->cap || c->e[i].stamp < c->e[best].stamp)) best = i;
    return best;
}

void *cache_get(Cache *c, const char *key)
{
    check_cache(c);
    size_t n = strlen(key);
    if (n > CACHE_KEY_MAX) return NULL;
    uint32_t i = find(c, key, n, key_hash(key, n));
    if (!c->e[i].key) { c->misses++; return NULL; }
    c->e[i].stamp = ++c->clock;
    c->hits++;
    return c->e[i].value;
}

bool cache_put(Cache *c, const char *key, void *value, size_t bytes)
{
    check_cache(c);
    size_t n = strlen(key);
    if (n > CACHE_KEY_MAX) return false;
    uint32_t h = key_hash(key, n);
    if (c->e[find(c, key, n, h)].key) return false;
    if (c->count >= c->cap / 2) {
        uint32_t victim = lru(c, c->frame_mark);
        if (victim == c->cap) return false;
        remove_at(c, victim);
        c->evictions++;
    }
    uint32_t i = find(c, key, n, h);
    CacheEntry *e = &c->e[i];
    e->key = mem_strdup(key, MEM_CORE);
    e->hash = h;
    e->value = value;
    e->bytes = bytes;
    e->stamp = ++c->clock;
    c->count++;
    c->bytes += bytes;
    if (c->bytes > c->peak) c->peak = c->bytes;
    return true;
}

void cache_trim(Cache *c)
{
    check_cache(c);
    for (uint32_t guard = 0; c->bytes > c->budget && guard < c->cap; guard++) {
        uint32_t victim = lru(c, c->clock);
        if (victim == c->cap) break;
        remove_at(c, victim);
        c->evictions++;
    }
    c->frame_mark = c->clock;
}

/* Frees every entry in place (no backward shifting: with wrap-around a shift could move an entry into a slot the
 * sweep has already passed). */
void cache_clear(Cache *c)
{
    check_cache(c);
    for (uint32_t i = 0; i < c->cap; i++) {
        CacheEntry *e = &c->e[i];
        if (!e->key) continue;
        c->free_fn(e->value);
        mem_free(e->key);
        memset(e, 0, sizeof *e);
    }
    c->count = 0;
    c->bytes = 0;
    c->frame_mark = c->clock;
}

void cache_destroy(Cache *c)
{
    if (!c || c->magic != CACHE_LIVE) return;
    cache_clear(c);
    mem_free(c->e);
    memset(c, 0, sizeof *c);
}
