#include "seq.h"

void ds10_seq_set_tempo(ds10_seq *s, int tempo)
{
    s->tempo = (uint8_t)tempo;
    s->period = DS10_SEQ_TEMPO_NUM / tempo;
}

void ds10_seq_set_swing(ds10_seq *s, int swing)
{
    /* The original does this division by the usual magic-multiply, which is
     * exact for the whole 0..100 range, so a plain divide matches. */
    int even = (swing * DS10_SEQ_SUBTICKS + 50) / 100;

    s->swing = (uint8_t)swing;
    s->sub_even = (uint8_t)even;
    s->sub_odd = (uint8_t)(DS10_SEQ_SUBTICKS - even);
}

void ds10_seq_tick(ds10_seq *s)
{
    if (s->playing != 1)
        return;

    s->accum += s->incr;
    if (s->accum <= s->period)
        return;

    do {
        if (s->starting == 1) {
            /* Everything starts at -1 so the first sub-tick lands on step 0. */
            s->starting = 0;
            s->step = -1;
            s->step_count = -1;
            s->bar = -1;
            s->tick = -1;
            s->subtick = 0;
        }
        s->tick++;

        if (s->subtick == 0) {
            s->step++;
            s->step_count++;
            if ((int8_t)s->steps <= s->step)
                s->step = 0;
            if (s->step == 0)
                s->bar++;
            /* Swing selects the count by step parity - but only when a pattern
             * is loaded, so with none the previous value persists. */
            if (s->pattern != 0) {
                s->sub_cur = (s->step & 1) ? s->sub_odd : s->sub_even;
                s->auto_sub = 0;
            }
            s->subtick = s->sub_cur;
        }

        s->accum -= s->period;
        s->subtick--;
    } while (s->accum > s->period);
}
