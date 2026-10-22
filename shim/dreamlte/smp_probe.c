/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy S8 secondary-CPU probe, run by the loader before XNU (MMU off).
 *
 * s8-probe v3-v6: PSCI CPU_ON returns 0 (twice, never ALREADY_ON) yet no
 * secondary ever ran our entry. Samsung's 4.4 kernel powers cores through
 * the PMU itself (drivers/soc/samsung/exynos-pmu.c): CPUn_CONFIGURATION at
 * PMU + 0x2000 + ((Aff1 << 2 | Aff0) * 0x80), LOCAL_PWR_CFG = 0xF, status
 * at +4; per-cluster CPUSEQ_OPTION / NONCPU / L2 status at 0x2488 / 0x2404 /
 * 0x2604 + cluster * 0x20. This probe tries CPU_ON alone, then PMU power-up
 * followed by CPU_ON, for one A53 (0x101) and one Mongoose (0x000) core.
 * A started core marks its slot and turns itself off again (CPU_OFF).
 */
#include <stdint.h>
#include "smp_probe.h"
#include "pmic.h"

#define PMU		0x16480000UL
#define CPU_CFG(m)	(PMU + 0x2000 + ((((m) >> 8) & 0xf) << 2 | ((m) & 0xf)) * 0x80)
#define CPU_STAT(m)	(CPU_CFG(m) + 4)
#define CL_CPUSEQ(c)	(PMU + 0x2488 + (c) * 0x20)
#define CL_NONCPU(c)	(PMU + 0x2404 + (c) * 0x20)
#define CL_L2(c)	(PMU + 0x2604 + (c) * 0x20)

#define PSCI_CPU_ON	0xc4000003UL
#define TIMER_HZ	26000000UL

extern char smp_entry[];
extern volatile uint64_t smp_flags[8];	/* start.S: set by a started core */

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static int64_t smc3(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
			 "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
			 "x14", "x15", "x16", "x17", "memory");
	return (int64_t)x0;
}

static int wait_flag(unsigned slot, unsigned ms)
{
	uint64_t end = ticks() + TIMER_HZ / 1000 * ms;
	while (ticks() < end)
		if (smp_flags[slot])
			return 1;
	return 0;
}

static void regs(void (*kv)(const char *, uint64_t), uint64_t m)
{
	unsigned c = (m >> 8) & 0xf;
	kv(" cfg", rd(CPU_CFG(m)));
	kv("stat", rd(CPU_STAT(m)));
	kv("cpuseq", rd(CL_CPUSEQ(c)));
	kv("noncpu", rd(CL_NONCPU(c)));
	kv("l2", rd(CL_L2(c)));
}

/* Poll a PMU bit with a timeout; 1 = reached. */
static int wait_bit(uintptr_t a, uint32_t bit, unsigned ms)
{
	uint64_t end = ticks() + TIMER_HZ / 1000 * ms;
	while (ticks() < end)
		if (rd(a) & bit)
			return 1;
	return 0;
}

#define DD(n)		(0x16840000UL + (n) * 0x400)	/* DDn_DD_DA_INPUT_0 */

/*
 * Mongoose (CLUSTER0, MPIDR 0x0-0x3) bring-up test on core 0x000, in the
 * order of Samsung's cal-if/exynos8895/pmucal_cpu_exynos8895.h:
 * cluster0_on = CPU_QCH 0x3C20 := 1, wait 0x3C24; CLKSTOP 0x45C0 := 1,
 * wait 0x45C4; DD4 := 1; CPUSEQUENCER_OPTION 0x2488 bit 0 := 0.
 * cpu00_on = CONFIGURATION 0x2000 := 0xF; DD0 := 1. Then PSCI CPU_ON.
 * Every wait has a timeout and stops the test, so a stuck step is logged.
 */
void mngs_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	out("MNGS probe (Mongoose cluster 0)\n");
	kv("seq st", rd(PMU + 0x2484));
	kv("seq opt", rd(PMU + 0x2488));
	kv("qch", rd(PMU + 0x3c24));
	kv("clkstop", rd(PMU + 0x45c4));
	kv("dd4", rd(DD(4)));
	kv("cpu0 st", rd(CPU_STAT(0)));
	out("\n");
	wr(PMU + 0x3c20, rd(PMU + 0x3c20) | 1);
	if (!wait_bit(PMU + 0x3c24, 1, 50)) {
		out("MNGS: QCH status stuck\n");
		return;
	}
	wr(PMU + 0x45c0, rd(PMU + 0x45c0) | 1);
	if (!wait_bit(PMU + 0x45c4, 1, 50)) {
		out("MNGS: CLKSTOP status stuck\n");
		return;
	}
	wr(DD(4), rd(DD(4)) | 1);
	wr(PMU + 0x2488, rd(PMU + 0x2488) & ~1u);
	kv("cluster on: seq st", rd(PMU + 0x2484));
	out("\n");
	/*
	 * Boot 31 powered the core first, then CPU_ON: status 0xf, rc 0, the
	 * core never ran our entry. The order that starts the A53s (SweetCPU,
	 * patch 0124): power off, CPU_ON (firmware records the entry), power
	 * 0 -> 0xF edge, CPU_ON again. cpu00_on adds DD0 after the power-up.
	 */
	smp_flags[0] = 0;
	wr(DD(0), rd(DD(0)) & ~1u);
	wr(CPU_CFG(0), rd(CPU_CFG(0)) & ~0xfu);
	uint64_t end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && (rd(CPU_STAT(0)) & 0xf))
		;
	int64_t rc0 = smc3(PSCI_CPU_ON, 0x000, (uintptr_t)smp_entry, 0);
	wr(CPU_CFG(0), rd(CPU_CFG(0)) | 0xf);
	end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && (rd(CPU_STAT(0)) & 0xf) != 0xf)
		;
	wr(DD(0), rd(DD(0)) | 1);
	kv("first CPU_ON rc", (uint64_t)rc0);
	kv("cpu0 st", rd(CPU_STAT(0)));
	kv("dd0", rd(DD(0)));
	out("\n");
	int up = wait_flag(0, 100);
	int64_t rc = 0;
	if (!up) {
		rc = smc3(PSCI_CPU_ON, 0x000, (uintptr_t)smp_entry, 0);
		up = wait_flag(0, 300);
	}
	kv("CPU_ON rc", (uint64_t)rc);
	kv("MONGOOSE STARTED", up);
	out("\nMNGS probe done\n");
}

/*
 * galaxios8 Mongoose diagnostics (loader built with MNGS_DIAG=1).
 * Samsung's 4.4 kernel starts every core with plain PSCI CPU_ON; its
 * cal_cpu_enable / cal_cluster_enable run on CPU_STARTING, from the core that
 * is already up (exynos-powermode.c). So first try what Samsung does, then
 * the cluster0_on + core power edge, and dump PMU and CMU state around each
 * step. CMU_CPUCL0 (0x16800000) is embedded in the Mongoose cluster's power
 * domain, so it is only read once the cluster sequencer reports on; CMU_CPUCL1
 * (0x16900000, the running A53s) is the reference.
 */
#define CMU_CL0		0x16800000UL
#define CMU_CL1		0x16900000UL

/* NONCPU / L2 CONFIGURATION at 0x2400 / 0x2600 (+ cluster * 0x20), STATUS
 * at +4; Samsung's exynos-pmu.c only reads the status (LOCAL_PWR_CFG 0xF and
 * 0x7), the firmware writes the configuration. */
#define CL_NONCPU_CFG(c)	(PMU + 0x2400 + (c) * 0x20)
#define CL_L2_CFG(c)		(PMU + 0x2600 + (c) * 0x20)

static void cl_regs(void (*kv)(const char *, uint64_t), unsigned c)
{
	kv("seq st", rd(PMU + 0x2484 + c * 0x20));
	kv("seq opt", rd(PMU + 0x2488 + c * 0x20));
	kv("noncpu cfg", rd(CL_NONCPU_CFG(c)));
	kv("noncpu", rd(PMU + 0x2404 + c * 0x20));
	kv("l2 cfg", rd(CL_L2_CFG(c)));
	kv("l2", rd(PMU + 0x2604 + c * 0x20));
}

static void cmu_regs(void (*kv)(const char *, uint64_t), uintptr_t cmu, uint32_t div_off)
{
	kv("pll", rd(cmu + 0x120));
	kv("sw_user", rd(cmu + 0x100));
	kv("mux", rd(cmu + 0x1000));
	kv("div", rd(cmu + div_off));
}

static int mngs_try(const char *what, void (*kv)(const char *, uint64_t),
		    void (*out)(const char *))
{
	smp_flags[0] = 0;
	__asm__ volatile("dsb sy");
	int64_t rc = smc3(PSCI_CPU_ON, 0x000, (uintptr_t)smp_entry, 0);
	int up = wait_flag(0, 300);
	out(what);
	kv(" rc", (uint64_t)rc);
	kv("up", up);
	kv("cfg", rd(CPU_CFG(0)));
	kv("st", rd(CPU_STAT(0)));
	kv("rst", rd(CPU_CFG(0) + 0xc));
	out("\n");
	return up;
}

void mngs_diag(void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	/* S2MPS17 BUCK2 = vdd_cpucl0 (Mongoose), BUCK3 = vdd_cpucl1 (A53), per
	 * exynos8895-dreamlte_common.dtsi; CTRL bits 7:6 = enable. */
	out("MNGS DIAG v4\n pmic:");
	kv("b2ctrl", (uint32_t)pmic_read(0x1b));
	kv("b2out", (uint32_t)pmic_read(0x1c));
	kv("b3ctrl", (uint32_t)pmic_read(0x1d));
	kv("b3out", (uint32_t)pmic_read(0x1e));
	/* v2 (boot m3): A53 cluster seq 1, NONCPU cfg 0xf0f st 0xc000f, L2 cfg
	 * 0x101 st 1, PLL 0xa0f30311; not repeated. */
	out("\n MNGS cl0:");
	cl_regs(kv, 0);
	kv("qch", rd(PMU + 0x3c24));
	kv("clkstop", rd(PMU + 0x45c4));
	kv("cmu pwr", rd(PMU + 0x1438));
	kv("dd4", rd(DD(4)));
	kv("dd0", rd(DD(0)));
	out("\n");

	/* v1 (boot m2): T1 plain PSCI CPU_ON and T2 cluster0_on + CPU_ON left
	 * the core off; T3 powered it (status 0xf) but the cluster sequencer,
	 * NONCPU and L2 stayed off. v2 skips T1/T2 and adds T4. */

	/* cluster0_on (pmucal). */
	wr(PMU + 0x3c20, rd(PMU + 0x3c20) | 1);
	int q = wait_bit(PMU + 0x3c24, 1, 50);
	wr(PMU + 0x45c0, rd(PMU + 0x45c0) | 1);
	int k = wait_bit(PMU + 0x45c4, 1, 50);
	wr(DD(4), rd(DD(4)) | 1);
	wr(PMU + 0x2488, rd(PMU + 0x2488) & ~1u);
	uint64_t end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && !(rd(PMU + 0x2484) & 1))
		;
	out(" cluster0_on:");
	kv("qch ok", q);
	kv("clkstop ok", k);
	out("\n");

	/* 3. Core power edge 0 -> 0xF + DD0 (what starts the A53s), CPU_ON. */
	wr(DD(0), rd(DD(0)) & ~1u);
	wr(CPU_CFG(0), rd(CPU_CFG(0)) & ~0xfu);
	end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && (rd(CPU_STAT(0)) & 0xf))
		;
	smc3(PSCI_CPU_ON, 0x000, (uintptr_t)smp_entry, 0);
	wr(CPU_CFG(0), rd(CPU_CFG(0)) | 0xf);
	end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && (rd(CPU_STAT(0)) & 0xf) != 0xf)
		;
	wr(DD(0), rd(DD(0)) | 1);
	if (mngs_try(" T3 core edge:", kv, out))
		goto done;
	/* 4. Power the cluster's NONCPU and L2 ourselves, then the core edge. */
	wr(CL_NONCPU_CFG(0), rd(CL_NONCPU_CFG(0)) | 0xf);
	end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && (rd(PMU + 0x2404) & 0xf) != 0xf)
		;
	wr(CL_L2_CFG(0), rd(CL_L2_CFG(0)) | 0x7);
	end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && (rd(PMU + 0x2604) & 0x7) != 0x7)
		;
	out(" T4 noncpu+l2:");
	cl_regs(kv, 0);
	out("\n");
	if ((rd(PMU + 0x2404) & 0xf) == 0xf) {
		out(" MNGS cmu:");
		cmu_regs(kv, CMU_CL0, 0x180c);
		out("\n");
	}
	wr(DD(0), rd(DD(0)) & ~1u);
	wr(CPU_CFG(0), rd(CPU_CFG(0)) & ~0xfu);
	end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && (rd(CPU_STAT(0)) & 0xf))
		;
	smc3(PSCI_CPU_ON, 0x000, (uintptr_t)smp_entry, 0);
	wr(CPU_CFG(0), rd(CPU_CFG(0)) | 0xf);
	end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && (rd(CPU_STAT(0)) & 0xf) != 0xf)
		;
	wr(DD(0), rd(DD(0)) | 1);
	if (mngs_try(" T4 core edge:", kv, out))
		goto done;

	/* v3 (boot m5): BUCK2 (vdd_cpucl0) is already on (ctrl 0x78, out 0x3c,
	 * ~675 mV; BUCK3 ctrl 0x58 out 0x4c), so the rail is not the problem.
	 * T4 only OR'ed 0xF into NONCPU/L2 configurations that already read
	 * 0xF/0x7: no edge. The A53 cores need a 0 -> 0xF edge, so try edges on
	 * the cluster's NONCPU and L2 too (5), with the core edge again. */
	out(" T5 cluster edge:");
	wr(CL_L2_CFG(0), rd(CL_L2_CFG(0)) & ~0x7u);
	wr(CL_NONCPU_CFG(0), rd(CL_NONCPU_CFG(0)) & ~0xfu);
	end = ticks() + TIMER_HZ / 100;
	while (ticks() < end)
		;
	kv("off seq", rd(PMU + 0x2484));
	kv("off noncpu", rd(PMU + 0x2404));
	wr(CL_NONCPU_CFG(0), rd(CL_NONCPU_CFG(0)) | 0xf);
	end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && (rd(PMU + 0x2404) & 0xf) != 0xf)
		;
	wr(CL_L2_CFG(0), rd(CL_L2_CFG(0)) | 0x7);
	end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && (rd(PMU + 0x2604) & 0x7) != 0x7)
		;
	cl_regs(kv, 0);
	out("\n");
	if ((rd(PMU + 0x2404) & 0xf) == 0xf) {
		out(" MNGS cmu:");
		cmu_regs(kv, CMU_CL0, 0x180c);
		out("\n");
	}
	wr(DD(0), rd(DD(0)) & ~1u);
	wr(CPU_CFG(0), rd(CPU_CFG(0)) & ~0xfu);
	end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && (rd(CPU_STAT(0)) & 0xf))
		;
	smc3(PSCI_CPU_ON, 0x000, (uintptr_t)smp_entry, 0);
	wr(CPU_CFG(0), rd(CPU_CFG(0)) | 0xf);
	end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && (rd(CPU_STAT(0)) & 0xf) != 0xf)
		;
	wr(DD(0), rd(DD(0)) | 1);
	if (mngs_try(" T5 core edge:", kv, out))
		goto done;
	out("MNGS: no Mongoose core ran the entry\n");
	return;
done:
	out("MONGOOSE STARTED\n");
}

/*
 * E1 (boot m7): plain PSCI CPU_ON first thing in the loader started neither
 * 0x101 nor 0x000, so the loader's probes are not the cause.
 *
 * E2: S-Boot (sdb, function at file offset 0x74f1c, run before the kernel)
 * writes CPUn_OPTION = 0x10a for the Mongoose cores; bit 8 is
 * ENABLE_AUTO_WAKEUP_INT_REQ (Samsung S5E8890-syspwr.c): the PMU powers a core
 * up when the GIC raises an interrupt request for it. TWRP's Linux enables
 * the GIC before it boots secondaries with plain PSCI. So: enable the GIC
 * distributor, CPU_ON, and if the core is still off, send it SGI 1.
 * GIC-400: GICD 0x10201000, GICC 0x10202000; SGIR at GICD + 0xf00.
 */
#define GICD		0x10201000UL
#define GICC		0x10202000UL

void psci_early(void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	static const uint64_t targets[] = { 0x101, 0x000 };
	out("E2 gic:");
	kv("gicd ctlr", rd(GICD));
	kv("gicc ctlr", rd(GICC));
	kv("self", rd(GICD + 0x800) & 0xff);	/* ITARGETSR0: own interface */
	kv("opt 0x000", rd(CPU_CFG(0x000) + 8));
	kv("opt 0x101", rd(CPU_CFG(0x101) + 8));
	wr(GICD, rd(GICD) | 1);
	kv("gicd now", rd(GICD));
	out("\n");
	uint32_t self = rd(GICD + 0x800) & 0xff;
	/* Interface numbers are assumed to follow the PMU's linear order
	 * (cluster << 2 | core), checked against the boot core's own bit. */
	unsigned self_lin = 4;
	kv("self as expected", self == (1u << self_lin));
	out("\n");
	for (unsigned t = 0; t < 2; t++) {
		uint64_t m = targets[t];
		unsigned slot = (((m >> 8) & 1) << 2) | (m & 3);
		smp_flags[slot] = 0;
		__asm__ volatile("dsb sy");
		int64_t rc = smc3(PSCI_CPU_ON, m, (uintptr_t)smp_entry, slot);
		int up = wait_flag(slot, 50);
		int sgi = 0;
		if (!up) {
			wr(GICD + 0xf00, (1u << (16 + slot)) | 1);
			sgi = 1;
			up = wait_flag(slot, 300);
		}
		out(" E2");
		kv(" mpidr", m);
		kv("rc", (uint64_t)rc);
		kv("sgi", sgi);
		kv("up", up);
		kv("st", rd(CPU_STAT(m)));
		kv("seq0", rd(PMU + 0x2484));
		out("\n");
	}
}

void smp_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	static const uint64_t targets[] = { 0x101, 0x000 };
	uint64_t self = 0;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(self));
	out("SMP probe\n");
	kv("boot mpidr", self & 0xffffff);
	regs(kv, self & 0xffffff);
	out("\n");
	for (unsigned t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
		uint64_t m = targets[t];
		unsigned slot = (((m >> 8) & 1) << 2) | (m & 3);
		smp_flags[slot] = 0;
		kv("target", m);
		regs(kv, m);
		out("\n");
		int64_t rc = smc3(PSCI_CPU_ON, m, (uintptr_t)smp_entry, slot);
		int up = wait_flag(slot, 300);
		kv(" CPU_ON rc", (uint64_t)rc);
		kv("started", up);
		regs(kv, m);
		out("\n");
		if (up)
			continue;
		/* Power the core (and its cluster sequencer) through the PMU. */
		unsigned c = (m >> 8) & 0xf;
		wr(CL_CPUSEQ(c), rd(CL_CPUSEQ(c)) & ~1u);
		wr(CPU_CFG(m), rd(CPU_CFG(m)) | 0xf);
		uint64_t end = ticks() + TIMER_HZ / 10;
		while (ticks() < end && (rd(CPU_STAT(m)) & 0xf) != 0xf)
			;
		out(" after PMU power-up:");
		regs(kv, m);
		out("\n");
		rc = smc3(PSCI_CPU_ON, m, (uintptr_t)smp_entry, slot);
		up = wait_flag(slot, 300);
		kv(" CPU_ON rc", (uint64_t)rc);
		kv("started", up);
		regs(kv, m);
		out("\n");
	}
	out("SMP probe done\n");
}
