# Unplug crash: Tegra xHCI dies when the dock is pulled

**Status (2026-10-07 23:00): narrowed to the xHCI command path during the first device's teardown, not fixed.** The workaround is
[`scripts/dock-eject.sh`](../scripts/dock-eject.sh) before pulling the cable.

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

## Working hypothesis

When VBUS drops with transfers in flight, the xHCI controller (or the fabric
path to it) wedges. A CPU MMIO access or BPMP clock request then stalls with
IRQs off, which fits both the 23 s whole-SoC stall and the cpu0 hard lockup.
Not confirmed yet.

## Tried: sampling cpu0's PC through CoreSight (not possible)

[`drivers/cpu-pcsample`](../drivers/cpu-pcsample) finds each A57 core's
external debug block (CCPLEX ROM table at `0x73000000`, core debug at
`0x73410000 + n*1M`; `MDRAR_EL1` reads 0, so the addresses are hardcoded) and
reads EDPCSR. Stages 0-2 ran without problems, but every core reports
`EDPRSR 0x28b/0x281`, which has **EDAD and EPMAD set**: external debug access is
disabled (retail fuses or secure firmware), so EDPCSR reads `0xffffffff`. A
lockup PC sampler isn't possible on a retail Switch. [Log](../evidence/cpu-pcsample-2026-10-07-2249-stage2.log).

## Next step

Work out from the kernel source (theofficialgman/switch-l4t-kernel-4.9 @807d12f)
where the xHCI command-timeout path busy-waits with IRQs off during the `1-1.1`
teardown, then fix it there. The kernel has no kprobes, ftrace or kcore, and
`xhci-tegra`/`phy-tegra-xusb` are built in (`=y`), so the fix is either a
pointer swap from a module (as the two experiments above did) or a patched
kernel Image.

## Workaround

```sh
sudo scripts/dock-eject.sh   # deauthorizes the dock's USB ports; then pull the cable
```

Tested: a clean pull, then a physical replug works and DisplayLinkManager
reconnects. A software re-authorize does *not* bring the DisplayLink chip back
(port gives -71), so reattaching needs the physical replug.
