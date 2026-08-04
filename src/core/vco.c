#include "vco.h"

#include "fixed.h"

/* Audio range used by the oscillator section: +-32767, i.e. Q15.
 * The original holds -32767 in a register (ip) for the whole loop and derives
 * the upper bound as `ip lsr 17`, which is 0x7FFF because the shift is
 * logical on 0xFFFF8001. Written out here rather than reproducing the trick. */
#define OSC_LO (-32767)
#define OSC_HI (32767)

/* Galois LFSR taps. The same constant 0x1064 is also used as the triangle
 * tilt base (0x1064 + 0x3F), so it does double duty in the original. */
#define LFSR_POLY 0x1064
#define TRI_TILT (0x1064 + 0x3F) /* 4259 */

/* Advance a Galois LFSR one step, exactly as the original does it. */
static inline int32_t lfsr_step(int32_t reg)
{
    if ((reg & 0x40000000) == 0)
        return ds10_shl(reg ^ LFSR_POLY, 1) | 1;
    return ds10_shl(reg, 1);
}

void ds10_vco_clock_noise(ds10_vco *o, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (o->wave1 == DS10_WAVE_NOISE)
            o->lfsr1 = lfsr_step(o->lfsr1);
        if (o->wave2 == DS10_WAVE_NOISE)
            o->lfsr2 = lfsr_step(o->lfsr2);
    }
}

/* Slightly tilted triangle: -32767 + 2*|p| + (p * 4259 >> 15). */
static inline int32_t wave_triangle(int32_t phase)
{
    int32_t p = phase >> 16;
    int32_t a = p;

    if (a <= 0)
        a = -a;
    return OSC_LO + ds10_shl(a, 1) + (ds10_mul32(p, TRI_TILT) >> 15);
}

/* Sawtooth: p minus a shaped copy of itself. The shaped term saturates
 * everywhere except within one sample step either side of the zero crossing, so
 * the output is a descending ramp whose jump is replaced by a rising edge two
 * samples wide at any pitch - the only band-limiting the oscillator has.
 * Clamp order here is upper bound first, which differs from the square case
 * below - both are preserved. */
static inline int32_t wave_saw(int32_t phase, int32_t shape)
{
    int32_t p = phase >> 16;
    int32_t s = ds10_mul32(p, shape >> 16) >> 8;

    if (s > OSC_HI)
        s = OSC_HI;
    else if (s < OSC_LO)
        s = OSC_LO;
    return p - s;
}

/* Square/pulse: a folded triangle scaled by shape, which saturates into flat
 * tops. Clamp order is lower bound first and, when it trips, the upper test is
 * skipped entirely - that asymmetry is in the original's branch structure. */
static inline int32_t wave_square(int32_t phase, int32_t shape)
{
    int32_t p = phase >> 16;
    int32_t folded, prod, out;

    if (p <= 0)
        p = -p;
    folded = OSC_LO + ds10_shl(p, 1);
    prod = ds10_mul32(folded, shape >> 16);
    out = prod >> 8;
    if ((prod >> 8) < OSC_LO)
        return OSC_LO;
    if (out > OSC_HI)
        out = OSC_HI;
    return out;
}

int32_t ds10_vco_step(ds10_vco *o)
{
    int32_t old1 = o->phase1;
    int32_t old2 = o->phase2;
    int32_t out1, out2;

    /* Both accumulators count down. */
    o->phase1 = old1 - o->inc1;
    o->phase2 = old2 - o->inc2;

    /* Cross-modulation. Modes 1 and 3 share the phase2 update; mode 2 only
     * touches phase1. The feedback term is the previous VCO2 output. */
    switch (o->xmod) {
    case 1: {
        int32_t g = o->xm_60;
        int32_t t = ds10_mul32(g, ds10_mul32(o->prev2, g) >> 7);

        o->phase1 = o->phase1 - (t >> 1);
        o->phase2 = o->phase2 - ds10_mul24(o->prev2, o->xm_64);
        break;
    }
    case 2: {
        int32_t g = o->xm_68;
        int32_t t = ds10_mul32(g, ds10_mul32(o->prev2, g) >> 7);

        o->phase1 = o->phase1 - (t >> 1);
        break;
    }
    case 3:
        o->phase2 = o->phase2 - ds10_mul24(o->prev2, o->xm_6c);
        break;
    default:
        break;
    }

    /* Hard sync: VCO1 wrapping (counting down, so new > old) resets VCO2. */
    if (o->sync == 1 && o->phase1 > old1)
        o->phase2 = 0x7FFFFFFF;

    /* VCO1. A waveform value of 4 or more leaves the raw phase through, which
     * the knob range never produces but the original allows. */
    out1 = o->phase1;
    switch (o->wave1) {
    case DS10_WAVE_TRIANGLE:
        out1 = wave_triangle(o->phase1);
        break;
    case DS10_WAVE_SAW:
        out1 = wave_saw(o->phase1, o->shape1);
        break;
    case DS10_WAVE_SQUARE:
        out1 = wave_square(o->phase1, o->shape1);
        break;
    case DS10_WAVE_NOISE:
        o->lfsr1 = lfsr_step(o->lfsr1);
        out1 = o->lfsr1 >> 16;
        break;
    default:
        break;
    }

    /* VCO2 mirrors VCO1 except that its noise is sample-and-hold, clocked by
     * phase2 changing sign rather than updated every sample. */
    out2 = o->phase2;
    switch (o->wave2) {
    case DS10_WAVE_TRIANGLE:
        out2 = wave_triangle(o->phase2);
        break;
    case DS10_WAVE_SAW:
        out2 = wave_saw(o->phase2, o->shape2);
        break;
    case DS10_WAVE_SQUARE:
        out2 = wave_square(o->phase2, o->shape2);
        break;
    case DS10_WAVE_NOISE:
        o->lfsr2 = lfsr_step(o->lfsr2);
        if (((o->phase2 ^ old2) >> 31) != 0) {
            o->hold2 = o->lfsr2 >> 16;
            out2 = o->hold2;
        } else {
            out2 = o->hold2;
        }
        break;
    default:
        break;
    }

    o->prev2 = ds10_shl(out2, 9);
    return (ds10_mul32(out1, o->vol1) + ds10_mul32(out2, o->vol2)) >> 15;
}

/* --- per-frame preparation (render preamble, 0x02074AD8..0x02074B98) ---
 *
 * The shaping value handed to the saw and square generators is a reciprocal of
 * the phase increment, hence proportional to the period in samples, and that is
 * what fixes the steep edge at two samples wide whatever the pitch. It is not
 * amplitude that it holds constant: a raw ramp is already +-32767 at every
 * pitch, and the shaping term is what makes the peak fall once the edge takes up
 * a real share of the cycle, down to +-16319 at a four-sample period.
 *
 * The original computes it on the DS hardware divider; the guard below is why
 * the divider is only engaged for increments at or above 0x01000000 - the
 * quotient would otherwise exceed 32 bits. Below that the value saturates
 * instead of growing, so the edge widens as pitch drops: four samples at a
 * 512-sample period, thirty-two at 4096.
 */
#define VCO_RECIP_NUM 0x007FFFFFFF000000ULL
#define VCO_RECIP_MIN 0x01000000
#define VCO_RECIP_SAT 0x7FFFFFFF

static inline int32_t shaping_recip(int32_t inc)
{
    if (inc < VCO_RECIP_MIN)
        return VCO_RECIP_SAT;
    return (int32_t)(VCO_RECIP_NUM / (uint64_t)(uint32_t)inc);
}

void ds10_vco_update(ds10_vco *o, int32_t inc1, int32_t inc2)
{
    o->inc1 = inc1;
    o->inc2 = inc2;
    o->shape1 = shaping_recip(inc1);
    o->shape2 = shaping_recip(inc2);

    /* Cross-mod depths: two used directly, two scaled by the VCO2 increment so
     * the modulation tracks pitch. */
    o->xm_60 = o->pitch_in;
    o->xm_68 = o->pitch1_in;
    o->xm_64 = (int32_t)(((int64_t)inc2 * (int64_t)o->pitch_in) >> 7);
    o->xm_6c = (int32_t)(((int64_t)inc2 * (int64_t)o->pitch2_in) >> 7);
}
