// SPDX-License-Identifier: MIT OR Apache-2.0
#include "cue/shortcut_tap.h"
#include "test.h"
int main(void)
{
    int desired = -1;
    struct cue_shortcut_tap tap = {0};
    T_CASE("overlapping native mouse-up and new button apply one absolute state");
    for (int active = 0; active <= 1; active++) {
        for (int native_hit = 0; native_hit <= 1; native_hit++) {
            int runtime = active;
            cue_tap_down(&tap, runtime);
            cue_tap_up(&tap, 1);
            if (native_hit)
                runtime = !runtime;
            CHECK(cue_tap_take(&tap, 1, &desired));
            runtime = desired;
            CHECK_INT(runtime, !active);
            CHECK(!cue_tap_take(&tap, 1, &desired));
        }
    }
    T_CASE("release outside and unarmed release do not request a change");
    cue_tap_down(&tap, 0);
    cue_tap_up(&tap, 0);
    CHECK(!cue_tap_take(&tap, 1, &desired));
    cue_tap_up(&tap, 1);
    CHECK(!cue_tap_take(&tap, 1, &desired));
    T_CASE("disabling the master cancels both queued and held taps");
    cue_tap_down(&tap, 0);
    cue_tap_up(&tap, 1);
    CHECK(!cue_tap_take(&tap, 0, &desired));
    CHECK(!cue_tap_take(&tap, 1, &desired));
    cue_tap_down(&tap, 0);
    CHECK(!cue_tap_take(&tap, 0, &desired));
    cue_tap_up(&tap, 1);
    CHECK(!cue_tap_take(&tap, 1, &desired));
    T_CASE("two quick taps before the timer cancel, including outside native hitbox");
    cue_tap_down(&tap, 0);
    cue_tap_up(&tap, 1);
    cue_tap_down(&tap, 0);
    cue_tap_up(&tap, 1);
    CHECK(cue_tap_take(&tap, 1, &desired));
    CHECK_INT(desired, 0);
    return t_done("cue_shortcut_tap");
}
