/* core/hash.h — checksums and hashes over byte ranges. No state, no allocation, thread-safe. */
#ifndef CORE_HASH_H
#define CORE_HASH_H
#include <stddef.h>
#include <stdint.h>

/* IEEE 802.3 / zlib / PNG CRC-32 (reflected polynomial 0xEDB88320). Start with crc = 0 and chain:
 * hash_crc32(hash_crc32(0, a, na), b, nb) == hash_crc32(0, a||b, na + nb). data may be NULL when n == 0. */
uint32_t hash_crc32(uint32_t crc, const void *data, size_t n);

/* 64-bit FNV-1a. Start with HASH_FNV64_INIT and chain like hash_crc32. */
#define HASH_FNV64_INIT 0xcbf29ce484222325ull
uint64_t hash_fnv64(uint64_t h, const void *data, size_t n);

#endif
