#!/bin/bash
# dock-eject.sh -- run (as root) BEFORE pulling the dock's USB-C cable.
#
# Why: yanking the dock with its USB devices live makes the Tegra xHCI stop
# answering ("Stopped the command ring failed, maybe the host is dead" ->
# "HC died"). The xHCI abort path spins with the controller lock held and IRQs
# off, so the box either stalls ~20 s and loses USB until reboot, or trips the
# hard-lockup detector and panics (hekate L4T_panic: "hard LOCKUP on cpu 0").
# evdi/DisplayLinkManager disconnect cleanly either way -- they're not the cause.
#
# Fix: de-authorize the dock's hub(s) first. The kernel then tears the
# devices down in software while the controller is healthy, DisplayLinkManager
# sees a normal disconnect, and the cable pull has no outstanding transfers.
# A re-plugged dock is a new device, so it comes back authorized automatically.
set -u
found=0
for d in /sys/bus/usb/devices/[0-9]*-[0-9]*; do
    name=${d##*/}
    [[ $name =~ ^[0-9]+-[0-9]+$ ]] || continue      # top-level ports only
    [ -f "$d/authorized" ] || continue
    echo "de-authorizing $name ($(cat "$d/idVendor"):$(cat "$d/idProduct") $(cat "$d/product" 2>/dev/null))"
    echo 0 > "$d/authorized"
    found=1
done
[ $found = 1 ] || { echo "no dock USB devices found"; exit 1; }
sleep 2
lsusb
echo "safe to unplug the dock now"
