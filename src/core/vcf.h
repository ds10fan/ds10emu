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
    int32_t type;        /* enum ds10_vcf_type */
    int32_t f;           /* cutoff coefficient, ramped */
    int32_t f_delta;
    int32_t damp;        /* damping coefficient, ramped */
    int32_t damp_delta;
} ds10_vcf;

/* Advance one sample. `in` is the mixed oscillator output (Q15-ish); the
 * return value is in the same scale. */
int32_t ds10_vcf_step(ds10_vcf *f, int32_t in);

/* Map the peak knob (0..127) to the resonance coefficient `q`.
 * Mirrors FUN_02075F58; the table lives at 0x020805C4 in the ROM. */
int32_t ds10_vcf_peak_to_q(const uint32_t *peak_table, int peak);

#endif /* DS10_VCF_H */
