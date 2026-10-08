// SPDX-License-Identifier: GPL-2.0
/*
 * xusb_otgdefer -- experiment for the dock-unplug crash (Switch L4T 4.9).
 *
 * The USB-C port is an OTG port. When the PD controller (bm92t) reports
 * "USB HOST detached", ~30 ms after the cable is pulled, xhci-tegra's
 * tegra_xhci_set_host_mode(off) removes port power on the OTG USB3 and USB2
 * root ports and polls for up to 0.7 s for it to drop. That happens while
 * the hub driver is still tearing down the dock's devices behind the same
 * port. With scripts/dock-eject.sh the devices are already gone at that
 * point and the pull is always clean, so the overlap is the suspect.
 *
 * This module delays the "detached" event by `delay_ms` (default 3 s) so the
 * teardown finishes first. A reattach inside the window cancels it; the port
 * never left host mode, so the attach is a no-op for the driver.
 *
 * How: tegra->id_extcons_nb is a notifier_block in the driver's heap struct;
 * we swap its notifier_call for ours. It is found by scanning the struct for
 * the address of tegra_xhci_id_notifier (from kallsyms) and confirmed by
 * checking that the work_struct right after it runs tegra_xhci_id_extcon_work.
 * The extcon chain runs under edev->lock (IRQs off), so the wrapper only
 * schedules work and never calls back into extcon. No unload: reboot to undo.
 */
#define pr_fmt(fmt) "xusb_otgdefer: " fmt

#include <linux/device.h>
#include <linux/kallsyms.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

static char *xusb_dev = "70090000.xusb";
module_param(xusb_dev, charp, 0444);

static unsigned int delay_ms = 3000;
module_param(delay_ms, uint, 0644);
MODULE_PARM_DESC(delay_ms, "how long to hold back a USB HOST detach");

static struct notifier_block *target_nb;
static notifier_fn_t orig_call;
static void *pending_edev;

static void deferred_detach(struct work_struct *work)
{
	pr_info("passing on the held-back USB HOST detach\n");
	orig_call(target_nb, 0, pending_edev);
}
static DECLARE_DELAYED_WORK(defer_work, deferred_detach);

/* Runs under the extcon spinlock: schedule work only. */
static int defer_notifier(struct notifier_block *nb, unsigned long state, void *data)
{
	if (!state) {
		pending_edev = data;
		mod_delayed_work(system_wq, &defer_work, msecs_to_jiffies(delay_ms));
		pr_info("USB HOST detached: holding it back %u ms\n", delay_ms);
		return NOTIFY_OK;
	}
	if (cancel_delayed_work(&defer_work))
		pr_info("USB HOST re-attached within the window: dropped the held-back detach\n");
	return orig_call(nb, state, data);
}

static int __init otgdefer_init(void)
{
	unsigned long fn_notifier, fn_work;
	struct device *dev;
	void *tegra;
	int off, found = -1;

	fn_notifier = kallsyms_lookup_name("tegra_xhci_id_notifier");
	fn_work = kallsyms_lookup_name("tegra_xhci_id_extcon_work");
	if (!fn_notifier || !fn_work) {
		pr_err("xhci-tegra symbols not found\n");
		return -ENOENT;
	}

	dev = bus_find_device_by_name(&platform_bus_type, NULL, xusb_dev);
	if (!dev) {
		pr_err("%s not found\n", xusb_dev);
		return -ENODEV;
	}
	tegra = dev_get_drvdata(dev);
	put_device(dev);
	if (!tegra) {
		pr_err("%s has no driver data\n", xusb_dev);
		return -ENODEV;
	}

	/*
	 * notifier_block = { call, next, priority } (24 bytes), then
	 * work_struct = { data, entry (2 ptrs), func }: func is at nb + 48.
	 */
	for (off = 0; off < 4096; off += sizeof(long)) {
		unsigned long *p = tegra + off;

		if (p[0] == fn_notifier && p[6] == fn_work) {
			if (found >= 0) {
				pr_err("ambiguous layout (offsets %#x and %#x)\n", found, off);
				return -EINVAL;
			}
			found = off;
		}
	}
	if (found < 0) {
		pr_err("id_extcons_nb not found in tegra_xusb\n");
		return -EINVAL;
	}

	target_nb = tegra + found;
	orig_call = target_nb->notifier_call;
	WRITE_ONCE(target_nb->notifier_call, defer_notifier);
	pr_info("hooked id_extcons_nb at tegra+%#x; USB HOST detach held back %u ms\n",
		found, delay_ms);
	return 0;
}

module_init(otgdefer_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Hold back xhci-tegra's OTG host-mode-off on dock unplug");
