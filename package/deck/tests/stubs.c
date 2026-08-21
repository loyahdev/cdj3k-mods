// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stubs.c - the shim state the tested sources read.
 *
 * These live in mods/common.c and mods/draw.c on the deck, which reach the
 * settings file, the resolver and JUCE. The definitions here are what let
 * roles.c and presets.c link into a host test binary; the tests drive them
 * directly. Nothing in PURE_SRCS needs any of it -- a source that does is
 * impure by definition and `make purity` rejects it.
 */
#include "mods/core/mod_core.h"
#include "mods/juce/draw.h"
#include "mods/theme/theme.h"

int g_mod_log = MOD_LOG_ERROR;
int g_theme_id;

void mod_draw_ground(int light)
{
    (void)light;
}
