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
#include "voice_state.h"

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

    if (nfields != 90) {
        fprintf(stderr, "vector file has %u state fields, expected 90 - "
                "struct layout and the capture lists are out of sync\n",
                nfields);
        return 2;
    }

    for (i = 0; i < ncases; i++) {
        ds10_voice v;
        int16_t got[FRAME], want[FRAME];
        int frame_bad = 0;
        int j;

        voice_state_load(&v, s);

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
