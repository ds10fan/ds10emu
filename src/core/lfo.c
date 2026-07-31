#include "lfo.h"

#include "fixed.h"

/* Same polynomial the oscillator noise uses. */
#define LFSR_POLY 0x1064
#define LFO_ONE 0x01000000
#define LFO_MAX 0x00FFFFFF

static inline int32_t lfsr_step(int32_t reg)
{
    if ((reg & 0x40000000) == 0)
        return ds10_shl(reg ^ LFSR_POLY, 1) | 1;
    return ds10_shl(reg, 1);
}

void ds10_lfo_step(ds10_lfo *l)
{
    int32_t old_up = l->phase_up;
    int32_t saw, folded, ramp;

    /* Two accumulators run off the same increment in opposite directions.
     * The down-counter drives saw and triangle; the up-counter drives the ramp
     * and clocks sample-and-hold when it changes sign. */
    l->phase_down -= l->inc;
    saw = l->phase_down >> 7;

    l->phase_up = old_up + l->inc;
    ramp = saw + (l->phase_up >> 7);

    /* The LFSR advances every frame regardless; only the held output waits for
     * the up-counter to wrap. */
    l->lfsr = lfsr_step(l->lfsr);
    if (((old_up ^ l->phase_up) >> 31) != 0)
        l->out[DS10_LFO_SAMPLE_HOLD] = l->lfsr >> 7;

    folded = saw;
    if (folded < 1)
        folded = -folded;

    l->out[DS10_LFO_SAW] = saw;
    /* Fold to a triangle: 2*|saw| + 1 - 0x01000000, i.e. 2*|saw| - 0x00FFFFFF.
     * The original really does add 1 before subtracting the whole unit. */
    l->out[DS10_LFO_TRIANGLE] = ds10_shl(folded, 1) + 1 - LFO_ONE;
    l->out[DS10_LFO_RAMP] = (ramp + LFO_ONE) >> 1;
}
