// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cue/gate.c - Gate Cue transport for the exact OEM 3.22 EP122 profile.
 *
 * Three guarded stock slots carry the complete behavior: Hot Cue change uses
 * the OEM HotCueHandler+0xe7 gate-eligibility byte, PLAY latches a held gate,
 * and the final Hot Cue release is changed to mode 1 only for an unlatched
 * stopped-state press. A press made while already playing is passed through
 * untouched and therefore remains an ordinary Hot Cue jump.
 */
#include "cue/cue.h"
#include "cue/gate_state.h"
#include "kit/menu.h"
#include "kit/mod.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GATE_HOT_CHANGE_SLOT    UINT64_C(0x221bcd0)
#define GATE_HOT_CHANGE_TARGET  UINT64_C(0x1858270)
#define GATE_HOT_RELEASE_SLOT   UINT64_C(0x221bcd8)
#define GATE_HOT_RELEASE_TARGET UINT64_C(0x1858778)
#define GATE_PLAY_LATCH_SLOT    UINT64_C(0x22209a0)
#define GATE_PLAY_LATCH_TARGET  UINT64_C(0x1878098)

enum gate_hook_id {
    GATE_HOOK_HOT_CHANGE = 1,
    GATE_HOOK_HOT_RELEASE = 2,
    GATE_HOOK_PLAY_LATCH = 3,
};

struct gate_oem_event {
    uint32_t pad;
    uint32_t operation;
};

int g_gate_on;     /* persisted master; see core/common.c */
int g_gate_active; /* runtime play-screen toggle; intentionally not persisted */
int g_gate_default_active = 1; /* new tracks start with Gate Cue on by default */
static int gate_g_track_pending;

void cdj_gate_cue_hot_change(void);
void cdj_gate_cue_hot_release(void);
void cdj_gate_cue_play_latch(void);
uintptr_t cdj_gate_cue_original_hot_change;
uintptr_t cdj_gate_cue_original_hot_release;
uintptr_t cdj_gate_cue_original_play_latch;

static struct gate_cue_state gate_g_state;
static _Thread_local struct gate_oem_event gate_g_rewritten_release;

static const uint8_t k_change_guard[] = {
    0xfd, 0x7b, 0xb8, 0xa9, 0xfd, 0x03, 0x00, 0x91,
    0xf3, 0x53, 0x01, 0xa9, 0xf3, 0x03, 0x00, 0xaa,
};
static const uint8_t k_release_guard[] = {
    0xfd, 0x7b, 0xb8, 0xa9, 0xfd, 0x03, 0x00, 0x91,
    0xf3, 0x53, 0x01, 0xa9, 0xf4, 0x03, 0x00, 0xaa,
};
static const uint8_t k_play_guard[] = {
    0xfd, 0x7b, 0xb8, 0xa9, 0xfd, 0x03, 0x00, 0x91,
    0xf3, 0x53, 0x01, 0xa9, 0xf3, 0x03, 0x00, 0xaa,
};

/* Verified from the working OEM 3.22 Gate Cue backend: before the stock Hot
 * Cue change handler runs, it reads one byte directly from HotCueHandler+0xe7.
 * Zero permits a fresh Gate Cue session; non-zero leaves the press as a normal
 * Hot Cue. Unknown/unreadable layouts fail closed and do not arm Gate Cue. */
static int gate_press_allowed(uintptr_t handler)
{
    uint8_t state = 0xff;

    if (!handler ||
        mod_safe_read(handler + 0xe7, &state, sizeof(state)) != 0)
        return 0;

    MDBG("gate: HotCueHandler+0xe7 = %u -> %s\n",
         (unsigned)state, state == 0 ? "arm" : "normal hot cue");

    return state == 0;
}

/* Called by the ABI-preserving assembly entries. Hot Cue hooks use the return
 * value as their possibly rewritten x1 event pointer. PLAY uses 1 to consume
 * the press while an active gate is latched, or 0 to continue to stock PLAY. */
uintptr_t cdj_gate_cue_prepare(unsigned int id, uintptr_t arg0, uintptr_t arg1,
                               uintptr_t arg2, uintptr_t arg3);
uintptr_t cdj_gate_cue_prepare(unsigned int id, uintptr_t arg0, uintptr_t arg1,
                               uintptr_t arg2, uintptr_t arg3)
{
    struct gate_oem_event event;
    int continuing;

    (void)arg2;
    (void)arg3;
    gate_cue_state_set_enabled(&gate_g_state, g_gate_on && g_gate_active);

    if (id == GATE_HOOK_PLAY_LATCH)
        return (uintptr_t)gate_cue_state_play(&gate_g_state);
    if (!arg1 || mod_safe_read(arg1, &event, sizeof(event)) != 0)
        return arg1;

    if (id == GATE_HOOK_HOT_CHANGE) {
        continuing = gate_cue_state_active(&gate_g_state);
        (void)gate_cue_state_press(&gate_g_state,
                                   event.pad,
                                   event.operation,
                                   continuing ? 0 : !gate_press_allowed(arg0));
        return arg1;
    }
    if (id == GATE_HOOK_HOT_RELEASE &&
        gate_cue_state_release(&gate_g_state, event.pad) ==
            GATE_RELEASE_MODE_ONE) {
        gate_g_rewritten_release = event;
        gate_g_rewritten_release.operation = 1U;
        return (uintptr_t)&gate_g_rewritten_release;
    }
    return arg1;
}


/* Exact-update mode: the recovered companion DSO owns Gate Cue transport and
 * the parent mouse listener. The shim must never patch its three transport
 * slots. MOD SETTINGS seeds the native runtime byte, while shortcut.c adds a
 * visible child between the measured TIME/TEMPO bounds. The companion's nested
 * listener still receives mouse-up; the shortcut coalesces its tap with that
 * listener by setting the desired state on the next display tick. */
#define NATIVE_GATE_TOGGLE_INIT_OFF UINT64_C(0x34a0)
#define NATIVE_GATE_STATE_OFF       UINT64_C(0x1b008)

static uintptr_t native_gate_base(void)
{
    FILE *fp;
    char line[512];

    fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        unsigned long start = 0, end = 0, off = 0;
        char perms[5] = {0};
        int n;
        uint8_t magic[4];
        static const uint8_t elf_magic[4] = {0x7f, 'E', 'L', 'F'};

        if (!strstr(line, "/preui.so") &&
            !strstr(line, "/libgatecue-native-physical-stems-"))
            continue;
        n = sscanf(line, "%lx-%lx %4s %lx", &start, &end, perms, &off);
        if (n == 4 && start >= off) {
            uintptr_t base = (uintptr_t)(start - off);
            if (mod_safe_read(base, magic, sizeof(magic)) == 0 &&
                memcmp(magic, elf_magic, sizeof(magic)) == 0) {
                fclose(fp);
                return base;
            }
        }
    }
    fclose(fp);
    return 0;
}

static int native_gate_seed(int on)
{
    static const uint8_t guard[16] = {
        0x21,0x1c,0x00,0x12,0x40,0x00,0x00,0xb4,
        0x01,0xfc,0x9f,0x08,0xc0,0x03,0x5f,0xd6
    };
    uintptr_t base = native_gate_base();
    uintptr_t fn;
    uint8_t got[sizeof(guard)];
    typedef void (*toggle_init_fn)(uint8_t *, int);

    if (!base) return -1;
    fn = base + NATIVE_GATE_TOGGLE_INIT_OFF;
    if (mod_safe_read(fn, got, sizeof(got)) != 0 ||
        memcmp(got, guard, sizeof(got)) != 0)
        return -1;
    ((toggle_init_fn)(void *)fn)((uint8_t *)(base + NATIVE_GATE_STATE_OFF), on ? 1 : 0);
    return 0;
}

int cue_gate_native_owned(void)
{
    const char *v = getenv("EP122_NATIVE_OVERCUE");
    return v && *v && strcmp(v, "0") != 0;
}

int cue_gate_runtime_active(void)
{
    uintptr_t base;
    uint8_t state = 0;

    if (!cue_gate_native_owned())
        return g_gate_active ? 1 : 0;
    base = native_gate_base();
    if (!base || mod_safe_read(base + NATIVE_GATE_STATE_OFF,
                               &state, sizeof(state)) != 0)
        return 0;
    return state ? 1 : 0;
}

int cue_gate_runtime_set_active(int active)
{
    if (!g_gate_on) return -1;
    if (cue_gate_native_owned())
        return native_gate_seed(active);
    g_gate_active = active ? 1 : 0;
    cue_gate_active_changed();
    return 0;
}

void cue_gate_changed(void)
{
    if (cue_gate_native_owned()) {
        if (native_gate_seed(g_gate_on) != 0)
            MWARN("gate: native Gate Cue control unavailable\n");
        cue_shortcut_refresh();
        return;
    }
    g_gate_active = g_gate_on ? 1 : 0;
    gate_cue_state_set_enabled(&gate_g_state, g_gate_on && g_gate_active);
    cue_shortcut_refresh();
}

void cue_gate_active_changed(void)
{
    if (!g_gate_on)
        g_gate_active = 0;
    gate_cue_state_set_enabled(&gate_g_state, g_gate_on && g_gate_active);
    cue_shortcut_refresh();
}

void cue_gate_track_loaded(void)
{
    /* Source identity is observed by the audio hook. Defer native writes and
     * UI refresh to cue_shortcut_tick, which runs on the message thread. */
    if (g_gate_on)
        __atomic_store_n(&gate_g_track_pending, 1, __ATOMIC_RELEASE);
}

void cue_gate_tick(void)
{
    if (__atomic_exchange_n(&gate_g_track_pending, 0, __ATOMIC_ACQ_REL) && g_gate_on)
        (void)cue_gate_runtime_set_active(g_gate_default_active);
}

static const struct kit_row k_rows[] = {
    KIT_ROW_BOOL("GATE CUE", &g_gate_on, .idx = KIT_IDX_GATE,
                 .changed = cue_gate_changed),
    KIT_ROW_BOOL("GATE CUE DEFAULT", &g_gate_default_active, .idx = 0,
                 .parent = &k_rows[0], .show_when = 1),
};

static int gate_install(void)
{
    int ok = 0;

    /* Register the master switch regardless of hook outcome. This setting is
     * the only control that makes the play-screen shortcut exist/disappear. */
    kit_menu_add(k_rows, (int)(sizeof(k_rows) / sizeof(k_rows[0])));

    if (cue_gate_native_owned()) {
        if (native_gate_seed(g_gate_on) != 0) {
            MERR("gate: exact native subsystem requested but companion DSO is unavailable\n");
            return -1;
        }
        g_gate_active = cue_gate_runtime_active();
        MDBG("gate: exact native companion owns transport; source shortcut mirrors its toggle\n");
        return 0;
    }

    g_gate_active = g_gate_on ? 1 : 0;
    gate_cue_state_init(&gate_g_state, g_gate_on && g_gate_active);

    /* Contract check: the repo's generic cue layer wraps this same PLAY slot
     * at priority 5. Gate Cue must own the stock slot first so cue_pad saves
     * our PLAY entry as its original and chains through it. */
    if (ep122_sym(EP122_GATE_PLAYPAUSE_TASK) + 0x10 != GATE_PLAY_LATCH_SLOT ||
        ep122_sym(EP122_GATE_PLAYPAUSE_RUN) != GATE_PLAY_LATCH_TARGET) {
        MERR("gate: resolved PLAY slot does not match OEM 3.22 profile\n");
        return -1;
    }

    ok += mod_patch_exec_slot("gateHotChange", GATE_HOT_CHANGE_SLOT,
                              GATE_HOT_CHANGE_TARGET, k_change_guard,
                              sizeof(k_change_guard),
                              (void *)cdj_gate_cue_hot_change,
                              &cdj_gate_cue_original_hot_change) == 0;
    ok += mod_patch_exec_slot("gateHotRelease", GATE_HOT_RELEASE_SLOT,
                              GATE_HOT_RELEASE_TARGET, k_release_guard,
                              sizeof(k_release_guard),
                              (void *)cdj_gate_cue_hot_release,
                              &cdj_gate_cue_original_hot_release) == 0;
    ok += mod_patch_exec_slot("gatePlayLatch", GATE_PLAY_LATCH_SLOT,
                              GATE_PLAY_LATCH_TARGET, k_play_guard,
                              sizeof(k_play_guard),
                              (void *)cdj_gate_cue_play_latch,
                              &cdj_gate_cue_original_play_latch) == 0;
    if (ok != 3) {
        MERR("gate: exact OEM hook set incomplete (%d/3) -> refused\n", ok);
        return -1;
    }

    return 0;
}

/* Priority 1 is functional: cue_pad (priority 5) patches the PLAY slot after
 * this and deliberately chains to cdj_gate_cue_play_latch. */
KIT_MOD(k_mod_cue_gate,
        .name = "cue_gate", .prio = 1, .install = gate_install,
        .what = "OEM 3.22 Gate Cue transport with verified handler state");
