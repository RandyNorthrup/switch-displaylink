#!/usr/bin/env bash
# fix-evdi-version.sh — the targeted panic fix for THIS machine.
#
# Aligns the evdi kernel module to the version DisplayLinkManager 5.9.184 expects
# (1.14.x) so the "disconnect failed -> Double connect -> drm_mode_dirtyfb_ioctl"
# use-after-free panic on hotplug stops.
#
# SAFE BY DESIGN: it DKMS-installs 1.14.15 and removes 1.12.0, but does NOT
# rmmod/reload the live module. The new module loads on the next clean reboot.
# Live-unloading evdi while Xorg holds /dev/dri/card0 is exactly what crashes
# the box, so we don't.
#
# Run it with the dock UNPLUGGED, then reboot, then plug the dock in.
set -euo pipefail
EVDI_VERSION=1.14.15
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
KVER="$(uname -r)"
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

echo "[fix] staging evdi $EVDI_VERSION source"
[ -d "/usr/src/evdi-$EVDI_VERSION" ] || cp -a "$REPO_DIR/drivers/evdi/evdi-$EVDI_VERSION" "/usr/src/evdi-$EVDI_VERSION"

echo "[fix] building + installing evdi $EVDI_VERSION (does not touch the loaded module)"
dkms status | grep -q "evdi/$EVDI_VERSION" || dkms add -m evdi -v "$EVDI_VERSION"
dkms build   -m evdi -v "$EVDI_VERSION" -k "$KVER"
dkms install -m evdi -v "$EVDI_VERSION" -k "$KVER" --force

echo "[fix] removing mismatched evdi versions"
for d in /usr/src/evdi-*; do
  v="${d#/usr/src/evdi-}"; [ "$v" = "$EVDI_VERSION" ] && continue
  dkms remove -m evdi -v "$v" --all 2>/dev/null || true
  rm -rf "$d"
done

echo "[fix] pinning evdi version in the DisplayLink service (was 'ls -t', which chose 1.12.0)"
# Use a systemd drop-in rather than editing the vendor unit: reset the
# mtime-based ExecStartPre (the empty assignment) and add a pinned one.
DROPIN_DIR=/etc/systemd/system/displaylink-driver.service.d
mkdir -p "$DROPIN_DIR"
cat > "$DROPIN_DIR/override.conf" <<EOF
[Service]
ExecStartPre=
ExecStartPre=/bin/sh -c 'modprobe evdi || (dkms install -m evdi -v $EVDI_VERSION -k \$(uname -r) && modprobe evdi)'
EOF
systemctl daemon-reload

echo
echo "[fix] DONE. Current loaded module is still the old one:"
echo "       $(cat /sys/module/evdi/version 2>/dev/null || echo 'evdi loaded')"
echo "[fix] Now: (1) unplug the dock, (2) sudo reboot, (3) plug the dock in."
echo "[fix] After reboot verify:  cat /sys/module/evdi/version   # expect $EVDI_VERSION"
