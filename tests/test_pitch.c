/* Differential test: our note tracking and pitch control against the original.
 *
 * Vectors from `python3 tools/oracle.py pitch-vectors`. Each record carries the
 * NOTECTRL and PITCHCTRL state before the frame plus the ENV and LFO values the
 * pitch block reads, and the three values the original produced: the glided
 * note pitch and the two oscillator phase increments.
 *
 * The two ROM tables are embedded in the vector file so the test needs no
 * separate asset path.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/pitch.h"

#define MAGIC "PIT1"
#define NDEPTH 128
#define NPITCH 130
#define NC_N 4
#define PC_N 14

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
    const char *path = argc > 1 ? argv[1] : "tests/vectors/pitch.bin";
    FILE *f = fopen(path, "rb");
    char magic[4];
    uint32_t ncases, i, k;
    unsigned long bad = 0;
    static int32_t depth[NDEPTH], pitch[NPITCH];
    ds10_pitch_tables tables;

    if (!f) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py pitch-vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32(f);
    for (k = 0; k < NDEPTH; k++)
        depth[k] = (int32_t)rd32(f);
    for (k = 0; k < NPITCH; k++)
        pitch[k] = (int32_t)rd32(f);
    tables.depth = depth;
    tables.pitch = pitch;

    for (i = 0; i < ncases; i++) {
        ds10_notectrl nc;
        ds10_pitchctrl pc;
        int32_t nv[NC_N], pv[PC_N], env[3], lfo[5];
        int32_t want_cur, want_i1, want_i2;

        for (k = 0; k < NC_N; k++)
            nv[k] = (int32_t)rd32(f);
        for (k = 0; k < PC_N; k++)
            pv[k] = (int32_t)rd32(f);
        for (k = 0; k < 3; k++)
            env[k] = (int32_t)rd32(f);
        for (k = 0; k < 5; k++)
            lfo[k] = (int32_t)rd32(f);
        want_cur = (int32_t)rd32(f);
        want_i1 = (int32_t)rd32(f);
        want_i2 = (int32_t)rd32(f);

        nc.target = nv[0];
        nc.current = nv[1];
        nc.rate = nv[2];
        nc.dir = (uint8_t)nv[3];

        pc.eg_depth = pv[0];
        pc.depth_sel[0] = pv[1];
        pc.depth_sel[1] = pv[2];
        pc.depth_sel[2] = pv[3];
        pc.octave12 = pv[4];
        pc.vco2pitch = (int16_t)pv[5];
        pc.inc1 = pv[6];
        pc.inc2 = pv[7];
        pc.lfo_sel[0] = (int16_t)pv[8];
        pc.lfo_sel[1] = (int16_t)pv[9];
        pc.lfo_sel[2] = (int16_t)pv[10];
        pc.env_sel[0] = pv[11];
        pc.env_sel[1] = pv[12];
        pc.env_sel[2] = pv[13];

        /* The original updates NOTECTRL before PITCHCTRL reads it. */
        ds10_notectrl_step(&nc);
        ds10_pitchctrl_step(&pc, &nc, env, lfo, &tables);

        if (nc.current != want_cur || pc.inc1 != want_i1 ||
            pc.inc2 != want_i2) {
            if (bad < 8)
                fprintf(stderr,
                        "case %u: got cur=%d inc1=%d inc2=%d, "
                        "want cur=%d inc1=%d inc2=%d\n",
                        i, nc.current, pc.inc1, pc.inc2, want_cur, want_i1,
                        want_i2);
            bad++;
        }
    }
    fclose(f);

    printf("%s: %u frame transitions, %lu mismatches\n",
           bad ? "FAIL" : "PASS", ncases, bad);
    return bad ? 1 : 0;
}
