#include "voice.h"

void ds10_voice_render(ds10_voice *v, int16_t *out, int n)
{
    /* The modulation sources are read through small arrays whose last element
     * is a permanent zero - that is how the routing expresses "no
     * modulation" without branching. See ds10_pitchctrl. */
    int32_t env_out[3];
    int32_t lfo_out[5];
    int i;

    /* --- per-frame preamble, in the original's order ------------------- */
    ds10_lfo_step(&v->lfo);
    ds10_env_step(&v->env, &v->env_params);

    env_out[0] = v->env.level;
    env_out[1] = v->env.aux;
    env_out[2] = 0;
    for (i = 0; i < 5; i++)
        lfo_out[i] = v->lfo.out[i];

    ds10_notectrl_step(&v->nc);
    ds10_pitchctrl_step(&v->pc, &v->nc, env_out, lfo_out);

    ds10_vco_update(&v->vco, v->pc.inc1, v->pc.inc2);

    /* The filter's oscillator-modulation term reads OSC+0x4C, which the sample
     * loop last wrote in the PREVIOUS frame. Order matters here. */
    ds10_vcf_update(&v->vcf, env_out, lfo_out, v->vco.prev2);

    ds10_amp_update(&v->amp, env_out, lfo_out);

    /* --- sample loop --------------------------------------------------- */
    for (i = 0; i < n; i++) {
        int32_t mix = ds10_vco_step(&v->vco);
        int32_t filtered = ds10_vcf_step(&v->vcf, mix);

        /* The amp's ring-modulation path wants the raw VCO2 output, which the
         * oscillator stashed as prev2 = out2 << 9. */
        out[i] = ds10_amp_step(&v->amp, filtered, v->vco.prev2 >> 9);
    }
}
