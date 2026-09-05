// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef CUE_SHORTCUT_TAP_H
#define CUE_SHORTCUT_TAP_H
struct cue_shortcut_tap {
    int armed, desired, pending, queued;
};
static inline void cue_tap_down(struct cue_shortcut_tap *tap, int active)
{
    tap->desired = !(tap->pending ? tap->queued : active);
    tap->armed = 1;
}
static inline void cue_tap_up(struct cue_shortcut_tap *tap, int inside)
{
    if (tap->armed && inside) {
        tap->queued = tap->desired;
        tap->pending = 1;
    }
    tap->armed = 0;
}
static inline int cue_tap_take(struct cue_shortcut_tap *tap, int enabled, int *desired)
{
    if (!enabled) {
        tap->pending = tap->armed = 0;
        return 0;
    }
    if (!tap->pending)
        return 0;
    *desired = tap->queued;
    tap->pending = 0;
    return 1;
}
#endif
