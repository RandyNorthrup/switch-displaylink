# DisplayLink docks on the Nintendo Switch (L4T Ubuntu)

Use a USB **DisplayLink** dock or adapter to put the Switch's Linux desktop on an
external monitor: **mirrored**, or as an **extended desktop** across the Switch
screen plus one or two monitors. For [Switchroot](https://switchroot.org/)
L4T Ubuntu (kernel `4.9.140-l4t`, Tegra X1, aarch64).

It was built for the **Switch Lite**, which has no video output at all. DisplayLink
only needs plain USB, so this is the way to get a monitor on a Lite.

Stock DisplayLink doesn't work on this kernel out of the box. The kernel is too old
for evdi, the Tegra GPU driver can't hand displays to another GPU, and unplugging
the dock froze the Switch. This repo fixes or works around each of those problems.

## Contents

- [What works](#what-works)
- [Will it work on my setup?](#will-it-work-on-my-setup)
- [Install](#install)
- [Using it](#using-it)
- [Troubleshooting](#troubleshooting)
- [Uninstall](#uninstall)
- [How it works](#how-it-works)
- [The kernel fork](#the-kernel-fork)
- [Repo layout](#repo-layout)
- [Reporting a problem](#reporting-a-problem)
- [Credits and license](#credits-and-license)

## What works

| Feature | Status | Notes |
|---|---|---|
| Mirror the Switch screen to a monitor | ✅ Works | Tear-free, starts at login, comes back after a replug. [docs/mirror.md](docs/mirror.md) |
| Extended desktop (Switch + 1–2 monitors) | ✅ Works | KDE Plasma on X11, monitors in any left/right order. [docs/extend.md](docs/extend.md) |
| Hot-unplugging the dock | ✅ Works | Needs the `xusb_bwfix` module (installed by `install.sh`). Without it, unplugging can freeze the Switch. [docs/unplug-crash.md](docs/unplug-crash.md) |
| Charging through the dock | ✅ Works | Only if the dock is powered up *before* you connect it. [docs/charging.md](docs/charging.md) |
| Crash-free replug (evdi) | ✅ Fixed | evdi version aligned to DisplayLinkManager. [docs/evdi-panic.md](docs/evdi-panic.md) |
| Wayland | ❌ No | X11 (Xorg) only. |
| GPU-accelerated output to the dock | ❌ No | The Tegra X driver has no PRIME, so the picture is a CPU copy of the screen. Fine for desktop use; it costs about 23% of one core at 60 fps. |
| 1080p on the dock | ⚠️ Not recommended | A Switch Lite's USB-C port is USB 2 (480 Mbps). 720p sent to a 1080p monitor looks much better than 1080p sent over USB 2. |
| Horizon / Atmosphère (no Linux) | 🚧 Research | Protocol notes only, no working driver: [docs/atmosphere.md](docs/atmosphere.md) |

**Plug-in to picture takes about 15 seconds.** Most of that is DisplayLinkManager
talking to the dock, not something this repo can speed up.

## Will it work on my setup?

**Tested on:**

| | |
|---|---|
| Device | Nintendo Switch Lite (`nintendo,hoag`) |
| OS | Switchroot L4T Ubuntu 24.04 (Noble), KDE Plasma 5.27 on X11, SDDM |
| Kernel | `4.9.140-l4t` (theofficialgman L4T build #149) |
| GPU X driver | NVIDIA Tegra DDX (`nvidia_drv.so`), internal panel `DSI-0` at 1280x720 |
| Dock | [Plugable UD-3900PDZ](https://plugable.com/) (DisplayLink DL-6950, USB ID `17e9:4323`) |
| DisplayLink userspace | DisplayLinkManager **5.9.184.0** (`sha256 845d98da…e83ee23b`) |
| Monitors | 1920x1080 monitors, one or two at a time |

**Should work, but untested** (please [report](#reporting-a-problem) how it goes):

- **Other DisplayLink docks and adapters.** Everything chip-specific is in
  DisplayLinkManager, which supports most DisplayLink chips (DL-3xxx, DL-5xxx, DL-6xxx). Check yours
  with `lsusb | grep -i 17e9`.
- **Other Switch models** (original, Mariko, OLED) on the same L4T kernel. Those
  already have video out through the official dock, so there DisplayLink is mainly
  useful for extra monitors.
- **Other desktops on X11.** Mirroring only needs X11. The extended desktop's
  window placement is built for KDE (KWin); other window managers will get the
  monitor regions through RandR 1.5 monitors only.

**Won't work:** Wayland sessions, Android/LineageOS on the Switch, and kernels
other than 4.9 L4T without changes (the `xusb_bwfix` module is built against this
kernel's xHCI internals).

## Install

> **Before you start:** during development, the bugs fixed here caused hard
> freezes and resets. The fixes are solid on the tested setup, but have a
> backup of anything important, and do the first test from a session you can
> afford to lose.

### 1. Prerequisites

```sh
uname -r                                  # must be 4.9.140-l4t
ls /lib/modules/$(uname -r)/build         # kernel headers (switch-bsp provides them)
sudo apt install dkms build-essential git unzip curl
```

`install.sh` installs the remaining build dependencies itself (`libdrm-dev`,
`libx11-dev`, `libxext-dev`, `libxfixes-dev`, `libxcb-randr0-dev`, `libxi-dev`).

### 2. Get the DisplayLink driver

The proprietary DisplayLinkManager can't be redistributed here. Download
**"DisplayLink USB Graphics Software for Ubuntu"** from
[Synaptics](https://www.synaptics.com/products/displaylink-graphics/downloads/ubuntu).
Releases 5.6 and later include aarch64 binaries. Keep the `.zip` (or the `.run`
inside it).

The tested DisplayLinkManager is 5.9.184.0. A newer one may expect a newer evdi
than 1.14.15; if its log says `libevdi.so version is not compatible with Dlm`,
see [Troubleshooting](#troubleshooting).

### 3. Run the installer

```sh
git clone https://github.com/RandyNorthrup/switch-displaylink.git
cd switch-displaylink
sudo DL_DRIVER_FILE=/path/to/DisplayLink-USB-Graphics-Software-for-Ubuntu.zip ./scripts/install.sh
```

`DL_DRIVER_URL=https://...` works instead of `DL_DRIVER_FILE`. If
DisplayLinkManager is already in `/opt/displaylink`, neither is needed.

### 4. Reboot and plug in

1. Reboot **with the dock unplugged**.
2. Power the dock up first (power adapter in, wait ~15 s), then connect it to the
   Switch. This order is what makes the dock charge the Switch instead of the other
   way round ([why](docs/charging.md)).
3. Wait ~15 s. The Switch screen appears on the monitor.

### What the installer changes

So you know what's on your system and can undo it:

| What | Where |
|---|---|
| evdi 1.14.15 kernel module (DKMS); **other evdi versions are removed** | `/usr/src/evdi-1.14.15` |
| `xusb_bwfix` kernel module (DKMS), loaded at boot | `/usr/src/xusb_bwfix-1.0`, `/etc/modules-load.d/xusb_bwfix.conf` |
| DisplayLinkManager (vendor installer), if not already there | `/opt/displaylink`, `displaylink-driver.service` |
| Service drop-in that loads the pinned evdi | `/etc/systemd/system/displaylink-driver.service.d/override.conf` |
| Keeps Xorg from grabbing the evdi card | `/etc/X11/xorg.conf.d/20-dl-noautogpu.conf` |
| Crash capture at boot | `/usr/local/sbin/dl-panic-capture.sh`, `dl-panic-capture.service` |
| Mirror daemon, user service enabled for all users | `/usr/local/bin/dl-mirror`, `/etc/systemd/user/dl-mirror.service` |
| Extended-desktop tools | `/usr/local/bin/dl-extend`, `/usr/local/bin/dl-edgewrap`, `/usr/local/lib/dl-fakerandr.so`, `/etc/xdg/plasma-workspace/env/dl-extend.sh` |

> **Note:** the tested Switch was set up step by step during development. A
> from-scratch run of `install.sh` on a fresh image hasn't been done yet. If it
> trips over something, please [open an issue](#reporting-a-problem) with the
> output.

## Using it

### Mirror (default)

Nothing to do: `dl-mirror` starts at login, waits for the dock, and mirrors the
Switch screen at 720p. It goes back to waiting when the dock is unplugged.

```sh
systemctl --user status dl-mirror                 # is it running?
journalctl _SYSTEMD_USER_UNIT=dl-mirror.service   # its log
```

Options (edit the unit's `ExecStart`): `-f fps` (default 60), `-m WxH` (force a
mode), `-C` (no cursor), `-K` (keep KWin compositing on; makes the mirror lag).
Details in [docs/mirror.md](docs/mirror.md).

### Extended desktop

With the monitor(s) connected:

```sh
dl-extend detect                       # save the attached monitors to ~/.config/dl-extend.conf
dl-extend identify                     # show each screen's name on it
dl-extend arrange DOCK2 PANEL DOCK1    # your physical left-to-right order; applies live
```

Switch modes any time: `dl-extend mirror`, `dl-extend extend`, or `dl-extend off`
(forget the layout). The layout is read at login, so monitors added later need
`dl-extend detect` again. Details and known quirks in [docs/extend.md](docs/extend.md).

### Unplugging and replugging

With `xusb_bwfix` loaded (the installer does this), just pull the cable, even while
mirroring. On replug, the picture comes back after ~15 s. To replug *and* charge,
power-cycle the dock before connecting it ([docs/charging.md](docs/charging.md)).

Without `xusb_bwfix`, run `sudo ./scripts/dock-eject.sh` before pulling the cable.

### Don't do this

Don't `rmmod`/reload evdi or restart `displaylink-driver.service` while Xorg is
running and the dock is connected. That reproduces the old crash and can
hard-reset the Switch. Unplug the dock first.

## Troubleshooting

| Problem | What to check |
|---|---|
| **No picture** | Wait 15 s after plug-in. Then: `lsusb \| grep -i 17e9` (dock seen?), `systemctl status displaylink-driver` (DLM running?), `cat /sys/class/drm/card*-*/status` (monitor `connected`?), `journalctl _SYSTEMD_USER_UNIT=dl-mirror.service`. |
| **`libevdi.so version is not compatible with Dlm`** | evdi and DisplayLinkManager versions don't match. `dkms status` should show only `evdi/1.14.15`. Unplug the dock and run `sudo ./scripts/fix-evdi-version.sh`, then reboot. If you use a newer DLM, it may need a newer evdi; [open an issue](#reporting-a-problem). |
| **Switch battery drains while docked** | The Switch is powering the dock. Unplug both, plug in the dock's power, wait 15 s, *then* connect the Switch. [docs/charging.md](docs/charging.md) |
| **Freeze or dead USB after unplugging** | `lsmod \| grep xusb_bwfix` should show the module. If not: `sudo modprobe xusb_bwfix`, and check `/etc/modules-load.d/xusb_bwfix.conf`. |
| **Mirror lags, or dialogs only appear when the mouse moves** | Don't run `dl-mirror` with `-K`: KWin compositing makes screen grabs stale. dl-mirror suspends it automatically while mirroring. |
| **Picture looks soft or stutters** | Stay at 720p (the default). Turn on the monitor's game/low-latency mode. A Switch Lite is limited to USB 2. |
| **Broke after a kernel/BSP update** | DKMS should rebuild evdi and `xusb_bwfix`. If `dkms status` shows them missing for the new kernel, run `install.sh` again. |
| **Windows stuck on one screen in extended mode** | Run `dl-extend detect` and `dl-extend arrange ...` again, or log out and back in. Known limits are listed in [docs/extend.md](docs/extend.md#status-and-gotchas). |

## Uninstall

Unplug the dock first, then:

```sh
systemctl --user stop dl-mirror.service
sudo systemctl --global disable dl-mirror.service
sudo systemctl disable dl-panic-capture.service
sudo rm -f /usr/local/bin/{dl-mirror,dl-extend,dl-edgewrap} /usr/local/lib/dl-fakerandr.so \
  /etc/xdg/plasma-workspace/env/dl-extend.sh /etc/systemd/user/dl-mirror.service \
  /usr/local/sbin/dl-panic-capture.sh /etc/systemd/system/dl-panic-capture.service \
  /etc/modules-load.d/xusb_bwfix.conf /etc/X11/xorg.conf.d/20-dl-noautogpu.conf
sudo rm -rf /etc/systemd/system/displaylink-driver.service.d
sudo dkms remove -m xusb_bwfix -v 1.0 --all
sudo dkms remove -m evdi -v 1.14.15 --all
sudo reboot
```

DisplayLinkManager itself (`/opt/displaylink`, `displaylink-driver.service`) came
from the vendor installer; remove it per Synaptics' instructions.

## How it works

The short version. Each part has a doc with the details and the evidence.

1. **evdi on kernel 4.9.** evdi is the open-source kernel half of DisplayLink: it
   creates a virtual DRM card that DisplayLinkManager reads frames from. Upstream
   evdi supports 4.15 and later. [`drivers/evdi`](drivers/evdi) adds a 4.9
   compatibility shim plus fixes for a double free, a NULL vblank counter and a
   NULL `.dpms`. The version is pinned to match DisplayLinkManager, which fixed a
   replug panic. → [docs/evdi-panic.md](docs/evdi-panic.md)
2. **Mirroring without PRIME.** Normally X would render to the evdi card through
   RandR PRIME, but the Tegra X driver advertises no PRIME capabilities
   (`xrandr --listproviders` shows `NVIDIA-0 cap 0x0`). So
   [`dl-mirror`](mirror/dl-mirror.c) copies the X screen into the evdi card itself
   over KMS: changed rows only, double-buffered, page-flipped, so it doesn't tear.
   → [docs/mirror.md](docs/mirror.md)
3. **Extended desktop.** The X screen is made wider than the Switch panel, one
   `dl-mirror` shows each monitor's region, and an `LD_PRELOAD` RandR shim
   ([`fakerandr`](fakerandr/dl-fakerandr.c)) tells KWin and Qt that the extra
   monitors exist. → [docs/extend.md](docs/extend.md)
4. **Unplug freeze.** Not a DisplayLink bug: when a USB device vanished, the Tegra
   xHCI firmware never completed an endpoint-drop command, and the kernel's
   timeout handler then spun for ~18 s with interrupts off. The
   [`xusb_bwfix`](drivers/xusb-bwfix) module skips that command for a device that's
   already gone, and rebinds the controller if it dies anyway.
   → [docs/unplug-crash.md](docs/unplug-crash.md)
5. **Charging.** Whether the dock charges the Switch is decided by the Switch's
   USB-PD controller at the moment the cable connects, so plug-in order matters.
   → [docs/charging.md](docs/charging.md)

## The kernel fork

`xusb_bwfix` is an out-of-tree module, which is the easy way to get the unplug fix.
The proper fix is 4 xHCI patches to the kernel itself:

- **Source:** [RandyNorthrup/switch-l4t-kernel-4.9, branch `switch-displaylink`](https://github.com/RandyNorthrup/switch-l4t-kernel-4.9/tree/switch-displaylink)
  (the fork's default branch, `linux-dev`, is upstream and untouched)
- **Patch files:** [upstream/kernel](upstream/kernel), with notes in [upstream/README.md](upstream/README.md)
- **Upstream PR:** [theofficialgman/switch-l4t-kernel-4.9#1](https://github.com/theofficialgman/switch-l4t-kernel-4.9/pull/1) (open)
- **Build and boot it:** [kernel/README.md](kernel/README.md). It builds on a
  Windows PC under WSL and boots from a separate hekate entry, so the normal boot
  entry isn't touched.

The patched kernel hasn't been booted yet. The same logic is what `xusb_bwfix`
runs, and that's what was tested. For everyday use, stick with `install.sh`.

## Repo layout

```
drivers/evdi/            evdi kernel module source (1.14.15 is the one used; 1.12.0 kept
                         for reference), with the compat49 shim and the 4.9 fixes
drivers/xusb-bwfix/      the unplug-lockup fix (DKMS module, loads at boot)
drivers/xusb-padfix/     unplug-crash experiment; does NOT fix it, kept for reference
drivers/xusb-otgdefer/   unplug-crash experiment (defer OTG detach); does NOT fix it
drivers/cpu-pcsample/    CoreSight PC sampler; blocked (external debug is fused off)
mirror/                  dl-mirror (KMS mirror daemon) and its systemd user unit
extend/                  dl-extend (extended desktop layout/session/run) and dl-edgewrap
fakerandr/               dl-fakerandr.so: LD_PRELOAD xcb-randr shim so KWin/Qt see the monitors
configs/                 20-dl-noautogpu.conf (installed), displaylink unit and drop-in,
                         crash-debug sysctls; xorg.conf and 10-monitor.conf are the stock
                         switch-bsp files, kept for reference
scripts/                 install.sh            full build + install
                         fix-evdi-version.sh   align evdi with DisplayLinkManager (panic fix)
                         dock-eject.sh         safe unplug when xusb_bwfix isn't loaded
                         mirror-test.sh        timed dl-mirror run with dmesg capture
                         unplug-watch.sh       fsync'd dmesg capture for unplug tests
                         test-kernel.sh        fetch a kernel build, boot it as hekate entry "L4T Noble TEST"
                         capture-staleness.c   measures how stale X root grabs are
                         dl-panic-capture.*    keeps crash logs readable across a reset
kernel/                  build the kernel fork on Windows/WSL, plus the stock kernel config
upstream/                the kernel patches, as files
xorg-test/               isolated second X server on an evdi card (test rig)
evidence/                crash traces, hekate panic dumps, test logs
docs/                    mirror.md        how dl-mirror works, tuning notes
                         extend.md        extended desktop
                         charging.md      dock not charging: USB-PD role race at plug-in
                         unplug-crash.md  the xHCI unplug freeze (fixed by xusb_bwfix)
                         evdi-panic.md    the evdi replug panic and the 4.9 evdi fixes
                         atmosphere.md    driving the dock from Atmosphère (research)
                         fix-and-mirror-plan.md  the original plan, for history
```

## Reporting a problem

Please [open an issue](https://github.com/RandyNorthrup/switch-displaylink/issues/new/choose).
It helps a lot to include:

- Switch model, L4T image and version, `uname -r`
- Dock model and `lsusb` line
- `sha256sum /opt/displaylink/DisplayLinkManager` and `dkms status`
- `journalctl -b -u displaylink-driver` and `journalctl _SYSTEMD_USER_UNIT=dl-mirror.service`
- `sudo dmesg` after the problem

### Capturing a crash for a bug report

If the Switch freezes or resets, the normal logs are usually lost. `install.sh`
enables `dl-panic-capture.service`, which saves the raw kernel console from the
previous boot (pstore) early on every boot. After a crash and reboot, look in
`/var/log/dl-panics/<timestamp>/console-readable.txt`.

For more detail, [`configs/99-dl-crash-debug.conf`](configs/99-dl-crash-debug.conf)
turns hangs into panics with backtraces. Install it to `/etc/sysctl.d/` and run
`sudo sysctl --system`. A panic then reboots by itself after 5 s; don't hold the
power button, because a cold power-off wipes the log.

## Credits and license

- [Switchroot](https://switchroot.org/) and [theofficialgman](https://github.com/theofficialgman)
  for L4T Ubuntu and the kernel this builds on.
- [DisplayLink/evdi](https://github.com/DisplayLink/evdi), the open-source kernel module
  this repo patches.
- DisplayLinkManager is proprietary Synaptics software and isn't included.

Code in this repo is [MIT](LICENSE), except the kernel code: the evdi module, the
`xusb_*` modules and the kernel patches are GPL-2.0, as marked in their sources.
