# Building and testing our kernel

Kernel source: [RandyNorthrup/switch-l4t-kernel-4.9](https://github.com/RandyNorthrup/switch-l4t-kernel-4.9),
a fork of theofficialgman's tree (the one the stock Switchroot Noble kernel is built from).

| Branch | Use |
|---|---|
| `switch-displaylink` | Our working branch: `linux-dev` + our patches. `build-wsl.sh` builds this. |
| `xhci-unplug-lockup` | Kept clean for [theofficialgman/switch-l4t-kernel-4.9#1](https://github.com/theofficialgman/switch-l4t-kernel-4.9/pull/1). |
| `linux-dev` | Upstream, untouched. |

How it works: build on the Windows PC (WSL), publish the build as a GitHub release
on the fork, download it on the Switch, and boot it from a separate hekate entry.
The normal boot entry is never touched.

## One-time setup on the Windows PC

In PowerShell:

```powershell
wsl --install -d Ubuntu-20.04
```

It has to be **20.04**: it's the last Ubuntu that ships the GCC 7 aarch64 cross compiler.
The stock kernel was built with Linaro GCC 7.5, and Linaro's download links are dead now.
Then, inside the Ubuntu-20.04 shell:

```sh
git clone https://github.com/RandyNorthrup/switch-displaylink.git ~/switch-displaylink
```

Keep everything in the Linux filesystem (`~`), not under `/mnt/c`. The kernel build
breaks on the case-insensitive Windows drive, and it's about 10x slower there.

## Build (Windows PC, WSL)

```sh
cd ~/switch-displaylink && git pull
kernel/build-wsl.sh --publish
```

- The first run installs the build tools (it asks for your WSL sudo password) and clones
  about 2 GB of sources into `~/l4t-build`. A full build takes 20-60 min depending on the
  PC. Later runs only rebuild what changed.
- `--publish` installs the GitHub CLI if it's missing. You run `gh auth login` once, the
  first time. Without `--publish`, the output just stays in `~/l4t-build/out/`.
- It always builds the fork's `switch-displaylink` branch as it is on GitHub, so the
  published binary always matches published source (GPL). Any local edits in
  `~/l4t-build/kernel-4.9` are discarded on the next run.
- It uses the stock kernel's own config ([`config-4.9.140-l4t`](config-4.9.140-l4t),
  copied from the Switch's `/proc/config.gz`) and the same release string, so the
  stock modules in `/lib/modules/4.9.140-l4t` still load.

## Test (Switch)

```sh
scripts/test-kernel.sh fetch            # newest build-* release
sudo scripts/test-kernel.sh install     # -> hekate entry "L4T Noble TEST"
```

Reboot, then pick **hekate > More configs > L4T Noble TEST**. Once it's up,
`scripts/test-kernel.sh status` should say "running a TEST kernel", and xusb_bwfix
stays unloaded on it, so the built-in fix is what gets tested.

If it doesn't boot: hold power, then boot the normal **L4T Ubuntu Noble** entry. The
TEST entry returns to hekate's menu on reboot or panic, so it can't boot-loop.
`sudo scripts/test-kernel.sh remove` deletes the entry.

## Limits

- Only the kernel image (`uImage`) is swapped. The device tree, initramfs and modules
  stay stock. A change that touches modules or the device tree needs more than this
  (the release does include `modules.tar.gz` for that later).
- The two kernels share a root filesystem, so DKMS modules (evdi, xusb_bwfix) are
  shared too. That's fine as long as the config and release string stay the same.
