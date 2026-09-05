// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef EP122_MOD_STEM_MODE_H
#define EP122_MOD_STEM_MODE_H

enum stem_mode_preference {
    STEM_MODE_PREFER_SERVER = 0,
    STEM_MODE_PREFER_PRESTEM = 1,
};

/* Normalise the two persisted switches and resolve an impossible both-on
 * state in favour of the switch the DJ just selected. */
void stem_mode_exclusive(int *server, int *prestem,
                         enum stem_mode_preference preference);

#endif
