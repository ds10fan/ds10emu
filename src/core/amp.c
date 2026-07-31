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

/* --- per-frame level mapping (render preamble, 0x02074E0C..0x02074F00) ---
 *
 * The LFO contribution is unipolarised differently depending on which shape is
 * routed: the square shape (selector 3) is used directly, anything else is
 * offset and halved first. Selector 4 - the neutral slot - skips the whole
 * thing and yields unity.
 */
#define AMP_UNITY 0x01000000
#define AMP_FULL 0x00FFFFFF

void ds10_amp_update(ds10_amp *a, const int32_t *env_out,
                     const int32_t *lfo_out)
{
    int32_t shaped, scaled;
    int64_t mod, operand;

    if (a->lfo_sel == 4) {
        shaped = AMP_UNITY;
    } else {
        int32_t lfo = lfo_out[a->lfo_sel];
        int32_t t;

        if (a->lfo_sel == 3)
            t = ds10_mul24(lfo, a->in_depth);
        else
            t = ds10_mul24((lfo + AMP_FULL) >> 1, a->in_depth);

        /* A positive depth inverts the sense of the modulation; a negative or
         * zero depth leaves it alone. */
        shaped = (a->in_depth > 0) ? (t + AMP_FULL) - a->in_depth
                                   : t + AMP_FULL;
    }

    scaled = ds10_mul24(shaped, a->level_knob);

    /* The envelope term is kept at full 64-bit width before the final multiply,
     * which is why the original builds a signed 64x64 out of umull and mla
     * rather than using SMULL. */
    mod = ((int64_t)a->in_depth * (int64_t)env_out[a->env_sel]) >> 24;
    operand = (int64_t)env_out[a->eg_sel] + mod;

    {
        int32_t target = (int32_t)((operand * (int64_t)scaled) >> 24);

        a->level_delta = (target - a->target) >> 6;
        a->level = a->target;
        a->target = target;
    }
}
