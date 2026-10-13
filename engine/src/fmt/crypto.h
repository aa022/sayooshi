/* fmt/crypto.h — embedded key tables and the cyclic XOR (FORMATS "Crypto").
 * Constants transcribed from tools/siglus_crypto.py and the exe (MODE.CGM XOR table). CRC-32 lives in
 * core/hash.h. No state; thread-safe. */
#ifndef FMT_CRYPTO_H
#define FMT_CRYPTO_H
#include "core/base.h"

extern const uint8_t crypto_exe_key[16], crypto_scene_table[256], crypto_gameexe_table[256], crypto_cgm_table[256];

/* d[i] ^= key[(start + i) % klen]; klen 0 is a no-op. */
void crypto_xor_cycle(uint8_t *d, size_t n, const uint8_t *key, size_t klen, size_t start);

#endif
