#!/bin/bash
# phase1.sh - installer step 1 of 2. Runs at the first boot in place of the
# application launcher (staged as the overlay's scripts/apl_start.sh, which
# shadows the real one). Repackages the overlay so phase2 runs next boot.
#
# The shipped binaries live in mods/, not pdj/, so nothing here touches them.
set -euTo pipefail

shopt -s extglob
shopt -s nullglob

UPDATE_MEDIA_DEVICE=
UPDATE_MEDIA_MOUNTPOINT=

while read -r MOUNT; do
    DEVICE=$(echo "$MOUNT" | cut -d' ' -f1)
    MOUNTPOINT=$(echo "$MOUNT" | cut -d' ' -f2)
    if [[ -e $MOUNTPOINT/CDJ3Kv000.UPD ]]; then
        UPDATE_MEDIA_DEVICE=$DEVICE
        UPDATE_MEDIA_MOUNTPOINT=$MOUNTPOINT
        break
    fi
done < /proc/mounts

OVERLAY_MEDIA_MOUNTPOINT=/mnt
MODS_LOG_DIR=$OVERLAY_MEDIA_MOUNTPOINT/cdj3k-mods-logs

# Prefer the stick (readable on a computer), fall back to the overlay partition:
# a phase runs at a reboot, by which time the stick may have been pulled.
#
# Explicit name: these phases run as scripts/apl_start.sh and pdj/EP<n>.
LOG_NAME=phase1
LOG_DIR=

if [[ -n $UPDATE_MEDIA_DEVICE ]] &&
   mount -o remount,rw "$UPDATE_MEDIA_DEVICE" "$UPDATE_MEDIA_MOUNTPOINT" 2>/dev/null; then
    LOG_DIR=$UPDATE_MEDIA_MOUNTPOINT
elif mkdir -p "$MODS_LOG_DIR" 2>/dev/null; then
    LOG_DIR=$MODS_LOG_DIR
fi

if [[ -n $LOG_DIR ]]; then
    exec >"$LOG_DIR"/"$LOG_NAME".log || true
    exec 2>&1

    trap 'printf "[%s] %s: %s\n" "$(cut -d" " -f1 /proc/uptime)" "$LOG_NAME" "${BASH_COMMAND:-}" 1>&2 || true' DEBUG
fi

PDJ_TAR_WORKDIR=$(mktemp -d)
tar -xvzf "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz -C "$PDJ_TAR_WORKDIR"

rm "$PDJ_TAR_WORKDIR"/scripts/apl_start.sh

APP_ORIGINAL=(/home/root/pdj/EP+([[:digit:]]))

# phase2 is staged under the app's exact name to shadow it for one boot, so the
# glob must match. On a miss: log it and leave the deck stock.
if [[ ${#APP_ORIGINAL[@]} -ne 1 ]]; then
    echo "phase1: expected exactly one /home/root/pdj/EP<digits>, found ${#APP_ORIGINAL[@]}:"
    printf '  %s\n' "${APP_ORIGINAL[@]:-<none>}"
    echo "phase1: unsupported firmware layout - aborting and leaving the deck stock"
    tar -czf "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz.new -C "$PDJ_TAR_WORKDIR" .
    sync
    mv "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz.new "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz
    sync

    # Mid-boot: no updater UI for gui_image, so use a console. Best-effort.
    echo 1 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null || true
    openvt -s -- echo '*** cdj3k-mods: unsupported firmware - not installed. Deck runs stock. ***' \
        >/dev/null 2>&1 || true
    if [[ -b /dev/mmcblk1p8 ]]; then
        systemctl reboot
        exit 0
    fi
    exit 1
fi

APP="$PDJ_TAR_WORKDIR"/pdj/$(basename "${APP_ORIGINAL[0]}")
mkdir -p "$(dirname "$APP")"
mv "$PDJ_TAR_WORKDIR"/phase2.sh "$APP"

# Write-then-rename: an interrupted write loses the new archive, not the working
# one. See usb_update.sh.
tar -czf "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz.new -C "$PDJ_TAR_WORKDIR" .
sync
mv "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz.new "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz
sync

if [[ -b /dev/mmcblk1p8 ]]; then
    # Rockchip model
    systemctl reboot
    exit 0
fi

systemctl disable xserver-nodm
systemctl stop xserver-nodm
echo 1 > /sys/class/vtconsole/vtcon1/bind
openvt -s -- echo '*** Phase 1 of 2 complete. Please manually power cycle your CDJ. ***'
sleep 30d
