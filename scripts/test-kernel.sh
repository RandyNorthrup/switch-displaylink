#!/usr/bin/env bash
# test-kernel.sh -- try a kernel built by kernel/build-wsl.sh, without touching
# the normal boot entry.
#
#   scripts/test-kernel.sh fetch [TAG]    download a release from the fork (default: newest)
#   sudo scripts/test-kernel.sh install [TAG]
#                                         set up hekate entry "L4T Noble TEST" that boots
#                                         /switchroot/ubuntu-test/ (copy of ubuntu-noble
#                                         with the new uImage), same root filesystem
#   sudo scripts/test-kernel.sh remove    delete the TEST entry and its folder
#   scripts/test-kernel.sh status         which kernel is running
#
# Boot it: hekate > More configs > "L4T Noble TEST". If it doesn't come up, hold
# power, then boot the normal "L4T Ubuntu Noble" entry. The TEST entry reboots to
# hekate's menu (r2p_action=bootloader), so a panic can't loop into it.
#
# The test kernel has the same release string (4.9.140-l4t) and config, so it
# uses the stock /lib/modules. xusb_bwfix is skipped on it (the kernel has the
# fix built in, and the module would replace the patched timeout handler).
set -euo pipefail

FORK="${FORK:-RandyNorthrup/switch-l4t-kernel-4.9}"
STORE=/var/lib/test-kernel
SD_DEV=/dev/disk/by-label/SWITCH\\x20SD
MNT=/mnt/swsd
STOCK=switchroot/ubuntu-noble
TEST=switchroot/ubuntu-test
INI=bootloader/ini/L4T-noble-test.ini
MODPROBE=/etc/modprobe.d/xusb_bwfix-testkernel.conf
MARK='randy@wsl-build'

log(){ printf '\033[1;36m[test-kernel]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[test-kernel] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }
cfg(){ grep -E '^CONFIG_|^# CONFIG_.* is not set' | sort; }
need_root(){ [ "$(id -u)" -eq 0 ] || die "run with sudo"; }

newest_tag(){
  gh release list -R "$FORK" --limit 50 --json tagName,createdAt \
    -q 'map(select(.tagName|startswith("build-")))|sort_by(.createdAt)|last|.tagName'
}

fetch(){
  local tag="${1:-$(newest_tag)}"
  [ -n "$tag" ] && [ "$tag" != null ] || die "no build-* release on $FORK"
  local dir="$STORE/$tag"
  if [ -f "$dir/.ok" ]; then log "$tag already fetched"; echo "$tag"; return; fi
  sudo mkdir -p "$dir"; sudo chown "$(id -u):$(id -g)" "$dir"
  gh release download "$tag" -R "$FORK" -D "$dir" --clobber
  (cd "$dir" && sha256sum -c SHA256SUMS) || die "checksum mismatch in $dir"
  touch "$dir/.ok"
  log "fetched $tag into $dir"
  echo "$tag"
}

mount_sd(){
  mkdir -p "$MNT"
  if mountpoint -q "$MNT"; then mount -o remount,rw "$MNT"; else mount -o rw "$SD_DEV" "$MNT"; fi
}
umount_sd(){ sync; umount "$MNT"; }

install_(){
  need_root
  local tag="${1:-$(ls -t "$STORE" 2>/dev/null | head -1)}"
  local dir="$STORE/$tag"
  [ -n "$tag" ] && [ -f "$dir/.ok" ] || die "fetch a build first (scripts/test-kernel.sh fetch)"
  # The stock modules are reused, so the config has to match the stock kernel's.
  if ! grep -q "$MARK" /proc/version &&
     ! cmp -s <(cfg < "$dir/config") <(zcat /proc/config.gz | cfg); then
    die "$tag's config differs from the running stock kernel; the stock modules may not load"
  fi

  mount_sd
  trap umount_sd EXIT
  [ -f "$MNT/$STOCK/uImage" ] || die "$MNT/$STOCK/uImage missing; is $SD_DEV the hekate SD?"
  mkdir -p "$MNT/$TEST"
  # Everything but the kernel comes from the normal entry, refreshed every time.
  for f in "$MNT/$STOCK"/*; do
    [ "$(basename "$f")" = uImage ] || cp -r "$f" "$MNT/$TEST/"
  done
  cp "$dir/uImage" "$MNT/$TEST/uImage"
  echo "$tag" > "$MNT/$TEST/BUILD.txt"
  cat > "$MNT/$INI" <<EOF
[L4T Noble TEST]
l4t=1
boot_prefixes=/$TEST/
id=SWR-NOB
r2p_action=bootloader
usb3_enable=1
icon=$STOCK/icon_ubuntu_hue.bmp
logopath=$STOCK/bootlogo_ubuntu.bmp
EOF
  cat > "$MODPROBE" <<EOF
# Written by switch-displaylink scripts/test-kernel.sh: our test kernels
# ($MARK in /proc/version) have the unplug fix built in.
install xusb_bwfix grep -q '$MARK' /proc/version || /sbin/modprobe --ignore-install xusb_bwfix
EOF
  log "installed $tag: reboot, then hekate > More configs > \"L4T Noble TEST\""
}

remove(){
  need_root
  mount_sd
  trap umount_sd EXIT
  rm -rf "${MNT:?}/$TEST" "$MNT/$INI"
  rm -f "$MODPROBE"
  log "TEST entry removed"
}

status(){
  cat /proc/version
  if grep -q "$MARK" /proc/version; then echo "running a TEST kernel"; else echo "running the stock kernel"; fi
  lsmod | grep -q '^xusb_bwfix ' && echo "xusb_bwfix loaded" || echo "xusb_bwfix not loaded"
}

case "${1:-}" in
  fetch)   shift; fetch "$@" ;;
  install) shift; install_ "$@" ;;
  remove)  remove ;;
  status)  status ;;
  *) sed -n '2,20p' "$0"; exit 1 ;;
esac
