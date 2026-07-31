/* Loading a whole ds10_voice from a captured state, in the field order
 * tools/oracle.py voice_snapshot writes.
 *
 * Shared by the voice test and the song render test, so the two cannot drift
 * apart. The order here must match the V_* field lists in the generator.
 */
#ifndef DS10_VOICE_STATE_H
#define DS10_VOICE_STATE_H

#include "../src/core/voice.h"

/* `next` yields the next captured word. */
static void voice_state_load(ds10_voice *v, int32_t (*next)(void))
{
    int k;

#define s() next()
#define rd32() ((uint32_t)next())
    for (k = 0; k < 5; k++)
        v->lfo.out[k] = s();
    v->lfo.phase_down = s();
    v->lfo.inc = s();
    v->lfo.phase_up = s();
    v->lfo.lfsr = s();

    v->env.stage = (uint8_t)rd32();
    /* The retrigger byte at +0x05. The constructor does not leave it zero, and
     * without it a note-on glides instead of restarting the attack - which is
     * silence when the envelope is already at zero. */
    v->env.retrig = (uint8_t)rd32();
    v->env.level = s();
    v->env.aux = s();
    v->env_params.attack = s();
    v->env_params.decay = s();
    v->env_params.sustain = s();
    v->env_params.release = s();

    v->nc.target = s();
    v->nc.current = s();
    v->nc.rate = s();
    v->nc.dir = (uint8_t)rd32();

    v->pc.eg_depth = s();
    for (k = 0; k < 3; k++)
        v->pc.depth_sel[k] = s();
    v->pc.octave12 = s();
    v->pc.vco2pitch = (int16_t)s();
    v->pc.inc1 = s();
    v->pc.inc2 = s();
    for (k = 0; k < 3; k++)
        v->pc.lfo_sel[k] = (int16_t)s();
    for (k = 0; k < 3; k++)
        v->pc.env_sel[k] = s();

    v->vco.xmod = s();
    v->vco.phase1 = s();
    v->vco.phase2 = s();
    v->vco.inc1 = s();
    v->vco.inc2 = s();
    v->vco.shape1 = s();
    v->vco.shape2 = s();
    v->vco.wave1 = s();
    v->vco.wave2 = s();
    v->vco.vol1 = s();
    v->vco.vol2 = s();
    v->vco.lfsr1 = s();
    v->vco.lfsr2 = s();
    v->vco.sync = s();
    v->vco.prev2 = s();
    v->vco.hold2 = s();
    v->vco.xm_60 = s();
    v->vco.xm_64 = s();
    v->vco.xm_68 = s();
    v->vco.xm_6c = s();
    v->vco.pitch_in = s();
    v->vco.pitch1_in = s();
    v->vco.pitch2_in = s();

    v->vcf.q = s();
    v->vcf.band = s();
    v->vcf.low = s();
    v->vcf.type = s();
    v->vcf.f = s();
    v->vcf.f_delta = s();
    v->vcf.damp = s();
    v->vcf.damp_delta = s();
    v->vcf.lfo_sel = s();
    v->vcf.env_sel = s();
    v->vcf.osc_depth = s();
    v->vcf.peak_raw = s();
    v->vcf.cutoff = s();
    v->vcf.coef = s();
    v->vcf.type_req = s();
    v->vcf.eg_depth = s();
    v->vcf.in_depth = s();
    v->vcf.f_target = s();
    v->vcf.damp_target = s();

    v->amp.dist = s();
    v->amp.wet = s();
    v->amp.level = s();
    v->amp.level_delta = s();
    v->amp.clipped = s();
    v->amp.drive1 = s();
    v->amp.drive2 = s();
    v->amp.lfo_sel = s();
    v->amp.env_sel = s();
    v->amp.level_knob = s();
    v->amp.in_depth = s();
    v->amp.eg_sel = s();
    v->amp.target = s();
#undef s
#undef rd32
}

#endif /* DS10_VOICE_STATE_H */
