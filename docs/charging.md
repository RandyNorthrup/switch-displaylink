# Dock not charging the Switch (USB-C PD role race)

**Symptom:** the dock is powered, but the Switch battery drains while docked.
The Switch is actually *powering the dock*.

**Fix:** power the dock up fully **before** plugging its USB-C cable into the
Switch:

1. Unplug the dock's USB-C cable from the Switch.
2. Unplug the dock's power adapter and wait about 15 s.
3. Plug the power adapter back in and wait about 15 s for the dock to boot.
4. Plug the USB-C cable into the Switch.

Replugging only the USB-C cable while the dock stays powered did **not** fix
it (2026-10-20 12:13). The full dock power-cycle did (12:18).

## What decides it

The UD-3900PDZ's host port is dual-role. Whether the Switch charges is
settled by bm92t (the Switch's PD controller) at the moment the cable
connects. The deciding bit is `VSAFE` (0x0400) in `STATUS1` at insertion,
meaning the dock is already supplying VBUS:

| Attach | STATUS1 at insertion | Result |
|---|---|---|
| 2026-10-07 20:07 (boot) | `0x4580` (VSAFE) | "Device in powered mode", 15 V/3 A, charging |
| 2026-10-07 23:22 and 23:23 (replugs) | `0x0D80` (VSAFE) | 15 V/3 A, charging |
| 2026-10-07 20:14 (replug after `dock-eject.sh`) | `0x1280`, no VSAFE | OTG: the Switch powers the dock |
| 2026-10-07 21:15 boot | `0x1A80`, no VSAFE | OTG |
| 2026-10-20 boot, and a USB-C-only replug | `0x1A80` / `0x1AC0`, no VSAFE | OTG |
| 2026-10-20 12:18:55 (after a dock power-cycle) | `0x0D80` (VSAFE) | 15 V/3 A, charging |

**Good attach** in `dmesg`:

```
bm92t: PDO 1: 3000mA 5000mV DRD ... PDO 4: 3000mA 15000mV ... PDO 5: 5000mA 20000mV
bm92t: Device in powered mode
bm92t: New PD Contract: PDO: 4: 3000mA, 15000mV
bm92t: Set Charging Current Limit 900ma
bq2419x: Setting charging current 900 mA
```

Afterwards `/sys/class/extcon/extcon1/state` has `USB-PD=1`, and the battery
shows `Charging` at about +1.5 A.

**Bad attach** in `dmesg`:

```
bm92t: Device in OTG mode
bq2419x: VBUS enabled, charging disabled
bq2419x: Setting VBUS current 1300 mA
```

The battery drains at about −1 A. With the Switch sourcing at most 1.3 A for
the whole dock, bus-powered devices behind it can brown out. The Razer
Stream Controller locked up under load in that state.

Check the current state:

```sh
sudo grep -E 'STATUS1|CURRENT_PDO' /sys/kernel/debug/bm92txx/regs
cat /sys/class/extcon/extcon1/state /sys/class/power_supply/battery/{status,current_now}
```

## Things that do NOT fix it

- **A PD power-role swap from debugfs** (`echo 0x0303 >
  /sys/kernel/debug/bm92txx/cmd`). The Switch becomes the sink and the dock
  offers 15 V, but the driver sits in `VDM_DISC_ID_SENT` and ignores the new
  contract. In the driver, the charger path only runs from `INIT_STATE` and
  on first init. So it never raises the charger input limit: it stays at
  500 mA and the RDO asks for 15 V at 0.5 A. That's about 2.5 W net, and the
  battery status stays `Unknown`. A polling service that automated this
  (`switch-pd-sink`) was disabled for that reason.
- **A PD soft reset** (`0xACAC`). The contract and the limit stay the same.

A proper software fix would mean a driver change. For example, bm92t could
re-run its sink/charger path after a successful PR_SWAP, or prefer the sink
role (Try.SNK) when a dual-role partner attaches.
