/* Differential test: our drum voice against the original ARM code.
 *
 * Vectors from `python3 tools/oracle.py drum-vectors`. Each case seeds the
 * playback rate, envelope stage and level, ships the sample buffer, and records
 * the samples the original produced over several frames.
 *
 * Coverage: rates from 1/16 to 8x unity, all three envelope stages, a case that
 * runs past the end of the buffer so the one-shot clamp engages, and envelopes
 * starting from both zero and the ceiling.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/drum.h"

#define MAGIC "DRM1"
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

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/drum.bin";
    char magic[4];
    uint32_t ncases, i;
    unsigned long total = 0, bad = 0, badcases = 0;

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py drum-vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, g) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32();

    for (i = 0; i < ncases; i++) {
        ds10_drum d;
        int16_t *wave;
        uint32_t nwave, nframes, w, j;
        int case_bad = 0;

        memset(&d, 0, sizeof d);
        d.env.vol = (int32_t)rd32();
        d.env.stage = (uint8_t)rd32();
        d.speed = (int32_t)rd32();
        d.offset = (int32_t)rd32();
        d.length = (int32_t)rd32();
        nwave = rd32();
        nframes = rd32();

        wave = malloc(nwave * sizeof *wave);
        if (!wave)
            return 2;
        for (w = 0; w < nwave; w++)
            wave[w] = rd16();
        d.sample = wave;

        for (j = 0; j < nframes; j++) {
            int16_t got[FRAME], want[FRAME];
            int s;

            ds10_drum_render(&d, got, FRAME);
            for (s = 0; s < FRAME; s++)
                want[s] = rd16();
            for (s = 0; s < FRAME; s++) {
                total++;
                if (got[s] != want[s]) {
                    bad++;
                    if (!case_bad && badcases < 8) {
                        fprintf(stderr,
                                "case %u frame %u sample %d: got %d want %d "
                                "(speed=%d stage=%d vol=%d)\n",
                                i, j, s, got[s], want[s], d.speed,
                                d.env.stage, d.env.vol);
                        case_bad = 1;
                    }
                }
            }
        }
        if (case_bad)
            badcases++;
        free(wave);
    }
    fclose(g);

    printf("%s: %lu samples over %u cases, %lu mismatches",
           bad ? "FAIL" : "PASS", total, ncases, bad);
    if (bad)
        printf(" in %lu cases", badcases);
    printf("\n");
    return bad ? 1 : 0;
}
