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

typedef struct {
    ds10_tables t;
    ds10_fx_tables fx;
    void *blob;          /* owns everything the two views point at */
} ds10_table_set;

/* Returns 0 on success. On failure `blob` is NULL and nothing was allocated. */
int ds10_tables_load(ds10_table_set *s, const char *path);
void ds10_tables_free(ds10_table_set *s);

#endif /* DS10_TABLES_H */
