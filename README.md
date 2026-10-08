# DisplayLink on Nintendo Switch (L4T / Tegra X1)

Getting a **Plugable UD-3900PDZ** (DisplayLink, USB ID `17e9:4323`) working as an
external display on a Nintendo Switch running **L4T (Linux for Tegra)**, kernel
`4.9.140-l4t`, aarch64 (Tegra X1).

> **Status (2026-10-07):**
> - **Mirroring works.** [`dl-mirror`](docs/mirror.md) mirrors the Switch screen
>   to the dock tear-free, autostarts at login, and reattaches after a replug.
> - **The evdi hotplug panic is fixed:** the version is aligned to evdi 1.14.15,
>   plus two 4.9 fixes in `drivers/evdi`.
> - **Unplugging the dock is fixed by [`drivers/xusb-bwfix`](drivers/xusb-bwfix)**
>   (loaded by hand with insmod for now). A raw pull while mirroring no longer locks up,
>   and the module re-binds the dead xHCI controller, so a replug enumerates
>   and mirrors again without a reboot (tested 23:32). It was a Tegra xHCI bug,
>   not DisplayLink: a command-timeout handler spun ~18 s with IRQs off.
>   Details: [docs/unplug-crash.md](docs/unplug-crash.md).
> - An extended desktop over PRIME is impossible (`NVIDIA-0 cap 0x0`), so
>   multi-monitor is still to do.

## Hardware / software

| Thing | Value |
|---|---|
| Device | Nintendo **Switch Lite** (`nintendo,hoag`), hostname `randys-switch` — no video out, USB-C is USB 2 only |
| SoC | Tegra X1 (aarch64) |
| Kernel | `4.9.140-l4t` (theofficialgman L4T build #149) |
| GPU X driver | NVIDIA Tegra DDX (`nvidia_drv.so`), internal panel = `DSI-0` @ 1280x720 |
| Display server | Xorg (X11), SDDM |
| Dock | Plugable UD-3900PDZ, DisplayLink `17e9:4323` |
| Userspace | `/opt/displaylink/DisplayLinkManager` **v5.9.184.0** |
| Kernel module | **evdi** (DKMS) — see mismatch below |

## What already works

- `evdi` builds on 4.9 via its `compat49` shim and loads.
- `DisplayLinkManager` runs as `displaylink-driver.service`.
- The dock enumerates and **DisplayLinkManager reads the monitor's EDID** over
  the evdi i2c bus — the DisplayLink *sink* path is good.
- evdi **1.14.15** is installed (matches DLM 5.9.184), and the hotplug panic is gone.
- **Mirroring:** `dl-mirror.service` puts the Switch screen on the dock monitor
  (see [docs/mirror.md](docs/mirror.md)).

## Diagnosis

### 1. Kernel panic on unplug/replug — evdi ⇄ DisplayLinkManager version mismatch (FIXED)

> Fixed by `scripts/fix-evdi-version.sh` (evdi 1.14.15). A *different* unplug
> crash remains in the Tegra xHCI controller: [docs/unplug-crash.md](docs/unplug-crash.md).

`DisplayLinkManager v5.9.184` expects **evdi 1.14.x** (the binary literally logs
`libevdi.so version is not compatible with Dlm`). The loaded/DKMS module is
**evdi 1.12.0** — the `displaylink-driver.service` `ExecStartPre` picks the
newest source dir *by mtime* (`ls -t /usr/src | grep evdi | head -n1`), and
`evdi-1.12.0` was touched after `evdi-1.14.15`, so 1.12.0 keeps winning.

On unplug/replug, DLM's disconnect fails and it re-attaches to the stale evdi
node, then the next frame-damage ioctl frees an already-bad DRM atomic state:

```
evdi: [W] evdi_painter_connect_ioctl: (card0) disconnect failed
evdi: [W] evdi_painter_connect:892 (card0) Double connect - replacing <ptr> with <ptr>
Internal error: Oops ...
LR is at drm_atomic_state_clear+0x1c
Call trace:
  drm_atomic_state_free+0x1c
  drm_mode_dirtyfb_ioctl+0xf0
  drm_ioctl
```

Full traces: [evidence/evdi-crash-traces-2026-09-19.log](evidence/evdi-crash-traces-2026-09-19.log).

Tonight's crashes were *hard resets* (kernel log NUL-truncated right before
`Booting Linux`; pstore record corrupt — ramoops has **no ECC** and uses zlib).
So we rely on the earlier, intact traces, which are the same signature.

### 2. No extended desktop over PRIME — Tegra DDX has no RandR 1.4 capability

`xrandr --listproviders` shows only `NVIDIA-0` with **`cap: 0x0`**. The standard
DisplayLink-on-Linux path (`xrandr --setprovideroutputsource evdi NVIDIA-0`)
needs the primary GPU to advertise *Source Output*; the Tegra driver advertises
nothing. So a unified/extended desktop via PRIME is **not possible** on this
platform. Mirroring and multi-monitor therefore need a different mechanism
(separate X screen or a Wayland compositor) — see the plan doc.

## Repo layout

```
drivers/evdi/        open-source evdi kernel-module source (1.12.0 and 1.14.15),
                      incl. the compat49 shim that makes it build on L4T 4.9,
                      plus our 4.9 fixes (dirtyfb double free, vblank counter)
drivers/xusb-padfix/ unplug-crash experiment; does NOT fix it, kept for reference
drivers/xusb-bwfix/  the unplug-lockup fix (insmod; see docs/unplug-crash.md)
drivers/xusb-otgdefer/ unplug-crash experiment (defer OTG detach); does NOT fix it
drivers/cpu-pcsample/  CoreSight PC sampler; blocked (external debug fused off)
mirror/              dl-mirror (KMS mirror daemon) + its systemd user unit
configs/             /etc/X11 config, displaylink unit, crash-debug sysctls
evidence/            crash traces, hekate panic dumps, test logs
scripts/             install.sh            — full fetch+build+install
                      fix-evdi-version.sh   — targeted panic fix (version align)
                      dock-eject.sh         — run before unplugging (see below)
                      mirror-test.sh        — timed dl-mirror run with dmesg capture
                      unplug-watch.sh       — fsync'd dmesg capture for unplug tests
                      capture-staleness.c   — how stale X root grabs are (compositing test)
                      dl-panic-capture.*    — reliable crash capture
docs/                mirror.md             — how dl-mirror works, tuning notes
                      unplug-crash.md       — the open xHCI unplug crash
                      fix-and-mirror-plan.md — original plan + what was done
```

## Install (open-source here; proprietary driver fetched)

The proprietary DisplayLink userspace is **not** redistributed in this public
repo. Supply it (Switchroot/L4T aarch64 build — stock Synaptics is x86-only):

```sh
sudo DL_DRIVER_FILE=/path/to/displaylink-l4t-driver.zip ./scripts/install.sh
# or DL_DRIVER_URL=https://... ./scripts/install.sh
```

Already have DisplayLinkManager in `/opt/displaylink` and just need the panic
fix? Unplug the dock and run:

```sh
sudo ./scripts/fix-evdi-version.sh   # installs evdi 1.14.15, removes 1.12.0
sudo reboot                          # new module loads cleanly on boot
```

## Reliable crash capture (install on any affected device)

Because hard-reset panics corrupt the compressed pstore dump, archive the raw
(plain-text console) pstore records early on every boot:

```sh
sudo install -m0755 scripts/dl-panic-capture.sh    /usr/local/sbin/dl-panic-capture.sh
sudo install -m0644 scripts/dl-panic-capture.service /etc/systemd/system/dl-panic-capture.service
sudo systemctl daemon-reload
sudo systemctl enable dl-panic-capture.service
```

After the next crash + reboot, the trace is under `/var/log/dl-panics/<timestamp>/`
(see `console-readable.txt`).

## Unplugging the dock

Until the [xHCI unplug crash](docs/unplug-crash.md) is fixed, run this first:

```sh
sudo ./scripts/dock-eject.sh   # then pull the cable
```

Pulling the cable while the dock is active can freeze the Switch for ~20 s
(leaving USB dead until reboot) or hard-lock it. To reattach, plug the cable
back in. A picture returns after ~15 s.

## Safety

Do **not** reload/`rmmod` evdi or restart `displaylink-driver.service` while
Xorg holds `/dev/dri/card0` — that reproduces the crash and can hard-reset the
box. Unplug the dock first, and work from a session you can afford to lose.
