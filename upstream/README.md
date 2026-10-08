# Upstream kernel patches: dock-unplug lockup

These patches move the fix from [`drivers/xusb-bwfix`](../drivers/xusb-bwfix)
into the kernel itself. They're for
[theofficialgman/switch-l4t-kernel-4.9](https://github.com/theofficialgman/switch-l4t-kernel-4.9)
and are based on `807d12f6`, which is that repo's HEAD as of 2026-10-07 and the tree
this machine's `4.9.140-l4t` was built from. The background is in
[docs/unplug-crash.md](../docs/unplug-crash.md).

| Patch | What it does | Status |
|---|---|---|
| [0001](kernel/0001-usb-xhci-don-t-drop-endpoints-of-a-device-that-is-al.patch) | `xhci_check_bandwidth`: skips the Configure Endpoint drop for a `NOTATTACHED` device, which the Tegra firmware never completes. This is the root cause. | Same logic tested live in xusb_bwfix |
| [0002](kernel/0002-usb-xhci-don-t-spin-with-interrupts-off-while-aborti.patch) | `xhci_abort_cmd_ring`: polls CRR with `readl_poll_timeout` (sleeping, lock dropped) instead of `xhci_handshake` (iteration-counted, IRQs off, ~18 s on T210). Defense in depth for any command that times out. | Tested in xusb_bwfix with a 100 ms limit; the patch keeps the spec's 5 s + 3 s |
| [0003](kernel/0003-usb-xhci-tegra-re-probe-through-the-driver-core-in-h.patch) | `xhci_reinit_work`: re-probes with `device_release_driver` + `device_attach`, because calling remove()/probe() directly fails with "can't request region". | The driver-core rebind was tested in xusb_bwfix; the old path's failure was reproduced |
| [0004](kernel/0004-usb-xhci-tegra-enable-hcd_reinit-by-default.patch) | Turns `en_hcd_reinit` on by default, so USB comes back after the controller dies. Optional: maintainers can drop it. | Same as 0003 |

**Testing done:**
- Each file compiles cleanly (no errors or warnings) against this kernel's config, using the
  installed headers plus the `switch-l4t-kernel-nvidia` include overlay.
- The behavior was tested on a Switch Lite through the equivalent out-of-tree module:
  a raw pull while mirroring, recovery, a replug and a working mirror again
  ([log](../evidence/unplug-2026-10-07-2332-bwfix-rebind-rawpull.dmesg)).
- **Not done:** booting a kernel built with these patches. Building the full L4T
  kernel needs the whole build setup (nvidia overlays, l4t-kernel-build-scripts), so
  that part is left to the maintainers' CI or a later local build.

**Apply:**

```sh
cd switch-l4t-kernel-4.9
git am /path/to/switch-displaylink/upstream/kernel/*.patch
```

**Submitted:** 2026-10-08 as
[theofficialgman/switch-l4t-kernel-4.9#1](https://github.com/theofficialgman/switch-l4t-kernel-4.9/pull/1)
from branch `xhci-unplug-lockup` of RandyNorthrup/switch-l4t-kernel-4.9. Each commit
carries Randy's DCO `Signed-off-by` and credits AI assistance (`Co-Authored-By: Claude`).
