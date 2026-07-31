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

#include <math.h>

#include "ui.h"
#include "cell.h"
#include "obj.h"
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

/* One page's display list: which cell was drawn where, recovered from a
 * captured frame by tools/pages.py. The sprites are generated from this by
 * the decompiled expander, not replayed - so anything whose position or
 * rotation we drive can move. Entries that could not be attributed to a cell
 * are kept as raw OAM so no page is worse off than the frozen frame was. */
typedef struct {
    char bank[24];
    uint16_t cell;
    uint8_t prio, flags, affine;
    uint16_t base;      /* the tile the sheet is loaded at */
    int32_t x, y;
    int32_t mtx[4];
} page_item;

/* A record is either a cell to expand or one OAM entry nothing explained.
 * They are kept in the order the game emitted them, because sprites of equal
 * priority are resolved by OAM index. */
typedef struct {
    uint8_t kind;      /* 0 cell, 1 raw */
    page_item item;
    uint8_t entry[6];
} page_rec;

typedef struct {
    char name[24];
    uint8_t engine;
    int nrec;
    page_rec *rec;
} page_list;

/* The NCER banks a display list names. */
typedef struct {
    char name[24];
    uint8_t *blob;
    ds10_ncer ncer;
} cell_bank;

struct ds10_ui {
    char root[256];
    char err[600];
    ds10_gpu gpu;
    uint8_t *blob[8]; /* whatever is currently held open */
    int nblob;
    page_list *pages;
    int npages;
    cell_bank bank[24];
    int nbanks;
    /* The OAM slot and affine index each synth-edit knob's pointer uses,
     * or -1. The two are different: the sprite lives in one slot and its
     * rotation matrix in another, named by attr1 bits 9-13. */
    int knob_slot[16];
    int knob_affine[16];
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

    /* A page with its own palette - the title and advertisement screens have
     * one - replaces the shared colours in the slot its own layer reads,
     * rather than sitting alongside them. Engine B's page layer is BG0, which
     * takes extended slot 0; engine A's is BG1, which takes slot 3 because its
     * BGCNT has bit 13 set. It goes in at entry 4, the same place the shared
     * palette does - the slot is what differs, not the offset.
     */
    {
        uint8_t *pblob = slurp(u, "mainbg", name, ".NCLR", &glen);
        ds10_nclr pal;

        if (pblob && ds10_nclr_parse(&pal, pblob, glen) == 0) {
            uint8_t *pb = engine ? u->gpu.vram.h : u->gpu.vram.g;
            size_t pl = engine ? sizeof u->gpu.vram.h : sizeof u->gpu.vram.g;
            size_t slot = engine ? 0 : 0x2000;

            if (slot + 0x2000 <= pl) memset(pb + slot, 0, 0x2000);
            put(pb, pl, slot + PAL_OFFSET, pal.data,
                (size_t)pal.ncolors * 2);
        }
    }
    return 0;
}

/* 61_SYN_EDIT, from its constructor at 0x02047384 and the tables at
 * 0x020851d4 and 0x02085180. Sixteen rotary knobs, each with a 40 by 40 hit
 * box centred on the point, and three sliders.
 *
 * This replaces a layout measured off the rendered page. The sixteen knob ids
 * were right - they were read off the caption printed under each one - but
 * every position was out by (5,7), because the bright cap a blob detector
 * finds is not centred on the knob.
 */
#define KNOB_R 20

static const struct { int x, y, id; } SYN_KNOB[] = {
    { 27,  46,  0}, { 27,  87,  2}, { 27, 128,  1}, { 27, 169, 19},
    { 80,  46,  6}, { 80,  87,  9}, { 80, 169, 18},
    {127,  46,  7}, {127,  87,  8},
    {179,  46, 11}, {179,  87, 12}, {179, 128, 14},
    {230,  46, 20}, {230,  87, 21}, {230, 128, 22}, {230, 169, 23},
};

/* The three switches, which are sliders rather than knobs. */
static const struct { int x, y, w, h, id; } SYN_SLIDER[] = {
    {117, 111, 0x13, 0x1A, 10},  /* VCO SYNC */
    {117, 155, 0x13, 0x1A, 16},  /* VCA EG */
    {170, 155, 0x13, 0x1A, 13},  /* FILTER TYPE */
};

static const char *const KNOB_NAME[31] = {
    "OCTAVE", "PITCH EG INT", "PORTA", "PITCH IN INT", "VCO1 P.IN INT",
    "VCO2 P.IN INT", "VCO1 WAVE", "VCO2 WAVE", "VCO2 PITCH", "BALANCE",
    "VCO SYNC", "CUTOFF", "PEAK", "FILTER TYPE", "VCF EG INT",
    "CUTOFF IN INT", "VCA EG", "VCA IN INT", "DRIVE", "LEVEL", "ATTACK",
    "DECAY", "SUSTAIN", "RELEASE", "MG FREQ", "MG BPM", "PITCH IN",
    "VCO1 P.IN", "VCO2 P.IN", "CUTOFF IN", "VCA IN",
};

/* A knob pointer is one 8x16 sprite drawn double-size and rotated, with tile
 * 0x21a. That is how the captured layer draws them: OAM slot 0 of the synth
 * edit page is attr0 0x8317 - shape 2, rot/scale, double size - attr1 0x0216
 * with affine index 1, attr2 0x3a1a. The affine slots live in the same OAM,
 * four halfwords at entry*0x20 + 6, 0xe, 0x16, 0x1e.
 *
 * A double-size sprite is drawn from a box twice its size, so its top-left is
 * the centre minus the full sprite width and height.
 *
 * The sweep is the game's own, from 0x02055b98:
 *
 *     angle = 0xA000 + 0xC000 * (value - min) / (max - min)
 *
 * in sixteenths of a degree-ish units where 0x10000 is a full turn - so 225
 * degrees at the minimum, sweeping 270 degrees clockwise to 135. The 0xC000
 * comes out of a divide-by-360 the compiler turned into a multiply by
 * 0xB60B60B7; the screens all pass 270 as the sweep.
 *
 * Checked against a captured frame: all sixteen pointer matrices lie inside
 * that sweep, several sit exactly on its end stops, and OCTAVE - whose 0 is
 * the midpoint of its -2..2 range - has the identity matrix.
 */
#define PTR_SWEEP_START 225.0
#define PTR_TILE 0x21A
#define PTR_PAL 3
#define PTR_PRIO 2
#define PTR_W 8
#define PTR_H 16
#define PTR_SWEEP 270.0
#define PTR_PI 3.14159265358979323846

/* An affine slot holds four halfwords, 8.8, scattered through OAM at
 * entry*0x20 + 6, 0xe, 0x16, 0x1e - the gaps are the sprite attributes. */
static void write_affine_mtx(uint8_t *oam, int slot, const int32_t *mtx)
{
    int i;

    for (i = 0; i < 4; i++) {
        int off = slot * 0x20 + 6 + i * 8;
        int16_t v = (int16_t)(mtx[i] >> 4); /* 20.12 down to 8.8 */

        oam[off] = (uint8_t)v;
        oam[off + 1] = (uint8_t)(v >> 8);
    }
}

static void write_affine(uint8_t *oam, int slot, double deg)
{
    double c = cos(deg * PTR_PI / 180.0), s = sin(deg * PTR_PI / 180.0);
    /* The DS stores the inverse transform. */
    int32_t m[4] = {
        (int32_t)lrint(c * 4096.0), (int32_t)lrint(s * 4096.0),
        (int32_t)lrint(-s * 4096.0), (int32_t)lrint(c * 4096.0),
    };

    write_affine_mtx(oam, slot, m);
}

void ds10_ui_knob_value(ds10_ui *u, int id, int value, int lo, int range)
{
    uint8_t *oam = u->gpu.oam;
    size_t i;
    double frac, deg;

    if (range < 2) range = 2;
    /* The original divides by max - min, which is one less than the range the
     * knob descriptors carry. */
    frac = (double)(value - lo) / (double)(range - 1);
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    deg = PTR_SWEEP_START + frac * PTR_SWEEP;

    for (i = 0; i < sizeof SYN_KNOB / sizeof SYN_KNOB[0]; i++) {
        ds10_obj o;
        int slot;

        if (SYN_KNOB[i].id != id) continue;
        slot = u->knob_slot[i];
        if (slot < 0) continue;
        memset(&o, 0, sizeof o);
        o.count = 1;
        o.x = SYN_KNOB[i].x - PTR_W;
        o.y = SYN_KNOB[i].y - PTR_H;
        o.tile = PTR_TILE;
        o.palette = PTR_PAL;
        o.priority = PTR_PRIO;
        o.rot = 0x300;          /* rot/scale, double size */
        o.shape = 0x8000;       /* tall */
        o.affine = u->knob_affine[i];
        o.big = 0;
        ds10_obj_build(&o);
        memcpy(oam + slot * 8, o.oam, 6);
        write_affine(oam, u->knob_affine[i], deg);
    }
}

/* 30_MIXER, decompiled from 0x02065b04 and the two tables it walks
 * (0x02087610 and 0x020875b0):
 *
 *   six pan knobs   x = 21 + 43*ch, y = 47, hit box 28x28 centred there,
 *                   mixer knob 9 + ch
 *   six volume      x = 10 + 43*ch, y = 85, 32 wide and 0x51 + 0x10 tall,
 *                   mixer knob 17 + ch
 *   two rows of six buttons at x = 3 + 43*ch, y = 62 + 11*row, 40x10
 *
 * Those knob ids are the same ones the audio side arrived at independently -
 * the sequencer's channel volume is knob channel+17 and its pan channel+9.
 */
#define MIX_PAN_X 21
#define MIX_PAN_Y 47
#define MIX_PAN_R 14
#define MIX_VOL_X 10
#define MIX_VOL_Y 85
#define MIX_VOL_W 32
#define MIX_VOL_H 0x61
#define MIX_STEP 43

int ds10_ui_mixer_hit(int x, int y)
{
    int ch;

    for (ch = 0; ch < 6; ch++) {
        int cx = MIX_PAN_X + MIX_STEP * ch;

        if (x >= cx - MIX_PAN_R && x < cx + MIX_PAN_R &&
            y >= MIX_PAN_Y - MIX_PAN_R && y < MIX_PAN_Y + MIX_PAN_R)
            return 9 + ch;
    }
    for (ch = 0; ch < 6; ch++) {
        int vx = MIX_VOL_X + MIX_STEP * ch;

        if (x >= vx && x < vx + MIX_VOL_W &&
            y >= MIX_VOL_Y && y < MIX_VOL_Y + MIX_VOL_H)
            return 17 + ch;
    }
    return -1;
}

int ds10_ui_knob_hit(int x, int y)
{
    size_t i;

    for (i = 0; i < sizeof SYN_KNOB / sizeof SYN_KNOB[0]; i++)
        if (x >= SYN_KNOB[i].x - KNOB_R && x < SYN_KNOB[i].x + KNOB_R &&
            y >= SYN_KNOB[i].y - KNOB_R && y < SYN_KNOB[i].y + KNOB_R)
            return SYN_KNOB[i].id;
    for (i = 0; i < sizeof SYN_SLIDER / sizeof SYN_SLIDER[0]; i++)
        if (x >= SYN_SLIDER[i].x && x < SYN_SLIDER[i].x + SYN_SLIDER[i].w &&
            y >= SYN_SLIDER[i].y && y < SYN_SLIDER[i].y + SYN_SLIDER[i].h)
            return SYN_SLIDER[i].id;
    return -1;
}

const char *ds10_ui_knob_name(int id)
{
    if (id < 0 || id > 30) return "";
    return KNOB_NAME[id];
}

/* The captured sprite layer already draws a pointer for every knob: sixteen
 * double-size rot/scale sprites with tile 0x21a, each with its own affine
 * index. Take them over so turning a knob turns its pointer.
 *
 * Assignment is nearest-first and one-to-one. It has to be: the knob centres
 * here are measured off the rendered page and are a pixel or two out, so two
 * knobs can otherwise want the same pointer.
 */
static void claim_knob_slots(ds10_ui *u, const char *page)
{
    struct { int slot, affine, cx, cy; } ptr[32];
    int nptr = 0, slot;
    size_t i;

    for (i = 0; i < sizeof u->knob_slot / sizeof u->knob_slot[0]; i++) {
        u->knob_slot[i] = -1;
        u->knob_affine[i] = -1;
    }
    if (strcmp(page, "61_SYN_EDIT") != 0) return;

    for (slot = 0; slot < 128 && nptr < 32; slot++) {
        const uint8_t *e = u->gpu.oam + slot * 8;
        int a0 = e[0] | (e[1] << 8), a1 = e[2] | (e[3] << 8);
        int a2 = e[4] | (e[5] << 8);

        if ((a2 & 0x3FF) != PTR_TILE) continue;
        if (((a0 >> 8) & 3) != 3) continue; /* double-size rot/scale */
        ptr[nptr].slot = slot;
        ptr[nptr].affine = (a1 >> 9) & 0x1F;
        ptr[nptr].cx = (a1 & 0x1FF) + PTR_W;
        ptr[nptr].cy = (a0 & 0xFF) + PTR_H;
        nptr++;
    }

    for (;;) {
        int bestk = -1, bestp = -1, bestd = 24 * 24;
        int k, j;

        for (k = 0; k < (int)(sizeof SYN_KNOB / sizeof SYN_KNOB[0]); k++) {
            if (u->knob_slot[k] >= 0) continue;
            for (j = 0; j < nptr; j++) {
                int dx, dy, d;

                if (ptr[j].slot < 0) continue;
                dx = ptr[j].cx - SYN_KNOB[k].x;
                dy = ptr[j].cy - SYN_KNOB[k].y;
                d = dx * dx + dy * dy;
                if (d < bestd) { bestd = d; bestk = k; bestp = j; }
            }
        }
        if (bestk < 0) break;
        u->knob_slot[bestk] = ptr[bestp].slot;
        u->knob_affine[bestk] = ptr[bestp].affine;
        ptr[bestp].slot = -1;
    }
}

/* data/pages.bin, if it is there. Without it the pages simply draw with no
 * sprite layer, which is how this looked before the file existed. */
static int rd8(FILE *f) { return fgetc(f); }

static int rd16f(FILE *f)
{
    int a = fgetc(f), b = fgetc(f);

    return a | (b << 8);
}

static int32_t rd32f(FILE *f)
{
    uint32_t v = (uint32_t)fgetc(f);

    v |= (uint32_t)fgetc(f) << 8;
    v |= (uint32_t)fgetc(f) << 16;
    v |= (uint32_t)fgetc(f) << 24;
    return (int32_t)v;
}

static int rdname(FILE *f, char *dst, size_t cap)
{
    int len = fgetc(f);

    if (len <= 0 || (size_t)len >= cap) return -1;
    if (fread(dst, 1, (size_t)len, f) != (size_t)len) return -1;
    dst[len] = 0;
    return 0;
}

static void load_pages(ds10_ui *u)
{
    char path[512];
    FILE *f;
    unsigned char hdr[12];
    uint32_t version, count, i;

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
    if (version != 4 || count == 0 || count > 512) {
        fclose(f);
        return;
    }
    u->pages = calloc(count, sizeof *u->pages);
    if (!u->pages) {
        fclose(f);
        return;
    }
    for (i = 0; i < count; i++) {
        page_list *p = &u->pages[i];
        int k;

        if (rdname(f, p->name, sizeof p->name) < 0) break;
        p->engine = (uint8_t)rd8(f);
        p->nrec = rd16f(f);
        if (p->nrec < 0 || p->nrec > 256) break;
        p->rec = calloc((size_t)p->nrec + 1, sizeof *p->rec);
        if (!p->rec) break;
        for (k = 0; k < p->nrec; k++) {
            page_rec *r = &p->rec[k];
            page_item *it = &r->item;
            int j;

            r->kind = (uint8_t)rd8(f);
            if (r->kind) {
                if (fread(r->entry, 1, 6, f) != 6) goto done;
                continue;
            }
            if (rdname(f, it->bank, sizeof it->bank) < 0) goto done;
            it->cell = (uint16_t)rd16f(f);
            it->prio = (uint8_t)rd8(f);
            it->flags = (uint8_t)rd8(f);
            it->affine = (uint8_t)rd8(f);
            (void)rd8(f);
            it->base = (uint16_t)rd16f(f);
            it->x = rd32f(f);
            it->y = rd32f(f);
            for (j = 0; j < 4; j++) it->mtx[j] = rd32f(f);
        }
        u->npages++;
    }
done:
    fclose(f);
}

/* The cell banks a display list can name. */
static void load_banks(ds10_ui *u)
{
    static const char *const NAME[] = {
        "CHARACTER_c", "CHARACTER_S", "COMMON_c", "FONT", "DS10FONT",
        "S_c", "90_MAP", "81_KEY_c", "71_KAOSS_c", "60_SYN_SEQ_PIANO_c",
        "30_MIXER", "52_FX_c", "51_ALL_DRUMS_c", "55_DRUMEDITMAP_c",
        "54_DRUM_FX", "33_DRUM_SELECTOR", "31_SONG_c", "32_PATTERN_c",
        "01_MEIN_MENU_c", "3D_TEX", "3D_MESS_TEX", "icon", "UNFONT",
    };
    size_t k;

    for (k = 0; k < sizeof NAME / sizeof NAME[0] &&
                u->nbanks < (int)(sizeof u->bank / sizeof u->bank[0]); k++) {
        cell_bank *b = &u->bank[u->nbanks];
        uint8_t *blob;
        size_t len;

        blob = slurp(u, "mainobj", NAME[k], ".NCER", &len);
        if (!blob) continue;
        if (ds10_ncer_parse(&b->ncer, blob, len) < 0) continue;
        snprintf(b->name, sizeof b->name, "%s", NAME[k]);
        b->blob = blob;
        /* Keep this one: the display lists point into it. */
        u->nblob--;
        u->nbanks++;
    }
}

static const cell_bank *find_bank(const ds10_ui *u, const char *name)
{
    int i;

    for (i = 0; i < u->nbanks; i++)
        if (strcmp(u->bank[i].name, name) == 0) return &u->bank[i];
    return NULL;
}

/* Expand a page's display list into its half of OAM. */
static void draw_page(ds10_ui *u, const char *name, int engine)
{
    uint8_t *oam = u->gpu.oam + (engine ? 0x400 : 0);
    int slot = 0, i;
    const page_list *p = NULL;

    /* Unused slots park off the bottom of the screen, as the game's do. */
    for (i = 0; i < 128; i++) {
        oam[i * 8] = 192;
        oam[i * 8 + 1] = 0;
        oam[i * 8 + 2] = 0;
        oam[i * 8 + 3] = 0;
        oam[i * 8 + 4] = 0;
        oam[i * 8 + 5] = 0;
    }
    for (i = 0; i < u->npages; i++)
        if (strcmp(u->pages[i].name, name) == 0) { p = &u->pages[i]; break; }
    if (!p) return;

    for (i = 0; i < p->nrec && slot < 128; i++) {
        const page_rec *r = &p->rec[i];
        const page_item *it = &r->item;
        const cell_bank *b;
        ds10_cell cell;
        ds10_cell_ref ref;
        int32_t pos[2];
        int n, k;

        if (r->kind) {
            memcpy(oam + slot * 8, r->entry, 6);
            slot++;
            continue;
        }
        b = find_bank(u, it->bank);
        if (!b || ds10_ncer_cell(&b->ncer, it->cell, &cell) < 0) continue;
        ref.nobj = cell.nobj;
        ref.obj = cell.obj;
        pos[0] = it->x;
        pos[1] = it->y;
        n = ds10_cell_to_oam(oam + slot * 8, 128 - slot, &ref,
                             (it->flags & 1) ? it->mtx : NULL, pos,
                             it->affine, (it->flags >> 1) & 1);
        for (k = 0; k < n; k++) {
            uint8_t *e = oam + (slot + k) * 8;
            int a2 = e[4] | (e[5] << 8);
            /* The tile the sheet is loaded at: 0x200 for CHARACTER_c, 0
             * for the sheet at tile 0, and 0x100 for the top screen's second
             * copy of CHARACTER_S. */
            int base = it->base;

            a2 = (a2 & ~0x3FF) | (((a2 & 0x3FF) + base) & 0x3FF);
            a2 = (a2 & ~0x0C00) | (it->prio << 10);
            e[4] = (uint8_t)a2;
            e[5] = (uint8_t)(a2 >> 8);
        }
        if ((it->flags & 1) && it->affine < 32)
            write_affine_mtx(oam, it->affine, it->mtx);
        slot += n;
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
    load_banks(u);
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
        draw_page(u, bottom, 0);
        claim_knob_slots(u, bottom);
    }
    if (top) {
        rc |= load_page(u, top, 1);
        draw_page(u, top, 1);
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

/* 81_KEY's top bar, from 0x0204bff8: OCT- at (4,2) and OCT+ at (0x23,2), both
 * 0x1c by 0xe, and the synth selector at (0x42,4), 0x46 by 0xc. */
static const struct { int x, y, w, h, id; } KEY_BUTTON[] = {
    {0x04, 2, 0x1C, 0x0E, 0},
    {0x23, 2, 0x1C, 0x0E, 1},
    {0x42, 4, 0x46, 0x0C, 2},
};

/* 52_FX, from 0x0205e1e4 and its table at 0x02086874.
 *
 * Thirteen rotary knobs share four positions along y = 161. Which three of
 * them are showing depends on the effect type - the table holds all of them at
 * the same coordinates, three per type - and DRY/WET is always the fourth.
 * Below them is one slider at (8, 146).
 *
 * The parameter ids are mixer knob ids: the mixer forwards 1..8 straight to
 * the effect, which is what the audio side found independently.
 *
 *   delay    DELAY TIME 2   L/R RATIO 3   FEEDBACK 7
 *   flanger  LFO FREQ   4   DEPTH     5   (unused)
 *   chorus   LFO FREQ   4   DEPTH     5   FEEDBACK 7
 *   always   DRY/WET    8               slider 6
 */
#define FX_KNOB_Y 161
#define FX_KNOB_R 20

static const int FX_PARAM[3][3] = {
    {2, 3, 7},   /* delay */
    {4, 5, -1},  /* flanger */
    {4, 5, 7},   /* chorus */
};
static const int FX_KNOB_X[4] = {64, 118, 172, 226};

/* 60_SYN_SEQ_PIANO's grid. The cell is 13 by 13 with its origin at (42,32),
 * measured off the rendered page - and the constructor at 0x02050108 agrees:
 * it carries 0x9c = 156 = 12 rows of 13 for the visible height and 0x138 = 312
 * = 24 rows for the whole note range, which is why there is a scrollbar.
 */
#define SEQ_CELL 13
#define SEQ_X0 42
#define SEQ_Y0 32
#define SEQ_COLS 16
#define SEQ_ROWS 12

/* 71_KAOSS, from 0x02049150: the pad is the whole width below the button bar,
 * placed by FUN_02060330(pad, 0, 0x14, 0x100, 0xac) - x 0, y 20, 256 by 172.
 * Which two knobs it drives is a per-document assignment the screen reads at
 * construction; the axes here are just 0..127 and the caller decides. */
#define KAOSS_X 0
#define KAOSS_Y 20
#define KAOSS_W 256
#define KAOSS_H 172

/* 70_PATCH is the modulation patch bay, and this is read out of its
 * constructor (0x0204e140) and the table at 0x02085918 rather than measured -
 * which matters, because measuring it got both the row and the knobs wrong.
 * Six rotary knobs at y = 93, x = 23 + 43*i, each with a 40 by 40 hit box:
 *
 *   MG FREQ 24, PITCH IN INT 3, VCO1 P.IN INT 4, VCO2 P.IN INT 5,
 *   CUTOFF IN INT 15, VCA IN INT 17
 *
 * plus a slider at (11,38) for MG BPM, knob 25. The knobs numbered 26..30 are
 * the source selectors - the jack sockets along the bottom - not these.
 */
static const struct { int x, y, id; } PATCH_KNOB[] = {
    { 23, 93, 24}, { 66, 93,  3}, {109, 93,  4},
    {152, 93,  5}, {195, 93, 15}, {238, 93, 17},
};
#define PATCH_KNOB_R 20

/* 33_DRUM_SELECTER, from 0x020572b0 and its table at 0x02086300: four panels
 * of 117 by 76 in a two-by-two grid, labelled DRUM1 to DRUM4. */
static const struct { int x, y, w, h; } DRUM_PANEL[4] = {
    {  7,  25, 117, 76}, {132,  25, 117, 76},
    {  7, 110, 117, 76}, {132, 110, 117, 76},
};

/* The interface's text is one 8x8 sprite per character out of the character
 * sheet, not a font rasterised into VRAM. The sheet's first glyphs are, in
 * 32-byte tiles:
 *
 *   0x00..0x09  the digits
 *   0x0a, 0x0b  plus and minus
 *   0x11..0x2a  A to Z
 *   0x2b        hash
 *
 * The pen advances by the glyph's ink width plus one, which reproduces the
 * spacing of every caption checked against a captured frame.
 *
 * Engine B addresses sprite tiles in 32-byte units - DISPCNT bits 20-21 are 0
 * there and 1 on engine A - so a tile index is a glyph index directly.
 */
static int glyph_tile(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c == '+') return 0x0A;
    if (c == '-') return 0x0B;
    if (c >= 'A' && c <= 'Z') return 0x11 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 0x11 + (c - 'a');
    if (c == '#') return 0x2B;
    return -1;
}

/* How wide a glyph's ink is, read out of the sheet in VRAM. */
static int glyph_width(const ds10_ui *u, int engine, int tile)
{
    const uint8_t *vram = engine ? u->gpu.vram.i : u->gpu.vram.e;
    size_t len = engine ? sizeof u->gpu.vram.i : sizeof u->gpu.vram.e;
    size_t off = (size_t)tile * 32;
    int x, y, lo = 8, hi = -1;

    if (off + 32 > len) return 4;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            uint8_t b = vram[off + y * 4 + x / 2];
            int v = (x & 1) ? (b >> 4) : (b & 0x0F);

            if (!v) continue;
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
    return hi < 0 ? 2 : hi - lo + 1;
}

int ds10_ui_text(ds10_ui *u, int engine, int slot, int x, int y,
                 const char *s, int palette, int priority)
{
    uint8_t *oam = u->gpu.oam + (engine ? 0x400 : 0);

    for (; *s && slot < 128; s++) {
        int tile = glyph_tile((unsigned char)*s);
        uint8_t *e;

        if (tile < 0) {           /* a space, or something not in the sheet */
            x += 4;
            continue;
        }
        e = oam + slot * 8;
        e[0] = (uint8_t)y;
        e[1] = 0;                 /* 8x8, normal, 16 colour */
        e[2] = (uint8_t)(x & 0xFF);
        e[3] = (uint8_t)((x >> 8) & 1);
        e[4] = (uint8_t)tile;
        e[5] = (uint8_t)(((tile >> 8) & 3) | (priority << 2) |
                         (palette << 4));
        x += glyph_width(u, engine, tile) + 1;
        slot++;
    }
    return slot;
}

/* 90_MAP's bottom bar. The slots and positions are the ones a captured frame
 * has: the session name from slot 0 at x 27, the swing value at 117 and the
 * tempo at 176, all on the baseline y = 177 in palette 9. */
void ds10_ui_map_labels(ds10_ui *u, const char *name, int swing, int bpm)
{
    char num[8];
    int slot;

    slot = ds10_ui_text(u, 1, 0, 27, 177, name, 9, 2);
    while (slot < 9) {          /* park whatever the name did not need */
        uint8_t *e = u->gpu.oam + 0x400 + slot * 8;

        e[0] = 192;
        e[1] = e[2] = e[3] = e[4] = e[5] = 0;
        slot++;
    }
    snprintf(num, sizeof num, "%d", swing);
    slot = ds10_ui_text(u, 1, 9, 117, 177, num, 9, 0);
    snprintf(num, sizeof num, "%d", bpm);
    ds10_ui_text(u, 1, slot, 176, 177, num, 9, 0);
}

int ds10_ui_drum_hit(int x, int y)
{
    int i;

    for (i = 0; i < 4; i++)
        if (x >= DRUM_PANEL[i].x && x < DRUM_PANEL[i].x + DRUM_PANEL[i].w &&
            y >= DRUM_PANEL[i].y && y < DRUM_PANEL[i].y + DRUM_PANEL[i].h)
            return i;
    return -1;
}

int ds10_ui_patch_hit(int x, int y)
{
    size_t i;

    for (i = 0; i < sizeof PATCH_KNOB / sizeof PATCH_KNOB[0]; i++)
        if (x >= PATCH_KNOB[i].x - PATCH_KNOB_R &&
            x < PATCH_KNOB[i].x + PATCH_KNOB_R &&
            y >= PATCH_KNOB[i].y - PATCH_KNOB_R &&
            y < PATCH_KNOB[i].y + PATCH_KNOB_R)
            return PATCH_KNOB[i].id;
    /* The MG BPM slider. */
    if (x >= 11 && x < 11 + 0x13 && y >= 38 && y < 38 + 0x1A) return 25;
    return -1;
}

int ds10_ui_kaoss_hit(int x, int y, int *vx, int *vy)
{
    if (x < KAOSS_X || x >= KAOSS_X + KAOSS_W) return -1;
    if (y < KAOSS_Y || y >= KAOSS_Y + KAOSS_H) return -1;
    *vx = (x - KAOSS_X) * 127 / (KAOSS_W - 1);
    /* Up is more, as the pad is drawn. */
    *vy = 127 - (y - KAOSS_Y) * 127 / (KAOSS_H - 1);
    return 0;
}

int ds10_ui_seq_hit(int x, int y, int *step, int *row)
{
    int c = (x - SEQ_X0) / SEQ_CELL, r = (y - SEQ_Y0) / SEQ_CELL;

    if (x < SEQ_X0 || y < SEQ_Y0 || c >= SEQ_COLS || r >= SEQ_ROWS) return -1;
    *step = c;
    *row = r;
    return 0;
}

int ds10_ui_fx_hit(int x, int y, int type)
{
    int i;

    if (type < 0 || type > 2) type = 0;
    if (x >= 8 && x < 8 + 0x13 && y >= 146 && y < 146 + 0x1A) return 6;
    if (y < FX_KNOB_Y - FX_KNOB_R || y >= FX_KNOB_Y + FX_KNOB_R) return -1;
    for (i = 0; i < 4; i++) {
        int cx = FX_KNOB_X[i];

        if (x < cx - FX_KNOB_R || x >= cx + FX_KNOB_R) continue;
        return i == 3 ? 8 : FX_PARAM[type][i];
    }
    return -1;
}

int ds10_ui_key_button(int x, int y)
{
    size_t i;

    for (i = 0; i < sizeof KEY_BUTTON / sizeof KEY_BUTTON[0]; i++)
        if (x >= KEY_BUTTON[i].x && x < KEY_BUTTON[i].x + KEY_BUTTON[i].w &&
            y >= KEY_BUTTON[i].y && y < KEY_BUTTON[i].y + KEY_BUTTON[i].h)
            return KEY_BUTTON[i].id;
    return -1;
}

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

ds10_gpu *ds10_ui_gpu(ds10_ui *u)
{
    return &u->gpu;
}

const char *ds10_ui_error(const ds10_ui *u)
{
    return u->err;
}
