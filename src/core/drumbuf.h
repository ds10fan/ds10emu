/* A drum patch rendered into the one-second buffer its voice resamples.
 *
 * This is what 0x0206CCC8 does at song load, and again whenever a drum patch is
 * edited. It is not a plain voice render: the sample goes out through the mixer's
 * master stage, so it is doubled, soft-clipped and gain-scaled, and both of its
 * ends are shaped by the recorder - a fade in while the master gain is still
 * ramping, and a fade out once fewer than eight frames of buffer are left.
 *
 * recon.md section 14 has the recipe with an address for every constant, and
 * `make test-drumbuf` holds this to buffers read out of a running DS.
 */
#ifndef DS10_DRUMBUF_H
#define DS10_DRUMBUF_H

#include <stdint.h>

#include "fx.h"
#include "voice.h"

/* 0x8000 samples: 0x02070610 asks for 1000 ms and the rate is 32768 Hz. */
#define DS10_DRUMBUF_LEN 0x8000

/* The note a drum buffer is rendered at, and the one its voice plays it back at,
 * so playback at that note is unity speed. */
#define DS10_DRUMBUF_NOTE 60

/* Render `knobs` - a drum's 31 - into `out`, which needs DS10_DRUMBUF_LEN
 * samples. `frame` is the render block the envelopes are stepped on, 64 in the
 * original.
 *
 * `extra` is that drum's nine bytes at payload +0x2BA8: a route and the eight
 * effect parameters, which the render pushes in as mixer knobs 0..8 before it
 * starts - so a drum's sample has its own effect baked into it. `fx` is a
 * processor for that, and the caller owns it because of its two ring buffers;
 * pass NULL, or `extra` NULL, to render dry. */
void ds10_drumbuf_render(const int8_t *knobs, const int8_t *extra,
                         ds10_fx *fx, int16_t *out, int frame);

/* The same, with the voice supplied. 0x0206CCC8 renders the four drums in a loop
 * that pushes each one's knobs into the *same* machine, so whatever state a drum
 * leaves - a noise register, an oscillator phase - is where the next one starts.
 * `ds10_drumbuf_render` is this with a fresh voice, which is only right for the
 * first drum of a run. */
void ds10_drumbuf_render_with(ds10_voice *v, const int8_t *knobs,
                              const int8_t *extra, ds10_fx *fx, int16_t *out,
                              int frame);

#endif /* DS10_DRUMBUF_H */
