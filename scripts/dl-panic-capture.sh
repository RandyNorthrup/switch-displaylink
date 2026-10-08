#!/bin/sh
# dl-panic-capture.sh
# Run early on every boot. Archives anything the kernel left in /sys/fs/pstore
# (console + dmesg ramoops records from a crash) into a timestamped, readable
# folder BEFORE systemd-pstore moves/compresses it and before the ramoops ring
# degrades across subsequent boots.
#
# Why this exists: on this box (Tegra X1 / L4T 4.9) ramoops has no ECC and the
# dmesg backend is zlib-compressed, so hard-reset panics often land as a corrupt
# dmesg-ramoops.enc.z that neither the kernel nor zlib can decompress. The
# *console* ramoops region is plain text, so grabbing it immediately is the most
# reliable trace we get.
set -eu
DEST_ROOT=/var/log/dl-panics
STAMP="$(date +%Y%m%d-%H%M%S)"
SRC=/sys/fs/pstore

[ -d "$SRC" ] || exit 0
# Only create an archive if there's actually something there this boot.
if [ -z "$(ls -A "$SRC" 2>/dev/null)" ]; then
    exit 0
fi

DEST="$DEST_ROOT/$STAMP"
mkdir -p "$DEST"
cp -a "$SRC"/* "$DEST"/ 2>/dev/null || true

# Try to render a human-readable console log alongside the raw copy.
for f in "$DEST"/console-ramoops*; do
    [ -e "$f" ] || continue
    tr -c '[:print:]\n\t' '.' < "$f" > "$DEST/console-readable.txt" 2>/dev/null || true
done

logger -t dl-panic-capture "archived pstore crash records to $DEST"
exit 0
