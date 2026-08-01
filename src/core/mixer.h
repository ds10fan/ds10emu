/* DS-10 mixer - six voices to stereo, with the effect send and master stage.
 *
 * Reimplemented from 0x02072C88 (ctor) and 0x02072E8C (render). Channel order
 * is `0 = syn1, 1 = syn2, 2..5 = drums`.
 *
 * Three things worth knowing before reading the code:
 *
 *  - Panning is LINEAR, not equal-power: right = in*P>>15 and
 *    left = in*(vol-P)>>15 where P = pan*vol>>15.
 *  - The master gain is a click-free mute ramp, a one-pole smoother at
 *    0.99429 per sample. Muting is a ~200-sample fade, not a gate.
 *  - The final stage is a soft-knee waveshaper, not a plain clamp. It is
 *    roughly 2x gain that saturates toward full scale.
 */
#ifndef DS10_MIXER_H
#define DS10_MIXER_H

#include <stdint.h>

#include "fx.h"

#define DS10_MIX_CHANNELS 6

/* Which channels feed the effect; everything else is summed dry. */
enum ds10_fx_route {
    DS10_ROUTE_SYN1 = 0,
    DS10_ROUTE_SYN2 = 1,
    DS10_ROUTE_SYN12 = 2,
    DS10_ROUTE_DRUMS = 3,
    DS10_ROUTE_ALL = 4,
    DS10_ROUTE_OFF = 5,
};

typedef struct {
    int32_t gain;                       /* +0x30 ramped master gain */
    int32_t drywet;                     /* +0x34 stored so route 5 can restore it */
    int32_t pan[DS10_MIX_CHANNELS];     /* +0x38 + 4*ch */
    int32_t vol[DS10_MIX_CHANNELS];     /* +0x50 + 4*ch */
    int32_t prod[DS10_MIX_CHANNELS];    /* +0x68 + 4*ch, recomputed per frame */
    int32_t route;                      /* +0x84 */
    int16_t mute;                       /* +0x88 */
    ds10_fx fx;                         /* the object at +0x80 */
} ds10_mixer;

/* Render `n` samples. `in[ch]` are the six voice buffers. */
void ds10_mixer_render(ds10_mixer *m, const int16_t *const *in,
                       int16_t *out_l, int16_t *out_r, int n);

/* The mixer's 25 knobs, from the dispatch at 0x020736C0. A pattern carries all
 * 25 as bytes and the sequencer pushes them in on a pattern change, so this is
 * the whole of a song's mix:
 *
 *   0        route; also forces the effect fully dry when it is 5 (OFF)
 *   1..8     the effect, forwarded to ds10_fx_set with the SAME parameter id
 *   9..14    pan, channels 0..5:    value * 258            (linear)
 *   15, 16   forwarded to the effect as well
 *   17..22   volume, channels 0..5: value^2 * 0x10404 >> 15 (quadratic)
 *   23, 24   nothing at all
 *
 * Knob 8 is dry/wet: the value is stored either way but only reaches the effect
 * when the route is not OFF.
 *
 * Values arrive SIGNED - the pushers read them with ldrsb - so the effect's
 * bipolar knobs see negatives. */
#define DS10_MIX_KNOBS 25
#define DS10_MIX_PAN_STEP 0x102     /* 258 */
#define DS10_MIX_VOL_SCALE 0x10404  /* 258*258 */

void ds10_mixer_set_knob(ds10_mixer *m, int knob, int value);

/* Events the mixer acts on, from its dispatch at 0x020735D0. Opcodes outside
 * this set are ignored, so the sequencer's bar and pattern-change broadcasts do
 * not reach it. */
#define DS10_MIX_EV_TEMPO 4      /* payload word is the BPM; retunes a synced delay */
#define DS10_MIX_EV_KNOB 6
#define DS10_MIX_EV_MUTE_OFF 20
#define DS10_MIX_EV_MUTE_ON 21

/* The two knob ids a knob event can carry, and where the mixer sends them. The
 * sequencer posts these once per step per sounding track, which is how per-step
 * velocity and pan reach the mix. */
#define DS10_MIX_EVK_VOL 7       /* -> knob channel + 17 */
#define DS10_MIX_EVK_PAN 10      /* -> knob channel + 9 */

void ds10_mixer_event(ds10_mixer *m, int opcode, int channel, uint16_t payload,
                      int32_t arg);

#endif /* DS10_MIXER_H */
