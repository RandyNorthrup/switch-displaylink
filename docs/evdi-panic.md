# Kernel panic on dock unplug/replug: evdi version mismatch (fixed)

> **Fixed** by [`scripts/install.sh`](../scripts/install.sh), or on an existing
> setup by [`scripts/fix-evdi-version.sh`](../scripts/fix-evdi-version.sh)
> (evdi 1.14.15). A *different* unplug crash, in the Tegra xHCI controller, is
> covered in [unplug-crash.md](unplug-crash.md).

## Cause

`DisplayLinkManager v5.9.184` expects **evdi 1.14.x** (the binary literally logs
`libevdi.so version is not compatible with Dlm`). The loaded/DKMS module was
**evdi 1.12.0**. The vendor `displaylink-driver.service` `ExecStartPre` picks the
newest evdi source dir *by mtime* (`ls -t /usr/src | grep evdi | head -n1`), and
`evdi-1.12.0` had been touched after `evdi-1.14.15`, so 1.12.0 kept winning.

`install.sh` fixes both halves: it installs evdi 1.14.15 through DKMS, removes
every other evdi version, and replaces that `ExecStartPre` with a drop-in that
loads the pinned version.

## What the crash looked like

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

Full traces: [evidence/evdi-crash-traces-2026-09-19.log](../evidence/evdi-crash-traces-2026-09-19.log).

Some of these crashes were *hard resets*: the kernel log was NUL-truncated right
before `Booting Linux`, and the pstore record was corrupt (ramoops has **no ECC**
and uses zlib). The intact earlier traces have the same signature. That's why
[`dl-panic-capture`](../README.md#capturing-a-crash-for-a-bug-report) exists.

## The 4.9-specific evdi fixes in this repo

Even with matching versions, upstream evdi needed these fixes to run on 4.9.
They're all in [`drivers/evdi/evdi-1.14.15`](../drivers/evdi/evdi-1.14.15):

- **compat49 shim** so it builds at all (upstream evdi supports 4.15+).
- **Double free in `evdi_user_framebuffer_dirty`**: on 4.9 a successful
  `drm_atomic_commit` already frees the state.
- **`.get_vblank_counter = drm_vblank_no_hw_counter`**: without it,
  `drm_crtc_vblank_off` dereferences NULL when a CRTC is disabled.
- **`.dpms = drm_atomic_helper_connector_dpms`** for kernels before 4.14: 4.9
  calls `connector->funcs->dpms` unconditionally, and evdi leaves it NULL.
  Starting Xorg `modesetting` on an evdi card panicked without it
  (see [extend.md](extend.md#driver-fix-this-needed-evdi-dpms-on--414)).
