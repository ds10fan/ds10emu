/* The DS 2D hardware. See nds2d.h for what this is and why it is shaped the
 * way it is.
 *
 * Scanline order, matching the hardware: the backdrop fills the line, each
 * enabled background draws over it lowest priority first, sprites interleave
 * by priority, and then the colour effects run over the top two layers each
 * pixel kept. A pixel in flight is 6 bits per channel with layer flags in the
 * top byte; see color.h.
 */
#include <string.h>

#include "nds2d.h"
#include "color.h"

const char *ds10_2d_unsupported;

/* Layer flags in the top byte of a pixel in flight. The low five bits pick out
 * which layer put it there, in the same order BLDCNT's target bits use. */
#define F_BG0    0x01000000u
#define F_BD     0x20000000u  /* backdrop */
#define F_3D     0x40000000u
#define F_SPR    0x80000000u

/* Sprite line entries. The colour index lives in the low bits; the rest says
 * how to read it and how it composites. */
#define O_STDPAL   (1u << 12)
#define O_DIRECT   (1u << 15)
#define O_PRIO     (3u << 16)
#define O_OPAQUE   (1u << 18)
#define O_SPRITE   (1u << 19)
#define O_MOSAIC   (1u << 20)
#define O_OPAPRIO  (O_PRIO | O_OPAQUE)

typedef struct {
    uint32_t line[DS10_SCR_W * 2]; /* top layer, then the one under it */
    uint32_t obj[DS10_SCR_W];
    uint8_t objwin[DS10_SCR_W];
    uint8_t winmask[DS10_SCR_W];
} linebuf;

/* ------------------------------------------------------------------ VRAM */

/* One bank's contribution to a region: OR its bytes in at every block it is
 * mapped to, wrapping by the bank's own size. Overlapping banks OR together,
 * which is what the hardware does. */
static void spread(uint8_t *dst, size_t dstlen, const uint8_t *src,
                   uint32_t srcmask, const int *blocks, int nblocks,
                   size_t blocksize)
{
    int i;
    size_t j;

    for (i = 0; i < nblocks; i++) {
        size_t base = (size_t)blocks[i] * blocksize;

        if (base >= dstlen) continue;
        for (j = 0; j < blocksize; j++)
            dst[base + j] |= src[(base + j) & srcmask];
    }
}

#define BLK16 (16 * 1024)
#define BLK8 (8 * 1024)

void ds10_vram_flatten(ds10_vram *v)
{
    static const size_t sz[9] = {
        128 * 1024, 128 * 1024, 128 * 1024, 128 * 1024, 64 * 1024,
        16 * 1024, 16 * 1024, 32 * 1024, 16 * 1024,
    };
    const uint8_t *bank[9];
    uint32_t bmask[9];
    int i, blocks[8], n;

    bank[0] = v->a; bank[1] = v->b; bank[2] = v->c; bank[3] = v->d;
    bank[4] = v->e; bank[5] = v->f; bank[6] = v->g; bank[7] = v->h;
    bank[8] = v->i;
    for (i = 0; i < 9; i++) bmask[i] = (uint32_t)sz[i] - 1;

    memset(v->abg, 0, sizeof v->abg);
    memset(v->bbg, 0, sizeof v->bbg);
    memset(v->aobj, 0, sizeof v->aobj);
    memset(v->bobj, 0, sizeof v->bobj);
    memset(v->abgextpal, 0, sizeof v->abgextpal);
    memset(v->bbgextpal, 0, sizeof v->bbgextpal);
    memset(v->aobjextpal, 0, sizeof v->aobjextpal);
    memset(v->bobjextpal, 0, sizeof v->bobjextpal);

    for (i = 0; i < 9; i++) {
        uint8_t cnt = v->vramcnt[i];
        int mst = cnt & 7, ofs = (cnt >> 3) & 3, k;

        if (!(cnt & 0x80)) continue;

        switch (i) {
        case 0: case 1: /* A, B: 128 KB, eight 16 KB blocks */
            mst = cnt & 3;
            if (mst == 1) {
                for (k = 0; k < 8; k++) blocks[k] = (ofs << 3) + k;
                spread(v->abg, sizeof v->abg, bank[i], bmask[i], blocks, 8,
                       BLK16);
            } else if (mst == 2) {
                for (k = 0; k < 8; k++) blocks[k] = ((ofs & 1) << 3) + k;
                spread(v->aobj, sizeof v->aobj, bank[i], bmask[i], blocks, 8,
                       BLK16);
            }
            /* mst 0 is LCDC, mst 3 is a 3D texture: neither is a 2D source. */
            break;

        case 2: /* C */
            ofs = (cnt >> 3) & 7;
            if (mst == 1) {
                for (k = 0; k < 8; k++) blocks[k] = ((ofs & 3) << 3) + k;
                spread(v->abg, sizeof v->abg, bank[i], bmask[i], blocks, 8,
                       BLK16);
            } else if (mst == 4) {
                for (k = 0; k < 8; k++) blocks[k] = k;
                spread(v->bbg, sizeof v->bbg, bank[i], bmask[i], blocks, 8,
                       BLK16);
            }
            break;

        case 3: /* D */
            ofs = (cnt >> 3) & 7;
            if (mst == 1) {
                for (k = 0; k < 8; k++) blocks[k] = ((ofs & 3) << 3) + k;
                spread(v->abg, sizeof v->abg, bank[i], bmask[i], blocks, 8,
                       BLK16);
            } else if (mst == 4) {
                for (k = 0; k < 8; k++) blocks[k] = k;
                spread(v->bobj, sizeof v->bobj, bank[i], bmask[i], blocks, 8,
                       BLK16);
            }
            break;

        case 4: /* E: 64 KB, four 16 KB blocks, always at the bottom */
            for (k = 0; k < 4; k++) blocks[k] = k;
            if (mst == 1)
                spread(v->abg, sizeof v->abg, bank[i], bmask[i], blocks, 4,
                       BLK16);
            else if (mst == 2)
                spread(v->aobj, sizeof v->aobj, bank[i], bmask[i], blocks, 4,
                       BLK16);
            else if (mst == 4)
                spread(v->abgextpal, sizeof v->abgextpal, bank[i], 0x7FFF,
                       blocks, 4, BLK8);
            break;

        case 5: case 6: /* F, G: 16 KB, mapped into two blocks 32 KB apart */
            ofs = (cnt >> 3) & 7;
            n = (ofs & 1) + ((ofs & 2) << 1);
            blocks[0] = n;
            blocks[1] = n + 2;
            if (mst == 1)
                spread(v->abg, sizeof v->abg, bank[i], bmask[i], blocks, 2,
                       BLK16);
            else if (mst == 2)
                spread(v->aobj, sizeof v->aobj, bank[i], bmask[i], blocks, 2,
                       BLK16);
            else if (mst == 4) {
                blocks[0] = (ofs & 1) << 1;
                blocks[1] = ((ofs & 1) << 1) + 1;
                spread(v->abgextpal, sizeof v->abgextpal, bank[i], 0x3FFF,
                       blocks, 2, BLK8);
            } else if (mst == 5) {
                /* OBJ-A extended palette. melonDS has an off-by-one in the
                 * bank bit it checks here, so the two disagree if a game uses
                 * it; DS-10 never enables OBJ extended palettes (DISPCNT bit
                 * 31 is clear), so nothing exercises the difference. */
                blocks[0] = 0;
                spread(v->aobjextpal, sizeof v->aobjextpal, bank[i], 0x1FFF,
                       blocks, 1, BLK8);
            }
            break;

        case 7: /* H: 32 KB */
            if (mst == 1) {
                blocks[0] = 0; blocks[1] = 1; blocks[2] = 4; blocks[3] = 5;
                spread(v->bbg, sizeof v->bbg, bank[i], bmask[i], blocks, 4,
                       BLK16);
            } else if (mst == 2) {
                for (k = 0; k < 4; k++) blocks[k] = k;
                spread(v->bbgextpal, sizeof v->bbgextpal, bank[i], 0x7FFF,
                       blocks, 4, BLK8);
            }
            break;

        case 8: /* I: 16 KB */
            if (mst == 1) {
                blocks[0] = 2; blocks[1] = 3; blocks[2] = 6; blocks[3] = 7;
                spread(v->bbg, sizeof v->bbg, bank[i], bmask[i], blocks, 4,
                       BLK16);
            } else if (mst == 2) {
                for (k = 0; k < 8; k++) blocks[k] = k;
                spread(v->bobj, sizeof v->bobj, bank[i], bmask[i], blocks, 8,
                       BLK16);
            } else if (mst == 3) {
                blocks[0] = 0;
                spread(v->bobjextpal, sizeof v->bobjextpal, bank[i], 0x1FFF,
                       blocks, 1, BLK8);
            }
            break;
        }
    }
}

void ds10_2dmem_bind(ds10_2dmem *mem, const ds10_vram *v,
                     const uint8_t *palette, int num)
{
    if (num == 0) {
        mem->bg = v->abg;
        mem->bgmask = 0x7FFFF;
        mem->obj = v->aobj;
        mem->objmask = 0x3FFFF;
        mem->bgextpal = v->abgextpal;
        mem->objextpal = v->aobjextpal;
        mem->pal = palette;
    } else {
        mem->bg = v->bbg;
        mem->bgmask = 0x1FFFF;
        mem->obj = v->bobj;
        mem->objmask = 0x1FFFF;
        mem->bgextpal = v->bbgextpal;
        mem->objextpal = v->bobjextpal;
        mem->pal = palette + 0x400;
    }
}

/* --------------------------------------------------------------- helpers */

static uint16_t rd16(const uint8_t *p, uint32_t off)
{
    return (uint16_t)(p[off] | (p[off + 1] << 8));
}

static void put(uint32_t *slot, uint16_t color, uint32_t flag)
{
    slot[DS10_SCR_W] = *slot;
    *slot = ds10_col15to18(color) | flag;
}

/* Extended background palettes are 8 KB slots of sixteen 256-colour
 * palettes. */
static const uint8_t *bg_extpal(const ds10_2dmem *m, int slot, int pal)
{
    return m->bgextpal + slot * 8192 + pal * 512;
}

/* ---------------------------------------------------------- backgrounds */

static void draw_bg_text(const ds10_2d *e, const ds10_2dmem *m, int num,
                         int bg, int line, linebuf *lb)
{
    uint16_t bgcnt = e->bgcnt[bg];
    uint32_t tileset, tilemap, widex, extpal, extslot = 0;
    uint16_t xoff = e->bgx[bg];
    uint16_t yoff = (uint16_t)(e->bgy[bg] + line);
    const uint8_t *vram = m->bg;
    uint32_t mask = m->bgmask;
    const uint8_t *pal;
    uint16_t tile = 0;
    const uint8_t *curpal;
    uint32_t pixels = 0;
    int i;

    widex = (bgcnt & (1 << 14)) ? 0x100 : 0;
    extpal = e->dispcnt & (1u << 30);
    if (extpal) extslot = ((bg < 2) && (bgcnt & 0x2000)) ? (uint32_t)(2 + bg)
                                                        : (uint32_t)bg;
    if (num) {
        tileset = (uint32_t)(bgcnt & 0x003C) << 12;
        tilemap = (uint32_t)(bgcnt & 0x1F00) << 3;
    } else {
        tileset = ((e->dispcnt & 0x07000000) >> 8) +
                  ((uint32_t)(bgcnt & 0x003C) << 12);
        tilemap = ((e->dispcnt & 0x38000000) >> 11) +
                  ((uint32_t)(bgcnt & 0x1F00) << 3);
    }
    pal = m->pal;

    if (bgcnt & (1 << 15)) {
        tilemap += (uint32_t)(yoff & 0x1F8) << 3;
        if (bgcnt & (1 << 14)) tilemap += (uint32_t)(yoff & 0x100) << 3;
    } else {
        tilemap += (uint32_t)(yoff & 0xF8) << 3;
    }

    curpal = pal;
    if (bgcnt & (1 << 7)) {
        /* 256 colours */
        for (i = 0; i < DS10_SCR_W; i++, xoff++) {
            uint32_t tx;
            uint8_t c;

            if (i == 0 || !(xoff & 7)) {
                tile = rd16(vram, (tilemap + ((xoff & 0xF8) >> 2) +
                                   ((xoff & widex) << 3)) & mask);
                curpal = extpal ? bg_extpal(m, (int)extslot, tile >> 12) : pal;
                pixels = tileset + ((uint32_t)(tile & 0x03FF) << 6) +
                         (uint32_t)(((tile & (1 << 11)) ? (7 - (yoff & 7))
                                                        : (yoff & 7)) << 3);
            }
            if (!(lb->winmask[i] & (1 << bg))) continue;
            tx = (tile & (1 << 10)) ? (uint32_t)(7 - (xoff & 7))
                                    : (uint32_t)(xoff & 7);
            c = vram[(pixels + tx) & mask];
            if (c) put(&lb->line[i], rd16(curpal, (uint32_t)c * 2),
                       F_BG0 << bg);
        }
    } else {
        /* 16 colours */
        for (i = 0; i < DS10_SCR_W; i++, xoff++) {
            uint32_t tx;
            uint8_t c;

            if (i == 0 || !(xoff & 7)) {
                tile = rd16(vram, (tilemap + ((xoff & 0xF8) >> 2) +
                                   ((xoff & widex) << 3)) & mask);
                curpal = pal + ((tile & 0xF000) >> 8);
                pixels = tileset + ((uint32_t)(tile & 0x03FF) << 5) +
                         (uint32_t)(((tile & (1 << 11)) ? (7 - (yoff & 7))
                                                        : (yoff & 7)) << 2);
            }
            if (!(lb->winmask[i] & (1 << bg))) continue;
            tx = (tile & (1 << 10)) ? (uint32_t)(7 - (xoff & 7))
                                    : (uint32_t)(xoff & 7);
            c = vram[(pixels + (tx >> 1)) & mask];
            c = (tx & 1) ? (uint8_t)(c >> 4) : (uint8_t)(c & 0x0F);
            if (c) put(&lb->line[i], rd16(curpal, (uint32_t)c * 2),
                       F_BG0 << bg);
        }
    }
}

static void draw_bg_3d(const uint32_t *plane, linebuf *lb)
{
    int i;

    for (i = 0; i < DS10_SCR_W; i++) {
        uint32_t c = plane[i];

        if ((c >> 24) == 0) continue;
        if (!(lb->winmask[i] & 0x01)) continue;
        lb->line[i + DS10_SCR_W] = lb->line[i];
        lb->line[i] = c | F_3D;
    }
}

/* -------------------------------------------------------------- sprites */

static void spr_pixel(linebuf *lb, int window, int color, uint32_t attr,
                      int x)
{
    if (window) {
        if (color != -1) lb->objwin[x] = 1;
        return;
    }
    {
        uint32_t old = lb->obj[x];
        int oldopaque = (old & O_OPAQUE) != 0;
        int newopaque = color != -1;
        int prio = (attr & O_PRIO) < (old & O_PRIO);

        if (newopaque && (!oldopaque || prio)) {
            lb->obj[x] = (uint32_t)color | attr;
        } else if (!newopaque && !oldopaque) {
            lb->obj[x] &= ~(O_MOSAIC | O_PRIO);
            lb->obj[x] |= attr & (O_SPRITE | O_MOSAIC | O_PRIO);
        }
    }
}

static void draw_sprite_normal(const ds10_2d *e, const ds10_2dmem *m,
                               const uint8_t *oam, int sprnum, int window,
                               int width, int height, int xpos, int ypos,
                               linebuf *lb)
{
    const uint8_t *a = oam + sprnum * 8;
    uint16_t a0 = rd16(a, 0), a1 = rd16(a, 2), a2 = rd16(a, 4);
    uint32_t attr = ((uint32_t)(a2 & 0x0C00) << 6) | O_SPRITE | O_OPAQUE;
    uint32_t tilenum = a2 & 0x03FF;
    int mode = window ? 0 : ((a0 >> 10) & 3);
    uint32_t wmask = (uint32_t)width - 8;
    const uint8_t *vram = m->obj;
    uint32_t vmask = m->objmask;
    uint32_t pixels;
    int xoff, xend = width;
    int32_t stride;

    if ((a0 & (1 << 12)) && !window) attr |= O_MOSAIC;
    if (a1 & (1 << 13)) ypos = height - 1 - ypos; /* vertical flip */

    if (xpos >= 0) {
        xoff = 0;
        if (xpos + xend > DS10_SCR_W) xend = DS10_SCR_W - xpos;
    } else {
        xoff = -xpos;
        xpos = 0;
    }

    if (mode == 3) {
        /* Bitmap sprite: 16-bit colour straight out of OBJ VRAM. */
        uint32_t alpha = (uint32_t)(a2 >> 12);

        if (!alpha) return;
        alpha++;
        attr |= 0xC0000000u | (alpha << 24);
        pixels = tilenum;
        if (e->dispcnt & 0x40) {
            if (e->dispcnt & 0x20) return; /* reserved: draws nothing */
            pixels <<= 7 + ((e->dispcnt >> 22) & 1);
            pixels += (uint32_t)(ypos * width * 2);
        } else if (e->dispcnt & 0x20) {
            pixels = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
            pixels += (uint32_t)(ypos * 256 * 2);
        } else {
            pixels = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
            pixels += (uint32_t)(ypos * 128 * 2);
        }
        if (a1 & (1 << 12)) {
            pixels += (uint32_t)((width - 1) << 1);
            pixels -= (uint32_t)(xoff << 1);
            stride = -2;
        } else {
            pixels += (uint32_t)(xoff << 1);
            stride = 2;
        }
        for (; xoff < xend; xoff++, xpos++) {
            uint16_t c = rd16(vram, pixels & vmask);

            pixels = (uint32_t)((int32_t)pixels + stride);
            spr_pixel(lb, window, (c & 0x8000) ? c : -1, attr, xpos);
        }
        return;
    }

    pixels = tilenum;
    if (e->dispcnt & (1 << 4)) {
        pixels <<= (e->dispcnt >> 20) & 3;
        pixels += (uint32_t)(((ypos >> 3) * (width >> 3))
                             << ((a0 & 0x2000) ? 1 : 0));
    } else {
        pixels += (uint32_t)((ypos >> 3) * 0x20);
    }
    attr |= (mode == 1) ? 0x80000000u : 0x10000000u;

    if (a0 & (1 << 13)) {
        /* 256 colours */
        pixels <<= 5;
        pixels += (uint32_t)((ypos & 7) << 3);
        if (!window) {
            if (!(e->dispcnt & (1u << 31))) attr |= O_STDPAL;
            else attr |= (uint32_t)((a2 & 0xF000) >> 4);
        }
        if (a1 & (1 << 12)) {
            pixels += ((uint32_t)(width - 1) & wmask) << 3;
            pixels += (uint32_t)((width - 1) & 7);
            pixels -= ((uint32_t)xoff & wmask) << 3;
            pixels -= (uint32_t)(xoff & 7);
            stride = -1;
        } else {
            pixels += ((uint32_t)xoff & wmask) << 3;
            pixels += (uint32_t)(xoff & 7);
            stride = 1;
        }
        for (; xoff < xend; xoff++, xpos++) {
            uint8_t c = vram[pixels & vmask];

            pixels = (uint32_t)((int32_t)pixels + stride);
            spr_pixel(lb, window, c ? c : -1, attr, xpos);
            if (!((xoff + 1) & 7)) pixels = (uint32_t)((int32_t)pixels +
                                                       56 * stride);
        }
    } else {
        /* 16 colours */
        pixels <<= 5;
        pixels += (uint32_t)((ypos & 7) << 2);
        if (!window) {
            attr |= O_STDPAL;
            attr |= (uint32_t)((a2 & 0xF000) >> 8);
        }
        if (a1 & (1 << 12)) {
            pixels += ((uint32_t)(width - 1) & wmask) << 2;
            pixels += (uint32_t)(((width - 1) & 7) >> 1);
            pixels -= ((uint32_t)xoff & wmask) << 2;
            pixels -= (uint32_t)((xoff & 7) >> 1);
        } else {
            pixels += ((uint32_t)xoff & wmask) << 2;
            pixels += (uint32_t)((xoff & 7) >> 1);
        }
        for (; xoff < xend; xoff++, xpos++) {
            uint8_t c;

            if (a1 & (1 << 12)) {
                if (xoff & 1) { c = vram[pixels & vmask] & 0x0F; pixels--; }
                else c = (uint8_t)(vram[pixels & vmask] >> 4);
            } else {
                if (xoff & 1) { c = (uint8_t)(vram[pixels & vmask] >> 4);
                                pixels++; }
                else c = vram[pixels & vmask] & 0x0F;
            }
            spr_pixel(lb, window, c ? c : -1, attr, xpos);
            if (!((xoff + 1) & 7))
                pixels = (uint32_t)((int32_t)pixels +
                                    ((a1 & 0x1000) ? -28 : 28));
        }
    }
}

static void draw_sprite_rotscale(const ds10_2d *e, const ds10_2dmem *m,
                                 const uint8_t *oam, int sprnum, int window,
                                 int bw, int bh, int width, int height,
                                 int xpos, int ypos, linebuf *lb)
{
    const uint8_t *a = oam + sprnum * 8;
    uint16_t a0 = rd16(a, 0), a1 = rd16(a, 2), a2 = rd16(a, 4);
    uint32_t attr = ((uint32_t)(a2 & 0x0C00) << 6) | O_SPRITE | O_OPAQUE;
    uint32_t tilenum = a2 & 0x03FF;
    int mode = window ? 0 : ((a0 >> 10) & 3);
    const uint8_t *vram = m->obj;
    uint32_t vmask = m->objmask;
    uint32_t paramidx = (uint32_t)((a1 >> 9) & 0x1F);
    int16_t pa = (int16_t)rd16(oam, paramidx * 32 + 6);
    int16_t pb = (int16_t)rd16(oam, paramidx * 32 + 14);
    int16_t pc = (int16_t)rd16(oam, paramidx * 32 + 22);
    int16_t pd = (int16_t)rd16(oam, paramidx * 32 + 30);
    int32_t rotx, roty;
    int xoff, xend = bw;
    uint32_t base;

    if ((a0 & (1 << 12)) && !window) attr |= O_MOSAIC;

    if (xpos >= 0) {
        xoff = 0;
        if (xpos + xend > DS10_SCR_W) xend = DS10_SCR_W - xpos;
    } else {
        xoff = -xpos;
        xpos = 0;
    }

    /* Walk the source with the inverse transform, starting from the centre. */
    rotx = ((xoff - (bw >> 1)) * pa) + ((ypos - (bh >> 1)) * pb) +
           ((width >> 1) << 8);
    roty = ((xoff - (bw >> 1)) * pc) + ((ypos - (bh >> 1)) * pd) +
           ((height >> 1) << 8);

    if (mode == 3) {
        uint32_t alpha = (uint32_t)(a2 >> 12);

        if (!alpha) return;
        alpha++;
        attr |= 0xC0000000u | (alpha << 24);
        base = tilenum;
        if (e->dispcnt & 0x40) {
            if (e->dispcnt & 0x20) return;
            base <<= 7 + ((e->dispcnt >> 22) & 1);
        } else if (e->dispcnt & 0x20) {
            base = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
        } else {
            base = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
        }
        for (; xoff < xend; xoff++, xpos++, rotx += pa, roty += pc) {
            int sx = rotx >> 8, sy = roty >> 8;
            uint16_t c;
            uint32_t addr;

            if (sx < 0 || sx >= width || sy < 0 || sy >= height) {
                spr_pixel(lb, window, -1, attr, xpos);
                continue;
            }
            if (e->dispcnt & 0x40)
                addr = base + (uint32_t)((sy * width + sx) * 2);
            else if (e->dispcnt & 0x20)
                addr = base + (uint32_t)((sy * 256 + sx) * 2);
            else
                addr = base + (uint32_t)((sy * 128 + sx) * 2);
            c = rd16(vram, addr & vmask);
            spr_pixel(lb, window, (c & 0x8000) ? c : -1, attr, xpos);
        }
        return;
    }

    base = tilenum;
    if (e->dispcnt & (1 << 4)) base <<= (e->dispcnt >> 20) & 3;
    base <<= 5;
    attr |= (mode == 1) ? 0x80000000u : 0x10000000u;

    if (a0 & (1 << 13)) {
        uint32_t rowbytes = (e->dispcnt & (1 << 4))
                          ? (uint32_t)(width >> 3) * 64 : 1024;

        if (!window) {
            if (!(e->dispcnt & (1u << 31))) attr |= O_STDPAL;
            else attr |= (uint32_t)((a2 & 0xF000) >> 4);
        }
        for (; xoff < xend; xoff++, xpos++, rotx += pa, roty += pc) {
            int sx = rotx >> 8, sy = roty >> 8;
            uint32_t addr;
            uint8_t c;

            if (sx < 0 || sx >= width || sy < 0 || sy >= height) {
                spr_pixel(lb, window, -1, attr, xpos);
                continue;
            }
            addr = base + (uint32_t)(sy >> 3) * rowbytes +
                   (uint32_t)(sx >> 3) * 64 + (uint32_t)((sy & 7) << 3) +
                   (uint32_t)(sx & 7);
            c = vram[addr & vmask];
            spr_pixel(lb, window, c ? c : -1, attr, xpos);
        }
    } else {
        uint32_t rowbytes = (e->dispcnt & (1 << 4))
                          ? (uint32_t)(width >> 3) * 32 : 1024;

        if (!window) {
            attr |= O_STDPAL;
            attr |= (uint32_t)((a2 & 0xF000) >> 8);
        }
        for (; xoff < xend; xoff++, xpos++, rotx += pa, roty += pc) {
            int sx = rotx >> 8, sy = roty >> 8;
            uint32_t addr;
            uint8_t c;

            if (sx < 0 || sx >= width || sy < 0 || sy >= height) {
                spr_pixel(lb, window, -1, attr, xpos);
                continue;
            }
            addr = base + (uint32_t)(sy >> 3) * rowbytes +
                   (uint32_t)(sx >> 3) * 32 + (uint32_t)((sy & 7) << 2) +
                   (uint32_t)((sx & 7) >> 1);
            c = vram[addr & vmask];
            c = (sx & 1) ? (uint8_t)(c >> 4) : (uint8_t)(c & 0x0F);
            spr_pixel(lb, window, c ? c : -1, attr, xpos);
        }
    }
}

static const int SPR_W[16] = {
    8, 16, 8, 8, 16, 32, 8, 8, 32, 32, 16, 8, 64, 64, 32, 8,
};
static const int SPR_H[16] = {
    8, 8, 16, 8, 16, 8, 32, 8, 32, 16, 32, 8, 64, 32, 64, 8,
};

static void draw_sprites(const ds10_2d *e, const ds10_2dmem *m,
                         const uint8_t *oam, int line, linebuf *lb)
{
    int n;

    memset(lb->obj, 0, sizeof lb->obj);
    memset(lb->objwin, 0, sizeof lb->objwin);
    if (!(e->dispcnt & (1 << 12))) return;

    for (n = 0; n < 128; n++) {
        const uint8_t *a = oam + n * 8;
        uint16_t a0 = rd16(a, 0), a1 = rd16(a, 2);
        int type = (a0 >> 8) & 3;
        int window = ((a0 >> 10) & 3) == 2;
        int size, w, h, bw, bh, xpos, ypos;

        if (type == 2) continue; /* disabled */
        size = (a0 >> 14) | ((a1 & 0xC000) >> 12);
        w = SPR_W[size];
        h = SPR_H[size];
        bw = w;
        bh = h;
        if (type == 3) { bw <<= 1; bh <<= 1; } /* double-size rot/scale */

        ypos = a0 & 0xFF;
        if (((line - ypos) & 0xFF) >= bh) continue;
        xpos = (int32_t)((uint32_t)a1 << 23) >> 23;
        if (xpos <= -bw) continue;
        ypos = (line - ypos) & 0xFF;

        if (type & 1)
            draw_sprite_rotscale(e, m, oam, n, window, bw, bh, w, h, xpos,
                                 ypos, lb);
        else
            draw_sprite_normal(e, m, oam, n, window, w, h, xpos, ypos, lb);
    }
}

static void interleave_sprites(const ds10_2dmem *m, uint32_t prio, linebuf *lb)
{
    uint32_t want = (prio << 16) | O_OPAQUE;
    const uint8_t *pal = m->pal + 0x200;
    const uint8_t *ext = m->objextpal;
    int i;

    for (i = 0; i < DS10_SCR_W; i++) {
        uint32_t px = lb->obj[i];
        uint16_t color;

        if ((px & O_OPAPRIO) != want) continue;
        if (!(lb->winmask[i] & 0x10)) continue;
        if (px & O_DIRECT) color = (uint16_t)(px & 0x7FFF);
        else if (px & O_STDPAL) color = rd16(pal, (px & 0xFF) * 2);
        else color = rd16(ext, (px & 0xFFF) * 2);
        put(&lb->line[i], color, px & 0xFF000000u);
    }
}

/* --------------------------------------------------------------- windows */

static void window_mask(ds10_2d *e, int line, linebuf *lb)
{
    int i;

    if (!(e->dispcnt & 0xE000)) {
        memset(lb->winmask, 0xFF, sizeof lb->winmask);
        return;
    }
    for (i = 0; i < DS10_SCR_W; i++) lb->winmask[i] = e->wincnt[2];

    if (e->dispcnt & (1 << 15))
        for (i = 0; i < DS10_SCR_W; i++)
            if (lb->objwin[i]) lb->winmask[i] = e->wincnt[3];

    /* The vertical edges latch as the line counter passes them, which is why
     * y1 == y2 leaves the window on for the whole frame rather than off. */
    if (e->dispcnt & (1 << 14)) {
        uint8_t x1 = e->win1[0], x2 = e->win1[1];

        if (line == e->win1[3]) e->win1act &= (uint8_t)~0x1;
        if (line == e->win1[2]) e->win1act |= 0x1;
        for (i = 0; i < DS10_SCR_W; i++) {
            if (i == x2) e->win1act &= (uint8_t)~0x2;
            else if (i == x1) e->win1act |= 0x2;
            if (e->win1act == 0x3) lb->winmask[i] = e->wincnt[1];
        }
    }
    if (e->dispcnt & (1 << 13)) {
        uint8_t x1 = e->win0[0], x2 = e->win0[1];

        if (line == e->win0[3]) e->win0act &= (uint8_t)~0x1;
        if (line == e->win0[2]) e->win0act |= 0x1;
        for (i = 0; i < DS10_SCR_W; i++) {
            if (i == x2) e->win0act &= (uint8_t)~0x2;
            else if (i == x1) e->win0act |= 0x2;
            if (e->win0act == 0x3) lb->winmask[i] = e->wincnt[0];
        }
    }
}

/* -------------------------------------------------------------- compose */

static uint32_t composite(const ds10_2d *e, const linebuf *lb, int i)
{
    uint32_t v1 = lb->line[i], v2 = lb->line[i + DS10_SCR_W];
    uint32_t f1 = v1 >> 24, f2 = v2 >> 24;
    uint32_t bld = e->blendcnt, target2, effect = 0, eva = 0, evb = 0;

    if (f2 & 0x80) target2 = 0x1000;
    else if (f2 & 0x40) target2 = 0x0100;
    else target2 = f2 << 8;

    if ((f1 & 0x80) && (bld & target2)) {
        /* A semi-transparent sprite blends whatever its own alpha says. */
        effect = 1;
        if (f1 & 0x40) {
            eva = f1 & 0x1F;
            evb = 16 - eva;
        } else {
            eva = e->eva;
            evb = e->evb;
        }
    } else if ((f1 & 0x40) && (bld & target2)) {
        effect = 4; /* the 3D layer carries its own alpha */
    } else {
        if (f1 & 0x80) f1 = 0x10;
        else if (f1 & 0x40) f1 = 0x01;
        if ((bld & f1) && (lb->winmask[i] & 0x20)) {
            effect = (bld >> 6) & 3;
            if (effect == 1) {
                if (bld & target2) {
                    eva = e->eva;
                    evb = e->evb;
                } else {
                    effect = 0;
                }
            }
        }
    }

    switch (effect) {
    case 1: return ds10_blend4(v1, v2, eva, evb);
    case 2: return ds10_bright_up(v1, e->evy, 8);
    case 3: return ds10_bright_down(v1, e->evy, 7);
    case 4: return ds10_blend5(v1, v2);
    default: return v1;
    }
}

void ds10_2d_scanline(ds10_2d *e, const ds10_2dmem *mem, const uint8_t *oam,
                      int num, int line, const uint32_t *plane3d,
                      uint32_t *out)
{
    static linebuf lb;
    uint32_t backdrop;
    int prio, bg, i;

    backdrop = ds10_col15to18(rd16(mem->pal, 0)) | F_BD;
    for (i = 0; i < DS10_SCR_W; i++) {
        lb.line[i] = backdrop;
        lb.line[i + DS10_SCR_W] = 0;
    }
    draw_sprites(e, mem, oam, line, &lb);
    window_mask(e, line, &lb);

    if ((e->dispcnt & 7) != 0) {
        ds10_2d_unsupported = "BG mode other than 0";
        for (i = 0; i < DS10_SCR_W; i++) out[i] = backdrop | 0xFF000000u;
        return;
    }

    /* Lowest priority first, and within a priority the higher-numbered
     * background loses, which is what iterating downward gives. */
    for (prio = 3; prio >= 0; prio--) {
        for (bg = 3; bg >= 0; bg--) {
            if ((e->bgcnt[bg] & 3) != (uint32_t)prio) continue;
            if (!(e->dispcnt & (0x100 << bg))) continue;
            if (bg == 0 && num == 0 && (e->dispcnt & 8)) {
                if (plane3d) draw_bg_3d(plane3d, &lb);
                continue;
            }
            draw_bg_text(e, mem, num, bg, line, &lb);
        }
        interleave_sprites(mem, (uint32_t)prio, &lb);
    }

    for (i = 0; i < DS10_SCR_W; i++) out[i] = composite(e, &lb, i);
}

void ds10_gpu_frame(ds10_gpu *g, uint32_t *top, uint32_t *bottom)
{
    ds10_2dmem mem[2];
    uint32_t *dst[2];
    int line, n, i;

    ds10_vram_flatten(&g->vram);
    ds10_2dmem_bind(&mem[0], &g->vram, g->palette, 0);
    ds10_2dmem_bind(&mem[1], &g->vram, g->palette, 1);

    /* POWCNT1 bit 15 sends engine A to the top screen instead of the
     * bottom. */
    if (g->powcnt & (1 << 15)) { dst[0] = top; dst[1] = bottom; }
    else { dst[0] = bottom; dst[1] = top; }

    for (n = 0; n < 2; n++) {
        g->eng[n].win0act = 0;
        g->eng[n].win1act = 0;
    }

    for (line = 0; line < DS10_SCR_H; line++) {
        for (n = 0; n < 2; n++) {
            ds10_2d *e = &g->eng[n];
            uint32_t *out = dst[n] + line * DS10_SCR_W;
            int enabled = g->powcnt & (n ? (1 << 9) : (1 << 1));
            int mode = (int)((e->dispcnt >> 16) & (n ? 1 : 3));
            uint16_t bright = g->brightness[n];
            uint32_t factor = bright & 0x1F;

            if (!enabled) {
                for (i = 0; i < DS10_SCR_W; i++)
                    out[i] = n ? 0x3F3F3F : 0x000000;
            } else if (e->dispcnt & (1u << 7)) {
                for (i = 0; i < DS10_SCR_W; i++) out[i] = 0x3F3F3F;
            } else if (mode == 0) {
                for (i = 0; i < DS10_SCR_W; i++) out[i] = 0x3F3F3F;
            } else if (mode == 1) {
                ds10_2d_scanline(e, &mem[n], g->oam + (n ? 0x400 : 0), n,
                                 line,
                                 n == 0 ? &g->plane3d[line * DS10_SCR_W]
                                        : NULL, out);
            } else {
                ds10_2d_unsupported = "display mode other than 0 or 1";
                for (i = 0; i < DS10_SCR_W; i++) out[i] = 0x3F3F3F;
            }

            /* The rounding bias differs between the two directions, and it
             * is not the same bias the per-pixel colour effect uses. */
            if (factor > 16) factor = 16;
            if ((bright >> 14) == 1)
                for (i = 0; i < DS10_SCR_W; i++)
                    out[i] = ds10_bright_up(out[i], factor, 0x0);
            else if ((bright >> 14) == 2)
                for (i = 0; i < DS10_SCR_W; i++)
                    out[i] = ds10_bright_down(out[i], factor, 0xF);

            if (g->powcnt & 1)
                for (i = 0; i < DS10_SCR_W; i++)
                    out[i] = ds10_expand(out[i]);
            else
                for (i = 0; i < DS10_SCR_W; i++) out[i] = 0xFF000000u;
        }
    }
}
