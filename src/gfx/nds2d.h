/* The Nintendo DS 2D graphics hardware, as a model.
 *
 * DS-10's interface is drawn by this hardware: tiled backgrounds for the panel
 * art, sprites for everything that moves, and the 3D engine as one more
 * background layer. Reimplementing the UI means writing the same registers,
 * VRAM and OAM the ARM9 does, so the hardware has to be here too.
 *
 * The model is deliberately shaped like melonDS's software renderer, because
 * melonDS is the reference these are compared against (`make test-2d`), and
 * the DS has enough undocumented corners that agreeing with a known-good
 * implementation is worth more than agreeing with a reading of the docs.
 *
 * Two things are represented separately:
 *
 *   ds10_vram   the nine banks and the VRAMCNT mapping, which flattens to
 *   ds10_2dmem  the linear images a rendering engine actually reads;
 *   ds10_2d     one 2D engine's registers.
 */
#ifndef DS10_GFX_NDS2D_H
#define DS10_GFX_NDS2D_H

#include <stdint.h>

#define DS10_SCR_W 256
#define DS10_SCR_H 192

/* One 2D engine's register file. Field names follow GBATEK. */
typedef struct {
    uint32_t dispcnt;
    uint16_t bgcnt[4];
    uint16_t bgx[4], bgy[4];      /* BGnHOFS / BGnVOFS */
    int32_t bgxref[2], bgyref[2]; /* BG2/BG3 affine reference point */
    int16_t rota[2], rotb[2], rotc[2], rotd[2];
    uint8_t win0[4], win1[4];     /* x1, x2, y1, y2 */
    uint8_t wincnt[4];            /* win0, win1, outside, obj */
    uint8_t bgmosaic[2], objmosaic[2];
    uint16_t blendcnt;
    uint8_t eva, evb, evy;

    /* Not registers: what the hardware carries across a scanline. */
    uint8_t win0act, win1act;
    int32_t bgxint[2], bgyint[2];
    int32_t mosaic_line, objmosaic_line;
} ds10_2d;

/* The linear images one engine reads: backgrounds, sprites, extended palettes,
 * and its half of the standard palette. Sizes are the hardware's. */
typedef struct {
    const uint8_t *bg;
    uint32_t bgmask;
    const uint8_t *obj;
    uint32_t objmask;
    const uint8_t *bgextpal;  /* 32 KB: 4 slots of 16 palettes of 256 */
    const uint8_t *objextpal; /* 8 KB */
    const uint8_t *pal;       /* 1 KB: 512 entries, BG then OBJ */
} ds10_2dmem;

/* The nine banks plus the mapping registers. */
typedef struct {
    uint8_t a[128 * 1024], b[128 * 1024], c[128 * 1024], d[128 * 1024];
    uint8_t e[64 * 1024], f[16 * 1024], g[16 * 1024];
    uint8_t h[32 * 1024], i[16 * 1024];
    uint8_t vramcnt[9];

    /* Flattened by ds10_vram_flatten(). */
    uint8_t abg[512 * 1024], bbg[128 * 1024];
    uint8_t aobj[256 * 1024], bobj[128 * 1024];
    uint8_t abgextpal[32 * 1024], bbgextpal[32 * 1024];
    uint8_t aobjextpal[8 * 1024], bobjextpal[8 * 1024];
} ds10_vram;

/* Everything the display controller needs for one frame. */
typedef struct {
    ds10_vram vram;
    uint8_t palette[2048];
    uint8_t oam[2048];
    ds10_2d eng[2];
    uint16_t powcnt;        /* POWCNT1: bit 15 swaps the screens */
    uint16_t brightness[2]; /* MASTER_BRIGHT for each engine */
    /* The 3D engine's output, as BG0 of engine A when DISPCNT bit 3 is set.
     * 0xAARRGGBB-ish: 6-bit channels, alpha 0 meaning transparent. */
    uint32_t plane3d[DS10_SCR_W * DS10_SCR_H];
} ds10_gpu;

/* Composite the nine banks into the linear images, per VRAMCNT. */
void ds10_vram_flatten(ds10_vram *v);

/* Point mem at the flattened images belonging to engine `num`. */
void ds10_2dmem_bind(ds10_2dmem *mem, const ds10_vram *v,
                     const uint8_t *palette, int num);

/* One scanline of one engine, into 256 words of 6-bit-per-channel colour.
 * `plane3d` is that line of the 3D output, or NULL when there is none. */
void ds10_2d_scanline(ds10_2d *e, const ds10_2dmem *mem, const uint8_t *oam,
                      int num, int line, const uint32_t *plane3d,
                      uint32_t *out);

/* A whole frame, through master brightness and the screen swap, into two
 * 256x192 buffers of 0xAARRGGBB. */
void ds10_gpu_frame(ds10_gpu *g, uint32_t *top, uint32_t *bottom);

/* Which BG modes the model implements. Anything else renders as a backdrop and
 * sets this, so a screen that needs more is loud rather than subtly wrong. */
extern const char *ds10_2d_unsupported;

#endif /* DS10_GFX_NDS2D_H */
