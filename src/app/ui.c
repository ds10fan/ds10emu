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

/* Sprite graphics. DS10FONT and CHARACTER_c share OBJ-A; CHARACTER_S is the
 * top screen's. A widget's tile number is its cell's plus 0x200, which is
 * CHARACTER_c's base in 64-byte units - the same +0x200 the game's own OAM
 * builder adds. */
#define OBJ_FONT 0x0000
#define OBJ_CHARS 0x8000

/* One page's sprite layer, as the game had it. See tools/pages.py: these are
 * captured rather than generated, until the widget constructors are read. */
typedef struct {
    char name[24];
    uint8_t engine;
    uint8_t oam[1024];
} page_oam;

struct ds10_ui {
    char root[256];
    char err[600];
    ds10_gpu gpu;
    uint8_t *blob[8]; /* whatever is currently held open */
    int nblob;
    page_oam *pages;
    int npages;
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
    if (!sblob) {
        /* The main menu's two files disagree on how to spell it: the tiles are
         * 01_MAIN_MENU and the tilemap is 01_MEIN_MENU. */
        char alt[64];
        char *p;

        snprintf(alt, sizeof alt, "%s", name);
        p = strstr(alt, "MAIN");
        if (p) memcpy(p, "MEIN", 4);
        else if ((p = strstr(alt, "MEIN")) != NULL) memcpy(p, "MAIN", 4);
        if (p) sblob = slurp(u, "mainbg", alt, ".NSCR", &slen);
        if (!sblob) return -1;
    }
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

/* data/pages.bin, if it is there. Without it the pages simply draw with no
 * sprite layer, which is how this looked before the file existed. */
static void load_pages(ds10_ui *u)
{
    char path[512];
    FILE *f;
    uint32_t version, count, i;
    unsigned char hdr[12];

    snprintf(path, sizeof path, "%s/../pages.bin", u->root);
    f = fopen(path, "rb");
    if (!f) return;
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "PAGE", 4) != 0) {
        fclose(f);
        return;
    }
    version = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
              ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    count = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
            ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    if (version != 1 || count == 0 || count > 512) {
        fclose(f);
        return;
    }
    u->pages = calloc(count, sizeof *u->pages);
    if (!u->pages) {
        fclose(f);
        return;
    }
    for (i = 0; i < count; i++) {
        int len = fgetc(f);

        if (len <= 0 || len >= (int)sizeof u->pages[i].name) break;
        if (fread(u->pages[i].name, 1, (size_t)len, f) != (size_t)len) break;
        u->pages[i].name[len] = 0;
        u->pages[i].engine = (uint8_t)fgetc(f);
        if (fread(u->pages[i].oam, 1, 1024, f) != 1024) break;
        u->npages++;
    }
    fclose(f);
}

static void apply_page_oam(ds10_ui *u, const char *name, int engine)
{
    int i;

    memset(u->gpu.oam + (engine ? 0x400 : 0), 0, 1024);
    for (i = 0; i < u->npages; i++)
        if (strcmp(u->pages[i].name, name) == 0) {
            memcpy(u->gpu.oam + (engine ? 0x400 : 0), u->pages[i].oam, 1024);
            return;
        }
}

static int load_sprites(ds10_ui *u)
{
    static const struct { const char *name; int engine; size_t off; } BANK[] = {
        {"DS10FONT", 0, OBJ_FONT},
        {"CHARACTER_c", 0, OBJ_CHARS},
        {"CHARACTER_S", 1, 0},
    };
    uint8_t *blob;
    size_t len;
    ds10_ncgr g;
    ds10_nclr pal;
    size_t k;

    for (k = 0; k < sizeof BANK / sizeof BANK[0]; k++) {
        uint8_t *bank = BANK[k].engine ? u->gpu.vram.i : u->gpu.vram.e;
        size_t banklen = BANK[k].engine ? sizeof u->gpu.vram.i
                                        : sizeof u->gpu.vram.e;

        blob = slurp(u, "mainobj", BANK[k].name, ".NCGR", &len);
        if (!blob) return -1;
        if (ds10_ncgr_parse(&g, blob, len) < 0) {
            snprintf(u->err, sizeof u->err, "%s.NCGR: bad CHAR block",
                     BANK[k].name);
            return -1;
        }
        put(bank, banklen, BANK[k].off, g.data, g.len);
    }

    /* Each engine's sprite palette, at entry 4 of its OBJ half. */
    blob = slurp(u, "mainobj", "CHARACTER_c", ".NCLR", &len);
    if (blob && ds10_nclr_parse(&pal, blob, len) == 0)
        memcpy(u->gpu.palette + 0x200 + PAL_OFFSET, pal.data,
               (size_t)pal.ncolors * 2);
    blob = slurp(u, "mainobj", "CHARACTER_S", ".NCLR", &len);
    if (blob && ds10_nclr_parse(&pal, blob, len) == 0)
        memcpy(u->gpu.palette + 0x600 + PAL_OFFSET, pal.data,
               (size_t)pal.ncolors * 2);
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
    load_pages(u);
    if (load_shared(u) < 0 || load_sprites(u) < 0) {
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
    free(u->pages);
    free(u);
}

int ds10_ui_page(ds10_ui *u, const char *bottom, const char *top)
{
    int rc = 0;

    /* Engine A drives the touch screen when POWCNT's swap bit is clear. */
    if (bottom) {
        rc |= load_page(u, bottom, 0);
        apply_page_oam(u, bottom, 0);
    }
    if (top) {
        rc |= load_page(u, top, 1);
        apply_page_oam(u, top, 1);
    }
    drop_blobs(u);
    return rc;
}

void ds10_ui_frame(ds10_ui *u, uint32_t *top, uint32_t *bottom)
{
    ds10_gpu_frame(&u->gpu, top, bottom);
}

/* The navigation map's boxes. The coordinates are where the boxes are drawn
 * on 90_MAP; each opens the page named here. Read off the rendered screen
 * rather than out of the map's own hit-test code, which is not decompiled. */
static const struct {
    int x0, y0, x1, y1;
    const char *page;
} MAP_BOX[] = {
    {  8,  30,  76,  46, "60_SYN_SEQ_PIANO" },
    { 20,  46,  88,  62, "81_KEY_i" },
    { 20,  62,  88,  78, "71_KAOSS" },
    {104,  30, 168,  46, "61_SYN_EDIT" },
    {112,  46, 176,  62, "70_PATCH" },
    {  8,  90,  76, 106, "60_SYN_SEQ_PIANO" },
    { 20, 106,  88, 122, "81_KEY_i" },
    { 20, 122,  88, 138, "71_KAOSS" },
    {104,  90, 168, 106, "61_SYN_EDIT" },
    {112, 106, 176, 122, "70_PATCH" },
    {  8, 150,  84, 166, "62_DRUM_SEQ_GATE" },
    {200,  30, 252,  46, "30_MIXER" },
    {208,  46, 252,  62, "52_FX" },
    {200,  90, 252, 106, "01_MAIN_MENU" },
    {200, 106, 252, 122, "31_SONG" },
    {200, 122, 252, 138, "51_ALL_DRUMS" },
    {  0,   0, 120,  18, "32_PATTERN" },
};

const char *ds10_ui_map_hit(int x, int y)
{
    size_t i;

    for (i = 0; i < sizeof MAP_BOX / sizeof MAP_BOX[0]; i++)
        if (x >= MAP_BOX[i].x0 && x < MAP_BOX[i].x1 &&
            y >= MAP_BOX[i].y0 && y < MAP_BOX[i].y1)
            return MAP_BOX[i].page;
    return NULL;
}

/* The keyboard on 81_KEY: fourteen white keys of 18 pixels from x=3, and ten
 * black keys over them. Measured off the rendered page, which is pixel-exact
 * against the hardware, so the boundaries are the real ones.
 *
 * The keys run F to E over two octaves - the gaps between black keys fall
 * after the third and sixth white key, which only happens starting on F. Which
 * F is an assumption: the lowest key is taken as F3, and the OCT buttons move
 * it by twelve. That has not been checked against the device.
 */
#define KEY_TOP 28
#define KEY_BOTTOM 170
#define BLACK_BOTTOM 116
#define WHITE_X0 3
#define WHITE_W 18
#define KEY_BASE 53 /* F3 */

static const int WHITE_SEMI[14] = {
    0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23,
};

static const struct { int x0, x1, semi; } BLACK_KEY[10] = {
    { 14,  24,  1}, { 33,  43,  3}, { 52,  62,  5}, { 86,  96,  8},
    {106, 116, 10}, {140, 150, 13}, {159, 169, 15}, {178, 188, 17},
    {212, 222, 20}, {232, 242, 22},
};

int ds10_ui_key_hit(int x, int y, int octave)
{
    int i;

    if (y < KEY_TOP || y > KEY_BOTTOM) return -1;
    if (y <= BLACK_BOTTOM)
        for (i = 0; i < 10; i++)
            if (x >= BLACK_KEY[i].x0 && x <= BLACK_KEY[i].x1)
                return KEY_BASE + BLACK_KEY[i].semi + octave * 12;
    i = (x - WHITE_X0) / WHITE_W;
    if (x < WHITE_X0 || i < 0 || i >= 14) return -1;
    return KEY_BASE + WHITE_SEMI[i] + octave * 12;
}

/* 61_SYN_EDIT's knobs. Five columns at x = 22, 75, 124, 174, 225 and four
 * rows at y = 39, 80, 121, 162, confirmed by looking for the light knob cap
 * against the dark panel in the rendered page - which is pixel-exact against
 * the hardware. Which knob is at which position is read off the caption
 * printed under it. The four positions not listed carry switches rather than
 * knobs (VCO SYNC, VCA EG, filter TYPE) and are not wired.
 *
 * The names are the game's own, from the table at 0x020852dc. */
#define KNOB_R 12

static const struct { int x, y, id; } SYN_KNOB[] = {
    { 22,  39,  0}, { 22,  80,  2}, { 22, 121,  1}, { 22, 162, 19},
    { 75,  39,  6}, { 75,  80,  9},                 { 75, 162, 18},
    {124,  39,  7}, {124,  80,  8},
    {174,  39, 11}, {174,  80, 12}, {174, 121, 14},
    {225,  39, 20}, {225,  80, 21}, {225, 121, 22}, {225, 162, 23},
};

static const char *const KNOB_NAME[31] = {
    "OCTAVE", "PITCH EG INT", "PORTA", "PITCH IN INT", "VCO1 P.IN INT",
    "VCO2 P.IN INT", "VCO1 WAVE", "VCO2 WAVE", "VCO2 PITCH", "BALANCE",
    "VCO SYNC", "CUTOFF", "PEAK", "FILTER TYPE", "VCF EG INT",
    "CUTOFF IN INT", "VCA EG", "VCA IN INT", "DRIVE", "LEVEL", "ATTACK",
    "DECAY", "SUSTAIN", "RELEASE", "MG FREQ", "MG BPM", "PITCH IN",
    "VCO1 P.IN", "VCO2 P.IN", "CUTOFF IN", "VCA IN",
};

int ds10_ui_knob_hit(int x, int y)
{
    size_t i;

    for (i = 0; i < sizeof SYN_KNOB / sizeof SYN_KNOB[0]; i++) {
        int dx = x - SYN_KNOB[i].x, dy = y - SYN_KNOB[i].y;

        if (dx * dx + dy * dy <= KNOB_R * KNOB_R) return SYN_KNOB[i].id;
    }
    return -1;
}

const char *ds10_ui_knob_name(int id)
{
    if (id < 0 || id > 30) return "";
    return KNOB_NAME[id];
}

ds10_gpu *ds10_ui_gpu(ds10_ui *u)
{
    return &u->gpu;
}

const char *ds10_ui_error(const ds10_ui *u)
{
    return u->err;
}
