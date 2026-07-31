#include "mixer_state.h"

void ds10_mixer_state_load(ds10_mixer *m, const int32_t *f)
{
    int k = 0, c;

    m->gain = f[k++];
    for (c = 0; c < 6; c++)
        m->pan[c] = f[k++];
    for (c = 0; c < 6; c++)
        m->vol[c] = f[k++];
    for (c = 0; c < 6; c++)
        m->prod[c] = f[k++];
    m->route = f[k++];
    m->mute = (int16_t)f[k++];
}

void ds10_fx_state_load(ds10_fx *fx, const int32_t *f)
{
    int k = 0, i;

    fx->type = (int16_t)f[k++];
    fx->sync = (int16_t)f[k++];
    fx->k_time = (int16_t)f[k++];
    fx->k_ratio = (int16_t)f[k++];
    fx->k_lfo = (int16_t)f[k++];
    fx->k_depth = (int16_t)f[k++];
    fx->k_fb = (int16_t)f[k++];
    fx->k_drywet = (int16_t)f[k++];
    fx->fb_enable = (int16_t)f[k++];
    for (i = 0; i < 3; i++)
        fx->lfo_l[i] = f[k++];
    for (i = 0; i < 3; i++)
        fx->lfo_r[i] = f[k++];
    fx->lfo_phase = f[k++];
    fx->lfo_inc = f[k++];
    fx->depth = f[k++];
    fx->feedback = f[k++];
    fx->time = f[k++];
    fx->smooth_l = f[k++];
    fx->smooth_r = f[k++];
    fx->read_l = f[k++];
    fx->read_l_target = f[k++];
    fx->read_l_delta = f[k++];
    fx->read_r = f[k++];
    fx->read_r_target = f[k++];
    fx->read_r_delta = f[k++];
    fx->fb_l = f[k++];
    fx->fb_l_target = f[k++];
    fx->fb_l_delta = f[k++];
    fx->fb_r = f[k++];
    fx->fb_r_target = f[k++];
    fx->fb_r_delta = f[k++];
    fx->dry = f[k++];
    fx->wet = f[k++];
    fx->widx = f[k++];
    fx->ratio_l = f[k++];
    fx->ratio_r = f[k++];
    fx->tempo = f[k++];
}
