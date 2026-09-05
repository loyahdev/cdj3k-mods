// SPDX-License-Identifier: MIT OR Apache-2.0
/* Run the shipped row-selection and pane code with an in-memory JUCE model.
 * The stock selection substitute deliberately notifies BEFORE ownership moves,
 * reproducing the on-deck one-row lag without running the deck application. */
#include "kit/mod.h"
#include "test.h"
#undef KIT_MOD
#define KIT_MOD(sym, ...) static const struct kit_mod sym __attribute__((unused)) = {__VA_ARGS__}
#include "../mods/menu/hooks.c"
#include "../mods/menu/pane.c"
#include "../mods/menu/rows.c"
#include "../mods/menu/state.c"
uintptr_t g_ep122_sym[EP122_SYM__COUNT];
const char *const kit_off_on[2] = {"OFF", "ON"};
static const char *const themes[] = {"ORIGINAL", "WHITE", "GREY"};
static int flags[3] = {1, 2, 0};
static const struct kit_row rows[] = {
    {.label = "PRE-STEMS", .state = &flags[0], .values = kit_off_on, .nvalues = 2},
    {.label = "THEME", .state = &flags[1], .values = themes, .nvalues = 3},
    {.label = "ENABLE X-PAD", .state = &flags[2], .values = kit_off_on, .nvalues = 2},
};
static uint8_t view[0x600], model[0x200], list[0x200];
static const char *pending_labels[8], *model_labels[8];
static int pending_count, model_count, selected, rebuilds, saves, stock_calls;
const struct kit_row *kit_menu_row(int i) { return i >= 0 && i < 3 ? &rows[i] : NULL; }
int kit_menu_count(void) { return 3; }
int mod_safe_read(uintptr_t addr, void *buf, size_t n)
{
    memcpy(buf, (void *)addr, n);
    return 0;
}
int mod_safe_write(uintptr_t addr, const void *buf, size_t n)
{
    memcpy((void *)addr, buf, n);
    return 0;
}
uintptr_t menu_view_ptr(uintptr_t v, uintptr_t off)
{
    uintptr_t p;
    memcpy(&p, (void *)(v + off), sizeof(p));
    return p;
}
void menu_kbd_close(void) {}
void menu_kbd_open(const struct kit_row *r) { (void)r; }
int menu_kbd_is_up(void) { return 0; }
void menu_pane_show(uintptr_t v, int shown)
{
    (void)v;
    (void)shown;
}
void mods_settings_save(void) { saves++; }
void juce_strarray_set(void *sa, const char *const *labels, int n)
{
    (void)sa;
    pending_count = n;
    memcpy(pending_labels, labels, n * sizeof(*labels));
}
static void setstrings(void *m, void *sa)
{
    (void)m;
    (void)sa;
    model_count = pending_count;
    memcpy(model_labels, pending_labels, model_count * sizeof(*model_labels));
}
static void noop(void *x) { (void)x; }
static void bounds(void *l, int x, int y, int w, int h)
{
    int32_t b[] = {x, y, w, h};
    memcpy((uint8_t *)l + COMP_BOUNDS_OFF, b, sizeof(b));
}
static void update(void *l)
{
    (void)l;
    CHECK_INT(menu_rnumrows(model), model_count);
    for (int i = 0; i < model_count; i++)
        CHECK(model_labels[i] != NULL);
    rebuilds++;
}
static void selectrow(void *l, int row, int scroll, int others)
{
    (void)scroll;
    (void)others;
    if (l == list) {
        selected = row;
        CHECK(row >= 0 && row < model_count);
        menu_rselchanged(model, row);
    }
}
static int32_t currentrow(void *l, int index)
{
    (void)l;
    (void)index;
    return menu_g_setting_row;
}
static int32_t stock_numrows(void *m)
{
    (void)m;
    return model_count;
}
static void stock_rsel(void *m, int row)
{
    (void)m;
    (void)row;
}
static void stock_pane(void *listener, int row)
{
    (void)listener;
    (void)row;
    int32_t focus = FOCUS_SETTING_LIST;
    uint32_t optset = OPTSET_OFF_ON;
    mod_safe_write((uintptr_t)view + VIEW_FOCUS_OFF, &focus, sizeof(focus));
    mod_safe_write((uintptr_t)model + RMODEL_OPTSET_OFF, &optset, sizeof(optset));
    /* Simulate stock reusing its existing strings, which also exercises
     * re-lettering OFF/ON after the longer theme pane. */
    if (!model_count) {
        model_count = 2;
        model_labels[0] = "OFF";
        model_labels[1] = "ON";
    }
}
static void stock_sel(void *m, int row)
{
    (void)m;
    stock_calls++;
    menu_rowchanged(view + LISTENER_VIEW_DELTA, row);
}
int main(void)
{
    uintptr_t mp = (uintptr_t)model, lp = (uintptr_t)list;
    menu_g_view = (uintptr_t)view;
    menu_g_setting_row = 1;
    menu_g_mod_mode = menu_g_strarr_ok = menu_g_setstr_ok = 1;
    memcpy(view + VIEW_RIGHT_MODEL_OFF, &mp, sizeof(mp));
    memcpy(view + VIEW_RIGHT_LIST_OFF, &lp, sizeof(lp));
    bounds(list, 100, 100, 200, 80);
    g_ep122_sym[EP122_RMODEL_SETSTRINGS] = (uintptr_t)setstrings;
    g_ep122_sym[EP122_JUCE_STRARR_DTOR] = (uintptr_t)noop;
    g_ep122_sym[EP122_JUCE_LISTBOX_UPDATE] = (uintptr_t)update;
    g_ep122_sym[EP122_JUCE_COMP_SETBOUNDS] = (uintptr_t)bounds;
    g_ep122_sym[EP122_JUCE_LISTBOX_SELECTROW] = (uintptr_t)selectrow;
    g_ep122_sym[EP122_JUCE_LISTBOX_CURRENTROW] = (uintptr_t)currentrow;
    g_ep122_sym[EP122_JUCE_COMP_REPAINT] = (uintptr_t)noop;
    menu_g_orig_rowchanged = (uintptr_t)stock_pane;
    menu_g_orig_selchanged = (uintptr_t)stock_sel;
    menu_g_orig_rnumrows = (uintptr_t)stock_numrows;
    menu_g_orig_rselchanged = (uintptr_t)stock_rsel;
    T_CASE("scroll previews the newly selected row without a click or a setting write");
    for (int turn = 0; turn < 30; turn++) {
        int row = turn % 3 + 1;
        menu_g_in_input = 1;
        mod_selchanged(model, row);
        menu_g_in_input = 0;
        CHECK_INT(menu_g_setting_row, row);
        CHECK_INT(model_count, rows[row - 1].nvalues);
        CHECK_INT(selected, flags[row - 1]);
        int32_t b[4], focus;
        mod_safe_read((uintptr_t)list + COMP_BOUNDS_OFF, b, sizeof(b));
        mod_safe_read((uintptr_t)view + VIEW_FOCUS_OFF, &focus, sizeof(focus));
        CHECK_INT(b[3], 40 * model_count);
        CHECK_INT(focus, FOCUS_SETTING_LIST);
        for (int i = 0; i < model_count && i < (int)rows[row - 1].nvalues; i++)
            CHECK(!strcmp(model_labels[i], rows[row - 1].values[i]));
        CHECK_INT(saves, 0);
    }
    CHECK_INT(stock_calls, 30);
    CHECK(rebuilds >= 30);
    CHECK_INT(flags[0], 1);
    CHECK_INT(flags[1], 2);
    CHECK_INT(flags[2], 0);
    return t_done("menu_preview");
}
