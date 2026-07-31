/* DS-10 envelope generator.
 *
 * Reimplemented from the ENV block of the render handler's per-frame preamble
 * (0x0207481C..0x02074900). Verified bit-exact; see tests/test_env.c.
 *
 * IMPORTANT: the envelope advances once per 64-sample frame, not per sample.
 * That is a property of the original, not a simplification, and it is audible -
 * envelope resolution is 512 Hz.
 *
 * The generator exposes two outputs, both readable by the modulation matrix:
 *   level (+0x08) - the ADSR value proper, 0..0x00FFFFFF
 *   aux   (+0x0C) - a second contour that rises fast during attack and decays
 *                   geometrically during release
 */
#ifndef DS10_ENV_H
#define DS10_ENV_H

#include <stdint.h>

enum ds10_env_stage {
    DS10_ENV_ATTACK = 0,
    DS10_ENV_DECAY = 1,
    DS10_ENV_SUSTAIN = 2,
    DS10_ENV_RELEASE = 3,
};

/* The four knob-derived rates, in a block the ENV object points at (+0x00). */
typedef struct {
    int32_t attack;    /* +0x00 */
    int32_t decay;     /* +0x04 */
    int32_t sustain;   /* +0x08 - a level, not a rate */
    int32_t release;   /* +0x0C */
} ds10_env_params;

typedef struct {
    uint8_t stage;     /* +0x04 low byte, enum ds10_env_stage */
    int32_t level;     /* +0x08 */
    int32_t aux;       /* +0x0C */
    /* +0x05: set by note-off, consumed by note-on. Non-zero means the next
     * note-on restarts the attack; zero means it glides without retriggering,
     * which is how legato works - a note-on with no preceding note-off. */
    uint8_t retrig;
} ds10_env;

/* Advance one frame. Note-on resets stage to attack; note-off sets it to
 * release (the original also pokes a flag byte at +0x05 that the render path
 * does not read). */
void ds10_env_step(ds10_env *e, const ds10_env_params *p);

#endif /* DS10_ENV_H */
