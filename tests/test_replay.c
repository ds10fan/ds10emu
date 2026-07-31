/* Differential test: replay a pattern out of a real demo song and bit-compare.
 *
 * Vectors from `python3 tools/oracle.py replay-vectors`. Each case installs one
 * pattern of one demo .dat into the original sequencer, starts the transport at
 * that song's own tempo and swing, and calls the real tick 700 times - several
 * bars of music. After every tick it records the whole sequencer state, the
 * note/gate state of all six tracks, and every event the real router posted.
 *
 * This test drives the half of the tick the earlier sequencer test could not
 * reach: with no pattern loaded, the original never reads a step record, never
 * dispatches a play mode, and never posts the per-step velocity and pan.
 *
 * Two things are deliberately the same on both sides rather than checked:
 *
 *   - The song player. Its chain walk is not decompiled, so both sides just put
 *     the pattern straight back whenever the sequencer drops it at a bar end.
 *   - Automation. The enable byte is set so the original skips the processor,
 *     because only the lane mapping is verified and not the modes around it.
 *
 * So this is a replay of one pattern looped, not of a whole arrangement.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/seq.h"

#define MAGIC "RPL1"
#define NF 19
#define NTRK 6
#define TRKF 9
#define MAXEV 256

static FILE *g;

static void rd(void *p, size_t n)
{
    if (fread(p, 1, n, g) != n) {
        fprintf(stderr, "unexpected end of vector file\n");
        exit(2);
    }
}

static uint32_t rd32(void)
{
    unsigned char b[4];

    rd(b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static const char *NAME[NF] = {
    "playing", "starting", "tempo", "period", "accum", "incr", "tick",
    "swing", "step_count", "steps", "step", "subtick", "sub_even", "sub_odd",
    "sub_cur", "auto_sub", "pattern", "bar", "ntracks"
};

static const char *TNAME[TRKF] = {
    "sounding", "pending", "gate_left", "gate_mode", "note", "vel", "pan",
    "live_gate", "live_note"
};

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

static void trkstore(const ds10_seq_track *t, unsigned char *o)
{
    o[0] = t->sounding;
    o[1] = (uint8_t)t->pending;
    o[2] = (uint8_t)t->gate_left;
    o[3] = (uint8_t)t->gate_mode;
    o[4] = (uint8_t)t->note;
    o[5] = (uint8_t)t->vel;
    o[6] = (uint8_t)t->pan;
    o[7] = t->live_gate;
    o[8] = (uint8_t)t->live_note;
}

/* Stands in for the song player: keeps the same pattern going. The reinstall
 * happens outside the tick, in the loop below, so this only has to be harmless -
 * which is exactly what the `bx lr` on the oracle side is. */
static void player_noop(ds10_song_player *p, ds10_seq *s)
{
    (void)p;
    (void)s;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/replay.bin";
    char magic[4];
    uint32_t ncases, ticks, nmach, i;
    unsigned long total = 0, bad = 0, nev = 0;

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py replay-vectors\n", path);
        return 2;
    }
    rd(magic, 4);
    if (memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32();
    ticks = rd32();
    nmach = rd32();
    if (nmach != NTRK) {
        fprintf(stderr, "%u machines, expected %d\n", nmach, NTRK);
        return 2;
    }

    for (i = 0; i < ncases; i++) {
        char name[128];
        uint32_t nlen, pidx, tempo, swing, steps, nsyn, ndrum, k, t, st;
        unsigned char modes[NTRK], song_pat[676];
        signed char chan[16];
        unsigned char idx[16];
        ds10_seq s;
        ds10_seq_track trk[NTRK];
        ds10_rt_pattern pat;
        ds10_song_player player;
        ds10_seq_ctx ctx;
        ds10_seq_posted out[MAXEV], ord[MAXEV];
        int case_bad = 0;

        nlen = rd32();
        if (nlen >= sizeof name) {
            fprintf(stderr, "name too long\n");
            return 2;
        }
        rd(name, nlen);
        name[nlen] = 0;
        pidx = rd32();
        tempo = rd32();
        swing = rd32();
        steps = rd32();
        nsyn = rd32();
        ndrum = rd32();
        (void)nsyn;
        (void)ndrum;
        rd(modes, NTRK);
        rd(chan, 16);
        rd(idx, 16);
        rd(song_pat, sizeof song_pat);

        /* Build the runtime pattern the same way the generator did. */
        memset(&pat, 0, sizeof pat);
        pat.steps = (uint8_t)steps;
        for (t = 0; t < NTRK; t++)
            for (st = 0; st < DS10_SEQ_STEPS; st++)
                memcpy(pat.step[t][st], song_pat + 0x42 + t * 64 + st * 4, 4);

        /* Start from the state the original's constructor left, read out of the
         * vector - guessing at the defaults is how the first run of this test
         * went wrong. */
        memset(&s, 0, sizeof s);
        memset(trk, 0, sizeof trk);
        {
            int32_t f[NF];
            unsigned char it[NTRK][TRKF];

            for (k = 0; k < NF; k++)
                f[k] = (int32_t)rd32();
            rd(it, sizeof it);
            s.playing = (uint8_t)f[0];
            s.starting = (uint8_t)f[1];
            s.tempo = (uint8_t)f[2];
            s.period = f[3];
            s.accum = f[4];
            s.incr = f[5];
            s.tick = f[6];
            s.swing = (uint8_t)f[7];
            s.step_count = f[8];
            s.steps = (uint8_t)f[9];
            s.step = (int8_t)f[10];
            s.subtick = (uint8_t)f[11];
            s.sub_even = (uint8_t)f[12];
            s.sub_odd = (uint8_t)f[13];
            s.sub_cur = (uint8_t)f[14];
            s.auto_sub = (uint8_t)f[15];
            s.pattern = f[16] != 0;
            s.bar = f[17];
            s.ntracks = f[18];
            for (t = 0; t < NTRK; t++) {
                trk[t].sounding = it[t][0];
                trk[t].pending = (int8_t)it[t][1];
                trk[t].gate_left = (int8_t)it[t][2];
                trk[t].gate_mode = (int8_t)it[t][3];
                trk[t].note = (int8_t)it[t][4];
                trk[t].vel = (int8_t)it[t][5];
                trk[t].pan = (int8_t)it[t][6];
                trk[t].live_gate = it[t][7];
                trk[t].live_note = (int8_t)it[t][8];
                trk[t].play_mode = modes[t];
            }
        }

        memset(&player, 0, sizeof player);
        player.handle = player_noop;

        memset(&ctx, 0, sizeof ctx);
        ctx.s = &s;
        ctx.trk = trk;
        ctx.pat = &pat;
        ctx.player = &player;
        ctx.chan = chan;
        ctx.chan_idx = idx;
        ctx.out = out;
        ctx.max = MAXEV;

        for (k = 0; k < ticks; k++) {
            int32_t want[NF], got[NF];
            unsigned char wtrk[NTRK][TRKF], gtrk[NTRK][TRKF];
            uint32_t nwant, e;
            int n;

            n = ds10_seq_run(&ctx);
            /* The generator reads one destination queue after another, so its
             * order is destination-major. Within a destination both sides are
             * chronological, so regroup ours the same way rather than weakening
             * the comparison. */
            {
                int d, j, m = 0;

                for (d = 0; d <= NTRK + 1; d++)
                    for (j = 0; j < n; j++)
                        if (out[j].dest == d)
                            ord[m++] = out[j];
            }
            store(&s, got);
            for (t = 0; t < NTRK; t++)
                trkstore(&trk[t], gtrk[t]);

            for (e = 0; e < NF; e++)
                want[e] = (int32_t)rd32();
            rd(wtrk, sizeof wtrk);
            nwant = rd32();

            for (e = 0; e < NF; e++) {
                /* The pattern field holds a real pointer on the other side, so
                 * only its zero-ness is comparable. */
                int ok = (e == 16) ? ((want[e] != 0) == (got[e] != 0))
                                   : (want[e] == got[e]);

                total++;
                if (!ok) {
                    if (!case_bad)
                        fprintf(stderr, "%s pat %u tick %u: %s got %d want %d\n",
                                name, pidx, k, NAME[e], got[e], want[e]);
                    bad++;
                    case_bad = 1;
                }
            }
            for (t = 0; t < NTRK; t++)
                for (e = 0; e < TRKF; e++) {
                    total++;
                    if (gtrk[t][e] != wtrk[t][e]) {
                        if (!case_bad)
                            fprintf(stderr,
                                    "%s pat %u tick %u trk %u: %s got %d "
                                    "want %d\n", name, pidx, k, t, TNAME[e],
                                    gtrk[t][e], wtrk[t][e]);
                        bad++;
                        case_bad = 1;
                    }
                }

            total++;
            if ((uint32_t)n != nwant) {
                if (!case_bad)
                    fprintf(stderr, "%s pat %u tick %u: %d events, want %u\n",
                            name, pidx, k, n, nwant);
                bad++;
                case_bad = 1;
            }
            for (e = 0; e < nwant; e++) {
                unsigned char w[6];

                rd(w, 6);
                nev++;
                total++;
                if (e >= (uint32_t)n)
                    continue;
                if (ord[e].dest != w[0] || ord[e].opcode != w[1] ||
                    ord[e].channel != w[2] ||
                    ord[e].payload != (uint16_t)(w[4] | (w[5] << 8))) {
                    if (!case_bad)
                        fprintf(stderr,
                                "%s pat %u tick %u ev %u: got %u/%u/%u/%04x "
                                "want %u/%u/%u/%04x\n", name, pidx, k, e,
                                ord[e].dest, ord[e].opcode, ord[e].channel,
                                ord[e].payload, w[0], w[1], w[2],
                                (unsigned)(w[4] | (w[5] << 8)));
                    bad++;
                    case_bad = 1;
                }
            }

            /* The song player stand-in, matching the generator. */
            if (ctx.pat == 0) {
                ctx.pat = &pat;
                s.pattern = 1;
                s.steps = (uint8_t)steps;
            }
        }
    }
    fclose(g);

    printf("%s: %lu checks over %u cases (%lu routed events), %lu mismatches\n",
           bad ? "FAIL" : "PASS", total, ncases, nev, bad);
    return bad ? 1 : 0;
}
