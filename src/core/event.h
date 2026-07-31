/* Event opcodes and the synth voice's event handler. See event.c. */
#ifndef DS10_EVENT_H
#define DS10_EVENT_H

#include <stdint.h>

#include "patch.h"
#include "voice.h"

/* Opcodes the synth voice acts on. Others exist for the mixer and drums. */
enum ds10_event {
    DS10_EV_NOTE_OFF = 1,
    DS10_EV_NOTE_ON = 2,
    DS10_EV_SET_BPM = 4,
    DS10_EV_STOP_ABRUPT = 5,
    DS10_EV_PLAY = 0x18,
    DS10_EV_BAR_START = 0x1C,
    DS10_EV_PATTERN_CHANGE = 0x1D,
    DS10_EV_DRUM_SHAPE = 0x1E,
};

void ds10_voice_event(ds10_voice *v, int opcode, int note, int32_t arg,
                      const ds10_tables *t);
void ds10_voice_note_on(ds10_voice *v, int note, const ds10_tables *t);
void ds10_voice_note_off(ds10_voice *v, const ds10_tables *t);
void ds10_voice_set_bpm(ds10_voice *v, int32_t bpm, const ds10_tables *t);

#endif /* DS10_EVENT_H */
