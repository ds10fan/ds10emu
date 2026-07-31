/* DS-10 drum voice - resampled one-shot playback.
 *
 * Reimplemented from 0x02072194 (ctor) and 0x020722B8 (render). A drum part is
 * NOT a live synth voice: the engine renders a drum patch once into a
 * 0x8000-sample buffer and this plays it back at a variable rate. Trigger is
 * `speed = 0x8000 * 2^((note-60)/12)`, so pitch is playback rate.
 *
 * The amplitude contour is literally the synth envelope's `aux` output reused -
 * same 0x00B33333 ceiling, same 0x70 attack rate, same 0x42/0x80 release decay.
 *
 * Playback is one-shot: the offset clamps at length-2 rather than wrapping.
 */
#ifndef DS10_DRUM_H
#define DS10_DRUM_H

#include <stdint.h>

enum ds10_drum_stage {
    DS10_DRUM_ATTACK = 0,
    DS10_DRUM_RELEASE = 1,
    /* Any other value holds the current level. */
};

typedef struct {
    int32_t vol;            /* volblock +0x14 */
    uint8_t stage;          /* volblock +0x10 */
} ds10_drum_env;

typedef struct {
    ds10_drum_env env;      /* the 24-byte block at +0x30 */
    int32_t speed;          /* +0x34 phase increment, 17.15 */
    int32_t offset;         /* +0x38 phase */
    int32_t length;         /* +0x40 sample count */
    const int16_t *sample;  /* +0x44; NULL renders nothing */
} ds10_drum;

/* Render `n` samples. The envelope advances once, then the sample loop runs. */
void ds10_drum_render(ds10_drum *d, int16_t *out, int n);

#endif /* DS10_DRUM_H */
