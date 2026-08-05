#ifndef PLAYER_OUTPUTS_NULL_H
#define PLAYER_OUTPUTS_NULL_H

#include "../audio/output.h"

/* drops every frame -- for headless testing/decoder verification without a
 * real audio device. Stateless: the state pointer it hands back is NULL. */
CTUI_AUDIO_OUTPUT null_output_make(void);

#endif
