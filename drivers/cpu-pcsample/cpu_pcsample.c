// SPDX-License-Identifier: GPL-2.0
/*
 * cpu_pcsample -- find out what a hard-locked CPU is doing (Switch L4T 4.9).
 *
 * The dock-unplug crash ends in "Watchdog detected hard LOCKUP on cpu 0", but
 * arm64 on 4.9 has no NMI, so cpu0 never prints its own stack, and this kernel
 * has no kprobes/ftrace/CoreSight driver. The CPU's external debug interface
 * still works though: EDPCSR (external debug PC sample register) can be read
 * over the debug APB from another CPU while the target is running, IRQs off or
 * not. See docs/unplug-crash.md.
 *
 * Stages (module parameter "stage"), each one step riskier than the last:
 *   0  read MDRAR_EL1/MPIDR_EL1 on every CPU (system registers only)
 *   1  also walk the CoreSight ROM table and list the components
 *   2  also unlock each core's debug block and sample every CPU's PC once
 *   3  keep sampling: an hrtimer on a watcher CPU logs the target CPU's PC
 *      when it stops moving (hard lockup in progress)
 */
#define pr_fmt(fmt) "cpu_pcsample: " fmt

#include <linux/cpu.h>
#include <linux/io.h>
#include <linux/kallsyms.h>
#include <linux/module.h>
#include <linux/pm_qos.h>
#include <linux/smp.h>

/*
 * Tegra210 CCPLEX (Cortex-A57 cluster) CoreSight map. Not in the Switch DT;
 * from NVIDIA's tegra210-soc-base.dtsi ("ptm0".."ptm3" = 0x73440000 + n*1M)
 * plus the A57 TRM per-core layout: debug +0x10000, CTI +0x20000,
 * PMU +0x30000, ETM +0x40000. MDRAR_EL1 reads 0 here, so no discovery.
 */
#define CCPLEX_ROM	0x73000000UL
#define CORE_DBG(n)	(0x73410000UL + (n) * 0x100000UL)
#define NCORES		4

/* ARMv8 external debug registers (offsets in the 4K debug block) */
#define EDPCSR_LO	0x0a0
#define EDPCSR_HI	0x0ac
#define EDPRSR		0x314
#define EDLAR		0xfb0
#define EDLSR		0xfb4
#define EDDEVARCH	0xfbc
#define EDDEVTYPE	0xfcc
#define PIDR0		0xfe0
#define CIDR0		0xff0

#define EDPRSR_PU	BIT(0)	/* core powered up */
#define EDPRSR_SPD	BIT(1)	/* sticky powered down */
#define EDPRSR_OSLK	BIT(5)	/* OS lock locked */
#define EDPRSR_DLK	BIT(6)	/* double lock */

static void __iomem *dbg[NCORES];

static int stage;
module_param(stage, int, 0444);
MODULE_PARM_DESC(stage, "0=sysregs, 1=+ROM table, 2=+sample PCs once, 3=+lockup watcher");

struct cpu_info {
	u64 mdrar, mpidr;
};
static struct cpu_info info[NR_CPUS];

static void read_sysregs(void *unused)
{
	struct cpu_info *ci = &info[smp_processor_id()];

	/* MDRAR_EL1 = S2_0_C1_C0_0 (older assemblers lack the name) */
	asm volatile("mrs %0, S2_0_C1_C0_0" : "=r"(ci->mdrar));
	asm volatile("mrs %0, mpidr_el1" : "=r"(ci->mpidr));
}

static u32 cidr(void __iomem *base)
{
	u32 v = 0;
	int i;

	for (i = 0; i < 4; i++)
		v |= (readl_relaxed(base + CIDR0 + 4 * i) & 0xff) << (8 * i);
	return v;
}

static u32 pidr(void __iomem *base)
{
	u32 v = 0;
	int i;

	for (i = 0; i < 4; i++)
		v |= (readl_relaxed(base + PIDR0 + 4 * i) & 0xff) << (8 * i);
	return v;
}

/* Stage 1: ID registers only (debug power domain, always readable). */
static int probe_ids(void)
{
	void __iomem *rom = ioremap(CCPLEX_ROM, SZ_4K);
	int n, ok = 0;
	u32 c, devarch, devtype;
	bool is_dbg;

	if (!rom)
		return -ENOMEM;
	pr_info("CCPLEX ROM %#lx: CIDR %#010x PIDR %#010x\n", CCPLEX_ROM, cidr(rom), pidr(rom));
	iounmap(rom);

	for (n = 0; n < NCORES; n++) {
		dbg[n] = ioremap(CORE_DBG(n), SZ_4K);
		if (!dbg[n])
			return -ENOMEM;
		c = cidr(dbg[n]);
		devarch = readl_relaxed(dbg[n] + EDDEVARCH);
		devtype = readl_relaxed(dbg[n] + EDDEVTYPE);
		/* debug component: CIDR class 0x9, DEVTYPE 0x15, DEVARCH arch 0x6a15 */
		is_dbg = c == 0xb105900d && (devtype & 0xff) == 0x15 &&
			     (devarch & 0xffff) == 0x6a15;
		pr_info("core%d debug %#lx: CIDR %#010x PIDR %#010x DEVTYPE %#x DEVARCH %#010x %s\n",
			n, CORE_DBG(n), c, pidr(dbg[n]), devtype, devarch,
			is_dbg ? "= ARMv8 debug" : "NOT a debug block");
		ok += is_dbg;
	}
	return ok == NCORES ? 0 : -ENODEV;
}

static struct pm_qos_request no_idle;

static void wake(void *unused) { }

/* Stage 2: unlock and sample every core's PC once. */
static void sample_all(void)
{
	u32 prsr, lo, hi;
	u64 pc;
	int n;

	/* keep cores out of power-gated idle while we touch core-domain regs */
	pm_qos_add_request(&no_idle, PM_QOS_CPU_DMA_LATENCY, 0);
	on_each_cpu(wake, NULL, 1);

	for (n = 0; n < NCORES; n++) {
		prsr = readl_relaxed(dbg[n] + EDPRSR);

		if (!(prsr & EDPRSR_PU) || (prsr & EDPRSR_DLK)) {
			pr_info("core%d: EDPRSR %#x, not sampling (%s)\n", n, prsr,
				prsr & EDPRSR_PU ? "double lock" : "powered down");
			continue;
		}
		writel_relaxed(0xc5acce55, dbg[n] + EDLAR); /* software lock off */
		lo = readl_relaxed(dbg[n] + EDPCSR_LO); /* latches HI */
		hi = readl_relaxed(dbg[n] + EDPCSR_HI);
		pc = ((u64)hi << 32) | lo;

		pr_info("core%d: EDPRSR %#x EDLSR %#x PC %#llx %pS\n", n, prsr,
			readl_relaxed(dbg[n] + EDLSR), pc,
			lo == 0xffffffff ? NULL : (void *)pc);
	}
	pm_qos_remove_request(&no_idle);
}

static int __init pcsample_init(void)
{
	int cpu, ret;

	for_each_online_cpu(cpu) {
		smp_call_function_single(cpu, read_sysregs, NULL, 1);
		pr_info("cpu%d: MPIDR %#llx MDRAR %#llx (ROM table %s at %#llx)\n",
			cpu, info[cpu].mpidr, info[cpu].mdrar,
			(info[cpu].mdrar & 3) == 3 ? "valid" : "NOT valid",
			info[cpu].mdrar & ~0xfffULL);
	}
	if (stage < 1)
		return 0;

	ret = probe_ids();
	if (ret) {
		pr_err("debug blocks not where expected, stopping\n");
		goto unmap;
	}
	if (stage >= 2)
		sample_all();
	return 0;
unmap:
	for (cpu = 0; cpu < NCORES; cpu++)
		if (dbg[cpu])
			iounmap(dbg[cpu]);
	return ret;
}

static void __exit pcsample_exit(void)
{
	int n;

	for (n = 0; n < NCORES; n++)
		if (dbg[n])
			iounmap(dbg[n]);
}

module_init(pcsample_init);
module_exit(pcsample_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Sample a hard-locked CPU's PC via the external debug interface");
