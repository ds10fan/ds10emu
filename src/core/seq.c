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

/* --- note and gate handling (0x0206E9D8 and 0x0206EABC) ------------------- */

void ds10_seq_step_track(ds10_seq_track *t, int note, int gate, int subticks)
{
    int8_t count = 0;

    if (note < 0) {
        /* No note on this step. A tie that was running has to be released now;
         * anything else has already been released by its own gate. */
        t->gate_left = 0;
        if (t->gate_mode == (int8_t)DS10_GATE_TIE) {
            t->pending = DS10_NOTE_OFF;
            t->sounding = 0;
        } else {
            t->pending = DS10_NOTE_NONE;
        }
        t->note = (int8_t)note;
        return;
    }

    if (gate == DS10_GATE_RETRIG) {
        t->pending = DS10_NOTE_RETRIG;
    } else if (gate == DS10_GATE_TIE) {
        t->pending = DS10_NOTE_ON;
    } else {
        if (gate == DS10_GATE_OVER)
            count = (int8_t)(subticks + 1);
        else if (gate == DS10_GATE_FULL)
            count = (int8_t)(subticks - 1);
        else
            count = (int8_t)((subticks * gate) / 4);

        if (t->gate_left == 0) {
            /* Nothing sounding means a plain note-on; something still sounding
             * needs a retrigger so the envelope restarts. */
            t->pending = t->sounding == 0 ? DS10_NOTE_ON : DS10_NOTE_RETRIG;
        } else if (t->note == (int8_t)note) {
            /* Same note still inside its gate - leave it alone. This is what
             * makes repeated identical steps sustain rather than stutter. */
            t->pending = DS10_NOTE_NONE;
        } else {
            t->pending = DS10_NOTE_ON;
        }
    }
    t->sounding = 1;
    t->gate_mode = (int8_t)gate;
    t->gate_left = count;
    t->note = (int8_t)note;
}

int ds10_seq_gate(ds10_seq_track *t)
{
    int ev = t->pending;

    /* Retrigger and tie modes never auto-release. */
    if (t->gate_mode == (int8_t)DS10_GATE_RETRIG ||
        t->gate_mode == (int8_t)DS10_GATE_TIE)
        return ev;

    if (t->sounding == 1) {
        if (t->gate_left == 0) {
            t->sounding = 0;
            ev = DS10_NOTE_OFF;
        } else {
            t->gate_left--;
        }
    }
    return ev;
}
