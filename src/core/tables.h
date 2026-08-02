/* Reader for data/tables.bin, the ROM's own copy of the engine's tables.
 *
 * The engine does not use this. It indexes the named arrays in tables_data.h,
 * which are generated from the same file and compiled in - one fixed set of
 * constants, so nothing has to be handed them.
 *
 * What this is for is checking that those arrays are still the ROM's:
 * `make tables` re-extracts the binary and reports any difference against the
 * committed copy, and tests/test_tables.c parses that copy with this and
 * compares every entry against what is compiled in. A regenerated binary with
 * a stale tables_data.c would otherwise be silent.
 */
#ifndef DS10_TABLES_H
#define DS10_TABLES_H

#include "fx.h"
#include "patch.h"
#include <stddef.h>

#include "voice_state.h"

typedef struct {
    ds10_tables t;
    ds10_fx_tables fx;
    /* Drum playback rates, indexed by note - root + 64. Entry 64 is unity. */
    const uint32_t *drum_pitch;
    /* The sine/cosine table every rot/scale sprite's matrix comes out of. */
    const int16_t *sincos;
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

/* The same from an image already in memory. */
int ds10_tables_parse(ds10_table_set *s, const unsigned char *data,
                      size_t len);

void ds10_tables_free(ds10_table_set *s);

#endif /* DS10_TABLES_H */
