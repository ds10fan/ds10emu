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
#include "widget.h"
#include "cell.h"
#include "obj.h"
#include "../gfx/nitro.h"

static void draw_page(ds10_ui *u, const char *name, int engine);

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
    char shown[2][24];  /* the page on each engine */
    cell_bank bank[24];
    int nbanks;
    /* The OAM slot and affine index each synth-edit knob's pointer uses,
     * or -1. The two are different: the sprite lives in one slot and its
     * rotation matrix in another, named by attr1 bits 9-13. */
    int knob_slot[16];
    int knob_affine[16];
    /* The first OAM slot each engine's static page left free. Anything drawn
     * from live state - step markers, the cursor - goes after it, so a redraw
     * of the state does not have to rebuild the page. */
    int free_slot[2];
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
        /* And the drum selector, whose two files disagree the same way. */
        else if ((p = strstr(alt, "SELECTER")) != NULL)
            memcpy(p, "SELECTOR", 8);
        else if ((p = strstr(alt, "SELECTOR")) != NULL)
            memcpy(p, "SELECTER", 8);
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
        /* Most pages have no palette of their own, so a miss here is normal
         * and must not leave an error behind for the caller to report. */
        char keep[sizeof u->err];
        uint8_t *pblob;
        ds10_nclr pal;

        snprintf(keep, sizeof keep, "%s", u->err);
        pblob = slurp(u, "mainbg", name, ".NCLR", &glen);
        if (!pblob) snprintf(u->err, sizeof u->err, "%s", keep);

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
#define PTR_TILE 0x21A
#define PTR_PAL 3
#define PTR_PRIO 2
#define PTR_W 8
#define PTR_H 16
#define PTR_SWEEP 270   /* the sweep every screen passes 0x02055b98 */
/* The pointer sprite does not spin in place: its centre orbits the knob's,
 * because the game rotates about the knob centre and the sprite's own centre
 * is offset from that. The offset is this vector turned by the same angle.
 *
 * Read off thirteen pointers in the captured frame, at four distinct angles:
 * at 0 degrees the sprite centre sits 3 right and 7 up of the knob centre,
 * and rotating (3,-7) reproduces every other one exactly - (-7,+3) at -135,
 * (+3,+7) at +135, (-3,-7) at -45. Drawing it at the knob centre instead put
 * every pointer about 8 pixels out. */
#define PTR_ORBIT_X 3.0
#define PTR_ORBIT_Y (-7.0)
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

/* The matrix is the original's own now - the ROM's sine table indexed by the
 * angle, at unit scale - so the pointer is exact rather than approximately
 * right. See ds10_widget_affine. */
static void write_affine(uint8_t *oam, int slot, int16_t angle)
{
    int32_t m[4];

    ds10_widget_affine(angle, 0x1000, 0x1000, m);
    write_affine_mtx(oam, slot, m);
}

void ds10_ui_knob_value(ds10_ui *u, int id, int value, int lo, int range)
{
    uint8_t *oam = u->gpu.oam;
    size_t i;
    int16_t angle;
    double deg;

    if (range < 2) range = 2;
    /* Both of these are the original's own now, decompiled: the angle from
     * 0x02055b98 in 16-bit units of a turn, and the matrix from the ROM's
     * sine table. The knob descriptors carry a count, so the maximum is
     * lo + range - 1. Degrees are only for the orbit below. */
    angle = ds10_widget_knob_angle(lo, lo + range - 1, value, PTR_SWEEP, 0);
    deg = angle * 360.0 / 65536.0;

    for (i = 0; i < sizeof SYN_KNOB / sizeof SYN_KNOB[0]; i++) {
        ds10_obj o;
        int slot;

        if (SYN_KNOB[i].id != id) continue;
        slot = u->knob_slot[i];
        if (slot < 0) continue;
        {
            double rad = deg * PTR_PI / 180.0;
            double c = cos(rad), sn = sin(rad);

            memset(&o, 0, sizeof o);
            o.count = 1;
            o.x = SYN_KNOB[i].x - PTR_W +
                  (int)lrint(PTR_ORBIT_X * c - PTR_ORBIT_Y * sn);
            o.y = SYN_KNOB[i].y - PTR_H +
                  (int)lrint(PTR_ORBIT_X * sn + PTR_ORBIT_Y * c);
        }
        o.tile = PTR_TILE;
        o.palette = PTR_PAL;
        o.priority = PTR_PRIO;
        o.rot = 0x300;          /* rot/scale, double size */
        o.shape = 0x8000;       /* tall */
        o.affine = u->knob_affine[i];
        o.big = 0;
        ds10_obj_build(&o);
        memcpy(oam + slot * 8, o.oam, 6);
        write_affine(oam, u->knob_affine[i], angle);
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
#define MIX_VOL_H 0x61        /* the whole widget, which is what it catches */
/* ...of which 0x51 is track and 0x10 is the handle, so the handle's travel is
 * the track alone. Using the total sent the faders 16 pixels too far. */
#define MIX_VOL_TRAVEL 0x51
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
    u->free_slot[engine ? 1 : 0] = slot;
}

/* Put one cell from a named bank into the next free OAM slot. This is how the
 * live parts of a screen are drawn: the same expander the page itself uses,
 * pointed at whatever the state says should be there. */
static int put_cell(ds10_ui *u, int engine, int *slot, const char *bank,
                    int cell, int x, int y, int prio, int base)
{
    uint8_t *oam = u->gpu.oam + (engine ? 0x400 : 0);
    const cell_bank *b = find_bank(u, bank);
    ds10_cell c;
    ds10_cell_ref ref;
    int32_t pos[2];
    int n, k;

    if (!b || ds10_ncer_cell(&b->ncer, cell, &c) < 0) return 0;
    ref.nobj = c.nobj;
    ref.obj = c.obj;
    pos[0] = x * 0x1000;
    pos[1] = y * 0x1000;
    n = ds10_cell_to_oam(oam + *slot * 8, 128 - *slot, &ref, NULL, pos, 0, 0);
    for (k = 0; k < n; k++) {
        uint8_t *e = oam + (*slot + k) * 8;
        int a2 = e[4] | (e[5] << 8);

        a2 = (a2 & ~0x3FF) | (((a2 & 0x3FF) + base) & 0x3FF);
        a2 = (a2 & ~0x0C00) | (prio << 10);
        e[4] = (uint8_t)a2;
        e[5] = (uint8_t)(a2 >> 8);
    }
    *slot += n;
    return n;
}

/* Park every slot from `slot` on, so a redraw that draws fewer markers than
 * the last one does not leave the old ones behind. */
static void park_from(ds10_ui *u, int engine, int slot)
{
    uint8_t *oam = u->gpu.oam + (engine ? 0x400 : 0);

    for (; slot < 128; slot++) {
        oam[slot * 8] = 192;
        oam[slot * 8 + 1] = 0;
        oam[slot * 8 + 2] = 0;
        oam[slot * 8 + 3] = 0;
        oam[slot * 8 + 4] = 0;
        oam[slot * 8 + 5] = 0;
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
        snprintf(u->shown[0], sizeof u->shown[0], "%s", bottom);
        rc |= load_page(u, bottom, 0);
        draw_page(u, bottom, 0);
        claim_knob_slots(u, bottom);
    }
    /* Whatever the last page drew on the 3D plane is not this one's. */
    memset(u->gpu.plane3d, 0, sizeof u->gpu.plane3d);
    if (top) {
        snprintf(u->shown[1], sizeof u->shown[1], "%s", top);
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

/* 90_MAP's boxes, from its constructor at 0x02062914 and the tables at
 * 0x02087018 and 0x02086f08. Each entry carries its rectangle, its caption and
 * an index into the twenty-entry screen table at 0x02085f80, which is what
 * names the page it opens.
 *
 * This replaces coordinates read off the rendered screen. The second table is
 * the DRUMS submenu, which the map shows over itself.
 */
static const struct {
    int x, y, w, h;
    int screen;          /* index into the screen table at 0x02085f80 */
    const char *page;
} MAP_BOX[] = {
    {  7,  25, 46, 16,  0, "60_SYN_SEQ_PIANO"},  /* SYN1 SEQ */
    {  7,  87, 46, 16,  1, "60_SYN_SEQ_PIANO"},  /* SYN2 SEQ */
    {  7, 149, 54, 16, 12, "51_ALL_DRUMS"},      /* DRUMS SEQ */
    { 39,  45, 54, 16,  8, "81_KEY_i"},          /* SYN1 KBD */
    { 39,  65, 54, 16,  6, "71_KAOSS"},          /* SYN1 KAOSS */
    { 39, 107, 54, 16,  9, "81_KEY_i"},          /* SYN2 KBD */
    { 39, 127, 54, 16,  7, "71_KAOSS"},          /* SYN2 KAOSS */
    {117,  25, 46, 16,  2, "61_SYN_EDIT"},       /* SYN1 EDIT */
    {117,  87, 46, 16,  3, "61_SYN_EDIT"},       /* SYN2 EDIT */
    {131,  54, 53, 16,  4, "70_PATCH"},          /* SYN1 PATCH */
    {131, 116, 53, 16,  5, "70_PATCH"},          /* SYN2 PATCH */
    {203,  25, 36, 16, 11, "30_MIXER"},
    {207,  55, 30, 16, 10, "52_FX"},
    {  4,   2, 76, 14, 13, "32_PATTERN"},
    {206, 142, 46, 16, 18, "33_DRUM_SELECTER"},  /* DRUMS */
    /* the DRUMS submenu, which is the same table with four more in front */
    { 85,  64, 88, 16, 17, "53_DRUMEDIT"},
    { 85,  96, 88, 16, 19, "63_DRUM_EDIT_PATCH"},
    { 85, 128, 88, 16, 16, "54_DRUM_FX"},
};

/* Which synth a box selects, or -1 for a box that is not per-synth.
 *
 * The screen table's first ten entries are five pairs - sequencer, edit,
 * patch, KAOSS, keyboard - and the pair is the synth 1 and synth 2 variants
 * of one page. So the low bit of the index is the synth, which is how tapping
 * SYN2 EDIT gets you synth 2's patch rather than synth 1's. */
int ds10_ui_map_synth(int x, int y)
{
    size_t i;

    for (i = 0; i < sizeof MAP_BOX / sizeof MAP_BOX[0]; i++)
        if (x >= MAP_BOX[i].x && x < MAP_BOX[i].x + MAP_BOX[i].w &&
            y >= MAP_BOX[i].y && y < MAP_BOX[i].y + MAP_BOX[i].h)
            return MAP_BOX[i].screen < 10 ? MAP_BOX[i].screen & 1 : -1;
    return -1;
}

const char *ds10_ui_map_hit(int x, int y)
{
    size_t i;

    for (i = 0; i < sizeof MAP_BOX / sizeof MAP_BOX[0]; i++)
        if (x >= MAP_BOX[i].x && x < MAP_BOX[i].x + MAP_BOX[i].w &&
            y >= MAP_BOX[i].y && y < MAP_BOX[i].y + MAP_BOX[i].h)
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

/* 60_SYN_SEQ_PIANO's grid, from the update at 0x02051074:
 *
 *     FUN_0205fdc4(scroller, 0x2d, 0x23, 0xd0, 0x9c, 0x10, 0xc)
 *
 * - a scrollable region at (45,35), 208 by 156, of 16 by 12 cells. So the cell
 * is 13 square, and the constructor's 0x9c is those twelve rows while its
 * 0x138 is the whole twenty-four-row note range, which is what the scrollbar
 * moves through.
 *
 * Measuring the grid off the rendered page put the origin at (42,32), three
 * pixels out in both axes.
 */
#define SEQ_CELL 13
#define SEQ_X0 45
#define SEQ_Y0 35
#define SEQ_COLS 16
#define SEQ_ROWS 12
#define SEQ_RANGE 24    /* the scrollable height, in rows: two octaves */
#define SEQ_TOP_DEFAULT 69      /* A4 on the top row, as the capture had it */
/* The one sprite the captured frame puts above the grid, at the first
 * column's left edge. */
#define SEQ_CURSOR_CELL 4
#define SEQ_CURSOR_Y 20

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

/* A slider's value-to-position, from 0x02039188. The handle moves linearly
 * along the span; the orientation decides which axis and which end is the
 * minimum. The original writes the result into the drawable's x or y at 20.12
 * and leaves the other axis at the slider's origin.
 *
 * The knob's equivalent is the sweep in ds10_ui_knob_value; between them they
 * are the whole of what a control redraws when its value changes.
 */
int ds10_ui_slider_pos(int value, int min, int max, int span, int orient)
{
    int range = max - min, off;

    if (value > max) value = max;
    if (value < min) value = min;
    if (range <= 0) return 0;
    off = span * (value - min) / range;
    /* 0 and 3 measure from the far end. */
    return (orient == 0 || orient == 3) ? span - off : off;
}

/* The mixer's fader handles are the six cell-15 items in its display list, at
 * x = 10 + 43*ch. The constructor puts the slider's origin at y = 85 with a
 * span of 0x61, so the handle sits at 85 plus what ds10_ui_slider_pos gives -
 * a vertical slider with its minimum at the bottom.
 *
 * Moving it and redrawing is what the screen's redraw pass does; the meters
 * beside it (0x02067160) take each track's step velocity divided by four.
 */
#define MIX_FADER_CELL 15
#define MIX_FADER_ORIGIN 85

void ds10_ui_mixer_fader(ds10_ui *u, int ch, int value)
{
    page_list *p = NULL;
    int i, seen = 0;

    if (ch < 0 || ch >= 6) return;
    if (strcmp(u->shown[0], "30_MIXER") != 0) return;
    for (i = 0; i < u->npages; i++)
        if (strcmp(u->pages[i].name, "30_MIXER") == 0) { p = &u->pages[i]; break; }
    if (!p) return;

    for (i = 0; i < p->nrec; i++) {
        page_item *it = &p->rec[i].item;

        if (p->rec[i].kind || it->cell != MIX_FADER_CELL) continue;
        /* The list is not in channel order, so go by the x the constructor
         * gave each fader. */
        if ((it->x >> 12) != MIX_VOL_X + MIX_STEP * ch) continue;
        it->y = (MIX_FADER_ORIGIN +
                 ds10_ui_slider_pos(value, 0, 127, MIX_VOL_TRAVEL, 0)) << 12;
        seen = 1;
        break;
    }
    if (seen) draw_page(u, "30_MIXER", 0);
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

/* 62_DRUM_SEQ's grid, from its update at 0x0205a384:
 *
 *     FUN_0205fdc4(scroller, 0x2d, 0x23, 0xd0, 0x34, 0x10, 4)
 *
 * the same origin and cell as the synth sequencer's, four rows instead of
 * twelve - one per drum. */
#define DSEQ_ROWS 4

int ds10_ui_drumseq_hit(int x, int y, int *step, int *row)
{
    int c = (x - SEQ_X0) / SEQ_CELL, r = (y - SEQ_Y0) / SEQ_CELL;

    if (x < SEQ_X0 || y < SEQ_Y0 || c >= SEQ_COLS || r >= DSEQ_ROWS) return -1;
    *step = c;
    *row = r;
    return 0;
}

/* 32_PATTERN's sixteen buttons, four by four.
 *
 * Read out of the page's own constructor, 0x0202e01c, which recon had filed
 * as a shared widget helper rather than a screen. It places sixteen widgets
 * named PAT%02d at x = 6 + 63*(n%4), y = 24 + 33*(n/4), each 55 by 27.
 *
 * These were tapped before they were read - tools/probe_buttons.py found the
 * columns breaking between x 64 and 69, 129 and 134, 194 and 199, and the rows
 * between y 56 and 60, 88 and 92, 120 and 124. Those brackets agree with the
 * gaps the real rectangles leave (61..69, 124..132, 187..195), so the probe
 * was right about where the boundaries are; the pitch it implied, 65 and 32,
 * was one out.
 */
#define PAT_X0 6
#define PAT_Y0 24
#define PAT_W 55
#define PAT_H 27
#define PAT_XSTEP 63
#define PAT_YSTEP 33
#define PAT_COLS 4
#define PAT_ROWS 4

int ds10_ui_pattern_hit(int x, int y)
{
    int c, r;

    if (x < PAT_X0 || y < PAT_Y0) return -1;
    c = (x - PAT_X0) / PAT_XSTEP;
    r = (y - PAT_Y0) / PAT_YSTEP;
    if (c >= PAT_COLS || r >= PAT_ROWS) return -1;
    /* The gaps between the buttons are not part of any of them. */
    if (x - PAT_X0 - c * PAT_XSTEP >= PAT_W) return -1;
    if (y - PAT_Y0 - r * PAT_YSTEP >= PAT_H) return -1;
    return r * PAT_COLS + c;
}

/* Where that button is, so the selection marker can be drawn on it. */
void ds10_ui_pattern_box(int n, int *x, int *y)
{
    if (n < 0 || n >= PAT_COLS * PAT_ROWS) n = 0;
    *x = PAT_X0 + (n % PAT_COLS) * PAT_XSTEP;
    *y = PAT_Y0 + (n / PAT_COLS) * PAT_YSTEP;
}

/* The marker on the selected pattern. The captured page has CHARACTER_c cell
 * 20 at (6,24), which is exactly the first button's origin now that the origin
 * is the constructor's rather than the probe's - so the marker sits on the
 * button, with no offset. The (4,-2) that used to be here was the difference
 * between the two origins. */
#define PAT_MARK_CELL 20
#define PAT_MARK_DX 0
#define PAT_MARK_DY 0
#define PAT_MARK_BASE 0x200   /* CHARACTER_c's tile base, as everywhere */

void ds10_ui_pattern_mark(ds10_ui *u, int n)
{
    int slot = u->free_slot[0], x, y;

    ds10_ui_pattern_box(n, &x, &y);
    put_cell(u, 0, &slot, "CHARACTER_c", PAT_MARK_CELL, x + PAT_MARK_DX,
             y + PAT_MARK_DY, 2, PAT_MARK_BASE);
    park_from(u, 0, slot);
}

/* The button at the right of the sequencer's top bar brings the navigation
 * map to the touch screen - the same thing the L shoulder does. Tapping it
 * was the only thing in that half of the bar that changed anything: the two
 * controls between it and CLR did nothing the probe could see.
 *
 * It responded from x 224 to 252 and at both y 8 and y 15, and not at x 220,
 * so the box below is that bracket rounded to the drawn icon. */
#define MAPBTN_X 222
#define MAPBTN_Y 2
#define MAPBTN_W 34
#define MAPBTN_H 16

int ds10_ui_mapbutton_hit(int x, int y)
{
    return x >= MAPBTN_X && x < MAPBTN_X + MAPBTN_W &&
           y >= MAPBTN_Y && y < MAPBTN_Y + MAPBTN_H;
}

/* And the one next to the sub-mode button, which raises the CLEAR ALL DATA
 * prompt. It responded from x 132 to 156 on both rows. Not wired: the prompt
 * is modal and would have to be drawn. */
int ds10_ui_clear_hit(int x, int y)
{
    return x >= 130 && x < 159 && y >= 2 && y < 18;
}

/* The top bar.
 *
 * The buttons are drawn by the page's own tile image, so their rectangles are
 * in the rendered page exactly - tools/probe_frames.py walks a page and
 * reports every closed frame it finds, which is where the x below come from.
 * They are all at y 3 and 11 high; the hit box is the band y 2..17, because
 * the real thing answered a tap at both y 8 and y 15 (tools/probe_buttons.py)
 * and the drawn frame is only part of what it accepts.
 *
 * Two of these were unknown until they were tapped on the hardware:
 *
 *   the triangle at x 193 is the transport. tools/probe_play.py taps it and
 *   watches for the screen to *move* rather than to differ - a running
 *   sequencer moves its cursor, and a single frame taken later can look
 *   identical to the baseline. It moves after the first tap and stops after
 *   the second.
 *
 *   the "S" changes nothing on screen at all, and tools/probe_ram.py - which
 *   diffs main RAM across a tap, masking whatever churns on its own - finds
 *   it toggling bit 1 of one global byte at 0x0208cf70 and nothing else, back
 *   and forth over four taps. What that byte means is not proven. It is taken
 *   here as SOLO for the track the page is editing, because it is on exactly
 *   those pages that edit one track and absent from every page that carries
 *   its own mute and solo strip: 51_ALL_DRUMS has one per drum row, 30_MIXER
 *   and 32_PATTERN have one per channel, and none of the three has an S.
 */
#define BAR_Y0 2
#define BAR_Y1 18
#define BAR_W 26

static const struct { const char *page; int x, w, id; } BAR_BUTTON[] = {
    /* the sequencer, both variants of it */
    {"60_SYN_SEQ_",  42, 13, DS10_BAR_MODE},
    {"60_SYN_SEQ_", 131, 26, DS10_BAR_CLR},
    {"60_SYN_SEQ_", 162, 26, DS10_BAR_SOLO},
    {"62_DRUM_SEQ",  42, 13, DS10_BAR_MODE},
    {"62_DRUM_SEQ", 131, 26, DS10_BAR_CLR},
    {"62_DRUM_SEQ", 162, 26, DS10_BAR_SOLO},
    /* the pad: three assignments, the key and scale, the rate, solo, record */
    {"71_KAOSS",      4, 16, DS10_BAR_SLOT1},
    {"71_KAOSS",     25, 16, DS10_BAR_SLOT2},
    {"71_KAOSS",     46, 16, DS10_BAR_SLOT3},
    {"71_KAOSS",     67, 26, DS10_BAR_SET},
    {"71_KAOSS",     98, 26, DS10_BAR_RATE},
    {"71_KAOSS",    131, 26, DS10_BAR_SOLO},
    {"71_KAOSS",    162, 26, DS10_BAR_REC},
    /* the keyboard */
    {"81_KEY",        4, 27, DS10_BAR_OCT_DOWN},
    {"81_KEY",       35, 27, DS10_BAR_OCT_UP},
    {"81_KEY",       66, 60, DS10_BAR_SYNTH},
    {"81_KEY",      131, 26, DS10_BAR_SOLO},
    {"81_KEY",      162, 26, DS10_BAR_REC},
    /* the two synth editors */
    {"61_SYN_EDIT", 162, 26, DS10_BAR_SOLO},
    {"70_PATCH",    162, 26, DS10_BAR_SOLO},
    /* the drums */
    {"51_ALL_DRUMS",    162, 26, DS10_BAR_CLR},
    {"33_DRUM_SELECTER",  4, 27, DS10_BAR_EDIT},
    {"33_DRUM_SELECTER", 162, 26, DS10_BAR_REC},
    /* the pattern picker, and the song */
    /* COPY and LOCK are in 32_PATTERN's constructor table at 0x020845dc,
     * 28 by 14 each. */
    {"32_PATTERN",    4, 28, DS10_BAR_COPY},
    {"32_PATTERN",   35, 28, DS10_BAR_LOCK},
    {"31_SONG",     162, 26, DS10_BAR_CLR},
};

/* Which pages have the transport. The four drum edit pages have only the
 * screen swap. */
static const char *const BAR_NO_PLAY[] = {
    "53_DRUMEDIT", "54_DRUM_FX", "55_DRUMEDITMAP", "63_DRUM_EDIT_PATCH",
};

#define BAR_PLAY_X 193
#define BAR_SWAP_X 224

static int has_play(const char *page)
{
    size_t i;

    for (i = 0; i < sizeof BAR_NO_PLAY / sizeof BAR_NO_PLAY[0]; i++)
        if (strcmp(page, BAR_NO_PLAY[i]) == 0) return 0;
    return 1;
}

int ds10_ui_bar_hit(const char *page, int x, int y)
{
    size_t i;

    if (!page || y < BAR_Y0 || y >= BAR_Y1) return DS10_BAR_NONE;
    /* The swap is on every page including the map, which is how a screen that
     * has been brought down gets sent back up. */
    if (x >= BAR_SWAP_X) return DS10_BAR_SWAP;
    if (strcmp(page, "90_MAP") == 0) return DS10_BAR_NONE;
    if (has_play(page) && x >= BAR_PLAY_X && x < BAR_PLAY_X + BAR_W)
        return DS10_BAR_PLAY;
    for (i = 0; i < sizeof BAR_BUTTON / sizeof BAR_BUTTON[0]; i++) {
        const char *p = BAR_BUTTON[i].page;

        if (strncmp(page, p, strlen(p)) != 0) continue;
        if (x >= BAR_BUTTON[i].x && x < BAR_BUTTON[i].x + BAR_BUTTON[i].w)
            return BAR_BUTTON[i].id;
    }
    return DS10_BAR_NONE;
}

int ds10_ui_bar_box(const char *page, int id, int *x, int *y, int *h)
{
    size_t i;

    if (!page) return -1;
    *y = 3;
    *h = 11;
    if (id == DS10_BAR_SWAP) { *x = BAR_SWAP_X; return 0; }
    if (id == DS10_BAR_PLAY && has_play(page)) { *x = BAR_PLAY_X; return 0; }
    for (i = 0; i < sizeof BAR_BUTTON / sizeof BAR_BUTTON[0]; i++) {
        const char *p = BAR_BUTTON[i].page;

        if (BAR_BUTTON[i].id != id) continue;
        if (strncmp(page, p, strlen(p)) != 0) continue;
        *x = BAR_BUTTON[i].x;
        return 0;
    }
    return -1;
}

/* A button's indicator.
 *
 * Every button on the bar and on the FX page carries a small dot at its left,
 * and the pages that come up with something already chosen - LOCK on
 * 32_PATTERN, DELAY on 52_FX - draw that dot and the frame around it in red.
 * So lighting the dot is what the original does to say "this one is on", and
 * a four-pixel block at the button's origin plus (3,3) lands on it.
 *
 * It goes on the 3D plane rather than into OAM because the plane is a plain
 * framebuffer under the sprites: nothing has to be allocated a slot, and the
 * page's own sprites still draw over it.
 */
#define LED_DX 4
#define LED_SIZE 4
/* The plane holds the renderer's own 18-bit colour, which is red in the low
 * byte - r | g << 8 | b << 16, see ds10_col15to18 - with the alpha above it.
 * Writing 0xRRGGBB here gets you the blue you did not ask for. */
#define LED_COLOR 0x1F00003Fu

void ds10_ui_led(ds10_ui *u, int bx, int by, int bh)
{
    int x, y, dy = (bh - LED_SIZE) / 2;

    for (y = 0; y < LED_SIZE; y++)
        for (x = 0; x < LED_SIZE; x++) {
            int px = bx + LED_DX + x, py = by + dy + y;

            if (px < 0 || px >= DS10_SCR_W || py < 0 || py >= DS10_SCR_H)
                continue;
            u->gpu.plane3d[py * DS10_SCR_W + px] = LED_COLOR;
        }
}

/* Putting one out is not the reverse of lighting it: a lit dot on these pages
 * is a red glow several pixels across, not the four the core covers, so this
 * paints the whole of that corner in the interior grey. The bevelled dot the
 * page draws there is lost, and a flat dark patch reads as off, which is what
 * it is. */
void ds10_ui_led_off(ds10_ui *u, int bx, int by, int bh)
{
    int x, y;

    for (y = 2; y < bh - 2; y++)
        for (x = 2; x < LED_DX + LED_SIZE + 1; x++) {
            int px = bx + x, py = by + y;

            if (px < 0 || px >= DS10_SCR_W || py < 0 || py >= DS10_SCR_H)
                continue;
            u->gpu.plane3d[py * DS10_SCR_W + px] = 0x1F181818u;
        }
}

/* A frame round a button, for the pages that show which one of a group is
 * chosen. The page's own image already has one of them lit, so a dot alone
 * would be two marks saying different things. */
void ds10_ui_outline(ds10_ui *u, int bx, int by, int bw, int bh)
{
    int x, y;

    for (x = 0; x < bw; x++)
        for (y = 0; y < bh; y += bh - 1) {
            int px = bx + x, py = by + y;

            if (px >= 0 && px < DS10_SCR_W && py >= 0 && py < DS10_SCR_H)
                u->gpu.plane3d[py * DS10_SCR_W + px] = LED_COLOR;
        }
    for (y = 0; y < bh; y++)
        for (x = 0; x < bw; x += bw - 1) {
            int px = bx + x, py = by + y;

            if (px >= 0 && px < DS10_SCR_W && py >= 0 && py < DS10_SCR_H)
                u->gpu.plane3d[py * DS10_SCR_W + px] = LED_COLOR;
        }
}

void ds10_ui_plane_clear(ds10_ui *u)
{
    memset(u->gpu.plane3d, 0, sizeof u->gpu.plane3d);
}

/* The sequencer's scrollbar.
 *
 * The constructor's 0x138 is the scrollable height - twenty-four rows against
 * the twelve on screen - so the handle has thirteen positions. Everything else
 * here is read off the rendered page, pixel by pixel, because the page draws
 * both the groove and the handle itself: the groove is the four columns at
 * x 8..12 running from y 34 to 186, and the handle is the eleven rows at
 * y 97..107, x 5..15, with a two-pixel shadow below and to the right.
 *
 * Which means the handle cannot just be drawn where it belongs - the one the
 * page drew has to be painted out first, or there are two of them. So this
 * repaints the whole groove and then puts the handle back at the position it
 * is actually at. The greys are the page's own, converted from what it
 * renders to the six bits a colour component has.
 */
#define SCROLL_X0 3
#define SCROLL_X1 20
#define SCROLL_Y0 SEQ_Y0
#define SCROLL_H 11
#define SCROLL_BOTTOM 186
#define SCROLL_TRAVEL (SCROLL_BOTTOM - SCROLL_Y0 - SCROLL_H)
#define SCROLL_POSITIONS (SEQ_RANGE - SEQ_ROWS)

/* Where the window sits in the range, and back.
 *
 * The page draws the handle at y 97, which is five twelfths of the way down -
 * so the top note that frame shows, A4, is scroll position five, and the
 * twenty-four-row range runs from D5 down to D#3. Only that one handle
 * position is evidence; the two ends are what it implies.
 */
#define SCROLL_DEFAULT 5

int ds10_ui_seq_scroll_top(int pos)
{
    if (pos < 0) pos = 0;
    if (pos > SCROLL_POSITIONS) pos = SCROLL_POSITIONS;
    return SEQ_TOP_DEFAULT + SCROLL_DEFAULT - pos;
}

int ds10_ui_seq_scroll_pos(int top)
{
    int pos = SEQ_TOP_DEFAULT + SCROLL_DEFAULT - top;

    if (pos < 0) pos = 0;
    if (pos > SCROLL_POSITIONS) pos = SCROLL_POSITIONS;
    return pos;
}

int ds10_ui_seq_scroll_hit(int x, int y)
{
    int pos;

    if (x < SCROLL_X0 || x >= SCROLL_X1) return -1;
    if (y < SCROLL_Y0 || y > SCROLL_BOTTOM) return -1;
    /* The finger is the middle of the handle. */
    pos = (y - SCROLL_Y0 - SCROLL_H / 2) * SCROLL_POSITIONS / SCROLL_TRAVEL;
    if (pos < 0) pos = 0;
    if (pos > SCROLL_POSITIONS) pos = SCROLL_POSITIONS;
    return pos;
}

/* The handle, as the page draws it: eleven rows over x 5..15, then its
 * shadow. Six-bit greys - 22, 36, 50 and 58 - and 0 for the shadow. */
#define HANDLE_W 12
#define HANDLE_X 5
static const uint32_t SCROLL_GREY[5] = {
    0x1F000000u, 0x1F161616u, 0x1F242424u, 0x1F323232u, 0x1F3A3A3Au,
};
static const char HANDLE_PIX[SCROLL_H][HANDLE_W + 1] = {
    "bdddddddddb0",
    "cccccccccccc",
    "ceeeeeeeee c",
    "ceeeeeeeee c",
    "cddddddddd c",
    "bbbbbbbbbbb0",
    "cddddddddd c",
    "ceeeeeeeee c",
    "ceeeeeeeee c",
    "cccccccccc c",
    "bdddddddddb0",
};

void ds10_ui_seq_scroll_draw(ds10_ui *u, int pos)
{
    int x, y, y0;

    if (pos < 0) pos = 0;
    if (pos > SCROLL_POSITIONS) pos = SCROLL_POSITIONS;
    /* The empty groove, over the whole run of it, which is what paints out
     * the handle the page drew. */
    for (y = SCROLL_Y0 - 1; y <= SCROLL_BOTTOM; y++)
        for (x = SCROLL_X0; x < SCROLL_X1 - 2; x++) {
            uint32_t c = 0x1F060606u;

            if (x == 8) c = 0x1F040404u;
            else if (x >= 9 && x <= 11) c = 0x1F000000u;
            else if (x == 12) c = 0x1F0E0E0Eu;
            u->gpu.plane3d[y * DS10_SCR_W + x] = c;
        }
    y0 = SCROLL_Y0 + pos * SCROLL_TRAVEL / SCROLL_POSITIONS;
    for (y = 0; y < SCROLL_H; y++)
        for (x = 0; x < HANDLE_W; x++) {
            int px = HANDLE_X + x, py = y0 + y;
            char c = HANDLE_PIX[y][x];

            if (c == ' ') continue;
            if (py < 0 || py >= DS10_SCR_H) continue;
            u->gpu.plane3d[py * DS10_SCR_W + px] =
                c == '0' ? 0x1F000000u : SCROLL_GREY[c - 'a'];
        }
}

/* The strip of piano keys between the scrollbar and the grid, one per row. */
#define SEQ_KEY_X0 22
#define SEQ_KEY_X1 39

int ds10_ui_seq_key_hit(int x, int y)
{
    int row;

    if (x < SEQ_KEY_X0 || x >= SEQ_KEY_X1) return -1;
    if (y < SEQ_Y0) return -1;
    row = (y - SEQ_Y0) / SEQ_CELL;
    return row < SEQ_ROWS ? row : -1;
}

/* 52_FX's buttons, out of the ROM's own tables rather than measured: nine
 * entries of 36 bytes at 0x020869ac, each x, y, w, h, the label's position,
 * two index bytes, the caption and a group id. 54_DRUM_FX has the same table
 * for its three type buttons at 0x02086808, shifted right because that page
 * has no routing column.
 *
 * The index within a group is the value the mixer knob takes - route 0..5 in
 * knob 0, type 0..2 in knob 1 - which is what the audio side found on its own
 * from the songs.
 */
static const struct { int x, y, w, h, group, index; } FX_BUTTON[] = {
    { 11,  47, 66, 14, 0, 0},   /* DELAY */
    { 51,  78, 66, 14, 0, 1},   /* FLANGER */
    { 11, 109, 66, 14, 0, 2},   /* CHORUS */
    {139,  38, 66, 14, 1, 0},   /* SYN 1 */
    {179,  54, 66, 14, 1, 1},   /* SYN 2 */
    {139,  70, 66, 14, 1, 2},   /* SYN 1 + 2 */
    {179,  86, 66, 14, 1, 3},   /* DRUMS */
    {139, 102, 66, 14, 1, 4},   /* ALL */
    {179, 118, 66, 14, 1, 5},   /* OFF */
};
/* 54_DRUM_FX's three, from 0x02086808. Same rows, 64 pixels further right. */
#define FX_DRUM_DX 64
#define FX_DRUM_TYPES 3

static int fx_drum_page(const char *page)
{
    return page && strcmp(page, "54_DRUM_FX") == 0;
}

int ds10_ui_fx_button_hit(const char *page, int x, int y, int *group)
{
    size_t i, n = sizeof FX_BUTTON / sizeof FX_BUTTON[0];
    int dx = 0;

    if (fx_drum_page(page)) { n = FX_DRUM_TYPES; dx = FX_DRUM_DX; }
    for (i = 0; i < n; i++) {
        int bx = FX_BUTTON[i].x + dx;

        if (x < bx || x >= bx + FX_BUTTON[i].w) continue;
        if (y < FX_BUTTON[i].y || y >= FX_BUTTON[i].y + FX_BUTTON[i].h)
            continue;
        *group = FX_BUTTON[i].group;
        return FX_BUTTON[i].index;
    }
    return -1;
}

int ds10_ui_fx_button_box(const char *page, int group, int index,
                          int *x, int *y, int *h)
{
    size_t i, n = sizeof FX_BUTTON / sizeof FX_BUTTON[0];
    int dx = 0;

    if (fx_drum_page(page)) { n = FX_DRUM_TYPES; dx = FX_DRUM_DX; }
    for (i = 0; i < n; i++)
        if (FX_BUTTON[i].group == group && FX_BUTTON[i].index == index) {
            *x = FX_BUTTON[i].x + dx;
            *y = FX_BUTTON[i].y;
            *h = FX_BUTTON[i].h;
            return FX_BUTTON[i].w;
        }
    return 0;
}

/* The mute and solo strips.
 *
 * Three pages carry one. The mixer's and the pattern picker's are six pairs at
 * the channel pitch the faders already use, 43 pixels; 51_ALL_DRUMS puts its
 * four beside the grid rows, at the grid's own 13-pixel pitch. All three are
 * framed by the page image, so tools/probe_frames.py gives the rectangles.
 *
 * Mute and solo are not in the song file. The original keeps them in the
 * sequencer's per-track block - `+0x22` of the 36-byte record at seq + 0x64 +
 * track*0x24, 0 to play and 1 to mute - so they are a property of the running
 * transport, not of the document, and nothing about them is saved.
 */
static const struct {
    const char *page;
    int x, y, w, h, step, rows, solo_dx, solo_dy;
} MS_STRIP[] = {
    {"30_MIXER",       3,  62, 34, 9, 43, 6, 0, 11},
    /* 32_PATTERN's is its constructor's, not a measurement: twelve widgets
     * per row named tm%d and ts%d at x = 3 + 43*n, y 160 and 171, 40 by 10.
     * Twelve because the PLUS has a second deck; six are on screen. Which is
     * also why the mute and solo pass at 0x0206724c splits on bit 4 of the
     * widget index - the two rows are numbered n and n + 16. */
    {"32_PATTERN",     3, 160, 40, 10, 43, 6, 0, 11},
    {"51_ALL_DRUMS",   3,  34, 17, 11, 0, 4, 19, 0},
};

int ds10_ui_ms_hit(const char *page, int x, int y, int *track)
{
    size_t i;
    int t;

    if (!page) return -1;
    for (i = 0; i < sizeof MS_STRIP / sizeof MS_STRIP[0]; i++) {
        int solo;

        if (strcmp(page, MS_STRIP[i].page) != 0) continue;
        for (t = 0; t < MS_STRIP[i].rows; t++)
            for (solo = 0; solo < 2; solo++) {
                /* 51_ALL_DRUMS stacks the rows down the grid and puts solo
                 * beside mute; the other two put solo under mute. */
                int bx = MS_STRIP[i].x + t * MS_STRIP[i].step
                       + solo * MS_STRIP[i].solo_dx;
                int by = MS_STRIP[i].y + solo * MS_STRIP[i].solo_dy
                       + (MS_STRIP[i].step ? 0 : t * SEQ_CELL);

                if (x < bx || x >= bx + MS_STRIP[i].w) continue;
                if (y < by || y >= by + MS_STRIP[i].h) continue;
                *track = t;
                return solo;
            }
    }
    return -1;
}

int ds10_ui_ms_box(const char *page, int track, int solo, int *x, int *y,
                   int *h)
{
    size_t i;

    if (!page) return -1;
    for (i = 0; i < sizeof MS_STRIP / sizeof MS_STRIP[0]; i++) {
        if (strcmp(page, MS_STRIP[i].page) != 0) continue;
        if (track < 0 || track >= MS_STRIP[i].rows) return -1;
        *x = MS_STRIP[i].x + track * MS_STRIP[i].step
           + solo * MS_STRIP[i].solo_dx;
        *y = MS_STRIP[i].y + solo * MS_STRIP[i].solo_dy
           + (MS_STRIP[i].step ? 0 : track * SEQ_CELL);
        *h = MS_STRIP[i].h;
        return 0;
    }
    return -1;
}

/* The pad's cursor. The original draws crosshairs across the whole pad; this
 * draws the same, on the 3D plane, so a touch is visible on the screen it
 * happened on rather than only in whatever the two knobs do. */
#define KAOSS_COLOR 0x1F2F3F1Fu   /* r 0x1F, g 0x3F, b 0x2F - see LED_COLOR */

void ds10_ui_kaoss_mark(ds10_ui *u, int vx, int vy)
{
    int x, y, cx, cy;

    if (vx < 0 || vy < 0) return;
    cx = KAOSS_X + vx * (KAOSS_W - 1) / 127;
    cy = KAOSS_Y + (127 - vy) * (KAOSS_H - 1) / 127;
    for (x = KAOSS_X; x < KAOSS_X + KAOSS_W; x++)
        u->gpu.plane3d[cy * DS10_SCR_W + x] = KAOSS_COLOR;
    for (y = KAOSS_Y; y < KAOSS_Y + KAOSS_H; y++)
        u->gpu.plane3d[y * DS10_SCR_W + cx] = KAOSS_COLOR;
}

/* The sequencer's sub-mode button.
 *
 * This one is not in any table. The screens that place widgets through the
 * shared helper have their rectangles in a table the constructor walks; this
 * page does not call it, and its update has no coordinate compares either, so
 * the button was found by tapping the real thing - tools/probe_buttons.py -
 * and its edges by tools/probe_edge.py, which boots once per candidate point
 * so each tap is independent.
 *
 * It steps a six-position cycle. The two positions that show the KAOSS page
 * are the pattern's two automation lanes, which is why the song format
 * carries exactly two.
 */
#define SEQ_MODE_X 42
#define SEQ_MODE_Y 2
#define SEQ_MODE_W 17
#define SEQ_MODE_H 14

static const char *const SEQ_MODE_PAGE[] = {
    "60_SYN_SEQ_PIANO", "60_SYN_SEQ_GATE", "60_SYN_SEQ_VOL",
    "60_SYN_SEQ_PAN", "60_SYN_SEQ_KAOSS", "60_SYN_SEQ_KAOSS",
};

int ds10_ui_seq_modes(void)
{
    return (int)(sizeof SEQ_MODE_PAGE / sizeof SEQ_MODE_PAGE[0]);
}

const char *ds10_ui_seq_mode_page(int mode)
{
    if (mode < 0 || mode >= ds10_ui_seq_modes()) mode = 0;
    return SEQ_MODE_PAGE[mode];
}

int ds10_ui_seq_mode_hit(int x, int y)
{
    return x >= SEQ_MODE_X && x < SEQ_MODE_X + SEQ_MODE_W &&
           y >= SEQ_MODE_Y && y < SEQ_MODE_Y + SEQ_MODE_H;
}

int ds10_ui_seq_hit(int x, int y, int *step, int *row)
{
    int c = (x - SEQ_X0) / SEQ_CELL, r = (y - SEQ_Y0) / SEQ_CELL;

    if (x < SEQ_X0 || y < SEQ_Y0 || c >= SEQ_COLS || r >= SEQ_ROWS) return -1;
    *step = c;
    *row = r;
    return 0;
}

/* The step grids, drawn from the pattern instead of replayed from a capture.
 *
 * The marker is cell 7 of the page's own sheet. That is what the captured
 * frame drew - six of them, and every one lands exactly on this grid, which
 * is the check that the origin and the cell size above are right.
 *
 * The row is the note's distance below the top of the window, so row 0 is the
 * highest. What fixes the direction and the pitch is the keyboard down the
 * left of that frame: reading its whites and blacks off gives an octave with
 * A4 on the top row, and the game labels row nine "C4" with two font sprites
 * of its own. Hence note = top - row, with the captured window's top at 69.
 *
 * The scrollable range is the constructor's 0x138, twenty-four rows against
 * the twelve on screen, so the window slides over two octaves. Which two the
 * range ends on is not something the capture shows; C3 to B4 is assumed, and
 * only the default scroll position is evidence.
 */
#define SEQ_MARK_CELL 7
#define SEQ_MARK_BASE 0x200

int ds10_ui_seq_top_default(void) { return SEQ_TOP_DEFAULT; }

int ds10_ui_seq_row_note(int top, int row) { return top - row; }

void ds10_ui_seq_grid(ds10_ui *u, const char *page, const int *note,
                      int nsteps, int top, int cursor)
{
    int slot = u->free_slot[0], i;
    char bank[32];

    snprintf(bank, sizeof bank, "%s_c", page);
    if (nsteps > SEQ_COLS) nsteps = SEQ_COLS;
    for (i = 0; i < nsteps; i++) {
        int row = top - note[i];

        if (note[i] < 0 || row < 0 || row >= SEQ_ROWS) continue;
        put_cell(u, 0, &slot, bank, SEQ_MARK_CELL, SEQ_X0 + i * SEQ_CELL,
                 SEQ_Y0 + row * SEQ_CELL, 2, SEQ_MARK_BASE);
    }
    /* The cursor above the grid. The captured frame has one sprite there, at
     * the first column, and nothing else sits in that band - but the frame's
     * transport was stopped, so that it follows the step is inference. */
    if (cursor >= 0 && cursor < SEQ_COLS)
        put_cell(u, 0, &slot, "CHARACTER_c", SEQ_CURSOR_CELL,
                 SEQ_X0 + 1 + cursor * SEQ_CELL, SEQ_CURSOR_Y, 1,
                 SEQ_MARK_BASE);
    park_from(u, 0, slot);
}

/* The value sub-modes.
 *
 * They draw the same cell 7 on the same grid as the piano roll, one marker
 * per step, at the row for that step's value. What differs is the row count
 * and the mapping, and each one is pinned by the data rather than guessed:
 *
 * GATE has five rows - LEGATO, 100%, 75%, 50%, 25% - so row = 5 - gate. The
 * gate byte runs 1 to 6, and the sixth is retrigger, which song.h already had
 * as distinct from the four lengths and the tie; it has no row of its own and
 * draws on LEGATO.
 *
 * VOLUME has twelve, MAX at the top. Every velocity on every step of every
 * song in the ROM is one of exactly twelve values, and those values are
 * (11 - row) * 127 / 11 in integer division - 0, 11, 23, 34, 46, 57, 69, 80,
 * 92, 103, 115, 127. Twelve rows and that rounding reproduce the set exactly.
 *
 * PAN has five, L2 L1 C R1 R2, at row * 128 / 4 clamped: 0, 32, 64, 96, 127.
 *
 * The check on all three is that the blank template's defaults land on a row
 * rather than between two: gate 3 is the 75% row, velocity 127 is MAX, and
 * pan 64 is C. A mapping that put the untouched value between rows would be
 * the wrong one.
 */
static const struct { int rows; } SEQ_MODE[] = {
    {SEQ_ROWS}, {5}, {12}, {5}, {12}, {12},
};

int ds10_ui_seq_mode_rows(int mode)
{
    if (mode < 0 || mode >= ds10_ui_seq_modes()) mode = 0;
    return SEQ_MODE[mode].rows;
}

int ds10_ui_seq_row_value(int mode, int row)
{
    int n = ds10_ui_seq_mode_rows(mode);

    if (row < 0 || row >= n) return -1;
    switch (mode) {
    case 1: return 5 - row;                        /* gate */
    case 3: return row * 128 / 4 > 127 ? 127 : row * 128 / 4;   /* pan */
    default: return (n - 1 - row) * 127 / (n - 1); /* velocity, lanes */
    }
}

int ds10_ui_seq_value_row(int mode, int value)
{
    int n = ds10_ui_seq_mode_rows(mode), r, best = -1, bestd = 1 << 30;

    if (value < 0) return -1;
    /* The nearest row, so a value the editor did not write - a song made on
     * hardware, or the retrigger gate - still shows somewhere sensible. */
    for (r = 0; r < n; r++) {
        int d = ds10_ui_seq_row_value(mode, r) - value;

        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = r; }
    }
    return best;
}

void ds10_ui_seq_values(ds10_ui *u, const int *value, int nsteps, int mode,
                        int cursor)
{
    int slot = u->free_slot[0], i;

    if (nsteps > SEQ_COLS) nsteps = SEQ_COLS;
    for (i = 0; i < nsteps; i++) {
        int row = ds10_ui_seq_value_row(mode, value[i]);

        if (row < 0) continue;
        put_cell(u, 0, &slot, "60_SYN_SEQ_PIANO_c", SEQ_MARK_CELL,
                 SEQ_X0 + i * SEQ_CELL, SEQ_Y0 + row * SEQ_CELL, 2,
                 SEQ_MARK_BASE);
    }
    if (cursor >= 0 && cursor < SEQ_COLS)
        put_cell(u, 0, &slot, "CHARACTER_c", SEQ_CURSOR_CELL,
                 SEQ_X0 + 1 + cursor * SEQ_CELL, SEQ_CURSOR_Y, 1,
                 SEQ_MARK_BASE);
    park_from(u, 0, slot);
}

/* The drum grid.
 *
 * These markers are not sprites. 51_ALL_DRUMS turns the 3D engine on and
 * draws them as flat quads, which the compositor takes as BG0 - the captured
 * frame has one live sprite on the whole screen and 660 opaque pixels on the
 * 3D plane, at exactly this grid. The reason is capacity: sixteen steps by
 * four drums is sixty-four markers, and OAM holds 128 entries in total. The
 * piano roll needs at most one marker per step, so it uses sprites.
 *
 * Every marker is the same 11 by 10 block and the same in every cell, so
 * putting the block straight into the plane is exact without any of the 3D
 * pipeline. Its four colours are 6-bit per channel, as the plane is: a light
 * face, a lighter top and bottom edge, a dark left edge and a black shadow
 * down the right and along the bottom.
 */
#define MARK_W 11
#define MARK_H 10
static const uint32_t MARK_COLOR[4] = {
    0x1F000000u, 0x1F171717u, 0x1F333333u, 0x1F3B3B3Bu,
};
static const char MARK_PIX[MARK_H][MARK_W + 1] = {
    "bddddddddba",
    "bccccccccba",
    "bccccccccba",
    "bccccccccba",
    "bccccccccba",
    "bccccccccba",
    "bccccccccba",
    "bddddddddba",
    "bbbbbbbbbba",
    "aaaaaaaaaaa",
};

static void mark_3d(ds10_ui *u, int x0, int y0)
{
    int x, y;

    for (y = 0; y < MARK_H; y++)
        for (x = 0; x < MARK_W; x++) {
            int px = x0 + x, py = y0 + y;

            if (px < 0 || px >= DS10_SCR_W || py < 0 || py >= DS10_SCR_H)
                continue;
            u->gpu.plane3d[py * DS10_SCR_W + px] =
                MARK_COLOR[MARK_PIX[y][x] - 'a'];
        }
}

void ds10_ui_drum_grid(ds10_ui *u, const char *page, const uint8_t *on,
                       int nrows, int nsteps, int cursor)
{
    int slot = u->free_slot[0], r, i;

    (void)page;
    memset(u->gpu.plane3d, 0, sizeof u->gpu.plane3d);
    if (nsteps > SEQ_COLS) nsteps = SEQ_COLS;
    if (nrows > DSEQ_ROWS) nrows = DSEQ_ROWS;
    for (r = 0; r < nrows; r++)
        for (i = 0; i < nsteps; i++) {
            if (!on[r * SEQ_COLS + i]) continue;
            mark_3d(u, SEQ_X0 + i * SEQ_CELL, SEQ_Y0 + r * SEQ_CELL);
        }
    /* The cursor above the grid is a sprite here as it is on the piano
     * roll - the one live entry the captured frame has. */
    if (cursor >= 0 && cursor < SEQ_COLS)
        put_cell(u, 0, &slot, "CHARACTER_c", SEQ_CURSOR_CELL,
                 SEQ_X0 + 1 + cursor * SEQ_CELL, SEQ_CURSOR_Y, 1,
                 SEQ_MARK_BASE);
    park_from(u, 0, slot);
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
