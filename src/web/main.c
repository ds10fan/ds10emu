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
#include "../core/tables.h"
#include "../core/voice.h"
#include "../core/voice_state.h"
#include "../core/seq.h"

#define FB (DS10_SCR_W * DS10_SCR_H)

/* The synth engine, already decompiled and verified bit-exact. Two voices and
 * the mixer, the same objects songplay builds, driven here by the on-screen
 * keyboard instead of by the sequencer. */
#define NSYN 2
#define WEB_FRAME DS10_FRAME

static ds10_ui *ui;
static ds10_table_set tables;
static ds10_pitch_tables pitch_tables;
static ds10_vcf_tables vcf_tables;
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
static ds10_song_player seq_player;

/* The tick asks the song player what to do at a bar line. Ours always answers
 * with the one pattern, so it loops. */
static void seq_handle(ds10_song_player *p, ds10_seq *s)
{
    (void)s;
    if (p->msg == DS10_MSG_BAR) p->answer = &seq_pat;
}
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
    if (ds10_tables_load(&tables, "/data/tables.bin") != 0) {
        snprintf(last_error, sizeof last_error, "cannot load /data/tables.bin");
        return -1;
    }
    ds10_tables_views(&tables.t, &pitch_tables, &vcf_tables);
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
            ds10_voice_state_load(&voice[i], tables.voice_init);
            ds10_patch_load(&voice[i], PATCH, &tables.t);
        }
    }
    ds10_mixer_state_load(&mixer, tables.mixer_init);
    ds10_fx_state_load(&mixer.fx, tables.fx_init);
    mixer.fx.buf_l = delay_l;
    mixer.fx.buf_r = delay_r;
    ds10_mixer_set_knob(&mixer, 0, 5, &tables.fx);         /* effect off */
    for (i = 0; i < NSYN; i++) {
        ds10_mixer_set_knob(&mixer, 9 + i, 64, &tables.fx);   /* centre */
        ds10_mixer_set_knob(&mixer, 17 + i, 127, &tables.fx); /* full */
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
    seq_ctx.out = seq_ev;
    seq_ctx.max = SEQ_MAXEV;
    ds10_seq_set_swing(&seq, 50);
    ds10_seq_tempo(&seq_ctx, 120);

    audio_ready = 1;
    return 0;
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_seq_play(int on)
{
    int i;

    seq_playing = on;
    if (!on)
        for (i = 0; i < NSYN; i++)
            ds10_voice_event(&voice[i], DS10_EV_NOTE_OFF, 0, 0, &tables.t);
}

/* Set or clear one step. `note` below zero clears it; gate 4 is a full step,
 * which is what tapping a cell means. */
EMSCRIPTEN_KEEPALIVE
void ds10_web_seq_step(int track, int step, int note)
{
    if (track < 0 || track >= DS10_SEQ_MAXTRACKS) return;
    if (step < 0 || step >= DS10_SEQ_STEPS) return;
    seq_pat.step[track][step][0] = (int8_t)(note < 0 ? -1 : note);
    seq_pat.step[track][step][1] = (int8_t)(note < 0 ? 0 : 4);
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_seq_note(int track, int step)
{
    if (track < 0 || track >= DS10_SEQ_MAXTRACKS) return -1;
    if (step < 0 || step >= DS10_SEQ_STEPS) return -1;
    if (!seq_pat.step[track][step][1]) return -1;
    return (uint8_t)seq_pat.step[track][step][0];
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_seq_tempo(int bpm) { if (audio_ready) ds10_seq_tempo(&seq_ctx, bpm); }

EMSCRIPTEN_KEEPALIVE
int ds10_web_seq_pos(void) { return seq.step; }

EMSCRIPTEN_KEEPALIVE
void ds10_web_note(int v, int note, int on)
{
    if (!audio_ready || v < 0 || v >= NSYN) return;
    if (on) ds10_voice_note_on(&voice[v], note, &tables.t);
    else ds10_voice_event(&voice[v], DS10_EV_NOTE_OFF, note, 0, &tables.t);
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

                    if (dest < NSYN && (op == DS10_EV_NOTE_ON ||
                                        op == DS10_EV_NOTE_OFF))
                        ds10_voice_event(&voice[dest], op, note, 0, &tables.t);
                }
            }
        }
        for (i = 0; i < NSYN; i++)
            ds10_voice_render(&voice[i], &pitch_tables, &vcf_tables,
                              buf[i], WEB_FRAME);
        for (i = NSYN; i < 6; i++) memset(buf[i], 0, sizeof buf[i]);
        ds10_mixer_render(&mixer, chp, &tables.fx, l, r, WEB_FRAME);
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
    return 0;
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
    for (i = 0; i < NSYN; i++)
        ds10_patch_load(&voice[i], (const int8_t *)(blob + 0x20), &tables.t);
    return 0;
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
    ds10_mixer_set_knob(&mixer, id, value, &tables.fx);
}

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
    return tables.knob_min[id];
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_knob_range(int id)
{
    if (!audio_ready || id < 0 || id > 30) return 1;
    return tables.knob_range[id];
}

/* Apply a knob to both voices, through the engine's own dispatch, and turn
 * its pointer on screen. */
EMSCRIPTEN_KEEPALIVE
void ds10_web_knob_set(int id, int value)
{
    int i;

    if (id < 0 || id > 30) return;
    if (audio_ready) {
        for (i = 0; i < NSYN; i++)
            ds10_patch_set_knob(&voice[i], id, value, &tables.t);
        ds10_ui_knob_value(ui, id, value, tables.knob_min[id],
                           tables.knob_range[id]);
    }
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

EMSCRIPTEN_KEEPALIVE
void ds10_web_render(void)
{
    if (!ui) return;
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
