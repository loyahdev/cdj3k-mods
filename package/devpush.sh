#!/usr/bin/env bash
# devpush.sh - put a fresh build into a modded deck's overlay over SSH.
#
#   ./devpush.sh                    build and push
#   ./devpush.sh --no-build         push what is in build/dev/out/
#   ./devpush.sh --from DIR         push the binaries in DIR
#   ./devpush.sh --host other-deck  default is cdj-real
#
# Extracts /mnt/pdj.tar.gz, replaces mods/ep122_shim.so and mods/stemd_client,
# writes the archive back. That is all it does to the unit.
#
# Reboot the deck afterwards. /home/root is a ramdisk rebuilt from the archive
# at boot, so nothing changes until then.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

HOST=cdj-real
FROM=""
DO_BUILD=1
VERSION=""

need_val() {
    case "${2-}" in
        ''|-*) echo "devpush.sh: $1 needs a value" >&2; exit 2 ;;
    esac
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host)     need_val "$1" "${2-}"; HOST="$2"; shift 2 ;;
        --from)     need_val "$1" "${2-}"; FROM="$2"; DO_BUILD=0; shift 2 ;;
        --version)  need_val "$1" "${2-}"; VERSION="$2"; shift 2 ;;
        --no-build) DO_BUILD=0; shift ;;
        -h|--help)  sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "devpush.sh: unknown option: $1" >&2; exit 2 ;;
    esac
done

SSH=(ssh -o ConnectTimeout=10 -o BatchMode=yes "$HOST")
say() { printf '==> %s\n' "$*"; }

# ------------------------------------------------------------------ build ---
if [[ "$DO_BUILD" == 1 ]]; then
    say "building deck binaries"
    build_args=(--no-pack --out "$HERE/build/dev")
    [[ -n "$VERSION" ]] && build_args+=(--version "$VERSION")
    ./build.sh "${build_args[@]}"
fi
[[ -n "$FROM" ]] || FROM="$HERE/build/dev/out"

SHIM="$FROM/ep122_shim.so"
AGENT="$FROM/stemd_client_aarch64"
[[ -f "$AGENT" ]] || AGENT="$FROM/stemd_client"
for f in "$SHIM" "$AGENT"; do
    [[ -f "$f" ]] || { echo "devpush.sh: not found: $f" >&2; exit 1; }
done

SHIM_SUM=$(shasum -a 256 "$SHIM"  | cut -d' ' -f1)
AGENT_SUM=$(shasum -a 256 "$AGENT" | cut -d' ' -f1)
say "pushing to $HOST"
printf '    shim  %s  %s\n' "${SHIM_SUM:0:12}" "$SHIM"
printf '    agent %s  %s\n' "${AGENT_SUM:0:12}" "$AGENT"

# ----------------------------------------------------------------- upload ---
# Piped through ssh: dropbear serves no sftp-server.
"${SSH[@]}" 'rm -rf /tmp/.devpush && mkdir -p /tmp/.devpush'
"${SSH[@]}" 'cat > /tmp/.devpush/ep122_shim.so' < "$SHIM"
"${SSH[@]}" 'cat > /tmp/.devpush/stemd_client'  < "$AGENT"

# ------------------------------------------------------------------ apply ---
"${SSH[@]}" sh -s -- "$SHIM_SUM" "$AGENT_SUM" <<'REMOTE'
set -eu
SHIM_SUM=$1; AGENT_SUM=$2
NEW=/tmp/.devpush
TAR=/mnt/pdj.tar.gz

sum() { sha256sum "$1" | cut -d' ' -f1; }
cleanup() { rm -rf "${W:-}" "$NEW"; }
trap cleanup EXIT

[ "$(sum $NEW/ep122_shim.so)" = "$SHIM_SUM" ] || { echo "shim corrupted in transit" >&2; exit 1; }
[ "$(sum $NEW/stemd_client)" = "$AGENT_SUM" ] || { echo "agent corrupted in transit" >&2; exit 1; }

[ -f "$TAR" ] || { echo "no $TAR - deck is not modded, flash the .UPD first" >&2; exit 1; }

# Unpacked in tmpfs: the extracted tree never lands on the eMMC.
W=$(mktemp -d /tmp/devpush.XXXXXX)
tar -xzf "$TAR" -C "$W"

# Only mods/ is ours to replace; the rest of the overlay must already be there.
[ -f "$W/mods/ep122_shim.so" ]   || { echo "overlay has no mods/ - flash the .UPD first" >&2; exit 1; }
[ -f "$W/scripts/apl_start.sh" ] || { echo "overlay has no scripts/apl_start.sh" >&2; exit 1; }

if [ "$(sum $W/mods/ep122_shim.so)" = "$SHIM_SUM" ] \
   && [ "$(sum $W/mods/stemd_client)" = "$AGENT_SUM" ]; then
    echo "    overlay already carries this build - nothing written"
    exit 0
fi

cp $NEW/ep122_shim.so "$W/mods/ep122_shim.so"
cp $NEW/stemd_client  "$W/mods/stemd_client"
chmod 0755 "$W/mods/ep122_shim.so" "$W/mods/stemd_client"

# The new archive is the one thing written to /mnt, and it goes in beside the old
# one: rename(2) is atomic only within a filesystem, so an interrupted write
# loses the new archive, not the working one.
tar -czf "$TAR".new -C "$W" .
sync
mv "$TAR".new "$TAR"
sync
echo "    overlay rewritten ($(wc -c < $TAR) bytes)"
REMOTE

say "done - reboot the deck to pick it up"
echo "    ssh $HOST systemctl reboot"
