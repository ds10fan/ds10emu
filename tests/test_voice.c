/* End-to-end differential test: a whole synth voice against the original.
 *
 * Vectors from `python3 tools/oracle.py voice-vectors`. Each record is the
 * complete voice state before a frame - LFO, envelope and its rates, note
 * tracking, pitch control, oscillator, filter, amp - and the 64 samples the
 * original wrote to its output buffer.
 *
 * This subsumes the per-stage tests: it exercises the per-frame preamble and
 * the sample loop together, so an error anywhere in the chain shows up here.
 * The per-stage tests remain useful for localising a failure.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/voice.h"

#define MAGIC "VOI1"
#define NDEPTH 128
#define NPITCH 130
#define NMOD 128
#define NCOEF 152
#define FRAME DS10_FRAME

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

static int32_t s(void)
{
    return (int32_t)rd32();
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/voice.bin";
    char magic[4];
    uint32_t ncases, nfields, i, k;
    unsigned long total = 0, bad = 0, badframes = 0;
    static int32_t depth[NDEPTH], pitchtab[NPITCH], coef[NCOEF];
    static int16_t mod_a[NMOD], mod_b[NMOD];
    ds10_pitch_tables pt;
    ds10_vcf_tables ft;

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py voice-vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, g) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32();
    nfields = rd32();
    for (k = 0; k < NDEPTH; k++)
        depth[k] = s();
    for (k = 0; k < NPITCH; k++)
        pitchtab[k] = s();
    for (k = 0; k < NMOD; k++)
        mod_a[k] = rd16();
    for (k = 0; k < NMOD; k++)
        mod_b[k] = rd16();
    for (k = 0; k < NCOEF; k++)
        coef[k] = s();
    pt.depth = depth;
    pt.pitch = pitchtab;
    ft.mod_a = mod_a;
    ft.mod_b = mod_b;
    ft.coef = coef;

    if (nfields != 89) {
        fprintf(stderr, "vector file has %u state fields, expected 89 - "
                "struct layout and the capture lists are out of sync\n",
                nfields);
        return 2;
    }

    for (i = 0; i < ncases; i++) {
        ds10_voice v;
        int16_t got[FRAME], want[FRAME];
        int frame_bad = 0;
        int j;

        for (k = 0; k < 5; k++)
            v.lfo.out[k] = s();
        v.lfo.phase_down = s();
        v.lfo.inc = s();
        v.lfo.phase_up = s();
        v.lfo.lfsr = s();

        v.env.stage = (uint8_t)rd32();
        v.env.level = s();
        v.env.aux = s();
        v.env_params.attack = s();
        v.env_params.decay = s();
        v.env_params.sustain = s();
        v.env_params.release = s();

        v.nc.target = s();
        v.nc.current = s();
        v.nc.rate = s();
        v.nc.dir = (uint8_t)rd32();

        v.pc.eg_depth = s();
        for (k = 0; k < 3; k++)
            v.pc.depth_sel[k] = s();
        v.pc.octave12 = s();
        v.pc.vco2pitch = (int16_t)s();
        v.pc.inc1 = s();
        v.pc.inc2 = s();
        for (k = 0; k < 3; k++)
            v.pc.lfo_sel[k] = (int16_t)s();
        for (k = 0; k < 3; k++)
            v.pc.env_sel[k] = s();

        v.vco.xmod = s();
        v.vco.phase1 = s();
        v.vco.phase2 = s();
        v.vco.inc1 = s();
        v.vco.inc2 = s();
        v.vco.shape1 = s();
        v.vco.shape2 = s();
        v.vco.wave1 = s();
        v.vco.wave2 = s();
        v.vco.vol1 = s();
        v.vco.vol2 = s();
        v.vco.lfsr1 = s();
        v.vco.lfsr2 = s();
        v.vco.sync = s();
        v.vco.prev2 = s();
        v.vco.hold2 = s();
        v.vco.xm_60 = s();
        v.vco.xm_64 = s();
        v.vco.xm_68 = s();
        v.vco.xm_6c = s();
        v.vco.pitch_in = s();
        v.vco.pitch1_in = s();
        v.vco.pitch2_in = s();

        v.vcf.q = s();
        v.vcf.band = s();
        v.vcf.low = s();
        v.vcf.type = s();
        v.vcf.f = s();
        v.vcf.f_delta = s();
        v.vcf.damp = s();
        v.vcf.damp_delta = s();
        v.vcf.lfo_sel = s();
        v.vcf.env_sel = s();
        v.vcf.osc_depth = s();
        v.vcf.peak_raw = s();
        v.vcf.cutoff = s();
        v.vcf.coef = s();
        v.vcf.type_req = s();
        v.vcf.eg_depth = s();
        v.vcf.in_depth = s();
        v.vcf.f_target = s();
        v.vcf.damp_target = s();

        v.amp.dist = s();
        v.amp.wet = s();
        v.amp.level = s();
        v.amp.level_delta = s();
        v.amp.clipped = s();
        v.amp.drive1 = s();
        v.amp.drive2 = s();
        v.amp.lfo_sel = s();
        v.amp.env_sel = s();
        v.amp.level_knob = s();
        v.amp.in_depth = s();
        v.amp.eg_sel = s();
        v.amp.target = s();

        for (j = 0; j < FRAME; j++)
            want[j] = rd16();

        ds10_voice_render(&v, &pt, &ft, got, FRAME);

        for (j = 0; j < FRAME; j++) {
            total++;
            if (got[j] != want[j]) {
                bad++;
                if (!frame_bad && badframes < 8) {
                    fprintf(stderr,
                            "frame %u sample %d: got %d want %d  "
                            "(w1=%d w2=%d type=%d stage=%d)\n",
                            i, j, got[j], want[j], v.vco.wave1, v.vco.wave2,
                            v.vcf.type, v.env.stage);
                    frame_bad = 1;
                }
            }
        }
        if (frame_bad)
            badframes++;
    }
    fclose(g);

    printf("%s: %lu samples over %u frames, %lu mismatches",
           bad ? "FAIL" : "PASS", total, ncases, bad);
    if (bad)
        printf(" in %lu frames", badframes);
    printf("\n");
    return bad ? 1 : 0;
}
