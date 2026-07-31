/* Differential test: our patch loader against the original's knob dispatch.
 *
 * Vectors from `python3 tools/oracle.py patch-vectors`: all 24 factory patches
 * plus every knob swept across its full range with the rest at INIT. Each
 * record is the 31 knob values, the LFO tempo (an input, not knob-driven), and
 * the 45 internal fields the original produced.
 *
 * The bar is zero mismatching fields.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/patch.h"

#define MAGIC "PAT1"
#define NF 45

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

static const char *NAME[NF] = {
    "pc.octave12", "pc.eg_depth", "pc.depth_sel0", "pc.depth_sel1",
    "pc.depth_sel2", "pc.vco2pitch",
    "pc.lfo_sel0", "pc.lfo_sel1", "pc.lfo_sel2",
    "pc.env_sel0", "pc.env_sel1", "pc.env_sel2",
    "nc.rate",
    "vco.pitch_in", "vco.pitch1_in", "vco.pitch2_in",
    "vco.wave1", "vco.wave2", "vco.balance", "vco.vol1", "vco.vol2",
    "vco.sync",
    "vcf.cutoff", "vcf.peak_raw", "vcf.q", "vcf.type_req", "vcf.eg_depth",
    "vcf.in_depth", "vcf.lfo_sel", "vcf.env_sel",
    "amp.eg_sel", "amp.in_depth", "amp.wet", "amp.drive1", "amp.drive2",
    "amp.level_knob", "amp.lfo_sel", "amp.env_sel",
    "env.attack", "env.decay", "env.sustain", "env.release",
    "lfo.inc", "lfo.rate_knob", "lfo.bpm_sync"
};

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/patch.bin";
    char magic[4];
    uint32_t ncases, nf, i, k;
    unsigned long bad = 0, badcases = 0;
    static uint32_t env_rate[128], lfo_bpm[16], lfo_free[128], porta[128],
        peak[128];
    ds10_tables t;

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py patch-vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, g) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32();
    nf = rd32();
    if (nf != NF) {
        fprintf(stderr, "vector file has %u fields, expected %d\n", nf, NF);
        return 2;
    }
    for (k = 0; k < 128; k++)
        env_rate[k] = rd32();
    for (k = 0; k < 16; k++)
        lfo_bpm[k] = rd32();
    for (k = 0; k < 128; k++)
        lfo_free[k] = rd32();
    for (k = 0; k < 128; k++)
        porta[k] = rd32();
    for (k = 0; k < 128; k++)
        peak[k] = rd32();
    memset(&t, 0, sizeof t);
    t.env_rate = env_rate;
    t.lfo_bpm = lfo_bpm;
    t.lfo_free = lfo_free;
    t.porta = porta;
    t.peak = peak;

    for (i = 0; i < ncases; i++) {
        ds10_voice v;
        int8_t knobs[DS10_NKNOBS];
        int32_t want[NF], got[NF];
        int case_bad = 0;

        if (fread(knobs, 1, DS10_NKNOBS, g) != DS10_NKNOBS)
            return 2;
        memset(&v, 0, sizeof v);
        v.lfo.bpm = (int32_t)rd32();
        for (k = 0; k < NF; k++)
            want[k] = (int32_t)rd32();

        ds10_patch_load(&v, knobs, &t);

        k = 0;
        got[k++] = v.pc.octave12;
        got[k++] = v.pc.eg_depth;
        got[k++] = v.pc.depth_sel[0];
        got[k++] = v.pc.depth_sel[1];
        got[k++] = v.pc.depth_sel[2];
        got[k++] = v.pc.vco2pitch;
        got[k++] = v.pc.lfo_sel[0];
        got[k++] = v.pc.lfo_sel[1];
        got[k++] = v.pc.lfo_sel[2];
        got[k++] = v.pc.env_sel[0];
        got[k++] = v.pc.env_sel[1];
        got[k++] = v.pc.env_sel[2];
        got[k++] = v.nc.rate;
        got[k++] = v.vco.pitch_in;
        got[k++] = v.vco.pitch1_in;
        got[k++] = v.vco.pitch2_in;
        got[k++] = v.vco.wave1;
        got[k++] = v.vco.wave2;
        got[k++] = v.vco.balance;
        got[k++] = v.vco.vol1;
        got[k++] = v.vco.vol2;
        got[k++] = v.vco.sync;
        got[k++] = v.vcf.cutoff;
        got[k++] = v.vcf.peak_raw;
        got[k++] = v.vcf.q;
        got[k++] = v.vcf.type_req;
        got[k++] = v.vcf.eg_depth;
        got[k++] = v.vcf.in_depth;
        got[k++] = v.vcf.lfo_sel;
        got[k++] = v.vcf.env_sel;
        got[k++] = v.amp.eg_sel;
        got[k++] = v.amp.in_depth;
        got[k++] = v.amp.wet;
        got[k++] = v.amp.drive1;
        got[k++] = v.amp.drive2;
        got[k++] = v.amp.level_knob;
        got[k++] = v.amp.lfo_sel;
        got[k++] = v.amp.env_sel;
        got[k++] = v.env_params.attack;
        got[k++] = v.env_params.decay;
        got[k++] = v.env_params.sustain;
        got[k++] = v.env_params.release;
        got[k++] = v.lfo.inc;
        got[k++] = v.lfo.rate_knob;
        got[k++] = v.lfo.bpm_sync;

        for (k = 0; k < NF; k++) {
            if (got[k] != want[k]) {
                if (bad < 12)
                    fprintf(stderr, "case %u: %s got %d want %d\n",
                            i, NAME[k], got[k], want[k]);
                bad++;
                case_bad = 1;
            }
        }
        if (case_bad)
            badcases++;
    }
    fclose(g);

    printf("%s: %u patches x %d fields, %lu mismatches",
           bad ? "FAIL" : "PASS", ncases, NF, bad);
    if (bad)
        printf(" in %lu patches", badcases);
    printf("\n");
    return bad ? 1 : 0;
}
