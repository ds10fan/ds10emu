/* Differential test: the sprite affine matrix against the original's,
 * 0x02022ba4 and the two helpers it calls.
 *
 * Every rot/scale sprite in the interface is drawn through this - the knob
 * pointers are the visible ones. It used to be cos() and sin() here, which
 * agreed with the ROM's own table on three angles in four and was one unit of
 * 8.8 out on the rest; the captured frames happened to land on angles where
 * it agreed, which is exactly what a screenshot comparison cannot tell you.
 *
 *     python3 tools/oracle.py ui-affine
 *     make test-uiaffine
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/app/widget.h"

#define MAGIC "AFF1"

static FILE *g;
static int fails;

static int32_t rd32(void)
{
    unsigned char b[4];

    if (fread(b, 1, 4, g) != 4) exit(2);
    return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                     ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/uiaffine.bin";
    char magic[4];
    int32_t ncases, i;
    long checked = 0;

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py ui-affine\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, g) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32();
    for (i = 0; i < ncases; i++) {
        int32_t a = rd32(), sx = rd32(), sy = rd32();
        int32_t fwd[4], inv[4], got[4], k;

        for (k = 0; k < 4; k++) fwd[k] = rd32();
        for (k = 0; k < 4; k++) inv[k] = rd32();

        ds10_widget_affine((int16_t)a, sx, sy, got);
        for (k = 0; k < 4; k++, checked++)
            if (got[k] != fwd[k] && fails++ < 12)
                fprintf(stderr, "FAIL: angle %#x scale %#x/%#x m[%d]: "
                                "the original has %d, we have %d\n",
                        (unsigned)a, sx, sy, (int)k, fwd[k], got[k]);

        /* The inverse is the same matrix with the scales reciprocated, which
         * is what OAM is given; at unit scale the two coincide. */
        if (sx == 0x1000 && sy == 0x1000)
            for (k = 0; k < 4; k++)
                if (inv[k] != fwd[k] && fails++ < 12)
                    fprintf(stderr, "FAIL: angle %#x m[%d]: forward %d but "
                                    "inverse %d at unit scale\n",
                            (unsigned)a, (int)k, fwd[k], inv[k]);
    }
    fclose(g);
    if (fails) {
        fprintf(stderr, "FAIL: %d mismatches over %d matrices\n", fails,
                ncases);
        return 1;
    }
    printf("PASS: %d matrices x 4 entries (%ld checks), 0 mismatches\n",
           ncases, checked);
    return 0;
}
