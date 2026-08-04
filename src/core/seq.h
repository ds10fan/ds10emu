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
#define DS10_SEQ_STEPS 16

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
 *
 * The gate codes a song file carries are the six the interface offers, and the
 * sub-tick counts below are for a straight step of 12:
 *
 *   0     0%     count 0, so it is released on the sub-tick it started
 *   1..3  25..75% count gate*subticks/4, i.e. 3, 6 and 9
 *   4     100%   count subticks-1 = 11, a sub-tick short so the next note of a
 *                different pitch can articulate
 *   5     TIE    count subticks+1 = 13, so it is still inside its gate when the
 *                next step arrives: an identical next step then emits nothing
 *                and the note is held, which is how a tie is written
 *   6            the drum mode - retriggers, and never auto-releases, so the
 *                sample plays out even if the following steps are empty. Only
 *                the drum tracks use it; no synth step in the ROM or in the 108
 *                saved songs does
 *
 * 0xFF is a seventh mode the interface has no button for and no song file uses:
 * it sounds until an empty step releases it. */
#define DS10_GATE_HOLD 0xFF
#define DS10_GATE_RETRIG 6
#define DS10_GATE_TIE 5
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
    int8_t vel;         /* +6 the step's velocity, copied on a note */
    int8_t pan;         /* +7 and its pan */
    uint8_t live_gate;  /* +0x0C gate from live input, for record mode */
    int8_t live_note;   /* +0x0D note from live input, negative = none */
    uint8_t play_mode;  /* +0x22 enum ds10_play_mode */
} ds10_seq_track;

/* What a track does with the pattern, from the jump table at 0x0206E7A4.
 * Anything above RECORD does nothing at all. */
enum ds10_play_mode {
    DS10_PLAY_PATTERN = 0,
    DS10_PLAY_MUTE = 1,
    DS10_PLAY_UNLESS_LIVE = 2,   /* play the step only if no live note is held */
    DS10_PLAY_RECORD = 3,        /* write the live note back into the pattern */
};

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

/* Sets tempo and period only. The real setter is ds10_seq_tempo below, which
 * also tells the machines - this one is for tests that drive state directly. */
void ds10_seq_set_tempo(ds10_seq *s, int tempo);
void ds10_seq_set_swing(ds10_seq *s, int swing);
void ds10_seq_tick(ds10_seq *s);

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
/* Hold mode - the pad's HLD, enable 1: latch the step's value for the whole
 * step. */
void ds10_lane_hold(ds10_lane *l, int raw, const int8_t *defaults);

/* Smooth mode - the pad's SMT, enable 2: latch this step's value and set the
 * per-sub-tick delta that walks it to `next` over `subticks` of them. `next` is
 * the following step's raw byte, or this step's own on the last step of a
 * pattern, which is what makes the ramp flatten there. */
void ds10_lane_smooth(ds10_lane *l, int raw, int next, int subticks,
                      const int8_t *defaults);

/* --- the pattern-driven tick (the rest of 0x0206E5AC) --------------------- */

#define DS10_SEQ_MAXTRACKS 6

/* A pattern as the sequencer reads it - not the layout a .dat file uses, which
 * has a 64-byte track stride and puts the two automation bytes elsewhere. See
 * recon.md section 13 for the header fields; only the ones the tick reads are
 * modelled here. */
typedef struct {
    uint8_t steps;                                  /* +0x06 */
    /* +0x24 + track*0x68 + step*4: note, gate, velocity, pan */
    int8_t step[DS10_SEQ_MAXTRACKS][DS10_SEQ_STEPS][4];
} ds10_rt_pattern;

/* Message ids the tick writes into the song player, u32 constants that read as
 * FourCCs. The answer to BAR is the next pattern, or NULL for none. */
#define DS10_MSG_STEP 0x73746570u   /* "step", arg = the step being left */
#define DS10_MSG_ENDPAT 0x65647470u /* "edtp", arg = bar */
#define DS10_MSG_BAR 0x73747074u    /* "stpt", arg 0 in, next pattern out */
#define DS10_MSG_RUN 0x726E7470u    /* "rntp", arg = bar */

typedef struct ds10_song_player ds10_song_player;
struct ds10_song_player {
    uint32_t msg;                   /* +0x04 */
    int32_t arg;                    /* +0x08 in the original this word is both */
    ds10_rt_pattern *answer;        /* the argument and the answer; split here */
    void (*handle)(ds10_song_player *p, ds10_seq *s);  /* vtable slot 1 */
};

/* The mixer knobs the per-step velocity and pan land on. */
#define DS10_KNOB_CHAN_VOL 7
#define DS10_KNOB_CHAN_PAN 10

/* Two automation lanes per synth track, initialised from the pattern by
 * 0x0206DEE4: `knob` is the lane's target (0xFF = lane off), `offset` is that
 * knob's minimum and `scale` its range, so a recorded 0..127 value sweeps exactly
 * one knob's range. */
#define DS10_SEQ_LANES 2
#define DS10_LANE_OFF 0xFF

typedef struct {
    ds10_lane lane[DS10_SEQ_LANES];
    uint8_t enable;      /* track state +0x20: 1 and 2 are two processor paths */
    uint8_t mode;        /* track state +0x21, 0..3 */
} ds10_seq_auto;

typedef struct ds10_seq_ctx ds10_seq_ctx;

struct ds10_seq_ctx {
    ds10_seq *s;
    ds10_seq_track *trk;
    ds10_rt_pattern *pat;           /* seq +0x40, NULL = none */
    ds10_rt_pattern *prev;          /* seq +0x44 */
    ds10_song_player *player;       /* seq +0x3C */
    const int8_t *chan;             /* the byte array at seq +0x60 */
    const uint8_t *chan_idx;        /* two per track: volume then pan */
    ds10_seq_posted *out;           /* every routed event, in order */
    int nout;
    int max;

    /* Automation. `nauto` is the synth track count the processor loops over
     * (seq +0x4C). `knob` is the machine's knob handler, reached through vtable
     * slot +0x18 in the original; NULL disables the dispatch. */
    ds10_seq_auto *autom;
    const int8_t *lane_raw;         /* two bytes per (track, step) */
    const int8_t *const *defaults;  /* per-track knob array, for a raw of -1 */
    int nauto;
    void (*knob)(void *machine, int knob, int value);
    void *machine[DS10_SEQ_MAXTRACKS];

    /* Called where the original calls set_pattern (0x0206DBE4), i.e. inside the
     * tick the moment the song player answers with a pattern and before any step
     * is processed. That is where the machines are re-initialised from the new
     * pattern - patches, mixer knobs, automation lanes - and doing it after the
     * tick returns is too late: the first step's own volume events would already
     * have gone out against the previous pattern's mixer settings. */
    void (*install)(ds10_seq_ctx *c, ds10_rt_pattern *pat);
    void *user;
};

/* One tick call, the whole of 0x0206E5AC: accumulator, step advance, the song
 * player messages, the pattern read, per-track play modes, the velocity and pan
 * posts, note emission, then the automation processor - which runs whenever there
 * is a pattern, on whichever of its paths each track's `enable` selects. Returns
 * the events appended to c->out. */
int ds10_seq_run(ds10_seq_ctx *c);

/* The sequencer's own tempo setter, 0x0206DA90: clamp to 10..250 and, if it
 * changed, broadcast opcode 4 to target 0x70 - every machine AND the mixer.
 * That broadcast is the only thing that tells a voice the tempo, and a
 * BPM-synced LFO's rate is scaled by it, so without it every synced LFO runs at
 * whatever tempo the constructor left. Returns events appended to c->out. */
int ds10_seq_tempo(ds10_seq_ctx *c, int tempo);

#define DS10_SEQ_TEMPO_MIN 10
#define DS10_SEQ_TEMPO_MAX 250

#endif /* DS10_SEQ_H */
