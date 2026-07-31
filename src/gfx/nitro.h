/* The NITRO asset containers, as the game's screens are stored in them.
 *
 * Every file is a 16-byte header naming it, then blocks of an 8-byte header
 * and a payload. Magics are stored byte-reversed on disk ("RGCN" for NCGR),
 * which these readers undo.
 *
 * Nothing here copies: a parsed asset points into the buffer it was given, so
 * the caller keeps that alive. The whole asset set is 4.8 MB and is loaded
 * once, so there is no reason to do anything cleverer.
 */
#ifndef DS10_GFX_NITRO_H
#define DS10_GFX_NITRO_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data; /* tiles, or a linear bitmap if `linear` */
    size_t len;
    int width, height;   /* in tiles; -1 when the file does not say */
    int bpp;             /* 4 or 8 */
    int linear;          /* NCBR: one bitmap rather than a run of tiles */
    uint32_t mapping;
} ds10_ncgr;

typedef struct {
    const uint8_t *data; /* BGR555, little-endian */
    int ncolors;
    int fmt;      /* 3 = 16-colour palettes, 4 = 256-colour */
    int extended;
} ds10_nclr;

typedef struct {
    const uint8_t *data; /* one u16 per cell: tile, flips, palette */
    int width, height;   /* in pixels */
    int mode;
    int ncells;
} ds10_nscr;

/* One OAM entry of a cell: the three attribute words, unshifted. */
typedef struct {
    uint16_t attr[3];
} ds10_cell_obj;

typedef struct {
    int nobj;
    uint16_t attr;
    const uint8_t *obj; /* nobj * 6 bytes */
    int16_t bounds[4];  /* only meaningful when the bank has bounding boxes */
} ds10_cell;

typedef struct {
    int ncells;
    int with_bounds;
    uint32_t mapping;
    const uint8_t *cells; /* ncells * (with_bounds ? 16 : 8) */
    const uint8_t *objs;
    const uint8_t *labels; /* LABL block, or NULL */
    size_t labels_len;
} ds10_ncer;

/* Find a block by its four-character name, e.g. "CHAR". Returns the payload
 * and its length, or NULL. */
const uint8_t *ds10_nitro_block(const uint8_t *blob, size_t len,
                                const char *name, size_t *out_len);

int ds10_ncgr_parse(ds10_ncgr *g, const uint8_t *blob, size_t len);
int ds10_nclr_parse(ds10_nclr *p, const uint8_t *blob, size_t len);
int ds10_nscr_parse(ds10_nscr *s, const uint8_t *blob, size_t len);
int ds10_ncer_parse(ds10_ncer *c, const uint8_t *blob, size_t len);

/* Cell `n` of a bank. Returns 0 on success. */
int ds10_ncer_cell(const ds10_ncer *c, int n, ds10_cell *out);

/* The name of cell `n`, from the LABL block, or NULL. Cells are named in the
 * editor and the game looks them up by those names. */
const char *ds10_ncer_label(const ds10_ncer *c, int n);

#endif /* DS10_GFX_NITRO_H */
