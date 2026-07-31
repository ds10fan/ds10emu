/* DS-10 VCA / drive stage - the last block in the per-sample chain.
 *
 * Reimplemented from 0x0207534C..0x02075410 of the render handler. Verified
 * bit-exact; see tests/test_amp.c.
 *
 * The stage does three things: a two-path drive that sums a hard-clipped copy
 * with a scaled clean copy, an optional ring-modulation-style "extra
 * distortion" against the VCO2 output, and the level multiply. The level is
 * ramped per sample from a per-frame target, same scheme as the filter
 * coefficients.
 *
 * Output range: the signal is clamped to +-16383 and only then doubled, which
 * is why the engine's peak output is +-32766 rather than +-32767.
 */
#ifndef DS10_AMP_H
#define DS10_AMP_H

#include <stdint.h>

typedef struct {
    int32_t dist;        /* +0x0C nonzero enables the extra distortion path */
    int32_t wet;         /* +0x20 wet/dry balance for that path */
    int32_t level;       /* +0x2C ramped output level */
    int32_t level_delta; /* +0x30 per-sample ramp increment */
    int32_t clipped;     /* +0x34 written by the stage; also a mod source */
    int32_t drive1;      /* +0x38 (knob*0x102)^2 >> 15 */
    int32_t drive2;      /* +0x3C 0x7FFF - knob*0xE8; 0x7FFF is fully clean */
} ds10_amp;

/* `in` is the filter output, `vco2` the raw VCO2 output (only used when the
 * extra distortion path is enabled). Returns the sample written to the voice's
 * output buffer. */
int16_t ds10_amp_step(ds10_amp *a, int32_t in, int32_t vco2);

#endif /* DS10_AMP_H */
