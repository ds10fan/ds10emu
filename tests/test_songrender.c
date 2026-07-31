/* Differential test: render a demo song's pattern as audio and bit-compare.
 *
 * Vectors from `python3 tools/oracle.py songrender-vectors`. Each case wires the
 * original's own sequencer to two of the original's own synth voices through the
 * original's own router, loads the two 31-knob patches the pattern carries, and
 * records both voices' output buffers for 900 frames - about 1.8 seconds, a bar
 * or two of the real song.
 *
 * This is the whole chain at once: song file -> patches -> sequencer -> note and
 * gate emission -> router -> event dispatch -> oscillator, filter, envelope, LFO,
 * portamento, amp. A single wrong bit anywhere shows up here.
 *
 * The four drum tracks are not rendered: a drum voice plays back a buffer the
 * drum synth produces, and that synth is not decompiled. Their sequencer events
 * are still emitted and routed, they just go to stand-in machines on both sides.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/event.h"
#include "../src/core/seq.h"
#include "../src/core/song.h"
#include "../src/core/tables.h"
#include "../src/core/voice.h"
#include "voice_state.h"

#define MAGIC "SRN1"
#define NSYN 2
#define NTRK DS10_SONG_TRACKS
#define MAXEV 256
#define TICK_NUM 1600
#define TICK_DEN 4096
#define NSEQF 19

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

static int32_t next_word(void)
{
    return (int32_t)rd32();
}

static void player_noop(ds10_song_player *p, ds10_seq *s)
{
    (void)p;
    (void)s;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/songrender.bin";
    const char *tbl = argc > 2 ? argv[2] : "out/tables.bin";
    char magic[4];
    uint32_t ncases, frames, frame, i;
    unsigned long total = 0, bad = 0, badcases = 0;
    ds10_table_set ts;
    ds10_pitch_tables pt;
    ds10_vcf_tables ft;

    if (ds10_tables_load(&ts, tbl) != 0) {
        fprintf(stderr, "cannot read %s - run `make tables`\n", tbl);
        return 2;
    }
    ds10_tables_views(&ts.t, &pt, &ft);

    g = fopen(path, "rb");
    if (!g) {
        fprintf(stderr, "cannot open %s\n"
                "run: python3 tools/oracle.py songrender-vectors\n", path);
        return 2;
    }
    rd(magic, 4);
    if (memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        return 2;
    }
    ncases = rd32();
    frames = rd32();
    frame = rd32();
    if (frame != DS10_FRAME) {
        fprintf(stderr, "frame %u, expected %d\n", frame, DS10_FRAME);
        return 2;
    }

    for (i = 0; i < ncases; i++) {
        char name[128];
        uint32_t nlen, pidx, tempo, swing, steps, nvf, k, t, s, v;
        unsigned char song_pat[676];
        static ds10_voice voice[NSYN];
        ds10_seq seq;
        ds10_seq_track trk[NTRK];
        ds10_seq_posted ev[MAXEV];
        ds10_seq_ctx ctx;
        ds10_rt_pattern pat;
        ds10_song_player player;
        int8_t chan[64];
        uint8_t chan_idx[2 * NTRK];
        unsigned long acc = 0;
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
        nvf = rd32();
        rd(song_pat, sizeof song_pat);

        memset(&pat, 0, sizeof pat);
        pat.steps = (uint8_t)steps;
        for (t = 0; t < NTRK; t++)
            for (s = 0; s < DS10_SONG_STEPS; s++)
                memcpy(pat.step[t][s], song_pat + 0x42 + t * 64 + s * 4, 4);

        /* Start from the state the original's constructor left. Its accumulator
         * is not zero, and starting from zero delays the first step by three
         * frames - which is how this test first failed. */
        memset(&seq, 0, sizeof seq);
        memset(trk, 0, sizeof trk);
        {
            int32_t f[NSEQF];

            for (k = 0; k < NSEQF; k++)
                f[k] = (int32_t)rd32();
            seq.playing = (uint8_t)f[0];
            seq.starting = (uint8_t)f[1];
            seq.tempo = (uint8_t)f[2];
            seq.period = f[3];
            seq.accum = f[4];
            seq.incr = f[5];
            seq.tick = f[6];
            seq.swing = (uint8_t)f[7];
            seq.step_count = f[8];
            seq.steps = (uint8_t)f[9];
            seq.step = (int8_t)f[10];
            seq.subtick = (uint8_t)f[11];
            seq.sub_even = (uint8_t)f[12];
            seq.sub_odd = (uint8_t)f[13];
            seq.sub_cur = (uint8_t)f[14];
            seq.auto_sub = (uint8_t)f[15];
            seq.pattern = f[16] != 0;
            seq.bar = f[17];
            seq.ntracks = f[18];
        }
        (void)tempo;
        (void)swing;
        for (t = 0; t < NTRK; t++)
            trk[t].live_note = -1;

        memset(&player, 0, sizeof player);
        player.handle = player_noop;
        memset(chan, 0x40, sizeof chan);
        for (t = 0; t < NTRK; t++) {
            chan_idx[2 * t] = 0;
            chan_idx[2 * t + 1] = 1;
        }

        memset(&ctx, 0, sizeof ctx);
        ctx.s = &seq;
        ctx.trk = trk;
        ctx.pat = &pat;
        ctx.player = &player;
        ctx.chan = chan;
        ctx.chan_idx = chan_idx;
        ctx.out = ev;
        ctx.max = MAXEV;

        /* The patch is already in the captured state - the generator loaded it
         * through the original's own knob handler before snapshotting - so this
         * only has to reproduce the state, not re-derive it. */
        memset(voice, 0, sizeof voice);
        for (v = 0; v < NSYN; v++)
            voice_state_load(&voice[v], next_word);
        (void)nvf;

        for (k = 0; k < frames; k++) {
            int16_t got[NSYN][DS10_FRAME], want[DS10_FRAME];
            int n, j;

            acc += TICK_NUM;
            while (acc >= TICK_DEN) {
                acc -= TICK_DEN;
                n = ds10_seq_run(&ctx);
                for (j = 0; j < n; j++)
                    if (ev[j].dest < NSYN &&
                        (ev[j].opcode == DS10_EV_NOTE_ON ||
                         ev[j].opcode == DS10_EV_NOTE_OFF))
                        ds10_voice_event(&voice[ev[j].dest], ev[j].opcode,
                                         (int8_t)(ev[j].payload & 0xFF), 0,
                                         &ts.t);
                if (ctx.pat == 0) {
                    ctx.pat = &pat;
                    seq.pattern = 1;
                    seq.steps = (uint8_t)steps;
                }
            }
            for (v = 0; v < NSYN; v++)
                ds10_voice_render(&voice[v], &pt, &ft, got[v], DS10_FRAME);

            for (v = 0; v < NSYN; v++) {
                for (s = 0; s < DS10_FRAME; s++) {
                    unsigned char b[2];

                    rd(b, 2);
                    want[s] = (int16_t)((uint16_t)b[0] | (b[1] << 8));
                }
                for (s = 0; s < DS10_FRAME; s++) {
                    total++;
                    if (got[v][s] != want[s]) {
                        bad++;
                        if (!case_bad) {
                            fprintf(stderr,
                                    "%s pat %u voice %u frame %u sample %u: "
                                    "got %d want %d\n", name, pidx, v, k, s,
                                    got[v][s], want[s]);
                            case_bad = 1;
                        }
                    }
                }
            }
        }
        if (case_bad)
            badcases++;
    }
    fclose(g);
    ds10_tables_free(&ts);

    printf("%s: %lu samples over %u cases, %lu mismatches",
           bad ? "FAIL" : "PASS", total, ncases, bad);
    if (bad)
        printf(" in %lu cases", badcases);
    printf("\n");
    return bad ? 1 : 0;
}
