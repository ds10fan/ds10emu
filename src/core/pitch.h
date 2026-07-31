/* DS-10 note tracking and pitch control.
 *
 * Reimplemented from the NOTECTRL and PITCHCTRL blocks of the render handler's
 * per-frame preamble (0x02074900..0x02074A9C). Verified bit-exact; see
 * tests/test_pitch.c.
 *
 * Both run once per 64-sample frame, so portamento and pitch modulation update
 * at 512 Hz while the oscillators run at 32768 Hz.
 *
 * NOTECTRL is the portamento glide: `current` chases `target` and is clamped so
 * it cannot overshoot, with the clamp direction chosen by `dir`.
 *
 * PITCHCTRL turns that plus three modulation contributions into the two
 * oscillator phase increments, via a semitone-indexed pitch table with linear
 * interpolation on the fractional part.
 */
#ifndef DS10_PITCH_H
#define DS10_PITCH_H

#include <stdint.h>

/* ROM tables. `depth` is the bipolar modulation-depth curve at 0x0207EB38
 * (128 signed entries spanning roughly -32768..+32767, indexed by knob+0x40).
 * `pitch` is the semitone pitch table at 0x0207E950; the code reads entry and
 * entry+1 to interpolate, so it must have one extra readable element. */
typedef struct {
    const int32_t *depth;
    const int32_t *pitch;
} ds10_pitch_tables;

typedef struct {
    int32_t target;   /* +0x00 note << 15 */
    int32_t current;  /* +0x04 glides toward target */
    int32_t rate;     /* +0x0C portamento rate */
    uint8_t dir;      /* +0x10 0 = gliding down, else up */
} ds10_notectrl;

typedef struct {
    int32_t eg_depth;      /* +0x20 depth index for the always-on EG term */
    int32_t depth_sel[3];  /* +0x24 common, +0x28 VCO1, +0x2C VCO2 */
    int32_t octave12;      /* +0x30 octave knob * 12 */
    int16_t vco2pitch;     /* +0x34 depth index, VCO2 detune */
    int32_t inc1;          /* +0x38 output: VCO1 phase increment */
    int32_t inc2;          /* +0x3C output: VCO2 phase increment */
    int16_t lfo_sel[3];    /* +0x40 common, +0x44 VCO1, +0x48 VCO2 */
    int32_t env_sel[3];    /* +0x4C common, +0x50 VCO1, +0x54 VCO2 */
} ds10_pitchctrl;

void ds10_notectrl_step(ds10_notectrl *n);

/* `env_out` is the envelope's output pair (level, aux) - i.e. a pointer to
 * ENV+0x08. `lfo_out` is the LFO's four-shape array. */
void ds10_pitchctrl_step(ds10_pitchctrl *p, const ds10_notectrl *n,
                         const int32_t *env_out, const int32_t *lfo_out,
                         const ds10_pitch_tables *t);

#endif /* DS10_PITCH_H */
