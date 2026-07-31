/* See cell.h. Decompiled from 0x02069564.
 *
 * The arithmetic below is the original's, kept in the order it does it: the
 * entry's own position is taken to 20.12, a cell that is already double-size
 * has half a sprite added back, the matrix is applied about the sprite's
 * centre, and the result is rounded by adding half a pixel before shifting.
 * Doing any of it in a tidier order changes the rounding.
 */
#include <string.h>

#include "cell.h"

/* From 0x0207e7f0 and 0x0207e7d8, indexed shape*4 + size.
 *
 * Shape 3 does not exist on the hardware, and the last four entries of each
 * table are whatever the ROM happens to store next rather than dimensions.
 * They are here because the expander indexes them without checking, so a cell
 * with shape 3 would be positioned by these numbers. None of the ROM's 1715
 * cell entries is shape 3, so this only matters for a cell we build wrong -
 * which is exactly when matching the original is worth something. */
const uint16_t ds10_obj_width[16] = {
    8, 16, 32, 64, 16, 32, 32, 64, 8, 8, 16, 32, 4096, 0, 4096, 0,
};
const uint16_t ds10_obj_height[16] = {
    8, 16, 32, 64, 8, 8, 16, 32, 16, 32, 32, 64, 8, 16, 32, 64,
};

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Shape is attr0 bits 14-15, size is attr1 bits 14-15; together they index the
 * dimension tables. The original reads them out of the packed u32. */
static int dim_index(uint32_t attr)
{
    int shape = (int)((attr & 0xC000u) >> 14);
    int size = (int)((attr & 0xC0000000u) >> 30);

    return shape * 4 + size;
}

int ds10_cell_to_oam(uint8_t *dst, int max, const ds10_cell_ref *c,
                     const int32_t *mtx, const int32_t *pos, uint16_t affine,
                     int dbl)
{
    int i;
    int n = c->nobj;

    if (n > max) n = max;
    for (i = 0; i < n; i++) {
        const uint8_t *src = c->obj + i * 6;
        uint8_t *e = dst + i * 8;
        uint32_t attr;
        int32_t x, y;

        /* attr0 and attr1 as one word, attr2 as a halfword. */
        wr32(e, (uint32_t)rd16(src) | ((uint32_t)rd16(src + 2) << 16));
        e[4] = src[4];
        e[5] = src[5];
        if (!mtx && !pos) continue;

        attr = rd32(e);
        x = (int32_t)((attr & 0x01FF0000u) >> 16);
        if (x > 0xFF) x = (int16_t)((uint16_t)x | 0xFF00u); /* 9-bit signed */
        x *= 0x1000;
        y = (int32_t)(attr & 0xFF);
        if (y > 0x7F) y = (int16_t)((uint16_t)y | 0xFF00u); /* 8-bit signed */
        y *= 0x1000;

        if (mtx) {
            uint32_t rot = attr & 0x300u;
            int32_t hw, hh;
            int64_t px, py;
            int32_t nx;

            if (rot != 0x100 && rot != 0x300) rot = attr & 0x30000300u;
            if (rot == 0x300) {
                /* Already double-size: its position is the corner of the
                 * doubled box, so put the sprite's centre back. */
                x += (int32_t)ds10_obj_width[dim_index(attr)] * 0x800;
                y += (int32_t)ds10_obj_height[dim_index(attr)] * 0x800;
            }
            rot = dbl ? 0x300u : 0x100u;

            px = (int64_t)x * mtx[0] + (int64_t)mtx[2] * y + 0x1000;
            py = (int64_t)x * mtx[1] + (int64_t)mtx[3] * y + 0x1000;

            attr = (attr & 0xC1FFFCFFu) | rot | ((uint32_t)affine << 25);
            wr32(e, attr);

            hw = (int32_t)ds10_obj_width[dim_index(attr)] >> 1;
            hh = (int32_t)ds10_obj_height[dim_index(attr)] >> 1;

            nx = (int32_t)(px >> 12) + mtx[2] * hh + mtx[0] * hw - hw * 0x1000;
            y = (int32_t)(py >> 12) + mtx[3] * hh + mtx[1] * hw - hh * 0x1000;
            if (rot == 0x300) {
                nx -= hw * 0x1000;
                y -= hh * 0x1000;
            }
            x = nx;
        }
        if (pos) {
            x += pos[0];
            y += pos[1];
        }
        attr = rd32(e);
        attr = (attr & 0xFE00FF00u) | (((uint32_t)(y + 0x800) >> 12) & 0xFFu) |
               ((((uint32_t)(x + 0x800) >> 12) & 0x1FFu) << 16);
        wr32(e, attr);
    }
    return n;
}
