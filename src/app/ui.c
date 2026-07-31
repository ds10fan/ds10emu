/* See ui.h.
 *
 * The VRAM layout below is the game's, established by looking for each asset's
 * bytes in the VRAM ndsref captured and by decoding the BGCNT the game had
 * set at the time. Every page in /data/mainbg lands at the same addresses:
 *
 *   engine A (touch screen)   BG1  tiles 0x04000  map 0x0800   the page
 *                             BG2  tiles 0x10000  map 0x1000   S_bg backdrop
 *                             BG0  the 3D layer
 *   engine B (top screen)     BG0  tiles 0x04000  map 0x0800   the page
 *                             BG1  tiles 0x10000  map 0x1000   S_bg backdrop
 *
 * Bank B is BG-A, bank C is BG-B, E is OBJ-A, I is OBJ-B, G and H are the
 * extended palettes, A and F belong to the 3D engine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "../gfx/nitro.h"

#define PAGE_TILES 0x04000
#define PAGE_MAP 0x00800
#define BACK_TILES 0x10000
#define BACK_MAP 0x01000

/* PALETTE.NCLR holds 252 colours and the game loads them at entry 4, leaving
 * the first four to the backdrop and whatever else wants them. */
#define PAL_OFFSET 8

struct ds10_ui {
    char root[256];
    char err[600];
    ds10_gpu gpu;
    uint8_t *blob[8]; /* whatever is currently held open */
    int nblob;
};

static uint8_t *slurp(ds10_ui *u, const char *dir, const char *name,
                      const char *ext, size_t *len)
{
    char path[512];
    FILE *f;
    long n;
    uint8_t *buf;

    snprintf(path, sizeof path, "%s/%s/%s%s", u->root, dir, name, ext);
    f = fopen(path, "rb");
    if (!f) {
        snprintf(u->err, sizeof u->err, "cannot open %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        snprintf(u->err, sizeof u->err, "short read on %s", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (len) *len = (size_t)n;
    if (u->nblob < (int)(sizeof u->blob / sizeof u->blob[0]))
        u->blob[u->nblob++] = buf;
    return buf;
}

static void drop_blobs(ds10_ui *u)
{
    int i;

    for (i = 0; i < u->nblob; i++) free(u->blob[i]);
    u->nblob = 0;
}

/* Copy an asset's payload into a VRAM bank at the address the game uses. */
static void put(uint8_t *bank, size_t banklen, size_t off, const uint8_t *src,
                size_t len)
{
    if (off >= banklen) return;
    if (len > banklen - off) len = banklen - off;
    memcpy(bank + off, src, len);
}

static int load_page(ds10_ui *u, const char *name, int engine)
{
    uint8_t *gblob, *sblob;
    size_t glen, slen;
    ds10_ncgr g;
    ds10_nscr s;
    uint8_t *bank = engine ? u->gpu.vram.c : u->gpu.vram.b;
    size_t banklen = engine ? sizeof u->gpu.vram.c : sizeof u->gpu.vram.b;

    gblob = slurp(u, "mainbg", name, ".NCGR", &glen);
    if (!gblob) return -1;
    sblob = slurp(u, "mainbg", name, ".NSCR", &slen);
    if (!sblob) return -1;
    if (ds10_ncgr_parse(&g, gblob, glen) < 0) {
        snprintf(u->err, sizeof u->err, "%s.NCGR: bad CHAR block", name);
        return -1;
    }
    if (ds10_nscr_parse(&s, sblob, slen) < 0) {
        snprintf(u->err, sizeof u->err, "%s.NSCR: bad SCRN block", name);
        return -1;
    }
    put(bank, banklen, PAGE_TILES, g.data, g.len);
    put(bank, banklen, PAGE_MAP, s.data, (size_t)s.ncells * 2);
    return 0;
}

static int load_shared(ds10_ui *u)
{
    uint8_t *blob;
    size_t len;
    ds10_nclr pal;
    ds10_ncgr g;
    ds10_nscr s;
    int e;

    /* The one background palette both engines use. */
    blob = slurp(u, "mainbg", "PALETTE", ".NCLR", &len);
    if (!blob) return -1;
    if (ds10_nclr_parse(&pal, blob, len) < 0) {
        snprintf(u->err, sizeof u->err, "PALETTE.NCLR: bad PLTT block");
        return -1;
    }
    memcpy(u->gpu.palette + PAL_OFFSET, pal.data, (size_t)pal.ncolors * 2);
    memcpy(u->gpu.palette + 0x400 + PAL_OFFSET, pal.data,
           (size_t)pal.ncolors * 2);

    /* The page backgrounds are 256-colour with extended palettes enabled, so
     * what they actually read is palette 0 of an extended slot, not palette
     * RAM. Bank G holds slots 2 and 3 for engine A and bank H slots 0..3 for
     * engine B; the game writes the same 252 colours into each 8 KB slot at
     * the same offset, so BG1 (slot 3) and BG2 (slot 2) agree. */
    for (e = 0; e < 2; e++) {
        put(u->gpu.vram.g, sizeof u->gpu.vram.g, (size_t)e * 0x2000 +
            PAL_OFFSET, pal.data, (size_t)pal.ncolors * 2);
        put(u->gpu.vram.h, sizeof u->gpu.vram.h, (size_t)e * 0x2000 +
            PAL_OFFSET, pal.data, (size_t)pal.ncolors * 2);
    }

    /* And the sprite palette, which is the same colours in the OBJ half. */
    blob = slurp(u, "mainobj", "CHARACTER_c", ".NCLR", &len);
    if (blob && ds10_nclr_parse(&pal, blob, len) == 0) {
        memcpy(u->gpu.palette + 0x200 + PAL_OFFSET, pal.data,
               (size_t)pal.ncolors * 2);
        memcpy(u->gpu.palette + 0x600 + PAL_OFFSET, pal.data,
               (size_t)pal.ncolors * 2);
    }

    /* S_bg is the panel texture behind every page, on both screens. */
    blob = slurp(u, "mainbg", "S_bg", ".NCGR", &len);
    if (!blob) return -1;
    if (ds10_ncgr_parse(&g, blob, len) < 0) {
        snprintf(u->err, sizeof u->err, "S_bg.NCGR: bad CHAR block");
        return -1;
    }
    for (e = 0; e < 2; e++) {
        uint8_t *bank = e ? u->gpu.vram.c : u->gpu.vram.b;
        size_t banklen = e ? sizeof u->gpu.vram.c : sizeof u->gpu.vram.b;

        put(bank, banklen, BACK_TILES, g.data, g.len);
    }
    blob = slurp(u, "mainbg", "S_bg", ".NSCR", &len);
    if (blob && ds10_nscr_parse(&s, blob, len) == 0) {
        for (e = 0; e < 2; e++) {
            uint8_t *bank = e ? u->gpu.vram.c : u->gpu.vram.b;
            size_t banklen = e ? sizeof u->gpu.vram.c : sizeof u->gpu.vram.b;

            put(bank, banklen, BACK_MAP, s.data, (size_t)s.ncells * 2);
        }
    }
    return 0;
}

static void set_registers(ds10_ui *u)
{
    ds10_2d *a = &u->gpu.eng[0];
    ds10_2d *b = &u->gpu.eng[1];

    memset(a, 0, sizeof *a);
    memset(b, 0, sizeof *b);

    /* Mode 0, BG0 is the 3D layer, sprites are 1D-mapped with a 64-byte
     * boundary, BG extended palettes on. */
    a->dispcnt = 0x40111318u;
    a->bgcnt[0] = 0x0001; /* the 3D layer, priority 1 */
    a->bgcnt[1] = 0x2186; /* page:  256 colour, tiles 0x4000, map 0x800 */
    a->bgcnt[2] = 0xc293; /* S_bg:  512x512, tiles 0x10000, map 0x1000 */
    a->blendcnt = 0x0292;

    b->dispcnt = 0x40011310u;
    b->bgcnt[0] = 0x0186;
    b->bgcnt[1] = 0xc293;
    b->blendcnt = 0x0191;

    /* B is BG-A, C is BG-B, E is OBJ-A, I is OBJ-B, G and H the extended
     * palettes; A and F belong to the 3D engine. */
    u->gpu.vram.vramcnt[0] = 0x83;
    u->gpu.vram.vramcnt[1] = 0x81;
    u->gpu.vram.vramcnt[2] = 0x84;
    u->gpu.vram.vramcnt[3] = 0x00;
    u->gpu.vram.vramcnt[4] = 0x82;
    u->gpu.vram.vramcnt[5] = 0x83;
    u->gpu.vram.vramcnt[6] = 0x8c;
    u->gpu.vram.vramcnt[7] = 0x82;
    u->gpu.vram.vramcnt[8] = 0x82;

    /* Both engines on, screens on, no swap. */
    u->gpu.powcnt = 0x020f;
    u->gpu.brightness[0] = 0;
    u->gpu.brightness[1] = 0;
}

ds10_ui *ds10_ui_new(const char *root)
{
    ds10_ui *u = calloc(1, sizeof *u);

    if (!u) return NULL;
    snprintf(u->root, sizeof u->root, "%s", root);
    set_registers(u);
    if (load_shared(u) < 0) {
        fprintf(stderr, "ds10_ui_new: %s\n", u->err);
        drop_blobs(u);
        free(u);
        return NULL;
    }
    drop_blobs(u);
    return u;
}

void ds10_ui_free(ds10_ui *u)
{
    if (!u) return;
    drop_blobs(u);
    free(u);
}

int ds10_ui_page(ds10_ui *u, const char *bottom, const char *top)
{
    int rc = 0;

    /* Engine A drives the touch screen when POWCNT's swap bit is clear. */
    if (bottom) rc |= load_page(u, bottom, 0);
    if (top) rc |= load_page(u, top, 1);
    drop_blobs(u);
    return rc;
}

void ds10_ui_frame(ds10_ui *u, uint32_t *top, uint32_t *bottom)
{
    ds10_gpu_frame(&u->gpu, top, bottom);
}

ds10_gpu *ds10_ui_gpu(ds10_ui *u)
{
    return &u->gpu;
}

const char *ds10_ui_error(const ds10_ui *u)
{
    return u->err;
}
