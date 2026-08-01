/* See editor.h. */
#include <string.h>

#include "editor.h"

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int step_ok(int track, int step)
{
    return track >= 0 && track < DS10_SONG_TRACKS && step >= 0 &&
           step < DS10_SONG_STEPS;
}

int ds10_editor_init(ds10_editor *e, const uint8_t *blob, size_t len)
{
    int st;

    memset(e, 0, sizeof *e);
    st = ds10_song_parse(&e->song, blob, len);
    return st;
}

void ds10_editor_adopt(ds10_editor *e, const ds10_song *s)
{
    e->song = *s;
    e->pattern = 0;
    e->track = 0;
    e->changed++;
}

ds10_song_pattern *ds10_editor_pattern(ds10_editor *e)
{
    return &e->song.pattern[clamp(e->pattern, 0, DS10_SONG_PATTERNS - 1)];
}

static ds10_song_step *step_of(ds10_editor *e, int track, int step)
{
    if (!step_ok(track, step)) return NULL;
    return &ds10_editor_pattern(e)->step[track][step];
}

void ds10_editor_note(ds10_editor *e, int track, int step, int note)
{
    ds10_song_step *s = step_of(e, track, step);

    if (!s) return;
    /* A rest is the note byte alone; the gate is the step's length and stays
     * whatever the user set it to. */
    s->note = (int8_t)(note < 0 || note > 127 ? DS10_EDIT_REST : note);
    e->changed++;
}

int ds10_editor_get_note(const ds10_editor *e, int track, int step)
{
    int n;

    if (!step_ok(track, step)) return -1;
    n = (uint8_t)e->song.pattern[clamp(e->pattern, 0, DS10_SONG_PATTERNS - 1)]
            .step[track][step].note;
    return n == DS10_EDIT_REST ? -1 : n;
}

#define STEP_FIELD(lname, field, lo, hi)                                      \
    void ds10_editor_##lname(ds10_editor *e, int track, int step, int v)      \
    {                                                                         \
        ds10_song_step *s = step_of(e, track, step);                          \
                                                                              \
        if (!s) return;                                                       \
        s->field = (int8_t)clamp(v, lo, hi);                                  \
        e->changed++;                                                         \
    }                                                                         \
    int ds10_editor_get_##lname(const ds10_editor *e, int track, int step)    \
    {                                                                         \
        if (!step_ok(track, step)) return 0;                                  \
        return e->song                                                        \
            .pattern[clamp(e->pattern, 0, DS10_SONG_PATTERNS - 1)]            \
            .step[track][step]                                                \
            .field;                                                           \
    }

STEP_FIELD(gate, gate, 1, 6)
STEP_FIELD(vel, vel, 0, 127)
STEP_FIELD(pan, pan, 0, 127)

/* The automation lanes. The pattern's array is indexed by track and step,
 * with the two lanes inside the record - which is how 0x0206DFCC's caller
 * reads it, `q->lane[track][step].raw[lane]`. */
static int lane_ok(int track, int lane, int step)
{
    return track >= 0 && track < DS10_SONG_LANES && lane >= 0 &&
           lane < DS10_SONG_LANES && step >= 0 && step < DS10_SONG_STEPS;
}

int ds10_editor_get_lane(const ds10_editor *e, int track, int lane, int step)
{
    if (!lane_ok(track, lane, step)) return -1;
    return e->song.pattern[clamp(e->pattern, 0, DS10_SONG_PATTERNS - 1)]
        .lane[track][step].raw[lane];
}

void ds10_editor_lane(ds10_editor *e, int track, int lane, int step, int v)
{
    if (!lane_ok(track, lane, step)) return;
    ds10_editor_pattern(e)->lane[track][step].raw[lane] =
        (int8_t)(v < 0 ? -1 : v > 127 ? 127 : v);
    e->changed++;
}

int ds10_editor_get_drum_knob(const ds10_editor *e, int drum, int knob)
{
    if (drum < 0 || drum >= DS10_SONG_DRUMS || knob < 0 ||
        knob >= DS10_SONG_KNOBS)
        return 0;
    return ds10_song_drum_knobs(&e->song, drum)[knob];
}

void ds10_editor_drum_knob(ds10_editor *e, int drum, int knob, int value)
{
    if (drum < 0 || drum >= DS10_SONG_DRUMS || knob < 0 ||
        knob >= DS10_SONG_KNOBS)
        return;
    /* The accessor is const because nothing else writes here; this is the
     * one place that does, and it writes the same bytes it reads. */
    ((int8_t *)ds10_song_drum_knobs(&e->song, drum))[knob] = (int8_t)value;
    e->changed++;
}

int ds10_editor_get_lane_knob(const ds10_editor *e, int track, int lane)
{
    if (!lane_ok(track, lane, 0)) return -1;
    return (uint8_t)e->song
        .pattern[clamp(e->pattern, 0, DS10_SONG_PATTERNS - 1)]
        .lane[track][0]
        .knob[lane];
}

void ds10_editor_lane_knob(ds10_editor *e, int track, int lane, int knob)
{
    int s;

    if (!lane_ok(track, lane, 0) || knob < 0 || knob >= DS10_SONG_KNOBS)
        return;
    /* The sequencer reads the knob from step zero, but the field exists on
     * every step and the ROM's songs keep them equal. */
    for (s = 0; s < DS10_SONG_STEPS; s++)
        ds10_editor_pattern(e)->lane[track][s].knob[lane] = (int8_t)knob;
    e->changed++;
}

void ds10_editor_steps(ds10_editor *e, int steps)
{
    ds10_editor_pattern(e)->steps = (uint32_t)clamp(steps, 1, DS10_SONG_STEPS);
    e->changed++;
}

int ds10_editor_get_steps(const ds10_editor *e)
{
    return (int)e->song.pattern[clamp(e->pattern, 0, DS10_SONG_PATTERNS - 1)]
        .steps;
}

void ds10_editor_knob(ds10_editor *e, int track, int knob, int value)
{
    if (track < 0 || track >= 2 || knob < 0 || knob >= DS10_SONG_KNOBS) return;
    ds10_editor_pattern(e)->knobs[track][knob] = (int8_t)value;
    e->changed++;
}

int ds10_editor_get_knob(const ds10_editor *e, int track, int knob)
{
    if (track < 0 || track >= 2 || knob < 0 || knob >= DS10_SONG_KNOBS)
        return 0;
    return e->song.pattern[clamp(e->pattern, 0, DS10_SONG_PATTERNS - 1)]
        .knobs[track][knob];
}

void ds10_editor_mixer(ds10_editor *e, int knob, int value)
{
    if (knob < 0 || knob >= DS10_SONG_MIX_KNOBS) return;
    ds10_editor_pattern(e)->mixer[knob] = (int8_t)value;
    e->changed++;
}

int ds10_editor_get_mixer(const ds10_editor *e, int knob)
{
    if (knob < 0 || knob >= DS10_SONG_MIX_KNOBS) return 0;
    return e->song.pattern[clamp(e->pattern, 0, DS10_SONG_PATTERNS - 1)]
        .mixer[knob];
}

void ds10_editor_tempo(ds10_editor *e, int bpm)
{
    e->song.tempo = (uint32_t)clamp(bpm, 20, 300);
    e->changed++;
}

void ds10_editor_swing(ds10_editor *e, int percent)
{
    e->song.swing = (uint16_t)clamp(percent, 50, 75);
    e->changed++;
}

void ds10_editor_name(ds10_editor *e, const char *name)
{
    size_t i;

    memset(e->song.name, 0, sizeof e->song.name);
    for (i = 0; i < 6 && name[i]; i++) e->song.name[i] = name[i];
    /* The six the map shows are space padded, as the ROM's own names are. */
    for (; i < 6; i++) e->song.name[i] = ' ';
    e->changed++;
}

int ds10_editor_bars(const ds10_editor *e)
{
    return ds10_song_bars(&e->song);
}

int ds10_editor_chain_get(const ds10_editor *e, int bar)
{
    if (bar < 0 || bar >= DS10_SONG_CHAIN) return -1;
    if (e->song.chain[bar] == DS10_SONG_NO_PATTERN) return -1;
    return (int)(e->song.chain[bar] % DS10_SONG_PATTERNS);
}

void ds10_editor_chain_set(ds10_editor *e, int bar, int pattern)
{
    int n = ds10_editor_bars(e);

    if (bar < 0 || bar >= DS10_SONG_CHAIN) return;
    if (pattern < 0) {
        ds10_editor_chain_remove(e, bar);
        return;
    }
    /* Setting the bar just past the end appends; further out would leave a
     * hole, and a hole ends the arrangement. */
    if (bar > n) bar = n;
    e->song.chain[bar] = (uint16_t)(pattern % DS10_SONG_PATTERNS);
    if (bar == n && bar + 1 < DS10_SONG_CHAIN)
        e->song.chain[bar + 1] = DS10_SONG_NO_PATTERN;
    e->changed++;
}

void ds10_editor_chain_insert(ds10_editor *e, int bar, int pattern)
{
    int n = ds10_editor_bars(e), i;

    if (bar < 0 || bar > n || n + 1 >= DS10_SONG_CHAIN || pattern < 0) return;
    for (i = n; i > bar; i--) e->song.chain[i] = e->song.chain[i - 1];
    e->song.chain[bar] = (uint16_t)(pattern % DS10_SONG_PATTERNS);
    e->song.chain[n + 1] = DS10_SONG_NO_PATTERN;
    e->changed++;
}

void ds10_editor_chain_remove(ds10_editor *e, int bar)
{
    int n = ds10_editor_bars(e), i;

    if (bar < 0 || bar >= n) return;
    for (i = bar; i < n - 1; i++) e->song.chain[i] = e->song.chain[i + 1];
    e->song.chain[n - 1] = DS10_SONG_NO_PATTERN;
    e->changed++;
}

void ds10_editor_pattern_clear(ds10_editor *e, int pattern)
{
    ds10_song_pattern *p;
    int t, s;

    if (pattern < 0 || pattern >= DS10_SONG_PATTERNS) return;
    p = &e->song.pattern[pattern];
    /* What the blank template holds: every step a rest at the default gate,
     * full velocity, centre pan. The patch and the mixer are left alone. */
    for (t = 0; t < DS10_SONG_TRACKS; t++)
        for (s = 0; s < DS10_SONG_STEPS; s++) {
            p->step[t][s].gate = DS10_EDIT_GATE_DEFAULT;
            p->step[t][s].note = (int8_t)DS10_EDIT_REST;
            p->step[t][s].vel = 127;
            p->step[t][s].pan = 64;
        }
    e->changed++;
}

void ds10_editor_pattern_copy(ds10_editor *e, int from, int to)
{
    if (from < 0 || from >= DS10_SONG_PATTERNS) return;
    if (to < 0 || to >= DS10_SONG_PATTERNS || from == to) return;
    e->song.pattern[to] = e->song.pattern[from];
    e->changed++;
}

void ds10_editor_track_clear(ds10_editor *e, int pattern, int track)
{
    ds10_song_pattern *p;
    int s;

    if (pattern < 0 || pattern >= DS10_SONG_PATTERNS) return;
    if (track < 0 || track >= DS10_SONG_TRACKS) return;
    p = &e->song.pattern[pattern];
    for (s = 0; s < DS10_SONG_STEPS; s++)
        p->step[track][s].note = (int8_t)DS10_EDIT_REST;
    e->changed++;
}

void ds10_editor_transpose(ds10_editor *e, int pattern, int track, int by)
{
    ds10_song_pattern *p;
    int s;

    if (pattern < 0 || pattern >= DS10_SONG_PATTERNS) return;
    if (track < 0 || track >= DS10_SONG_TRACKS) return;
    p = &e->song.pattern[pattern];
    for (s = 0; s < DS10_SONG_STEPS; s++) {
        int n = (uint8_t)p->step[track][s].note;

        if (n == DS10_EDIT_REST) continue;
        p->step[track][s].note = (int8_t)clamp(n + by, 0, 127);
    }
    e->changed++;
}

size_t ds10_editor_export(const ds10_editor *e, uint8_t *out)
{
    return ds10_song_write(&e->song, out);
}
