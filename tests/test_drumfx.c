/* A drum's own effect settings, against the original.
 *
 * The nine bytes per drum at payload +0x2BA8 are a route and the eight effect
 * parameters, and 0x0206CCC8 pushes them into the mixer as knobs 0..8 before it
 * renders that drum's one-second buffer - so a drum's sample goes through the
 * effect as it is written. recon.md §14 has the trace.
 *
 * What this checks is that stage: given the dry buffer the voice produced and the
 * drum's nine bytes, our mixer and effect have to produce the stereo the
 * original's do, sample for sample. The vector carries the state the original's
 * constructors left, so what is being compared is the knob mapping and the DSP,
 * not the constructors.
 *
 * What it does NOT check is how the offline recorder turns that stereo back into
 * the mono buffer a drum voice resamples: the recorder is the object at +0x10 of
 * 0x0206CCC8's `this` and is not decompiled. Note that L != R even for a dry
 * drum, because centre pan is 64 of 127 - so that step is a real question and not
 * a formality. TODO.md §5.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/mixer.h"
#include "../src/core/mixer_state.h"

#define FRAME 64
#define NM 21
#define NF 40
#define NKNOBS 31
#define NEXTRA 9

static FILE *f;

static uint32_t rd32(void)
{
    uint8_t b[4];

    if (fread(b, 1, 4, f) != 4) { fprintf(stderr, "short read\n"); exit(1); }
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
           (uint32_t)b[3] << 24;
}

static int16_t rd16(void)
{
    uint8_t b[2];

    if (fread(b, 1, 2, f) != 2) { fprintf(stderr, "short read\n"); exit(1); }
    return (int16_t)((uint16_t)b[0] | (uint16_t)b[1] << 8);
}

static int8_t rd8(void)
{
    int c = fgetc(f);

    if (c == EOF) { fprintf(stderr, "short read\n"); exit(1); }
    return (int8_t)c;
}

int main(void)
{
    static int16_t buf_l[DS10_FX_BUFLEN], buf_r[DS10_FX_BUFLEN];
    static int16_t chan[6][FRAME];
    char magic[4];
    uint32_t ncases, c;
    long total = 0, bad = 0;

    f = fopen("tests/vectors/drumfx.bin", "rb");
    if (!f) {
        fprintf(stderr, "cannot open tests/vectors/drumfx.bin\n");
        return 1;
    }
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "DFX1", 4)) {
        fprintf(stderr, "bad vector file\n");
        return 1;
    }
    ncases = rd32();

    for (c = 0; c < ncases; c++) {
        ds10_mixer m;
        int32_t mf[NM], ff[NF];
        int8_t knobs[NKNOBS], extra[NEXTRA];
        int32_t drum, frames;
        const int16_t *ptrs[6];
        int i, j, s, case_bad = 0;

        drum = (int32_t)rd32();
        frames = (int32_t)rd32();
        for (i = 0; i < NKNOBS; i++) knobs[i] = rd8();
        for (i = 0; i < NEXTRA; i++) extra[i] = rd8();
        for (i = 0; i < NM; i++) mf[i] = (int32_t)rd32();
        for (i = 0; i < NF; i++) ff[i] = (int32_t)rd32();

        memset(&m, 0, sizeof m);
        ds10_mixer_state_load(&m, mf);
        ds10_fx_state_load(&m.fx, ff);
        memset(buf_l, 0, sizeof buf_l);
        memset(buf_r, 0, sizeof buf_r);
        m.fx.buf_l = buf_l;
        m.fx.buf_r = buf_r;
        for (i = 0; i < 6; i++) ptrs[i] = chan[i];

        /* The render's own mixer setup: channel 0 at full volume and centre pan,
         * everything else silent - then the drum's nine as knobs 0..8. */
        ds10_mixer_set_knob(&m, 17, 127);
        ds10_mixer_set_knob(&m, 9, 64);
        for (i = 1; i < 6; i++) {
            ds10_mixer_set_knob(&m, 17 + i, 0);
            ds10_mixer_set_knob(&m, 9 + i, 0);
        }
        for (i = 0; i < NEXTRA; i++)
            ds10_mixer_set_knob(&m, i, extra[i]);

        for (j = 0; j < frames; j++) {
            int16_t got_l[FRAME], got_r[FRAME], want_l[FRAME], want_r[FRAME];

            for (s = 0; s < FRAME; s++) chan[0][s] = rd16();
            for (i = 1; i < 6; i++) memset(chan[i], 0, sizeof chan[i]);
            for (s = 0; s < FRAME; s++) want_l[s] = rd16();
            for (s = 0; s < FRAME; s++) want_r[s] = rd16();

            ds10_mixer_render(&m, ptrs, got_l, got_r, FRAME);

            for (s = 0; s < FRAME; s++) {
                total += 2;
                if (got_l[s] != want_l[s] || got_r[s] != want_r[s]) {
                    bad += (got_l[s] != want_l[s]) + (got_r[s] != want_r[s]);
                    if (!case_bad) {
                        fprintf(stderr,
                                "FAIL drum %d frame %d sample %d: "
                                "got %d/%d want %d/%d (wet=%d type=%d)\n",
                                drum, j, s, got_l[s], got_r[s], want_l[s],
                                want_r[s], extra[8], extra[1]);
                        case_bad = 1;
                    }
                }
            }
        }
    }
    fclose(f);
    if (bad) {
        printf("FAIL: %ld of %ld samples differ\n", bad, total);
        return 1;
    }
    printf("PASS: %ld samples over %u drums, the effect their own nine bytes ask"
           " for\n", total, ncases);
    return 0;
}
