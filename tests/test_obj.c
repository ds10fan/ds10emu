/* Differential test: our sprite widget against the original's.
 *
 * Vectors from `python3 tools/oracle.py obj-vectors`. For each case the
 * generator calls the game's own setter (0x0202a820) under Unicorn and records
 * both the object it leaves behind and the OAM entries it built. We run the
 * same case and compare word for word.
 *
 * The resource lookup the setter does is stubbed in the generator, so the tile
 * and palette it would have found are handed to us as res_tile/res_pal; the
 * rule for when they are used is ours to get right and is checked here too.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/app/obj.h"

/* Must match OBJ_CASES in tools/oracle.py. */
static const int CASES[][15] = {
    {4, 0, 0, 0x16, 3, 2, 8, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0x21A, 3, 0, 0, 0, 0, 0, 1},
    {8, 1, 2, 200, 150, 3, 12, 0x2FF, 15, 7, 1, 1, 0x100, 0x4000, 1},
    {16, 0, 0, -20, 191, 1, 5, 0x100, 5, 31, 3, 0, 0x300, 0x8000, 0},
    {2, 3, 1, 255, 255, 2, 1, 0x3FF, 8, 3, 3, 1, 0x100, 0xC000, 1},
    {3, 0, 0, 100, 64, 0, 43, 0, 0, 0, 2, 0, 0, 0x4000, 0},
    {5, 2, 0, -8, 8, 2, 16, 0x200, 1, 0, 0, 1, 0x300, 0, 1},
    {12, 0, 1, 74, 3, 3, 9, 0x21B, 2, 12, 1, 0, 0, 0x8000, 1},
};

/* The generator hands the resource lookup this packed {palette, tile}. */
#define RES_PACKED 0x301A

static FILE *g;

static void rd(void *p, size_t n)
{
    if (fread(p, 1, n, g) != n) {
        fprintf(stderr, "unexpected end of vector file\n");
        exit(2);
    }
}

static uint32_t rd32(void)
{
    unsigned char b[4];

    rd(b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/obj.bin";
    char magic[4];
    uint32_t n, i;
    unsigned long total = 0, bad = 0;

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py obj-vectors\n", path);
        return 2;
    }
    rd(magic, 4);
    if (memcmp(magic, "OBJ1", 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    n = rd32();
    if (n != sizeof CASES / sizeof CASES[0]) {
        fprintf(stderr, "%s: %u cases, expected %u - regenerate\n", path, n,
                (unsigned)(sizeof CASES / sizeof CASES[0]));
        return 2;
    }

    for (i = 0; i < n; i++) {
        int32_t want[16];
        uint32_t oamlen, k;
        uint8_t oam[DS10_OBJ_MAX * 8];
        ds10_obj o;
        const int *c = CASES[i];
        static const char *FIELD[16] = {
            "x", "y", "priority", "tile", "palette", "affine", "mode",
            "mosaic", "rot", "shape", "big", "oam", "count", "res", "xstep",
            "bank",
        };
        const int32_t *mine[16];

        for (k = 0; k < 16; k++) want[k] = (int32_t)rd32();
        oamlen = rd32();
        if (oamlen > sizeof oam) {
            fprintf(stderr, "case %u: %u bytes of OAM, too much\n", i, oamlen);
            return 2;
        }
        rd(oam, oamlen);

        memset(&o, 0, sizeof o);
        ds10_obj_set(&o, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8],
                     c[9], c[10], c[11], c[12], c[13], c[14],
                     RES_PACKED & 0x3FF, (RES_PACKED & 0xF000) >> 12);

        mine[0] = &o.x;
        mine[1] = &o.y;
        mine[2] = &o.priority;
        mine[3] = &o.tile;
        mine[4] = &o.palette;
        mine[5] = &o.affine;
        mine[6] = &o.mode;
        mine[7] = &o.mosaic;
        mine[8] = &o.rot;
        mine[9] = &o.shape;
        mine[10] = &o.big;
        mine[11] = NULL; /* the original's OAM pointer; ours is inline */
        mine[12] = &o.count;
        mine[13] = &o.res;
        mine[14] = &o.xstep;
        mine[15] = &o.bank;

        for (k = 0; k < 16; k++) {
            if (!mine[k]) continue;
            total++;
            if (*mine[k] != want[k]) {
                fprintf(stderr, "case %u: %s = %d, want %d\n", i, FIELD[k],
                        *mine[k], want[k]);
                bad++;
            }
        }
        for (k = 0; k < oamlen; k += 8) {
            total++;
            /* The last two bytes of an entry are the affine slot, which this
             * path does not touch and the original leaves as it found. */
            if (memcmp(o.oam + k, oam + k, 6) != 0) {
                fprintf(stderr, "case %u sprite %u: got %02x%02x%02x%02x%02x%02x"
                        " want %02x%02x%02x%02x%02x%02x\n", i, k / 8,
                        o.oam[k], o.oam[k + 1], o.oam[k + 2], o.oam[k + 3],
                        o.oam[k + 4], o.oam[k + 5],
                        oam[k], oam[k + 1], oam[k + 2], oam[k + 3],
                        oam[k + 4], oam[k + 5]);
                bad++;
            }
        }
    }
    fclose(g);
    printf("%s: %lu checks over %u widgets, %lu mismatches\n",
           bad ? "FAIL" : "PASS", total, n, bad);
    return bad ? 1 : 0;
}
