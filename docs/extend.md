# Extended desktop (multi-monitor)

The Switch panel plus one or two dock monitors as one desktop. Built 2026-10-12.

## Why it can't be done the normal way

- **No PRIME.** The Tegra X driver reports `NVIDIA-0 cap 0x0`, so
  `xrandr --setprovideroutputsource` can't attach the evdi cards.
- **No Xinerama either.** A second X screen (`modesetting` on the evdi card)
  works, but KWin then sees one RandR screen and can't place windows on the dock.

## How it works

1. **Enlarged X screen.** At login, `dl-extend session` grows the NVIDIA screen
   past the panel, for example to `xrandr --fb 5120x1080`. It pins the panel
   with `--panning FWxFH+0+0/1x1+0+0`. The 1x1 tracking area stops the panel
   from scrolling after the pointer.
2. **One `dl-mirror` per monitor.** `dl-mirror -s WxH+X+Y -d <evdi card>`
   shows only its region of the X screen. `dl-extend run`, the ExecStart of
   `dl-mirror.service`, starts one of these per monitor.
3. **RandR 1.5 monitors.** `xrandr --setmonitor` describes the regions to
   clients that read monitors (GTK, Chromium).
4. **`fakerandr/dl-fakerandr.so`.** KWin 5.27 and Qt 5 enumerate CRTCs and
   outputs instead, and KWin clamps windows to the screens it knows about. This
   `LD_PRELOAD` shim for xcb-randr appends one fake CRTC, output and mode per
   region. It reads them from `$XDG_RUNTIME_DIR/dl-fake-randr` and passes
   everything through when that file is missing. kscreen* is skipped. The login
   hook (`/etc/xdg/plasma-workspace/env/dl-extend.sh`) exports the preload in
   both mirror and extend mode, so switching mid-session works.
5. **`extend/dl-edgewrap`** handles monitors that stand *left* of the Switch
   (see below).

## Usage

```sh
dl-extend detect            # write ~/.config/dl-extend.conf from the monitors attached now
dl-extend identify [secs]   # show each screen's name on it
dl-extend arrange DOCK2 PANEL DOCK1   # physical left-to-right order; applies live
dl-extend mirror            # every monitor shows the Switch screen (now and at login)
dl-extend extend            # back to the extended desktop
dl-extend off               # forget the layout; plain mirror from the next login
```

The mode persists in `~/.config/dl-extend.mode` (`mirror`, or absent = extend).
`arrange` restarts `dl-mirror.service` and plasmashell and jiggles the
framebuffer size so KWin and Qt re-read the layout. The frame rate is
`DL_EXTEND_FPS` (default 30). Xorg uses about 45 % of a core while extended.

## Panel position: a NVIDIA limit, and the workaround

The panel (`DSI-0`) only stays still when it sits at the X screen's origin
(0,0). At the bottom-right corner it doesn't pan, but RandR then **confines the
pointer to the panel**. Anywhere else, it pans after the pointer. Variants of
`xrandr --panning` and raw `XRRSetPanning` didn't help.

So the X screen order is always `PANEL(0) DOCK1 DOCK2`. A monitor that stands
physically left of the Switch goes at the far right end of the X screen, and
the conf gets a `WRAP <seam x>` line. `dl-edgewrap` (XI2 raw motion plus XFixes
pointer barriers) then:

- wraps the pointer between x=0 and the right edge of the framebuffer after a
  24-unit push,
- blocks the seam between the two dock monitors,
- fences off the dead strip below the panel.

`dl-extend arrange DOCK2 PANEL DOCK1` takes the physical order, with PANEL
anywhere.

Known cosmetic limits: a window can't straddle the panel/left-monitor edge
(it jumps), and KWin's idea of left and right neighbours follows the X order.

## Driver fix this needed: evdi `.dpms` on < 4.14

Starting Xorg `modesetting` on an evdi card panicked the box twice. On 4.9,
setting the DPMS property calls `connector->funcs->dpms` unconditionally,
and evdi leaves it NULL. `drivers/evdi/evdi-1.14.15/evdi_connector.c` now sets
`.dpms = drm_atomic_helper_connector_dpms` for kernels before 4.14. Verified
after a reboot: Xorg modesetting on card0 at 1080p, a DPMS cycle, a clean
exit, and no panic.

## Testing on a second X server

`xorg-test/evdi-screen.conf` runs an isolated X server `:1` on the evdi card
(no input, `DontVTSwitch`, `AccelMethod none`). Start it with
`-configdir xorg-test/empty.d`, and stop `dl-mirror.service` first. Stopping
it on its own is safe; this was verified for 60 s.

## Status and gotchas

- Verified live on 2026-10-12 with the panel and both dock monitors, in
  `DOCK2 | PANEL | DOCK1` order. Pointer reachability was checked with
  `xdotool mousemove_relative`. Plain `mousemove` warps hide the confinement.
- The layout is fixed at login, because clients don't re-read it on hotplug.
- `dl-edgewrap` must `_exit` in its signal handler. Otherwise `XNextEvent`
  restarts and `systemctl stop dl-mirror` hangs for 90 s.
- From the Bash tool, `pkill -f <pattern>` matches its own command line and
  kills the shell. Use `pgrep` and `kill <PID>` instead.
