/* Loading a ds10_mixer and its ds10_fx from a captured state snapshot.
 *
 * The constructors leave neither zeroed, so anything that has to match the
 * original starts from these rather than from a memset. Field order matches
 * MX_FIELDS and FXS_FIELDS in tools/oracle.py.
 */
#ifndef DS10_MIXER_STATE_H
#define DS10_MIXER_STATE_H

#include "mixer.h"

#define DS10_MIX_STATE_FIELDS 21
#define DS10_FX_STATE_FIELDS 40

void ds10_mixer_state_load(ds10_mixer *m, const int32_t *f);
void ds10_fx_state_load(ds10_fx *fx, const int32_t *f);

#endif /* DS10_MIXER_STATE_H */
