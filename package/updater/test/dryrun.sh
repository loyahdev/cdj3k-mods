#!/usr/bin/env bash
# dryrun.sh - run the install/runtime/removal scripts against a fake deck in a
# throwaway container, and assert they behave (and that nothing destructive
# touches the stock rootfs). No real hardware, no key, no .UPD needed.
#
#   updater/test/dryrun.sh
#
# Privileged because the simulation makes a real tmpfs mount (for /proc/mounts
# discovery) and a block-device node (for the `[[ -b /dev/mmcblk1p8 ]]` gate).
# Everything happens inside the container; the repo is mounted read-only.
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"

exec docker run --rm -i --privileged -v "$PKG:/pkg:ro" alpine:3.19 sh -euc '
    apk add --no-cache bash tar coreutils >/dev/null 2>&1
    exec bash /pkg/updater/test/sim.sh
'
