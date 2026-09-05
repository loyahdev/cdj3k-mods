// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef EP122_MOD_GATE_STATE_H
#define EP122_MOD_GATE_STATE_H

#include <stdbool.h>
#include <stdatomic.h>

/*
 * Gate Cue session state, modeled after the working EP122 3.22 Gate Cue
 * implementation.
 *
 * session:
 *   low nibble (0x0f): active Hot Cue pad + 1; zero means no active gate
 *   bit 4      (0x10): PLAY was pressed while the gate was active (latched)
 */
struct gate_cue_state {
    atomic_bool enabled;
    atomic_uint session;
};

enum gate_release_action {
    GATE_RELEASE_STOCK = 0,
    GATE_RELEASE_MODE_ONE = 1,
};

void gate_cue_state_init(struct gate_cue_state *state, int enabled);
void gate_cue_state_set_enabled(struct gate_cue_state *state, int enabled);
int gate_cue_state_active(const struct gate_cue_state *state);

/*
 * pad:             Hot Cue pad index, 0..7
 * operation:       second 32-bit OEM event word
 * handler_blocked: non-zero when HotCueHandler+0xe7 was non-zero before stock
 *
 * Return values mirror the working backend: 0 = unchanged/rejected,
 * 1 = fresh gate armed, 2 = active gate moved to another pad.
 */
int gate_cue_state_press(struct gate_cue_state *state,
                         unsigned int pad,
                         unsigned int operation,
                         int handler_blocked);

/* Returns 1 when PLAY is consumed to latch an active gate, otherwise 0. */
int gate_cue_state_play(struct gate_cue_state *state);

enum gate_release_action gate_cue_state_release(struct gate_cue_state *state,
                                                unsigned int pad);

#endif
