/* See obj.h. Decompiled from 0x0202a820.
 *
 * The odd shape of the branch below is the original's, not a tidier equivalent:
 * when a sprite is rot/scale (0x100 or 0x300) the affine index goes in, and
 * when it is a bitmap sprite (mode 3) the 256-colour bit is dropped and the
 * shape word with it. Written out as one 32-bit word holding attr0 and attr1,
 * then attr2 as a halfword, which is how the game writes it.
 */
#include <string.h>

#include "obj.h"

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void ds10_obj_set(ds10_obj *o, int count, int res, int bank, int x, int y,
                  int priority, int xstep, int tile, int palette, int affine,
                  int mode, int mosaic, int rot, int shape, int big,
                  int res_tile, int res_pal)
{
    o->count = count;
    o->res = res;
    o->bank = bank;
    o->x = x;
    o->y = y;
    o->priority = priority;
    o->xstep = xstep;
    o->affine = affine;
    o->mode = mode;
    o->mosaic = mosaic;
    o->rot = rot;
    o->shape = shape;
    o->big = big;
    /* Only a sprite from the default sheet falls back on the resource. */
    if (bank == 0 && tile == 0) o->tile = (res_tile & 0x3FF) + 0x200;
    else o->tile = tile;
    o->palette = palette ? palette : res_pal;
    ds10_obj_build(o);
}

void ds10_obj_build(ds10_obj *o)
{
    int i, n = o->count;

    if (n > DS10_OBJ_MAX) n = DS10_OBJ_MAX;
    for (i = 0; i < n; i++) {
        uint32_t x = (uint32_t)(o->xstep * i + o->x);
        uint32_t y = (uint32_t)o->y;
        uint32_t rot = (uint32_t)o->rot;
        uint32_t w;

        if (rot == 0x100 || rot == 0x300) {
            if (o->mode == 3) {
                w = (y & 0xFF) | ((uint32_t)o->affine << 25);
                w |= (uint32_t)o->shape | ((uint32_t)o->mode << 10) |
                     ((uint32_t)o->mosaic << 12) | ((x & 0x1FF) << 16);
            } else {
                w = (uint32_t)o->shape | (y & 0xFF) |
                    ((uint32_t)o->big << 13) | ((uint32_t)o->affine << 25) |
                    ((uint32_t)o->mode << 10) | ((uint32_t)o->mosaic << 12) |
                    ((x & 0x1FF) << 16);
            }
        } else {
            uint32_t a0 = y & 0xFF;

            if (o->mode != 3) a0 |= (uint32_t)o->big << 13;
            w = (uint32_t)o->shape | a0 | ((uint32_t)o->mode << 10) |
                ((uint32_t)o->mosaic << 12) | ((x & 0x1FF) << 16);
        }
        put32(o->oam + i * 8, rot | w);
        put16(o->oam + i * 8 + 4,
              (uint16_t)((uint32_t)o->tile | ((uint32_t)o->priority << 10) |
                         ((uint32_t)o->palette << 12)));
        put16(o->oam + i * 8 + 6, 0);
    }
}
