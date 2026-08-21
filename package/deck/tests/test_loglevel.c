// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * test_loglevel.c - the log level grammar (shared/loglevel.h).
 *
 * Both binaries read a level from the environment through this one parser, so
 * what it accepts IS the documented interface for EP122_MOD_LOGLEVEL and
 * STEMD_LOGLEVEL. The cases that matter are the ones that decide how a deck
 * behaves when someone gets it slightly wrong.
 */
#include "loglevel.h"

#include "test.h"

int main(void)
{
    T_CASE("names");
    CHECK_INT(log_level_from("error"), LOG_ERROR);
    CHECK_INT(log_level_from("warn"),  LOG_WARN);
    CHECK_INT(log_level_from("info"),  LOG_INFO);
    CHECK_INT(log_level_from("debug"), LOG_DEBUG);
    CHECK_INT(log_level_from("trace"), LOG_TRACE);

    T_CASE("case-insensitive");
    CHECK_INT(log_level_from("ERROR"), LOG_ERROR);
    CHECK_INT(log_level_from("Debug"), LOG_DEBUG);
    CHECK_INT(log_level_from("TrAcE"), LOG_TRACE);

    T_CASE("digits");
    CHECK_INT(log_level_from("0"), LOG_ERROR);
    CHECK_INT(log_level_from("3"), LOG_DEBUG);
    CHECK_INT(log_level_from("4"), LOG_TRACE);
    /* Above the top level is not a mistake worth refusing: someone reaching for
     * "as loud as it goes" gets it. */
    CHECK_INT(log_level_from("9"), LOG_TRACE);

    T_CASE("unset is the default, not an error");
    CHECK_INT(log_level_from(NULL), LOG_ERROR);
    CHECK_INT(log_level_from(""),   LOG_ERROR);
    CHECK_INT(log_level_from("  "), LOG_ERROR);

    T_CASE("leading blanks");
    CHECK_INT(log_level_from(" debug"),   LOG_DEBUG);
    CHECK_INT(log_level_from("\tinfo"),   LOG_INFO);
    CHECK_INT(log_level_from("  warn  "), LOG_WARN);

    /* The whole point of the -1: a value that was meant to select something and
     * did not is REPORTED, rather than reading as the quietest setting and
     * looking like the variable does nothing. */
    T_CASE("unrecognised is -1, never a level");
    CHECK_INT(log_level_from("verbose"), -1);
    CHECK_INT(log_level_from("quiet"),   -1);
    CHECK_INT(log_level_from("on"),      -1);
    CHECK_INT(log_level_from("1=true"),  -1);
    CHECK_INT(log_level_from("-1"),      -1);
    CHECK_INT(log_level_from("10"),      -1);   /* two digits is not a level */

    T_CASE("whole word only");
    CHECK_INT(log_level_from("infomercial"), -1);
    CHECK_INT(log_level_from("debugger"),    -1);
    CHECK_INT(log_level_from("warning"),     -1);
    CHECK_INT(log_level_from("traceroute"),  -1);
    CHECK_INT(log_level_from("err"),         -1);   /* a prefix is not the name */

    return t_done("loglevel");
}
