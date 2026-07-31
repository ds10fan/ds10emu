/* DS-10 VCF - Chamberlin state-variable filter.
 *
 * Reimplemented from the per-sample filter section of the original render
 * handler at 0x02074720 (the block spanning 0x02075254..0x02075348).
 * Verified bit-exact against the original code; see tests/test_vcf.c.
 */
#ifndef DS10_VCF_H
#define DS10_VCF_H

#include <stdint.h>

enum ds10_vcf_type {
    DS10_VCF_LOWPASS = 0,   /* output = low integrator  */
    DS10_VCF_HIGHPASS = 1,  /* output = the hp node     */
    DS10_VCF_BANDPASS = 2,  /* output = band integrator */
};

/* Field names map to offsets in the original 100-byte FILTER object:
 *   q +0x24, band +0x34, low +0x38, type +0x40,
 *   f +0x50, f_delta +0x54, damp +0x5C, damp_delta +0x60
 *
 * f and damp are recomputed once per 64-sample frame and then linearly ramped
 * per sample by their deltas, so a frame boundary is where new cutoff and
 * resonance settings take effect. */
typedef struct {
    int32_t q;           /* resonance, from the peak knob via 0x02075F58 */
    int32_t band;        /* bandpass integrator state */
    int32_t low;         /* lowpass integrator state */
    int32_t type;        /* enum ds10_vcf_type - the ACTIVE type, +0x40 */
    int32_t f;           /* cutoff coefficient, ramped */
    int32_t f_delta;
    int32_t damp;        /* damping coefficient, ramped */
    int32_t damp_delta;

    /* Per-frame control fields. Offsets refer to the 100-byte FILTER object. */
    int32_t lfo_sel;     /* +0x04 */
    int32_t env_sel;     /* +0x08 */
    int32_t osc_depth;   /* +0x0C scales the oscillator-as-modulator term */
    int32_t peak_raw;    /* +0x20 raw peak-table value; drives the damping */
    int32_t cutoff;      /* +0x1C knob << 15 */
    int32_t coef;        /* +0x28 cutoff coefficient from the table */
    int32_t type_req;    /* +0x3C requested type, copied to `type` on change */
    int32_t eg_depth;    /* +0x44 depth index for the always-on EG term */
    int32_t in_depth;    /* +0x48 depth index for the routed term */
    int32_t f_target;    /* +0x4C ramp target for f */
    int32_t damp_target; /* +0x58 ramp target for damp */
} ds10_vcf;

/* ROM tables the cutoff mapping needs.
 *   mod_a  0x020803C4, 128 x s16 - depth curve for the routed terms
 *   mod_b  0x020804C4, 128 x s16 - depth curve for the oscillator term
 *   coef   0x020807C4, 151 x u32 - cutoff coefficient, read at idx and idx+1 */
typedef struct {
    const int16_t *mod_a;
    const int16_t *mod_b;
    const int32_t *coef;
} ds10_vcf_tables;

/* Recompute the cutoff coefficients for the coming frame and set up their
 * per-sample ramps. `env_out` is ENV+0x08 (3 entries), `lfo_out` the LFO
 * output array (5 entries), `osc_prev2` the oscillator's OSC+0x4C feedback. */
void ds10_vcf_update(ds10_vcf *f, const int32_t *env_out,
                     const int32_t *lfo_out, int32_t osc_prev2,
                     const ds10_vcf_tables *t);

/* Advance one sample. `in` is the mixed oscillator output (Q15-ish); the
 * return value is in the same scale. */
int32_t ds10_vcf_step(ds10_vcf *f, int32_t in);

/* Map the peak knob (0..127) to the resonance coefficient `q`.
 * Mirrors FUN_02075F58; the table lives at 0x020805C4 in the ROM. */
int32_t ds10_vcf_peak_to_q(const uint32_t *peak_table, int peak);

#endif /* DS10_VCF_H */
