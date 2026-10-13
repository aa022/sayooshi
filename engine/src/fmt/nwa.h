/* fmt/nwa.h — NWA decoder (AST §2). All game files are raw PCM (complevel -1).
 * Memory: NwaSound.pcm is a tracked heap block (MEM_PCM), released with nwa_free (or, after taking it over, with
 * fmt_pcm_free, which is mem_free). Thread-safe. */
#ifndef FMT_NWA_H
#define FMT_NWA_H
#include "core/base.h"

typedef struct { int channels, rate, bits; uint32_t frames; int16_t *pcm; } NwaSound;   /* pcm interleaved */

bool nwa_decode(const uint8_t *data, size_t n, NwaSound *out);   /* false (out zeroed) on a bad header */
void nwa_free(NwaSound *s);
void fmt_pcm_free(int16_t *pcm);                                  /* PCM of nwa_decode / ovk_decode; NULL ok */

/* AST §2.1 header (0x2c bytes, FUN_006d1d70). samplecount = total samples over all channels. */
enum { NWA_HEADER_SIZE = 0x2c };
typedef struct {
    int channels, bits, freq, complevel, use_runlength, blocks;
    int datasize, compdatasize, samplecount, blocksize, restsize;
} NwaHeader;
bool nwa_header(const uint8_t *data, size_t n, NwaHeader *h);   /* false if channels/bits are invalid */

#endif
