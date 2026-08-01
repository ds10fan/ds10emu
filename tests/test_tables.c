/* The compiled-in tables against the file they were generated from.
 *
 * tools/mktables.py writes each table out as its own named array so it can be
 * read, which means the file is no longer the only copy and the two can drift
 * - a regenerated data/tables.bin with a stale tables_data.c would be silent.
 * This compares every entry of every view.
 */
#include <stdio.h>
#include <string.h>

#include "../src/core/tables.h"

static int fails;

static void cmp32(const char *what, const void *a, const void *b, int n)
{
    const int32_t *x = a, *y = b;
    int i;

    if (!x || !y) {
        fprintf(stderr, "FAIL: %s is missing from one of them\n", what);
        fails++;
        return;
    }
    for (i = 0; i < n; i++)
        if (x[i] != y[i]) {
            fprintf(stderr, "FAIL: %s[%d]: file %d, built in %d\n", what, i,
                    x[i], y[i]);
            fails++;
            return;
        }
}

static void cmp16(const char *what, const int16_t *x, const int16_t *y, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (x[i] != y[i]) {
            fprintf(stderr, "FAIL: %s[%d]: file %d, built in %d\n", what, i,
                    x[i], y[i]);
            fails++;
            return;
        }
}

int main(void)
{
    static ds10_table_set f, b;
    int checked = 0;

    if (ds10_tables_load(&f, "data/tables.bin") != 0) {
        fprintf(stderr, "cannot read data/tables.bin\n");
        return 1;
    }
    if (ds10_tables_builtin(&b) != 0) {
        fprintf(stderr, "the built-in tables did not load\n");
        return 1;
    }

#define C32(field, n) do { cmp32(#field, f.field, b.field, (n)); \
                           checked += (n); } while (0)
#define C16(field, n) do { cmp16(#field, f.field, b.field, (n)); \
                           checked += (n); } while (0)
    C32(t.env_rate, 128);
    C32(t.lfo_bpm, 16);
    C32(t.lfo_free, 128);
    C32(t.porta, 128);
    C32(t.peak, 128);
    C32(t.pitch, 130);
    C32(t.depth, 128);
    C16(t.mod_a, 128);
    C16(t.mod_b, 128);
    C32(t.coef, 152);
    C32(fx.lfo_phase_off, 3);
    C16(fx.flanger_fb, 128);
    C16(fx.depth, 128);
    C32(fx.delay_div, 12);
    C32(fx.lfo_rate, 128);
    C32(drum_pitch, 128);
    C32(voice_init, DS10_VOICE_STATE_FIELDS);
    C32(mixer_init, 21);
    C32(fx_init, 40);
    C32(knob_min, 31);
    C32(knob_range, 31);
#undef C32
#undef C16

    if (fails) {
        fprintf(stderr, "FAIL: %d tables differ - regenerate with "
                        "tools/mktables.py\n", fails);
        return 1;
    }
    printf("PASS: %d entries over 21 tables, built in matches the file\n",
           checked);
    return 0;
}
