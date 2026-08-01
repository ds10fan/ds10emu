/* Differential test: our effect parameter mappings against the original.
 *
 * Vectors from `python3 tools/oracle.py fx-vectors`. Each sequence starts from
 * a freshly constructed effect processor, then applies a series of parameter
 * sets; after every one, all 17 mapping-visible fields are compared.
 *
 * Sequences cover all three types (a type change reapplies the other six
 * parameters through the new mapping, so order matters) plus two tempo-synced
 * delays, which are the only path that uses the hardware divider.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/fx.h"
#include "vectortables.h"

#define MAGIC "FXP1"
#define NF 17

static FILE *g;

static uint32_t rd32(void)
{
    unsigned char b[4];
    if (fread(b, 1, 4, g) != 4) {
        fprintf(stderr, "unexpected end of vector file\n");
        exit(2);
    }
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static int16_t rd16(void)
{
    unsigned char b[2];
    if (fread(b, 1, 2, g) != 2) {
        fprintf(stderr, "unexpected end of vector file\n");
        exit(2);
    }
    return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static const char *NAME[NF] = {
    "type", "sync", "k_time", "k_ratio", "k_lfo", "k_depth", "k_fb",
    "k_drywet", "fb_enable", "lfo_inc", "depth", "feedback", "time",
    "dry", "wet", "ratio_l", "ratio_r"
};

static void load(ds10_fx *fx, const int32_t *f)
{
    fx->type = (int16_t)f[0];
    fx->sync = (int16_t)f[1];
    fx->k_time = (int16_t)f[2];
    fx->k_ratio = (int16_t)f[3];
    fx->k_lfo = (int16_t)f[4];
    fx->k_depth = (int16_t)f[5];
    fx->k_fb = (int16_t)f[6];
    fx->k_drywet = (int16_t)f[7];
    fx->fb_enable = (int16_t)f[8];
    fx->lfo_inc = f[9];
    fx->depth = f[10];
    fx->feedback = f[11];
    fx->time = f[12];
    fx->dry = f[13];
    fx->wet = f[14];
    fx->ratio_l = f[15];
    fx->ratio_r = f[16];
}

static void store(const ds10_fx *fx, int32_t *f)
{
    f[0] = fx->type;
    f[1] = fx->sync;
    f[2] = fx->k_time;
    f[3] = fx->k_ratio;
    f[4] = fx->k_lfo;
    f[5] = fx->k_depth;
    f[6] = fx->k_fb;
    f[7] = fx->k_drywet;
    f[8] = fx->fb_enable;
    f[9] = fx->lfo_inc;
    f[10] = fx->depth;
    f[11] = fx->feedback;
    f[12] = fx->time;
    f[13] = fx->dry;
    f[14] = fx->wet;
    f[15] = fx->ratio_l;
    f[16] = fx->ratio_r;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/fx.bin";
    char magic[4];
    uint32_t nseq, nf, nsteps, i, k;
    unsigned long bad = 0, total = 0;
    static int32_t phase_off[3], delay_div[12], lfo_rate[128];
    static int16_t flanger_fb[128], depth[128];

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py fx-vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, g) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    nseq = rd32();
    nf = rd32();
    nsteps = rd32();
    if (nf != NF) {
        fprintf(stderr, "vector file has %u fields, expected %d\n", nf, NF);
        return 2;
    }
    for (k = 0; k < 3; k++)
        phase_off[k] = (int32_t)rd32();
    for (k = 0; k < 128; k++)
        flanger_fb[k] = rd16();
    for (k = 0; k < 128; k++)
        depth[k] = rd16();
    for (k = 0; k < 12; k++)
        delay_div[k] = (int32_t)rd32();
    for (k = 0; k < 128; k++)
        lfo_rate[k] = (int32_t)rd32();
    vt_i32("fx_lfo_phase", phase_off, ds10_tbl_fx_lfo_phase, 3);
    vt_i16("fx_flanger_fb", flanger_fb, ds10_tbl_fx_flanger_fb, 128);
    vt_i16("fx_depth", depth, ds10_tbl_fx_depth, 128);
    vt_u32("fx_delay_div", (const uint32_t *)delay_div,
           ds10_tbl_fx_delay_div, 12);
    vt_u32("fx_lfo_rate", (const uint32_t *)lfo_rate,
           ds10_tbl_fx_lfo_rate, 128);

    for (i = 0; i < nseq; i++) {
        ds10_fx fx;
        int32_t init[NF], want[NF], got[NF];
        int32_t tempo, n, j;

        for (k = 0; k < NF; k++)
            init[k] = (int32_t)rd32();
        tempo = (int32_t)rd32();
        n = (int32_t)rd32();

        memset(&fx, 0, sizeof fx);
        load(&fx, init);
        fx.tempo = tempo;

        for (j = 0; j < n; j++) {
            int32_t param = (int32_t)rd32();
            int32_t value = (int32_t)rd32();

            for (k = 0; k < NF; k++)
                want[k] = (int32_t)rd32();

            ds10_fx_set(&fx, param, value);
            store(&fx, got);
            total++;

            for (k = 0; k < NF; k++) {
                if (got[k] != want[k]) {
                    if (bad < 12)
                        fprintf(stderr,
                                "seq %u step %d (param=%d value=%d): "
                                "%s got %d want %d\n",
                                i, j, param, value, NAME[k], got[k], want[k]);
                    bad++;
                }
            }
        }
    }
    fclose(g);

    printf("%s: %lu parameter sets x %d fields, %lu mismatches\n",
           bad ? "FAIL" : "PASS", total, NF, bad);
    (void)nsteps;
    return bad ? 1 : 0;
}
