// SPDX-License-Identifier: MIT OR Apache-2.0
#include "cue/gate_state.h"

#define GATE_SESSION_PAD_MASK 0x0fU
#define GATE_SESSION_LATCH    0x10U

void gate_cue_state_init(struct gate_cue_state *state, int enabled)
{
    atomic_init(&state->enabled, enabled != 0);
    atomic_init(&state->session, 0U);
}

void gate_cue_state_set_enabled(struct gate_cue_state *state, int enabled)
{
    atomic_store_explicit(&state->enabled, enabled != 0, memory_order_release);
    if (!enabled)
        atomic_store_explicit(&state->session, 0U, memory_order_release);
}

int gate_cue_state_active(const struct gate_cue_state *state)
{
    return (atomic_load_explicit(&state->session, memory_order_acquire) &
            GATE_SESSION_PAD_MASK) != 0U;
}

int gate_cue_state_press(struct gate_cue_state *state,
                         unsigned int pad,
                         unsigned int operation,
                         int handler_blocked)
{
    unsigned int old_state, new_state, active_pad, encoded_pad;

    if (!atomic_load_explicit(&state->enabled, memory_order_acquire))
        return 0;

    if (pad > 7U) {
        atomic_store_explicit(&state->session, 0U, memory_order_release);
        return 0;
    }

    encoded_pad = pad + 1U;

    for (;;) {
        old_state = atomic_load_explicit(&state->session, memory_order_acquire);
        active_pad = old_state & GATE_SESSION_PAD_MASK;

        if (active_pad == 0U) {
            /* Exact fresh-session eligibility from the working 3.22 backend. */
            if (handler_blocked || operation == 2U)
                return 0;
            new_state = encoded_pad;
        } else {
            /* Active sessions may move pads and preserve the PLAY latch. */
            new_state = (old_state & GATE_SESSION_LATCH) | encoded_pad;
        }

        if (new_state == old_state)
            return 0;

        if (atomic_compare_exchange_weak_explicit(&state->session,
                                                  &old_state,
                                                  new_state,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire))
            return active_pad == 0U ? 1 : 2;
    }
}

int gate_cue_state_play(struct gate_cue_state *state)
{
    unsigned int old_state, new_state;

    if (!atomic_load_explicit(&state->enabled, memory_order_acquire))
        return 0;

    for (;;) {
        old_state = atomic_load_explicit(&state->session, memory_order_acquire);
        if ((old_state & GATE_SESSION_PAD_MASK) == 0U)
            return 0;

        new_state = old_state | GATE_SESSION_LATCH;
        if (new_state == old_state)
            return 1;

        if (atomic_compare_exchange_weak_explicit(&state->session,
                                                  &old_state,
                                                  new_state,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire))
            return 1;
    }
}

enum gate_release_action gate_cue_state_release(struct gate_cue_state *state,
                                                unsigned int pad)
{
    unsigned int old_state;

    if (!atomic_load_explicit(&state->enabled, memory_order_acquire))
        return GATE_RELEASE_STOCK;

    if (pad > 7U) {
        atomic_store_explicit(&state->session, 0U, memory_order_release);
        return GATE_RELEASE_STOCK;
    }

    old_state = atomic_exchange_explicit(&state->session, 0U,
                                         memory_order_acq_rel);
    if ((old_state & GATE_SESSION_PAD_MASK) == 0U)
        return GATE_RELEASE_STOCK;

    return (old_state & GATE_SESSION_LATCH)
               ? GATE_RELEASE_STOCK
               : GATE_RELEASE_MODE_ONE;
}
