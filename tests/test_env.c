/* Differential test: our envelope against the original ARM code.
 *
 * Vectors from `python3 tools/oracle.py env-vectors`. The envelope advances
 * once per frame, so each record is a single state transition: the four rate
 * parameters, the state before, and the state the original produced.
 *
 * The bar is zero mismatching transitions.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/env.h"

#define MAGIC "ENV1"

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
    const char *path = argc > 1 ? argv[1] : "tests/vectors/env.bin";
    FILE *f = fopen(path, "rb");
    char magic[4];
    uint32_t ncases, i;
    unsigned long bad = 0;

    if (!f) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py env-vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32(f);

    for (i = 0; i < ncases; i++) {
        ds10_env_params p;
        ds10_env e;
        int32_t want_stage, want_level, want_aux;

        p.attack = (int32_t)rd32(f);
        p.decay = (int32_t)rd32(f);
        p.sustain = (int32_t)rd32(f);
        p.release = (int32_t)rd32(f);

        e.stage = (uint8_t)rd32(f);
        e.level = (int32_t)rd32(f);
        e.aux = (int32_t)rd32(f);

        want_stage = (int32_t)rd32(f);
        want_level = (int32_t)rd32(f);
        want_aux = (int32_t)rd32(f);

        ds10_env_step(&e, &p);

        if (e.stage != want_stage || e.level != want_level ||
            e.aux != want_aux) {
            if (bad < 8)
                fprintf(stderr,
                        "case %u: got stage=%d level=%d aux=%d, "
                        "want stage=%d level=%d aux=%d  "
                        "(a=%d d=%d s=%d r=%d)\n",
                        i, e.stage, e.level, e.aux, want_stage, want_level,
                        want_aux, p.attack, p.decay, p.sustain, p.release);
            bad++;
        }
    }
    fclose(f);

    printf("%s: %u frame transitions, %lu mismatches\n",
           bad ? "FAIL" : "PASS", ncases, bad);
    return bad ? 1 : 0;
}
