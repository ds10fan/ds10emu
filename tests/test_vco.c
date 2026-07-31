/* Differential test: our oscillator section against the original ARM code.
 *
 * Vectors from `python3 tools/oracle.py vco-vectors`: per 64-sample frame, the
 * OSC object state at the loop head plus the mixed output for every sample.
 * The mix is what feeds the filter, so this covers both VCOs, cross-modulation,
 * hard sync and the balance mix in one comparison.
 *
 * The bar is zero mismatching samples.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/vco.h"

#define MAGIC "VCO1"
#define NFIELDS 20

static uint32_t rd32(FILE *f)
{
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4) {
        fprintf(stderr, "unexpected end of vector file\n");
        exit(2);
    }
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/vco.bin";
    FILE *f = fopen(path, "rb");
    char magic[4];
    uint32_t ncases, i;
    unsigned long total = 0, bad = 0, badframes = 0;

    if (!f) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py vco-vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32(f);

    for (i = 0; i < ncases; i++) {
        ds10_vco vco;
        int32_t *fields = (int32_t *)&vco;
        uint32_t n, j, k;
        int32_t *want;
        int frame_bad = 0;

        /* Field order in the vector file matches the struct layout exactly. */
        for (k = 0; k < NFIELDS; k++)
            fields[k] = (int32_t)rd32(f);

        n = rd32(f);
        want = malloc(n * sizeof *want);
        if (!want)
            return 2;
        for (j = 0; j < n; j++)
            want[j] = (int32_t)rd32(f);

        for (j = 0; j < n; j++) {
            int32_t got = ds10_vco_step(&vco);
            total++;
            if (got != want[j]) {
                bad++;
                if (!frame_bad && badframes < 8) {
                    fprintf(stderr,
                            "frame %u sample %u: got %d want %d  "
                            "(w1=%d w2=%d xmod=%d sync=%d)\n",
                            i, j, got, want[j], vco.wave1, vco.wave2,
                            vco.xmod, vco.sync);
                    frame_bad = 1;
                }
            }
        }
        if (frame_bad)
            badframes++;
        free(want);
    }
    fclose(f);

    printf("%s: %lu samples over %u frames, %lu mismatches",
           bad ? "FAIL" : "PASS", total, ncases, bad);
    if (bad)
        printf(" in %lu frames", badframes);
    printf("\n");
    return bad ? 1 : 0;
}
