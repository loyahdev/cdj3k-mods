// SPDX-License-Identifier: MIT OR Apache-2.0
/* The MOD SETTINGS overlay borrows an eight-row stock EP122 model. Row zero is
 * its title, so every feature row must fit at stock indices 1..7. */
#include "test.h"
#include "cue/gate_menu_state.h"
#include "../mods/kit/menu.c"

static int gate_menu, smart, preview, theme, xpad, server, overcue;
static const char *const gate_values[GATE_MENU_STATE_COUNT] = {
    "OFF", "ON / DEFAULT OFF", "ON / DEFAULT ON"
};
static const char *const themes[] = { "ORIGINAL", "WHITE" };

static const struct kit_row gate_rows[] = {
    { .label = "GATE CUE", .idx = KIT_IDX_GATE,
      .state = &gate_menu, .values = gate_values,
      .nvalues = GATE_MENU_STATE_COUNT },
};
static const struct kit_row smart_rows[] = {
    KIT_ROW_BOOL("SMART CUE", &smart, .idx = KIT_IDX_SMART),
};
static const struct kit_row preview_rows[] = {
    KIT_ROW_BOOL("PREVIEW HOTCUE", &preview, .idx = KIT_IDX_PREVIEW),
};
static const struct kit_row theme_rows[] = {
    { .label = "THEME", .idx = KIT_IDX_THEME, .state = &theme,
      .values = themes, .nvalues = 2 },
};
static const struct kit_row xpad_rows[] = {
    KIT_ROW_BOOL("ENABLE X-PAD", &xpad, .idx = KIT_IDX_XPAD),
};
static const struct kit_row stem_rows[] = {
    KIT_ROW_BOOL("SERVER STEMS", &server, .idx = KIT_IDX_STEMS),
    KIT_ROW_BOOL("OverCue Stems", &overcue,
                 .idx = 0, .parent = &stem_rows[0], .show_when = 0),
};

static void add_rows(const struct kit_row *rows, size_t bytes)
{
    kit_menu_add(rows, (int)(bytes / sizeof(*rows)));
}

int main(void)
{
    int gate_enabled = 0, gate_default = 1;

    add_rows(gate_rows, sizeof(gate_rows));
    add_rows(smart_rows, sizeof(smart_rows));
    add_rows(preview_rows, sizeof(preview_rows));
    add_rows(theme_rows, sizeof(theme_rows));
    add_rows(xpad_rows, sizeof(xpad_rows));
    add_rows(stem_rows, sizeof(stem_rows));

    T_CASE("every visible setting fits stock EP122 indices 1 through 7");
    CHECK_INT(KIT_MENU_MAX_ROWS, 7);
    CHECK_INT(kit_menu_count(), 7);
    CHECK_STR(kit_menu_row(0)->label, "GATE CUE");
    CHECK_STR(kit_menu_row(6)->label, "OverCue Stems");
    CHECK(kit_menu_row(7) == NULL);

    T_CASE("one Gate Cue row preserves both master and default states");
    CHECK_INT(gate_menu_state_from(0, 0), GATE_MENU_OFF);
    CHECK_INT(gate_menu_state_from(0, 1), GATE_MENU_OFF);
    CHECK_INT(gate_menu_state_from(1, 0), GATE_MENU_ON_DEFAULT_OFF);
    CHECK_INT(gate_menu_state_from(1, 1), GATE_MENU_ON_DEFAULT_ON);

    gate_menu_state_apply(GATE_MENU_ON_DEFAULT_OFF,
                          &gate_enabled, &gate_default);
    CHECK_INT(gate_enabled, 1);
    CHECK_INT(gate_default, 0);
    gate_menu_state_apply(GATE_MENU_ON_DEFAULT_ON,
                          &gate_enabled, &gate_default);
    CHECK_INT(gate_enabled, 1);
    CHECK_INT(gate_default, 1);
    gate_menu_state_apply(GATE_MENU_OFF, &gate_enabled, &gate_default);
    CHECK_INT(gate_enabled, 0);
    CHECK_INT(gate_default, 1);

    return t_done("menu_capacity");
}
