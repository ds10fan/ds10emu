/* The interface's sprite widget.
 *
 * Every moving thing on screen - a step marker, a knob pointer, a digit, a
 * highlight - is one of these: a run of `count` identical sprites laid out
 * along x at a fixed stride, written into a private OAM buffer that the frame
 * later copies into OAM proper.
 *
 * Decompiled from `0x0202a820`, which is the setter, with the same code
 * inlined into each screen's constructor. The field order here is that
 * function's word order, so `ds10_obj` is the game's own 64-byte object.
 */
#ifndef DS10_APP_OBJ_H
#define DS10_APP_OBJ_H

#include <stdint.h>

#define DS10_OBJ_MAX 32

typedef struct {
    int32_t x;        /* [0]  of the first sprite in the run */
    int32_t y;        /* [1] */
    int32_t priority; /* [2]  OAM priority, 0..3 */
    int32_t tile;     /* [3]  already includes the sheet's base */
    int32_t palette;  /* [4] */
    int32_t affine;   /* [5]  rot/scale parameter index */
    int32_t mode;     /* [6]  0 normal, 1 blend, 2 window, 3 bitmap */
    int32_t mosaic;   /* [7] */
    int32_t rot;      /* [8]  0, 0x100 rot/scale, 0x300 double size */
    int32_t shape;    /* [9]  attr0 bits 14-15, taken raw */
    int32_t big;      /* [10] 256-colour */
    uint8_t oam[DS10_OBJ_MAX * 8]; /* [11] the run, as OAM entries */
    int32_t count;    /* [12] */
    int32_t res;      /* [13] which sprite the resource lookup returns */
    int32_t xstep;    /* [14] */
    int32_t bank;     /* [15] which sheet the resource lookup uses */
} ds10_obj;

/* Rebuild the run's OAM entries from the fields above. Called after anything
 * that moves or changes the sprite. */
void ds10_obj_build(ds10_obj *o);

/* Set every field and rebuild, in the argument order the game's own setter
 * uses. `tile` and `palette` of zero mean "take the default for this sprite",
 * which is where `res_tile` and `res_pal` come in: they are what the resource
 * manager returns, and the sheet base of 0x200 is added to the tile. */
void ds10_obj_set(ds10_obj *o, int count, int res, int bank, int x, int y,
                  int priority, int xstep, int tile, int palette, int affine,
                  int mode, int mosaic, int rot, int shape, int big,
                  int res_tile, int res_pal);

#endif /* DS10_APP_OBJ_H */
