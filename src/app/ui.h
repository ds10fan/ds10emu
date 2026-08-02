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

/* Which synth the map box under the touch selects, 0 or 1, or -1 where the
 * box is not one of the per-synth pairs. */
int ds10_ui_map_synth(int x, int y);

/* Hit-test a touch on the keyboard page. Returns a MIDI note, or -1 if the
 * touch is not on a key. `octave` shifts by twelve, as the OCT buttons do. */
int ds10_ui_key_hit(int x, int y, int octave);

/* Hit-test a touch on the synth edit page. Returns a knob id 0..30, or -1.
 * The name of knob `id`, for display, is ds10_ui_knob_name(). */
int ds10_ui_knob_hit(int x, int y);
const char *ds10_ui_knob_name(int id);

/* Move the mixer's fader handle for channel `ch` to `value` (0..127) and
 * redraw. This is what the screen's redraw pass does when a fader changes. */
void ds10_ui_mixer_fader(ds10_ui *u, int ch, int value);

/* Where a slider's handle sits for a value, decompiled from 0x02039188.
 * `orient` is 0 for vertical with the value upward, 1 for vertical downward,
 * 3 for horizontal reversed, anything else horizontal. Returns the offset
 * along the span, in pixels from the slider's origin. */
int ds10_ui_slider_pos(int value, int min, int max, int span, int orient);

/* Redraw the navigation map's bottom bar: the session name, the swing and the
 * tempo, at the positions the game puts them. */
void ds10_ui_map_labels(ds10_ui *u, const char *name, int swing, int bpm);

/* Draw a string as one 8x8 sprite per character, into `engine`'s OAM starting
 * at `slot`. Returns the slot after the last one used. Uppercase, digits and
 * a few symbols only - that is all the sheet has. */
int ds10_ui_text(ds10_ui *u, int engine, int slot, int x, int y,
                 const char *s, int palette, int priority);

/* Hit-test the drum selector's four panels. Returns 0..3, or -1.
 * Decompiled from 0x020572b0. */
int ds10_ui_drum_hit(int x, int y);

/* Hit-test the modulation patch bay on 70_PATCH. Returns a synth knob id, or
 * -1. */
int ds10_ui_patch_hit(int x, int y);

/* Hit-test the KAOSS pad. Writes the two axes as 0..127 and returns 0 on a
 * hit. Decompiled from 0x02049150. */
int ds10_ui_kaoss_hit(int x, int y, int *vx, int *vy);

/* Hit-test the drum sequencer's grid on 62_DRUM_SEQ_GATE: sixteen steps by
 * four drums. Decompiled from 0x0205a384. */
int ds10_ui_drumseq_hit(int x, int y, int *step, int *row);

/* Hit-test the step grid on 60_SYN_SEQ_PIANO. Writes the step and the row
 * (0 at the top of the twelve visible), returns 0 on a hit. */
int ds10_ui_seq_hit(int x, int y, int *step, int *row);

/* Draw the synth sequencer's grid from the pattern. `note` is one entry per
 * step, -1 for a rest; `top` is the note on the top row, and notes outside
 * the twelve visible rows are not drawn, as the original's are not. `cursor`
 * is the step the transport is on, or -1 for none. Call after ds10_ui_page:
 * the markers go in the slots the static page left free, so this can be
 * called again on every edit without rebuilding the screen. */
void ds10_ui_seq_grid(ds10_ui *u, const char *page, const int *note,
                      int nsteps, int top, int cursor);

/* The same for the drum grid: `on` is nrows by sixteen, non-zero to light. */
void ds10_ui_drum_grid(ds10_ui *u, const char *page, const uint8_t *on,
                       int nrows, int nsteps, int cursor);

/* The sequencer's value sub-modes: one marker per step at the row for its
 * value. `mode` is the sub-mode index - 1 gate, 2 volume, 3 pan, 4 and 5 the
 * automation lanes. */
void ds10_ui_seq_values(ds10_ui *u, const int *value, int nsteps, int mode,
                        int cursor);

/* How a value maps to a row on those pages, and back. -1 for a value that has
 * no row. */
int ds10_ui_seq_mode_rows(int mode);
int ds10_ui_seq_row_value(int mode, int row);
int ds10_ui_seq_value_row(int mode, int value);

/* The button that brings the navigation map to the touch screen, and the one
 * that raises the CLEAR ALL DATA prompt. Both are on the sequencer's top bar
 * and neither is in a table; see tools/probe_buttons.py. */
int ds10_ui_mapbutton_hit(int x, int y);
int ds10_ui_clear_hit(int x, int y);

/* The strip of buttons along the top of nearly every page.
 *
 * Every page draws its own set into the same slots, and the slot does not fix
 * the role: x 162 is SOLO on the sequencer, CLR on 51_ALL_DRUMS and RECORD on
 * the keyboard. So the layout is a table per page, and it is read off the
 * rendered pages by tools/probe_frames.py, which finds each button by the
 * frame the page's own tile image draws around it.
 */
enum {
    DS10_BAR_NONE = -1,
    DS10_BAR_SWAP = 0,   /* the up and down arrows: exchange the two screens */
    DS10_BAR_PLAY,
    DS10_BAR_REC,
    DS10_BAR_SOLO,
    DS10_BAR_CLR,
    DS10_BAR_MODE,       /* the sequencer's sub-mode cycle */
    DS10_BAR_SLOT1,      /* the KAOSS pad's three assignments */
    DS10_BAR_SLOT2,
    DS10_BAR_SLOT3,
    DS10_BAR_SET,        /* the pad's key and scale */
    DS10_BAR_RATE,       /* SMT / HLD */
    DS10_BAR_OCT_DOWN,
    DS10_BAR_OCT_UP,
    DS10_BAR_SYNTH,      /* the keyboard's synth selector */
    DS10_BAR_COPY,
    DS10_BAR_LOCK,
    DS10_BAR_EDIT,
    DS10_BAR_IDS
};

/* Which of them is under the touch, or DS10_BAR_NONE. */
int ds10_ui_bar_hit(const char *page, int x, int y);

/* Where that button is, and how tall, so its indicator can be lit. Returns 0
 * if the page has one. */
int ds10_ui_bar_box(const char *page, int id, int *x, int *y, int *h);

/* Light a button's indicator - the small dot every button carries at its left,
 * which the page draws dark and turns red when the button is engaged. `bh` is
 * the button's height, because the dot is centred in it. Drawn on the 3D
 * plane, which is cleared per frame. */
void ds10_ui_led(ds10_ui *u, int x, int y, int bh);

/* And put one out, for a page whose tile image draws a default as chosen -
 * 52_FX comes up with DELAY and SYN 1 + 2 lit whatever the song says. */
void ds10_ui_led_off(ds10_ui *u, int x, int y, int bh);

/* A red frame round a button, which is how one of a group is shown chosen. */
void ds10_ui_outline(ds10_ui *u, int x, int y, int bw, int bh);
void ds10_ui_plane_clear(ds10_ui *u);

/* The sequencer's scrollbar, down the left of the grid. The window is twelve
 * of twenty-four rows, so the handle has thirteen positions; the hit returns
 * the top row, 0 at the top, or -1 off the bar. */
int ds10_ui_seq_scroll_hit(int x, int y);
void ds10_ui_seq_scroll_draw(ds10_ui *u, int pos);

/* The original's stored 0..84 to and from a row position, and the note on the
 * top row for one. The stored value counts upward; the position counts down
 * from the top of the range, seven units to a row. */
int ds10_ui_seq_scroll_value(int pos);
int ds10_ui_seq_scroll_from_value(int value);

/* The note on the top row for a scroll position, and back. */
int ds10_ui_seq_scroll_top(int pos);
int ds10_ui_seq_scroll_pos(int top);

/* The strip of piano keys between the scrollbar and the grid. Returns the row
 * 0..11, or -1. Tapping one sounds that row's note on the original. */
int ds10_ui_seq_key_hit(int x, int y);

/* 52_FX and 54_DRUM_FX's buttons, from the tables at 0x020869ac and
 * 0x02086808: `group` comes back 0 for the effect type and 1 for the routing,
 * and the return is the index within it - which is the value the mixer knob
 * takes. -1 for a touch on neither. */
int ds10_ui_fx_button_hit(const char *page, int x, int y, int *group);

/* Where one of them is. Returns its width, or 0 if the page has no such
 * button. */
int ds10_ui_fx_button_box(const char *page, int group, int index,
                          int *x, int *y, int *h);

/* The mute and solo strips on 30_MIXER, 32_PATTERN and 51_ALL_DRUMS. Writes
 * the track and returns 0 for mute, 1 for solo, -1 for neither. */
int ds10_ui_ms_hit(const char *page, int x, int y, int *track);
int ds10_ui_ms_box(const char *page, int track, int solo, int *x, int *y,
                   int *h);

/* Draw the KAOSS pad's cursor at the two axis values. The original leaves a
 * trail; this draws the one point. */
void ds10_ui_kaoss_mark(ds10_ui *u, int vx, int vy);

/* Hit-test 32_PATTERN's sixteen buttons. Returns 0..15, or -1. */
int ds10_ui_pattern_hit(int x, int y);
void ds10_ui_pattern_box(int n, int *x, int *y);

/* Draw the marker on the selected pattern. */
void ds10_ui_pattern_mark(ds10_ui *u, int n);

/* The sequencer's sub-mode button, which steps NOTE, GATE, VOL, PAN and the
 * two automation lanes. Returns non-zero for a hit; the page each position
 * shows is ds10_ui_seq_mode_page(). */
int ds10_ui_seq_mode_hit(int x, int y);
const char *ds10_ui_seq_mode_page(int mode);
int ds10_ui_seq_modes(void);

/* The note on the top row when the grid has not been scrolled, and the note a
 * row shows for a given top. */
int ds10_ui_seq_top_default(void);
int ds10_ui_seq_row_note(int top, int row);

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
