// SPDX-License-Identifier: MIT OR Apache-2.0
#include "stem/mode.h"

void stem_mode_exclusive(int *server, int *prestem,
                         enum stem_mode_preference preference)
{
    if (!server || !prestem)
        return;

    *server = *server ? 1 : 0;
    *prestem = *prestem ? 1 : 0;
    if (!*server || !*prestem)
        return;

    if (preference == STEM_MODE_PREFER_PRESTEM)
        *server = 0;
    else
        *prestem = 0;
}
