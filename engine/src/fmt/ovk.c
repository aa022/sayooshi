/* fmt/ovk.c — OVK voice archives (AST §3.1) + koe number mapping (AST §3.2, FUN_0050e370).
 * Index: u32 count, count x {u32 size, u32 offset, u32 id, u32 samples}; every entry's range must lie inside the
 * file. Streams are Ogg Vorbis decoded with the vendored stb_vorbis (engine/third_party/stb_impl.c). */
#include "fmt/ovk.h"
#include "core/bin.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "fmt/nwa.h"
#include <limits.h>
#include <string.h>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"   /* NOLINT(bugprone-suspicious-include): stb_vorbis ships as one .c; header part only */

/* engine/third_party/stb_impl.c: builds stb_vorbis's CRC table once. stb_vorbis would otherwise rebuild it on
 * every open (a data race between decoder threads); after the first call, opening only reads it. */
void stbv_crc32_global_init(void);

/* Every stb_vorbis decoder works inside one fixed buffer (stb_vorbis_alloc): no malloc, no alloca, bounded memory
 * whatever a corrupt stream claims. The game's streams need <= 176 KiB (setup + temp, measured over all 2607). */
enum { OVK_ENTRY = 16, OVK_CHUNK = 4096, OVK_VORBIS_MEM = 1 << 20 };
#define OVK_MAX_FRAMES ((size_t)1 << 24)   /* 6+ minutes at 44.1 kHz; longer streams are cut */
#define OVK_MAX_HINT   ((size_t)1 << 21) /* the index's sample count only sizes the first buffer (~47 s at 44.1 kHz) */

bool ovk_parse(const uint8_t *data, size_t n, OvkIndex *out)
{
    memset(out, 0, sizeof *out);
    stbv_crc32_global_init();                                 /* main thread, before any decoder thread runs */
    Rd r = rd_make(data, data ? n : 0);
    uint32_t cnt = rd_count(&r, OVK_MAX_ENTRIES, OVK_ENTRY);
    if (!rd_ok(&r)) return false;
    OvkEntry *e = mem_alloc_array(cnt ? cnt : 1, sizeof *e, MEM_FMT);
    for (uint32_t i = 0; i < cnt; i++) {
        e[i].size = rd_u32le(&r);
        e[i].offset = rd_u32le(&r);
        e[i].id = rd_u32le(&r);
        e[i].samples = rd_u32le(&r);
        if ((uint64_t)e[i].offset + e[i].size > n) { mem_free(e); return false; }
    }
    out->e = e;
    out->n = (int)cnt;
    return true;
}

/* Entries are not sorted by id (AST §3.1): linear search. */
const OvkEntry *ovk_find(const OvkIndex *idx, uint32_t id)
{
    if (!idx) return NULL;
    for (int i = 0; i < idx->n; i++) if (idx->e[i].id == id) return &idx->e[i];
    return NULL;
}

/* Decodes the whole stream; *frames = frames decoded. The buffer starts at the index's sample count (a hint from
 * the file, capped at OVK_MAX_HINT so a hostile index cannot claim a huge first buffer) and doubles up to
 * OVK_MAX_FRAMES. */
static int16_t *decode_stream(stb_vorbis *v, int ch, uint32_t hint, size_t *frames)
{
    size_t cap = (hint && hint < OVK_MAX_HINT ? hint : OVK_CHUNK) + OVK_CHUNK, len = 0;
    int16_t *pcm = mem_alloc_array(cap, (size_t)ch * sizeof *pcm, MEM_PCM);
    for (;;) {                                                /* each pass adds frames or stops; len is capped */
        if (cap - len < OVK_CHUNK) {
            if (cap >= OVK_MAX_FRAMES) {
                log_msg(LOG_WARN, "ovk: stream longer than %zu frames cut", OVK_MAX_FRAMES);
                break;
            }
            cap = cap * 2 < OVK_MAX_FRAMES ? cap * 2 : OVK_MAX_FRAMES;
            pcm = mem_realloc_array(pcm, cap, (size_t)ch * sizeof *pcm);
        }
        int got =
            stb_vorbis_get_samples_short_interleaved(v, ch, pcm + len * (size_t)ch, (int)((cap - len) * (size_t)ch));
        if (got <= 0) break;
        len += (size_t)got;
    }
    *frames = len;
    return mem_realloc_array(pcm, len ? len : 1, (size_t)ch * sizeof *pcm);   /* exact size: the voice keeps it */
}

int16_t *ovk_decode(const uint8_t *data, size_t n, const OvkEntry *e, int *channels, int *rate, int *frames)
{
    if (!data || !e || (uint64_t)e->offset + e->size > n || e->size > INT_MAX) return NULL;
    if (mem_on_main_thread()) stbv_crc32_global_init();
    int err = 0;
    stb_vorbis_alloc mem = { mem_alloc(OVK_VORBIS_MEM, MEM_PCM), OVK_VORBIS_MEM };
    stb_vorbis *v = stb_vorbis_open_memory(data + e->offset, (int)e->size, &err, &mem);
    int16_t *pcm = NULL;
    size_t len = 0;
    stb_vorbis_info info = { 0, 0, 0, 0, 0, 0 };
    if (!v) log_msg(LOG_WARN, "ovk: id %u: stb_vorbis error %d", e->id, err);
    else {
        info = stb_vorbis_get_info(v);
        if (info.channels > 0) pcm = decode_stream(v, info.channels, e->samples, &len);
        stb_vorbis_close(v);
    }
    mem_free(mem.alloc_buffer);
    if (!pcm) return NULL;
    if (channels) *channels = info.channels;
    if (rate) *rate = (int)info.sample_rate;
    if (frames) *frames = (int)len;
    return pcm;
}

void ovk_free(OvkIndex *idx)
{
    if (!idx) return;
    mem_free(idx->e);
    idx->e = NULL;
    idx->n = 0;
}

void ovk_koe_split(int koe_no, int *file_no, int *entry_id)
{
    /* signed /100000 and %100000 (imul 0x14f8b589; sar 13 at 0x50e3f4) */
    if (file_no) *file_no = koe_no / 100000;
    if (entry_id) *entry_id = koe_no % 100000;
}

void ovk_koe_file_name(int file_no, char *buf, size_t n)
{
    if (!buf || !n) return;
    if (!str_fmt(buf, n, "z%04d", file_no)) buf[0] = 0;     /* too small a buffer: no name rather than a wrong one */
}
