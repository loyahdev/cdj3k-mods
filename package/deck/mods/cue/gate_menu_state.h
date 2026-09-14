// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * gate_menu_state.h - pack Gate Cue's master and new-track default into the
 * one MOD SETTINGS row that the stock eight-row list can safely carry.
 */
#ifndef EP122_MOD_GATE_MENU_STATE_H
#define EP122_MOD_GATE_MENU_STATE_H

enum gate_menu_state {
    GATE_MENU_OFF = 0,
    GATE_MENU_ON_DEFAULT_OFF,
    GATE_MENU_ON_DEFAULT_ON,
    GATE_MENU_STATE_COUNT,
};

static inline int gate_menu_state_from(int enabled, int default_active)
{
    if (!enabled)
        return GATE_MENU_OFF;
    return default_active ? GATE_MENU_ON_DEFAULT_ON
                          : GATE_MENU_ON_DEFAULT_OFF;
}

static inline void gate_menu_state_apply(int state, int *enabled,
                                         int *default_active)
{
    if (!enabled || !default_active)
        return;

    switch (state) {
    case GATE_MENU_ON_DEFAULT_OFF:
        *enabled = 1;
        *default_active = 0;
        break;
    case GATE_MENU_ON_DEFAULT_ON:
        *enabled = 1;
        *default_active = 1;
        break;
    default:
        /* Turning the feature off does not erase the saved default. */
        *enabled = 0;
        break;
    }
}

#endif /* EP122_MOD_GATE_MENU_STATE_H */
