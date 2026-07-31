/* DS-10 effect section - delay, chorus and flanger.
 *
 * Reimplemented from the effect processor at 0x0207251C (ctor), 0x020726D4
 * (parameter setter), 0x02072900 (reapply-all) and the effect half of the mixer
 * render loop at 0x02072E8C.
 *
 * There is only ONE effect algorithm: a stereo fractional-delay line with
 * feedback and a dry/wet mix. Delay, chorus and flanger differ only in how the
 * six knobs map onto its coefficients. In the original that is expressed as
 * three 4-byte strategy objects whose vtables hold the mappings, selected by
 * `*(void**)(fx + type*4)` - which is why reading the setters linearly gets
 * nowhere. Here it is a switch on `type`.
 *
 * The neutral-slot trick from the synth appears again: lfo_l[0] and lfo_r[0]
 * are never written, so a delay (type 0) indexes zero modulation without a
 * branch.
 */
#ifndef DS10_FX_H
#define DS10_FX_H

#include <stdint.h>

#define DS10_FX_BUFLEN 65536 /* samples; 0x20000 bytes each side */
#define DS10_FX_MASK 0xFFFF

enum ds10_fx_type {
    DS10_FX_DELAY = 0,
    DS10_FX_CHORUS = 1,
    DS10_FX_FLANGER = 2,
};

/* Parameter ids as passed to the original's setter. */
enum ds10_fx_param {
    DS10_FX_P_TYPE = 1,
    DS10_FX_P_TIME = 2,
    DS10_FX_P_LR_RATIO = 3,
    DS10_FX_P_LFO_RATE = 4,
    DS10_FX_P_DEPTH = 5,
    DS10_FX_P_SYNC = 6,
    DS10_FX_P_FEEDBACK = 7,
    DS10_FX_P_DRYWET = 8,
};

/* ROM tables. All contiguous in the image:
 *   lfo_phase_off 0x0207EF38    3  per-type LFO phase offset for the right tap
 *   flanger_fb    0x0207EF44  128  s16, indexed 63 + knob (bipolar)
 *   depth         0x0207F044  128  s16
 *   delay_div     0x0207F144   12  tempo-synced note divisions
 *   lfo_rate      0x0207FFC4  128  shared with the synth LFO
 */
typedef struct {
    const int32_t *lfo_phase_off;
    const int16_t *flanger_fb; /* points at 0x0207EF44, i.e. index 0 */
    const int16_t *depth;
    const uint32_t *delay_div;
    const uint32_t *lfo_rate;
} ds10_fx_tables;

typedef struct {
    int16_t type;       /* +0x0C */
    int16_t sync;       /* +0x0E tempo-sync divisor; 0 = free running */
    int16_t k_time;     /* +0x10 raw knobs, kept so a type change can reapply */
    int16_t k_ratio;    /* +0x12 */
    int16_t k_lfo;      /* +0x14 */
    int16_t k_depth;    /* +0x16 */
    int16_t k_fb;       /* +0x18 */
    int16_t k_drywet;   /* +0x1A */
    int16_t fb_enable;  /* +0x1C 1 for delay and flanger, 0 for chorus */

    int32_t lfo_l[3];   /* +0x28 (always 0), +0x2C triangle, +0x30 cubed */
    int32_t lfo_r[3];   /* +0x34 (always 0), +0x38, +0x3C */
    int32_t lfo_phase;  /* +0x40 */
    int32_t lfo_inc;    /* +0x44 */
    int32_t depth;      /* +0x48 */
    int32_t feedback;   /* +0x4C */
    int32_t time;       /* +0x50 delay time, 15.15 */
    int32_t smooth_l;   /* +0x54 one-pole smoothed delay time */
    int32_t smooth_r;   /* +0x58 */

    int32_t read_l, read_l_target, read_l_delta;   /* +0x5C +0x60 +0x64 */
    int32_t read_r, read_r_target, read_r_delta;   /* +0x68 +0x6C +0x70 */
    int32_t fb_l, fb_l_target, fb_l_delta;         /* +0x74 +0x78 +0x7C */
    int32_t fb_r, fb_r_target, fb_r_delta;         /* +0x80 +0x84 +0x88 */

    int32_t dry;        /* +0x8C */
    int32_t wet;        /* +0x90 */
    int32_t widx;       /* +0x94 */
    int32_t ratio_l;    /* +0x98 */
    int32_t ratio_r;    /* +0x9C */
    int32_t tempo;      /* +0xA0 */

    int16_t *buf_l;     /* +0x20  DS10_FX_BUFLEN samples */
    int16_t *buf_r;     /* +0x24 */
} ds10_fx;

/* Apply one parameter. Setting the type reapplies all six others through the
 * new mapping, exactly as the original does. */
void ds10_fx_set(ds10_fx *fx, int param, int value, const ds10_fx_tables *t);

/* Per-frame: advance the LFO and recompute the ramp targets. */
void ds10_fx_update(ds10_fx *fx, const ds10_fx_tables *t);

/* Per-sample. `in_*` is the wet-path input; the caller adds the dry bypass. */
void ds10_fx_step(ds10_fx *fx, int32_t in_l, int32_t in_r,
                  int32_t *out_l, int32_t *out_r);

#endif /* DS10_FX_H */
