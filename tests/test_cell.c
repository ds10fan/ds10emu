/* Differential test: our NCER cell expander against the original's.
 *
 * Vectors from `python3 tools/oracle.py cell-vectors`. Each case is a cell,
 * a 2x2 matrix, a translation and an affine slot, run through the game's own
 * 0x02069564 under Unicorn; what is recorded is the OAM it produced. This is
 * the function the whole interface draws through, so its rounding is worth
 * having exactly right rather than approximately.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/app/cell.h"

#define MAXOBJ 16

static FILE *g;

static void rd(void *p, size_t n)
{
    if (fread(p, 1, n, g) != n) {
        fprintf(stderr, "unexpected end of vector file\n");
        exit(2);
    }
}

static uint16_t rd16(void)
{
    unsigned char b[2];

    rd(b, 2);
    return (uint16_t)(b[0] | (b[1] << 8));
}

static int32_t rd32(void)
{
    unsigned char b[4];

    rd(b, 4);
    return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                     ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/cell.bin";
    char magic[4];
    uint32_t ncases, i;
    unsigned long total = 0, bad = 0;

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py cell-vectors\n", path);
        return 2;
    }
    rd(magic, 4);
    if (memcmp(magic, "CEL1", 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = (uint32_t)rd32();

    for (i = 0; i < ncases; i++) {
        uint8_t objs[MAXOBJ * 6], want[MAXOBJ * 8], got[MAXOBJ * 8];
        int32_t mtx[4], pos[2];
        int hasmtx, haspos;
        uint16_t nobj, affine, dbl;
        uint32_t n, k;
        ds10_cell_ref c;
        int mine;

        nobj = rd16();
        if (nobj > MAXOBJ) {
            fprintf(stderr, "case %u: %u objects, too many\n", i, nobj);
            return 2;
        }
        rd(objs, (size_t)nobj * 6);
        hasmtx = rd32();
        for (k = 0; k < 4; k++) mtx[k] = rd32();
        haspos = rd32();
        for (k = 0; k < 2; k++) pos[k] = rd32();
        affine = rd16();
        dbl = rd16();
        n = (uint32_t)rd32();
        rd(want, (size_t)(n ? n : 1) * 8);

        c.nobj = nobj;
        c.obj = objs;
        memset(got, 0, sizeof got);
        mine = ds10_cell_to_oam(got, MAXOBJ, &c, hasmtx ? mtx : NULL,
                                haspos ? pos : NULL, affine, dbl);

        total++;
        if ((uint32_t)mine != n) {
            fprintf(stderr, "case %u: produced %d entries, want %u\n", i, mine,
                    n);
            bad++;
            continue;
        }
        for (k = 0; k < n; k++) {
            total++;
            if (memcmp(got + k * 8, want + k * 8, 6) != 0) {
                fprintf(stderr, "case %u entry %u: got %02x%02x %02x%02x "
                        "%02x%02x, want %02x%02x %02x%02x %02x%02x\n", i, k,
                        got[k * 8 + 1], got[k * 8], got[k * 8 + 3],
                        got[k * 8 + 2], got[k * 8 + 5], got[k * 8 + 4],
                        want[k * 8 + 1], want[k * 8], want[k * 8 + 3],
                        want[k * 8 + 2], want[k * 8 + 5], want[k * 8 + 4]);
                bad++;
            }
        }
    }
    fclose(g);
    printf("%s: %lu checks over %u cells, %lu mismatches\n",
           bad ? "FAIL" : "PASS", total, ncases, bad);
    return bad ? 1 : 0;
}
