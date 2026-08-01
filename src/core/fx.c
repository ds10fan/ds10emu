#include "fx.h"
#include "tables_data.h"

#include "fixed.h"

#define FX_ONE 0x7FFF
#define FX_MIN_DELAY 0x8000 /* one sample, in 15.15 */

/* Shared by all three types. Same quadratic curve as the oscillator balance
 * law, but without its 0x4444 gain: dry and wet are each
 * 0x7FFC - (x*x >> 15) for opposite ends of the sweep, so knob 0 is fully dry
 * and knob 127 fully wet. */
#define DRYWET_STEP 0x102
#define DRYWET_ONE 0x7FFC

static void map_drywet(ds10_fx *fx, int v)
{
    int32_t a = v * DRYWET_STEP;
    int32_t b = (DRYWET_ONE + 2) - a;

    fx->dry = DRYWET_ONE - (ds10_mul32(a, a) >> 15);
    fx->wet = DRYWET_ONE - (ds10_mul32(b, b) >> 15);
}

/* Only the delay derives its time from the knob. Chorus and flanger use fixed
 * delays - 228 and 4.6 samples - and get their movement purely from LFO
 * modulation of the read offset. That is the whole difference between them. */
#define CHORUS_TIME 0x728C49  /* 228.5 samples */
#define FLANGER_TIME 0x25050  /* 4.63 samples */

/* Free-running delay time is quadratic in the knob, which gives fine control
 * at short times: knob 1 is 2 samples, knob 127 is 32727 (just under a second
 * at 32768 Hz, and just inside the 65536-sample buffer). */
#define DELAY_TIME_SCALE 0x103BA

static void map_time(ds10_fx *fx, int v)
{
    int32_t div;

    if (fx->type == DS10_FX_CHORUS) {
        fx->time = CHORUS_TIME;
        return;
    }
    if (fx->type == DS10_FX_FLANGER) {
        fx->time = FLANGER_TIME;
        return;
    }

    /* The divisor is the TEMPO, not the sync flag: the dispatcher passes
     * fx->tempo when sync is on and literally 0 when it is off. So with sync
     * enabled but no tempo event yet received, the free-running path is still
     * what runs. */
    div = (fx->sync != 0) ? fx->tempo : 0;
    if (div == 0) {
        fx->time = (v != 0) ? ds10_mul32(ds10_mul32(v, v), DELAY_TIME_SCALE)
                            : FX_MIN_DELAY;
        return;
    }

    /* Tempo-synced: pick the coarsest note division that still fits the tempo,
     * cap it by the knob, then divide. The original uses the hardware divider
     * and reassembles a 15.15 result from quotient and remainder. */
    {
        int idx = 11;
        int i;
        int32_t q, r;

        for (i = 11; i >= 0; i--) {
            if ((int32_t)(ds10_tbl_fx_delay_div[i] >> 15) < ds10_shl(div, 16)) {
                idx = i;
                break;
            }
        }
        if ((v >> 3) < 0)
            idx = 0;
        else if ((v >> 3) <= idx)
            idx = v >> 3;

        q = (int32_t)ds10_tbl_fx_delay_div[idx] / div;
        r = (int32_t)ds10_tbl_fx_delay_div[idx] % div;
        if (q > 0xFFFF)
            q = 0xFFFF;
        fx->time = (r >> 16) + ds10_shl(q, 15);
    }
}

/* Positive ratio shortens the right tap, negative shortens the left. */
#define RATIO_STEP 0x1C7

static void map_lr_ratio(ds10_fx *fx, int v)
{
    switch (fx->type) {
    case DS10_FX_DELAY:
        if (v > 0) {
            fx->ratio_l = FX_ONE;
            fx->ratio_r = FX_ONE - v * RATIO_STEP;
        } else {
            fx->ratio_l = v * RATIO_STEP + FX_ONE;
            fx->ratio_r = FX_ONE;
        }
        break;
    case DS10_FX_CHORUS:
        /* Fixed stereo spread. */
        fx->ratio_l = 0x5B6D;
        fx->ratio_r = FX_ONE;
        break;
    default:
        fx->ratio_l = FX_ONE;
        fx->ratio_r = FX_ONE;
        break;
    }
}

#define DEPTH_SCALE 0x0105D2F1

static void map_depth(ds10_fx *fx, int v)
{
    if (fx->type == DS10_FX_DELAY) {
        fx->depth = 0; /* a delay does not modulate its time */
        return;
    }
    fx->depth = (int32_t)(((int64_t)ds10_tbl_fx_depth[v] * (int64_t)DEPTH_SCALE) >> 15);
}

/* Delay feedback saturates at the top of the knob; below that it is linear in
 * (knob + 63), i.e. the bipolar knob is folded to unipolar. */
#define FB_STEP 0x105

static void map_feedback(ds10_fx *fx, int v)
{
    switch (fx->type) {
    case DS10_FX_DELAY:
        fx->feedback = (v >= 0x3F) ? FX_ONE : (v + 0x3F) * FB_STEP;
        break;
    case DS10_FX_CHORUS:
        fx->feedback = 0;
        break;
    default:
        /* Flanger feedback is inverted, and the table is indexed from its
         * centre so the knob can be bipolar. */
        fx->feedback = -(int32_t)ds10_tbl_fx_flanger_fb[63 + v];
        break;
    }
}

static void reapply_all(ds10_fx *fx)
{
    map_time(fx, fx->k_time);
    map_lr_ratio(fx, fx->k_ratio);
    fx->lfo_inc = (int32_t)ds10_tbl_fx_lfo_rate[fx->k_lfo];
    map_depth(fx, fx->k_depth);
    map_feedback(fx, fx->k_fb);
    map_drywet(fx, fx->k_drywet);
}

void ds10_fx_set(ds10_fx *fx, int param, int value)
{
    switch (param) {
    case DS10_FX_P_TYPE:
        if (value == fx->type || value < 0 || value > 2)
            return;
        fx->type = (int16_t)value;
        /* Chorus is the only type with feedback disabled at the DSP level, as
         * well as by its own mapping returning zero. */
        fx->fb_enable = (value == DS10_FX_CHORUS) ? 0 : 1;
        reapply_all(fx);
        break;
    case DS10_FX_P_TIME:
        fx->k_time = (int16_t)value;
        map_time(fx, value);
        break;
    case DS10_FX_P_LR_RATIO:
        fx->k_ratio = (int16_t)value;
        map_lr_ratio(fx, value);
        break;
    case DS10_FX_P_LFO_RATE:
        fx->k_lfo = (int16_t)value;
        fx->lfo_inc = (int32_t)ds10_tbl_fx_lfo_rate[value];
        break;
    case DS10_FX_P_DEPTH:
        fx->k_depth = (int16_t)value;
        map_depth(fx, value);
        break;
    case DS10_FX_P_SYNC:
        /* Only meaningful for a delay, and it re-derives the time. */
        if (fx->type != DS10_FX_DELAY || fx->sync == value)
            return;
        fx->sync = (int16_t)value;
        map_time(fx, fx->k_time);
        break;
    case DS10_FX_P_FEEDBACK:
        fx->k_fb = (int16_t)value;
        map_feedback(fx, value);
        break;
    case DS10_FX_P_DRYWET:
        fx->k_drywet = (int16_t)value;
        map_drywet(fx, value);
        break;
    default:
        break;
    }
}

/* One-pole smoother on the delay time. 30439 + 2329 == 32768, so the
 * coefficient is 2329/32768 = 0.071 per frame - slow enough that sweeping the
 * time knob glides rather than jumps. */
#define SMOOTH_KEEP 30439
#define SMOOTH_ADD (0x8000 - SMOOTH_KEEP)

static int32_t smooth(int32_t cur, int32_t target)
{
    return (int32_t)((((int64_t)SMOOTH_KEEP * cur) +
                      ((int64_t)SMOOTH_ADD * target)) >> 15);
}

/* Triangle from the phase accumulator, plus its cube. The cube is what the
 * flanger uses, so its sweep is concentrated near the extremes. */
static void lfo_tap(int32_t phase, int32_t *tri, int32_t *cubed)
{
    int32_t a = phase >> 16;
    int32_t c;

    if (a < 1)
        a = -a;
    c = ds10_mul32(ds10_mul32(a, a) >> 15, a) >> 15;
    *tri = a;
    *cubed = (c == 0) ? 1 : c;
}

void ds10_fx_update(ds10_fx *fx)
{
    int32_t base, mod, target;

    fx->lfo_phase -= fx->lfo_inc;
    lfo_tap(fx->lfo_phase, &fx->lfo_l[1], &fx->lfo_l[2]);
    /* The right tap is offset in phase by a per-type constant: 0 for delay,
     * half a cycle for chorus, about a ninth for flanger. */
    lfo_tap(fx->lfo_phase + ds10_tbl_fx_lfo_phase[fx->type],
            &fx->lfo_r[1], &fx->lfo_r[2]);

    base = ds10_mul15(fx->time, fx->ratio_l);
    if (base < FX_MIN_DELAY + 1)
        base = FX_MIN_DELAY;
    mod = ds10_mul15(fx->depth, fx->lfo_l[fx->type]);
    fx->smooth_l = smooth(fx->smooth_l, base + mod);
    fx->read_l_delta = (fx->smooth_l - fx->read_l_target) >> 6;
    fx->read_l = fx->read_l_target;
    fx->read_l_target = fx->smooth_l;

    base = ds10_mul15(fx->time, fx->ratio_r);
    if (base < FX_MIN_DELAY + 1)
        base = FX_MIN_DELAY;
    mod = ds10_mul15(fx->depth, fx->lfo_r[fx->type]);
    fx->smooth_r = smooth(fx->smooth_r, base + mod);
    fx->read_r_delta = (fx->smooth_r - fx->read_r_target) >> 6;
    fx->read_r = fx->read_r_target;
    fx->read_r_target = fx->smooth_r;

    /* Both channels share one feedback value. */
    target = ds10_mul32(fx->feedback, fx->fb_enable);
    fx->fb_l_delta = (target - fx->fb_l_target) >> 6;
    fx->fb_r_delta = fx->fb_l_delta;
    fx->fb_l = fx->fb_l_target;
    fx->fb_r = fx->fb_l_target;
    fx->fb_l_target = target;
    fx->fb_r_target = target;
}

static int32_t clamp16(int32_t v)
{
    if (v < -FX_ONE)
        return -FX_ONE;
    if (v > FX_ONE)
        return FX_ONE;
    return v;
}

/* Fractional read: the integer part of the 15.15 delay indexes the ring buffer
 * and the low 15 bits interpolate toward the previous sample. */
static int32_t tap(const int16_t *buf, int32_t widx, int32_t delay)
{
    int32_t r = (widx - (delay >> 15)) & DS10_FX_MASK;
    int32_t a = buf[r];
    int32_t b = buf[(r - 1) & DS10_FX_MASK];

    return a + (ds10_mul32(delay & 0x7FFF, b - a) >> 15);
}

void ds10_fx_step(ds10_fx *fx, int32_t in_l, int32_t in_r,
                  int32_t *out_l, int32_t *out_r)
{
    int32_t w = fx->widx & DS10_FX_MASK;
    int32_t dl = tap(fx->buf_l, w, fx->read_l);
    int32_t dr = tap(fx->buf_r, w, fx->read_r);

    fx->buf_l[w] = (int16_t)clamp16(in_l + (ds10_mul32(dl, fx->fb_l) >> 15));
    fx->buf_r[w] = (int16_t)clamp16(in_r + (ds10_mul32(dr, fx->fb_r) >> 15));
    fx->widx = w + 1;

    *out_l = (ds10_mul32(in_l, fx->dry) + ds10_mul32(dl, fx->wet)) >> 15;
    *out_r = (ds10_mul32(in_r, fx->dry) + ds10_mul32(dr, fx->wet)) >> 15;

    fx->fb_l += fx->fb_l_delta;
    fx->fb_r += fx->fb_r_delta;
    fx->read_l += fx->read_l_delta;
    fx->read_r += fx->read_r_delta;
}
