/* Differential test: our VCF against the original ARM code.
 *
 * Vectors are produced by `python3 tools/oracle.py vectors`, which runs the
 * real DS-10 filter under emulation and records, per 64-sample frame, the
 * filter state at the loop head plus every input and output sample.
 *
 * The bar is zero mismatching samples. Everything is integer, so there is no
 * tolerance to argue about.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/vcf.h"

#define MAGIC "VCF1"

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
    const char *path = argc > 1 ? argv[1] : "tests/vectors/vcf.bin";
    FILE *f = fopen(path, "rb");
    char magic[4];
    uint32_t ncases, i;
    unsigned long total = 0, bad = 0, badframes = 0;

    if (!f) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32(f);

    for (i = 0; i < ncases; i++) {
        ds10_vcf vcf;
        uint32_t n, j;
        int32_t *in, *want;
        int frame_bad = 0;

        vcf.q = (int32_t)rd32(f);
        vcf.band = (int32_t)rd32(f);
        vcf.low = (int32_t)rd32(f);
        vcf.type = (int32_t)rd32(f);
        vcf.f = (int32_t)rd32(f);
        vcf.f_delta = (int32_t)rd32(f);
        vcf.damp = (int32_t)rd32(f);
        vcf.damp_delta = (int32_t)rd32(f);

        n = rd32(f);
        in = malloc(n * sizeof *in);
        want = malloc(n * sizeof *want);
        if (!in || !want)
            return 2;
        for (j = 0; j < n; j++)
            in[j] = (int32_t)rd32(f);
        for (j = 0; j < n; j++)
            want[j] = (int32_t)rd32(f);

        for (j = 0; j < n; j++) {
            int32_t got = ds10_vcf_step(&vcf, in[j]);
            total++;
            if (got != want[j]) {
                bad++;
                if (!frame_bad && badframes < 6) {
                    fprintf(stderr,
                            "frame %u sample %u: got %d want %d  "
                            "(q=%d type=%d f=%d damp=%d)\n",
                            i, j, got, want[j], vcf.q, vcf.type, vcf.f,
                            vcf.damp);
                    frame_bad = 1;
                }
            }
        }
        if (frame_bad)
            badframes++;
        free(in);
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
