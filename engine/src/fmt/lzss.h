/* fmt/lzss.h — Siglus byte LZSS and pixel LZSS32 (FORMATS "Crypto"; AST §1.3).
 * Reference module of docs/CODE_STANDARD.md: validated header, explicit size bounds, bounds-checked decoding,
 * and the caller chooses the lifetime of the output (arena, tracked heap, or its own buffer).
 *
 * Stream: u32 packed size (incl. this 8-byte header; not trusted), u32 unpacked size, then one flag byte per
 * eight items, least significant bit first. */
#ifndef FMT_LZSS_H
#define FMT_LZSS_H
#include "core/base.h"
#include "core/mem.h"

enum {
    LZSS_HEADER = 8,
    LZSS_MAX_EXPANSION = 8,                 /* a copy item turns 2 input bytes into at most 17: <= 8x per group */
    LZSS_MAX_UNPACKED = 64 << 20            /* larger claims are corrupt data */
};

/* Validate + decode into memory from arena (typically a Scratch); NULL on corrupt input or when the arena has no
 * room (recoverable). *out_len = unpacked size. */
uint8_t *lzss_decode_arena(Arena *arena, const uint8_t *src, size_t n, uint32_t *out_len);
/* Same on the tracked heap (MEM_FMT): the caller releases the result with mem_free. */
uint8_t *lzss_decode(const uint8_t *src, size_t n, uint32_t *out_len);
/* Pixel variant (g00 type 0): fills dst_len bytes, zero-padding what the stream does not cover. */
bool     lzss_decode32(const uint8_t *src, size_t n, uint8_t *dst, uint32_t dst_len);

#endif
