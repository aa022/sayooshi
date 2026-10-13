/* Single translation unit for the vendored stb libraries. */
/* stb_truetype asserts on font contents it does not support (a composite glyph without x/y offsets, cmap format 2,
 * rasterizer rounding checks). Fonts are untrusted input and assert() is compiled in, so a crafted font aborted the
 * game: the asserts are disabled and stb continues with its fallback (fonts are also bounds-checked, see the
 * "sayooshi patch" notes in stb_truetype.h and txt/font_sfnt.c). */
#define STBTT_assert(x) ((void)0)
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* stb_vorbis rebuilds its global CRC table at every stream open ("always init it, to avoid multithread race
 * conditions"), which is itself a data race when several threads open streams (Helgrind/DRD report it). The
 * macro pair below renames the table builder and turns the call in start_decoder into a check of a flag that
 * fmt/ovk.c sets once on the main thread (stbv_crc32_global_init) before decoder threads exist; afterwards the
 * table is only read. The vendored stb_vorbis.c itself is unchanged. */
static void stbv_crc32_build(void);
static void stbv_crc32_ensure(void);
void stbv_crc32_global_init(void);
#define crc32_init(...) STBV_CRC32_##__VA_ARGS__
#define STBV_CRC32_void stbv_crc32_build(void)   /* the definition "static void crc32_init(void)" */
#define STBV_CRC32_ stbv_crc32_ensure()          /* the call "crc32_init();" */
#include "stb_vorbis.c"
#undef crc32_init
#undef STBV_CRC32_void
#undef STBV_CRC32_

static int stbv_crc32_ready;
static void stbv_crc32_ensure(void)
{
    if (!stbv_crc32_ready) {
        stbv_crc32_build();
        stbv_crc32_ready = 1;
    }
}
void stbv_crc32_global_init(void) { stbv_crc32_ensure(); }
