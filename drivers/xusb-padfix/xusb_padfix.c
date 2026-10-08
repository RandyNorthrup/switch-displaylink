// SPDX-License-Identifier: GPL-2.0
/*
 * xusb_padfix -- stop a dock unplug from killing the Tegra xHCI (Switch L4T 4.9).
 *
 * On a USB2 root-port disconnect, tegra_xhci_hub_control() powers the UTMI
 * pad down as soon as the hub driver clears C_CONNECTION -- which happens
 * BEFORE usb_disconnect() tears down the devices that were behind the port.
 * With a busy dock (DisplayLink streaming) the controller is then asked to
 * stop/deconfigure endpoints with its PHY pad off; the commands never
 * complete, the 5 s timeout + command-ring abort spin with IRQs off, and the
 * box stalls ~20 s with USB dead, or the hard-lockup watchdog panics it.
 * (Log: "power down UTMI pad 0" -> "USB disconnect" -> ... "Stopped the
 * command ring failed, maybe the host is dead" -> "HC died".)
 *
 * Fix: skip the pad power-down when it comes from tegra_xhci_hub_control().
 * The pad is still powered down a couple of seconds later when the
 * controller enters ELPG (that path is a different caller), so power use is
 * unchanged once the port is idle.
 *
 * How: the call goes through padctl->soc->ops->utmi_pad_power_down. The ops
 * table is in rodata, but padctl->soc is a plain pointer in the padctl's
 * heap struct, so we point it at a copy of soc + ops with our hook. Every
 * address and layout assumption is checked against kallsyms before anything
 * is changed. No unload: the hook could be mid-call, and putting it back
 * only matters until the next reboot anyway.
 */
#define pr_fmt(fmt) "xusb_padfix: " fmt

#include <linux/device.h>
#include <linux/kallsyms.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/phy/phy.h>

#include "tegra-xusb.h"

static char *padctl_dev = "7009f000.xusb_padctl";
module_param(padctl_dev, charp, 0444);
MODULE_PARM_DESC(padctl_dev, "padctl platform device name");

static void (*orig_power_down)(struct phy *phy);
static atomic_t skipped = ATOMIC_INIT(0);
module_param_named(skipped, skipped.counter, int, 0444);
MODULE_PARM_DESC(skipped, "number of hub_control pad power-downs skipped");

static void padfix_utmi_pad_power_down(struct phy *phy)
{
	/*
	 * Who called tegra_phy_xusb_utmi_pad_power_down()? The kernel is built
	 * with frame pointers and -fno-optimize-sibling-calls, so the wrapper
	 * has its own frame record {fp, lr}: our record's fp points at it and
	 * its lr is the return address into the caller. (GCC on arm64 turns
	 * __builtin_return_address(1) into 0, so walk the chain by hand.)
	 */
	char sym[KSYM_SYMBOL_LEN];
	unsigned long *fp = __builtin_frame_address(0);
	unsigned long *wrapper_fp = (unsigned long *)fp[0];
	void *caller = wrapper_fp ? (void *)wrapper_fp[1] : NULL;

	sprint_symbol_no_offset(sym, (unsigned long)caller);
	if (!strcmp(sym, "tegra_xhci_hub_control")) {
		atomic_inc(&skipped);
		pr_info("skipped UTMI pad power-down from %pS\n", caller);
		return;
	}
	pr_info("UTMI pad power-down from %pS\n", caller);
	orig_power_down(phy);
}

static int __init padfix_init(void)
{
	const struct tegra_xusb_padctl_soc *soc_t210, *soc_b01, *cur;
	const struct tegra_xusb_padctl_ops *ops;
	struct tegra_xusb_padctl_soc *nsoc;
	struct tegra_xusb_padctl_ops *nops;
	struct tegra_xusb_padctl *padctl;
	struct device *dev;
	void *fn;

	soc_t210 = (void *)kallsyms_lookup_name("tegra210_xusb_padctl_soc");
	soc_b01 = (void *)kallsyms_lookup_name("tegra210b01_xusb_padctl_soc");
	ops = (void *)kallsyms_lookup_name("tegra210_xusb_padctl_ops");
	fn = (void *)kallsyms_lookup_name("tegra210_utmi_pad_power_down");
	if (!soc_t210 || !soc_b01 || !ops || !fn ||
	    !kallsyms_lookup_name("tegra_xhci_hub_control")) {
		pr_err("kernel symbols not found, not a Tegra210 L4T kernel?\n");
		return -ENOENT;
	}

	dev = bus_find_device_by_name(&platform_bus_type, NULL, padctl_dev);
	if (!dev) {
		pr_err("no device %s\n", padctl_dev);
		return -ENODEV;
	}
	padctl = dev_get_drvdata(dev);
	put_device(dev);	/* the padctl is built in and never goes away */

	/* Layout checks: our copy of xusb.h must match the running kernel. */
	if (!padctl || padctl->dev != dev) {
		pr_err("padctl drvdata layout mismatch\n");
		return -EINVAL;
	}
	cur = READ_ONCE(padctl->soc);
	if (cur != soc_t210 && cur != soc_b01) {
		pr_err("padctl->soc %p is not a tegra210 soc (already patched?)\n", cur);
		return -EINVAL;
	}
	if (cur->ops != ops || ops->utmi_pad_power_down != fn) {
		pr_err("padctl ops layout mismatch\n");
		return -EINVAL;
	}

	nops = kmemdup(ops, sizeof(*ops), GFP_KERNEL);
	nsoc = kmemdup(cur, sizeof(*cur), GFP_KERNEL);
	if (!nops || !nsoc) {
		kfree(nops);
		kfree(nsoc);
		return -ENOMEM;
	}
	orig_power_down = ops->utmi_pad_power_down;
	nops->utmi_pad_power_down = padfix_utmi_pad_power_down;
	nsoc->ops = nops;
	smp_wmb();
	WRITE_ONCE(padctl->soc, nsoc);

	pr_info("hooked utmi_pad_power_down on %s (%s)\n", padctl_dev,
		cur == soc_b01 ? "t210b01" : "t210");
	return 0;
}
module_init(padfix_init);

MODULE_DESCRIPTION("Keep the Tegra UTMI pad powered until USB disconnect teardown");
MODULE_LICENSE("GPL");
