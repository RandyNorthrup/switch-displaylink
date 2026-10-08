# dl-mirror: mirroring the Switch screen to the dock

**Status (2026-10-07): working.** It autostarts at login, the picture is
tear-free, and it reattaches after a replug.

## Why a custom mirror

The NVIDIA Tegra X driver has no RandR PRIME (`xrandr --listproviders` shows
`NVIDIA-0 cap 0x0`), so X can't render to the evdi card. Only DisplayLinkManager
(DLM) holds `card0`. Xorg opens it at startup but doesn't keep it
(`AutoAddGPU=false`).

[`mirror/dl-mirror.c`](../mirror/dl-mirror.c) works around that with direct KMS
on the evdi card:

1. Grab the X root window with XShm (60 fps max) and draw the cursor in
   (XFixes). KWin compositing is suspended while mirroring (see below).
2. Copy only the changed rows into the hidden one of two dumb buffers.
3. `DirtyFB` the hidden buffer with just those rows, then `drmModePageFlip` to it.
4. evdi tells DLM an update is ready, DLM grabs the pixels and sends them over USB.
   evdi completes the flip only after that grab, so the buffer we draw into is
   never being read (no tearing).

## Things that matter (learned the hard way)

- **KWin compositing makes the grab stale.** With the GL compositor running,
  a root-window grab lags the real screen by 40 ms on average and up to
  ~230 ms, and it only catches up on KWin's next repaint. On the dock, short
  changes were skipped and a dialog that appeared and then sat still didn't
  show until the mouse moved. dl-mirror suspends compositing over D-Bus while
  it mirrors (grab lag < 3 ms) and resumes it when the dock goes away. The unit
  also resumes it in `ExecStopPost` in case dl-mirror dies. `-K` keeps
  compositing on. Measure it with
  [`scripts/capture-staleness.c`](../scripts/capture-staleness.c).
  It changes a window's color while a loop polls the root window (`XGetImage`).

- **Flips mark the whole screen dirty.** On a flip, evdi marks the full frame
  dirty unless rects are already pending, and DLM then re-encodes all
  1280×720 of it. Doing that per keystroke made typing lag on the dock. Dirtying
  the off-screen back buffer first only queues the rects (no plane uses that
  framebuffer, so nothing is committed), and the flip then carries just those
  rows.
- **Full repaints only at startup, and when idle.** DLM reconnects right after
  our modeset and drops what it had, so dl-mirror resends the whole frame every
  second for the first 5 s. A full 720p frame costs DLM a 100–200 ms encode,
  and the picture freezes behind it. So after startup, the safety repaint runs
  at most every 30 s, and only after the screen has been still for 2 s, when
  nothing visible can stall.
- **Steady state:** with a box flashing at 20 Hz, flip → DLM grab is ≤ 65 ms
  apart from rare ~85 ms outliers (measured under strace, which slows DLM).
  The once-a-minute stats line reports `max grab`, `max flip` and
  `max loop gap` to catch regressions.
- **Measured pipeline:** change on screen → our push ≤ 17 ms (one 60 fps tick;
  dl-mirror uses ~23% of one core).
  Flip → DLM grab ≈ 30 µs. Flip-done event ≈ 1–2 ms (the pixel copy). See
  [`evidence/dl-mirror-flip-latency-2026-10-07.log`](../evidence/dl-mirror-flip-latency-2026-10-07.log).
  DLM then starts the USB send ~8 ms after the grab, and a typical 720p update
  is ~20 KB taking < 1 ms on the wire. Anything slower happens after the
  Switch: the dock's decoder or the monitor.

- evdi on 4.9 needed two fixes for this path (in `drivers/evdi`): a
  double free in `evdi_user_framebuffer_dirty` (a successful 4.9
  `drm_atomic_commit` frees the state), and `.get_vblank_counter =
  drm_vblank_no_hw_counter` (NULL deref in `drm_crtc_vblank_off` on CRTC
  disable).
- The `Double connect - replacing X with X` warning after our modeset is
  harmless: DLM reconnects with the same pointer.

## Hardware limits on this unit

- **It's a Switch Lite, so the dock link is USB 2 (480 Mbps).** The Lite has no
  video output (the bootloader sets `rohm,dp-disable`), and its USB-C port
  doesn't carry USB 3. `usb3_enable=1` is set but changes nothing here (the USB 3
  root hub stays empty). The dock's VIA Billboard device reports DP Alt Mode
  (pin C, all four lanes) as configured, which would take every SuperSpeed lane
  anyway.
- **Keep 720p.** The monitor (N1F PRO) is 1920×1080 native, so it upscales our
  720p. Sending 1080p and scaling on the Switch (`-m 1920x1080`) looked much
  worse: frames grew to 200 KB–1.5 MB and took up to 300 ms on USB 2. If the
  monitor has a game/low-latency mode, turning it on may cut its scaler delay.

## Running it

Install it (also done by `scripts/install.sh`):

```sh
make -C mirror && sudo make -C mirror install
sudo systemctl --global enable dl-mirror.service
systemctl --user daemon-reload && systemctl --user start dl-mirror.service
```

The user service runs `dl-mirror -w`: it waits for the dock monitor by polling
`/sys/class/drm/card0-*/status` (opening the card while waiting spams evdi
Opened/Closed messages), and it returns to waiting when the dock goes away.
Logs: `journalctl _SYSTEMD_USER_UNIT=dl-mirror.service`.

Options: `-f fps` (default 60), `-m WxH` (force mode), `-C` (no cursor),
`-K` (keep KWin compositing on), `-1` (single buffer + dirtyfb; may tear),
`-d /dev/dri/cardN`.

Diagnostics: `DL_MIRROR_TRACE=1` logs every push and flip. `=2` also blocks on
each flip to time the flip event (it changes timing, so use it for diagnosis only).
[`scripts/mirror-test.sh [secs]`](../scripts/mirror-test.sh) runs a timed
session and captures dmesg to `/var/log/dl-mirror/`.

## Replug latency

From plug-in to picture takes about 13–16 s: ~2.6 s for USB/PD enumeration,
~10–12 s for DLM from evdi attach to connect, and under 1 s for dl-mirror. The
DLM part is on the dock's side. Keeping DLM resident across unplugs saved only
~2 s, so that change was reverted.
