#!/usr/bin/env bash
# build-wsl.sh -- build the Switch L4T kernel from our fork, on Windows/WSL.
#
# Run inside WSL Ubuntu 20.04 (it's the last Ubuntu that ships the GCC 7
# aarch64 cross compiler; the stock kernel was built with Linaro GCC 7.5, whose
# download is gone):
#
#   ./build-wsl.sh            # build only, output in ~/l4t-build/out/
#   ./build-wsl.sh --publish  # build, then upload as a GitHub release on the fork
#
# The Switch side picks a published build up with scripts/test-kernel.sh.
# Same source layout and flags as theofficialgman/l4t-kernel-build-scripts.
set -euo pipefail

FORK="${FORK:-RandyNorthrup/switch-l4t-kernel-4.9}"
BRANCH="${BRANCH:-switch-displaylink}"
WORK="${WORK:-$HOME/l4t-build}"
CPUS="${CPUS:-$(nproc)}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PUBLISH=0; [ "${1:-}" = "--publish" ] && PUBLISH=1

log(){ printf '\033[1;36m[build]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[build] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

case "$WORK" in /mnt/*) die "WORK=$WORK is on the Windows drive; build in the Linux filesystem (e.g. ~/l4t-build)";; esac
[ "$(uname -m)" = x86_64 ] || die "expected an x86_64 WSL machine"

# --- tools ------------------------------------------------------------------
if ! command -v aarch64-linux-gnu-gcc-7 >/dev/null; then
  apt-cache show gcc-7-aarch64-linux-gnu >/dev/null 2>&1 \
    || die "gcc-7-aarch64-linux-gnu not available: use WSL Ubuntu 20.04 (wsl --install -d Ubuntu-20.04)"
  log "installing build tools"
  sudo apt-get update
  sudo apt-get install -y git make bc bison flex xxd kmod u-boot-tools build-essential \
    libssl-dev python2 python-is-python2 python3 gcc-7-aarch64-linux-gnu
fi

# --- sources (same layout as l4t-linux-build.sh) ------------------------------
mkdir -p "$WORK"; cd "$WORK"
SUBREPOS="nvidia nvgpu hardware/nvidia/platform/t210/nx hardware/nvidia/soc/t210 hardware/nvidia/soc/tegra hardware/nvidia/platform/tegra/common hardware/nvidia/platform/t210/common"
clone() { # dir url branch
  if [ ! -d "$1/.git" ]; then
    git clone -b "$3" --single-branch --depth 50 "$2" "$1"
  else
    git -C "$1" fetch -q --depth 50 origin "$3"
    git -C "$1" reset -q --hard FETCH_HEAD
  fi
}
log "fetching sources"
clone kernel-4.9 "https://github.com/$FORK.git" "$BRANCH"
clone nvidia https://github.com/theofficialgman/switch-l4t-kernel-nvidia.git linux-dev
clone hardware/nvidia/platform/t210/nx https://github.com/theofficialgman/switch-l4t-platform-t210-nx.git linux-dev
clone nvgpu https://gitlab.com/switchroot/kernel/l4t-kernel-nvgpu.git linux-3.4.0-r32.5
clone hardware/nvidia/soc/t210 https://gitlab.com/switchroot/kernel/l4t-soc-t210.git l4t/l4t-r32.5
clone hardware/nvidia/soc/tegra https://gitlab.com/switchroot/kernel/l4t-soc-tegra.git l4t/l4t-r32.5
clone hardware/nvidia/platform/tegra/common https://gitlab.com/switchroot/kernel/l4t-platform-tegra-common.git l4t/l4t-r32.5
clone hardware/nvidia/platform/t210/common https://gitlab.com/switchroot/kernel/l4t-platform-t210-common.git l4t/l4t-r32.5

# --- build --------------------------------------------------------------------
# Same release string as the stock kernel ("4.9.140-l4t", LOCALVERSION= stops
# setlocalversion from adding "+"), so the stock /lib/modules keep loading.
# The build user/host mark our kernels in /proc/version (test-kernel.sh uses it).
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
export KBUILD_BUILD_USER=randy KBUILD_BUILD_HOST=wsl-build
export KCFLAGS="-march=armv8-a+simd+crypto+crc -mtune=cortex-a57 --param=l1-cache-line-size=64 --param=l1-cache-size=32 --param=l2-cache-size=2048"
MK=(make -C kernel-4.9 -j"$CPUS" CC=aarch64-linux-gnu-gcc-7 LOCALVERSION=)

log "configuring (the stock kernel's own config)"
cp "$HERE/config-4.9.140-l4t" kernel-4.9/.config
"${MK[@]}" olddefconfig
REL="$("${MK[@]}" -s kernelrelease)"
[ "$REL" = "4.9.140-l4t" ] || die "kernel release is '$REL', expected 4.9.140-l4t"

log "building kernel + modules ($CPUS jobs)"
"${MK[@]}" tegra-dtstree=../hardware/nvidia

# --- package ------------------------------------------------------------------
SHA="$(git -C kernel-4.9 rev-parse --short=12 HEAD)"
OUT="$WORK/out"; rm -rf "$OUT"; mkdir -p "$OUT"
mkimage -A arm64 -O linux -T kernel -C gzip -a 0x80200000 -e 0x80200000 \
  -n "switch-displaylink-$SHA" -d kernel-4.9/arch/arm64/boot/zImage "$OUT/uImage"
"${MK[@]}" modules_install INSTALL_MOD_PATH="$OUT/modroot" INSTALL_MOD_STRIP=1 >/dev/null
rm -f "$OUT/modroot/lib/modules/$REL/build" "$OUT/modroot/lib/modules/$REL/source"
tar -C "$OUT/modroot/lib" --owner=0 --group=0 -czf "$OUT/modules.tar.gz" modules
rm -rf "$OUT/modroot"
cp kernel-4.9/.config "$OUT/config"
{
  echo "kernel $FORK@$BRANCH $(git -C kernel-4.9 rev-parse HEAD)"
  for d in $SUBREPOS; do echo "$d $(git -C "$d" rev-parse HEAD)"; done
  echo "built $(date -Is) by $(aarch64-linux-gnu-gcc-7 --version | head -1)"
} > "$OUT/SOURCES.txt"
(cd "$OUT" && sha256sum uImage modules.tar.gz config > SHA256SUMS)
log "done: $OUT (uImage, modules.tar.gz, config, SOURCES.txt)"

# --- publish ------------------------------------------------------------------
if [ "$PUBLISH" = 1 ]; then
  if ! command -v gh >/dev/null; then
    log "installing the GitHub CLI (not in Ubuntu 20.04's archive)"
    curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg \
      | sudo dd of=/usr/share/keyrings/githubcli-archive-keyring.gpg status=none
    echo "deb [arch=amd64 signed-by=/usr/share/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" \
      | sudo tee /etc/apt/sources.list.d/github-cli.list >/dev/null
    sudo apt-get update && sudo apt-get install -y gh
  fi
  gh auth status >/dev/null 2>&1 || die "run 'gh auth login' first"
  TAG="build-$(date +%Y%m%d-%H%M)-$SHA"
  gh release create "$TAG" -R "$FORK" --prerelease --target "$(git -C kernel-4.9 rev-parse HEAD)" \
    --title "Test build $TAG" \
    --notes "Test kernel from \`$BRANCH\` @ $SHA. Source: this commit plus SOURCES.txt. Built by kernel/build-wsl.sh in RandyNorthrup/switch-displaylink." \
    "$OUT/uImage" "$OUT/modules.tar.gz" "$OUT/config" "$OUT/SOURCES.txt" "$OUT/SHA256SUMS"
  log "published $TAG; on the Switch: scripts/test-kernel.sh fetch"
fi
