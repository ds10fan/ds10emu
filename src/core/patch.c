#include "patch.h"
#include "tables_data.h"

#include "fixed.h"

/* Modulation source -> (lfo index, env index). Index 4 of the LFO array and
 * index 2 of the envelope pair are words those objects never write, so they
 * read zero; routing a selector there is how "no modulation" is expressed
 * without a branch. Verified against the original for all seven sources.
 *
 *   0 none   1 "sine" (actually triangle)   2 saw   3 square
 *   4 S&H    5 EG                           6 VCO2 (handled by cross-mod)
 */
static const struct {
    int8_t lfo;
    int8_t env;
} MOD_ROUTE[7] = {
    {4, 2}, {2, 2}, {1, 2}, {3, 2}, {0, 2}, {4, 0}, {4, 2},
};

/* Balance/pan law. 0x4444 is 17476, i.e. 0.5333 in Q15 - headroom for summing
 * two oscillators. The curve is quadratic rather than the cos/sin an equal-power
 * pan would use. */
#define BAL_STEP 0x102  /* 258 */
#define BAL_ONE 0x7FFC  /* 32764 */
#define BAL_GAIN 0x4444 /* 17476 */

#define DRIVE_STEP 0x102
#define DRIVE_BASE 0x7FFF
#define DRIVE_SLOPE 0xE8

#define VCA_IN_STEP 0x41041
#define VCA_WET_STEP 0x102
#define AMP_LEVEL_STEP 0x20408

static void set_mod_source(ds10_voice *v, int dest, int src)
{
    int lfo, env;

    if (src > 6)
        src = 6;
    lfo = MOD_ROUTE[src].lfo;
    env = MOD_ROUTE[src].env;

    /* Destinations 0..2 drive both the pitch block and the oscillator; 3 is the
     * filter and 4 the amp.
     *
     * Source 6 is the other oscillator - audio-rate modulation, which the VCO,
     * filter and amp do themselves rather than through the modulation matrix, so
     * it has a side effect per destination on top of the routing above. */
    switch (dest) {
    case 0:
    case 1:
    case 2:
        v->pc.lfo_sel[dest] = (int16_t)lfo;
        v->pc.env_sel[dest] = env;
        /* One shared field says WHICH pitch destination is cross-modulated, so
         * only source 0 - "none" - on the destination that owns it clears it.
         * Selecting an LFO on a cross-modulated destination leaves cross
         * modulation running, and clearing a different destination does nothing. */
        if (src == 6)
            v->vco.xmod = dest + 1;
        else if (src == 0 && v->vco.xmod == dest + 1)
            v->vco.xmod = 0;
        break;
    case 3:
        v->vcf.lfo_sel = lfo;
        v->vcf.env_sel = env;
        v->vcf.osc_depth = (src == 6);
        break;
    case 4:
        v->amp.lfo_sel = lfo;
        v->amp.env_sel = env;
        v->amp.dist = (src == 6);
        break;
    default:
        break;
    }
}

static void set_lfo_rate(ds10_voice *v, int knob)
{
    if (knob < 0)
        knob = 0;
    else if (knob > 0x7F)
        knob = 0x7F;
    v->lfo.rate_knob = (int16_t)knob;

    if (v->lfo.bpm_sync) {
        uint32_t u = (knob < 0x7E) ? ds10_tbl_lfo_bpm[knob >> 3] : 0x4459A045u;

        /* Split into high and low halves so the product fits: the tempo value
         * is 24-bit-ish and the multiplier can be large. */
        v->lfo.inc = ds10_mul32((int32_t)u >> 9, v->lfo.bpm) +
                     (ds10_mul32((int32_t)(u & 0x1FF), v->lfo.bpm) >> 9);
    } else {
        v->lfo.inc = (int32_t)ds10_tbl_lfo_free[knob];
    }
}

void ds10_patch_set_knob(ds10_voice *v, int knob, int value)
{
    switch (knob) {
    case 0: /* octave */
        v->pc.octave12 = value * 12;
        break;
    case 1: /* pitch EG intensity */
        v->pc.eg_depth = value + 0x40;
        break;
    case 2: /* portamento */
        v->nc.rate = (int32_t)ds10_tbl_porta[value];
        break;
    case 3: /* pitch.in depth - common */
        v->vco.pitch_in = value + 0x3F;
        v->pc.depth_sel[0] = value + 0x40;
        break;
    case 4: /* pitch1.in depth - VCO1 */
        v->vco.pitch1_in = value + 0x3F;
        v->pc.depth_sel[1] = value + 0x40;
        break;
    case 5: /* pitch2.in depth - VCO2 */
        v->vco.pitch2_in = value + 0x3F;
        v->pc.depth_sel[2] = value + 0x40;
        break;
    case 6: /* VCO1 waveform */
        v->vco.wave1 = value > 3 ? 3 : value;
        break;
    case 7: /* VCO2 waveform */
        v->vco.wave2 = value > 3 ? 3 : value;
        break;
    case 8: /* VCO2 detune */
        v->pc.vco2pitch = (int16_t)(value + 0x40);
        break;
    case 9: { /* balance */
        int32_t a = value * BAL_STEP;
        int32_t b = (BAL_ONE + 2) - a;

        v->vco.balance = value;
        v->vco.vol1 = ds10_mul32(BAL_ONE - (ds10_mul32(a, a) >> 15),
                                 BAL_GAIN) >> 15;
        v->vco.vol2 = ds10_mul32(BAL_ONE - (ds10_mul32(b, b) >> 15),
                                 BAL_GAIN) >> 15;
        break;
    }
    case 10: /* VCO sync */
        v->vco.sync = value;
        break;
    case 11: /* cutoff */
        v->vcf.cutoff = ds10_shl(value, 15);
        break;
    case 12: /* peak */
        v->vcf.peak_raw = (int32_t)ds10_tbl_peak[value];
        v->vcf.q = ds10_vcf_peak_to_q(value);
        break;
    case 13: /* filter type */
        v->vcf.type_req = value;
        break;
    case 14: /* cutoff EG intensity */
        v->vcf.eg_depth = value + 0x40;
        break;
    case 15: /* cutoff.in depth */
        v->vcf.in_depth = value + 0x40;
        break;
    case 16: /* VCA EG source */
        v->amp.eg_sel = (value == 0) ? 0 : (value == 1) ? 1 : 2;
        break;
    case 17: /* vca.in depth */
        v->amp.in_depth = value * VCA_IN_STEP;
        v->amp.wet = (value + 0x40) * VCA_WET_STEP;
        break;
    case 18: { /* drive */
        int32_t d = value * DRIVE_STEP;

        v->amp.drive1 = ds10_mul32(d, d) >> 15;
        v->amp.drive2 = DRIVE_BASE - value * DRIVE_SLOPE;
        break;
    }
    case 19: /* level */
        v->amp.level_knob = value * AMP_LEVEL_STEP;
        break;
    case 20: /* attack */
        v->env_params.attack = (int32_t)ds10_tbl_env_rate[value];
        break;
    case 21: /* decay */
        v->env_params.decay = (int32_t)ds10_tbl_env_rate[value];
        break;
    case 22: /* sustain - a level, not a rate, so no table */
        v->env_params.sustain = ds10_shl(value, 17);
        break;
    case 23: /* release */
        v->env_params.release = (int32_t)ds10_tbl_env_rate[value];
        break;
    case 24: /* LFO rate */
        set_lfo_rate(v, value);
        break;
    case 25: /* LFO BPM sync - re-derives the rate from the stored knob */
        if (v->lfo.bpm_sync != (value != 0)) {
            v->lfo.bpm_sync = (value != 0);
            set_lfo_rate(v, v->lfo.rate_knob);
        }
        break;
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
        set_mod_source(v, knob - 26, value);
        break;
    default:
        break;
    }
}

void ds10_patch_load(ds10_voice *v, const int8_t *knobs)
{
    int i;

    for (i = 0; i < DS10_NKNOBS; i++)
        ds10_patch_set_knob(v, i, knobs[i]);
}
