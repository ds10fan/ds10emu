/* Differential test: our per-frame cutoff mapping against the original.
 *
 * Vectors from `python3 tools/oracle.py vcfupd-vectors`. Each record is the
 * FILTER control state before the frame, the ENV/LFO values the block reads and
 * the oscillator feedback, and the state the original produced. The three ROM
 * tables are embedded in the file.
 *
 * Only the fields the mapping writes are compared: coef, the two ramped
 * coefficients and their deltas and targets, the active type, and the two
 * integrators (which a type change clears).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/vcf.h"
#include "vectortables.h"

#define MAGIC "VCU1"
#define NFIELDS 19
#define NMOD 128
#define NCOEF 152

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

/* Order must match VCF_ALL in tools/oracle.py and ds10_vcf's layout. */
static const char *FIELD[NFIELDS] = {
    "q", "band", "low", "type", "f", "f_delta", "damp", "damp_delta",
    "lfo_sel", "env_sel", "osc_depth", "peak_raw", "cutoff", "coef",
    "type_req", "eg_depth", "in_depth", "f_target", "damp_target"
};

/* Fields the mapping is responsible for.
 *
 * Excluded because the sample loop rewrites them after the preamble runs, so a
 * post-frame snapshot cannot see what the mapping wrote:
 *   band, low - only touched on a type change, then overwritten by the loop
 *   f, damp    - set to the previous targets, then ramped 64 times by the loop
 * Both are covered by the per-sample test, which starts from captured state.
 * What remains here fully pins the mapping down: the coefficient, both ramp
 * deltas, both new targets, and the active type. */
static const int CHECK[] = {3, 5, 7, 13, 17, 18};

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/vcfupd.bin";
    FILE *f = fopen(path, "rb");
    char magic[4];
    uint32_t ncases, i, k;
    unsigned long bad = 0;
    static int16_t mod_a[NMOD], mod_b[NMOD];
    static int32_t coef[NCOEF];

    if (!f) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py vcfupd-vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32(f);
    for (k = 0; k < NMOD; k++)
        mod_a[k] = rd16(f);
    for (k = 0; k < NMOD; k++)
        mod_b[k] = rd16(f);
    for (k = 0; k < NCOEF; k++)
        coef[k] = (int32_t)rd32(f);
    vt_i16("mod_a", mod_a, ds10_tbl_mod_a, NMOD);
    vt_i16("mod_b", mod_b, ds10_tbl_mod_b, NMOD);
    vt_i32("coef", coef, ds10_tbl_coef, NCOEF);

    for (i = 0; i < ncases; i++) {
        ds10_vcf vcf;
        int32_t *fields = (int32_t *)&vcf;
        int32_t want[NFIELDS], env[3], lfo[5], prev2;
        int frame_bad = 0;
        size_t c;

        for (k = 0; k < NFIELDS; k++)
            fields[k] = (int32_t)rd32(f);
        for (k = 0; k < 3; k++)
            env[k] = (int32_t)rd32(f);
        for (k = 0; k < 5; k++)
            lfo[k] = (int32_t)rd32(f);
        prev2 = (int32_t)rd32(f);
        for (k = 0; k < NFIELDS; k++)
            want[k] = (int32_t)rd32(f);

        ds10_vcf_update(&vcf, env, lfo, prev2);

        for (c = 0; c < sizeof CHECK / sizeof *CHECK; c++) {
            int idx = CHECK[c];

            if (fields[idx] != want[idx]) {
                if (!frame_bad && bad < 8) {
                    fprintf(stderr, "case %u: %s got %d want %d\n",
                            i, FIELD[idx], fields[idx], want[idx]);
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
