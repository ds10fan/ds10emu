/* DS-10 patch loading - the 31 knobs to internal voice state.
 *
 * Reimplemented from the knob dispatch at 0x02075648 and the ~13 small setters
 * it calls. Verified bit-exact against the original; see tests/test_patch.c.
 *
 * This is what makes ds10_voice usable standalone: without it a voice can only
 * be set up by the emulation oracle.
 *
 * Patch bytes 0x20..0x3E of a ds10synthNN.dat file are knobs 0..30 in order.
 * Bipolar knobs are signed; see recon.md section 4 for the table.
 */
#ifndef DS10_PATCH_H
#define DS10_PATCH_H

#include <stdint.h>

#include "voice.h"

#define DS10_NKNOBS 31

/* The contiguous table bank in ROM at 0x0207FD80..0x02080A20.
 *
 * The engine does not use this struct: it indexes the named arrays in
 * tables_data.h, which are one fixed set of constants. What this describes is
 * a set parsed out of data/tables.bin at run time, which only tables.c
 * produces and only tests/test_tables.c consumes - the check that the
 * compiled-in copy is still the ROM's.
 *
 * Entry counts are as listed; `coef` is read at idx and idx+1 so it needs one
 * spare element.
 *
 *   env_rate  0x0207FD80  128  shared by attack, decay and release
 *   lfo_bpm   0x0207FF80   16  BPM-synced LFO increments, indexed by knob>>3
 *   lfo_free  0x0207FFC4  128  free-running LFO increments
 *   porta     0x020801C4  128  portamento rates (descending: 0 is fastest)
 *   mod_a     0x020803C4  128  s16 modulation depth curve
 *   mod_b     0x020804C4  128  s16 modulation depth curve
 *   peak      0x020805C4  128  filter resonance
 *   coef      0x020807C4  151  filter cutoff coefficient
 *   pitch     0x0207E950  ~120 semitone pitch table
 *   depth     0x0207EB38  128  bipolar modulation depth
 */
typedef struct {
    const uint32_t *env_rate;
    const uint32_t *lfo_bpm;
    const uint32_t *lfo_free;
    const uint32_t *porta;
    const uint32_t *peak;
    const int32_t *pitch;
    const int32_t *depth;
    const int16_t *mod_a;
    const int16_t *mod_b;
    const int32_t *coef;
} ds10_tables;

/* Apply a single knob, exactly as the original's dispatch does. */
void ds10_patch_set_knob(ds10_voice *v, int knob, int value);

/* Apply all 31 knobs in order. */
void ds10_patch_load(ds10_voice *v, const int8_t *knobs);

#endif /* DS10_PATCH_H */
