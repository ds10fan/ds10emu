/* Loading a whole ds10_voice from a captured state snapshot.
 *
 * The original's constructor does not leave a voice zeroed, and the residue is
 * audible - a decaying tail on the first frames, and a retrigger byte without
 * which a note-on glides instead of restarting the attack. Anything that has to
 * match the original sample for sample therefore has to start from the state the
 * constructor left, not from a memset.
 *
 * The field order matches the V_* lists in tools/oracle.py voice_snapshot. It
 * lives in one place so the tests and the renderer cannot drift apart.
 */
#ifndef DS10_VOICE_STATE_H
#define DS10_VOICE_STATE_H

#include "voice.h"

#define DS10_VOICE_STATE_FIELDS 93

/* Reads DS10_VOICE_STATE_FIELDS words from `f`. */
void ds10_voice_state_load(ds10_voice *v, const int32_t *f);

#endif /* DS10_VOICE_STATE_H */
