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
 * The four drum voices are in it too. A drum patch is not played live: the engine
 * renders it once into a 0x8000-sample buffer and the voice resamples that, so
 * the generator renders each of the song's four drum patches through a real synth
 * voice and hands the buffer to a real drum voice. The buffers themselves are
 * compared as well, which is what checks that the reimplementation renders a drum
 * patch identically.
 *
 * What is NOT checked is how the original drives that offline render - it goes
 * through a recorder the sequencer drives (0x0206EEA0), which is not decompiled.
 * Both sides here render one second from a freshly constructed voice at note 60,
 * which is the root note 0x0206CE04 hands to the drum voice.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/event.h"
#include "../src/core/seq.h"
#include "../src/core/drum.h"
#include "../src/core/mixer_state.h"
#include "../src/core/song.h"
#include "../src/core/tables.h"
#include "../src/core/voice.h"
#include "../src/core/voice_state.h"

#define MAGIC "SRN1"
#define NSYN 2
#define NDRUM DS10_SONG_DRUMS
#define NCH (NSYN + NDRUM + 2)   /* + the mixer's stereo pair */
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

/* The automation processor pushes lane values straight into the machine through
 * vtable slot +0x18, which for a synth is the knob handler. */
static const ds10_tables *g_tables;

static void knob_hook(void *machine, int knob, int value)
{
    ds10_patch_set_knob((ds10_voice *)machine, knob, value, g_tables);
}

static void player_noop(ds10_song_player *p, ds10_seq *s)
{
    (void)p;
    (void)s;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/songrender.bin";
    const char *tbl = argc > 2 ? argv[2] : "data/tables.bin";
    char magic[4];
    uint32_t ncases, frames, frame, drumlen, i;
    unsigned long total = 0, bad = 0, badcases = 0;
    ds10_table_set ts;
    ds10_pitch_tables pt;
    ds10_vcf_tables ft;

    if (ds10_tables_load(&ts, tbl) != 0) {
        fprintf(stderr, "cannot read %s - run `make tables`\n", tbl);
        return 2;
    }
    ds10_tables_views(&ts.t, &pt, &ft);
    g_tables = &ts.t;

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
    drumlen = rd32();
    if (rd32() != DS10_MIX_STATE_FIELDS || rd32() != DS10_FX_STATE_FIELDS) {
        fprintf(stderr, "mixer state field counts out of sync\n");
        return 2;
    }
    if (frame != DS10_FRAME) {
        fprintf(stderr, "frame %u, expected %d\n", frame, DS10_FRAME);
        return 2;
    }

    for (i = 0; i < ncases; i++) {
        char name[128];
        uint32_t nlen, pidx, tempo, swing, steps, nvf, k, t, s, v;
        unsigned char song_pat[676];
        static ds10_voice voice[NSYN];
        static ds10_drum drum[NDRUM];
        static int16_t dbuf[NDRUM][0x8000], want_buf[0x8000];
        static ds10_mixer mix;
        static int16_t delay_l[DS10_FX_BUFLEN], delay_r[DS10_FX_BUFLEN];
        const int16_t *chp[DS10_MIX_CHANNELS];
        ds10_seq seq;
        ds10_seq_track trk[NTRK];
        ds10_seq_posted ev[MAXEV];
        ds10_seq_ctx ctx;
        ds10_seq_auto autom[NSYN];
        int8_t lane_raw[NSYN * 2 * DS10_SEQ_STEPS];
        const int8_t *dflt[NSYN];
        ds10_rt_pattern pat;
        ds10_song_player player;
        int8_t chan[64];
        int8_t drum_knobs[NDRUM][DS10_SONG_KNOBS];
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
        rd(drum_knobs, sizeof drum_knobs);

        memset(&pat, 0, sizeof pat);
        pat.steps = (uint8_t)steps;
        for (t = 0; t < NTRK; t++)
            for (s = 0; s < DS10_SONG_STEPS; s++)
                {
                    /* File order is gate, note; the runtime wants note, gate. */
                    const unsigned char *r = song_pat + 0x42 + t * 64 + s * 4;

                    pat.step[t][s][0] = (int8_t)r[1];
                    pat.step[t][s][1] = (int8_t)r[0];
                    pat.step[t][s][2] = (int8_t)r[2];
                    pat.step[t][s][3] = (int8_t)r[3];
                }

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
        /* The sequencer reads per-channel volume and pan out of the mixer's own
         * knob array - the pattern's 25 bytes - indexed by that channel's own
         * volume and pan knobs. Velocity is therefore a scale on the mix. */
        memset(chan, 0, sizeof chan);
        memcpy(chan, song_pat + 0x242, DS10_MIX_KNOBS);
        for (t = 0; t < NTRK; t++) {
            chan_idx[2 * t] = (uint8_t)(17 + t);
            chan_idx[2 * t + 1] = (uint8_t)(9 + t);
        }

        /* Automation: two lanes per synth track. The knob ids come from the
         * file's per-step bytes (identical on every step), the offset and scale
         * from the knob descriptors, exactly as 0x0206DEE4 sets them up. */
        memset(autom, 0, sizeof autom);
        for (t = 0; t < NSYN; t++) {
            const unsigned char *ab = song_pat + 0x1C2 + t * 64;

            autom[t].enable = 1;
            autom[t].mode = 0;
            dflt[t] = (const int8_t *)song_pat + 4 + t * 31;
            for (s = 0; s < DS10_SEQ_STEPS; s++) {
                lane_raw[t * 2 * DS10_SEQ_STEPS + s * 2] = (int8_t)ab[s * 4];
                lane_raw[t * 2 * DS10_SEQ_STEPS + s * 2 + 1] =
                    (int8_t)ab[s * 4 + 1];
            }
            for (v = 0; v < DS10_SEQ_LANES; v++) {
                int kn = ab[2 + v];

                autom[t].lane[v].knob = (uint8_t)kn;
                if (kn < 31) {
                    autom[t].lane[v].offset = (int8_t)ts.knob_min[kn];
                    autom[t].lane[v].scale = (uint8_t)ts.knob_range[kn];
                }
            }
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
        ctx.autom = autom;
        ctx.lane_raw = lane_raw;
        ctx.defaults = dflt;
        ctx.nauto = NSYN;
        ctx.knob = knob_hook;
        for (v = 0; v < NSYN; v++)
            ctx.machine[v] = &voice[v];

        /* The patch is already in the captured state - the generator loaded it
         * through the original's own knob handler before snapshotting - so this
         * only has to reproduce the state, not re-derive it. */
        memset(voice, 0, sizeof voice);
        for (v = 0; v < NSYN; v++) {
            int32_t vf[DS10_VOICE_STATE_FIELDS];

            for (k = 0; k < DS10_VOICE_STATE_FIELDS; k++)
                vf[k] = (int32_t)rd32();
            ds10_voice_state_load(&voice[v], vf);
        }
        if (nvf != DS10_VOICE_STATE_FIELDS) {
            fprintf(stderr, "%u voice fields, expected %d\n", nvf,
                    DS10_VOICE_STATE_FIELDS);
            return 2;
        }

        /* The mixer and its effect, as the original left them after the
         * pattern's own 25 knobs and the tempo event went in. */
        {
            int32_t mf[DS10_MIX_STATE_FIELDS], ff[DS10_FX_STATE_FIELDS];

            for (v = 0; v < DS10_MIX_STATE_FIELDS; v++)
                mf[v] = (int32_t)rd32();
            for (v = 0; v < DS10_FX_STATE_FIELDS; v++)
                ff[v] = (int32_t)rd32();
            memset(&mix, 0, sizeof mix);
            memset(delay_l, 0, sizeof delay_l);
            memset(delay_r, 0, sizeof delay_r);
            ds10_mixer_state_load(&mix, mf);
            ds10_fx_state_load(&mix.fx, ff);
            mix.fx.buf_l = delay_l;
            mix.fx.buf_r = delay_r;
        }
        for (v = 0; v < DS10_MIX_CHANNELS; v++)
            chp[v] = 0;

        /* Render the four drum buffers, then check them against the original's
         * before using them. */
        for (v = 0; v < NDRUM; v++) {
            ds10_voice dv;
            uint32_t j;
            unsigned long bad0 = bad;

            memset(&dv, 0, sizeof dv);
            ds10_voice_state_load(&dv, ts.voice_init);
            /* The drum patches are per song, not per pattern. */
            ds10_patch_load(&dv, drum_knobs[v], &ts.t);
            ds10_voice_note_on(&dv, 60, &ts.t);
            for (j = 0; j < drumlen; j += DS10_FRAME)
                ds10_voice_render(&dv, &pt, &ft, dbuf[v] + j, DS10_FRAME);
            for (j = 0; j < drumlen; j++) {
                unsigned char b[2];

                rd(b, 2);
                want_buf[j] = (int16_t)((uint16_t)b[0] | (b[1] << 8));
            }
            for (j = 0; j < drumlen; j++) {
                total++;
                if (dbuf[v][j] != want_buf[j]) {
                    bad++;
                    if (bad == bad0 + 1)
                        fprintf(stderr,
                                "%s drum %u buffer sample %u: got %d want %d\n",
                                name, v, j, dbuf[v][j], want_buf[j]);
                }
            }
            memset(&drum[v], 0, sizeof drum[v]);
            drum[v].env.stage = DS10_DRUM_RELEASE;
            drum[v].env.retrig = 1;
            ds10_drum_set_sample(&drum[v], dbuf[v], (int32_t)drumlen, 60);
        }
        if (bad)
            case_bad = 1;


        for (k = 0; k < frames; k++) {
            int16_t got[NCH][DS10_FRAME], want[DS10_FRAME];
            int n, j;

            acc += TICK_NUM;
            while (acc >= TICK_DEN) {
                acc -= TICK_DEN;
                n = ds10_seq_run(&ctx);
                for (j = 0; j < n; j++) {
                    int dst = ev[j].dest, op = ev[j].opcode;
                    int note = (int8_t)(ev[j].payload & 0xFF);

                    if (op != DS10_EV_NOTE_ON && op != DS10_EV_NOTE_OFF)
                        continue;
                    if (dst < NSYN)
                        ds10_voice_event(&voice[dst], op, note, 0, &ts.t);
                    else if (dst < NTRK) {
                        if (op == DS10_EV_NOTE_ON)
                            ds10_drum_note_on(&drum[dst - NSYN], note,
                                              ts.drum_pitch);
                        else
                            ds10_drum_note_off(&drum[dst - NSYN]);
                    }
                }
                for (j = 0; j < n; j++)
                    if (ev[j].dest == DS10_BUS1(NTRK))
                        ds10_mixer_event(&mix, ev[j].opcode, ev[j].channel,
                                         ev[j].payload, 0, &ts.fx);
                if (ctx.pat == 0) {
                    ctx.pat = &pat;
                    seq.pattern = 1;
                    seq.steps = (uint8_t)steps;
                }
            }
            for (v = 0; v < NSYN; v++)
                ds10_voice_render(&voice[v], &pt, &ft, got[v], DS10_FRAME);
            for (v = 0; v < NDRUM; v++) {
                memset(got[NSYN + v], 0, sizeof got[NSYN + v]);
                ds10_drum_render(&drum[v], got[NSYN + v], DS10_FRAME);
            }
            for (v = 0; v < DS10_MIX_CHANNELS; v++)
                chp[v] = got[v];
            ds10_mixer_render(&mix, chp, &ts.fx, got[NSYN + NDRUM],
                              got[NSYN + NDRUM + 1], DS10_FRAME);

            for (v = 0; v < NCH; v++) {
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
