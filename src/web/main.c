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
    audio_ready = 1;
    return 0;
}

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
