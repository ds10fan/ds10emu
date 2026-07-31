/* The DS-10 interface, on top of the 2D hardware model.
 *
 * A page is one screen of the interface: a tile image and a tilemap out of
 * /data/mainbg, drawn as one background layer, over the shared S_bg backdrop.
 * Which layer, which palette and which VRAM addresses are not choices made
 * here - they are what the game does, read out of the VRAM and registers that
 * ndsref captured, and checked against them by `make test-page`.
 */
#ifndef DS10_APP_UI_H
#define DS10_APP_UI_H

#include <stdint.h>

#include "../gfx/nds2d.h"

typedef struct ds10_ui ds10_ui;

/* `root` is the directory holding mainbg/, mainobj/, data0/ - normally
 * "data/nitrofs". Returns NULL if the shared assets are missing. */
ds10_ui *ds10_ui_new(const char *root);
void ds10_ui_free(ds10_ui *u);

/* Choose what each screen shows, by asset name ("61_SYN_EDIT", "90_MAP").
 * Either may be NULL to leave that screen as it was. Returns 0 on success. */
int ds10_ui_page(ds10_ui *u, const char *bottom, const char *top);

/* Render one frame. Both buffers are 256x192 of 0xAARRGGBB. */
void ds10_ui_frame(ds10_ui *u, uint32_t *top, uint32_t *bottom);

/* Hit-test a touch on the navigation map. Returns the name of the page that
 * box opens, or NULL if the touch is not on one. The map is the only screen
 * whose behaviour is wired up so far. */
const char *ds10_ui_map_hit(int x, int y);

/* Hit-test a touch on the keyboard page. Returns a MIDI note, or -1 if the
 * touch is not on a key. `octave` shifts by twelve, as the OCT buttons do. */
int ds10_ui_key_hit(int x, int y, int octave);

/* Hit-test a touch on the synth edit page. Returns a knob id 0..30, or -1.
 * The name of knob `id`, for display, is ds10_ui_knob_name(). */
int ds10_ui_knob_hit(int x, int y);
const char *ds10_ui_knob_name(int id);

/* Hit-test the step grid on 60_SYN_SEQ_PIANO. Writes the step and the row
 * (0 at the top of the twelve visible), returns 0 on a hit. */
int ds10_ui_seq_hit(int x, int y, int *step, int *row);

/* Hit-test the FX page. Returns the mixer knob id the control drives, or -1.
 * `type` picks which set of three knobs is showing: 0 delay, 1 flanger,
 * 2 chorus. Decompiled from 0x0205e1e4. */
int ds10_ui_fx_hit(int x, int y, int type);

/* Hit-test the keyboard page's top bar. Returns 0 for OCT-, 1 for OCT+,
 * 2 for the synth selector, or -1. Decompiled from 0x0204bff8. */
int ds10_ui_key_button(int x, int y);

/* Hit-test a touch on the mixer page. Returns a mixer knob id - 9..14 are the
 * pans, 17..22 the volumes - or -1. Decompiled from 0x02065b04. */
int ds10_ui_mixer_hit(int x, int y);

/* Set a knob's value and redraw its pointer. The pointer is a rot/scale
 * sprite, so turning it is an affine matrix, not a different tile. */
void ds10_ui_knob_value(ds10_ui *u, int id, int value, int lo, int range);

/* The GPU state, for tests that compare it against a captured frame. */
ds10_gpu *ds10_ui_gpu(ds10_ui *u);

/* The last error, for the caller to report. */
const char *ds10_ui_error(const ds10_ui *u);

#endif /* DS10_APP_UI_H */
