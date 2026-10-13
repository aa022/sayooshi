/* snd/snd_asset.c — sound assets: refcounted MixSound PCM, NWA files (bgm/, wav/), the SE cache, OVK voice
 * archives (koe/) and the #BGM table (AST §2–3, ENGINE_SPEC §2.10).
 *
 * Ownership:
 *  - MixSound: tracked heap (MEM_SND). NWA PCM stays in the decoder's NwaSound (nwa_free); OVK PCM (a MEM_PCM
 *    block from ovk_decode) is taken over as heap_pcm. Freed by the last snd_sound_unref.
 *  - SE cache: a core Cache holding one reference per wav/ sound, LRU-trimmed once per frame (snd_assets_trim).
 *  - OVK archives: the last KOE_ARCHIVES files (fs_read data + OvkIndex), round robin. A voice's PCM is decoded
 *    into its own buffer, so closing an archive never touches a playing voice.
 *  - #BGM table: fixed-capacity entry array; names in an arena that is reset on every reload.
 * Main thread only. */
#include "snd/snd_internal.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "fmt/gameexe.h"
#include "fmt/nwa.h"
#include "fmt/ovk.h"
#include "plat/fs.h"
#include <string.h>

static void bgm_table_free(void);

enum {
    SE_CACHE_BYTES = 32 << 20, SE_CACHE_ENTRIES = 64,   /* every wav/ file of the game is ~6.6 MB together */
    KOE_ARCHIVES = 4,
    BGM_TABLE_MAX = 256, BGM_NAMES_BYTES = 64 << 10,
};

/* ------------------------------------------------------------------------------------------------ MixSound */

static MixSound *snd_sound_ref(MixSound *s)
{
    ASSERT(s && s->refs > 0 && mem_on_main_thread());
    s->refs++;
    return s;
}

void snd_sound_unref(MixSound *s)
{
    if (!s) return;
    ASSERT_MSG(s->refs > 0 && mem_on_main_thread(), "MixSound released twice or off the main thread");
    if (--s->refs > 0) return;
    ASSERT_MSG(!mixer_uses(s), "MixSound freed while a mixer voice still plays it");
    nwa_free(&s->nwa);
    mem_free(s->heap_pcm);
    mem_free(s);
}

/* Validates the decoder's format (external data): false = unusable, logged. */
static bool format_ok(const char *what, int channels, int rate)
{
    if (channels >= 1 && channels <= SND_CHANNELS_MAX && rate >= 1 && rate <= SND_RATE_MAX) return true;
    log_msg(LOG_WARN, "audio: %s: unsupported format (%d channels, %d Hz)", what, channels, rate);
    return false;
}

static MixSound *sound_new(const int16_t *pcm, int channels, int rate, uint32_t frames)
{
    MixSound *s = mem_zalloc(sizeof *s, MEM_SND);
    s->pcm = pcm;
    s->channels = channels;
    s->rate = rate;
    s->frames = frames;
    s->refs = 1;
    return s;
}

static size_t sound_bytes(const MixSound *s) { return (size_t)s->frames * (size_t)s->channels * sizeof(int16_t); }

/* ------------------------------------------------------------------------------------------------ NWA */

/* Game data goes through the layered fs (directory or SAYOPAK payload). */
static uint8_t *read_asset(const char *sub, const char *name, const char *ext, size_t *len)
{
    *len = 0;
    char *path = fs_path(sub, name, ext);
    if (!path) return NULL;
    uint8_t *d = fs_read(path, len);
    fs_free(path);
    return d;
}

MixSound *snd_load_nwa(const char *sub, const char *name)
{
    size_t n = 0;
    uint8_t *d = read_asset(sub, name, "nwa", &n);
    if (!d) { log_msg(LOG_WARN, "audio: %s/%s.nwa not found", sub, name); return NULL; }
    NwaSound w;
    bool ok = nwa_decode(d, n, &w);
    fs_free(d);
    if (!ok) return NULL;
    if (!format_ok(name, w.channels, w.rate)) { nwa_free(&w); return NULL; }
    MixSound *s = sound_new(w.pcm, w.channels, w.rate, w.frames);
    s->nwa = w;
    return s;
}

/* ------------------------------------------------------------------------------------------------ SE cache */

static Cache g_se;
static bool g_se_live;

static void se_release(void *value) { snd_sound_unref(value); }

/* Cache key: the name folded to ASCII lower case (lookups are case-insensitive). False if it does not fit. */
static bool se_key(const char *name, char *key, size_t cap)
{
    size_t n = strlen(name);
    if (n >= cap) return false;
    for (size_t i = 0; i <= n; i++) key[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    return true;
}

MixSound *snd_load_se(const char *name)
{
    ASSERT(g_se_live);
    char key[CACHE_KEY_MAX + 1];
    bool cacheable = se_key(name, key, sizeof key);
    MixSound *s = cacheable ? cache_get(&g_se, key) : NULL;
    if (s) return snd_sound_ref(s);
    s = snd_load_nwa("wav", name);
    if (s && cacheable && cache_put(&g_se, key, s, sound_bytes(s))) snd_sound_ref(s);   /* the cache's reference */
    return s;
}

/* ------------------------------------------------------------------------------------------------ OVK / koe */

typedef struct KoeArchive { int file_no; uint8_t *data; size_t n; OvkIndex idx; } KoeArchive;
static KoeArchive g_arc[KOE_ARCHIVES];
static int g_arc_next;

static void archive_close(KoeArchive *a)
{
    fs_free(a->data);
    ovk_free(&a->idx);
    memset(a, 0, sizeof *a);
}

static const KoeArchive *archive_open(int file_no)
{
    for (int i = 0; i < KOE_ARCHIVES; i++)
        if (g_arc[i].data && g_arc[i].file_no == file_no) return &g_arc[i];
    char name[16];
    ovk_koe_file_name(file_no, name, sizeof name);       /* "z%04d" of an int always fits */
    size_t n = 0;
    uint8_t *d = read_asset("koe", name, "ovk", &n);
    if (!d) return NULL;
    OvkIndex idx;
    if (!ovk_parse(d, n, &idx)) { fs_free(d); log_msg(LOG_WARN, "audio: bad ovk %s", name); return NULL; }
    KoeArchive *a = &g_arc[g_arc_next];
    g_arc_next = (g_arc_next + 1) % KOE_ARCHIVES;
    archive_close(a);
    *a = (KoeArchive){ file_no, d, n, idx };
    return a;
}

/* The entry of koe_no, or NULL (out: its archive). */
static const OvkEntry *koe_entry(int koe_no, const KoeArchive **arc)
{
    int file_no, id;
    ovk_koe_split(koe_no, &file_no, &id);
    *arc = koe_no >= 0 ? archive_open(file_no) : NULL;
    return *arc ? ovk_find(&(*arc)->idx, (uint32_t)id) : NULL;
}

bool snd_koe_exists(int koe_no)
{
    const KoeArchive *a;
    return koe_entry(koe_no, &a) != NULL;
}

MixSound *snd_load_koe(int koe_no)
{
    int file_no, id;
    ovk_koe_split(koe_no, &file_no, &id);
    const KoeArchive *a;
    const OvkEntry *e = koe_entry(koe_no, &a);
    if (!e) {
        log_trace("SND", "koe %d -> z%04d.ovk id=%d MISSING", koe_no, file_no, id);
        log_msg(LOG_WARN, "audio: koe %d (z%04d.ovk id %d) not found", koe_no, file_no, id);
        return NULL;
    }
    int ch = 0, rate = 0, frames = 0;
    int16_t *pcm = ovk_decode(a->data, a->n, e, &ch, &rate, &frames);
    if (!pcm) return NULL;
    if (frames < 0 || !format_ok("koe", ch, rate)) { fmt_pcm_free(pcm); return NULL; }
    log_trace("SND", "koe %d -> z%04d.ovk id=%d frames=%d", koe_no, file_no, id, frames);
    MixSound *s = sound_new(NULL, ch, rate, (uint32_t)frames);
    s->heap_pcm = pcm;                     /* a MEM_PCM heap block (fmt/nwa.h): mem_free releases it */
    s->pcm = s->heap_pcm;
    return s;
}

/* ------------------------------------------------------------------------------------------------ BGM table */

static BgmEntry g_tab[BGM_TABLE_MAX];
static int g_ntab;
static Arena g_names;
static bool g_names_live;

/* Copy of s in the names arena, NULL when the budget is exhausted. */
static const char *name_dup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = arena_try_alloc(&g_names, n, 1);
    if (p) memcpy(p, s, n);
    return p;
}

bool bgm_table_add(const char *reg, const char *file, int start, int end, int repeat)
{
    if (!g_names_live) { arena_init(&g_names, "snd.bgm_names", BGM_NAMES_BYTES); g_names_live = true; }
    const char *r = g_ntab < BGM_TABLE_MAX ? name_dup(reg) : NULL;
    const char *f = r ? name_dup(file) : NULL;
    if (!f) { log_msg(LOG_WARN, "audio: #BGM table full; '%s' ignored", reg); return false; }
    g_tab[g_ntab++] = (BgmEntry){ r, f, start, end, repeat };
    return true;
}

static void add_from_gameexe(int i)
{
    char key[16];
    if (!str_fmt(key, sizeof key, "BGM.%03d", i)) return;    /* i < BGM_TABLE_MAX: always fits */
    WStr reg = {0}, file = {0};
    if (gx_str(key, 0, &reg) && gx_str(key, 1, &file) && reg.len) {
        Scratch sc = mem_scratch_begin(NULL);
        const char *r = ws_utf8_arena(sc.arena, &reg, WS_UTF8_SHORT_MAX);
        const char *f = ws_utf8_arena(sc.arena, &file, WS_UTF8_SHORT_MAX);
        (void)bgm_table_add(r, f, gx_int(key, 2, 0), gx_int(key, 3, -1), gx_int(key, 4, 0));   /* logged */
        mem_scratch_end(&sc);
    }
    ws_free(&reg);
    ws_free(&file);
}

void bgm_table_load(void)
{
    bgm_table_free();
    int cnt = gx_int("BGM.CNT", 0, 0);
    if (cnt > BGM_TABLE_MAX) {
        log_msg(LOG_WARN, "audio: #BGM.CNT %d exceeds %d; the rest is ignored", cnt, BGM_TABLE_MAX);
        cnt = BGM_TABLE_MAX;
    }
    for (int i = 0; i < cnt; i++) add_from_gameexe(i);
}

static void bgm_table_free(void)
{
    if (g_names_live) arena_destroy(&g_names);
    g_names_live = false;
    g_ntab = 0;
}

int bgm_table_count(void) { return g_ntab; }

bool bgm_table_find(const char *reg, BgmEntry *out)
{
    for (int i = 0; i < g_ntab; i++)
        if (str_eq_fold(str_c(g_tab[i].reg), str_c(reg))) { *out = g_tab[i]; return true; }
    if (g_ntab) return false;
    *out = (BgmEntry){ reg, reg, 0, -1, 0 };
    return true;
}

/* ------------------------------------------------------------------------------------------------ lifetime */

void snd_assets_init(void)
{
    ASSERT(!g_se_live);
    cache_init(&g_se, "snd.se", SE_CACHE_BYTES, SE_CACHE_ENTRIES, se_release);
    g_se_live = true;
}

void snd_assets_shutdown(void)
{
    if (g_se_live) cache_destroy(&g_se);
    g_se_live = false;
    for (int i = 0; i < KOE_ARCHIVES; i++) archive_close(&g_arc[i]);
    g_arc_next = 0;
    bgm_table_free();
}

void snd_assets_trim(void)
{
    if (g_se_live) cache_trim(&g_se);
}
