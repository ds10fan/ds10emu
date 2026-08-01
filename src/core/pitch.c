#include "pitch.h"
#include "tables_data.h"

#include "fixed.h"

/* Pitch is accumulated in semitones scaled by 0x8000, so one table entry per
 * semitone with a 15-bit fraction between entries. */
#define PITCH_FRAC_MASK 0x7FFF
#define PITCH_SHIFT 15
#define PITCH_MAX 0x003C0000 /* 120 semitones */

/* Modulation contributions are summed at 1/0x30 scale and multiplied up. */
#define MOD_SCALE 0x30

void ds10_notectrl_step(ds10_notectrl *n)
{
    n->current += ds10_mul15(n->rate, n->target - n->current);

    /* Clamp so the glide stops at the target rather than overshooting. Which
     * side to clamp depends on the direction the glide is travelling. */
    if (n->dir == 0) {
        if (n->current < n->target)
            n->current = n->target;
    } else {
        if (n->target < n->current)
            n->current = n->target;
    }
}

/* One modulation term: depth * ((lfo + env) >> 9), at 32-bit precision. */
static inline int32_t mod_term(int32_t depth_sel, int32_t lfo_val,
                               int32_t env_val)
{
    return ds10_mul32(ds10_tbl_depth[depth_sel], (lfo_val + env_val) >> 9) >> 15;
}

/* Semitone table lookup with linear interpolation on the 15-bit fraction. */
static inline int32_t pitch_lookup(int32_t v)
{
    int32_t idx, frac, lo, hi;

    if (v < 0)
        v = 0;
    else if (v > PITCH_MAX)
        v = PITCH_MAX;

    idx = v >> PITCH_SHIFT;
    frac = v & PITCH_FRAC_MASK;
    lo = ds10_tbl_pitch[idx];
    hi = ds10_tbl_pitch[idx + 1];
    return lo + ds10_mul15(frac, hi - lo);
}

void ds10_pitchctrl_step(ds10_pitchctrl *p, const ds10_notectrl *n,
                         const int32_t *env_out, const int32_t *lfo_out)
{
    int32_t common, base, v;

    /* Shared by both oscillators: one routable term plus an always-on envelope
     * term whose depth comes from the pitch EG-intensity knob. */
    common = mod_term(p->depth_sel[0], lfo_out[p->lfo_sel[0]],
                      env_out[p->env_sel[0]]);
    common += ds10_mul32(ds10_tbl_depth[p->eg_depth], env_out[0] >> 9) >> 15;

    /* Note pitch after portamento, offset by the octave switch. */
    base = n->current + p->octave12 * 0x8000;

    v = (common + mod_term(p->depth_sel[1], lfo_out[p->lfo_sel[1]],
                           env_out[p->env_sel[1]])) * MOD_SCALE + base;
    p->inc1 = pitch_lookup(v);

    /* VCO2 additionally carries its own detune, applied at the same scale as
     * the modulation sum rather than as a plain semitone offset. */
    v = (common + mod_term(p->depth_sel[2], lfo_out[p->lfo_sel[2]],
                           env_out[p->env_sel[2]])) * MOD_SCALE + base +
        ds10_tbl_depth[p->vco2pitch] * MOD_SCALE;
    p->inc2 = pitch_lookup(v);
}
