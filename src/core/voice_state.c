#include "voice_state.h"

void ds10_voice_state_load(ds10_voice *v, const int32_t *f)
{
    int k, i = 0;

    for (k = 0; k < 5; k++)
        v->lfo.out[k] = f[i++];
    v->lfo.phase_down = f[i++];
    v->lfo.inc = f[i++];
    v->lfo.phase_up = f[i++];
    v->lfo.lfsr = f[i++];

    v->env.stage = (uint8_t)f[i++];
    /* The retrigger byte at +0x05. The constructor does not leave it zero, and
     * without it a note-on glides instead of restarting the attack - which is
     * silence when the envelope is already at zero. */
    v->env.retrig = (uint8_t)f[i++];
    v->env.level = f[i++];
    v->env.aux = f[i++];
    v->env_params.attack = f[i++];
    v->env_params.decay = f[i++];
    v->env_params.sustain = f[i++];
    v->env_params.release = f[i++];

    v->nc.target = f[i++];
    v->nc.current = f[i++];
    v->nc.rate = f[i++];
    v->nc.dir = (uint8_t)f[i++];

    v->pc.eg_depth = f[i++];
    for (k = 0; k < 3; k++)
        v->pc.depth_sel[k] = f[i++];
    v->pc.octave12 = f[i++];
    v->pc.vco2pitch = (int16_t)f[i++];
    v->pc.inc1 = f[i++];
    v->pc.inc2 = f[i++];
    for (k = 0; k < 3; k++)
        v->pc.lfo_sel[k] = (int16_t)f[i++];
    for (k = 0; k < 3; k++)
        v->pc.env_sel[k] = f[i++];

    v->vco.xmod = f[i++];
    v->vco.phase1 = f[i++];
    v->vco.phase2 = f[i++];
    v->vco.inc1 = f[i++];
    v->vco.inc2 = f[i++];
    v->vco.shape1 = f[i++];
    v->vco.shape2 = f[i++];
    v->vco.wave1 = f[i++];
    v->vco.wave2 = f[i++];
    v->vco.vol1 = f[i++];
    v->vco.vol2 = f[i++];
    v->vco.lfsr1 = f[i++];
    v->vco.lfsr2 = f[i++];
    v->vco.sync = f[i++];
    v->vco.prev2 = f[i++];
    v->vco.hold2 = f[i++];
    v->vco.xm_60 = f[i++];
    v->vco.xm_64 = f[i++];
    v->vco.xm_68 = f[i++];
    v->vco.xm_6c = f[i++];
    v->vco.pitch_in = f[i++];
    v->vco.pitch1_in = f[i++];
    v->vco.pitch2_in = f[i++];

    v->vcf.q = f[i++];
    v->vcf.band = f[i++];
    v->vcf.low = f[i++];
    v->vcf.type = f[i++];
    v->vcf.f = f[i++];
    v->vcf.f_delta = f[i++];
    v->vcf.damp = f[i++];
    v->vcf.damp_delta = f[i++];
    v->vcf.lfo_sel = f[i++];
    v->vcf.env_sel = f[i++];
    v->vcf.osc_depth = f[i++];
    v->vcf.peak_raw = f[i++];
    v->vcf.cutoff = f[i++];
    v->vcf.coef = f[i++];
    v->vcf.type_req = f[i++];
    v->vcf.eg_depth = f[i++];
    v->vcf.in_depth = f[i++];
    v->vcf.f_target = f[i++];
    v->vcf.damp_target = f[i++];

    v->amp.dist = f[i++];
    v->amp.wet = f[i++];
    v->amp.level = f[i++];
    v->amp.level_delta = f[i++];
    v->amp.clipped = f[i++];
    v->amp.drive1 = f[i++];
    v->amp.drive2 = f[i++];
    v->amp.lfo_sel = f[i++];
    v->amp.env_sel = f[i++];
    v->amp.level_knob = f[i++];
    v->amp.in_depth = f[i++];
    v->amp.eg_sel = f[i++];
    v->amp.target = f[i++];

    /* Appended last, see V_LFO2 in the generator. */
    v->lfo.bpm_sync = (uint8_t)f[i++];
    v->lfo.bpm = f[i++];
    v->lfo.rate_knob = (int16_t)f[i++];
}
