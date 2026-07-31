#include "mixer.h"

#include "fixed.h"

#define MIX_ONE 0x7FFF

/* Master gain smoother. 0x7F35 is 0x7FFF - 0xCA, and the increment is 0xCB, so
 * the coefficient is 0.99429 per sample. */
#define GAIN_KEEP 0x7F35
#define GAIN_STEP 0xCB

/* Soft-knee master waveshaper.
 *
 * Reflect about whichever rail the sample is nearer, square the distance, and
 * come back. Net effect is roughly 2x gain that saturates as it approaches full
 * scale - out(0x4000) is 0x6000, out(0x7FFF) is 0x7FFF. It is NOT a clamp, and
 * replacing it with one measurably changes the output. */
static int32_t shape(int32_t x)
{
    int32_t ref = (x < 1) ? -MIX_ONE : MIX_ONE;
    int32_t d = x - ref;
    int32_t a = d < 0 ? -d : d;

    return ref + (ds10_mul32(d, a) >> 15);
}

static int32_t clamp_one(int32_t v)
{
    if (v < -MIX_ONE)
        return -MIX_ONE;
    if (v > MIX_ONE)
        return MIX_ONE;
    return v;
}

void ds10_mixer_render(ds10_mixer *m, const int16_t *const *in,
                       const ds10_fx_tables *t, int16_t *out_l, int16_t *out_r,
                       int n)
{
    int ch, i;

    for (ch = 0; ch < DS10_MIX_CHANNELS; ch++)
        m->prod[ch] = ds10_mul32(m->pan[ch], m->vol[ch]) >> 15;

    ds10_fx_update(&m->fx, t);

    for (i = 0; i < n; i++) {
        int32_t s1l, s1r, s2l, s2r, dl, dr;
        int32_t wet_l, wet_r, byp_l, byp_r, byp_d_l, byp_d_r;
        int32_t fx_l, fx_r, l, r;

        /* Linear pan: the product is the right gain, volume minus it the left. */
        s1r = ds10_mul32(in[0][i], m->prod[0]) >> 15;
        s1l = ds10_mul32(in[0][i], m->vol[0] - m->prod[0]) >> 15;
        s2r = ds10_mul32(in[1][i], m->prod[1]) >> 15;
        s2l = ds10_mul32(in[1][i], m->vol[1] - m->prod[1]) >> 15;

        dr = dl = 0;
        for (ch = 2; ch < DS10_MIX_CHANNELS; ch++) {
            dr += ds10_mul32(in[ch][i], m->prod[ch]) >> 15;
            dl += ds10_mul32(in[ch][i], m->vol[ch] - m->prod[ch]) >> 15;
        }

        /* Routing decides what enters the effect and what bypasses it. */
        wet_l = s1l;
        wet_r = s1r;
        byp_l = s2l;
        byp_r = s2r;
        byp_d_l = dl;
        byp_d_r = dr;
        switch (m->route) {
        case DS10_ROUTE_SYN1:
            break;
        case DS10_ROUTE_SYN2:
            byp_l = s1l;
            byp_r = s1r;
            wet_l = s2l;
            wet_r = s2r;
            break;
        case DS10_ROUTE_SYN12:
            byp_l = 0;
            byp_r = 0;
            wet_l = s1l + s2l;
            wet_r = s1r + s2r;
            break;
        case DS10_ROUTE_DRUMS:
            byp_d_l = s1l;
            byp_d_r = s1r;
            wet_l = dl;
            wet_r = dr;
            break;
        case DS10_ROUTE_ALL:
            byp_l = 0;
            byp_r = 0;
            byp_d_l = 0;
            byp_d_r = 0;
            wet_l = dl + s1l + s2l;
            wet_r = dr + s1r + s2r;
            break;
        case DS10_ROUTE_OFF:
            byp_l = s2l + s1l;
            byp_r = s2r + s1r;
            wet_l = 0;
            wet_r = 0;
            break;
        default:
            break;
        }

        ds10_fx_step(&m->fx, wet_l, wet_r, &fx_l, &fx_r);

        l = clamp_one(byp_l + byp_d_l + fx_l);
        r = clamp_one(byp_r + byp_d_r + fx_r);

        m->gain = (ds10_mul32(m->gain, GAIN_KEEP) >> 15) +
                  (m->mute ? 0 : GAIN_STEP);

        out_l[i] = (int16_t)(ds10_mul32(shape(l), m->gain) >> 15);
        out_r[i] = (int16_t)(ds10_mul32(shape(r), m->gain) >> 15);
    }
}
