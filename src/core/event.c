/* DS-10 synth event handling.
 *
 * Reimplemented from the event dispatcher at 0x0207546C, which the render
 * handler calls before each frame's preamble. Events are 8 bytes in the
 * original: opcode in byte 0, signed note in byte 4, or a 32-bit argument at
 * +4 for the tempo event.
 *
 * Only these opcodes do anything to a synth voice; the rest are for the mixer
 * or the drum voices.
 */
#include "event.h"

#include "fixed.h"

/* Oscillator reset (0x02071E14): phase1 to zero, phase2 to this constant. */
#define OSC_RESET_PHASE2 0x0017F3A7

#define LFO_RESET_PHASE 0x7FFFFFFF

static void set_note(ds10_notectrl *n, int note)
{
    int32_t target = ds10_shl(note, 15);

    /* Direction decides which side the portamento clamp guards, so it has to be
     * chosen when the target changes, not while gliding. */
    n->dir = (n->current > target) ? 0 : 1;
    n->target = target;
}

static void reset_lfo_phase(ds10_lfo *l)
{
    l->phase_down = LFO_RESET_PHASE;
    l->phase_up = 0;
}

static void zero_env(ds10_env *e)
{
    e->aux = 0;
    e->level = 0;
}

void ds10_voice_event(ds10_voice *v, int opcode, int note, int32_t arg,
                      const ds10_tables *t)
{
    switch (opcode) {
    case DS10_EV_NOTE_OFF:
        v->env.stage = DS10_ENV_RELEASE;
        v->env.retrig = 1;
        break;

    case DS10_EV_NOTE_ON:
        set_note(&v->nc, note);
        /* A pending pattern change forces a hard restart rather than a glide. */
        if (v->bar_env != 0) {
            v->bar_env = 0;
            zero_env(&v->env);
            v->env.stage = DS10_ENV_RELEASE;
            v->env.retrig = 1;
        }
        if (v->env.retrig != 0)
            v->env.stage = DS10_ENV_ATTACK;
        v->env.retrig = 0;
        break;

    case DS10_EV_SET_BPM:
        v->bpm = arg;
        v->lfo.bpm = arg;
        /* Re-derive the LFO rate from the stored knob; when BPM sync is on the
         * rate depends on the tempo. */
        ds10_patch_set_knob(v, 24, v->lfo.rate_knob, t);
        break;

    case DS10_EV_STOP_ABRUPT:
        v->env.stage = DS10_ENV_RELEASE;
        v->env.retrig = 1;
        break;

    case DS10_EV_PLAY:
        reset_lfo_phase(&v->lfo);
        zero_env(&v->env);
        break;

    case DS10_EV_BAR_START:
        /* Only a BPM-synced LFO is re-phased at the bar line; a free-running
         * one keeps its phase across bars. */
        if (v->bar_lfo != 0) {
            v->bar_lfo = 0;
            if (v->lfo.bpm_sync)
                reset_lfo_phase(&v->lfo);
        }
        break;

    case DS10_EV_PATTERN_CHANGE:
        v->bar_lfo = 1;
        v->bar_env = 1;
        break;

    case DS10_EV_DRUM_SHAPE:
        reset_lfo_phase(&v->lfo);
        zero_env(&v->env);
        v->vco.phase1 = 0;
        v->vco.phase2 = OSC_RESET_PHASE2;
        break;

    default:
        break;
    }
}

void ds10_voice_note_on(ds10_voice *v, int note, const ds10_tables *t)
{
    ds10_voice_event(v, DS10_EV_NOTE_ON, note, 0, t);
}

void ds10_voice_note_off(ds10_voice *v, const ds10_tables *t)
{
    ds10_voice_event(v, DS10_EV_NOTE_OFF, 0, 0, t);
}

void ds10_voice_set_bpm(ds10_voice *v, int32_t bpm, const ds10_tables *t)
{
    ds10_voice_event(v, DS10_EV_SET_BPM, 0, bpm, t);
}
