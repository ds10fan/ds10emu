#include "drum.h"

#include "fixed.h"

/* Identical to the synth envelope's aux contour. */
#define DRUM_VOL_MAX 0x00B33333
#define DRUM_ATTACK_RATE 0x70
#define DRUM_RELEASE 0x42

void ds10_drum_render(ds10_drum *d, int16_t *out, int n)
{
    int i;

    /* Envelope, once per frame like everything else outside the sample loop. */
    if (d->env.stage == DS10_DRUM_ATTACK) {
        d->env.vol += (int32_t)(((int64_t)(DRUM_VOL_MAX - d->env.vol) *
                                 DRUM_ATTACK_RATE) >> 7);
        if (d->env.vol > DRUM_VOL_MAX)
            d->env.vol = DRUM_VOL_MAX;
    } else if (d->env.stage == DS10_DRUM_RELEASE) {
        d->env.vol = ds10_mul32(d->env.vol, DRUM_RELEASE) >> 7;
    }

    if (d->sample == 0)
        return;

    for (i = 0; i < n; i++) {
        int32_t idx, a, b, s;

        /* The offset advances BEFORE the read, so the first sample of a frame
         * is already one increment in. */
        d->offset += d->speed;
        if ((d->offset >> 15) > d->length - 2)
            d->offset = ds10_mul32(d->length - 2, 0x8000);

        idx = d->offset >> 15;
        a = d->sample[idx];
        b = d->sample[idx + 1];
        s = a + (ds10_mul32(d->offset & 0x7FFF, b - a) >> 15);
        out[i] = (int16_t)(ds10_mul32(s, d->env.vol >> 9) >> 15);
    }
}
