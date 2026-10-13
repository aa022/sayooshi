/* fmt/ovk.h — OVK voice archives + koe number mapping (AST §3).
 * file = koe/z%04d.ovk with koe_no/100000, entry id = koe_no%100000; streams decoded with stb_vorbis.
 * Memory: an OvkIndex is released with ovk_free; ovk_decode's PCM (a MEM_PCM heap block, exact size) with
 * fmt_pcm_free (fmt/nwa.h) or mem_free. Threads: ovk_parse / ovk_free on the main thread; ovk_decode is thread-safe
 * once ovk_parse has run once on the main thread (it initialises stb_vorbis's shared tables). */
#ifndef FMT_OVK_H
#define FMT_OVK_H
#include "core/base.h"

typedef struct { uint32_t size, offset, id, samples; } OvkEntry;   /* AST §3.1, 16 bytes each */
typedef struct { OvkEntry *e; int n; } OvkIndex;

enum { OVK_MAX_ENTRIES = 1 << 20 };

bool ovk_parse(const uint8_t *data, size_t n, OvkIndex *out);     /* false (out empty) on a corrupt index */
const OvkEntry *ovk_find(const OvkIndex *idx, uint32_t id);
/* Interleaved 16-bit PCM of entry e, release with fmt_pcm_free; NULL on a corrupt stream. */
int16_t *ovk_decode(const uint8_t *data, size_t n, const OvkEntry *e, int *channels, int *rate, int *frames);
void ovk_free(OvkIndex *idx);

void ovk_koe_split(int koe_no, int *file_no, int *entry_id);   /* AST §3.2 */
void ovk_koe_file_name(int file_no, char *buf, size_t n);     /* "z%04d" (no directory, no extension) */

#endif
