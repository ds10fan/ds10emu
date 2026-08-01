/* Checking a vector's tables against the ones the engine reads.
 *
 * Every vector carries the ROM tables the oracle ran against, and the tests
 * used to hand those straight to our code - so both sides indexed the same
 * bytes by construction. The engine now reads its own compiled-in copy
 * instead, which means the two are only the same if nobody has regenerated
 * one without the other. That is exactly the kind of drift that makes a
 * differential test pass while comparing the wrong things, so it is checked
 * rather than assumed: every vector test calls these before it compares
 * anything, and a mismatch is a failure in its own right.
 */
#ifndef DS10_TEST_VECTORTABLES_H
#define DS10_TEST_VECTORTABLES_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/core/tables_data.h"

/* A test only checks the widths its own vector carries. */
#ifdef __GNUC__
#define DS10_UNUSED __attribute__((unused))
#else
#define DS10_UNUSED
#endif

/* A stale table makes every comparison after it meaningless, so this stops
 * rather than counting: there is nothing useful left to report. */
DS10_UNUSED static void vt_stop(void)
{
    fprintf(stderr, "FAIL: the vector and the engine disagree about a ROM "
                    "table. Regenerate both - `make tables` then the "
                    "vectors - or one of them is from a different build.\n");
    exit(1);
}

DS10_UNUSED static void vt_i32(const char *what, const int32_t *vec, const int32_t *own,
                   int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (vec[i] != own[i]) {
            fprintf(stderr, "FAIL: %s[%d]: the vector has %d, the engine "
                            "reads %d - one of them is stale\n",
                    what, i, vec[i], own[i]);
            vt_stop();
        }
}

DS10_UNUSED static void vt_u32(const char *what, const uint32_t *vec, const uint32_t *own,
                   int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (vec[i] != own[i]) {
            fprintf(stderr, "FAIL: %s[%d]: the vector has %u, the engine "
                            "reads %u - one of them is stale\n",
                    what, i, vec[i], own[i]);
            vt_stop();
        }
}

DS10_UNUSED static void vt_i16(const char *what, const int16_t *vec, const int16_t *own,
                   int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (vec[i] != own[i]) {
            fprintf(stderr, "FAIL: %s[%d]: the vector has %d, the engine "
                            "reads %d - one of them is stale\n",
                    what, i, vec[i], own[i]);
            vt_stop();
        }
}

#endif /* DS10_TEST_VECTORTABLES_H */
