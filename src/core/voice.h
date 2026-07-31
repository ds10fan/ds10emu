/* DS-10 synth voice - one complete monophonic voice.
 *
 * Wires the eight decompiled stages in the order the original render handler
 * runs them (0x02074720). Verified bit-exact end to end against the original:
 * given the same state and the same note, `ds10_voice_render` produces the
 * identical 64 output samples. See tests/test_voice.c.
 *
 * Frame structure, which is the thing to understand about this engine:
 * everything except the oscillator, filter and amp runs ONCE per 64-sample
 * frame. Modulation resolution is therefore 512 Hz, not 32768 Hz, and the
 * coefficients the per-sample stages consume are linearly ramped across the
 * frame rather than stepped.
 */
#ifndef DS10_VOICE_H
#define DS10_VOICE_H

#include <stdint.h>

#include "amp.h"
#include "env.h"
#include "lfo.h"
#include "pitch.h"
#include "vcf.h"
#include "vco.h"

#define DS10_FRAME 64
#define DS10_RATE 32768

typedef struct {
    ds10_lfo lfo;
    ds10_env env;
    ds10_env_params env_params;
    ds10_notectrl nc;
    ds10_pitchctrl pc;
    ds10_vco vco;
    ds10_vcf vcf;
    ds10_amp amp;
} ds10_voice;

/* Render `n` samples (normally DS10_FRAME). The per-frame preamble runs once,
 * then the sample loop. */
void ds10_voice_render(ds10_voice *v, const ds10_pitch_tables *pt,
                       const ds10_vcf_tables *ft, int16_t *out, int n);

#endif /* DS10_VOICE_H */
