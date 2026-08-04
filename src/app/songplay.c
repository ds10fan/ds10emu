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
 *   - Nothing about the drum buffers: src/core/drumbuf.c renders them the way
 *     0x0206CCC8 does, master stage and per-drum effect included, and it is
 *     bit-exact against a running DS for a dry drum. A fully wet one still
 *     differs in about 4% of its samples, all of them the delay's echo, which
 *     arrives 64 samples early there - TODO.md section 5.
 *   - Automation paths other than the per-step latch. A track whose pattern asks
 *     for one of the others is left un-automated; none of the ROM's demos does.
 *   - The 73 bytes of a pattern after its 25 mixer knobs, at payload +0x25B.
 *     Nothing here reads them.
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
#include "../core/drumbuf.h"
#include "../core/event.h"
#include "../core/mixer_state.h"
#include "../core/seq.h"
#include "../core/song.h"
#include "../core/tables_data.h"
#include "../core/voice.h"
#include "../core/voice_state.h"

#define RATE DS10_RATE
#define FRAME DS10_FRAME
#define NSYN 2
#define NTRK DS10_SONG_TRACKS
#define TICK_NUM 1600           /* per 64-sample frame */
#define TICK_DEN 4096           /* per sequencer tick */
#define MAXEV 256
#define NDRUM DS10_SONG_DRUMS
#define DRUM_LEN 0x8000        /* one second at 32768 Hz */
#define DRUM_ROOT 60

/* The automation processor writes straight into the machine, which for a synth
 * track is its knob handler. */
static void knob_hook(void *machine, int knob, int value)
{
    ds10_patch_set_knob((ds10_voice *)machine, knob, value);
}

typedef struct {
    ds10_song_player base;
    const ds10_song *song;
    ds10_rt_pattern *rt;        /* one per song pattern */
    int last;                   /* pattern installed most recently, -1 = none */
    int done;
    const uint16_t *chain;      /* the arrangement to walk */
    int nbars;
    /* What a pattern change has to re-initialise. */
    ds10_voice *voice;
    ds10_mixer *mix;
    int8_t *chan;
    int8_t *lane_raw;
    const int8_t **dflt;
    const ds10_seq_track *trk;  /* for the velocity latch a volume push folds in */
    int muted;                  /* the mask applied now */
    int bar;                    /* the bar the player last answered for */
} song_player;

/* The song page's per-bar mute, as 0x020330AC and 0x02079D58 apply it: a muted
 * track's channel volume knob goes to zero, and an unmuted one goes back to the
 * pattern's own value. The notes still play - it is the channel that is silent,
 * which is what makes this different from a track's MUTE play mode.
 *
 * Deck A only: deck B is the DSi build's second instance, and its 42..47 knobs
 * are not a space this mixer has. */
#define CHAN_VOL_KNOB 17        /* 17 + channel, the ids at 0x02088080 */

/* The song page's per-bar mute, the way 0x020330AC and 0x02079D58 apply it: a
 * muted track's channel volume goes to zero and an unmuted one goes back to the
 * pattern's own value. The notes still play - it is the channel that falls silent,
 * which is why this is not a track's MUTE play mode.
 *
 * Two halves, because the original's are two halves. The live knob array is
 * written for every channel, since a pattern change has just overwritten it with
 * the pattern's values (0x0206EC10 does the same); the mixer is pushed only for a
 * channel whose state actually changed, which is what 0x02079D58 tests. Pushing
 * every channel every bar would fold in a velocity latch that is stale at a bar
 * boundary and briefly silence tracks nothing muted.
 *
 * Deck A only: deck B is the DSi build's second instance, and its 42..47 knobs are
 * not a space this mixer has.
 */
static void apply_mute(song_player *sp, int bar)
{
    int mask = ds10_song_mute(sp->song, 0, bar);
    const ds10_song_pattern *q;
    int changed, t;

    if (sp->last < 0)
        return;
    changed = mask ^ sp->muted;
    sp->muted = mask;
    q = &sp->song->pattern[sp->last];
    for (t = 0; t < NTRK; t++) {
        int vol = (mask >> t) & 1 ? 0 : q->mixer[CHAN_VOL_KNOB + t];

        sp->chan[CHAN_VOL_KNOB + t] = (int8_t)vol;
        if (changed & (1 << t))
            ds10_mixer_set_knob(sp->mix, CHAN_VOL_KNOB + t,
                                vol * (sp->trk ? sp->trk[t].vel : 0) / 127);
    }
    if (getenv("DS10_TRACE"))
        fprintf(stderr, "[mute] bar %d mask %02x -> vol %d,%d,%d,%d,%d,%d\n",
                bar, mask, sp->chan[17], sp->chan[18], sp->chan[19],
                sp->chan[20], sp->chan[21], sp->chan[22]);
}

/* Everything a pattern change re-initialises, in the order and at the moment the
 * original does it - inside the tick, before the new pattern's first step is
 * processed. 0x0206DFCC reloads every synth knob, 0x0206EC10 pushes all 25 mixer
 * knobs, and 0x0206DEE4 re-initialises both automation lanes of every synth
 * track. Doing this after the tick returned silenced the first note of a song:
 * the step's own volume event had already gone out scaled against a mixer knob
 * array that was still zero. */
static void install_pattern(ds10_seq_ctx *c, ds10_rt_pattern *pat)
{
    song_player *sp = (song_player *)c->user;
    int i = (int)(pat - sp->rt), t, l, st;
    const ds10_song_pattern *q;

    sp->last = i;
    q = &sp->song->pattern[i];

    for (t = 0; t < NSYN; t++)
        ds10_patch_load(&sp->voice[t], q->knobs[t]);
    for (t = 0; t < DS10_MIX_KNOBS; t++)
        ds10_mixer_set_knob(sp->mix, t, q->mixer[t]);
    /* The sequencer reads per-channel volume and pan out of this same array. */
    memcpy(sp->chan, q->mixer, DS10_MIX_KNOBS);
    /* ...which has just overwritten any mute, so re-apply this bar's. */
    apply_mute(sp, sp->bar);

    for (t = 0; t < NSYN; t++) {
        memset(&c->autom[t], 0, sizeof c->autom[t]);
        /* The pattern's own automation path: byte 6 of that track's editor
         * record, 1 for HLD and 2 for SMT, which is what 0x0206DEE4 copies out
         * of the runtime pattern. */
        c->autom[t].enable = q->params[1 + t * 12 + 6];
        sp->dflt[t] = q->knobs[t];
        for (st = 0; st < DS10_SONG_STEPS; st++)
            for (l = 0; l < DS10_SEQ_LANES; l++)
                sp->lane_raw[t * 2 * DS10_SONG_STEPS + st * 2 + l] =
                    q->lane[t][st].raw[l];
        for (l = 0; l < DS10_SEQ_LANES; l++) {
            int kn = (uint8_t)q->lane[t][0].knob[l];

            c->autom[t].lane[l].knob = (uint8_t)kn;
            if (kn < DS10_SONG_KNOBS) {
                c->autom[t].lane[l].offset = (int8_t)ds10_tbl_knob_min[kn];
                c->autom[t].lane[l].scale = (uint8_t)ds10_tbl_knob_range[kn];
            }
        }
    }
}

/* The sequencer reads a pattern with a 104-byte track stride; a song file uses 64
 * and a different header. The step record's first two bytes also swap: the file
 * has gate then note, the runtime note then gate. */
static void build_pattern(ds10_rt_pattern *rt, const ds10_song_pattern *p)
{
    int t, s;

    memset(rt, 0, sizeof *rt);
    rt->steps = (uint8_t)p->steps;
    for (t = 0; t < NTRK; t++)
        for (s = 0; s < DS10_SONG_STEPS; s++) {
            rt->step[t][s][0] = p->step[t][s].note;
            rt->step[t][s][1] = p->step[t][s].gate;
            rt->step[t][s][2] = p->step[t][s].vel;
            rt->step[t][s][3] = p->step[t][s].pan;
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
    if (bar < 0 || bar >= sp->nbars) {
        sp->done = 1;
        return;
    }
    if (sp->chain[bar] == DS10_SONG_NO_PATTERN ||
        sp->chain[bar] >= DS10_SONG_PATTERNS) {
        sp->done = 1;
        return;
    }
    p->answer = &sp->rt[sp->chain[bar]];
    sp->bar = bar;
    apply_mute(sp, bar);
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

/* Per-channel raw output, for tools/songwalk.py: the six machine buffers before
 * the mixer, one file each, so a divergence can be pinned on a track. */
static FILE *chdump[NTRK];

static void open_chdump(const char *prefix)
{
    char path[512];
    int i;

    for (i = 0; i < NTRK; i++) {
        snprintf(path, sizeof path, "%s.ch%d.raw", prefix, i);
        chdump[i] = fopen(path, "wb");
        if (!chdump[i]) {
            fprintf(stderr, "cannot write %s\n", path);
            exit(2);
        }
    }
}

/* The four one-second drum buffers, for tools/songwalk.py. Their content is
 * checked against a running DS by make test-drumbuf; dumping them lets the
 * whole-song comparison hold them constant and test the wiring instead. */
static void dump_drums(const char *prefix, int16_t (*buf)[DRUM_LEN])
{
    char path[512];
    int i;

    for (i = 0; i < NDRUM; i++) {
        FILE *f;

        snprintf(path, sizeof path, "%s.drum%d.raw", prefix, i);
        f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "cannot write %s\n", path);
            exit(2);
        }
        fwrite(buf[i], 2, DRUM_LEN, f);
        fclose(f);
    }
}

static void usage(void)
{
    fprintf(stderr,
            "usage: songplay [-b bars] [-o out.wav] [-p pattern]"
            " song.dat\n"
            "  -b  bars to render (default: the whole chain)\n"
            "  -p  loop one pattern instead of following the chain\n"
            "  -c  channel bitmask, bit 0..5 = syn1 syn2 drum1..4"
            " (default 0x3f)\n"
            "  -a  if the arrangement is shorter than N bars, ignore it and\n"
            "      sweep all 16 patterns in order, twice\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *out = "song.wav", *in = NULL;
    int bars = 0, only = -1, i, chmask = 0x3F, automin = 0;
    const char *drumdump = 0;
    static uint16_t fill[2 * DS10_SONG_PATTERNS];
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
    static ds10_seq_auto autom[NSYN];
    static int8_t lane_raw[NSYN * 2 * DS10_SONG_STEPS];
    const int8_t *dflt[NSYN];
    song_player sp;
    const int8_t *chan_vals;
    static int8_t chan[64];
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
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
            bars = atoi(argv[++i]);
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            only = atoi(argv[++i]);
        else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc)
            open_chdump(argv[++i]);
        else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc)
            drumdump = argv[++i];
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            chmask = (int)strtol(argv[++i], 0, 0);
        else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc)
            automin = atoi(argv[++i]);
        else if (argv[i][0] == '-')
            usage();
        else
            in = argv[i];
    }
    if (!in)
        usage();

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

    memset(&sp, 0, sizeof sp);
    for (i = 0; i < DS10_SONG_PATTERNS; i++)
        build_pattern(&rt[i], &song.pattern[i]);

    /* Render each drum patch into its one-second buffer the way 0x0206CCC8
     * does - through the master stage and with its own nine effect bytes - then
     * hand it to the voice the way 0x02072268 does. */
    for (i = 0; i < NDRUM; i++) {
        static ds10_fx dfx;
        static int16_t dfx_l[DS10_FX_BUFLEN], dfx_r[DS10_FX_BUFLEN];

        memset(&dfx, 0, sizeof dfx);
        memset(dfx_l, 0, sizeof dfx_l);
        memset(dfx_r, 0, sizeof dfx_r);
        ds10_fx_state_load(&dfx, ds10_tbl_fx_init);
        dfx.buf_l = dfx_l;
        dfx.buf_r = dfx_r;
        ds10_drumbuf_render(ds10_song_drum_knobs(&song, i),
                            ds10_song_drum_extra(&song, i), &dfx,
                            drumbuf[i], FRAME);
        memset(&drum[i], 0, sizeof drum[i]);
        drum[i].env.stage = DS10_DRUM_RELEASE;
        drum[i].env.retrig = 1;
        ds10_drum_set_sample(&drum[i], drumbuf[i], DRUM_LEN, DRUM_ROOT);
    }

    /* An unarranged song is a scratchpad of patterns with no chain worth
     * playing, so walk all sixteen twice instead. */
    sp.chain = song.chain;
    sp.nbars = ds10_song_bars(&song);
    if (automin > 0 && sp.nbars < automin) {
        for (i = 0; i < 2 * DS10_SONG_PATTERNS; i++)
            fill[i] = (uint16_t)(i % DS10_SONG_PATTERNS);
        sp.chain = fill;
        sp.nbars = 2 * DS10_SONG_PATTERNS;
        printf("%s: %d-bar arrangement, sweeping all 16 patterns twice\n",
               in, ds10_song_bars(&song));
    }
    if (bars <= 0)
        bars = only >= 0 ? 8 : sp.nbars;
    if (bars <= 0) {
        fprintf(stderr, "%s: empty chain, nothing to play\n", in);
        return 2;
    }

    printf("%s: \"%.6s\"  version %u  %u BPM  swing %u%%  %d bars\n",
           in, song.name, song.version, song.tempo, song.swing, bars);

    /* Sequencer. The transport's own start latch does the rest. */
    memset(&seq, 0, sizeof seq);
    memset(trk, 0, sizeof trk);
    seq.incr = DS10_SEQ_INCR;
    seq.ntracks = NTRK;
    seq.steps = DS10_SONG_STEPS;
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
        ds10_voice_state_load(&voice[i], ds10_tbl_voice_init);

    sp.base.handle = player_handle;
    sp.song = &song;
    sp.rt = rt;
    sp.last = -1;
    sp.muted = 0;
    sp.bar = 0;
    sp.trk = trk;
    sp.voice = voice;
    sp.mix = &mix;
    sp.chan = chan;
    sp.lane_raw = lane_raw;
    sp.dflt = dflt;

        /* The mixer starts from its constructor's state; each pattern's own 25 knobs
     * go in when it is installed. The tempo event is the only way a synced delay
     * learns the BPM - nothing the sequencer broadcasts carries it. */
    memset(&mix, 0, sizeof mix);
    ds10_mixer_state_load(&mix, ds10_tbl_mixer_init);
    ds10_fx_state_load(&mix.fx, ds10_tbl_fx_init);
    mix.fx.buf_l = dl;
    mix.fx.buf_r = dr;
    /* The array the sequencer reads per-channel volume and pan defaults from,
     * indexed through a two-byte-per-track table. It is the mixer's own knob
     * array, so it tracks the pattern. */
    for (i = 0; i < NTRK; i++) {
        chan_idx[2 * i] = (uint8_t)(17 + i);
        chan_idx[2 * i + 1] = (uint8_t)(9 + i);
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
    ctx.autom = autom;
    ctx.lane_raw = lane_raw;
    ctx.defaults = dflt;
    ctx.nauto = NSYN;
    ctx.knob = knob_hook;
    ctx.install = install_pattern;
    ctx.user = &sp;
    for (i = 0; i < NSYN; i++)
        ctx.machine[i] = &voice[i];

    if (drumdump)
        dump_drums(drumdump, drumbuf);

    /* Loading a song pushes the first pattern's patches and mixer knobs before
     * anything plays - 0x0206DFCC and 0x0206EC10 run at load, not at the first
     * tick. Waiting for the tick left our first two frames holding the INIT patch
     * against a zeroed mixer, which is where songwalk found us silent while the
     * original was already sounding. */
    if (only < 0 && sp.nbars > 0 && sp.chain[0] < DS10_SONG_PATTERNS)
        install_pattern(&ctx, &rt[sp.chain[0]]);

    /* Now that the router sink exists, set the tempo the way the sequencer does:
     * it broadcasts opcode 4 to every machine and the mixer, which is the only
     * thing that tells a voice the BPM its synced LFO runs against. */
    {
        int n = ds10_seq_tempo(&ctx, (int)song.tempo);
        int j;

        for (j = 0; j < n; j++) {
            if (ev[j].dest < NSYN)
                ds10_voice_event(&voice[ev[j].dest], ev[j].opcode, 0,
                                 (int32_t)ev[j].payload);
            else if (ev[j].dest == DS10_BUS1(NTRK))
                ds10_mixer_event(&mix, ev[j].opcode, ev[j].channel,
                                 ev[j].payload, (int32_t)ev[j].payload);
        }
    }

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
            for (k = 0; k < n; k++) {
                int dest = ev[k].dest, op = ev[k].opcode;
                int note = (int8_t)(ev[k].payload & 0xFF);

                if (dest < NSYN &&
                    (op == DS10_EV_NOTE_ON || op == DS10_EV_NOTE_OFF)) {
                    if (getenv("DS10_TRACE"))
                        fprintf(stderr, "[ev] f=%lu syn%d %s note=%d\n",
                                nframes / FRAME, dest,
                                op == DS10_EV_NOTE_ON ? "on " : "off", note);
                    ds10_voice_event(&voice[dest], op, note, 0);
                } else if (dest >= NSYN && dest < NTRK &&
                           (op == DS10_EV_NOTE_ON || op == DS10_EV_NOTE_OFF)) {
                    if (op == DS10_EV_NOTE_ON)
                        ds10_drum_note_on(&drum[dest - NSYN], note,
                                          ds10_tbl_drum_pitch);
                    else
                        ds10_drum_note_off(&drum[dest - NSYN]);
                } else if (dest == DS10_BUS1(NTRK)) {
                    ds10_mixer_event(&mix, op, ev[k].channel, ev[k].payload,
                                     0);
                } else if (dest < NSYN) {
                    /* Everything else a voice acts on, the tempo included - the
                     * payload word carries its argument. */
                    ds10_voice_event(&voice[dest], op, 0,
                                     (int32_t)ev[k].payload);
                }
            }
        }

        for (i = 0; i < NSYN; i++)
            ds10_voice_render(&voice[i], buf[i], FRAME);
        for (i = 0; i < NDRUM; i++) {
            memset(buf[NSYN + i], 0, sizeof buf[NSYN + i]);
            ds10_drum_render(&drum[i], buf[NSYN + i], FRAME);
        }
        for (i = 0; i < NTRK; i++)
            if (!(chmask & (1 << i)))
                memset(buf[i], 0, sizeof buf[i]);
        for (i = 0; i < NTRK; i++)
            if (chdump[i])
                fwrite(buf[i], 2, FRAME, chdump[i]);
        ds10_mixer_render(&mix, chp, l, r, FRAME);
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
    printf("%s: %lu frames, %.2f s\n", out, nframes,
           (double)nframes / RATE);
    return 0;
}
