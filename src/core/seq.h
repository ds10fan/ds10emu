/* DS-10 sequencer - transport, tempo, swing and step advance.
 *
 * Reimplemented from the sequencer object at 0x0206D79C (ctor), 0x0206D830
 * (config), 0x0206DB14 (swing) and the main tick at 0x0206E5AC.
 *
 * Timing model:
 *   period    = 60000000 / tempo_bpm        (samples-ish per sub-tick)
 *   accum    += incr (240000) per tick call; a sub-tick fires each time the
 *               accumulator passes `period`, which it then subtracts
 *   24 sub-ticks per step PAIR, split by swing:
 *       even = (swing*24 + 50) / 100,  odd = 24 - even
 *   so swing 50 gives 12/12 (straight) and the extremes give 0/24 and 24/0
 *   16 steps per pattern
 *
 * The tick drives three things, in order: the step advance (with callbacks to
 * the song player), a gate/note processor that emits note-on and note-off
 * events, and an automation-lane processor that writes knobs directly. Only the
 * step advance is implemented here; see recon.md section 13 for the rest.
 */
#ifndef DS10_SEQ_H
#define DS10_SEQ_H

#include <stdint.h>

#define DS10_SEQ_TEMPO_NUM 60000000
#define DS10_SEQ_SUBTICKS 24       /* per step pair */
#define DS10_SEQ_INCR 240000

typedef struct {
    uint8_t playing;     /* +0x0C 1 = running */
    uint8_t starting;    /* +0x0D set on start; first tick initialises counters */
    uint8_t tempo;       /* +0x10 BPM */
    int32_t period;      /* +0x14 derived from tempo */
    int32_t accum;       /* +0x18 */
    int32_t incr;        /* +0x1C */
    int32_t tick;        /* +0x20 sub-tick counter */
    uint8_t swing;       /* +0x24 percent; 50 is straight */
    int32_t step_count;  /* +0x28 monotonic step counter */
    uint8_t steps;       /* +0x2C steps per pattern, 16 */
    int8_t step;         /* +0x2D current step */
    uint8_t subtick;     /* +0x2E countdown to the next step */
    uint8_t sub_even;    /* +0x30 sub-ticks for even steps */
    uint8_t sub_odd;     /* +0x31 for odd steps */
    uint8_t sub_cur;     /* +0x32 the current step's count */
    uint8_t auto_sub;    /* +0x33 automation sub-step */
    int32_t pattern;     /* +0x40 current pattern, 0 = none */
    int32_t bar;         /* +0x48 bar counter */
    int32_t ntracks;     /* +0x54 */
} ds10_seq;

/* Per-track note/gate state, the first six bytes of the 36-byte track block.
 * Gate values are special-cased: 0xFF is a tie (sounds until the next note,
 * never auto-released), 6 retriggers, 5 and 4 are one sub-tick over and under a
 * full step, and anything else is gate*subticks/4. */
#define DS10_GATE_TIE 0xFF
#define DS10_GATE_RETRIG 6
#define DS10_GATE_OVER 5
#define DS10_GATE_FULL 4

/* What the gate countdown wants emitted this sub-tick. */
enum ds10_note_ev {
    DS10_NOTE_NONE = 0,
    DS10_NOTE_OFF = 1,
    DS10_NOTE_ON = 2,
    DS10_NOTE_RETRIG = 3,
};

typedef struct {
    uint8_t sounding;   /* +0 */
    int8_t pending;     /* +1 enum ds10_note_ev, consumed each sub-tick */
    int8_t gate_left;   /* +2 countdown in sub-ticks */
    int8_t gate_mode;   /* +4 the raw gate value */
    int8_t note;        /* +5 */
} ds10_seq_track;

/* Apply one step's (note, gate) to a track. `subticks` is the current step's
 * sub-tick count, which is where swing feeds into gate length. */
void ds10_seq_step_track(ds10_seq_track *t, int note, int gate, int subticks);

/* Advance the gate countdown one sub-tick; returns what to emit. */
int ds10_seq_gate(ds10_seq_track *t);

/* An event as the router builds it: opcode, channel, and a payload whose low
 * byte is the note. Target bits: 0x20 machine array, 0x10 broadcast, 0x40
 * mixer, 0x80 second bus, low nibble channel. */
/* One queue entry as it lands on a destination. A single logical event fans out
 * to several of these: a note-on targets 0xA0, which is the track's machine
 * plus the second bus, so it posts twice. */
typedef struct {
    uint8_t dest;     /* machine index, or DS10_BUS1 / DS10_BUS2 */
    uint8_t opcode;
    uint8_t channel;  /* byte 1, written by the router */
    uint16_t payload; /* note | 0x7F00 */
} ds10_seq_posted;

/* Destination ids above the machine array. */
#define DS10_BUS1(nmach) (nmach)
#define DS10_BUS2(nmach) ((nmach) + 1)

/* Run the gate countdown for every track, emit note events and route them.
 * Returns the number of queue entries written. Mirrors 0x0206E8C8 feeding
 * 0x0207025C. */
int ds10_seq_emit(ds10_seq *s, ds10_seq_track *tracks, ds10_seq_posted *out);

void ds10_seq_set_tempo(ds10_seq *s, int tempo);
void ds10_seq_set_swing(ds10_seq *s, int swing);
void ds10_seq_tick(ds10_seq *s);

#endif /* DS10_SEQ_H */

/* --- automation lanes (0x0206E1EC) --------------------------------------- */
/* Two per synth track, 8 bytes each. `value` of -1 means take the per-track
 * default from the pattern header instead of the step byte. */
typedef struct {
    uint8_t knob;
    int8_t value;
    int8_t offset;
    uint8_t scale;
    int16_t cur;     /* result, held as value<<8 */
    int16_t delta;
} ds10_lane;

/* Map a raw lane byte to the knob value the machine receives. A raw value of -1
 * means "no automation on this step": the per-track default from the pattern
 * header is used directly, bypassing offset and scale entirely. */
int32_t ds10_lane_map(const ds10_lane *l, int raw, const int8_t *defaults);
/* Hold mode: latch the step's value for the whole step. */
void ds10_lane_hold(ds10_lane *l, int raw, const int8_t *defaults);
