/* Differential test: our sequencer timing core against the original.
 *
 * Vectors from `python3 tools/oracle.py seq-vectors`, in three parts:
 *   - the tempo mapping, taken straight off the original's divide routine for
 *     every BPM from 1 to 300
 *   - the swing mapping, from the original's own 0x0206DB14 for 0..100
 *   - 20 runs of 400 ticks of the real 0x0206E5AC with no pattern loaded, which
 *     isolates the transport, accumulator, step advance and bar counting
 *
 * With no pattern the per-track note and automation work is skipped, so this
 * verifies the timing state machine only. The note, gate and automation lanes
 * are decompiled but not yet implemented - see recon.md section 13.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/seq.h"

#define MAGIC "SEQ2"
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
    "playing", "starting", "tempo", "period", "accum", "incr", "tick",
    "swing", "step_count", "steps", "step", "subtick", "sub_even", "sub_odd",
    "sub_cur", "auto_sub", "pattern", "bar", "ntracks"
};

static void load(ds10_seq *s, const int32_t *f)
{
    s->playing = (uint8_t)f[0];
    s->starting = (uint8_t)f[1];
    s->tempo = (uint8_t)f[2];
    s->period = f[3];
    s->accum = f[4];
    s->incr = f[5];
    s->tick = f[6];
    s->swing = (uint8_t)f[7];
    s->step_count = f[8];
    s->steps = (uint8_t)f[9];
    s->step = (int8_t)f[10];
    s->subtick = (uint8_t)f[11];
    s->sub_even = (uint8_t)f[12];
    s->sub_odd = (uint8_t)f[13];
    s->sub_cur = (uint8_t)f[14];
    s->auto_sub = (uint8_t)f[15];
    s->pattern = f[16];
    s->bar = f[17];
    s->ntracks = f[18];
}

static void store(const ds10_seq *s, int32_t *f)
{
    f[0] = s->playing;
    f[1] = s->starting;
    f[2] = s->tempo;
    f[3] = s->period;
    f[4] = s->accum;
    f[5] = s->incr;
    f[6] = s->tick;
    f[7] = s->swing;
    f[8] = s->step_count;
    f[9] = s->steps;
    f[10] = (uint8_t)s->step;
    f[11] = s->subtick;
    f[12] = s->sub_even;
    f[13] = s->sub_odd;
    f[14] = s->sub_cur;
    f[15] = s->auto_sub;
    f[16] = s->pattern;
    f[17] = s->bar;
    f[18] = s->ntracks;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/seq.bin";
    char magic[4];
    uint32_t ntempo, nswing, nf, nnotes, i, k;
    unsigned long bad = 0, total = 0;
    int32_t nruns;

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py seq-vectors\n", path);
        return 2;
    }
    if (fread(magic, 1, 4, g) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ntempo = rd32();
    nswing = rd32();
    nf = rd32();
    nnotes = rd32();
    if (nf != NF) {
        fprintf(stderr, "vector file has %u fields, expected %d\n", nf, NF);
        return 2;
    }

    /* Note and gate handling: the step trigger, then one sub-tick of the gate
     * countdown, comparing all five track bytes after each plus the event code
     * the countdown returns. */
    for (i = 0; i < nnotes; i++) {
        ds10_seq_track t;
        int32_t before[5], note, gate, sub, wstep[5], wgate[5], wr;
        int32_t got[5];
        int r;

        for (k = 0; k < 5; k++)
            before[k] = (int32_t)rd32();
        note = (int32_t)rd32();
        gate = (int32_t)rd32();
        sub = (int32_t)rd32();
        for (k = 0; k < 5; k++)
            wstep[k] = (int32_t)rd32();
        for (k = 0; k < 5; k++)
            wgate[k] = (int32_t)rd32();
        wr = (int32_t)rd32();

        t.sounding = (uint8_t)before[0];
        t.pending = (int8_t)before[1];
        t.gate_left = (int8_t)before[2];
        t.gate_mode = (int8_t)before[3];
        t.note = (int8_t)before[4];

        ds10_seq_step_track(&t, note, gate, sub);
        got[0] = t.sounding; got[1] = t.pending; got[2] = t.gate_left;
        got[3] = t.gate_mode; got[4] = t.note;
        total++;
        for (k = 0; k < 5; k++) {
            if (got[k] != wstep[k]) {
                if (bad < 10)
                    fprintf(stderr, "note case %u (note=%d gate=%d sub=%d) "
                            "after step: field %u got %d want %d\n",
                            i, note, gate, sub, k, got[k], wstep[k]);
                bad++;
            }
        }

        r = ds10_seq_gate(&t);
        got[0] = t.sounding; got[1] = t.pending; got[2] = t.gate_left;
        got[3] = t.gate_mode; got[4] = t.note;
        total++;
        if (r != wr) {
            if (bad < 10)
                fprintf(stderr, "note case %u: gate event got %d want %d\n",
                        i, r, wr);
            bad++;
        }
        for (k = 0; k < 5; k++) {
            if (got[k] != wgate[k]) {
                if (bad < 10)
                    fprintf(stderr, "note case %u after gate: field %u "
                            "got %d want %d\n", i, k, got[k], wgate[k]);
                bad++;
            }
        }
    }

    for (i = 0; i < ntempo; i++) {
        ds10_seq s;
        int32_t tempo = (int32_t)rd32();
        int32_t want = (int32_t)rd32();

        memset(&s, 0, sizeof s);
        ds10_seq_set_tempo(&s, tempo);
        total++;
        if (s.period != want) {
            if (bad < 8)
                fprintf(stderr, "tempo %d: period got %d want %d\n",
                        tempo, s.period, want);
            bad++;
        }
    }

    for (i = 0; i < nswing; i++) {
        ds10_seq s;
        int32_t w = (int32_t)rd32();
        int32_t we = (int32_t)rd32();
        int32_t wo = (int32_t)rd32();

        memset(&s, 0, sizeof s);
        ds10_seq_set_swing(&s, w);
        total++;
        if (s.sub_even != we || s.sub_odd != wo) {
            if (bad < 8)
                fprintf(stderr, "swing %d: got %d/%d want %d/%d\n",
                        w, s.sub_even, s.sub_odd, we, wo);
            bad++;
        }
    }

    nruns = (int32_t)rd32();
    for (i = 0; i < (uint32_t)nruns; i++) {
        int32_t tempo = (int32_t)rd32();
        int32_t swing = (int32_t)rd32();
        int32_t nticks = (int32_t)rd32();
        int32_t j;

        for (j = 0; j < nticks; j++) {
            ds10_seq s;
            int32_t before[NF], want[NF], got[NF];

            for (k = 0; k < NF; k++)
                before[k] = (int32_t)rd32();
            for (k = 0; k < NF; k++)
                want[k] = (int32_t)rd32();

            memset(&s, 0, sizeof s);
            load(&s, before);
            ds10_seq_tick(&s);
            store(&s, got);
            total++;

            for (k = 0; k < NF; k++) {
                if (got[k] != want[k]) {
                    if (bad < 10)
                        fprintf(stderr,
                                "tempo %d swing %d tick %d: %s got %d want %d\n",
                                tempo, swing, j, NAME[k], got[k], want[k]);
                    bad++;
                }
            }
        }
    }
    fclose(g);

    printf("%s: %lu checks (tempo, swing, notes/gates, %d tick runs), "
           "%lu mismatches\n", bad ? "FAIL" : "PASS", total, nruns, bad);
    return bad ? 1 : 0;
}
