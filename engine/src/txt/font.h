/* txt/font.h — stb_truetype glyph cache, outline/shadow layers (AST §5, WP4); a Japanese and an English face
 * (docs/SPEC_ENPATCH.md §1.3.1, §5, §6.3.1). */
#ifndef TXT_FONT_H
#define TXT_FONT_H
#include "core/base.h"
#include "gfx/canvas.h"
#include "txt/lang.h"

/* Faces. FONT_JA is the Japanese Gothic face (--font, bundled fonts/, GameData font.ttf, system fonts). FONT_EN is
 * the English patch face sayooshieng-Regular.ttf (Kosugi + comma kern pairs), read from the game data
 * (/en/font/sayooshieng-Regular.ttf). Text is drawn with the primary face; a code point the primary face lacks is
 * drawn with the other face. The pen advance is always the engine rule (size or size/2), never the TTF advance. */
enum { FONT_JA = 0, FONT_EN = 1, FONT_FACES = 2 };

/* Loads FONT_JA (ttf_path, or the search list of ENGINE_SPEC §2.12) and FONT_EN (if the data has it). True when
 * at least one face is usable: without a Japanese system font the EN patch face, which covers every JA and EN
 * character of the game (SPEC_ENPATCH §5.3), draws the Japanese text too. */
bool font_init(const char *ttf_path);
bool font_has_face(int face);
/* SPEC_ENPATCH §6.3.1: English uses FONT_EN (font type 3, "sayooshieng"), Japanese FONT_JA; each falls back to
 * the other face when its own is missing. Clears the glyph cache. */
void font_select_for_language(LangId lang);
/* Comma kerning of the EN exe (H5) is active only while the sayooshieng face draws the text. */
bool font_kerning_active(void);
/* Pair kerning of FONT_EN in pixels at `size`, rounded like the patched exe ((v*size + 512) >> 10 with the sign
 * kept, v in 1/1024 em). 0 without the face or the pair. */
int  font_kern_px(uint16_t left, uint16_t right, int size);

/* Font file validation (txt/font_sfnt.c; pure functions of the bytes, fuzzed). A file is handed to stb_truetype
 * only when font_file_valid accepts it: every table inside the file, head/hhea/maxp/hmtx usable, the loca glyph
 * index inside glyf. font_sfnt_offset: table directory of face 0 (sfnt or collection), -1 if not a font. */
bool font_file_valid(const uint8_t *d, size_t n);
bool font_builtin_covers(const char *utf8);   /* selftest: the built-in fallback face maps every code point */
long font_sfnt_offset(const uint8_t *d, size_t n);

typedef struct { int w, h, ox, oy, adv; uint8_t *a; } Glyph;   /* a = alpha 0..255 coverage ramp (w*h, may be NULL) */
/* The cached glyph (NULL for size outside 1..255). Owned by the glyph cache: valid until the next font_glyph,
 * font_draw_*, font_init, font_select_for_language or txt_shutdown call. */
const Glyph *font_glyph(uint16_t code, int size, int effect /*0..3 dilation*/);
void font_draw_char(Canvas *c, uint16_t code, int size, int x, int y, uint32_t moji_rgb, uint32_t fuchi_rgb,
                    uint32_t shadow_rgb, int shadow_mode /*0..3*/, bool bold, int tr);
/* Draw with a given face first (the language selector shows each language in its own face). */
void font_draw_char_face(Canvas *c, int face, uint16_t code, int size, int x, int y, uint32_t moji_rgb,
                         uint32_t fuchi_rgb, uint32_t shadow_rgb, int shadow_mode, int tr);

#endif
