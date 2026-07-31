/* Differential test: our synth event handling against the original dispatcher.
 *
 * Vectors from `python3 tools/oracle.py event-vectors`. Each step posts an event
 * batch into the real queue, calls 0x0207546C directly (which isolates event
 * handling from the DSP), and records every field the dispatcher can touch.
 *
 * Coverage: all nine opcodes individually; note-on/off pairs and legato;
 * pattern-change then bar-start in both orders; tempo changes followed by a bar
 * start, which is the BPM-synced LFO re-phase path; multi-event batches; and
 * ascending and descending notes for the portamento direction flag. Everything
 * is run with LFO BPM sync both off and on.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/event.h"

#define MAGIC "EVT1"
#define NF 19

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
    "env.stage", "env.retrig", "env.level", "env.aux",
    "nc.target", "nc.current", "nc.rate", "nc.dir",
    "lfo.phase_down", "lfo.inc", "lfo.phase_up", "lfo.bpm_sync", "lfo.bpm",
    "lfo.rate_knob", "syn.bpm", "syn.bar_lfo", "syn.bar_env",
    "vco.phase1", "vco.phase2"
};

static void load(ds10_voice *v, const int32_t *f)
{
    v->env.stage = (uint8_t)f[0];
    v->env.retrig = (uint8_t)f[1];
    v->env.level = f[2];
    v->env.aux = f[3];
    v->nc.target = f[4];
    v->nc.current = f[5];
    v->nc.rate = f[6];
    v->nc.dir = (uint8_t)f[7];
    v->lfo.phase_down = f[8];
    v->lfo.inc = f[9];
    v->lfo.phase_up = f[10];
    v->lfo.bpm_sync = (uint8_t)f[11];
    v->lfo.bpm = f[12];
    v->lfo.rate_knob = (int16_t)f[13];
    v->bpm = f[14];
    v->bar_lfo = (int16_t)f[15];
    v->bar_env = (int16_t)f[16];
    v->vco.phase1 = f[17];
    v->vco.phase2 = f[18];
}

static void store(const ds10_voice *v, int32_t *f)
{
    f[0] = v->env.stage;
    f[1] = v->env.retrig;
    f[2] = v->env.level;
    f[3] = v->env.aux;
    f[4] = v->nc.target;
    f[5] = v->nc.current;
    f[6] = v->nc.rate;
    f[7] = v->nc.dir;
    f[8] = v->lfo.phase_down;
    f[9] = v->lfo.inc;
    f[10] = v->lfo.phase_up;
    f[11] = v->lfo.bpm_sync;
    f[12] = v->lfo.bpm;
    f[13] = v->lfo.rate_knob;
    f[14] = v->bpm;
    f[15] = v->bar_lfo;
    f[16] = v->bar_env;
    f[17] = v->vco.phase1;
    f[18] = v->vco.phase2;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/event.bin";
    char magic[4];
    uint32_t nseq, nf, i, k;
    unsigned long bad = 0, total = 0;
    static uint32_t lfo_bpm[16], lfo_free[128];
    ds10_tables t;

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py event-vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, g) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    nseq = rd32();
    nf = rd32();
    if (nf != NF) {
        fprintf(stderr, "vector file has %u fields, expected %d\n", nf, NF);
        return 2;
    }
    for (k = 0; k < 16; k++)
        lfo_bpm[k] = rd32();
    for (k = 0; k < 128; k++)
        lfo_free[k] = rd32();
    memset(&t, 0, sizeof t);
    t.lfo_bpm = lfo_bpm;
    t.lfo_free = lfo_free;

    for (i = 0; i < nseq; i++) {
        int32_t nsteps = (int32_t)rd32();
        int32_t j;

        for (j = 0; j < nsteps; j++) {
            ds10_voice v;
            int32_t nbatch = (int32_t)rd32();
            int32_t ops[8], notes[8], args[8];
            int32_t before[NF], want[NF], got[NF];
            int32_t e;

            for (e = 0; e < nbatch && e < 8; e++) {
                ops[e] = (int32_t)rd32();
                notes[e] = (int32_t)rd32();
                args[e] = (int32_t)rd32();
            }
            for (k = 0; k < NF; k++)
                before[k] = (int32_t)rd32();
            for (k = 0; k < NF; k++)
                want[k] = (int32_t)rd32();

            memset(&v, 0, sizeof v);
            load(&v, before);
            for (e = 0; e < nbatch; e++)
                ds10_voice_event(&v, ops[e], notes[e], args[e], &t);
            store(&v, got);
            total++;

            for (k = 0; k < NF; k++) {
                if (got[k] != want[k]) {
                    if (bad < 12)
                        fprintf(stderr,
                                "seq %u step %d (op=%d): %s got %d want %d\n",
                                i, j, ops[0], NAME[k], got[k], want[k]);
                    bad++;
                }
            }
        }
    }
    fclose(g);

    printf("%s: %lu dispatch calls x %d fields, %lu mismatches\n",
           bad ? "FAIL" : "PASS", total, NF, bad);
    return bad ? 1 : 0;
}
