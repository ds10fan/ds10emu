/* Differential test: our LFO against the original ARM code.
 *
 * Vectors from `python3 tools/oracle.py lfo-vectors`. The LFO advances once per
 * frame, so each record is a full before/after state pair covering all four
 * output shapes, both accumulators and the noise register.
 *
 * The bar is zero mismatching transitions.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/lfo.h"

#define MAGIC "LFO1"
#define NFIELDS 9

static const char *FIELD[NFIELDS] = {
    "out[sh]", "out[saw]", "out[tri]", "out[square]", "out[none]",
    "phase_down", "inc", "phase_up", "lfsr"
};

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
    const char *path = argc > 1 ? argv[1] : "tests/vectors/lfo.bin";
    FILE *f = fopen(path, "rb");
    char magic[4];
    uint32_t ncases, i;
    unsigned long bad = 0;

    if (!f) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py lfo-vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32(f);

    for (i = 0; i < ncases; i++) {
        ds10_lfo lfo;
        int32_t *fields = (int32_t *)&lfo;
        int32_t want[NFIELDS];
        uint32_t k;
        int frame_bad = 0;

        for (k = 0; k < NFIELDS; k++)
            fields[k] = (int32_t)rd32(f);
        for (k = 0; k < NFIELDS; k++)
            want[k] = (int32_t)rd32(f);

        ds10_lfo_step(&lfo);

        for (k = 0; k < NFIELDS; k++) {
            if (fields[k] != want[k]) {
                if (!frame_bad && bad < 8) {
                    fprintf(stderr, "case %u: %s got %d want %d\n",
                            i, FIELD[k], fields[k], want[k]);
                    frame_bad = 1;
                }
            }
        }
        if (frame_bad)
            bad++;
    }
    fclose(f);

    printf("%s: %u frame transitions, %lu mismatches\n",
           bad ? "FAIL" : "PASS", ncases, bad);
    return bad ? 1 : 0;
}
