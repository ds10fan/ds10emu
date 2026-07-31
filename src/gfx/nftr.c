/* See nftr.h. */
#include <string.h>

#include "nftr.h"
#include "nitro.h"

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int ds10_nftr_parse(ds10_nftr *f, const uint8_t *blob, size_t len)
{
    size_t flen, clen;
    const uint8_t *finf = ds10_nitro_block(blob, len, "FINF", &flen);
    const uint8_t *cglp;
    uint32_t pcglp;

    if (!finf || flen < 20) return -1;
    memset(f, 0, sizeof *f);
    f->blob = blob;
    f->len = len;
    f->line_feed = finf[1];
    pcglp = rd32(finf + 8);
    f->cwdh = rd32(finf + 12);
    f->cmap = rd32(finf + 16);
    /* The FINF pointers are offsets to a block's data, not to its header. */
    if (pcglp + 8 > len) return -1;
    cglp = blob + pcglp;
    f->cell_w = cglp[0];
    f->cell_h = cglp[1];
    f->cell_size = rd16(cglp + 2);
    f->baseline = cglp[4];
    f->max_width = cglp[5];
    f->bpp = cglp[6];
    if (f->cell_size <= 0 || f->cell_w <= 0 || f->cell_h <= 0) return -1;

    /* The glyphs run to the end of the CGLP block. */
    if (!ds10_nitro_block(blob, len, "CGLP", &clen)) return -1;
    f->glyph = cglp + 8;
    if (clen < 8) return -1;
    f->nglyphs = (int)((clen - 8) / (size_t)f->cell_size);
    return 0;
}

int ds10_nftr_index(const ds10_nftr *f, int ch)
{
    uint32_t p = f->cmap;

    while (p != 0 && p + 12 <= f->len) {
        const uint8_t *m = f->blob + p;
        int lo = rd16(m), hi = rd16(m + 2);
        int method = rd16(m + 4);
        uint32_t next = rd32(m + 8);

        if (ch >= lo && ch <= hi) {
            switch (method) {
            case 0: /* a run of consecutive glyphs */
                return rd16(m + 12) + (ch - lo);
            case 1: { /* one entry per character in the range */
                int i = ch - lo;
                uint32_t off = p + 12 + (uint32_t)i * 2;

                if (off + 2 > f->len) return -1;
                {
                    int g = rd16(f->blob + off);

                    return g == 0xFFFF ? -1 : g;
                }
            }
            case 2: { /* a sorted list of pairs */
                int n = rd16(m + 12), i;

                for (i = 0; i < n; i++) {
                    uint32_t off = p + 14 + (uint32_t)i * 4;

                    if (off + 4 > f->len) break;
                    if (rd16(f->blob + off) == (uint16_t)ch)
                        return rd16(f->blob + off + 2);
                }
                break;
            }
            default:
                break;
            }
        }
        p = next;
    }
    return -1;
}

int ds10_nftr_advance(const ds10_nftr *f, int glyph, int *left, int *width)
{
    uint32_t p = f->cwdh;

    while (p != 0 && p + 8 <= f->len) {
        const uint8_t *w = f->blob + p;
        int lo = rd16(w), hi = rd16(w + 2);
        uint32_t next = rd32(w + 4);

        if (glyph >= lo && glyph <= hi) {
            uint32_t off = p + 8 + (uint32_t)(glyph - lo) * 3;

            if (off + 3 > f->len) break;
            if (left) *left = (int8_t)f->blob[off];
            if (width) *width = f->blob[off + 1];
            return (int8_t)f->blob[off + 2];
        }
        p = next;
    }
    if (left) *left = 0;
    if (width) *width = f->max_width;
    return f->max_width;
}

void ds10_nftr_glyph(const ds10_nftr *f, int glyph, uint8_t *dst, int stride)
{
    const uint8_t *g;
    int x, y, bit = 0;

    if (glyph < 0 || glyph >= f->nglyphs) return;
    g = f->glyph + (size_t)glyph * f->cell_size;
    /* Rows are packed end to end, most significant bit first, with no
     * padding between them - which is why a 10 by 10 cell at one bit takes
     * thirteen bytes and not twenty. */
    for (y = 0; y < f->cell_h; y++)
        for (x = 0; x < f->cell_w; x++, bit += f->bpp) {
            int v;

            if (f->bpp == 1) {
                v = (g[bit >> 3] >> (7 - (bit & 7))) & 1;
            } else {
                int shift = 8 - f->bpp - (bit & 7);

                v = (g[bit >> 3] >> shift) & ((1 << f->bpp) - 1);
            }
            dst[y * stride + x] = (uint8_t)v;
        }
}

int ds10_nftr_width(const ds10_nftr *f, const char *s)
{
    int w = 0;

    for (; *s; s++) {
        int g = ds10_nftr_index(f, (unsigned char)*s);

        if (g < 0) continue;
        w += ds10_nftr_advance(f, g, NULL, NULL);
    }
    return w;
}

int ds10_nftr_text(const ds10_nftr *f, const char *s, uint8_t *dst, int stride,
                   int height)
{
    uint8_t cell[64 * 64];
    int pen = 0;

    if (f->cell_w > 64 || f->cell_h > 64) return 0;
    for (; *s; s++) {
        int g = ds10_nftr_index(f, (unsigned char)*s);
        int left = 0, width = 0, adv, x, y;

        if (g < 0) continue;
        adv = ds10_nftr_advance(f, g, &left, &width);
        memset(cell, 0, sizeof cell);
        ds10_nftr_glyph(f, g, cell, 64);
        for (y = 0; y < f->cell_h && y < height; y++)
            for (x = 0; x < f->cell_w; x++) {
                int px = pen + left + x;

                if (px < 0 || px >= stride) continue;
                if (cell[y * 64 + x]) dst[y * stride + px] = cell[y * 64 + x];
            }
        pen += adv;
    }
    return pen;
}
