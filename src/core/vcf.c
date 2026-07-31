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

/* --- per-frame cutoff mapping (render preamble, 0x02074A9C..0x02074BF0) ---
 *
 * Cutoff is accumulated in the same semitone-ish domain as pitch but with a
 * different offset and range: the sum is biased by 0xB8000 before indexing, so
 * negative modulation still lands inside the table.
 */
#define VCF_MOD_SCALE 0x7F
#define VCF_BIAS 0x000B8000
#define VCF_CLAMP_LO (-0x000B8000)
#define VCF_CLAMP_HI 0x003F8000
#define VCF_FRAC_MASK 0x7FFF
#define VCF_ONE 0x01000000

/* Constant added to the raw coefficient before squaring. As a signed 32-bit
 * value this is large and negative, so the sum is usually clamped to zero and
 * the resonance term only becomes non-trivial for high cutoff settings. */
#define VCF_RES_BIAS (-1367566549) /* 0xAE7C972B */
#define VCF_RES_SCALE 0x01800000

void ds10_vcf_update(ds10_vcf *f, const int32_t *env_out,
                     const int32_t *lfo_out, int32_t osc_prev2,
                     const ds10_vcf_tables *t)
{
    int32_t mod, v, idx, frac, lo, hi;
    int32_t res, sq, damped, target;

    /* Three modulation terms, same shape as the pitch block but with s16 depth
     * curves instead of s32, and a third term driven by the oscillator. */
    mod = ds10_mul32(t->mod_a[f->in_depth],
                     (env_out[f->env_sel] + lfo_out[f->lfo_sel]) >> 9) >> 15;
    mod += ds10_mul32(t->mod_a[f->eg_depth], env_out[0] >> 9) >> 15;
    mod += ds10_mul32(t->mod_b[f->in_depth],
                      ds10_mul32(f->osc_depth, osc_prev2) >> 9) >> 15;

    v = mod * VCF_MOD_SCALE + f->cutoff;
    if (v < VCF_CLAMP_LO)
        v = VCF_CLAMP_LO;
    else if (v > VCF_CLAMP_HI)
        v = VCF_CLAMP_HI;

    v += VCF_BIAS;
    idx = v >> 15;
    frac = v & VCF_FRAC_MASK;
    lo = t->coef[idx];
    hi = t->coef[idx + 1];
    f->coef = lo + ds10_mul15(frac, hi - lo);

    /* Resonance: the coefficient is biased, squared, and used to scale down the
     * peak setting, so resonance narrows as cutoff rises. */
    res = f->coef + VCF_RES_BIAS;
    if (res < 1)
        res = 0;
    sq = (int32_t)(((int64_t)res * res) >> 31);
    damped = ds10_mul24(f->peak_raw >> 7,
                        VCF_ONE - ds10_mul24(sq >> 7, VCF_RES_SCALE));

    /* damp target, then f target. Both ramp over the 64 samples of the frame. */
    target = ds10_mul24(VCF_ONE - damped, VCF_ONE - (f->coef >> 8));
    f->damp_delta = (target - f->damp_target) >> 6;
    f->damp = f->damp_target;
    f->damp_target = target;

    target = ds10_mul24((damped >> 1) + 0x800000, f->coef >> 7);
    f->f_delta = (target - f->f_target) >> 6;
    f->f = f->f_target;
    f->f_target = target;

    /* Changing filter type clears both integrators, so switching type is not
     * click-free in the original either. */
    if (f->type != f->type_req) {
        f->type = f->type_req;
        f->band = 0;
        f->low = 0;
    }
}
