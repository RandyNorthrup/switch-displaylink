#!/bin/bash
# mirror-test.sh -- run dl-mirror for N seconds (default 60), then SIGINT it
# to exercise the teardown (crtc disable) path.
# dl-mirror output and the kernel log are written with an fsync per line so
# they survive a hard lockup. Output: /var/log/dl-mirror/<timestamp>.{log,dmesg}
set -u
SECS=${1:-60}
HERE=$(cd "$(dirname "$0")/.." && pwd)
DIR=/var/log/dl-mirror
sudo mkdir -p "$DIR" && sudo chown "$(id -u):$(id -g)" "$DIR"
BASE="$DIR/$(date +%Y%m%d-%H%M%S)"

synced() { while IFS= read -r l; do printf '%s\n' "$l" >>"$1"; sync "$1"; done; }

echo "== $(date -Is) evdi $(/sbin/modinfo -F version evdi) md5 $(md5sum < "$(/sbin/modinfo -n evdi)" | cut -c1-12) ==" >"$BASE.log"
sudo dmesg -w --time-format iso | synced "$BASE.dmesg" &
DMESG=$!

DISPLAY=${DISPLAY:-:0} XAUTHORITY=${XAUTHORITY:-$HOME/.Xauthority} \
  "$HERE/mirror/dl-mirror" 2>&1 | synced "$BASE.log" &
sleep "$SECS"
pkill -INT -x dl-mirror
sleep 3
echo "== dl-mirror exited: $(pgrep -x dl-mirror >/dev/null && echo NO || echo yes) ==" | synced "$BASE.log"
kill "$DMESG" 2>/dev/null; sudo pkill -f 'dmesg -w --time-format iso'
echo "$BASE"
