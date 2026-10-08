// SPDX-License-Identifier: GPL-2.0
/*
 * xusb_bwfix -- fix for the dock-unplug lockup (Switch L4T 4.9).
 *
 * When the dock is pulled, the hub driver tears down the devices behind it.
 * usb_disable_device() drops their endpoints and asks the HCD to commit that
 * with a Configure Endpoint command (hcd->driver->check_bandwidth). The Tegra
 * xHC firmware never completes that command for a device behind the vanished
 * hub. After the 5 s command timer, xhci_handle_command_timeout() takes
 * xhci->lock with IRQs off and xhci_abort_cmd_ring() busy-polls the command
 * ring for 5M + 3M loop iterations (xhci_handshake counts iterations, not
 * time: ~18 s on this SoC). Meanwhile cpu0 sits with IRQs off, the whole box
 * stalls, and it often ends in "Watchdog detected hard LOCKUP on cpu 0".
 * See docs/unplug-crash.md.
 *
 * Two hooks:
 *
 * 1. check_bandwidth: the bandwidth drop is pointless for a device that is
 *    already gone. The Disable Slot that follows (xhci_free_dev) releases
 *    everything in the controller, and the USB core ignores check_bandwidth's
 *    result on this path. So for a NOTATTACHED device we revert the software
 *    state (reset_bandwidth) and skip the command. tegra_xhci_hc_driver is a
 *    writable static shared by both root hubs; we swap the pointer.
 *
 * 2. Safety net for any other command that times out (Disable Slot, Stop
 *    Endpoint...): xhci->cmd_timer's work function is replaced with a copy of
 *    xhci_handle_command_timeout() that polls the ring abort for at most
 *    abort_ms, sleeping and without the lock. If the abort doesn't finish,
 *    the controller is declared dead the same way the original does it, just
 *    without the IRQs-off busy-wait.
 *
 * 3. Recovery: a dead controller is unbound and re-bound through the driver
 *    core, so USB works again on the next plug-in (see rebind_fn).
 *
 * The xhci_hcd layout comes from the exact kernel tree this kernel was built
 * from (kernel-807d12f/, theofficialgman/switch-l4t-kernel-4.9 @807d12f6) and
 * is checked at load time: the module refuses to load unless
 * xhci->cmd_timer.work.func is xhci_handle_command_timeout.
 * No unload: reboot to undo.
 */
#define pr_fmt(fmt) "xusb_bwfix: " fmt

#include <linux/delay.h>
#include <linux/idr.h>
#include <linux/kallsyms.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include "kernel-807d12f/xhci.h"

static char *xusb_dev = "70090000.xusb";
module_param(xusb_dev, charp, 0444);

static bool skip_bw_drop = true;
module_param(skip_bw_drop, bool, 0644);
MODULE_PARM_DESC(skip_bw_drop, "skip the Configure Endpoint drop for unplugged devices");

static unsigned int abort_ms = 100;
module_param(abort_ms, uint, 0644);
MODULE_PARM_DESC(abort_ms, "max time to wait for a command ring abort");

static bool rebind = true;
module_param(rebind, bool, 0644);
MODULE_PARM_DESC(rebind, "unbind and rebind xhci-tegra after the controller died, so USB comes back");

static unsigned int rebind_delay_ms = 1000;
module_param(rebind_delay_ms, uint, 0644);

static struct hc_driver *drv;
static void *orig_timeout;
static int (*orig_check_bandwidth)(struct usb_hcd *, struct usb_device *);
static void (*p_cleanup_command_queue)(struct xhci_hcd *);
static void (*p_handle_stopped_cmd_ring)(struct xhci_hcd *, struct xhci_command *);
static void (*p_quiesce)(struct xhci_hcd *);
static int (*p_halt)(struct xhci_hcd *);

/*
 * Recovery after the controller died. xhci-tegra's own hcd_reinit can't be
 * used: xhci_reinit_work() calls tegra_xusb_remove()/probe() directly, so
 * the devm resources are never released and the re-probe fails ("can't
 * request region"), leaving USB gone until reboot. A driver-core unbind +
 * bind does the same thing properly.
 */
static struct device *rebind_dev;
static void rebind_fn(struct work_struct *work)
{
	struct device *dev = rebind_dev;
	int ret;

	pr_info("re-binding %s to bring USB back\n", dev_name(dev));
	device_release_driver(dev);
	ret = device_attach(dev);
	pr_info("%s re-bound: %s\n", dev_name(dev), ret == 1 ? "ok" : "FAILED");
	rebind_dev = NULL;
	put_device(dev);
}
static DECLARE_DELAYED_WORK(rebind_work, rebind_fn);

static int bwfix_check_bandwidth(struct usb_hcd *hcd, struct usb_device *udev)
{
	if (skip_bw_drop && udev->state == USB_STATE_NOTATTACHED) {
		drv->reset_bandwidth(hcd, udev);
		pr_info("%s gone: skipped the Configure Endpoint drop\n",
			dev_name(&udev->dev));
		return -ENODEV;
	}
	return orig_check_bandwidth(hcd, udev);
}

static bool cmd_ring_running(struct xhci_hcd *xhci)
{
	return xhci_read_64(xhci, &xhci->op_regs->cmd_ring) & CMD_RING_RUNNING;
}

/*
 * Same as xhci_handle_command_timeout() + xhci_abort_cmd_ring() at 807d12f,
 * except the abort is polled for abort_ms with the lock dropped.
 */
static void bwfix_command_timeout(struct work_struct *work)
{
	struct xhci_hcd *xhci = container_of(to_delayed_work(work), struct xhci_hcd, cmd_timer);
	struct usb_hcd *hcd = xhci_to_hcd(xhci);
	unsigned long flags, deadline;
	u64 ring;
	u32 f3;
	int ret;

	spin_lock_irqsave(&xhci->lock, flags);

	if (!xhci->current_cmd || delayed_work_pending(&xhci->cmd_timer)) {
		spin_unlock_irqrestore(&xhci->lock, flags);
		return;
	}
	xhci->current_cmd->status = COMP_CMD_ABORT;

	ring = xhci_read_64(xhci, &xhci->op_regs->cmd_ring);
	if (!((xhci->cmd_ring_state & CMD_RING_STATE_RUNNING) && (ring & CMD_RING_RUNNING))) {
		if (xhci->xhc_state & XHCI_STATE_REMOVING)
			p_cleanup_command_queue(xhci);
		else
			p_handle_stopped_cmd_ring(xhci, xhci->current_cmd);
		spin_unlock_irqrestore(&xhci->lock, flags);
		return;
	}

	f3 = le32_to_cpu(xhci->current_cmd->command_trb->generic.field[3]);
	pr_warn("xHCI command (TRB type %u, slot %u) timed out, aborting the command ring\n",
		TRB_FIELD_TO_TYPE(f3), TRB_TO_SLOT_ID(f3));
	/* Prevent new doorbells, and start the abort */
	xhci->cmd_ring_state = CMD_RING_STATE_ABORTED;
	reinit_completion(&xhci->cmd_ring_stop_completion);
	xhci_write_64(xhci, ring | CMD_RING_ABORT, &xhci->op_regs->cmd_ring);
	spin_unlock_irqrestore(&xhci->lock, flags);

	deadline = jiffies + msecs_to_jiffies(abort_ms);
	while (cmd_ring_running(xhci) && time_before(jiffies, deadline))
		usleep_range(500, 1000);

	if (cmd_ring_running(xhci)) {
		pr_err("command ring abort didn't finish in %u ms: xHCI is dead\n", abort_ms);
		spin_lock_irqsave(&xhci->lock, flags);
		xhci->xhc_state |= XHCI_STATE_DYING;
		p_quiesce(xhci);
		p_halt(xhci);
		p_cleanup_command_queue(xhci);
		spin_unlock_irqrestore(&xhci->lock, flags);
		usb_hc_died(hcd->primary_hcd);
		if (rebind && !rebind_dev) {
			rebind_dev = get_device(hcd->self.controller);
			schedule_delayed_work(&rebind_work, msecs_to_jiffies(rebind_delay_ms));
		}
		return;
	}

	/* Abort done; the stop event normally follows (see xhci_abort_cmd_ring) */
	ret = wait_for_completion_timeout(&xhci->cmd_ring_stop_completion,
					  msecs_to_jiffies(2000));
	spin_lock_irqsave(&xhci->lock, flags);
	if (!ret)
		p_cleanup_command_queue(xhci);
	else
		p_handle_stopped_cmd_ring(xhci,
			list_first_entry_or_null(&xhci->cmd_list, struct xhci_command, cmd_list));
	spin_unlock_irqrestore(&xhci->lock, flags);
}

/*
 * Swap the timeout handler if it is the stock one. Comparing against the
 * address of xhci_handle_command_timeout also checks that our xhci_hcd
 * layout matches the kernel's: anything else at that offset is left alone.
 * If the timer is queued right now it runs whichever function it reads;
 * both are correct.
 */
static void hook_timeout(struct usb_hcd *hcd, const char *why)
{
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);

	void *func = READ_ONCE(xhci->cmd_timer.work.func);

	if (func == orig_timeout) {
		WRITE_ONCE(xhci->cmd_timer.work.func, bwfix_command_timeout);
		pr_info("command timeout hooked (%s)\n", why);
	} else if (func != bwfix_command_timeout) {
		pr_err("xhci_hcd layout mismatch (cmd_timer.work.func = %pS): timeout not hooked\n",
		       func);
	}
}

/*
 * xhci_mem_init() re-runs INIT_DELAYED_WORK(cmd_timer) when the controller
 * is re-initialized (resume with power loss, hcd_reinit re-probes it with a
 * new xhci_hcd), which would drop our handler. Every (re-)init is followed by
 * a root hub being added, so check on each device add. This also covers
 * loading before xhci-tegra has probed.
 */
static int bwfix_usb_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct usb_device *udev = data;
	struct usb_hcd *hcd;

	if (action != USB_DEVICE_ADD)
		return NOTIFY_DONE;
	hcd = bus_to_hcd(udev->bus);
	if (hcd->driver == drv && !strcmp(dev_name(udev->bus->controller), xusb_dev))
		hook_timeout(hcd, "re-init");
	return NOTIFY_OK;
}
static struct notifier_block bwfix_nb = { .notifier_call = bwfix_usb_notify };

static struct usb_hcd *find_hcd(void)
{
	struct usb_bus *bus;
	struct usb_hcd *hcd = NULL;
	int id;

	mutex_lock(&usb_bus_idr_lock);
	idr_for_each_entry(&usb_bus_idr, bus, id) {
		if (bus->controller && !strcmp(dev_name(bus->controller), xusb_dev)) {
			hcd = bus_to_hcd(bus)->primary_hcd;
			break;
		}
	}
	mutex_unlock(&usb_bus_idr_lock);
	return hcd;
}

#define LOOKUP(var, name) do {						\
		*(unsigned long *)&(var) = kallsyms_lookup_name(name);	\
		if (!(var)) {						\
			pr_err("symbol %s not found\n", name);		\
			return -ENOENT;					\
		}							\
	} while (0)

static int __init bwfix_init(void)
{
	void *check;
	struct usb_hcd *hcd;

	LOOKUP(drv, "tegra_xhci_hc_driver");
	LOOKUP(check, "xhci_check_bandwidth");
	LOOKUP(orig_timeout, "xhci_handle_command_timeout");
	LOOKUP(p_cleanup_command_queue, "xhci_cleanup_command_queue");
	LOOKUP(p_handle_stopped_cmd_ring, "xhci_handle_stopped_cmd_ring");
	LOOKUP(p_quiesce, "xhci_quiesce");
	LOOKUP(p_halt, "xhci_halt");

	if ((void *)drv->check_bandwidth != check || !drv->reset_bandwidth) {
		pr_err("check_bandwidth is %pS, expected xhci_check_bandwidth\n",
		       drv->check_bandwidth);
		return -EINVAL;
	}

	usb_register_notify(&bwfix_nb);
	hcd = find_hcd();
	if (hcd && hcd->driver == drv)
		hook_timeout(hcd, "load");
	else
		pr_info("%s not up yet: will hook its command timeout when it is\n", xusb_dev);

	orig_check_bandwidth = drv->check_bandwidth;
	WRITE_ONCE(drv->check_bandwidth, bwfix_check_bandwidth);
	pr_info("hooked check_bandwidth of %s (abort_ms=%u, rebind=%d)\n",
		xusb_dev, abort_ms, rebind);
	return 0;
}

module_init(bwfix_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Tegra xHCI: no IRQs-off lockup when a dock is pulled");
