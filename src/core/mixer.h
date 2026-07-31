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
    int32_t pan[DS10_MIX_CHANNELS];     /* +0x38 + 4*ch */
    int32_t vol[DS10_MIX_CHANNELS];     /* +0x50 + 4*ch */
    int32_t prod[DS10_MIX_CHANNELS];    /* +0x68 + 4*ch, recomputed per frame */
    int32_t route;                      /* +0x84 */
    int16_t mute;                       /* +0x88 */
    ds10_fx fx;                         /* the object at +0x80 */
} ds10_mixer;

/* Render `n` samples. `in[ch]` are the six voice buffers. */
void ds10_mixer_render(ds10_mixer *m, const int16_t *const *in,
                       const ds10_fx_tables *t, int16_t *out_l, int16_t *out_r,
                       int n);

#endif /* DS10_MIXER_H */
