// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * version.h - which build of the mods this is. Stamped in by guest/Makefile.
 *
 *   EP122_MOD_VERSION  the release tag, with '+' when the build is past it or
 *                      the tree was dirty. Short enough for the glass.
 *   EP122_MOD_BUILD    the full `git describe`, printed once at init.
 *
 * Both fall back to "unknown" without a git checkout. That is the normal case
 * inside the container -- .dockerignore keeps .git out of the context -- so
 * `make docker` passes the host's values in.
 */
#ifndef EP122_MOD_VERSION_H
#define EP122_MOD_VERSION_H

#ifndef EP122_MOD_VERSION
#define EP122_MOD_VERSION "dev"
#endif

#ifndef EP122_MOD_BUILD
#define EP122_MOD_BUILD "dev"
#endif

#endif /* EP122_MOD_VERSION_H */
