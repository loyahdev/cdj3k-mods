// SPDX-License-Identifier: MIT OR Apache-2.0
#include "cue/gate_state.h"

#include <assert.h>

int main(void)
{
    struct gate_cue_state state;

    gate_cue_state_init(&state, 1);

    /* Handler state non-zero: ordinary Hot Cue, never arms Gate Cue. */
    assert(gate_cue_state_press(&state, 1, 0, 1) == 0);
    assert(!gate_cue_state_active(&state));
    assert(gate_cue_state_release(&state, 1) == GATE_RELEASE_STOCK);

    /* OEM operation 2 is also excluded from starting a fresh gate. */
    assert(gate_cue_state_press(&state, 1, 2, 0) == 0);
    assert(!gate_cue_state_active(&state));

    /* Eligible fresh press arms; unlatched release requests mode-one/back-cue. */
    assert(gate_cue_state_press(&state, 1, 0, 0) == 1);
    assert(gate_cue_state_active(&state));
    assert(gate_cue_state_release(&state, 1) == GATE_RELEASE_MODE_ONE);
    assert(!gate_cue_state_active(&state));

    /* PLAY is consumed only while a gate is active and latches playback. */
    assert(gate_cue_state_play(&state) == 0);
    assert(gate_cue_state_press(&state, 2, 0, 0) == 1);
    assert(gate_cue_state_play(&state) == 1);
    assert(gate_cue_state_play(&state) == 1);
    assert(gate_cue_state_release(&state, 2) == GATE_RELEASE_STOCK);
    assert(!gate_cue_state_active(&state));

    /* While active, another valid pad replaces the active pad and preserves
     * latch state. Handler eligibility and operation 2 no longer block it. */
    assert(gate_cue_state_press(&state, 3, 0, 0) == 1);
    assert(gate_cue_state_press(&state, 4, 2, 1) == 2);
    assert(gate_cue_state_release(&state, 4) == GATE_RELEASE_MODE_ONE);

    assert(gate_cue_state_press(&state, 3, 0, 0) == 1);
    assert(gate_cue_state_play(&state) == 1);
    assert(gate_cue_state_press(&state, 4, 2, 1) == 2);
    assert(gate_cue_state_release(&state, 4) == GATE_RELEASE_STOCK);

    /* Repeating the same active pad is a no-op, matching native return 0. */
    assert(gate_cue_state_press(&state, 5, 0, 0) == 1);
    assert(gate_cue_state_press(&state, 5, 0, 0) == 0);
    assert(gate_cue_state_release(&state, 5) == GATE_RELEASE_MODE_ONE);

    /* Invalid pad clears any current session and falls through to stock. */
    assert(gate_cue_state_press(&state, 6, 0, 0) == 1);
    assert(gate_cue_state_press(&state, 8, 0, 0) == 0);
    assert(!gate_cue_state_active(&state));
    assert(gate_cue_state_release(&state, 6) == GATE_RELEASE_STOCK);

    /* Disabling Gate Cue clears state and makes every path stock/no-op. */
    assert(gate_cue_state_press(&state, 7, 0, 0) == 1);
    gate_cue_state_set_enabled(&state, 0);
    assert(!gate_cue_state_active(&state));
    assert(gate_cue_state_play(&state) == 0);
    assert(gate_cue_state_press(&state, 1, 0, 0) == 0);
    assert(gate_cue_state_release(&state, 1) == GATE_RELEASE_STOCK);

    return 0;
}
