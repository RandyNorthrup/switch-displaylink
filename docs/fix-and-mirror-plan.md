# Fix & mirror plan

Two independent problems. Fix the panic first (stability), then get a picture
(mirror), then multi-monitor.

## Part A — stop the panic (version alignment)

The crash is an evdi ⇄ DisplayLinkManager ABI mismatch. DLM 5.9.184 wants evdi
1.14.x; the loaded module is 1.12.0. Two ways to make them agree:

### Option A1 — match evdi to DLM 5.9.184  ✅ CONFIRMED VIABLE, PREFERRED
**`evdi-1.14.15` compiles cleanly on `4.9.140-l4t`** via its `compat49/` shim
(verified: out-of-tree build `make -C /lib/modules/4.9.140-l4t/build M=<src>
modules` → exit 0, 3.2 MB `evdi.ko`). So we align to the 1.14.x the DLM expects:
1. Build + DKMS-install evdi 1.14.15, and **remove 1.12.0** so nothing can
   mismatch. `scripts/install.sh` does this with a pinned version.
2. Fix the fragile selector in `displaylink-driver.service` `ExecStartPre` — it
   chose evdi by mtime (`ls -t | head`), which is how 1.12.0 kept winning. The
   installer rewrites it to a pinned `dkms install -m evdi -v 1.14.15`.

Apply on *this* machine with `scripts/fix-evdi-version.sh` (dock unplugged;
takes effect on the next clean reboot — no live `rmmod`).

### Option A2 — match DLM to evdi 1.12.0 (fallback if 1.14.x won't build on 4.9)
Install an **older DisplayLink release whose bundled evdi is 1.12.x** (DL driver
~5.6/5.7-era). Then DLM and evdi agree and the double-connect/disconnect path is
consistent. Downside: older firmware/features.

### Regardless of A1/A2
- Add a udev/suspend hook so unplug cleanly tears down the evdi connection
  before the device disappears (the crash rides in on "disconnect failed").
- Keep the dock unplugged when starting/stopping the service.

**Decision needed:** try A1 (build evdi 1.14.15 on 4.9) first, or go straight to
A2 (downgrade DLM)? A1 is cleaner if it compiles; A2 is the known-good fallback.

## Part B — mirror the internal panel to the DisplayLink output

PRIME is out (`NVIDIA-0 cap 0x0`). evdi is a separate DRM device that *something*
must render into; DisplayLinkManager then pushes that framebuffer over USB.
Options, roughly easiest-first:

### B1 — separate X screen on evdi + a framebuffer mirror daemon
- Re-enable `AutoAddGPU` (remove/adjust `20-dl-noautogpu.conf`) so X attaches the
  `modesetting` driver to `card0` as a **second screen** (`:0.1`), software
  (llvmpipe) rendered.
- Mirror the primary panel onto it by continuously copying the framebuffer
  (e.g. a small grabber that blits `DSI-0`'s content to the evdi screen). This
  is CPU-copy mirroring, not GPU — fine for a desktop/slides, weak for video.
- `xorg.conf` currently has `Disable "dri"` globally and the DL no-autogpu flag;
  both must change for this path.

### B2 — Wayland compositor that drives both DRM nodes
- A wlroots-based compositor (e.g. `wayfire`/`sway`) or `weston` with the DRM
  backend can enumerate `card0` (evdi) + the Tegra node and mirror/extend
  without needing RandR PRIME from the NVIDIA DDX.
- Risk: L4T R32-era (kernel 4.9) EGL/Wayland on Tegra is shaky; may need
  software rendering for the compositor. Bigger change — replaces the SDDM/X
  session. Best long-term answer for true multi-monitor.

**Recommendation:** get stability (Part A) + prove a picture via B1 mirror
first; pursue B2 only if we want a proper extended desktop later.

## Order of operations
1. Install `dl-panic-capture` (done in repo) so the next crash is readable.
2. Part A: align versions, stop the panic. Verify replug survives.
3. Part B1: get the internal panel mirrored onto the dock.
4. Part B2 / multi-monitor: later.
