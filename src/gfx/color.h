/* The DS's colour arithmetic.
 *
 * Everything between the palette and the screen is done at 6 bits per channel,
 * one channel per byte of a u32: red in bits 0-5, green in 8-13, blue in 16-21.
 * The top byte carries layer flags while a pixel is still in the compositor.
 *
 * A palette entry is 5 bits per channel, so red and blue are shifted up by one
 * and green takes bit 15 of the entry as its low bit - that bit is how the 3D
 * engine and direct-colour sprites get a sixth green bit.
 */
#ifndef DS10_GFX_COLOR_H
#define DS10_GFX_COLOR_H

#include <stdint.h>

static inline uint32_t ds10_col15to18(uint16_t c)
{
    uint32_t r = (c & 0x001F) << 1;
    uint32_t g = ((c & 0x03E0) >> 4) | ((c & 0x8000) >> 15);
    uint32_t b = (c & 0x7C00) >> 9;

    return r | (g << 8) | (b << 16);
}

/* EVA/EVB alpha blend, 4-bit weights over 16. */
static inline uint32_t ds10_blend4(uint32_t a, uint32_t b, uint32_t eva,
                                   uint32_t evb)
{
    uint32_t r = (((a & 0x00003F) * eva) + ((b & 0x00003F) * evb) + 0x8) >> 4;
    uint32_t g = ((((a & 0x003F00) * eva) + ((b & 0x003F00) * evb) +
                   0x000800) >> 4) & 0x007F00;
    uint32_t bl = ((((a & 0x3F0000) * eva) + ((b & 0x3F0000) * evb) +
                    0x080000) >> 4) & 0x7F0000;

    if (r > 0x00003F) r = 0x00003F;
    if (g > 0x003F00) g = 0x003F00;
    if (bl > 0x3F0000) bl = 0x3F0000;
    return r | g | bl | 0xFF000000u;
}

/* The 3D layer's own alpha, 5 bits over 32. */
static inline uint32_t ds10_blend5(uint32_t a, uint32_t b)
{
    uint32_t eva = ((a >> 24) & 0x1F) + 1;
    uint32_t evb = 32 - eva;
    uint32_t r, g, bl;

    if (eva == 32) return a;
    r = (((a & 0x00003F) * eva) + ((b & 0x00003F) * evb) + 0x10) >> 5;
    g = ((((a & 0x003F00) * eva) + ((b & 0x003F00) * evb) +
          0x001000) >> 5) & 0x007F00;
    bl = ((((a & 0x3F0000) * eva) + ((b & 0x3F0000) * evb) +
           0x100000) >> 5) & 0x7F0000;
    if (r > 0x00003F) r = 0x00003F;
    if (g > 0x003F00) g = 0x003F00;
    if (bl > 0x3F0000) bl = 0x3F0000;
    return r | g | bl | 0xFF000000u;
}

static inline uint32_t ds10_bright_up(uint32_t v, uint32_t factor,
                                      uint32_t bias)
{
    uint32_t rb = v & 0x3F003F;
    uint32_t g = v & 0x003F00;

    rb += ((((0x3F003F - rb) * factor) + (bias * 0x010001)) >> 4) & 0x3F003F;
    g += ((((0x003F00 - g) * factor) + (bias * 0x000100)) >> 4) & 0x003F00;
    return rb | g | 0xFF000000u;
}

static inline uint32_t ds10_bright_down(uint32_t v, uint32_t factor,
                                        uint32_t bias)
{
    uint32_t rb = v & 0x3F003F;
    uint32_t g = v & 0x003F00;

    rb -= (((rb * factor) + (bias * 0x010001)) >> 4) & 0x3F003F;
    g -= (((g * factor) + (bias * 0x000100)) >> 4) & 0x003F00;
    return rb | g | 0xFF000000u;
}

/* 6 bits per channel to 8, replicating the top two bits into the bottom, and
 * out as 0xAARRGGBB. */
static inline uint32_t ds10_expand(uint32_t c)
{
    uint32_t r = (c << 18) & 0xFC0000;
    uint32_t g = (c << 2) & 0xFC00;
    uint32_t b = (c >> 14) & 0xFC;

    c = r | g | b;
    return c | ((c & 0x00C0C0C0) >> 6) | 0xFF000000u;
}

#endif /* DS10_GFX_COLOR_H */
