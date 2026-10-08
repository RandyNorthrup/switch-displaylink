# Unplug crash: Tegra xHCI dies when the dock is pulled

**Status (2026-10-07 23:35): fixed by [`drivers/xusb-bwfix`](../drivers/xusb-bwfix).**
A raw pull while mirroring doesn't lock up, the module re-binds the dead xHCI
controller, and a replug enumerates and mirrors again without a reboot. The module
loads at boot: `scripts/install.sh` installs it via DKMS (`xusb_bwfix/1.0`) and
`/etc/modules-load.d/xusb_bwfix.conf`. Check it with `lsmod | grep xusb_bwfix` or
`dmesg | grep xusb_bwfix` ("hooked check_bandwidth"). If it isn't loaded, the
unplug lockup is back, so run [`scripts/dock-eject.sh`](../scripts/dock-eject.sh)
before unplugging.

## Symptom

Pulling the USB-C cable while the dock is active (DisplayLink streaming) does one
of two things:

- stalls the whole SoC for about 23 s, after which USB stays dead until reboot, or
- panics with `Watchdog detected hard LOCKUP on cpu 0`.

The old evdi 1.12.0 panic (see the README) was a separate bug and is fixed. With
evdi 1.14.15, evdi and DisplayLinkManager disconnect cleanly, and the crash comes
after that.

## What the logs show

From [`evidence/unplug-2026-10-07-2013-xhci-died.log`](../evidence/unplug-2026-10-07-2013-xhci-died.log)
(a pull the box survived):

```
20:13:33.483  tegra-xusb-padctl: power down UTMI pad 0
20:13:33.484  usb 1-1: USB disconnect ...
20:13:33.486  bq2419x: Charging Fault: Input Fault        (dock VBUS gone)
20:13:33.507  evdi: Disconnected from DisplayLinkManager   (clean)
20:13:33.514  bm92t: extcon USB-PD / USB HOST detached
   ... ~23 s with no progress, and the whole SoC is affected:
20:13:50      tegra-i2c: pio timed out (temp sensor)
20:13:44-56   brcmfmac: Timeout on response for query command
              joycon: delta=17427 ms (IMU reports dropped)
20:13:56.322  tegra-xusb: Stopped the command ring failed, maybe the host is dead
              bpmp: mrq 22 (clock) took 1282000 us
              tegra-xusb: Timeout while waiting for configure endpoint command
              tegra-xusb: HC died; cleaning up
```

The padctl power-down and the `USB disconnect` fire within 1 ms of the pull,
before the PD controller (bm92t) reports the detach. So a udev or extcon hook
would run too late.

**The stall starts inside the teardown of `1-1.1`** (the Logitech HID receiver,
the first device behind the dock hub). `1-1.1` logs its disconnect at +1 ms,
`1-1.2` (the VIA USB 2 hub) only at +22.8 s, right after `HC died`. The 22:53
crash log shows the same thing: nothing after `1-1.1` before the box locked up.
So the hub driver is stuck in `usb_disconnect(1-1.1)`, and the xHCI commands it
issues there (stop endpoint, then the configure-endpoint bandwidth drop) never
complete. That's where the `Timeout while waiting for configure endpoint command`
line comes from.

The hard-lockup dumps (`evidence/hekate-L4T_panic-*-unplug*.txt`) only carry the
detecting CPU's backtrace (an idle CPU). arm64 on 4.9 has no NMI, so cpu0's own
stack is never printed.

## Ruled out

| Idea | Result |
|---|---|
| evdi / DisplayLinkManager | Disconnect is clean (logged), and the stall comes after it. |
| Early UTMI pad power-down in `tegra_xhci_hub_control` | [`drivers/xusb-padfix`](../drivers/xusb-padfix) skips it, and it still hard-locked (2026-10-07 22:02, [evidence](../evidence/hekate-L4T_panic-2026-10-07-2202-unplug-with-padfix.txt)). |
| OTG host-mode switch-off racing the teardown | [`drivers/xusb-otgdefer`](../drivers/xusb-otgdefer) held bm92t's "USB HOST detached" back 3 s. The hold worked, but the teardown was already stuck on `1-1.1` and cpu0 still hard-locked about 23 s after the pull (2026-10-07 22:53, [evidence](../evidence/hekate-L4T_panic-2026-10-07-2253-unplug-with-otgdefer.txt), [log](../evidence/unplug-2026-10-07-2253-otgdefer-rawpull.dmesg)). |
| Charger input fault by itself | Pulls with the bus idle (xHCI already in ELPG) don't crash. |

## Root cause

From the kernel source this kernel was built from (theofficialgman/switch-l4t-kernel-4.9
@807d12f6, `drivers/usb/host`):

1. The pull wedges the xHC firmware: no command completes after it. The firmware is
   closed, so why is unknown. Pulls with an idle bus (controller in ELPG) don't
   trigger it, and neither does tearing the devices down first (`dock-eject.sh`).
2. `usb_disconnect(1-1.1)` → `usb_disable_device` → `xhci_check_bandwidth` queues a
   Configure Endpoint (drop) command and waits for it.
3. After 5 s the command timer runs `xhci_handle_command_timeout()` (xhci-ring.c:1276).
   It takes `xhci->lock` with `spin_lock_irqsave` and calls `xhci_abort_cmd_ring()`,
   which polls the ring with `xhci_handshake(..., 5*1000*1000)`, then again with
   `3*1000*1000` (xhci-ring.c:343-351). `xhci_handshake` counts loop iterations
   (readl + udelay(1)), not time, so on this SoC that's about 18 s with IRQs off.
4. 5 s + 18 s is the 23 s stall. With IRQs off that long, i2c, wifi and Joy-Con
   time out, and when it passes the 10 s watchdog threshold you get
   `Watchdog detected hard LOCKUP on cpu 0`. It then logs "Stopped the command ring
   failed", "Abort command ring failed", "HC died", in that order.

## The fix: `drivers/xusb-bwfix`

A module that swaps three things (no kernel rebuild):

- **`tegra_xhci_hc_driver.check_bandwidth`:** for a device that is already
  `NOTATTACHED`, revert the software state (`reset_bandwidth`) and send no command.
  The USB core ignores the result on that path, and Disable Slot frees the
  controller's side.
- **`xhci->cmd_timer` work function:** a copy of the timeout handler that waits for
  the ring abort at most `abort_ms` (100 ms), sleeping with the lock dropped. If the
  abort doesn't finish, it declares the controller dead the same way the original
  does, minus the IRQs-off spin. The hook is guarded: it only replaces the function
  pointer if it equals `xhci_handle_command_timeout`, so a wrong struct layout
  can't write anything. It's re-applied on every root-hub add, because a controller
  re-init re-runs `INIT_DELAYED_WORK`.
- **Recovery:** a dead controller is unbound and re-bound through the driver core,
  so USB comes back. NVIDIA's `en_hcd_reinit` can't do this. Its
  `xhci_reinit_work()` calls `tegra_xusb_remove()`/`tegra_xusb_probe()` directly, so
  devm resources are never freed and the re-probe fails with `can't request region
  for resource [mem 0x70099000-0x70099fff]`, which leaves USB gone until reboot
  ([log](../evidence/nvidia-hcd-reinit-2026-10-07-broken.dmesg)).

**Test 2026-10-07 23:21** (first version, without the rebind), a raw pull while
mirroring ([log](../evidence/unplug-2026-10-07-2321-bwfix-rawpull.dmesg)):

```
23:21:47.865  usb 1-1: USB disconnect / usb 1-1.1: USB disconnect
23:21:47.929  xusb_bwfix: 1-1.1 gone: skipped the Configure Endpoint drop
   ... 1-1.2, 1-1.3, 1-1.4, 1-1.5, 1-1: same; teardown done at 48.009 (145 ms)
23:21:53.362  xusb_bwfix: xHCI command timed out, aborting the command ring
23:21:53.462  xusb_bwfix: command ring abort didn't finish in 100 ms: xHCI is dead
23:21:53.462  tegra-xusb: HC died; cleaning up
```

No stall and no lockup, and the box kept running. One more command (queued around
48.36, type not logged in that version; the current version logs it) still timed
out, which confirms the firmware is wedged. USB stayed dead until reboot because
that version had no recovery.

**Test 2026-10-07 23:32** (current version, with the rebind), a raw pull while
mirroring, then a replug ([log](../evidence/unplug-2026-10-07-2332-bwfix-rebind-rawpull.dmesg)):

```
23:32:59.252  usb 1-1: USB disconnect
23:32:59.319  xusb_bwfix: 1-1.1 gone: skipped the Configure Endpoint drop
   ... all 6 devices skipped; teardown done at 59.435 (183 ms)
23:33:04.414  xusb_bwfix: xHCI command (TRB type 10, slot 3) timed out, aborting the command ring
23:33:04.515  xusb_bwfix: command ring abort didn't finish in 100 ms: xHCI is dead
23:33:04.515  tegra-xusb: HC died; cleaning up
23:33:05.566  xusb_bwfix: re-binding 70090000.xusb to bring USB back
23:33:05.578  xusb_bwfix: 70090000.xusb re-bound: ok
23:33:28.984  usb 1-1: new high-speed USB device number 2   (replug)
23:33:42.102  evdi: Connector state: connected               (DLM)
23:33:43.971  evdi: Opened by dl-mirror                      (picture back)
```

No stall and no lockup. USB came back on its own, and the mirror resumed about 15 s
after the replug (the normal replug latency, see [mirror.md](mirror.md)). The command
that times out after the pull is TRB type 10 (Disable Slot) on slot 3. It's queued
by `xhci_free_dev` when the last device reference goes away, not by the bandwidth drop,
so the firmware is wedged by the pull itself. The rebind is the recovery, not a workaround.

## Tried: sampling cpu0's PC through CoreSight (not possible)

[`drivers/cpu-pcsample`](../drivers/cpu-pcsample) finds each A57 core's
external debug block (CCPLEX ROM table at `0x73000000`, core debug at
`0x73410000 + n*1M`; `MDRAR_EL1` reads 0, so the addresses are hardcoded) and
reads EDPCSR. Stages 0-2 ran without problems, but every core reports
`EDPRSR 0x28b/0x281`, which has **EDAD and EPMAD set**: external debug access is
disabled (retail fuses or secure firmware), so EDPCSR reads `0xffffffff`. A
lockup PC sampler isn't possible on a retail Switch. [Log](../evidence/cpu-pcsample-2026-10-07-2249-stage2.log).

## Next step

The in-kernel version of the fix is written as four patches against
theofficialgman/switch-l4t-kernel-4.9 in [upstream/](../upstream/README.md): skip
`check_bandwidth` for `NOTATTACHED` devices, a sleeping time-based command ring
abort, a working `hcd_reinit`, and `hcd_reinit` turned on by default. They compile
but haven't been boot-tested in a built kernel. Submitted as
[theofficialgman/switch-l4t-kernel-4.9#1](https://github.com/theofficialgman/switch-l4t-kernel-4.9/pull/1).

## Workaround (no longer needed while xusb_bwfix is loaded)

```sh
sudo scripts/dock-eject.sh   # deauthorizes the dock's USB ports; then pull the cable
```

Tested: a clean pull, then a physical replug works and DisplayLinkManager
reconnects. A software re-authorize does *not* bring the DisplayLink chip back
(port gives -71), so reattaching needs the physical replug.
