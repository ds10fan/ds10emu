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

void ds10_seq_set_tempo(ds10_seq *s, int tempo);
void ds10_seq_set_swing(ds10_seq *s, int swing);
void ds10_seq_tick(ds10_seq *s);

#endif /* DS10_SEQ_H */
