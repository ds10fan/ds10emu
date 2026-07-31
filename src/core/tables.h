/* Loader for the ROM tables the engine indexes.
 *
 * These are data, not code: ten curve tables the synth reads and five the effect
 * reads. `tools/oracle.py tables` writes them to out/tables.bin, which is
 * ROM-derived and so never committed - regenerate it with `make tables`.
 */
#ifndef DS10_TABLES_H
#define DS10_TABLES_H

#include "fx.h"
#include "patch.h"
#include "voice_state.h"

typedef struct {
    ds10_tables t;
    ds10_fx_tables fx;
    /* Drum playback rates, indexed by note - root + 64. Entry 64 is unity. */
    const uint32_t *drum_pitch;
    /* The state the original's constructors leave a voice, a mixer and an
     * effect in. None of them is zero, and starting from a memset does not
     * reproduce the original. */
    const int32_t *voice_init;
    const int32_t *mixer_init;
    const int32_t *fx_init;
    /* Per-knob minimum and range, from the synth's knob descriptors. An
     * automation lane uses them as its offset and scale, so a recorded 0..127
     * lane sweeps exactly that knob's range. */
    const int32_t *knob_min;
    const int32_t *knob_range;
    void *blob;          /* owns everything the views point at */
} ds10_table_set;

/* Returns 0 on success. On failure `blob` is NULL and nothing was allocated. */
int ds10_tables_load(ds10_table_set *s, const char *path);
void ds10_tables_free(ds10_table_set *s);

#endif /* DS10_TABLES_H */
