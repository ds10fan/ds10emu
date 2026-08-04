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
        /* No note on this step. The 0xFF hold mode is the only one that has
         * to be released here; everything else has already been released by its
         * own gate, and gate 6 is meant to play out. */
        t->gate_left = 0;
        if (t->gate_mode == (int8_t)DS10_GATE_HOLD) {
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
    } else if (gate == DS10_GATE_HOLD) {
        t->pending = DS10_NOTE_ON;
    } else {
        if (gate == DS10_GATE_TIE)
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

    /* Gate 6 and the 0xFF hold never auto-release. */
    if (t->gate_mode == (int8_t)DS10_GATE_RETRIG ||
        t->gate_mode == (int8_t)DS10_GATE_HOLD)
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
        /* ADD, not or: the note is sign-extended first, so a negative note
         * borrows from the high byte. 0x7F00 + (-22) is 0x7EEA. */
        uint16_t payload = (uint16_t)(PAYLOAD_HI + tracks[tr].note);

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
 * for negatives too.
 *
 * A raw of -1 is what makes a step with no automation restore the knob rather
 * than leave it: `defaults` is that track's 31-byte patch array, the same one the
 * pattern change pushed into the machine, so the lane keeps writing and what it
 * writes is the un-automated value. The processor pushes on every sub-tick, so
 * there is no gap to detect - see automation() below.
 *
 * `l->knob` is not bounded against `defaults`, which is 0x0206E1EC exactly: it
 * indexes with the knob byte read unsigned, while the lane-off test at the push
 * site reads that same byte signed and only skips on -1. A target out of range but
 * not 0xFF therefore reads past the array here. One lane in data/saves does it,
 * with target 31, and the read is harmless because the value is then pushed to a
 * knob ds10_patch_set_knob does not dispatch. */
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

void ds10_lane_smooth(ds10_lane *l, int raw, int next, int subticks,
                      const int8_t *defaults)
{
    int32_t from = ds10_lane_map(l, raw, defaults);
    int32_t to = ds10_lane_map(l, next, defaults);

    l->cur = (int16_t)(from << 8);
    /* 0x0200C158 is a truncating signed divide, which is C's, and the original
     * asks it for ((to - from) << 8) / subticks. A step with no sub-ticks cannot
     * happen on a running sequencer; hold rather than divide by zero. */
    l->delta = (int16_t)(subticks ? ((to - from) * 256) / subticks : 0);
}

/* --- the pattern-driven tick (0x0206E5AC in full) ------------------------- */

static void say(ds10_seq_ctx *c, uint32_t msg, int32_t arg)
{
    c->player->msg = msg;
    c->player->arg = arg;
    c->player->handle(c->player, c->s);
}

static void post(ds10_seq_ctx *c, int opcode, int target, uint16_t payload)
{
    if (c->nout + 8 > c->max)
        return;
    c->nout += route((int)c->s->ntracks, target, opcode, payload,
                     c->out + c->nout);
}

/* Truncating signed divide by 127, which the original does with the magic
 * multiplier 0x81020409 rather than a call to the divider. */
static int div127(int32_t v)
{
    return (int)(v / 127);
}

/* The two mixer posts a sounding track makes every step (0x0206EB44 and
 * 0x0206EBB0). Both go to `track | 0x40`, which is the mixer only. */
static void post_chan(ds10_seq_ctx *c, int track)
{
    const ds10_seq_track *t = &c->trk[track];
    int32_t vol = div127((int32_t)t->vel * c->chan[c->chan_idx[track * 2]]);
    int32_t pan = (int32_t)t->pan + c->chan[c->chan_idx[track * 2 + 1]] - 0x40;

    if (pan < 0)
        pan = 0;
    else if (pan > 127)
        pan = 127;

    post(c, 6, track | 0x40,
         (uint16_t)(DS10_KNOB_CHAN_VOL | ((vol & 0xFF) << 8)));
    post(c, 6, track | 0x40,
         (uint16_t)(DS10_KNOB_CHAN_PAN | ((pan & 0xFF) << 8)));
}

static void run_step(ds10_seq_ctx *c)
{
    ds10_seq *s = c->s;
    int track;

    /* Swing picks the sub-tick count by step parity. */
    s->sub_cur = (s->step & 1) ? s->sub_odd : s->sub_even;

    for (track = 0; track < s->ntracks; track++) {
        int8_t *rec = c->pat->step[track][s->step];
        ds10_seq_track *t = &c->trk[track];

        /* Velocity and pan are only latched when the step has a note. */
        if (rec[0] >= 0) {
            t->vel = rec[2];
            t->pan = rec[3];
        }
        /* The note is read signed and the gate UNSIGNED, which is what makes
         * gate 0xFF the hold mode rather than a negative countdown. */
        switch (t->play_mode) {
        case DS10_PLAY_PATTERN:
            ds10_seq_step_track(t, rec[0], (uint8_t)rec[1], s->sub_cur);
            break;
        case DS10_PLAY_MUTE:
            break;
        case DS10_PLAY_UNLESS_LIVE:
            if (t->live_note < 0)
                ds10_seq_step_track(t, rec[0], (uint8_t)rec[1], s->sub_cur);
            break;
        case DS10_PLAY_RECORD:
            rec[0] = t->live_note;
            rec[1] = (int8_t)t->live_gate;
            ds10_seq_step_track(t, t->live_note, t->live_gate, s->sub_cur);
            break;
        default:
            break;
        }
        if (t->note >= 0)
            post_chan(c, track);
    }
    s->auto_sub = 0;
}

/* The automation processor, 0x0206E1EC, for the two paths a recorded song plays
 * back through - the pad's HLD (enable 1) and SMT (enable 2), both with mode 0.
 * Modes 1..3 consume a pending value from live knob input, are not decompiled,
 * and a track set to one is left alone here.
 *
 * Which one a pattern uses is byte 6 of its per-track editor record; nearly a
 * third of the records in data/saves ask for SMT, most of them on cutoff or peak.
 *
 * Runs on every sub-tick. It latches on a step's first one - HLD to the step's
 * own value, SMT to that plus a delta - then walks an SMT lane one delta per
 * sub-tick, and always pushes both lanes' current values into the machine. */
static void automation(ds10_seq_ctx *c)
{
    int track, l;

    if (c->autom == 0)
        return;
    for (track = 0; track < c->nauto; track++) {
        ds10_seq_auto *a = &c->autom[track];

        if ((a->enable != 1 && a->enable != 2) || a->mode != 0)
            continue;
        if (c->s->auto_sub == 0) {
            const int8_t *raw = c->lane_raw + track * 2 * DS10_SEQ_STEPS +
                                c->s->step * 2;

            for (l = 0; l < DS10_SEQ_LANES; l++)
                if (a->enable == 1)
                    ds10_lane_hold(&a->lane[l], raw[l], c->defaults[track]);
                else
                    ds10_lane_smooth(&a->lane[l], raw[l],
                                     c->s->step == (int8_t)(c->s->steps - 1)
                                         ? raw[l] : raw[2 + l],
                                     c->s->sub_cur, c->defaults[track]);
        } else if (a->enable == 2) {
            /* Every sub-tick after the latch walks the lane one delta on. */
            for (l = 0; l < DS10_SEQ_LANES; l++)
                a->lane[l].cur = (int16_t)(a->lane[l].cur + a->lane[l].delta);
        }
        if (c->knob == 0 || c->machine[track] == 0)
            continue;
        for (l = 0; l < DS10_SEQ_LANES; l++)
            if (a->lane[l].knob != DS10_LANE_OFF)
                c->knob(c->machine[track], a->lane[l].knob,
                        a->lane[l].cur >> 8);
    }
}

int ds10_seq_tempo(ds10_seq_ctx *c, int tempo)
{
    if (tempo > DS10_SEQ_TEMPO_MAX)
        tempo = DS10_SEQ_TEMPO_MAX;
    else if (tempo < DS10_SEQ_TEMPO_MIN)
        tempo = DS10_SEQ_TEMPO_MIN;

    c->nout = 0;
    if (c->s->tempo != (uint8_t)tempo) {
        c->s->tempo = (uint8_t)tempo;
        post(c, 4, 0x70, (uint16_t)tempo);
    }
    c->s->period = DS10_SEQ_TEMPO_NUM / tempo;
    return c->nout;
}

int ds10_seq_run(ds10_seq_ctx *c)
{
    ds10_seq *s = c->s;

    c->nout = 0;
    if (s->playing != 1) {
        /* Not running still drains the gate countdowns. */
        c->nout += ds10_seq_emit(s, c->trk, c->out + c->nout);
        return c->nout;
    }

    s->accum += s->incr;
    if (s->accum <= s->period)
        return 0;

    do {
        if (s->starting == 1) {
            s->starting = 0;
            s->step = -1;
            s->step_count = -1;
            s->bar = -1;
            s->tick = -1;
            s->subtick = 0;
        }
        s->tick++;

        if (s->subtick == 0) {
            say(c, DS10_MSG_STEP, s->step);

            s->step++;
            s->step_count++;
            if (s->step >= (int8_t)s->steps) {
                if (c->pat != 0) {
                    c->prev = c->pat;
                    say(c, DS10_MSG_ENDPAT, s->bar);
                    c->pat = 0;
                    s->pattern = 0;
                }
                s->step = 0;
            }
            if (s->step == 0) {
                s->bar++;
                c->player->answer = 0;
                say(c, DS10_MSG_BAR, 0);
                if (c->player->answer != 0) {
                    /* The answer IS the next pattern, and installing it is
                     * set_pattern (0x0206DBE4). Step count and automation
                     * sub-step are reset either way, but the 0x1D broadcast and
                     * the machine re-initialisation happen only when the pattern
                     * actually differs from the previous one - repeating a
                     * pattern does not re-phase anything. */
                    c->pat = c->player->answer;
                    s->pattern = 1;
                    s->steps = c->pat->steps;
                    s->auto_sub = 0;
                    if (c->prev != c->pat) {
                        c->prev = c->pat;
                        /* 0x1D PATTERN_CHANGE, broadcast with the tempo. This is
                         * what arms the voices' bar flags; without it the bar
                         * line below has nothing to consume and a BPM-synced LFO
                         * never re-phases. */
                        post(c, 29, 0x70, s->tempo);
                        if (c->install)
                            c->install(c, c->pat);
                    }
                    say(c, DS10_MSG_RUN, s->bar);
                    /* 0x1C BAR_START, broadcast, carrying the tempo. The
                     * original puts it in the event's u32 at +4; the low half is
                     * what a queue entry's payload word holds. */
                    post(c, 28, 0x70, s->tempo);
                }
            }
            if (c->pat != 0)
                run_step(c);
            s->subtick = s->sub_cur;
        }

        c->nout += ds10_seq_emit(s, c->trk, c->out + c->nout);
        /* The automation processor, 0x0206E1EC. It always bumps the automation
         * sub-step on the way out, whatever the tracks are set to. */
        if (c->pat != 0) {
            automation(c);
            s->auto_sub++;
        }

        s->subtick--;
        s->accum -= s->period;
    } while (s->accum > s->period);

    return c->nout;
}
