#include "env.h"

#include "fixed.h"

/* Ceiling for `level`. The attack stage reaching it advances to decay. */
#define ENV_MAX 0x00FFFFFF

/* The attack ramp aims at a target ABOVE the ceiling (0x014CCCCD vs
 * 0x00FFFFFF), which is what makes the attack curve steepen into the clamp
 * rather than settle asymptotically. */
#define ENV_ATTACK_TARGET 0x014CCCCD

/* Independent ceiling and rate for the `aux` contour. */
#define ENV_AUX_MAX 0x00B33333
#define ENV_AUX_RATE 0x70
#define ENV_AUX_RELEASE 0x42 /* aux *= 0x42/0x80 each frame in release */

void ds10_env_step(ds10_env *e, const ds10_env_params *p)
{
    switch (e->stage) {
    case DS10_ENV_ATTACK:
        /* aux rises toward its own ceiling first, then level. */
        e->aux += (int32_t)(((int64_t)(ENV_AUX_MAX - e->aux) * ENV_AUX_RATE) >> 7);
        if (e->aux > ENV_AUX_MAX)
            e->aux = ENV_AUX_MAX;

        e->level += ds10_mul24(ENV_ATTACK_TARGET - e->level, p->attack);
        if (e->level > ENV_MAX) {
            e->level = ENV_MAX;
            e->stage = DS10_ENV_DECAY;
        }
        break;

    case DS10_ENV_DECAY:
        e->level += ds10_mul24(p->decay, p->sustain - e->level);
        if (e->level < p->sustain) {
            e->level = p->sustain;
            e->stage = DS10_ENV_SUSTAIN;
        }
        break;

    case DS10_ENV_SUSTAIN:
        e->level = p->sustain;
        break;

    case DS10_ENV_RELEASE:
        /* 32-bit multiply here, unlike the 64-bit products elsewhere. */
        e->aux = ds10_mul32(e->aux, ENV_AUX_RELEASE) >> 7;
        e->level = ds10_mul24(e->level, ENV_MAX - p->release);
        break;

    default:
        break;
    }
}
