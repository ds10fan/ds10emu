/* The browser build's entry points.
 *
 * Nothing here is DS-10; it is the thin edge between the reimplementation and
 * JavaScript. The C side owns two 256x192 framebuffers and hands their
 * addresses over; the page copies them into canvases.
 *
 * Assets come from the Emscripten virtual filesystem, preloaded at build time
 * from data/nitrofs, so the same ds10_ui code runs here and natively.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten/emscripten.h>

#include "../app/ui.h"
#include "../core/event.h"
#include "../core/mixer.h"
#include "../core/mixer_state.h"
#include "../core/patch.h"
#include "../core/tables_data.h"
#include "../core/voice.h"
#include "../core/voice_state.h"
#include "../core/seq.h"
#include "../core/song.h"
#include "../app/editor.h"
#include "../core/drum.h"

#define FB (DS10_SCR_W * DS10_SCR_H)

/* The synth engine, already decompiled and verified bit-exact. Two voices and
 * the mixer, the same objects songplay builds, driven here by the on-screen
 * keyboard instead of by the sequencer. */
#define NSYN 2
#define WEB_FRAME DS10_FRAME

static ds10_ui *ui;
static ds10_voice voice[NSYN];
static ds10_mixer mixer;
/* The effect's delay lines. The mixer renders through the effect whatever the
 * route is set to, so these have to exist even with it switched off. */
static int16_t delay_l[DS10_FX_BUFLEN], delay_r[DS10_FX_BUFLEN];

/* The sequencer, verified against the original, driven from the audio loop.
 * One pattern of sixteen steps on synth 1 - enough to be a step sequencer,
 * and every event goes through the same tick the ROM runs. */
#define SEQ_TICK_NUM 1600   /* per 64-sample frame */
#define SEQ_TICK_DEN 4096   /* per sequencer tick */
#define SEQ_MAXEV 64

static ds10_seq seq;
static ds10_seq_track seq_track[DS10_SEQ_MAXTRACKS];
static ds10_seq_ctx seq_ctx;
static ds10_rt_pattern seq_pat;
static ds10_seq_posted seq_ev[SEQ_MAXEV];
static int8_t seq_chan[DS10_MIX_KNOBS];
static uint8_t seq_chan_idx[2 * DS10_SEQ_MAXTRACKS];
static int seq_acc, seq_playing;

/* A loaded song: sixteen patterns, four drum buffers, and the arrangement.
 * All of it goes through the same code songplay uses, which is bit-exact
 * against the original. */
#define NDRUM DS10_SONG_DRUMS
#define DRUM_LEN 0x8000     /* one second at 32768 Hz */
#define DRUM_ROOT 60

/* The song being edited. Playback and editing share it: an edit writes the
 * song record and rebuilds that pattern's runtime copy, so what you hear is
 * always what would be written to a file. */
static ds10_editor ed;
static ds10_rt_pattern song_rt[DS10_SONG_PATTERNS];
static int16_t drumbuf[NDRUM][DRUM_LEN];
static ds10_drum drum[NDRUM];
static ds10_seq_auto seq_auto[NSYN];
static int8_t lane_raw[NSYN * 2 * DS10_SONG_STEPS];
static const int8_t *lane_dflt[NSYN];
static int song_loaded, song_bar, song_bars;

static ds10_song_player seq_player;

/* A pattern's patches and its twenty-five mixer knobs are installed by the
 * sequencer at a bar line. With the transport stopped that never happens, so
 * a freshly loaded song is silent under the keyboard - which is what "neither
 * synth plays anything" was.
 *
 * Installing eagerly is not the answer: the first bar would then install a
 * second time, and the knob handler's side effects depend on what was there
 * before, so playback stopped matching songplay by 5706 samples. Instead the
 * preview paths install once, and only while stopped.
 */
static int preview_ready;

/* Whether selecting a pattern takes effect at once or at the next bar line.
 * The original waits; waiting is also what makes a chained song hold
 * together, so it stays the default. */
static int instant_pattern;

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_instant_pattern(int on) { instant_pattern = on ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE
int ds10_web_instant_pattern(void) { return instant_pattern; }

/* Note-ons the sequencer has dispatched, per track, since anyone last asked.
 * Sound that is missing for one track and not another is either not being
 * emitted or not being heard, and from outside those look the same. */
static uint32_t track_notes[DS10_SONG_TRACKS];

EMSCRIPTEN_KEEPALIVE
int ds10_web_track_notes(int t)
{
    int n;

    if (t < 0 || t >= DS10_SONG_TRACKS) return -1;
    n = (int)track_notes[t];
    track_notes[t] = 0;
    return n;
}

/* Mute and solo.
 *
 * Neither is in the song file. The original keeps them in the sequencer's own
 * per-track block - `+0x22` of the 36-byte record at seq + 0x64 + track*0x24,
 * where 0 plays the pattern and 1 mutes it - so they belong to the running
 * transport and nothing about them is saved. Muting there rather than at the
 * mixer is also why a muted track still answers the keyboard: it is the
 * pattern that is silenced, not the channel.
 */
static uint8_t track_mute[DS10_SONG_TRACKS], track_solo[DS10_SONG_TRACKS];

/* Whether live input is being written into the pattern under the playhead;
 * where the pad was last touched, so 71_KAOSS can draw its cursor there; and
 * which of the pad's three assignments is selected and how often it applies.
 * All of them are the transport's or the screen's, not the song's, and the
 * live redraw needs them, so they are declared here with it. */
static int recording;
static int pad_x = -1, pad_y = -1;
static int pad_slot, pad_rate = 2;

static int track_audible(int track)
{
    int i, any = 0;

    if (track < 0 || track >= DS10_SONG_TRACKS) return 1;
    for (i = 0; i < DS10_SONG_TRACKS; i++) any |= track_solo[i];
    if (any) return track_solo[track];
    return !track_mute[track];
}

/* Whether the transport follows the arrangement or loops the one pattern
 * being edited. Entering notes is done against a looping pattern, which is
 * what the original's pattern mode is for. */
static int song_mode;

/* The tick asks the song player what to do at a bar line. With no song loaded
 * the answer is always the scratch pattern, so it loops; with one, it either
 * repeats the pattern under the cursor or walks the arrangement. */
static void seq_handle(ds10_song_player *p, ds10_seq *s)
{
    if (p->msg != DS10_MSG_BAR) return;
    if (!song_loaded) {
        p->answer = &seq_pat;
        return;
    }
    if (!song_mode || song_bars <= 0) {
        p->answer = &song_rt[ed.pattern % DS10_SONG_PATTERNS];
        return;
    }
    song_bar = s->bar % song_bars;
    p->answer = &song_rt[ed.song.chain[song_bar] % DS10_SONG_PATTERNS];
}

/* What a pattern change re-initialises: both patches, the mixer's 25 knobs,
 * and the automation lanes. It has to run inside the tick - doing it after
 * leaves the first step's volume events scaled against the old mixer. */
static void seq_install(ds10_seq_ctx *c, ds10_rt_pattern *pat)
{
    int i = (int)(pat - song_rt), t, l, st;
    const ds10_song_pattern *q;

    if (!song_loaded || i < 0 || i >= DS10_SONG_PATTERNS) return;
    q = &ed.song.pattern[i];
    for (t = 0; t < NSYN; t++)
        ds10_patch_load(&voice[t], q->knobs[t]);
    for (t = 0; t < DS10_MIX_KNOBS; t++)
        ds10_mixer_set_knob(&mixer, t, q->mixer[t]);
    memcpy(seq_chan, q->mixer, DS10_MIX_KNOBS);

    for (t = 0; t < NSYN; t++) {
        memset(&c->autom[t], 0, sizeof c->autom[t]);
        c->autom[t].enable = 1;
        lane_dflt[t] = q->knobs[t];
        for (st = 0; st < DS10_SONG_STEPS; st++)
            for (l = 0; l < DS10_SEQ_LANES; l++)
                lane_raw[t * 2 * DS10_SONG_STEPS + st * 2 + l] =
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

/* The sequencer reads a pattern with a different stride from the file's, and
 * the step record's first two bytes swap: the file has gate then note. */
static void build_rt(ds10_rt_pattern *rt, const ds10_song_pattern *p)
{
    int t, s;

    memset(rt, 0, sizeof *rt);
    rt->steps = (uint8_t)p->steps;
    for (t = 0; t < DS10_SONG_TRACKS; t++)
        for (s = 0; s < DS10_SONG_STEPS; s++) {
            rt->step[t][s][0] = p->step[t][s].note;
            rt->step[t][s][1] = p->step[t][s].gate;
            rt->step[t][s][2] = p->step[t][s].vel;
            rt->step[t][s][3] = p->step[t][s].pan;
        }
}

/* A drum is not played live: its patch is rendered once into a one-second
 * buffer at note 60 and the voice resamples that. */
static void render_drums(void)
{
    ds10_voice v;
    int i, j;

    for (i = 0; i < NDRUM; i++) {
        memset(&v, 0, sizeof v);
        ds10_voice_state_load(&v, ds10_tbl_voice_init);
        ds10_patch_load(&v, ds10_song_drum_knobs(&ed.song, i));
        ds10_voice_note_on(&v, DRUM_ROOT);
        for (j = 0; j < DRUM_LEN; j += WEB_FRAME)
            ds10_voice_render(&v,
                              drumbuf[i] + j, WEB_FRAME);
        memset(&drum[i], 0, sizeof drum[i]);
        drum[i].env.stage = DS10_DRUM_RELEASE;
        drum[i].env.retrig = 1;
        ds10_drum_set_sample(&drum[i], drumbuf[i], DRUM_LEN, DRUM_ROOT);
    }
}

/* Deliver the events a sequencer call posted.
 *
 * The tempo broadcast has to go through here rather than just being posted:
 * opcode 4 reaches every machine and the mixer, and it is the only thing that
 * tells a voice the BPM its synced LFO runs against. */
static void dispatch(int n)
{
    int j;

    for (j = 0; j < n; j++) {
        if (seq_ev[j].dest < NSYN)
            ds10_voice_event(&voice[seq_ev[j].dest], seq_ev[j].opcode, 0,
                             (int32_t)seq_ev[j].payload);
        else if (seq_ev[j].dest == DS10_BUS1(DS10_SONG_TRACKS))
            ds10_mixer_event(&mixer, seq_ev[j].opcode, seq_ev[j].channel,
                             seq_ev[j].payload, (int32_t)seq_ev[j].payload);
    }
}

/* The automation processor writes straight into the machine, which for a
 * synth track is its knob handler. */
static void seq_knob(void *machine, int knob, int value)
{
    ds10_patch_set_knob((ds10_voice *)machine, knob, value);
}
/* What each screen is showing, so the live redraw knows which page's state to
 * draw, and how far the piano roll is scrolled. */
static char page_bottom[32], page_top[32];
static int seq_top = 69;   /* A4 on the top row; see ds10_ui_seq_grid */

static int audio_ready;
static float audio_out[4096 * 2];
static uint32_t fb_top[FB], fb_bottom[FB];
static char last_error[256];

/* Canvas ImageData wants RGBA bytes in memory order; the renderer produces
 * 0xAARRGGBB words. On a little-endian target that is B,G,R,A, so the red and
 * blue lanes swap on the way out. */
static void to_rgba(uint32_t *p)
{
    int i;

    for (i = 0; i < FB; i++) {
        uint32_t c = p[i];

        p[i] = (c & 0xFF00FF00u) | ((c & 0xFF0000u) >> 16) |
               ((c & 0xFFu) << 16);
    }
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_init(void)
{
    if (ui) return 0;
    ui = ds10_ui_new("/data/nitrofs");
    if (!ui) {
        snprintf(last_error, sizeof last_error,
                 "could not load assets from /data/nitrofs");
        return -1;
    }
    return 0;
}

/* Bring up the synth with the factory patch the keyboard screen starts on. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_audio_init(void)
{
    int i;

    if (audio_ready) return 0;
    memset(voice, 0, sizeof voice);
    memset(&mixer, 0, sizeof mixer);
    /* The tables are compiled in by tools/mktables.py and read by name, so
     * there is nothing to load here and nothing that needs a filesystem -
     * which an AudioWorklet has none of. */
    /* A bare sawtooth with the filter open, so a key is audible before any
     * patch has been chosen. All 31 knobs go in, not a subset: the rest of
     * the constructor's state is not a usable patch on its own. */
    {
        static const int8_t PATCH[31] = {
            [6] = 1,    /* VCO1 saw */
            [7] = 1,    /* VCO2 saw, silent at this balance */
            [11] = 127, /* cutoff wide open */
            [16] = 1,   /* VCA follows the envelope */
            [19] = 110, /* level */
            [21] = 90,  /* decay */
            [22] = 110, /* sustain */
            [23] = 30,  /* release */
        };

        for (i = 0; i < NSYN; i++) {
            ds10_voice_state_load(&voice[i], ds10_tbl_voice_init);
            ds10_patch_load(&voice[i], PATCH);
        }
    }
    ds10_mixer_state_load(&mixer, ds10_tbl_mixer_init);
    ds10_fx_state_load(&mixer.fx, ds10_tbl_fx_init);
    mixer.fx.buf_l = delay_l;
    mixer.fx.buf_r = delay_r;
    ds10_mixer_set_knob(&mixer, 0, 5);         /* effect off */
    for (i = 0; i < NSYN; i++) {
        ds10_mixer_set_knob(&mixer, 9 + i, 64);   /* centre */
        ds10_mixer_set_knob(&mixer, 17 + i, 127); /* full */
    }
    /* The sequencer starts with every step empty: gate 0, note 0xFF. */
    /* The transport fields the constructor does not set. `starting` makes the
     * first tick reset the counters, and the accumulator starts one increment
     * short of the period so the first step lands immediately - both taken
     * from what the original leaves behind. */
    memset(&seq, 0, sizeof seq);
    seq.incr = DS10_SEQ_INCR;
    seq.ntracks = DS10_SEQ_MAXTRACKS;
    seq.steps = DS10_SEQ_STEPS;
    seq.accum = DS10_SEQ_TEMPO_NUM / 120 - DS10_SEQ_INCR + 1;
    seq.playing = 1;
    seq.starting = 1;
    memset(seq_track, 0, sizeof seq_track);
    memset(&seq_pat, 0, sizeof seq_pat);
    seq_pat.steps = DS10_SEQ_STEPS;
    for (i = 0; i < DS10_SEQ_MAXTRACKS; i++) {
        int k;

        /* The runtime pattern's record is note, gate, velocity, pan - the
         * reverse of the song file's first two, which is the same trap that
         * once turned every rest into a tie. 0xFF is no note. */
        for (k = 0; k < DS10_SEQ_STEPS; k++) {
            seq_pat.step[i][k][0] = -1;    /* note 0xFF: nothing here */
            seq_pat.step[i][k][1] = 0;     /* gate */
            seq_pat.step[i][k][2] = 127;   /* velocity */
            seq_pat.step[i][k][3] = 64;    /* pan, centre */
        }
    }
    for (i = 0; i < DS10_SEQ_MAXTRACKS; i++) {
        seq_chan_idx[2 * i] = (uint8_t)(17 + i);
        seq_chan_idx[2 * i + 1] = (uint8_t)(9 + i);
    }
    for (i = 0; i < DS10_MIX_KNOBS; i++) seq_chan[i] = 100;
    memset(&seq_ctx, 0, sizeof seq_ctx);
    seq_ctx.s = &seq;
    seq_ctx.trk = seq_track;
    seq_ctx.pat = &seq_pat;
    seq_ctx.prev = &seq_pat;
    seq_ctx.chan = seq_chan;
    seq_ctx.chan_idx = seq_chan_idx;
    seq_player.handle = seq_handle;
    seq_ctx.player = &seq_player;
    seq_ctx.autom = seq_auto;
    seq_ctx.lane_raw = lane_raw;
    seq_ctx.defaults = lane_dflt;
    seq_ctx.nauto = NSYN;
    seq_ctx.knob = seq_knob;
    for (i = 0; i < NSYN; i++) seq_ctx.machine[i] = &voice[i];
    seq_ctx.install = seq_install;
    seq_ctx.out = seq_ev;
    seq_ctx.max = SEQ_MAXEV;
    ds10_seq_set_swing(&seq, 50);
    ds10_seq_tempo(&seq_ctx, 120);

    audio_ready = 1;
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_song_bytes(const uint8_t *blob, int len);

/* Load a song file into the editor and start it.
 *
 * "ds10init" is the ROM's own blank template, so that is what a new song
 * starts from rather than anything invented here. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_song(const char *name)
{
    char path[256];
    FILE *f;
    static uint8_t blob[16 + 12252];
    long n;

    if (!audio_ready && ds10_web_audio_init() < 0) return -1;
    snprintf(path, sizeof path, "/data/nitrofs/demo/%s.dat", name);
    f = fopen(path, "rb");
    if (!f) {
        /* Songs pulled out of the .SAV files live beside the ROM's own. */
        snprintf(path, sizeof path, "/data/saves/%s.dat", name);
        f = fopen(path, "rb");
    }
    if (!f) {
        snprintf(last_error, sizeof last_error, "cannot open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (long)sizeof blob ||
        fread(blob, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        snprintf(last_error, sizeof last_error, "short read on %s", path);
        return -1;
    }
    fclose(f);
    return ds10_web_song_bytes(blob, (int)n);
}

/* The same from an image already in memory. This is what the page uses for a
 * file the user opened, and what the audio worklet is sent on every edit. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_song_bytes(const uint8_t *blob, int len)
{
    int i;

    if (!audio_ready && ds10_web_audio_init() < 0) return -1;
    if (ds10_editor_init(&ed, blob, (size_t)len) != DS10_SONG_OK) {
        snprintf(last_error, sizeof last_error, "not a song");
        return -1;
    }
    for (i = 0; i < DS10_SONG_PATTERNS; i++)
        build_rt(&song_rt[i], &ed.song.pattern[i]);
    render_drums();

    /* Back to the constructor state before the song's own patches go in. The
     * knob handler has order-dependent side effects - a mod source writes
     * fields a later knob only clears for its own destination - so starting
     * from the scratch patch does not give the same voice as starting fresh. */
    memset(voice, 0, sizeof voice);
    memset(&mixer, 0, sizeof mixer);
    for (i = 0; i < NSYN; i++)
        ds10_voice_state_load(&voice[i], ds10_tbl_voice_init);
    ds10_mixer_state_load(&mixer, ds10_tbl_mixer_init);
    ds10_fx_state_load(&mixer.fx, ds10_tbl_fx_init);
    mixer.fx.buf_l = delay_l;
    mixer.fx.buf_r = delay_r;
    memset(delay_l, 0, sizeof delay_l);
    memset(delay_r, 0, sizeof delay_r);
    memset(seq_track, 0, sizeof seq_track);
    song_bars = ds10_editor_bars(&ed);
    song_bar = 0;
    song_loaded = 1;
    /* A song with an arrangement plays it; the blank template has none, so it
     * comes up looping the pattern under the cursor, ready to be edited. */
    song_mode = song_bars > 0;

    /* Restart the transport and let the first bar install pattern zero. */
    seq.accum = DS10_SEQ_TEMPO_NUM / 120 - DS10_SEQ_INCR + 1;
    seq.playing = 1;
    seq.starting = 1;
    seq_ctx.pat = &song_rt[0];
    seq_ctx.prev = NULL;
    ds10_seq_set_swing(&seq, (int)ed.song.swing);

    dispatch(ds10_seq_tempo(&seq_ctx, (int)ed.song.tempo));
    seq_playing = 1;
    preview_ready = 0;

    /* The map's bottom bar shows what was just loaded. */
    {
        char nm[8];
        int k;

        for (k = 0; k < 6; k++) nm[k] = (char)ed.song.name[k];
        nm[6] = 0;
        for (k = 5; k >= 0 && (nm[k] == ' ' || nm[k] == 0); k--) nm[k] = 0;
        ds10_ui_map_labels(ui, nm, (int)ed.song.swing, (int)ed.song.tempo);
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_song_bars(void) { return song_bars; }

EMSCRIPTEN_KEEPALIVE
int ds10_web_song_bar(void) { return song_bar; }

EMSCRIPTEN_KEEPALIVE
void ds10_web_seq_play(int on)
{
    int i;

    seq_playing = on;
    if (on) preview_ready = 0;   /* the sequencer takes it back over */
    if (!on)
        for (i = 0; i < NSYN; i++)
            ds10_voice_event(&voice[i], DS10_EV_NOTE_OFF, 0, 0);
}

/* The editor.
 *
 * Every edit goes into the song record and then rebuilds that pattern's
 * runtime copy, so the change is audible on the next bar and would survive
 * being written to a file. Nothing is kept anywhere else. */
static void rebuild(int pattern)
{
    if (pattern < 0 || pattern >= DS10_SONG_PATTERNS) return;
    build_rt(&song_rt[pattern], &ed.song.pattern[pattern]);
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_seq_step(int track, int step, int note)
{
    ds10_editor_note(&ed, track, step, note);
    rebuild(ed.pattern);
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_seq_note(int track, int step)
{
    return ds10_editor_get_note(&ed, track, step);
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_step_gate(int track, int step)
{
    return ds10_editor_get_gate(&ed, track, step);
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_step_gate(int track, int step, int v)
{
    ds10_editor_gate(&ed, track, step, v);
    rebuild(ed.pattern);
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_step_vel(int track, int step)
{
    return ds10_editor_get_vel(&ed, track, step);
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_step_vel(int track, int step, int v)
{
    ds10_editor_vel(&ed, track, step, v);
    rebuild(ed.pattern);
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_step_pan(int track, int step)
{
    return ds10_editor_get_pan(&ed, track, step);
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_step_pan(int track, int step, int v)
{
    ds10_editor_pan(&ed, track, step, v);
    rebuild(ed.pattern);
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_pattern(void) { return ed.pattern; }

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_pattern(int p)
{
    if (p < 0 || p >= DS10_SONG_PATTERNS) return;
    ed.pattern = p;
    preview_ready = 0;          /* the new pattern's sound, on the next note */
    if (instant_pattern && song_loaded) {
        seq_ctx.pat = &song_rt[p];
        seq_install(&seq_ctx, &song_rt[p]);
    }
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_track(void) { return ed.track; }

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_track(int t)
{
    if (t < 0 || t >= DS10_SONG_TRACKS) return;
    ed.track = t;
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_pattern_steps(void) { return ds10_editor_get_steps(&ed); }

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_pattern_steps(int n)
{
    ds10_editor_steps(&ed, n);
    rebuild(ed.pattern);
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_pattern_clear(int p)
{
    ds10_editor_pattern_clear(&ed, p < 0 ? ed.pattern : p);
    rebuild(p < 0 ? ed.pattern : p);
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_pattern_copy(int from, int to)
{
    ds10_editor_pattern_copy(&ed, from, to);
    rebuild(to);
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_track_clear(int track)
{
    ds10_editor_track_clear(&ed, ed.pattern, track);
    rebuild(ed.pattern);
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_transpose(int track, int by)
{
    ds10_editor_transpose(&ed, ed.pattern, track, by);
    rebuild(ed.pattern);
}

/* The arrangement. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_chain_get(int bar) { return ds10_editor_chain_get(&ed, bar); }

EMSCRIPTEN_KEEPALIVE
void ds10_web_chain_set(int bar, int pattern)
{
    ds10_editor_chain_set(&ed, bar, pattern);
    song_bars = ds10_editor_bars(&ed);
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_chain_insert(int bar, int pattern)
{
    ds10_editor_chain_insert(&ed, bar, pattern);
    song_bars = ds10_editor_bars(&ed);
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_chain_remove(int bar)
{
    ds10_editor_chain_remove(&ed, bar);
    song_bars = ds10_editor_bars(&ed);
}

/* Follow the arrangement, or loop the pattern being edited. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_song_mode(void) { return song_mode; }

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_song_mode(int on) { song_mode = on ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE
int ds10_web_get_tempo(void) { return (int)ed.song.tempo; }

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_tempo(int bpm)
{
    ds10_editor_tempo(&ed, bpm);
    if (audio_ready) dispatch(ds10_seq_tempo(&seq_ctx, (int)ed.song.tempo));
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_get_swing(void) { return (int)ed.song.swing; }

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_swing(int percent)
{
    ds10_editor_swing(&ed, percent);
    ds10_seq_set_swing(&seq, (int)ed.song.swing);
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_name(const char *name) { ds10_editor_name(&ed, name); }

EMSCRIPTEN_KEEPALIVE
const char *ds10_web_get_name(void)
{
    static char nm[8];
    int k;

    for (k = 0; k < 6; k++) nm[k] = (char)ed.song.name[k];
    nm[6] = 0;
    for (k = 5; k >= 0 && (nm[k] == ' ' || nm[k] == 0); k--) nm[k] = 0;
    return nm;
}

/* A song coming the other way: the page writes into this and says how long
 * it is. A static buffer rather than an allocation, because the audio build
 * exports no allocator - cwrap("malloc") there is undefined, and a worklet
 * that tried it threw on every song and so never played one. */
static uint8_t import_buf[16 + 12252];

EMSCRIPTEN_KEEPALIVE
uint8_t *ds10_web_song_in(void) { return import_buf; }

EMSCRIPTEN_KEEPALIVE
int ds10_web_song_load(int len)
{
    if (len <= 0 || len > (int)sizeof import_buf) return -1;
    return ds10_web_song_bytes(import_buf, len);
}

/* Take an edited copy of the song that is already playing.
 *
 * Loading goes through the whole start-up - voices back to their constructor
 * state, mixer reloaded, transport restarted at bar zero - which is right for
 * a song you just chose and wrong for one you are editing. Sending the whole
 * song after every change therefore restarted it, which is what "changing the
 * volume resets the pattern" was. This replaces the data and leaves the
 * transport, the voices and the mixer exactly where they are.
 */
EMSCRIPTEN_KEEPALIVE
int ds10_web_song_update(int len)
{
    static int8_t last_drums[DS10_SONG_DRUMS * DS10_SONG_KNOBS];
    int8_t now[DS10_SONG_DRUMS * DS10_SONG_KNOBS];
    int i;

    if (len <= 0 || len > (int)sizeof import_buf) return -1;
    if (!song_loaded) return ds10_web_song_bytes(import_buf, len);
    if (ds10_editor_init(&ed, import_buf, (size_t)len) != DS10_SONG_OK)
        return -1;
    for (i = 0; i < DS10_SONG_PATTERNS; i++)
        build_rt(&song_rt[i], &ed.song.pattern[i]);
    song_bars = ds10_editor_bars(&ed);

    /* A drum is a rendered buffer, not a live voice, so it only has to be
     * made again when its patch actually changed - four one-second renders
     * on every slider movement would not keep up. */
    for (i = 0; i < DS10_SONG_DRUMS; i++)
        memcpy(now + i * DS10_SONG_KNOBS, ds10_song_drum_knobs(&ed.song, i),
               DS10_SONG_KNOBS);
    if (memcmp(now, last_drums, sizeof now) != 0) {
        memcpy(last_drums, now, sizeof now);
        render_drums();
    }
    return 0;
}

/* Write the song out as a file the hardware would load, and hand the page its
 * address and length so it can offer it as a download. */
static uint8_t export_buf[16 + 12252];
static int export_len;

EMSCRIPTEN_KEEPALIVE
int ds10_web_export(void)
{
    export_len = (int)ds10_editor_export(&ed, export_buf);
    return export_len;
}

EMSCRIPTEN_KEEPALIVE
uint8_t *ds10_web_export_buf(void) { return export_buf; }

EMSCRIPTEN_KEEPALIVE
void ds10_web_seq_tempo(int bpm) { if (audio_ready) ds10_seq_tempo(&seq_ctx, bpm); }

EMSCRIPTEN_KEEPALIVE
int ds10_web_seq_pos(void) { return seq.step; }

/* Make the current pattern audible for a note played by hand. Only while the
 * transport is stopped: once it is running the sequencer owns this. */
static void ensure_preview(void)
{
    if (preview_ready || seq_playing || !song_loaded) return;
    seq_install(&seq_ctx, &song_rt[ed.pattern % DS10_SONG_PATTERNS]);
    preview_ready = 1;
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_note(int v, int note, int on)
{
    if (!audio_ready || v < 0 || v >= NSYN) return;
    if (on) ensure_preview();
    if (on) ds10_voice_note_on(&voice[v], note);
    else ds10_voice_event(&voice[v], DS10_EV_NOTE_OFF, note, 0);
}

/* Render `frames` samples of stereo into a float buffer for Web Audio.
 * Returns how many were produced. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_audio(int frames)
{
    int16_t buf[6][WEB_FRAME];
    const int16_t *chp[6];
    int16_t l[WEB_FRAME], r[WEB_FRAME];
    int done = 0, i, k;

    if (!audio_ready) return 0;
    if (frames > 4096) frames = 4096;
    for (i = 0; i < 6; i++) chp[i] = buf[i];
    while (done + WEB_FRAME <= frames) {
        /* Sequencer ticks landing in this frame, at the rate the accumulator
         * arithmetic implies: 1600 per frame against 4096 per tick. */
        if (seq_playing) {
            seq_acc += SEQ_TICK_NUM;
            while (seq_acc >= SEQ_TICK_DEN) {
                int n, k;

                seq_acc -= SEQ_TICK_DEN;
                n = ds10_seq_run(&seq_ctx);
                for (k = 0; k < n; k++) {
                    int dest = seq_ev[k].dest, op = seq_ev[k].opcode;
                    int note = (int8_t)(seq_ev[k].payload & 0xFF);

                    /* The same dispatch songplay uses; anything else a
                     * voice acts on - the tempo included - takes its
                     * argument from the payload word. */
                    if (op == DS10_EV_NOTE_ON && dest >= 0 &&
                        dest < DS10_SONG_TRACKS) {
                        track_notes[dest]++;
                        /* A muted track's pattern does not sound. Its
                         * note-offs still go through, or a note held when the
                         * mute went on would never stop. */
                        if (!track_audible(dest)) continue;
                    }
                    if (dest < NSYN && (op == DS10_EV_NOTE_ON ||
                                        op == DS10_EV_NOTE_OFF)) {
                        ds10_voice_event(&voice[dest], op, note, 0);
                    } else if (dest >= NSYN && dest < DS10_SONG_TRACKS &&
                               (op == DS10_EV_NOTE_ON ||
                                op == DS10_EV_NOTE_OFF)) {
                        if (op == DS10_EV_NOTE_ON)
                            ds10_drum_note_on(&drum[dest - NSYN], note,
                                              ds10_tbl_drum_pitch);
                        else
                            ds10_drum_note_off(&drum[dest - NSYN]);
                    } else if (dest == DS10_BUS1(DS10_SONG_TRACKS)) {
                        ds10_mixer_event(&mixer, op, seq_ev[k].channel,
                                         seq_ev[k].payload, 0);
                    } else if (dest < NSYN) {
                        ds10_voice_event(&voice[dest], op, 0,
                                         (int32_t)seq_ev[k].payload);
                    }
                }
            }
        }
        for (i = 0; i < NSYN; i++)
            ds10_voice_render(&voice[i],
                              buf[i], WEB_FRAME);
        for (i = NSYN; i < 6; i++) {
            memset(buf[i], 0, sizeof buf[i]);
            if (song_loaded) ds10_drum_render(&drum[i - NSYN], buf[i],
                                              WEB_FRAME);
        }
        ds10_mixer_render(&mixer, chp, l, r, WEB_FRAME);
        for (k = 0; k < WEB_FRAME; k++) {
            audio_out[(done + k) * 2] = (float)l[k] / 32768.0f;
            audio_out[(done + k) * 2 + 1] = (float)r[k] / 32768.0f;
        }
        done += WEB_FRAME;
    }
    return done;
}

EMSCRIPTEN_KEEPALIVE
float *ds10_web_audio_buf(void) { return audio_out; }

EMSCRIPTEN_KEEPALIVE
int ds10_web_rate(void) { return DS10_RATE; }

EMSCRIPTEN_KEEPALIVE
int ds10_web_page(const char *bottom, const char *top)
{
    if (!ui && ds10_web_init() < 0) return -1;
    if (ds10_ui_page(ui, (bottom && *bottom) ? bottom : NULL,
                     (top && *top) ? top : NULL) < 0) {
        snprintf(last_error, sizeof last_error, "%s", ds10_ui_error(ui));
        return -1;
    }
    if (bottom && *bottom)
        snprintf(page_bottom, sizeof page_bottom, "%s", bottom);
    if (top && *top) snprintf(page_top, sizeof page_top, "%s", top);
    return 0;
}

/* How far the piano roll is scrolled, as the note on its top row. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_seq_top(void) { return seq_top; }

/* The scroll's limits are not read from the ROM - the constructor gives the
 * window's twelve rows but not where the range ends - so they are set wide
 * enough that no note the format can hold is unreachable. The ROM's own songs
 * use 34 to 80, which a narrower guess would have put out of reach. */
EMSCRIPTEN_KEEPALIVE
void ds10_web_set_seq_top(int note)
{
    seq_top = note < 11 ? 11 : note > 127 ? 127 : note;
}

/* Put the track's notes in view. Entering a song means seeing what is already
 * there, and a pattern's notes can sit anywhere in the range. */
EMSCRIPTEN_KEEPALIVE
void ds10_web_seq_follow(void)
{
    int track = ed.track < NSYN ? ed.track : 0;
    int lo = -1, hi = -1, i;

    for (i = 0; i < DS10_SONG_STEPS; i++) {
        int n = ds10_editor_get_note(&ed, track, i);

        if (n < 0) continue;
        if (lo < 0 || n < lo) lo = n;
        if (hi < 0 || n > hi) hi = n;
    }
    if (hi < 0) return;                  /* nothing to look at; leave it */
    /* Show the top of the run, and centre it when the whole run fits. */
    ds10_web_set_seq_top(hi - lo < 12 ? hi + (11 - (hi - lo)) / 2 : hi);
}

/* The note a grid row stands for at the current scroll. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_row_note(int row) { return ds10_ui_seq_row_note(seq_top, row); }

/* The step the transport is on, or -1 when it is stopped.
 *
 * The audio runs in a worklet with its own copy of the engine, and this copy
 * never ticks, so the drawing takes the step from there instead. Without a
 * worklet - the native viewer, the tests - it falls back to this engine. */
static int ui_cursor = -2;      /* -2: nobody has told us, use our own */

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_cursor(int step) { ui_cursor = step; }

/* Which pattern the transport is on. In song mode that is the chain's, not
 * the one selected for editing, and the display follows it - otherwise the
 * notes on screen stay put while different ones are heard. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_play_pattern(void)
{
    if (!song_loaded) return 0;
    if (song_mode && song_bars > 0)
        return (int)(ed.song.chain[song_bar] % DS10_SONG_PATTERNS);
    return ed.pattern;
}

/* Follow the chain: change which pattern is selected without touching the
 * transport or reinstalling anything.
 *
 * In song mode the selection tracks what is sounding, so what is drawn and
 * what an edit lands on are the same pattern. Drawing the playing one while
 * the selection stayed put was worse than either - a tap edited a pattern you
 * were not looking at. In pattern mode the selection is yours and this is not
 * called. */
EMSCRIPTEN_KEEPALIVE
void ds10_web_follow_pattern(int p)
{
    if (p < 0 || p >= DS10_SONG_PATTERNS) return;
    ed.pattern = p;
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_cursor(void)
{
    if (ui_cursor != -2) return ui_cursor;
    return seq_playing ? seq.step : -1;
}

/* Which of the sequencer's six sub-modes is showing. The button that steps
 * them is the page's own, at ds10_ui_seq_mode_hit. */
static int seq_mode;

EMSCRIPTEN_KEEPALIVE
int ds10_web_seq_mode(void) { return seq_mode; }

EMSCRIPTEN_KEEPALIVE
const char *ds10_web_seq_mode_page(void)
{
    return ds10_ui_seq_mode_page(seq_mode);
}

EMSCRIPTEN_KEEPALIVE
const char *ds10_web_seq_mode_next(void)
{
    seq_mode = (seq_mode + 1) % ds10_ui_seq_modes();
    return ds10_ui_seq_mode_page(seq_mode);
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_seq_mode_hit(int x, int y)
{
    return ds10_ui_seq_mode_hit(x, y);
}

/* The value the current sub-mode edits, for a step, or -1 where there is
 * nothing to show.
 *
 * A rest has no value marker on any of these pages: the captured frames put
 * nine markers on GATE, VOLUME and PAN, and those are exactly the nine steps
 * of that pattern which sound. The bytes are still there on a rest - the
 * blank template gives every step gate 3, velocity 127 and pan 64 - so this
 * is the page's choice, not the format's. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_step_value(int track, int step)
{
    if (seq_mode >= 1 && seq_mode <= 3 &&
        ds10_editor_get_note(&ed, track, step) < 0)
        return -1;
    switch (seq_mode) {
    case 1: return ds10_editor_get_gate(&ed, track, step);
    case 2: return ds10_editor_get_vel(&ed, track, step);
    case 3: return ds10_editor_get_pan(&ed, track, step);
    case 4:
    case 5: return ds10_editor_get_lane(&ed, track, seq_mode - 4, step);
    default: return ds10_editor_get_note(&ed, track, step);
    }
}

/* Set it from a row of the grid, which is what a tap on a value page means. */
EMSCRIPTEN_KEEPALIVE
void ds10_web_set_step_row(int step, int row)
{
    int track = ed.track < NSYN ? ed.track : 0;
    int v = ds10_ui_seq_row_value(seq_mode, row);

    switch (seq_mode) {
    case 1: ds10_editor_gate(&ed, track, step, v); break;
    case 2: ds10_editor_vel(&ed, track, step, v); break;
    case 3: ds10_editor_pan(&ed, track, step, v); break;
    case 4:
    case 5:
        /* Tapping the row a lane is already on clears it, so a lane can be
         * emptied - which is what -1 in the raw byte means. */
        ds10_editor_lane(&ed, track, seq_mode - 4, step,
                         ds10_editor_get_lane(&ed, track, seq_mode - 4,
                                              step) == v ? -1 : v);
        break;
    default: return;
    }
    rebuild(ed.pattern);
}

/* Which drum panel a touch is on, or -1. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_drum_hit(int x, int y) { return ds10_ui_drum_hit(x, y); }

/* Trigger a drum, so the selector's panels make a sound. */
EMSCRIPTEN_KEEPALIVE
void ds10_web_drum_hit_play(int n)
{
    if (!song_loaded || n < 0 || n >= NDRUM) return;
    ds10_drum_note_on(&drum[n], DRUM_ROOT, ds10_tbl_drum_pitch);
}

/* Which synth knob a touch on the patch bay drives, or -1. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_patch_hit(int x, int y) { return ds10_ui_patch_hit(x, y); }

/* Load one of the twenty-four factory patches into both voices. Their 31
 * knobs sit at offset 0x20 of a 63-byte ds10synthNN.dat. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_factory_patch(int n)
{
    char path[256];
    FILE *f;
    uint8_t blob[64];
    int i;

    if (!audio_ready || n < 0 || n > 23) return -1;
    snprintf(path, sizeof path, "/data/nitrofs/demo/ds10synth%02d.dat", n);
    f = fopen(path, "rb");
    if (!f) {
        snprintf(last_error, sizeof last_error, "cannot open %s", path);
        return -1;
    }
    if (fread(blob, 1, sizeof blob, f) < 0x3F) {
        fclose(f);
        return -1;
    }
    fclose(f);
    /* Into the pattern as well as the voice, and only the selected track -
     * otherwise the next bar line pushes the pattern's own patch back and the
     * choice is lost. */
    {
        int track = ed.track < NSYN ? ed.track : 0;

        for (i = 0; i < DS10_SONG_KNOBS; i++)
            ds10_editor_knob(&ed, track, i, ((const int8_t *)blob)[0x20 + i]);
        ds10_patch_load(&voice[track], (const int8_t *)(blob + 0x20));
    }
    return 0;
}

/* A touch on the drum grid: returns step | (drum << 8), or -1. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_drumseq_hit(int x, int y)
{
    int step, row;

    if (ds10_ui_drumseq_hit(x, y, &step, &row) < 0) return -1;
    return step | (row << 8);
}

/* A touch on the KAOSS pad: returns vx | (vy << 8), or -1. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_kaoss_hit(int x, int y)
{
    int vx, vy;

    if (ds10_ui_kaoss_hit(x, y, &vx, &vy) < 0) return -1;
    return vx | (vy << 8);
}

/* A touch on the step grid: returns step | (row << 8), or -1. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_seq_hit(int x, int y)
{
    int step, row;

    if (ds10_ui_seq_hit(x, y, &step, &row) < 0) return -1;
    return step | (row << 8);
}

/* Which mixer knob an FX-page touch drives, or -1. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_fx_hit(int x, int y, int type)
{
    return ds10_ui_fx_hit(x, y, type);
}

/* Which of the keyboard page's top-bar buttons a touch is on, or -1. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_key_button(int x, int y) { return ds10_ui_key_button(x, y); }

/* Which mixer control a touch is on, or -1. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_mixer_hit(int x, int y) { return ds10_ui_mixer_hit(x, y); }

/* Set a mixer knob, through the mixer's own dispatch. */
EMSCRIPTEN_KEEPALIVE
void ds10_web_mixer_set(int id, int value)
{
    if (!audio_ready || id < 0 || id >= DS10_MIX_KNOBS) return;
    if (value < 0) value = 0;
    if (value > 127) value = 127;
    /* Into the pattern too, for the same reason as the patch knobs: a bar
     * line pushes the pattern's stored mixer back in. */
    ds10_editor_mixer(&ed, id, value);
    ds10_mixer_set_knob(&mixer, id, value);
    /* And the screen's redraw pass: a volume moves its fader handle. */
    if (id >= 17 && id < 23) ds10_ui_mixer_fader(ui, id - 17, value);
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_mixer_get(int id) { return ds10_editor_get_mixer(&ed, id); }

/* Which knob a touch on the synth edit page is on, or -1. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_knob_hit(int x, int y) { return ds10_ui_knob_hit(x, y); }

EMSCRIPTEN_KEEPALIVE
const char *ds10_web_knob_name(int id) { return ds10_ui_knob_name(id); }

/* A knob's range, from the descriptors the ROM carries. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_knob_min(int id)
{
    if (!audio_ready || id < 0 || id > 30) return 0;
    return ds10_tbl_knob_min[id];
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_knob_range(int id)
{
    if (!audio_ready || id < 0 || id > 30) return 1;
    return ds10_tbl_knob_range[id];
}

/* Apply a knob to both voices, through the engine's own dispatch, and turn
 * its pointer on screen. */
EMSCRIPTEN_KEEPALIVE
/* Turning a knob edits the pattern's patch as well as the live voice. Without
 * that the change would be undone at the next bar line, because installing a
 * pattern pushes its stored patch back into the machine. */
void ds10_web_knob_set(int id, int value)
{
    int track = ed.track < NSYN ? ed.track : 0;

    if (id < 0 || id > 30 || !audio_ready) return;
    if (value < ds10_tbl_knob_min[id]) value = ds10_tbl_knob_min[id];
    if (value > ds10_tbl_knob_min[id] + ds10_tbl_knob_range[id] - 1)
        value = ds10_tbl_knob_min[id] + ds10_tbl_knob_range[id] - 1;
    ds10_editor_knob(&ed, track, id, value);
    ds10_patch_set_knob(&voice[track], id, value);
    ds10_ui_knob_value(ui, id, value, ds10_tbl_knob_min[id],
                       ds10_tbl_knob_range[id]);
}

/* What a knob is set to now, so a drag can start from it instead of jumping
 * the value to wherever the finger landed. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_knob_get(int id)
{
    int track = ed.track < NSYN ? ed.track : 0;

    if (id < 0 || id > 30) return 0;
    return ds10_editor_get_knob(&ed, track, id);
}

/* Which MIDI note a touch on the keyboard page is, or -1. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_key_hit(int x, int y, int octave)
{
    return ds10_ui_key_hit(x, y, octave);
}

/* Which page a touch on the navigation map opens, or "" for none. */
EMSCRIPTEN_KEEPALIVE
const char *ds10_web_map_hit(int x, int y)
{
    const char *p = ds10_ui_map_hit(x, y);

    return p ? p : "";
}

/* Draw the parts of the current page that come from the song rather than from
 * the page itself. This runs every frame, so the screen always agrees with
 * what is loaded - which is what makes it usable for entering a song rather
 * than only for looking at one. */
/* What 52_FX's own tile image already draws as chosen, so a different choice
 * can paint it out. */
#define FX_DEFAULT_TYPE 0     /* DELAY */
#define FX_DEFAULT_ROUTE 2    /* SYN 1 + 2 */
#define PAD_SLOT_W 18         /* the pad's 1, 2 and 3, from 0x02049150 */

/* Light the button on the bar for `id` if the page has one. */
static void bar_led(int id, int on)
{
    int x, y, h;

    if (!on) return;
    if (ds10_ui_bar_box(page_bottom, id, &x, &y, &h) == 0)
        ds10_ui_led(ui, x, y, h);
}

/* Everything on the page whose state is a lit indicator rather than a value:
 * the transport, record, solo, which effect is chosen and where it is routed,
 * the mute and solo strips, and which of the pad's three is selected. */
static void draw_leds(void)
{
    int t, x, y, h, i;

    bar_led(DS10_BAR_PLAY, seq_playing);
    bar_led(DS10_BAR_REC, recording);
    bar_led(DS10_BAR_SOLO, track_solo[ed.track]);
    /* The pad's three: its page comes up with the first one lit, so the
     * choice is a frame and the stale dot is painted out. */
    if (ds10_ui_bar_box(page_bottom, DS10_BAR_SLOT1, &x, &y, &h) == 0) {
        if (pad_slot != 0) ds10_ui_led_off(ui, x, y, h);
        if (ds10_ui_bar_box(page_bottom, DS10_BAR_SLOT1 + pad_slot,
                            &x, &y, &h) == 0)
            ds10_ui_outline(ui, x, y, PAD_SLOT_W, h);
    }
    /* OFF is the pad doing nothing, so its button is dark. */
    bar_led(DS10_BAR_RATE, pad_rate != 0);

    if (strcmp(page_bottom, "52_FX") == 0 ||
        strcmp(page_bottom, "54_DRUM_FX") == 0) {
        /* The page's own image already has one dot lit - the default the
         * screen comes up on - so the one that is really chosen is drawn and
         * the stale one is painted out. */
        for (i = 0; i < 2; i++) {
            int want = ds10_editor_get_mixer(&ed, i ? 0 : 1);
            int stale = i ? FX_DEFAULT_ROUTE : FX_DEFAULT_TYPE;
            int w;

            if (want != stale &&
                (w = ds10_ui_fx_button_box(page_bottom, i, stale, &x, &y, &h)))
                ds10_ui_led_off(ui, x, y, h);
            w = ds10_ui_fx_button_box(page_bottom, i, want, &x, &y, &h);
            if (w) ds10_ui_outline(ui, x, y, w, h);
        }
    }
    for (t = 0; t < DS10_SONG_TRACKS; t++) {
        /* 51_ALL_DRUMS' four rows are the drums, so its strip starts there. */
        i = strcmp(page_bottom, "51_ALL_DRUMS") == 0 ? t - NSYN : t;
        if (i < 0) continue;
        if (track_mute[t] && ds10_ui_ms_box(page_bottom, i, 0, &x, &y, &h) == 0)
            ds10_ui_led(ui, x, y, h);
        if (track_solo[t] && ds10_ui_ms_box(page_bottom, i, 1, &x, &y, &h) == 0)
            ds10_ui_led(ui, x, y, h);
    }
}

static void refresh(void)
{
    int cursor = ds10_web_cursor();
    int track = ed.track < NSYN ? ed.track : 0;
    int i;

    /* The 3D plane is a plain framebuffer under the sprites and nothing else
     * clears it, so every frame starts from empty - otherwise a pad cursor or
     * a lit button would smear across the page it was drawn on. */
    ds10_ui_plane_clear(ui);

    if (strcmp(page_bottom, "60_SYN_SEQ_PIANO") == 0) {
        int note[DS10_SONG_STEPS];

        for (i = 0; i < DS10_SONG_STEPS; i++)
            note[i] = ds10_editor_get_note(&ed, track, i);
        ds10_ui_seq_grid(ui, page_bottom, note, ds10_editor_get_steps(&ed),
                         seq_top, cursor);
    } else if (strncmp(page_bottom, "60_SYN_SEQ_", 11) == 0) {
        int value[DS10_SONG_STEPS];

        for (i = 0; i < DS10_SONG_STEPS; i++)
            value[i] = ds10_web_step_value(track, i);
        ds10_ui_seq_values(ui, value, ds10_editor_get_steps(&ed), seq_mode,
                           cursor);
    } else if (strcmp(page_bottom, "51_ALL_DRUMS") == 0) {
        uint8_t on[DS10_SONG_DRUMS * DS10_SONG_STEPS];
        int r;

        for (r = 0; r < DS10_SONG_DRUMS; r++)
            for (i = 0; i < DS10_SONG_STEPS; i++)
                on[r * DS10_SONG_STEPS + i] =
                    (uint8_t)(ds10_editor_get_note(&ed, NSYN + r, i) >= 0);
        ds10_ui_drum_grid(ui, page_bottom, on, DS10_SONG_DRUMS,
                          ds10_editor_get_steps(&ed), cursor);
    } else if (strcmp(page_bottom, "61_SYN_EDIT") == 0) {
        for (i = 0; i < DS10_SONG_KNOBS; i++)
            ds10_ui_knob_value(ui, i, ds10_editor_get_knob(&ed, track, i),
                               ds10_tbl_knob_min[i], ds10_tbl_knob_range[i]);
    } else if (strcmp(page_bottom, "32_PATTERN") == 0) {
        ds10_ui_pattern_mark(ui, ed.pattern);
    } else if (strcmp(page_bottom, "30_MIXER") == 0) {
        for (i = 0; i < DS10_SONG_TRACKS; i++)
            ds10_ui_mixer_fader(ui, i, ds10_editor_get_mixer(&ed, 17 + i));
    } else if (strcmp(page_bottom, "71_KAOSS") == 0) {
        ds10_ui_kaoss_mark(ui, pad_x, pad_y);
    }
    /* The scrollbar's handle says where in the two octaves the window is, and
     * on a touch screen it is also the only way to move it. */
    if (strncmp(page_bottom, "60_SYN_SEQ_", 11) == 0)
        ds10_ui_seq_scroll_draw(ui, ds10_ui_seq_scroll_pos(seq_top));
    draw_leds();
    /* The map's bottom bar is live too, and drawing the page puts the
     * captured labels back, so it has to be redrawn with it. */
    if (strcmp(page_top, "90_MAP") == 0)
        ds10_ui_map_labels(ui, ds10_web_get_name(), (int)ed.song.swing,
                           (int)ed.song.tempo);
}

/* Which synth the map box under a touch selects, or -1. The map's per-synth
 * pairs are what makes SYN2 EDIT open synth 2's patch. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_map_synth(int x, int y) { return ds10_ui_map_synth(x, y); }

/* Which knob an automation lane drives.
 *
 * The KAOSS pad's two axes are this pair. Across the ROM's songs the lane
 * knob fields are almost entirely 11 and 12 - cutoff and peak - so that is
 * the fallback where a pattern names nothing usable. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_lane_knob(int lane)
{
    int track = ed.track < NSYN ? ed.track : 0;
    int k = ds10_editor_get_lane_knob(&ed, track, lane);

    return k >= 0 && k < DS10_SONG_KNOBS ? k : (lane ? 12 : 11);
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_mapbutton_hit(int x, int y) { return ds10_ui_mapbutton_hit(x, y); }

EMSCRIPTEN_KEEPALIVE
int ds10_web_clear_hit(int x, int y) { return ds10_ui_clear_hit(x, y); }

/* The top bar of whatever page the touch screen is showing. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_bar_hit(int x, int y)
{
    return ds10_ui_bar_hit(page_bottom, x, y);
}

/* The name of a bar button, in the order the enum declares them, so the page
 * can build its own table from this rather than repeating the list. */
EMSCRIPTEN_KEEPALIVE
const char *ds10_web_bar_name(int id)
{
    static const char *const NAME[DS10_BAR_IDS] = {
        "SWAP", "PLAY", "REC", "SOLO", "CLR", "MODE", "SLOT1", "SLOT2",
        "SLOT3", "SET", "RATE", "OCT_DOWN", "OCT_UP", "SYNTH", "COPY",
        "LOCK", "EDIT",
    };

    return id >= 0 && id < DS10_BAR_IDS ? NAME[id] : "";
}

/* The sequencer's scrollbar and its strip of keys. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_scroll_hit(int x, int y) { return ds10_ui_seq_scroll_hit(x, y); }

EMSCRIPTEN_KEEPALIVE
int ds10_web_seq_key_hit(int x, int y) { return ds10_ui_seq_key_hit(x, y); }

EMSCRIPTEN_KEEPALIVE
int ds10_web_scroll_top(int pos) { return ds10_ui_seq_scroll_top(pos); }

/* 52_FX's type and routing buttons: (group << 8) | index, or -1. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_fx_button_hit(int x, int y)
{
    int group = 0, index = ds10_ui_fx_button_hit(page_bottom, x, y, &group);

    return index < 0 ? -1 : (group << 8) | index;
}

/* Mute and solo: (solo << 8) | track, or -1. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_ms_hit(int x, int y)
{
    int track = 0, solo = ds10_ui_ms_hit(page_bottom, x, y, &track);

    if (solo < 0) return -1;
    /* 51_ALL_DRUMS numbers its four rows from the first drum. */
    if (strcmp(page_bottom, "51_ALL_DRUMS") == 0) track += NSYN;
    return (solo << 8) | track;
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_mute(int track)
{
    if (track < 0 || track >= DS10_SONG_TRACKS) return 0;
    return track_mute[track];
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_solo(int track)
{
    if (track < 0 || track >= DS10_SONG_TRACKS) return 0;
    return track_solo[track];
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_mute(int track, int on)
{
    if (track < 0 || track >= DS10_SONG_TRACKS) return;
    track_mute[track] = (uint8_t)(on ? 1 : 0);
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_solo(int track, int on)
{
    if (track < 0 || track >= DS10_SONG_TRACKS) return;
    track_solo[track] = (uint8_t)(on ? 1 : 0);
}

/* Whether a track should sound: any solo anywhere silences the rest. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_track_audible(int track) { return track_audible(track); }

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_pad(int vx, int vy) { pad_x = vx; pad_y = vy; }

/* The pad's three assignments and its rate: the selection is kept here so the
 * DS screen and the HTML panel cannot disagree about it. What each slot drives
 * is the page's business. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_pad_slot(void) { return pad_slot; }

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_pad_slot(int n)
{
    if (n >= 0 && n < 3) pad_slot = n;
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_pad_rate(void) { return pad_rate; }

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_pad_rate(int n)
{
    if (n >= 0 && n < 3) pad_rate = n;
}

/* Recording.
 *
 * The point of it is that the selection follows the chain: with the song
 * playing you can play into whichever pattern is passing, so one take covers
 * all sixteen. So a recorded note goes into the pattern that is *sounding*,
 * not the one selected for editing - in song mode those are the same, and
 * this is what makes them need to be.
 *
 * The worklet does the recording because it is the copy that knows which step
 * is under the playhead; it tells the page, which owns the song that gets
 * written to a file.
 */
EMSCRIPTEN_KEEPALIVE
void ds10_web_set_record(int on) { recording = on ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE
int ds10_web_record(void) { return recording; }

/* Whether the transport is running, so a caller that did not start it - the
 * page's own play button is on the C side's bar now - can tell. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_playing(void) { return seq_playing; }

/* Returns (pattern << 8) | step, or -1 if nothing was recorded. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_record_note(int track, int note)
{
    int pat, step, was;

    if (!recording || !seq_playing || !song_loaded) return -1;
    pat = ds10_web_play_pattern();
    step = seq.step;
    if (step < 0 || step >= DS10_SONG_STEPS) return -1;
    was = ed.pattern;
    ed.pattern = pat;
    ds10_editor_note(&ed, track, step, note);
    ed.pattern = was;
    build_rt(&song_rt[pat], &ed.song.pattern[pat]);
    return (pat << 8) | step;
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_record_lane(int lane, int value)
{
    int pat, step, was, track = ed.track < NSYN ? ed.track : 0;

    if (!recording || !seq_playing || !song_loaded) return -1;
    pat = ds10_web_play_pattern();
    step = seq.step;
    if (step < 0 || step >= DS10_SONG_STEPS) return -1;
    was = ed.pattern;
    ed.pattern = pat;
    ds10_editor_lane(&ed, track, lane, step, value);
    ed.pattern = was;
    build_rt(&song_rt[pat], &ed.song.pattern[pat]);
    return (pat << 8) | step;
}

/* The page applying what the worklet recorded, into its own copy. */
EMSCRIPTEN_KEEPALIVE
void ds10_web_apply_note(int pattern, int track, int step, int note)
{
    int was = ed.pattern;

    if (pattern < 0 || pattern >= DS10_SONG_PATTERNS) return;
    ed.pattern = pattern;
    ds10_editor_note(&ed, track, step, note);
    ed.pattern = was;
    build_rt(&song_rt[pattern], &ed.song.pattern[pattern]);
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_apply_lane(int pattern, int track, int lane, int step, int v)
{
    int was = ed.pattern;

    if (pattern < 0 || pattern >= DS10_SONG_PATTERNS) return;
    ed.pattern = pattern;
    ds10_editor_lane(&ed, track, lane, step, v);
    ed.pattern = was;
    build_rt(&song_rt[pattern], &ed.song.pattern[pattern]);
}

/* A drum's own 31-knob patch. Changing one means re-rendering that drum's
 * one-second buffer, which is what makes it audible. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_drum_knob(int drum, int knob)
{
    return ds10_editor_get_drum_knob(&ed, drum, knob);
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_set_drum_knob(int drum, int knob, int value)
{
    ds10_editor_drum_knob(&ed, drum, knob, value);
}

/* Render the drum buffers again after their patches change. Separate from the
 * setter so a slider drag does not re-render four one-second buffers on every
 * pixel; the caller does it when the finger lifts. */
EMSCRIPTEN_KEEPALIVE
void ds10_web_drums_rerender(void)
{
    if (audio_ready && song_loaded) render_drums();
}

/* Which knob an automation lane drives - the KAOSS pad's two axes. */
EMSCRIPTEN_KEEPALIVE
void ds10_web_set_lane_knob(int lane, int knob)
{
    int track = ed.track < NSYN ? ed.track : 0;

    ds10_editor_lane_knob(&ed, track, lane, knob);
}

/* Which of 32_PATTERN's sixteen buttons is under a touch, or -1. */
EMSCRIPTEN_KEEPALIVE
int ds10_web_pattern_hit(int x, int y) { return ds10_ui_pattern_hit(x, y); }

EMSCRIPTEN_KEEPALIVE
void ds10_web_render(void)
{
    if (!ui) return;
    refresh();
    ds10_ui_frame(ui, fb_top, fb_bottom);
    to_rgba(fb_top);
    to_rgba(fb_bottom);
}

EMSCRIPTEN_KEEPALIVE
uint32_t *ds10_web_top(void) { return fb_top; }

EMSCRIPTEN_KEEPALIVE
uint32_t *ds10_web_bottom(void) { return fb_bottom; }

EMSCRIPTEN_KEEPALIVE
int ds10_web_width(void) { return DS10_SCR_W; }

EMSCRIPTEN_KEEPALIVE
int ds10_web_height(void) { return DS10_SCR_H; }

EMSCRIPTEN_KEEPALIVE
const char *ds10_web_error(void) { return last_error; }

int main(void)
{
    /* Nothing to do at load time: the page drives everything. */
    return 0;
}
