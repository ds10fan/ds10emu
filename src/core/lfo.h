/* DS-10 LFO ("MG" on the panel).
 *
 * Reimplemented from the LFO block of the render handler's per-frame preamble
 * (0x0207473C..0x020747D8). Verified bit-exact; see tests/test_lfo.c.
 *
 * Like the envelope, it advances once per 64-sample frame.
 *
 * It computes all four output shapes every frame into the first four words of
 * the object; the modulation matrix then just indexes that array by the
 * mod-source selector, which is why picking a source costs nothing at
 * render time.
 */
#ifndef DS10_LFO_H
#define DS10_LFO_H

#include <stdint.h>

/* Index of each shape within the output array, i.e. mod source - 1. */
enum ds10_lfo_shape {
    DS10_LFO_SAMPLE_HOLD = 0, /* +0x00 */
    DS10_LFO_SAW = 1,         /* +0x04 */
    DS10_LFO_TRIANGLE = 2,    /* +0x08 */
    DS10_LFO_RAMP = 3,        /* +0x0C */
};

typedef struct {
    int32_t out[4];      /* +0x00..+0x0C, indexed by enum ds10_lfo_shape */
    int32_t phase_down;  /* +0x14 counts down by inc */
    int32_t inc;         /* +0x18 shared by both accumulators */
    int32_t phase_up;    /* +0x1C counts up by inc */
    int32_t lfsr;        /* +0x24 noise register for sample-and-hold */
} ds10_lfo;

void ds10_lfo_step(ds10_lfo *l);

#endif /* DS10_LFO_H */
