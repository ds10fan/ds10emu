/* See drumbuf.h, and recon.md section 14 for where each constant comes from. */
#include "drumbuf.h"

#include <string.h>

#include "event.h"
#include "fixed.h"
#include "mixer.h"
#include "patch.h"
#include "tables_data.h"
#include "voice_state.h"

/* 32764. Two paths produce it and they are the same number: the render's channel
 * volume, knob 17 = 127 through value*value*0x10404 >> 15, and the effect's dry
 * gain at dry/wet 0, which is fx.c's DRYWET_ONE. Whichever way a drum is routed,
 * its signal is multiplied by this exactly once before the master stage. */
#define CH_VOL (((127 * 127) * 0x10404) >> 15)

/* The master stage, 0x02074054..0x020740A8: clamp, soft knee, gain. */
#define MIX_ONE 0x7FFF

/* The gain the handler at 0x02072DAC writes once its counter reaches 64, and the
 * coefficient its smoother decays by once nothing is adding to it - the literal
 * at 0x020740E8. */
#define GAIN_UNITY 0x7FD7
#define GAIN_KEEP 0x7D85

/* Where the fade sits. 0x02070728 asks to stop when fewer than eight frames of
 * buffer are left, and it is asked once a frame, so the first frame that sees
 * `remaining/64 < 8` is the one starting seven frames from the end. */
#define FADE_AT (DS10_DRUMBUF_LEN - 7 * 64)

/* The frame that runs before the note goes on. */
#define PRE_FRAME 64

/* Enough ds10_fx_update calls for the effect's ramped parameters to stop moving. */
#define FX_SETTLE 128

static int32_t clamp_one(int32_t v)
{
    if (v < -MIX_ONE) return -MIX_ONE;
    if (v > MIX_ONE) return MIX_ONE;
    return v;
}

static int32_t shape(int32_t x)
{
    int32_t ref = (x < 1) ? -MIX_ONE : MIX_ONE;
    int32_t d = x - ref;
    int32_t a = d < 0 ? -d : d;

    return ref + (ds10_mul32(d, a) >> 15);
}

/* Which routes send the render's channel - channel 0, a synth slot - into the
 * effect. The others leave it dry, and knob 0 being OFF also forces dry/wet to
 * zero, exactly as ds10_mixer_set_knob does. */
static int routed_wet(int route)
{
    return route == DS10_ROUTE_SYN1 || route == DS10_ROUTE_SYN12 ||
           route == DS10_ROUTE_ALL;
}

void ds10_drumbuf_render(const int8_t *knobs, const int8_t *extra,
                         ds10_fx *fx, int16_t *out, int frame)
{
    ds10_voice v;

    memset(&v, 0, sizeof v);
    ds10_voice_state_load(&v, ds10_tbl_voice_init);
    ds10_drumbuf_render_with(&v, knobs, extra, fx, out, frame);
}

void ds10_drumbuf_render_with(ds10_voice *vp, const int8_t *knobs,
                              const int8_t *extra, ds10_fx *fx, int16_t *out,
                              int frame)
{
    ds10_voice v = *vp;
    int32_t gain = GAIN_UNITY;
    int wet = 0, i;

    if (frame <= 0) frame = 64;
    if (fx && extra) {
        /* The nine as mixer knobs 0..8: 0 is the route, 1..8 the effect's own
         * parameters, which is the same id space ds10_fx_set takes. */
        int k;

        for (k = 1; k <= 8; k++)
            ds10_fx_set(fx, k, extra[k]);
        if (extra[0] == DS10_ROUTE_OFF)
            ds10_fx_set(fx, DS10_FX_P_DRYWET, 0);
        wet = routed_wet(extra[0]);
        /* The effect this render pushes knobs into is a machine that has been
         * running since boot, so its ramped parameters are already at their
         * targets. Ours start from the constructor's, so settle them: 128 steps
         * is past where the delay's read pointer stops moving. */
        for (k = 0; k < FX_SETTLE; k++)
            ds10_fx_update(fx);
    }
    ds10_patch_load(&v, knobs);
    /* The render's first frame runs before the voice has a pitch: the phases stay
     * put but the noise registers clock, so the hardware's noise is 64 samples
     * ahead of a buffer that starts at the note. */
    {
        /* The frame before the note: the voice runs, so its noise register clocks
         * and its filter settles, but there is no pitch yet, so the phases stay
         * where they are. */
        int16_t scratch[PRE_FRAME];
        int32_t p1 = v.vco.phase1, p2 = v.vco.phase2;
        int j;

        ds10_voice_render(&v, scratch, PRE_FRAME);
        v.vco.phase1 = p1;
        v.vco.phase2 = p2;
        /* The effect sees that frame too, so its delay line is a frame further on
         * than a render starting at the note would leave it - which is why the
         * hardware's echo arrives 64 samples earlier. */
        if (fx) {
            for (j = 0; j < PRE_FRAME; j++) {
                int32_t s = scratch[j], fl, fr;

                ds10_fx_step(fx, wet ? s : 0, wet ? s : 0, &fl, &fr);
            }
            ds10_fx_update(fx);
        }
    }

    ds10_voice_note_on(&v, DS10_DRUMBUF_NOTE);

    for (i = 0; i < DS10_DRUMBUF_LEN; i += frame) {
        int n = DS10_DRUMBUF_LEN - i < frame ? DS10_DRUMBUF_LEN - i : frame;
        int j;

        if (fx) ds10_fx_update(fx);
        ds10_voice_render(&v, out + i, n);
        for (j = 0; j < n; j++) {
            int32_t s = out[i + j];

            /* The channel's volume, then the master stage over twice it. The
             * doubling is the original's: one shape over 2*s, not two shaped
             * sides summed - see recon.md section 14. */
            /* Past the point where the recorder asks to stop, nothing tops the
             * smoother up any more, so it decays and the tail fades to silence. */
            if (i + j >= FADE_AT)
                gain = ds10_mul32(gain, GAIN_KEEP) >> 15;
            if (fx) {
                /* The channel meets the effect as both of its inputs, and what
                 * comes back is summed - which is where the doubling of the dry
                 * path comes from, since a dry effect returns its input twice. */
                int32_t fl, fr;

                ds10_fx_step(fx, wet ? s : 0, wet ? s : 0, &fl, &fr);
                s = wet ? fl + fr : (ds10_mul32(s, CH_VOL) >> 15) * 2;
            } else {
                s = (ds10_mul32(s, CH_VOL) >> 15) * 2;
            }
            out[i + j] = (int16_t)(ds10_mul32(shape(clamp_one(s)), gain) >> 15);
        }
    }
    *vp = v;
}
