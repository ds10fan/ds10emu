/* Readers for the NITRO asset containers. See nitro.h. */
#include <string.h>

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

const uint8_t *ds10_nitro_block(const uint8_t *blob, size_t len,
                                const char *name, size_t *out_len)
{
    uint32_t hdrlen, nblocks, o;
    uint32_t i;

    if (len < 16) return NULL;
    hdrlen = rd16(blob + 12);
    nblocks = rd16(blob + 14);
    o = hdrlen;
    for (i = 0; i < nblocks; i++) {
        uint32_t blen;
        char magic[4];

        if ((size_t)o + 8 > len) return NULL;
        /* On disk the magic is reversed. */
        magic[0] = (char)blob[o + 3];
        magic[1] = (char)blob[o + 2];
        magic[2] = (char)blob[o + 1];
        magic[3] = (char)blob[o + 0];
        blen = rd32(blob + o + 4);
        if (blen < 8 || (size_t)o + blen > len) return NULL;
        if (memcmp(magic, name, 4) == 0) {
            if (out_len) *out_len = blen - 8;
            return blob + o + 8;
        }
        o += blen;
    }
    return NULL;
}

int ds10_ncgr_parse(ds10_ncgr *g, const uint8_t *blob, size_t len)
{
    size_t clen;
    const uint8_t *c = ds10_nitro_block(blob, len, "CHAR", &clen);
    uint32_t datalen, dataoff;

    if (!c || clen < 0x18) return -1;
    g->height = rd16(c + 0);
    g->width = rd16(c + 2);
    if (g->width == 0xFFFF) g->width = g->height = -1;
    g->bpp = rd32(c + 4) == 4 ? 8 : 4;
    g->mapping = rd32(c + 8);
    g->linear = (int)rd32(c + 12);
    datalen = rd32(c + 16);
    dataoff = rd32(c + 20);
    if (dataoff > clen || datalen > clen - dataoff) return -1;
    g->data = c + dataoff;
    g->len = datalen;
    return 0;
}

int ds10_nclr_parse(ds10_nclr *p, const uint8_t *blob, size_t len)
{
    size_t plen;
    const uint8_t *b = ds10_nitro_block(blob, len, "PLTT", &plen);
    uint32_t datalen, dataoff;

    if (!b || plen < 0x10) return -1;
    p->fmt = (int)rd32(b + 0);
    p->extended = (int)rd32(b + 4);
    datalen = rd32(b + 8);
    dataoff = rd32(b + 12);
    /* The offset counts from the start of these six words, not from the data.
     * The first two words are before it, hence the 8. */
    if ((size_t)dataoff + 8 > plen) return -1;
    if (datalen > plen - dataoff - 8) datalen = (uint32_t)(plen - dataoff - 8);
    p->data = b + 8 + dataoff;
    p->ncolors = (int)(datalen / 2);
    return 0;
}

int ds10_nscr_parse(ds10_nscr *s, const uint8_t *blob, size_t len)
{
    size_t slen;
    const uint8_t *b = ds10_nitro_block(blob, len, "SCRN", &slen);
    uint32_t datalen;

    if (!b || slen < 12) return -1;
    s->width = rd16(b + 0);
    s->height = rd16(b + 2);
    s->mode = (int)rd32(b + 4);
    datalen = rd32(b + 8);
    if (datalen > slen - 12) datalen = (uint32_t)(slen - 12);
    s->data = b + 12;
    s->ncells = (int)(datalen / 2);
    return 0;
}

int ds10_ncer_parse(ds10_ncer *c, const uint8_t *blob, size_t len)
{
    size_t blen;
    const uint8_t *b = ds10_nitro_block(blob, len, "CEBK", &blen);
    uint32_t cellsoff;

    if (!b || blen < 0x18) return -1;
    c->ncells = rd16(b + 0);
    c->with_bounds = rd16(b + 2) == 1;
    cellsoff = rd32(b + 4);
    c->mapping = rd32(b + 8);
    if (cellsoff > blen) return -1;
    c->cells = b + cellsoff;
    c->objs = c->cells + (size_t)c->ncells * (c->with_bounds ? 16 : 8);
    c->labels = ds10_nitro_block(blob, len, "LABL", &c->labels_len);
    return 0;
}

int ds10_ncer_cell(const ds10_ncer *c, int n, ds10_cell *out)
{
    const uint8_t *e;
    int stride = c->with_bounds ? 16 : 8;

    if (n < 0 || n >= c->ncells) return -1;
    e = c->cells + (size_t)n * stride;
    out->nobj = rd16(e + 0);
    out->attr = rd16(e + 2);
    out->obj = c->objs + rd32(e + 4);
    if (c->with_bounds) {
        out->bounds[0] = (int16_t)rd16(e + 8);
        out->bounds[1] = (int16_t)rd16(e + 10);
        out->bounds[2] = (int16_t)rd16(e + 12);
        out->bounds[3] = (int16_t)rd16(e + 14);
    } else {
        memset(out->bounds, 0, sizeof out->bounds);
    }
    return 0;
}

const char *ds10_ncer_label(const ds10_ncer *c, int n)
{
    uint32_t off;
    size_t base;

    if (!c->labels || n < 0 || n >= c->ncells) return NULL;
    /* LABL is a table of offsets followed by the NUL-terminated names those
     * offsets point into, counted from the end of the table. */
    base = (size_t)c->ncells * 4;
    if (base + 4 > c->labels_len) return NULL;
    off = rd32(c->labels + (size_t)n * 4);
    if (base + off >= c->labels_len) return NULL;
    return (const char *)(c->labels + base + off);
}
