#!/usr/bin/env bash
# install.sh — set up DisplayLink on an L4T / Tegra (aarch64, kernel 4.9) device.
#
# Open-source parts (evdi source, configs, services) ship in this repo.
# The proprietary DisplayLink userspace (DisplayLinkManager + .spkg firmware +
# libevdi) is NOT redistributed here — you supply it:
#
#   DL_DRIVER_FILE=/path/to/displaylink-driver.(zip|run)   ./scripts/install.sh
#   # or
#   DL_DRIVER_URL=https://.../displaylink-driver.zip       ./scripts/install.sh
#
# For Switch/L4T the known-working aarch64 build comes from the Switchroot /
# theofficialgman community packaging (the kernel here is their build). Stock
# Synaptics DisplayLink is x86-only and will NOT work on aarch64.
#
# Pinned known-good DisplayLinkManager (this machine), for verification:
#   v5.9.184.0  sha256 845d98daedd0f53658a2ff117b20db84790d595e069da356feea8445a83ee23b
set -euo pipefail

EVDI_VERSION="${EVDI_VERSION:-1.14.15}"   # must match the DisplayLinkManager ABI
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
KVER="$(uname -r)"

log(){ printf '\033[1;36m[install]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[install] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run with sudo"
[ "$(uname -m)" = "aarch64" ] || log "WARNING: not aarch64 ($(uname -m)) — this is tuned for L4T aarch64"

log "kernel $KVER, installing evdi $EVDI_VERSION"

# --- prerequisites ---------------------------------------------------------
for b in dkms gcc make; do command -v "$b" >/dev/null || die "missing: $b"; done
[ -d "/lib/modules/$KVER/build" ] || die "kernel headers for $KVER not found (install linux-headers-$KVER)"

# --- evdi via DKMS, pinned version (NOT 'ls -t') ---------------------------
SRC="$REPO_DIR/drivers/evdi/evdi-$EVDI_VERSION"
[ -d "$SRC" ] || die "evdi source $SRC not in repo"
if [ ! -d "/usr/src/evdi-$EVDI_VERSION" ]; then
  cp -a "$SRC" "/usr/src/evdi-$EVDI_VERSION"
fi
# Remove any other evdi versions so nothing can mismatch the DLM ABI.
for d in /usr/src/evdi-*; do
  v="${d#/usr/src/evdi-}"
  [ "$v" = "$EVDI_VERSION" ] && continue
  log "removing mismatched evdi $v"
  dkms remove -m evdi -v "$v" --all 2>/dev/null || true
  rm -rf "$d"
done
dkms status | grep -q "evdi/$EVDI_VERSION" || dkms add -m evdi -v "$EVDI_VERSION"
dkms build  -m evdi -v "$EVDI_VERSION" -k "$KVER"
dkms install -m evdi -v "$EVDI_VERSION" -k "$KVER" --force

# --- proprietary DisplayLink userspace -------------------------------------
if [ ! -x /opt/displaylink/DisplayLinkManager ]; then
  SRCFILE="${DL_DRIVER_FILE:-}"
  if [ -z "$SRCFILE" ] && [ -n "${DL_DRIVER_URL:-}" ]; then
    SRCFILE="$(mktemp --suffix=.dldriver)"
    log "downloading DisplayLink driver"; curl -fL "$DL_DRIVER_URL" -o "$SRCFILE"
  fi
  [ -n "$SRCFILE" ] || die "DisplayLinkManager not installed and no DL_DRIVER_FILE/URL given (see header)"
  log "running DisplayLink vendor installer: $SRCFILE"
  case "$SRCFILE" in
    *.zip) tmp="$(mktemp -d)"; unzip -o "$SRCFILE" -d "$tmp"; run="$(find "$tmp" -name '*.run' | head -1)";
           [ -n "$run" ] || die "no .run inside zip"; chmod +x "$run"; "$run" --noexec || "$run" ;;
    *.run) chmod +x "$SRCFILE"; "$SRCFILE" ;;
    *) die "unknown driver file type: $SRCFILE" ;;
  esac
fi
if command -v sha256sum >/dev/null && [ -x /opt/displaylink/DisplayLinkManager ]; then
  got="$(sha256sum /opt/displaylink/DisplayLinkManager | cut -d' ' -f1)"
  [ "$got" = "845d98daedd0f53658a2ff117b20db84790d595e069da356feea8445a83ee23b" ] \
    && log "DisplayLinkManager matches pinned v5.9.184.0" \
    || log "NOTE: DisplayLinkManager sha256=$got (differs from pinned build)"
fi

# --- pin evdi version in the service via a drop-in (vendor unit used 'ls -t',
#     which selected the mismatched 1.12.0) ---------------------------------
DROPIN_DIR=/etc/systemd/system/displaylink-driver.service.d
mkdir -p "$DROPIN_DIR"
cat > "$DROPIN_DIR/override.conf" <<EOF
[Service]
ExecStartPre=
ExecStartPre=/bin/sh -c 'modprobe evdi || (dkms install -m evdi -v $EVDI_VERSION -k \$(uname -r) && modprobe evdi)'
EOF

# --- reliable crash capture ------------------------------------------------
install -m0755 "$REPO_DIR/scripts/dl-panic-capture.sh"      /usr/local/sbin/dl-panic-capture.sh
install -m0644 "$REPO_DIR/scripts/dl-panic-capture.service" /etc/systemd/system/dl-panic-capture.service

systemctl daemon-reload
systemctl enable dl-panic-capture.service

# --- keep DisplayLinkManager running across unplugs (faster replug) ---------
install -m0644 "$REPO_DIR/configs/99-displaylink.rules" /etc/udev/rules.d/99-displaylink.rules
udevadm control --reload

# --- dl-mirror: mirror the desktop onto the dock (user service) -------------
for p in libdrm-dev libx11-dev libxext-dev libxfixes-dev; do
  dpkg -s "$p" >/dev/null 2>&1 || apt-get install -y "$p"
done
# Build as the invoking user so the repo doesn't end up with root-owned files.
sudo -u "${SUDO_USER:-root}" make -C "$REPO_DIR/mirror" dl-mirror
make -C "$REPO_DIR/mirror" install
systemctl --global enable dl-mirror.service
log "dl-mirror enabled; it starts with the next graphical login"
log "  (now: systemctl --user daemon-reload && systemctl --user start dl-mirror)"

log "done. evdi $EVDI_VERSION installed (loads on next boot)."
log "Reboot with the dock UNPLUGGED, then plug it in to test."
