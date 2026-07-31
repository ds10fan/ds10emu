/* songplay - render a DS-10 song file to a WAV, using only the decompiled core.
 *
 *   songplay [-b bars] [-o out.wav] [-p pattern] song.dat
 *
 * The chain is what the song file says: for each bar, look up the pattern index
 * and hand that pattern to the sequencer when it asks. Everything downstream is
 * the reimplementation - the sequencer's own tick decides when steps happen, its
 * own emission decides which notes go out, and the two synth voices render them.
 *
 * What comes out of the song file: tempo, swing, the bar chain, every step's
 * note, gate, velocity and pan, and both synth patches per pattern.
 *
 * What does NOT, and is therefore approximated here:
 *
 *   - The one-second drum buffers. A drum patch is not played live: the engine
 *     renders it once into a 0x8000-sample buffer (0x0206CE04 does this for all
 *     four drums, then hands each buffer to its voice with root note 60) and the
 *     voice resamples that. The render itself goes through an offline recorder
 *     driven by the sequencer, which is not decompiled, so what is reproduced
 *     here is the inference that the buffer is one second of the synth playing
 *     the drum patch from a fresh voice at note 60. The buffer CONTENT is checked
 *     against the original by make test-songrender; the driving conditions are
 *     not.
 *   - The nine extra bytes per drum at payload +0x2BA8. 0x0206B9DC pushes them
 *     into a second parameter space whose meaning is not established, so they are
 *     ignored.
 *   - The mixer and effect settings. The mixer takes 25 knobs and the pattern
 *     record has 98 bytes not yet named; until the mapping is known the mixer is
 *     set flat and the effect is bypassed.
 *   - Per-step velocity and pan DO come from the song, but the original sends
 *     them to the mixer as knobs 7 and 10, and that knob map is not established
 *     either. They are applied here as a straight channel gain and position.
 *
 * Tick rate. The sequencer's accumulator advances 240000 per call against a
 * period of 60000000/BPM, and 24 sub-ticks make a step pair, which works out at
 * exactly 200 calls per second whatever the tempo. The real rate comes from an
 * OS timer this project has not traced, so 200 Hz is derived rather than
 * observed - 32768/200 is 163.84 samples, and the accumulator below lands on it
 * exactly (1600 per 64-sample frame, 4096 per tick).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/drum.h"
#include "../core/event.h"
#include "../core/mixer.h"
#include "../core/seq.h"
#include "../core/song.h"
#include "../core/tables.h"
#include "../core/voice.h"
#include "../core/voice_state.h"

#define RATE DS10_RATE
#define FRAME DS10_FRAME
#define NSYN 2
#define NTRK DS10_SONG_TRACKS
#define TICK_NUM 1600           /* per 64-sample frame */
#define TICK_DEN 4096           /* per sequencer tick */
#define MAXEV 256
#define MIX_ONE 0x7FFF
#define NDRUM DS10_SONG_DRUMS
#define DRUM_LEN 0x8000        /* one second at 32768 Hz */
#define DRUM_ROOT 60

typedef struct {
    ds10_song_player base;
    const ds10_song *song;
    ds10_rt_pattern *rt;        /* one per song pattern */
    int last;                   /* pattern installed most recently, -1 = none */
    int done;
} song_player;

/* The sequencer reads a pattern with a 104-byte track stride; a song file uses
 * 64 and a different header. Same four bytes per step either way. */
static void build_pattern(ds10_rt_pattern *rt, const ds10_song_pattern *p)
{
    int t, s;

    memset(rt, 0, sizeof *rt);
    rt->steps = (uint8_t)p->steps;
    for (t = 0; t < NTRK; t++)
        for (s = 0; s < DS10_SONG_STEPS; s++) {
            rt->step[t][s][0] = p->step[t][s].note;
            rt->step[t][s][1] = p->step[t][s].gate;
            rt->step[t][s][2] = p->step[t][s].b2;
            rt->step[t][s][3] = p->step[t][s].b3;
        }
}

/* The song player: the sequencer asks for a pattern once per bar and the answer
 * is a pointer, or NULL to stop. */
static void player_handle(ds10_song_player *p, ds10_seq *s)
{
    song_player *sp = (song_player *)p;
    int bar;

    if (p->msg != DS10_MSG_BAR)
        return;
    bar = s->bar;
    if (bar < 0 || bar >= DS10_SONG_CHAIN) {
        sp->done = 1;
        return;
    }
    if (sp->song->chain[bar] == DS10_SONG_NO_PATTERN ||
        sp->song->chain[bar] >= DS10_SONG_PATTERNS) {
        sp->done = 1;
        return;
    }
    p->answer = &sp->rt[sp->song->chain[bar]];
}

static void put16(FILE *f, unsigned v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
}

static void put32(FILE *f, unsigned long v)
{
    put16(f, (unsigned)(v & 0xFFFF));
    put16(f, (unsigned)((v >> 16) & 0xFFFF));
}

static void wav_header(FILE *f, unsigned long frames)
{
    unsigned long data = frames * 4;

    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, RATE);
    put32(f, RATE * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, data);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: songplay [-b bars] [-o out.wav] [-p pattern] [-t tables]"
            " song.dat\n"
            "  -b  bars to render (default: the whole chain)\n"
            "  -p  loop one pattern instead of following the chain\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *out = "song.wav", *tbl = "out/tables.bin", *in = NULL;
    int bars = 0, only = -1, i;
    ds10_table_set ts;
    ds10_pitch_tables pt;
    ds10_vcf_tables ft;
    static ds10_song song;
    static ds10_rt_pattern rt[DS10_SONG_PATTERNS];
    static ds10_voice voice[NSYN];
    static ds10_drum drum[NDRUM];
    static int16_t drumbuf[NDRUM][DRUM_LEN];
    static ds10_mixer mix;
    static int16_t buf[NTRK][FRAME], silence[FRAME];
    static int16_t dl[DS10_FX_BUFLEN], dr[DS10_FX_BUFLEN];
    ds10_seq seq;
    ds10_seq_track trk[NTRK];
    ds10_seq_posted ev[MAXEV];
    ds10_seq_ctx ctx;
    song_player sp;
    const int8_t *chan_vals;
    static int8_t chan[32];
    static uint8_t chan_idx[2 * NTRK];
    const int16_t *chp[DS10_MIX_CHANNELS];
    unsigned char *blob;
    long size;
    FILE *f;
    unsigned long nframes = 0, acc = 0;
    int rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            out = argv[++i];
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            tbl = argv[++i];
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
            bars = atoi(argv[++i]);
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            only = atoi(argv[++i]);
        else if (argv[i][0] == '-')
            usage();
        else
            in = argv[i];
    }
    if (!in)
        usage();

    if (ds10_tables_load(&ts, tbl) != 0) {
        fprintf(stderr, "cannot read %s - run `make tables`\n", tbl);
        return 2;
    }
    ds10_tables_views(&ts.t, &pt, &ft);

    f = fopen(in, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", in);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    blob = malloc((size_t)size);
    if (!blob || fread(blob, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "cannot read %s\n", in);
        return 2;
    }
    fclose(f);
    rc = ds10_song_parse(&song, blob, (size_t)size);
    free(blob);
    if (rc != DS10_SONG_OK) {
        fprintf(stderr, "%s: not a song (status %d)\n", in, rc);
        return 2;
    }

    for (i = 0; i < DS10_SONG_PATTERNS; i++)
        build_pattern(&rt[i], &song.pattern[i]);

    /* Render each drum patch into its one-second buffer, then hand it to the
     * voice the way 0x02072268 does. */
    for (i = 0; i < NDRUM; i++) {
        ds10_voice v;
        int j;

        memset(&v, 0, sizeof v);
        ds10_voice_state_load(&v, ts.voice_init);
        ds10_patch_load(&v, ds10_song_drum_knobs(&song, i), &ts.t);
        ds10_voice_note_on(&v, DRUM_ROOT, &ts.t);
        for (j = 0; j < DRUM_LEN; j += FRAME)
            ds10_voice_render(&v, &pt, &ft, drumbuf[i] + j, FRAME);
        memset(&drum[i], 0, sizeof drum[i]);
        drum[i].env.stage = DS10_DRUM_RELEASE;
        drum[i].env.retrig = 1;
        ds10_drum_set_sample(&drum[i], drumbuf[i], DRUM_LEN, DRUM_ROOT);
    }

    if (bars <= 0)
        bars = only >= 0 ? 8 : ds10_song_bars(&song);
    if (bars <= 0) {
        fprintf(stderr, "%s: empty chain, nothing to play\n", in);
        return 2;
    }

    printf("%s: \"%.6s\"  version %u  %u BPM  swing %u%%  %d of %d bars\n",
           in, song.name, song.version, song.tempo, song.swing, bars,
           ds10_song_bars(&song));

    /* Sequencer. The transport's own start latch does the rest. */
    memset(&seq, 0, sizeof seq);
    memset(trk, 0, sizeof trk);
    seq.incr = DS10_SEQ_INCR;
    seq.ntracks = NTRK;
    seq.steps = DS10_SONG_STEPS;
    ds10_seq_set_tempo(&seq, (int)song.tempo);
    ds10_seq_set_swing(&seq, (int)song.swing);
    /* The original's constructor leaves the accumulator at period-incr+1 for its
     * default 120 BPM, so the very first tick call crosses the period and the
     * first step lands immediately. Setting the tempo afterwards changes the
     * period but not the accumulator. */
    seq.accum = DS10_SEQ_TEMPO_NUM / 120 - DS10_SEQ_INCR + 1;
    seq.playing = 1;
    seq.starting = 1;
    for (i = 0; i < NTRK; i++)
        trk[i].live_note = -1;

    for (i = 0; i < NSYN; i++)
        ds10_voice_state_load(&voice[i], ts.voice_init);

    memset(&sp, 0, sizeof sp);
    sp.base.handle = player_handle;
    sp.song = &song;
    sp.rt = rt;
    sp.last = -1;

    /* The mixer's knob map is unknown, so set its fields directly: flat levels,
     * centre pan, effect bypassed. */
    memset(&mix, 0, sizeof mix);
    for (i = 0; i < DS10_MIX_CHANNELS; i++) {
        mix.vol[i] = MIX_ONE;
        mix.pan[i] = MIX_ONE / 2;
    }
    mix.route = DS10_ROUTE_OFF;
    mix.fx.buf_l = dl;
    mix.fx.buf_r = dr;
    memset(chan, 0x40, sizeof chan);
    for (i = 0; i < NTRK; i++) {
        chan_idx[2 * i] = 0;
        chan_idx[2 * i + 1] = 1;
    }
    chan_vals = chan;

    memset(&ctx, 0, sizeof ctx);
    ctx.s = &seq;
    ctx.trk = trk;
    ctx.pat = 0;                /* the first bar asks the player for one */
    ctx.player = &sp.base;
    ctx.chan = chan_vals;
    ctx.chan_idx = chan_idx;
    ctx.out = ev;
    ctx.max = MAXEV;

    memset(silence, 0, sizeof silence);
    for (i = 0; i < DS10_MIX_CHANNELS; i++)
        chp[i] = i < NTRK ? buf[i] : silence;

    f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", out);
        return 2;
    }
    wav_header(f, 0);

    while (!sp.done && seq.bar < bars) {
        int16_t l[FRAME], r[FRAME];
        int n, k;

        /* Sequencer ticks that fall inside this frame. */
        acc += TICK_NUM;
        while (acc >= TICK_DEN) {
            acc -= TICK_DEN;
            n = ds10_seq_run(&ctx);
            /* A newly installed pattern brings its own two patches, the way
             * 0x0206DFCC reloads every knob on a pattern change. */
            if (ctx.pat != 0 && ctx.pat - rt != sp.last) {
                sp.last = (int)(ctx.pat - rt);
                for (i = 0; i < NSYN; i++)
                    ds10_patch_load(&voice[i], song.pattern[sp.last].knobs[i],
                                    &ts.t);
            }
            for (k = 0; k < n; k++) {
                int dest = ev[k].dest, op = ev[k].opcode;
                int note = (int8_t)(ev[k].payload & 0xFF);

                if (dest < NSYN &&
                    (op == DS10_EV_NOTE_ON || op == DS10_EV_NOTE_OFF)) {
                    ds10_voice_event(&voice[dest], op, note, 0, &ts.t);
                } else if (dest >= NSYN && dest < NTRK &&
                           (op == DS10_EV_NOTE_ON || op == DS10_EV_NOTE_OFF)) {
                    if (op == DS10_EV_NOTE_ON)
                        ds10_drum_note_on(&drum[dest - NSYN], note,
                                          ts.drum_pitch);
                    else
                        ds10_drum_note_off(&drum[dest - NSYN]);
                } else if (dest == DS10_BUS1(NTRK) && op == 6) {
                    /* Channel volume (knob 7) and pan (knob 10). */
                    int ch = ev[k].channel, knob = ev[k].payload & 0xFF;
                    int val = (int8_t)(ev[k].payload >> 8);

                    if (ch < DS10_MIX_CHANNELS && val >= 0) {
                        if (knob == DS10_KNOB_CHAN_VOL)
                            mix.vol[ch] = val * MIX_ONE / 127;
                        else if (knob == DS10_KNOB_CHAN_PAN)
                            mix.pan[ch] = val * MIX_ONE / 127;
                    }
                } else if (op == DS10_EV_BAR_START ||
                           op == DS10_EV_PATTERN_CHANGE) {
                    if (dest < NSYN)
                        ds10_voice_event(&voice[dest], op, 0,
                                         (int32_t)song.tempo, &ts.t);
                }
            }
        }

        for (i = 0; i < NSYN; i++)
            ds10_voice_render(&voice[i], &pt, &ft, buf[i], FRAME);
        for (i = 0; i < NDRUM; i++) {
            memset(buf[NSYN + i], 0, sizeof buf[NSYN + i]);
            ds10_drum_render(&drum[i], buf[NSYN + i], FRAME);
        }
        ds10_mixer_render(&mix, chp, &ts.fx, l, r, FRAME);
        for (k = 0; k < FRAME; k++) {
            put16(f, (unsigned)(uint16_t)l[k]);
            put16(f, (unsigned)(uint16_t)r[k]);
        }
        nframes += FRAME;
        if (only >= 0 && ctx.pat == 0 && only < DS10_SONG_PATTERNS) {
            ctx.pat = &rt[only];
            seq.pattern = 1;
            seq.steps = rt[only].steps;
        }
    }

    fseek(f, 0, SEEK_SET);
    wav_header(f, nframes);
    fclose(f);
    ds10_tables_free(&ts);
    printf("%s: %lu frames, %.2f s\n", out, nframes,
           (double)nframes / RATE);
    return 0;
}
