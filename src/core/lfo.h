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

/* Index of each shape within the output array. The pitch/cutoff modulation
 * routing pins these down exactly (see ds10_pitchctrl): mod source 4 selects
 * index 0, source 2 -> 1, source 1 -> 2, source 3 -> 3.
 *
 * Note source 1 is labelled "sine" on the panel but selects index 2, which is
 * the triangle. There is no sine anywhere in this LFO. */
enum ds10_lfo_shape {
    DS10_LFO_SAMPLE_HOLD = 0, /* +0x00 mod source 4 */
    DS10_LFO_SAW = 1,         /* +0x04 mod source 2 */
    DS10_LFO_TRIANGLE = 2,    /* +0x08 mod source 1, panel "sine" */
    DS10_LFO_SQUARE = 3,      /* +0x0C mod source 3 */
};

typedef struct {
    /* out[0..3] are the shapes; out[4] (+0x10) is never written and is the
     * "no modulation" slot the routing points at. Keep it in the struct so
     * selector indexing stays in bounds. */
    int32_t out[5];      /* +0x00..+0x10 */
    int32_t phase_down;  /* +0x14 counts down by inc */
    int32_t inc;         /* +0x18 shared by both accumulators */
    int32_t phase_up;    /* +0x1C counts up by inc */
    int32_t lfsr;        /* +0x24 noise register for sample-and-hold */
} ds10_lfo;

void ds10_lfo_step(ds10_lfo *l);

#endif /* DS10_LFO_H */
