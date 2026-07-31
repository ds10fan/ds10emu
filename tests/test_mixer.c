/* Differential test: our mixer and effect DSP against the original.
 *
 * Vectors from `python3 tools/oracle.py mixer-vectors`. Each case seeds the
 * mixer and effect state, then drives N frames of six known voice buffers
 * through the real 0x02072E8C and records the stereo output.
 *
 * The two 65536-sample delay buffers are not in the vectors: both sides start
 * them zeroed and they evolve deterministically from the same inputs, so they
 * stay in lockstep.
 *
 * Coverage: all three effect types x all six routings, pan and volume extremes,
 * the mute ramp in both directions, tempo-synced delay through the divider, and
 * heavy feedback so the ring buffer and its per-sample ramps matter.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/mixer.h"

#define MAGIC "MIX1"
#define NM 21
#define NF 40
#define FRAME 64

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

static void load_mixer(ds10_mixer *m, const int32_t *f)
{
    int k = 0, c;

    m->gain = f[k++];
    for (c = 0; c < 6; c++)
        m->pan[c] = f[k++];
    for (c = 0; c < 6; c++)
        m->vol[c] = f[k++];
    for (c = 0; c < 6; c++)
        m->prod[c] = f[k++];
    m->route = f[k++];
    m->mute = (int16_t)f[k++];
}

static void load_fx(ds10_fx *fx, const int32_t *f)
{
    int k = 0, i;

    fx->type = (int16_t)f[k++];
    fx->sync = (int16_t)f[k++];
    fx->k_time = (int16_t)f[k++];
    fx->k_ratio = (int16_t)f[k++];
    fx->k_lfo = (int16_t)f[k++];
    fx->k_depth = (int16_t)f[k++];
    fx->k_fb = (int16_t)f[k++];
    fx->k_drywet = (int16_t)f[k++];
    fx->fb_enable = (int16_t)f[k++];
    for (i = 0; i < 3; i++)
        fx->lfo_l[i] = f[k++];
    for (i = 0; i < 3; i++)
        fx->lfo_r[i] = f[k++];
    fx->lfo_phase = f[k++];
    fx->lfo_inc = f[k++];
    fx->depth = f[k++];
    fx->feedback = f[k++];
    fx->time = f[k++];
    fx->smooth_l = f[k++];
    fx->smooth_r = f[k++];
    fx->read_l = f[k++];
    fx->read_l_target = f[k++];
    fx->read_l_delta = f[k++];
    fx->read_r = f[k++];
    fx->read_r_target = f[k++];
    fx->read_r_delta = f[k++];
    fx->fb_l = f[k++];
    fx->fb_l_target = f[k++];
    fx->fb_l_delta = f[k++];
    fx->fb_r = f[k++];
    fx->fb_r_target = f[k++];
    fx->fb_r_delta = f[k++];
    fx->dry = f[k++];
    fx->wet = f[k++];
    fx->widx = f[k++];
    fx->ratio_l = f[k++];
    fx->ratio_r = f[k++];
    fx->tempo = f[k++];
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/mixer.bin";
    char magic[4];
    uint32_t ncases, nm, nf, i, k;
    unsigned long total = 0, bad = 0, badcases = 0;
    static int32_t phase_off[3], delay_div[12], lfo_rate[128];
    static int16_t flanger_fb[128], depth[128];
    static int16_t buf_l[DS10_FX_BUFLEN], buf_r[DS10_FX_BUFLEN];
    static int16_t chan[6][FRAME];
    ds10_fx_tables t;

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py mixer-vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, g) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32();
    nm = rd32();
    nf = rd32();
    if (nm != NM || nf != NF) {
        fprintf(stderr, "vector file has %u/%u fields, expected %d/%d\n",
                nm, nf, NM, NF);
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
    t.lfo_phase_off = phase_off;
    t.flanger_fb = flanger_fb;
    t.depth = depth;
    t.delay_div = (const uint32_t *)delay_div;
    t.lfo_rate = (const uint32_t *)lfo_rate;

    for (i = 0; i < ncases; i++) {
        ds10_mixer m;
        int32_t mf[NM], ff[NF];
        int32_t nsteps, j, s;
        const int16_t *ptrs[6];
        int case_bad = 0;

        for (k = 0; k < NM; k++)
            mf[k] = (int32_t)rd32();
        for (k = 0; k < NF; k++)
            ff[k] = (int32_t)rd32();
        nsteps = (int32_t)rd32();

        memset(&m, 0, sizeof m);
        load_mixer(&m, mf);
        load_fx(&m.fx, ff);
        memset(buf_l, 0, sizeof buf_l);
        memset(buf_r, 0, sizeof buf_r);
        m.fx.buf_l = buf_l;
        m.fx.buf_r = buf_r;
        for (k = 0; k < 6; k++)
            ptrs[k] = chan[k];

        for (j = 0; j < nsteps; j++) {
            int16_t got_l[FRAME], got_r[FRAME], want_l[FRAME], want_r[FRAME];

            for (k = 0; k < 6; k++)
                for (s = 0; s < FRAME; s++)
                    chan[k][s] = rd16();
            for (s = 0; s < FRAME; s++)
                want_l[s] = rd16();
            for (s = 0; s < FRAME; s++)
                want_r[s] = rd16();

            ds10_mixer_render(&m, ptrs, &t, got_l, got_r, FRAME);

            for (s = 0; s < FRAME; s++) {
                total += 2;
                if (got_l[s] != want_l[s] || got_r[s] != want_r[s]) {
                    bad += (got_l[s] != want_l[s]) + (got_r[s] != want_r[s]);
                    if (!case_bad && badcases < 8) {
                        fprintf(stderr,
                                "case %u frame %d sample %d: got L=%d R=%d, "
                                "want L=%d R=%d  (type=%d route=%d mute=%d)\n",
                                i, j, s, got_l[s], got_r[s], want_l[s],
                                want_r[s], m.fx.type, m.route, m.mute);
                        case_bad = 1;
                    }
                }
            }
        }
        if (case_bad)
            badcases++;
    }
    fclose(g);

    printf("%s: %lu samples over %u cases, %lu mismatches",
           bad ? "FAIL" : "PASS", total, ncases, bad);
    if (bad)
        printf(" in %lu cases", badcases);
    printf("\n");
    return bad ? 1 : 0;
}
