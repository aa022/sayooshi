/* fmt/g00.h — g00 type 0/1/2 decoding into cuts (AST §1).
 * Memory: a G00 is one tracked heap block (the struct, its cuts and all pixels); release it with g00_free only.
 * The pixels may be modified in place by the owner (gfx premultiplies them). Main thread (decode scratch). */
#ifndef FMT_G00_H
#define FMT_G00_H
#include "core/base.h"

typedef struct {
    int w, h;
    int cx, cy;
    uint32_t *px;       /* px = straight-alpha 0xAARRGGBB (=BGRA bytes LE); NULL if empty */
    int x1, y1, x2, y2; /* drawn-content rect inside the canvas (x2/y2 exclusive) */
    int nchunk;
    bool has_alpha; /* any chunk flag 1 (type 1: palette alpha < 255) */
} G00Cut;
/* Empty index entries (type 2, offset or size 0) are zero cuts: w=h=0, px=NULL. A cut with 0 chunks is a
 * zero-filled (transparent) canvas of its canvas size. Type 0/1: exactly one cut, center (0,0). */
typedef struct { int type; int ncut; G00Cut *cut; int sheet_w, sheet_h; } G00;

G00 *g00_decode(const uint8_t *data, size_t n);   /* NULL on corrupt data or a canvas over the pixel cap */
void g00_free(G00 *g);                            /* NULL is a no-op */

/* Decode every g00/ file and compare per-cut size+CRC32 with ref_tsv (tests/ref/g00_crc.tsv).
 * Returns the number of failures; prints the M0 counts. */

#endif
