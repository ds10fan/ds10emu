/* Expanding an NCER cell into OAM entries.
 *
 * This is how the interface actually draws: an element is a cell - a small
 * list of OAM entries - placed at a position and optionally run through a 2x2
 * matrix. `0x02069564` does the expansion, `0x02068a88` parks the matrix in an
 * affine slot, and `0x020689bc` appends the result to the OAM shadow.
 *
 * Every screen shares one scratch buffer for the expansion, which is why a
 * search of main RAM finds each OAM entry exactly once: only the last element
 * drawn is still lying around.
 *
 * Positions and matrices are 20.12 fixed point, as the original's are.
 */
#ifndef DS10_APP_CELL_H
#define DS10_APP_CELL_H

#include <stdint.h>

typedef struct {
    int nobj;
    const uint8_t *obj; /* nobj triplets of u16: attr0, attr1, attr2 */
} ds10_cell_ref;

/* Write up to `max` OAM entries for `c` into `dst` (8 bytes each) and return
 * how many. `mtx` is {a, b, c, d} in 20.12 or NULL for no transform; `pos` is
 * {x, y} in 20.12 or NULL. `affine` is the parameter slot the matrix went
 * into, and `dbl` asks for a double-size bounding box so a rotated sprite is
 * not clipped by its own corners. */
int ds10_cell_to_oam(uint8_t *dst, int max, const ds10_cell_ref *c,
                     const int32_t *mtx, const int32_t *pos, uint16_t affine,
                     int dbl);

/* The hardware's sprite dimensions, indexed by shape*4 + size. */
extern const uint16_t ds10_obj_width[16];
extern const uint16_t ds10_obj_height[16];

#endif /* DS10_APP_CELL_H */
