/* DS-10 dual oscillator section.
 *
 * Reimplemented from the oscillator block of the render handler at
 * 0x02074720 (the span 0x02074F98..0x02075250). Verified bit-exact against
 * the original code; see tests/test_vco.c.
 *
 * Both phase accumulators count *down*, and a wrap is detected as
 * new > old. Waveform enum measured by sweeping knob 6:
 *   0 = triangle, 1 = sawtooth, 2 = square, 3 = noise
 * This is NOT the same enum as the modulation-source list (knobs 26-30).
 */
#ifndef DS10_VCO_H
#define DS10_VCO_H

#include <stdint.h>

enum ds10_wave {
    DS10_WAVE_TRIANGLE = 0,
    DS10_WAVE_SAW = 1,
    DS10_WAVE_SQUARE = 2,
    DS10_WAVE_NOISE = 3,
};

/* Field names map to offsets in the original 160-byte OSC object. */
typedef struct {
    int32_t xmod;        /* +0x0C cross-modulation mode, 0..3 */
    int32_t phase1;      /* +0x14 */
    int32_t phase2;      /* +0x18 */
    int32_t inc1;        /* +0x1C phase increment (subtracted) */
    int32_t inc2;        /* +0x20 */
    int32_t shape1;      /* +0x24 waveform shaping amount for VCO1 */
    int32_t shape2;      /* +0x28 */
    int32_t wave1;       /* +0x2C enum ds10_wave */
    int32_t wave2;       /* +0x30 */
    int32_t vol1;        /* +0x38 */
    int32_t vol2;        /* +0x3C */
    int32_t lfsr1;       /* +0x40 noise register for VCO1 */
    int32_t lfsr2;       /* +0x44 noise register for VCO2 */
    int32_t sync;        /* +0x48 1 = reset VCO2 when VCO1 wraps */
    int32_t prev2;       /* +0x4C previous VCO2 output << 9 */
    int32_t hold2;       /* +0x50 sample-and-hold for VCO2 noise */
    int32_t xm_60;       /* +0x60 cross-mod coefficients */
    int32_t xm_64;       /* +0x64 */
    int32_t xm_68;       /* +0x68 */
    int32_t xm_6c;       /* +0x6C */

    /* Per-frame control. Appended so the per-sample test's flat field mapping
     * over the first twenty members stays valid. */
    int32_t pitch_in;    /* +0x54 knob 3 + 0x3F */
    int32_t pitch1_in;   /* +0x58 knob 4 + 0x3F */
    int32_t pitch2_in;   /* +0x5C knob 5 + 0x3F */
} ds10_vco;

/* Take the two phase increments the pitch block produced and derive everything
 * the sample loop needs: the shaping reciprocals and the cross-mod
 * coefficients. */
void ds10_vco_update(ds10_vco *o, int32_t inc1, int32_t inc2);

/* Advance one sample; returns the mixed output that feeds the filter. */
int32_t ds10_vco_step(ds10_vco *o);

#endif /* DS10_VCO_H */
