#include "tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Must match TABLES in tools/oracle.py, in order. */
enum {
    T_ENV_RATE, T_LFO_BPM, T_LFO_FREE, T_PORTA, T_PEAK, T_PITCH, T_DEPTH,
    T_MOD_A, T_MOD_B, T_COEF,
    T_FX_PHASE, T_FX_FLANGER_FB, T_FX_DEPTH, T_FX_DELAY_DIV, T_FX_LFO_RATE,
    T_COUNT
};

static const int WIDE[T_COUNT] = {
    4, 4, 4, 4, 4, 4, 4, 2, 2, 4, 4, 2, 2, 4, 4
};

int ds10_tables_load(ds10_table_set *s, const char *path)
{
    FILE *f = fopen(path, "rb");
    char magic[4];
    uint32_t n, i;
    long size;
    unsigned char *blob;
    void *ptr[T_COUNT];
    size_t off = 8;

    memset(s, 0, sizeof *s);
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 8 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    blob = malloc((size_t)size);
    if (!blob) {
        fclose(f);
        return -1;
    }
    if (fread(blob, 1, (size_t)size, f) != (size_t)size) {
        free(blob);
        fclose(f);
        return -1;
    }
    fclose(f);

    memcpy(magic, blob, 4);
    memcpy(&n, blob + 4, 4);
    if (memcmp(magic, "TBL1", 4) != 0 || n != T_COUNT) {
        free(blob);
        return -1;
    }
    /* The file is already little-endian words of the right width, so the views
     * can point straight into it. */
    for (i = 0; i < T_COUNT; i++) {
        uint32_t cnt;

        if (off + 4 > (size_t)size) {
            free(blob);
            return -1;
        }
        memcpy(&cnt, blob + off, 4);
        off += 4;
        if (off + (size_t)cnt * WIDE[i] > (size_t)size) {
            free(blob);
            return -1;
        }
        ptr[i] = blob + off;
        off += (size_t)cnt * WIDE[i];
    }

    s->blob = blob;
    s->t.env_rate = ptr[T_ENV_RATE];
    s->t.lfo_bpm = ptr[T_LFO_BPM];
    s->t.lfo_free = ptr[T_LFO_FREE];
    s->t.porta = ptr[T_PORTA];
    s->t.peak = ptr[T_PEAK];
    s->t.pitch = ptr[T_PITCH];
    s->t.depth = ptr[T_DEPTH];
    s->t.mod_a = ptr[T_MOD_A];
    s->t.mod_b = ptr[T_MOD_B];
    s->t.coef = ptr[T_COEF];
    s->fx.lfo_phase_off = ptr[T_FX_PHASE];
    s->fx.flanger_fb = ptr[T_FX_FLANGER_FB];
    s->fx.depth = ptr[T_FX_DEPTH];
    s->fx.delay_div = ptr[T_FX_DELAY_DIV];
    s->fx.lfo_rate = ptr[T_FX_LFO_RATE];
    return 0;
}

void ds10_tables_free(ds10_table_set *s)
{
    free(s->blob);
    memset(s, 0, sizeof *s);
}
