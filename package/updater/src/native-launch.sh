#!/bin/bash
# EP122 launcher retaining the exact Gate Cue companion only.
# PRE-STEMS is implemented in the shim; the recovered stems DSO is not loaded.
# The companion is attempted only on its two exact EP122 builds and exact DSO hashes.
# Any non-zero EP122 exit permanently disables the Gate companion for subsequent
# service restarts and falls back to the ordinary shim-only launch.
set -u

EP_DIR=/home/root/pdj
EP_PATH=$EP_DIR/EP122
MOD_DIR=/home/root/mods
SHIM=$MOD_DIR/ep122_shim.so
PREUI=$MOD_DIR/preui.so
CONTROL=/mnt/cdj3k-mods-native
DISABLED=$CONTROL/disabled

RK_EP=33c093bdc4fbdaeb191942fa39fe1ca5ca8426440981b93d785d517934af52bc
R8_EP=ae5ce5dcb007bbc9cf24b482959c3fc26c7cdb7f6d011ac04eaed71bd259dbe5
RK_PREUI=730e1fbb25e3b2151fc97d08724d0f97a3bb178501db44596cf582d391b90415
R8_PREUI=49a1f650fd0e0abc9a30b85e45ab769474725b0b14fd3c108476ff9e07deb3a2
ARM=overcue-physical-arm-once:ep122-3.22:15034:r50-shared-beta

hash_of() { /usr/bin/sha256sum "$1" 2>/dev/null | /usr/bin/awk '{print $1}'; }

shim_only()
{
    cd "$EP_DIR" || exit 127
    unset EP122_NATIVE_OVERCUE
    LD_PRELOAD="$SHIM" exec "$EP_PATH"
}

[ ! -e "$DISABLED" ] || shim_only
[ -f "$EP_PATH" ] && [ -f "$SHIM" ] && [ -f "$PREUI" ] || shim_only

EP_HASH=$(hash_of "$EP_PATH") || shim_only
PREUI_HASH=$(hash_of "$PREUI") || shim_only
case "$EP_HASH" in
    "$RK_EP") [ "$PREUI_HASH" = "$RK_PREUI" ] || shim_only ;;
    "$R8_EP") [ "$PREUI_HASH" = "$R8_PREUI" ] || shim_only ;;
    *) shim_only ;;
esac

# Observe a companion crash and retain v7's one-attempt fallback. glibc runs
# the Gate companion constructor before the shim. Only Gate hooks are excluded
# by EP122_NATIVE_OVERCUE; PRE-STEMS now uses the shared source hooks.
(
    export OVERCUE_EP122_SHA256="$EP_HASH"
    export EP122_NATIVE_OVERCUE=1
    export GATECUE_NATIVE_ENABLE=1 GATECUE_NATIVE_LOG=1
    export GATECUE_PHYSICAL_ARM_ONCE="$ARM"
    export STEMS_NATIVE_ENABLE=0 STEMS_NATIVE_LOG=0
    export LD_PRELOAD="$SHIM:$PREUI"
    cd "$EP_DIR" || exit 127
    exec "$EP_PATH"
) &
child=$!
wait "$child"
status=$?
[ "$status" -eq 0 ] && exit 0

# Fail closed after one bad native launch.  This survives the service restarting
# apl_start.sh, which is what prevents a bad test build from becoming a bootloop.
/bin/mkdir -p "$CONTROL" 2>/dev/null || true
/usr/bin/printf '%s\n' "Gate companion disabled after EP122 exit $status" > "$DISABLED" 2>/dev/null || true
/bin/sync
shim_only
