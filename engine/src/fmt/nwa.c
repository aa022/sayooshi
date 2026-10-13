/* fmt/nwa.c — NWA decoder (AST §2). Header read as in FUN_006d1d70; every file of this game is raw PCM
 * (complevel -1, AST §2.2), so the compressed block decoder (FUN_006d1ec0) is not implemented. A truncated file
 * plays what is there: the sample count never exceeds the bytes present. */
#include "fmt/nwa.h"
#include "core/bin.h"
#include "core/log.h"
#include "core/mem.h"
#include <string.h>

enum { NWA_RAW = -1, U8_BIAS = 128 };

bool nwa_header(const uint8_t *data, size_t n, NwaHeader *h)
{
    Rd r = rd_make(data, data ? n : 0);
    memset(h, 0, sizeof *h);
    h->channels = rd_u16le(&r);
    h->bits = rd_u16le(&r);
    h->freq = rd_i32le(&r);
    h->complevel = rd_i32le(&r);
    h->use_runlength = rd_i32le(&r);
    h->blocks = rd_i32le(&r);
    h->datasize = rd_i32le(&r);
    h->compdatasize = rd_i32le(&r);
    h->samplecount = rd_i32le(&r);
    h->blocksize = rd_i32le(&r);
    h->restsize = rd_i32le(&r);
    /* the reader rejects anything but 1/2 channels and 8/16 bits (FUN_006d1ff0) */
    return rd_ok(&r) && n >= NWA_HEADER_SIZE && (h->channels == 1 || h->channels == 2) &&
           (h->bits == 8 || h->bits == 16) && h->freq > 0;
}

/* Samples to decode: the header's count, cut to the data present and to whole frames. */
static size_t sample_count(const NwaHeader *h, size_t n)
{
    size_t bps = (size_t)h->bits / 8, avail = (n - NWA_HEADER_SIZE) / bps;
    size_t samples = h->samplecount > 0 ? (size_t)h->samplecount : 0;
    if (samples > avail) {
        log_msg(LOG_WARN, "nwa: truncated (%zu of %zu samples)", avail, samples);
        samples = avail;
    }
    return samples - samples % (size_t)h->channels;
}

bool nwa_decode(const uint8_t *data, size_t n, NwaSound *out)
{
    NwaHeader h;
    memset(out, 0, sizeof *out);
    if (!nwa_header(data, n, &h)) { log_msg(LOG_WARN, "nwa: bad header"); return false; }
    if (h.complevel != NWA_RAW) {
        log_msg(LOG_WARN, "nwa: compressed NWA (level %d) not supported", h.complevel);
        return false;
    }
    size_t samples = sample_count(&h, n), bps = (size_t)h.bits / 8;
    const uint8_t *p = data + NWA_HEADER_SIZE;                /* samples * bps <= n - header: proven above */
    int16_t *pcm = mem_alloc_array(samples ? samples : 1, sizeof *pcm, MEM_PCM);
    if (bps == 2) for (size_t i = 0; i < samples; i++) pcm[i] = (int16_t)bin_load_le16(p + 2 * i);
    else for (size_t i = 0; i < samples; i++) pcm[i] = (int16_t)((p[i] - U8_BIAS) * 256);   /* 8-bit WAV-style */
    out->channels = h.channels;
    out->rate = h.freq;
    out->bits = h.bits;
    out->frames = (uint32_t)(samples / (size_t)h.channels);
    out->pcm = pcm;
    return true;
}

void fmt_pcm_free(int16_t *pcm) { mem_free(pcm); }

void nwa_free(NwaSound *s)
{
    if (!s) return;
    fmt_pcm_free(s->pcm);
    memset(s, 0, sizeof *s);
}
