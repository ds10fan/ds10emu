#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/core/event.h"
#include "../src/core/tables.h"
#include "../src/core/voice_state.h"

int main(int argc, char **argv)
{
    ds10_table_set ts; ds10_pitch_tables pt; ds10_vcf_tables ft; ds10_voice v;
    int16_t buf[64]; int8_t knobs[31]; int i, f, note = atoi(argv[1]);
    if (ds10_tables_load(&ts, "out/tables.bin")) return 2;
    ds10_tables_views(&ts.t, &pt, &ft);
    for (i = 0; i < 31; i++) knobs[i] = (int8_t)atoi(argv[2 + i]);
    memset(&v, 0, sizeof v);
    ds10_voice_state_load(&v, ts.voice_init);
    printf("ctor: env.stage=%d retrig=%d level=%d amp.level=%d\n",
           v.env.stage, v.env.retrig, v.env.level, v.amp.level);
    ds10_patch_load(&v, knobs, &ts.t);
    printf("after patch_load: env.stage=%d retrig=%d level=%d\n",
           v.env.stage, v.env.retrig, v.env.level);
    ds10_voice_note_on(&v, note, &ts.t);
    printf("after note_on:    env.stage=%d retrig=%d level=%d\n",
           v.env.stage, v.env.retrig, v.env.level);
    for (f = 0; f < 12; f++) {
        int pk = 0;
        ds10_voice_render(&v, &pt, &ft, buf, 64);
        for (i = 0; i < 64; i++) { int a = buf[i]<0?-buf[i]:buf[i]; if (a>pk) pk=a; }
        printf("  f%-3d peak=%6d env=%d/%-9d amp.level=%-9d vcf.coef=%d\n",
               f, pk, v.env.stage, v.env.level, v.amp.level, v.vcf.coef);
    }
    return 0;
}
