# DisplayLink on Nintendo Switch (L4T / Tegra X1)

Getting a **Plugable UD-3900PDZ** (DisplayLink, USB ID `17e9:4323`) working as an
external display on a Nintendo Switch running **L4T (Linux for Tegra)**, kernel
`4.9.140-l4t`, aarch64 (Tegra X1).

> Status: driver stack installed and the DisplayLink sink is detected (reads
> monitor EDID). **Blocked** by (1) a kernel panic on USB hotplug caused by an
> evdi/DisplayLinkManager version mismatch, and (2) the Tegra X11 driver not
> supporting RandR PRIME. See [Diagnosis](#diagnosis) and
> [docs/fix-and-mirror-plan.md](docs/fix-and-mirror-plan.md).

## Hardware / software

| Thing | Value |
|---|---|
| Device | Nintendo Switch, hostname `randys-switch` |
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

## Diagnosis

### 1. Kernel panic on unplug/replug — evdi ⇄ DisplayLinkManager version mismatch

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
configs/    the actual /etc/X11 config + the displaylink systemd unit as installed
evidence/   real crash traces + an environment snapshot
scripts/    dl-panic-capture.{sh,service} — reliable crash capture for next time
docs/        fix-and-mirror-plan.md — the actual path forward
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

## Safety

Do **not** reload/`rmmod` evdi or restart `displaylink-driver.service` while
Xorg holds `/dev/dri/card0` — that reproduces the crash and can hard-reset the
box. Unplug the dock first, and work from a session you can afford to lose.
