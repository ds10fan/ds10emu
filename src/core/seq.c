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

/* --- note event emission and routing (0x0206E8C8, 0x0207025C) ------------- */

/* Note-off goes to the track's own machine only; note-on additionally goes to
 * the second bus, which is how the UI learns what is playing. */
#define TARGET_OFF 0x20
#define TARGET_ON 0xA0
#define PAYLOAD_HI 0x7F00

/* Route one logical event to every destination its target byte selects. */
static int route(int nmach, int target, int opcode, uint16_t payload,
                 ds10_seq_posted *out)
{
    int channel = target & 0x0F;
    int n = 0;

    if (target & 0x20) {
        if (target & 0x10) {
            /* Broadcast: every machine, each stamped with its own index. */
            int i;

            for (i = 0; i < nmach; i++) {
                out[n].dest = (uint8_t)i;
                out[n].opcode = (uint8_t)opcode;
                out[n].channel = (uint8_t)i;
                out[n++].payload = payload;
            }
        } else {
            out[n].dest = (uint8_t)channel;
            out[n].opcode = (uint8_t)opcode;
            out[n].channel = (uint8_t)channel;
            out[n++].payload = payload;
        }
    }
    if (target & 0x40) {
        out[n].dest = (uint8_t)DS10_BUS1(nmach);
        out[n].opcode = (uint8_t)opcode;
        out[n].channel = (uint8_t)channel;
        out[n++].payload = payload;
    }
    if (target & 0x80) {
        out[n].dest = (uint8_t)DS10_BUS2(nmach);
        out[n].opcode = (uint8_t)opcode;
        out[n].channel = (uint8_t)channel;
        out[n++].payload = payload;
    }
    return n;
}

int ds10_seq_emit(ds10_seq *s, ds10_seq_track *tracks, ds10_seq_posted *out)
{
    int tr, n = 0;

    for (tr = 0; tr < s->ntracks; tr++) {
        int ev = ds10_seq_gate(&tracks[tr]);
        uint16_t payload = (uint16_t)((tracks[tr].note & 0xFF) | PAYLOAD_HI);

        if (ev == DS10_NOTE_OFF) {
            n += route((int)s->ntracks, TARGET_OFF | tr, DS10_NOTE_OFF,
                       payload, out + n);
        } else if (ev == DS10_NOTE_ON) {
            n += route((int)s->ntracks, TARGET_ON | tr, DS10_NOTE_ON,
                       payload, out + n);
        } else if (ev == DS10_NOTE_RETRIG) {
            n += route((int)s->ntracks, TARGET_ON | tr, DS10_NOTE_OFF,
                       payload, out + n);
            n += route((int)s->ntracks, TARGET_ON | tr, DS10_NOTE_ON,
                       payload, out + n);
        }
        tracks[tr].pending = 0;
    }
    return n;
}

/* --- automation lanes (0x0206E1EC) --------------------------------------- */

/* offset + raw*scale/128, truncated to a signed byte. The original spells the
 * divide as an explicit round-toward-zero correction, which is what C's / does
 * for negatives too. */
int32_t ds10_lane_map(const ds10_lane *l, int raw, const int8_t *defaults)
{
    int32_t p;

    if (raw == -1)
        return defaults[l->knob];

    p = raw * (int32_t)l->scale;
    return (int8_t)(l->offset + p / 128);
}

void ds10_lane_hold(ds10_lane *l, int raw, const int8_t *defaults)
{
    l->cur = (int16_t)(ds10_lane_map(l, raw, defaults) << 8);
}
