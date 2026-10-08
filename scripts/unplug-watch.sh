#!/bin/bash
# unplug-watch.sh -- run (as root) before an unplug test.
# Streams the kernel log to disk with an fsync per line, so the last lines
# before a freeze survive even a power-button hold (which wipes ramoops).
# Output: /var/log/dl-unplug/<timestamp>.log
set -u
DIR=/var/log/dl-unplug
mkdir -p "$DIR"
OUT="$DIR/$(date +%Y%m%d-%H%M%S).log"
{
  echo "== $(date -Is) evdi $(modinfo -F version evdi) =="
  fuser -v /dev/dri/card0 2>&1
  lsusb | grep -i 17e9
} >"$OUT"
sync "$OUT"
echo "logging to $OUT -- unplug when ready (Ctrl-C to stop)"
dmesg -w --time-format iso | while IFS= read -r line; do
  printf '%s\n' "$line" >>"$OUT"
  sync "$OUT"
done
