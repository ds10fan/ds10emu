#include "vcf.h"

#include "fixed.h"

/* Per-sample filter, transcribed from the ARM at 0x02075254..0x02075348.
 *
 * Topology is a Chamberlin state-variable filter, but note the node ordering
 * differs from the textbook form - the hp node is computed first, from the
 * *previous* low and band, and band is integrated before low:
 *
 *     hp   = in*q>>6 - (low + band*damp)
 *     band = band + hp*f
 *     low  = low  + band*f
 *
 * Output tap is selected by `type`, and crucially the taps are read at
 * different points in the update: the hp tap is taken before band is
 * integrated, the band tap after. Reordering these changes the output.
 */
int32_t ds10_vcf_step(ds10_vcf *f, int32_t in)
{
    int32_t hp, out;

    /* `mul r3, r2, r3` is a 32-bit truncating multiply in the original and
     * does overflow for large in*q, so the wrap has to be reproduced. */
    hp = ds10_mul32(in, f->q) >> 6;
    hp -= f->low + ds10_mul23(f->band, f->damp);
    hp = ds10_clamp24(hp);

    /* Default tap: the input passed through, at Q24 scale. Every defined type
     * overrides this, but the original computes it unconditionally. */
    out = ds10_shl(in, 9);
    if (f->type == DS10_VCF_HIGHPASS)
        out = hp;

    f->band = ds10_clamp24(f->band + ds10_mul23(hp, f->f));
    if (f->type == DS10_VCF_BANDPASS)
        out = f->band;

    f->low = ds10_clamp24(f->low + ds10_mul23(f->band, f->f));
    if (f->type == DS10_VCF_LOWPASS)
        out = f->low;

    /* Coefficients ramp once per sample toward the value the frame preamble
     * computed; deltas are (target - current) >> 6, i.e. spread over 64. */
    f->f += f->f_delta;
    f->damp += f->damp_delta;

    return out >> 9;
}

/* Mirrors FUN_02075F58. The 0x40 multiply saturates for all but the very top
 * of the table, which is why high peak values collapse to a narrow range. */
int32_t ds10_vcf_peak_to_q(const uint32_t *peak_table, int peak)
{
    int32_t raw = (int32_t)peak_table[peak];
    int32_t t = 0xFFFFFF - (raw >> 7);
    int32_t t2 = t * 0x40;

    if (t2 > 0xFFFFFF)
        t2 = 0xFFFFFF;
    return ((t2 >> 1) + 0x800000) >> 9;
}
