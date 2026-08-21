/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * mod_platform.h - the libc surface every mod builds against.
 *
 * The mods are a package of their own and link into a host shim that owns the
 * process plumbing. Nothing here names that host; the symbols crossing between
 * them are in cdj3k_mods.h.
 */
#ifndef EP122_MOD_PLATFORM_H
#define EP122_MOD_PLATFORM_H

/* g++ predefines this; gcc does not. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <poll.h>
#include <sched.h>

#endif /* EP122_MOD_PLATFORM_H */
