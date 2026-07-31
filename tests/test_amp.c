/* Differential test: our VCA/drive stage against the original ARM code.
 *
 * Vectors from `python3 tools/oracle.py amp-vectors`. Per frame: the AMP state
 * at the loop head, the filter output and VCO2 output for each sample, and the
 * samples the original actually wrote to the voice's output buffer.
 *
 * This closes the per-sample chain - the expected values here are the real
 * render output, not an intermediate probe.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/amp.h"

#define MAGIC "AMP1"
#define NFIELDS 7

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

static int16_t rd16(FILE *f)
{
    unsigned char b[2];
    if (fread(b, 1, 2, f) != 2) {
        fprintf(stderr, "unexpected end of vector file\n");
        exit(2);
    }
    return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/amp.bin";
    FILE *f = fopen(path, "rb");
    char magic[4];
    uint32_t ncases, i;
    unsigned long total = 0, bad = 0, badframes = 0;

    if (!f) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py amp-vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32(f);

    for (i = 0; i < ncases; i++) {
        ds10_amp amp;
        int32_t *fields = (int32_t *)&amp;
        uint32_t n, j, k;
        int32_t *in, *vco2;
        int16_t *want;
        int frame_bad = 0;

        for (k = 0; k < NFIELDS; k++)
            fields[k] = (int32_t)rd32(f);

        n = rd32(f);
        in = malloc(n * sizeof *in);
        vco2 = malloc(n * sizeof *vco2);
        want = malloc(n * sizeof *want);
        if (!in || !vco2 || !want)
            return 2;
        for (j = 0; j < n; j++)
            in[j] = (int32_t)rd32(f);
        for (j = 0; j < n; j++)
            vco2[j] = (int32_t)rd32(f);
        for (j = 0; j < n; j++)
            want[j] = rd16(f);

        for (j = 0; j < n; j++) {
            int16_t got = ds10_amp_step(&amp, in[j], vco2[j]);
            total++;
            if (got != want[j]) {
                bad++;
                if (!frame_bad && badframes < 8) {
                    fprintf(stderr,
                            "frame %u sample %u: got %d want %d  "
                            "(in=%d vco2=%d drive1=%d drive2=%d level=%d "
                            "dist=%d)\n",
                            i, j, got, want[j], in[j], vco2[j], amp.drive1,
                            amp.drive2, amp.level, amp.dist);
                    frame_bad = 1;
                }
            }
        }
        if (frame_bad)
            badframes++;
        free(in);
        free(vco2);
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
