# hekate L4T_panic dump — 2026-10-07 ~19:5x unplug (evdi 1.12.0 loaded)

hekate writes `sd:/L4T_panic.bin` (raw 2 MB ramoops region) + `L4T_panic.txt`
(console record rendered) on the boot after an L4T panic. The .bin's dmesg
zones were empty; only the console record exists. No ECC → bit flips.

Readable tail (uptime 2615 s):

    bq24190 0-006b: Charging Fault: Input Fault (OVP or VBAT<VBUS)
    Kernel panic - not syncing: Watchdog detected hard LOCKUP on cpu 0
      panic <- watchdog_check_hardlockup_other_cpu <- watchdog_timer_fn
      <- hrtimer_interrupt <- tegra210_timer_isr <- ... cpuidle_enter_state
    SMP: failed to stop secondary CPUs 0,1

Reading: ~0.2 s after VBUS from the dock drops, CPU0 (and CPU1) wedge with
IRQs off. The backtrace is from the *detecting* CPU (idle), so it does not say
what CPU0 was doing — evdi is not ruled in or out. On 2026-09-19 several
unplugs logged the same charger fault and did NOT crash, so the fault alone is
not fatal. `kernel.hardlockup_all_cpu_backtrace=1` (configs/99-dl-crash-debug.conf)
should make the next dump include CPU0's stack.
