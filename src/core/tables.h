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
    /* The state the original's synth constructor leaves a voice in. */
    const int32_t *voice_init;
    void *blob;          /* owns everything the views point at */
} ds10_table_set;

/* Returns 0 on success. On failure `blob` is NULL and nothing was allocated. */
int ds10_tables_load(ds10_table_set *s, const char *path);
void ds10_tables_free(ds10_table_set *s);

#endif /* DS10_TABLES_H */
