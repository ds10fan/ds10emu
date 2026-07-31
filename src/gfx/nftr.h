/* NFTR fonts.
 *
 * The interface's text is not in any tile sheet: the glyphs are rasterised
 * into sprite VRAM at runtime from these. That is why the top screen's OBJ
 * VRAM holds far more than the one file loaded into it, and why searching the
 * assets for the map's captions finds nothing.
 *
 * The format is the usual NITRO container: FINF names the font and points at
 * CGLP (the glyph bitmaps), CWDH (per-glyph widths) and CMAP (character to
 * glyph). DS-10's eight fonts are all 1 bit per pixel, ten pixels tall.
 */
#ifndef DS10_GFX_NFTR_H
#define DS10_GFX_NFTR_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *blob;
    size_t len;

    int cell_w, cell_h;   /* the box a glyph is rasterised into */
    int cell_size;        /* bytes per glyph */
    int bpp;
    int baseline;
    int max_width;
    int line_feed;
    int nglyphs;

    const uint8_t *glyph; /* nglyphs * cell_size */
    uint32_t cwdh;        /* offset of the first CWDH block's data, 0 if none */
    uint32_t cmap;        /* offset of the first CMAP block's data */
} ds10_nftr;

int ds10_nftr_parse(ds10_nftr *f, const uint8_t *blob, size_t len);

/* The glyph a character maps to, walking the CMAP chain. -1 if unmapped. */
int ds10_nftr_index(const ds10_nftr *f, int ch);

/* How far the pen moves after this glyph, and where its ink starts. */
int ds10_nftr_advance(const ds10_nftr *f, int glyph, int *left, int *width);

/* Draw one glyph as coverage - 0 or 1 per byte - into an 8-bit bitmap. */
void ds10_nftr_glyph(const ds10_nftr *f, int glyph, uint8_t *dst, int stride);

/* Draw a NUL-terminated string the same way. Returns the width used. */
int ds10_nftr_text(const ds10_nftr *f, const char *s, uint8_t *dst, int stride,
                   int height);

/* How wide the string would be, without drawing it. */
int ds10_nftr_width(const ds10_nftr *f, const char *s);

#endif /* DS10_GFX_NFTR_H */
