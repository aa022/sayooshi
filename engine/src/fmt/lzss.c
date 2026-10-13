/* fmt/lzss.c — Siglus byte LZSS (FUN_006d1a80 / FUN_006c60f0) and pixel LZSS32 (FUN_006c6180).
 * Every input index is checked against n and every back-reference against the bytes already written, so corrupt
 * input fails instead of reading or writing out of range. Each loop iteration consumes at least one input byte,
 * which bounds the loops by n. */
#include "fmt/lzss.h"
#include "core/bin.h"
#include <string.h>

/* Unpacked size from the header; false when n is too small or the claim is impossible for n input bytes. */
static bool lzss_header(const uint8_t *src, size_t n, uint32_t *unpacked)
{
    Rd r = rd_make(src, n);
    (void)rd_u32le(&r);                                       /* packed size: not used (n is the real bound) */
    uint32_t size = rd_u32le(&r);
    if (!rd_ok(&r)) return false;
    size_t limit;
    if (!core_mul_sz(n - LZSS_HEADER, LZSS_MAX_EXPANSION, &limit)) limit = SIZE_MAX;
    if (size > LZSS_MAX_UNPACKED || size > limit) return false;
    *unpacked = size;
    return true;
}

/* bit 1 = literal byte; bit 0 = u16 v: copy (v & 15) + 2 bytes from (v >> 4) back, byte-wise (may overlap). */
/* Decode exactly dst_len bytes into dst; false on corrupt input (dst is then partly written). */
static bool lzss_decode_into(const uint8_t *src, size_t n, uint8_t *dst, size_t dst_len)
{
    if (!src || n < LZSS_HEADER || (!dst && dst_len)) return false;
    size_t p = LZSS_HEADER, o = 0;
    while (o < dst_len) {
        if (p >= n) return false;
        unsigned flags = src[p++];
        for (int b = 0; b < 8 && o < dst_len; b++, flags >>= 1) {
            if (flags & 1) {
                if (p >= n) return false;
                dst[o++] = src[p++];
                continue;
            }
            if (n - p < 2) return false;
            unsigned v = bin_load_le16(src + p);
            p += 2;
            size_t off = v >> 4, cnt = (v & 15u) + 2;
            if (off == 0 || off > o) return false;
            for (size_t i = 0; i < cnt && o < dst_len; i++, o++) dst[o] = dst[o - off];
        }
    }
    return true;
}

uint8_t *lzss_decode_arena(Arena *arena, const uint8_t *src, size_t n, uint32_t *out_len)
{
    uint32_t size;
    if (out_len) *out_len = 0;
    if (!src || !lzss_header(src, n, &size)) return NULL;
    uint8_t *out = arena_try_alloc(arena, size ? size : 1, 1);
    if (!out || !lzss_decode_into(src, n, out, size)) return NULL;
    if (out_len) *out_len = size;
    return out;
}

uint8_t *lzss_decode(const uint8_t *src, size_t n, uint32_t *out_len)
{
    uint32_t size;
    if (out_len) *out_len = 0;
    if (!src || !lzss_header(src, n, &size)) return NULL;
    uint8_t *out = mem_alloc(size ? size : 1, MEM_FMT);
    if (!lzss_decode_into(src, n, out, size)) {
        mem_free(out);
        return NULL;
    }
    if (out_len) *out_len = size;
    return out;
}

/* bit 1 = literal B,G,R + A = 0xFF; bit 0 = u16 v: copy (v & 15) + 1 dwords from (v >> 4) dwords back. */
bool lzss_decode32(const uint8_t *src, size_t n, uint8_t *dst, uint32_t dst_len)
{
    if (!src || !dst || n < LZSS_HEADER) return false;
    uint32_t size = bin_load_le32(src + 4);
    if (size > dst_len) size = dst_len;
    size_t p = LZSS_HEADER;
    uint32_t o = 0;
    while (o < size) {
        if (p >= n) return false;
        unsigned flags = src[p++];
        for (int b = 0; b < 8 && o < size; b++, flags >>= 1) {
            if (flags & 1) {
                if (n - p < 3) return false;
                const uint8_t px[4] = { src[p], src[p + 1], src[p + 2], 0xFF };
                p += 3;
                for (int i = 0; i < 4 && o < size; i++) dst[o++] = px[i];
                continue;
            }
            if (n - p < 2) return false;
            unsigned v = bin_load_le16(src + p);
            p += 2;
            uint32_t off = (v >> 4) * 4, cnt = ((v & 15u) + 1) * 4;
            if (off == 0 || off > o) return false;
            for (uint32_t i = 0; i < cnt && o < size; i++, o++) dst[o] = dst[o - off];
        }
    }
    if (size < dst_len) memset(dst + size, 0, dst_len - size);
    return true;
}
