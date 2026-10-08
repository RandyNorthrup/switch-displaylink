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

1. Grab the X root window with XShm (30 fps max) and draw the cursor in (XFixes).
2. Copy only the changed rows into the hidden one of two dumb buffers.
3. `DirtyFB` the hidden buffer with just those rows, then `drmModePageFlip` to it.
4. evdi tells DLM an update is ready, DLM grabs the pixels and sends them over USB.
   evdi completes the flip only after that grab, so the buffer we draw into is
   never being read (no tearing).

## Things that matter (learned the hard way)

- **Flips mark the whole screen dirty.** On a flip, evdi marks the full frame
  dirty unless rects are already pending, and DLM then re-encodes all
  1280×720 of it. Doing that per keystroke made typing lag on the dock. Dirtying
  the off-screen back buffer first only queues the rects (no plane uses that
  framebuffer, so nothing is committed), and the flip then carries just those
  rows.
- **Full repaints only at startup.** DLM reconnects right after our modeset and
  drops what it had, so dl-mirror resends the whole frame every second for the
  first 5 s. After that it does so only every 30 s as a safety net. Repainting
  every second all the time added a full-screen encode that keystrokes queued
  behind.
- **Measured pipeline:** change on screen → our push ≤ 33 ms (one capture tick).
  Flip → DLM grab ≈ 30 µs. Flip-done event ≈ 1–2 ms (the pixel copy). See
  [`evidence/dl-mirror-flip-latency-2026-10-07.log`](../evidence/dl-mirror-flip-latency-2026-10-07.log).
  Anything slower than that is DLM's encode or USB time.
- evdi on 4.9 needed two fixes for this path (in `drivers/evdi`): a
  double free in `evdi_user_framebuffer_dirty` (a successful 4.9
  `drm_atomic_commit` frees the state), and `.get_vblank_counter =
  drm_vblank_no_hw_counter` (NULL deref in `drm_crtc_vblank_off` on CRTC
  disable).
- The `Double connect - replacing X with X` warning after our modeset is
  harmless: DLM reconnects with the same pointer.

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

Options: `-f fps`, `-m WxH` (force mode), `-C` (no cursor), `-1` (single
buffer + dirtyfb; may tear), `-d /dev/dri/cardN`.

Diagnostics: `DL_MIRROR_TRACE=1` logs every push and flip. `=2` also blocks on
each flip to time the flip event (it changes timing, so use it for diagnosis only).
[`scripts/mirror-test.sh [secs]`](../scripts/mirror-test.sh) runs a timed
session and captures dmesg to `/var/log/dl-mirror/`.

## Replug latency

From plug-in to picture takes about 13–16 s: ~2.6 s for USB/PD enumeration,
~10–12 s for DLM from evdi attach to connect, and under 1 s for dl-mirror. The
DLM part is on the dock's side. Keeping DLM resident across unplugs saved only
~2 s, so that change was reverted.
