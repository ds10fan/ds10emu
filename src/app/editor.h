/* The song editor's model.
 *
 * Everything here mutates a `ds10_song` - the same record the ROM's own files
 * are a byte image of - so what the editor produces is a real song, and
 * `ds10_editor_export` writes a file the hardware would load. Nothing in here
 * touches the audio engine: the caller rebuilds the runtime pattern after an
 * edit. That keeps the model testable on its own.
 *
 * Two things about the step record are easy to get backwards, and both are
 * settled by the ROM's blank template `ds10init.dat`:
 *
 *   the note byte is 0xFF for a rest, and that is the only thing that decides
 *   whether a step sounds;
 *   the gate byte is the step's length, 1 to 6, and it survives a rest. Every
 *   step of the blank song is gate 3, which is why entering a note must set
 *   the note and leave the gate alone.
 */
#ifndef DS10_APP_EDITOR_H
#define DS10_APP_EDITOR_H

#include <stddef.h>
#include <stdint.h>

#include "../core/song.h"

#define DS10_EDIT_REST 0xFF
#define DS10_EDIT_GATE_DEFAULT 3

typedef struct {
    ds10_song song;
    int pattern;   /* the pattern being edited, 0..15 */
    int track;     /* the track being edited, 0..5 */
    int changed;   /* bumped on every edit, so a caller can tell */
} ds10_editor;

/* Start from the ROM's blank template. `blob` is ds10init.dat, hash and all.
 * Returns a ds10_song_status. */
int ds10_editor_init(ds10_editor *e, const uint8_t *blob, size_t len);

/* Adopt an already-parsed song, e.g. a demo the user loaded to edit. */
void ds10_editor_adopt(ds10_editor *e, const ds10_song *s);

/* The pattern being edited. */
ds10_song_pattern *ds10_editor_pattern(ds10_editor *e);

/* Steps. `note` below zero, or 0xFF, is a rest; the gate is left alone. */
void ds10_editor_note(ds10_editor *e, int track, int step, int note);
int ds10_editor_get_note(const ds10_editor *e, int track, int step);

/* The other three bytes of a step. Gate is 1..6, velocity and pan 0..127. */
void ds10_editor_gate(ds10_editor *e, int track, int step, int gate);
int ds10_editor_get_gate(const ds10_editor *e, int track, int step);
void ds10_editor_vel(ds10_editor *e, int track, int step, int vel);
int ds10_editor_get_vel(const ds10_editor *e, int track, int step);
void ds10_editor_pan(ds10_editor *e, int track, int step, int pan);
int ds10_editor_get_pan(const ds10_editor *e, int track, int step);

/* An automation lane's value at a step, or -1 where the lane has nothing and
 * the knob keeps the patch's own setting. A pattern carries two lanes per
 * synth track, which is what the sequencer's two KAOSS sub-modes edit. */
int ds10_editor_get_lane(const ds10_editor *e, int track, int lane, int step);
void ds10_editor_lane(ds10_editor *e, int track, int lane, int step, int v);

/* Which knob a lane drives. */
int ds10_editor_get_lane_knob(const ds10_editor *e, int track, int lane);
void ds10_editor_lane_knob(ds10_editor *e, int track, int lane, int knob);

/* How many of the sixteen steps this pattern plays. */
void ds10_editor_steps(ds10_editor *e, int steps);
int ds10_editor_get_steps(const ds10_editor *e);

/* A synth track's patch, and the mixer, as the pattern stores them. */
void ds10_editor_knob(ds10_editor *e, int track, int knob, int value);
int ds10_editor_get_knob(const ds10_editor *e, int track, int knob);
void ds10_editor_mixer(ds10_editor *e, int knob, int value);
int ds10_editor_get_mixer(const ds10_editor *e, int knob);

/* Song-wide settings. `name` is padded to the six characters the map shows. */
void ds10_editor_tempo(ds10_editor *e, int bpm);
void ds10_editor_swing(ds10_editor *e, int percent);
void ds10_editor_name(ds10_editor *e, const char *name);

/* The arrangement. Bars run 0..DS10_SONG_CHAIN-1 and end at the first
 * 0xFFFF; setting a bar past the end extends it, and clearing one closes the
 * gap so the chain stays contiguous. */
int ds10_editor_bars(const ds10_editor *e);
int ds10_editor_chain_get(const ds10_editor *e, int bar);
void ds10_editor_chain_set(ds10_editor *e, int bar, int pattern);
void ds10_editor_chain_insert(ds10_editor *e, int bar, int pattern);
void ds10_editor_chain_remove(ds10_editor *e, int bar);

/* Whole-pattern operations. Clearing rests every step and leaves the patch. */
void ds10_editor_pattern_clear(ds10_editor *e, int pattern);
void ds10_editor_pattern_copy(ds10_editor *e, int from, int to);
void ds10_editor_track_clear(ds10_editor *e, int pattern, int track);
void ds10_editor_transpose(ds10_editor *e, int pattern, int track, int by);

/* Write a file image. Needs song.payload_len + 16 bytes. */
size_t ds10_editor_export(const ds10_editor *e, uint8_t *out);

#endif /* DS10_APP_EDITOR_H */
