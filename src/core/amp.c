#include "amp.h"

#include "fixed.h"

#define AMP_LO (-32767)
#define AMP_HI (32767)

/* The pre-doubling limit. The original derives both from the same -32767 held
 * in a register (`ip lsr 18` and `ip + 0x4000`), which is why the bounds are
 * 16383 rather than a round 16384. */
#define AMP_HALF_LO (-16383)
#define AMP_HALF_HI (16383)

int16_t ds10_amp_step(ds10_amp *a, int32_t in, int32_t vco2)
{
    int32_t prod, clipped, mixed, out, doubled;

    /* Drive path one: scale hard and clip. This is where overdrive comes from -
     * drive1 grows quadratically with the knob, so the clip engages sooner. */
    prod = ds10_mul32(in, a->drive1);
    clipped = prod >> 11;
    if (clipped < AMP_LO)
        clipped = AMP_LO;
    else if (clipped > AMP_HI)
        clipped = AMP_HI;
    a->clipped = clipped;

    /* Drive path two: a clean copy scaled by drive2, which is the dry side of
     * the drive control. Half of each path is summed. */
    mixed = (clipped >> 1) + (ds10_mul32(in, a->drive2) >> 15);
    mixed >>= 1;

    out = ds10_mul32(mixed, a->level >> 9) >> 15;

    /* Extra distortion: ring-modulate against VCO2 and crossfade with the
     * undistorted signal by `wet`. */
    if (a->dist != 0) {
        int32_t wet = ds10_mul32(ds10_mul32(vco2, out) >> 15, a->wet) >> 15;
        int32_t dry = ds10_mul32(out, AMP_HI - a->wet) >> 15;

        out = wet + dry;
    }

    if (out < AMP_HALF_LO)
        out = AMP_HALF_LO;
    else if (out > AMP_HALF_HI)
        out = AMP_HALF_HI;

    doubled = ds10_shl(out, 1);
    if (doubled < AMP_LO)
        doubled = AMP_LO;
    else if (doubled > AMP_HI)
        doubled = AMP_HI;

    a->level += a->level_delta;
    return (int16_t)doubled;
}
