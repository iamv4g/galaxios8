/*
 * S8Mali: kernel driver for the Galaxy S8's Mali-G71 MP20 (Exynos 8895,
 * DT mali@13900000, GPU_ID 0x60a00001 = r0p0), written for this port.
 * Copied into the xnu tree as iokit/Kernel/arm/S8Mali.cpp by
 * apply-dreamlte.py. Boot-arg s8mali=1 starts it, s8mali=2 adds a self-test
 * (not s8gpu=: the loader appends its own probe record under that name).
 *
 * Register layout and bring-up order follow the public Bifrost register map
 * as Linux panfrost uses it (panfrost_regs.h): soft reset, the one G71 r0p0
 * erratum that matters here (T76X_3953: TILER_CONFIG clock-gate override),
 * L2 -> shader cores -> tiler power-up, address space 0 in legacy LPAE mode.
 * The loader proved the sequence (DREAMLTE_PROGRESS.md, GPU milestone 1b).
 *
 * Memory: low DRAM 0xd5000000-0xe3c00000 is outside XNU's RAM (XNU takes
 * the high bank) and free of TWRP DT reserved-memory nodes, so it is used as
 * a GPU carve-out: physically contiguous, known physical addresses, no
 * SYSMMU in the way. The first 2 MiB hold the page tables, the rest buffer
 * objects (BOs). The GPU is not coherent (COHERENCY_FEATURES 0): CPU
 * mappings are write-combined (Normal non-cacheable) and every job flushes
 * the GPU caches at start and end.
 *
 * GPU virtual addresses: one address space (AS 0) for all clients, BOs at
 * [32 MiB, 4 GiB) as panfrost does, own 4-level Mali-LPAE tables (table
 * entry 3, leaf 1 | AF | RW | outer shareable, XN for NOEXEC BOs).
 *
 * User client (selectors mirror the panfrost DRM UAPI so Mesa's Panfrost
 * driver can sit on top of a small libdrm stand-in):
 *   0 GetParam [param] -> [value]          (DRM_PANFROST_PARAM_* numbering)
 *   1 CreateBO [size, flags] -> [handle, gpu va]
 *   2 FreeBO [handle]
 *   3 Submit [jc, requirements] -> [JS_STATUS]   (synchronous)
 *   4 WaitBO [handle, timeout]             (always idle: submit waits)
 *   5 GetBOOffset [handle] -> [gpu va]
 *   6 Madvise [handle, madv] -> [retained]
 *   7 Report [code, a, b]                  (logged as "S8 MALI: <proc> report")
 *   8 ImportSurface [IOSurface ID] -> [handle, gpu va, size]
 *                                          (the surface's own pages, mapped for the GPU)
 *   9 SyncBO [handle]                      (clean + invalidate the CPU caches over the BO:
 *                                          before the GPU reads CPU writes to a cached
 *                                          mapping, and after it wrote, before the CPU reads)
 *  10 Log, structure input = text          (each line logged as "S8 LOG <proc>: ..."; for
 *                                          libraries inside iOS daemons whose stderr goes
 *                                          nowhere, e.g. the CoreAnimation bridge)
 * clientMemoryForType(handle) returns the BO's memory for
 * IOConnectMapMemory64 (pass kIOMapAnywhere | kIOMapWriteCombineCache).
 */
#include <pexpert/arm64/board_config.h>

#if defined(DREAMLTE)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/proc.h>
#include "SweetSurface.h"

extern "C" void flush_dcache64(addr64_t addr, unsigned count, int phys);
extern "C" boolean_t pmap_valid_address(uint64_t addr);

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern "C" void thread_deallocate(thread_t);
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
#ifndef VM_WIMG_WCOMB
#define VM_WIMG_WCOMB 0x6                 /* osfmk/arm/memory_types.h */
#endif

#define MALI_PHYS               0x13900000
#define CMU_G3D_PHYS            0x13800000
#define PMU_G3D_PHYS            0x16484000      /* page of 0x164840e0 */
#define POOL_PHYS               0xd5000000ULL
#define POOL_SIZE               0x0ec00000ULL   /* to 0xe3c00000 */
#define TABLES_SIZE             0x00200000ULL   /* 512 table pages */
#define BO_PHYS                 (POOL_PHYS + TABLES_SIZE)
#define BO_SIZE                 (POOL_SIZE - TABLES_SIZE)
#define BO_UNIT                 0x4000ULL       /* 16 KiB: user mappings use 16K VM pages */
#define BO_UNITS                (BO_SIZE / BO_UNIT)
#define TABLE_PAGES             (TABLES_SIZE >> 12)
#define VA_BASE                 0x02000000ULL   /* 32 MiB */
#define VA_END                  0xffff0000ULL   /* below 4 GiB */
#define VA_UNIT                 0x10000ULL
#define VA_UNITS                ((VA_END - VA_BASE) / VA_UNIT)
#define HEAP_BACKED_MAX         0x02000000ULL   /* HEAP BOs: first 32 MiB backed */
#define MAX_HANDLES             4096

/* Bifrost GPU_CONTROL / JOB_CONTROL / MMU registers (panfrost_regs.h) */
#define GPU_ID                  0x000
#define GPU_L2_FEATURES         0x004
#define GPU_CORE_FEATURES       0x008
#define GPU_TILER_FEATURES      0x00c
#define GPU_MEM_FEATURES        0x010
#define GPU_MMU_FEATURES        0x014
#define GPU_AS_PRESENT          0x018
#define GPU_JS_PRESENT          0x01c
#define GPU_INT_RAWSTAT         0x020
#define GPU_INT_CLEAR           0x024
#define GPU_INT_MASK            0x028
#define GPU_CMD                 0x030
#define GPU_FAULT_STATUS        0x03c
#define GPU_FAULT_ADDR_LO       0x040
#define GPU_AFBC_FEATURES       0x04c
#define GPU_THREAD_MAX_THREADS  0x0a0
#define GPU_THREAD_MAX_WG       0x0a4
#define GPU_THREAD_MAX_BARRIER  0x0a8
#define GPU_THREAD_FEATURES     0x0ac
#define GPU_TEXTURE_FEATURES(n) (0x0b0 + 4 * (n))
#define GPU_JS_FEATURES(n)      (0x0c0 + 4 * (n))
#define GPU_SHADER_PRESENT      0x100
#define GPU_TILER_PRESENT       0x110
#define GPU_L2_PRESENT          0x120
#define GPU_SHADER_READY        0x140
#define GPU_TILER_READY         0x150
#define GPU_L2_READY            0x160
#define GPU_SHADER_PWRON        0x180
#define GPU_TILER_PWRON         0x190
#define GPU_L2_PWRON            0x1a0
#define GPU_SHADER_PWROFF       0x1c0
#define GPU_TILER_PWROFF        0x1d0
#define GPU_L2_PWROFF           0x1e0
#define GPU_SHADER_PWRTRANS     0x200
#define GPU_TILER_PWRTRANS      0x210
#define GPU_L2_PWRTRANS         0x220
#define IDLE_OFF_TICKS          (26ULL * 200000)        /* cores off after 200 ms idle */
#define GPU_COHERENCY_FEATURES  0x300
#define GPU_THREAD_TLS_ALLOC    0x310
#define GPU_STACK_PRESENT       0xe00
#define GPU_TILER_CONFIG        0xf08
#define JOB_INT_RAWSTAT         0x1000
#define JOB_INT_CLEAR           0x1004
#define JOB_INT_MASK            0x1008
#define JS(n)                   (0x1800 + 0x80 * (n))
#define JS_HEAD_LO              0x00
#define JS_STATUS               0x24
#define JS_COMMAND              0x20
#define JS_HEAD_NEXT_LO         0x40
#define JS_AFFINITY_NEXT_LO     0x50
#define JS_CONFIG_NEXT          0x58
#define JS_COMMAND_NEXT         0x60
#define MMU_INT_RAWSTAT         0x2000
#define MMU_INT_CLEAR           0x2004
#define MMU_INT_MASK            0x2008
#define AS0                     0x2400
#define AS_TRANSTAB_LO          0x00
#define AS_MEMATTR_LO           0x08
#define AS_LOCKADDR_LO          0x10
#define AS_COMMAND              0x18
#define AS_FAULTSTATUS          0x1c
#define AS_FAULTADDR_LO         0x20
#define AS_STATUS               0x28
#define AS_TRANSCFG_LO          0x30

#define AS_CMD_UPDATE           1
#define AS_CMD_LOCK             2
#define AS_CMD_FLUSH_PT         4
#define AS_CMD_FLUSH_MEM        5
#define JS_CMD_START            1
#define JS_CMD_HARD_STOP        3
#define JS_CONFIG_DEFAULT       (8u << 16 | 3u << 8 | 3u << 12) /* pri 8, flush start+end, AS 0 */
#define PTE_TABLE               3ULL
#define PTE_LEAF                (1ULL | 1ULL << 10 | 3ULL << 6 | 2ULL << 8)
#define PTE_XN                  (1ULL << 54)
#define PTE_ADDR                0x000000fffffff000ULL
#define MEMATTR                 0x00888d88u     /* NC (imp. def.), WA cached, NC */
#define REQ_FS                  1               /* PANFROST_JD_REQ_FS */
#define BO_NOEXEC               1
#define BO_HEAP                 2

struct s8_bo {
	uint64_t pa;            /* backing (contiguous) */
	uint64_t backed;        /* bytes of backing */
	uint64_t va;            /* GPU virtual address */
	uint64_t vsize;         /* bytes of GPU VA */
	uint32_t flags;
	/* one descriptor per BO: IOConnectUnmapMemory finds the mapping through it (c38) */
	IOMemoryDescriptor *md;
	OSObject *owner;        /* imported IOSurface (its pages, not the pool's), or NULL */
};

class S8MaliUserClient;

class S8Mali : public IOService {
	OSDeclareDefaultStructors(S8Mali);

public:
	volatile uint8_t *fRegs;
	volatile uint8_t *fCmu;
	volatile uint8_t *fPmu;
	volatile uint8_t *fTables;      /* POOL_PHYS, TABLES_SIZE */
	volatile uint8_t *fBOs;         /* BO_PHYS, BO_SIZE */
	IOLock *fLock;
	uint64_t fPageMap[(BO_UNITS + 63) / 64];
	uint64_t fVAMap[(VA_UNITS + 63) / 64];
	uint64_t fTableMap[(TABLE_PAGES + 63) / 64];
	uint32_t fFeat[48];             /* DRM_PANFROST_PARAM_* values */
	uint32_t fJobs, fFaults, fResets, fPowerUps;
	bool fPowered;                  /* L2, shader cores and tiler on, AS 0 set up */
	uint64_t fLastUse;              /* mach_absolute_time of the last job */

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;

	uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(fRegs + off); }
	void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(fRegs + off) = v; }

	bool powerDomainOn(void);
	bool hwInit(void);
	void powerDown(void);
	static void idleThread(void *arg, wait_result_t wr);
	bool waitReg(uint32_t off, uint32_t mask, uint32_t want, uint32_t us);
	void asCommand(uint32_t cmd);
	void asLockFlush(uint64_t va, uint64_t size, uint32_t cmd);
	void readFeatures(void);

	uint64_t tablePA(uint32_t idx) { return POOL_PHYS + ((uint64_t)idx << 12); }
	volatile uint64_t *tableVA(uint64_t pa) { return (volatile uint64_t *)(fTables + (pa - POOL_PHYS)); }
	int64_t allocTable(void);
	volatile uint64_t *l3For(uint64_t va, bool create);
	bool mapRange(uint64_t va, uint64_t pa, uint64_t size, bool noexec);
	void unmapRange(uint64_t va, uint64_t size);

	s8_bo *boCreate(uint64_t size, uint32_t flags);
	s8_bo *boImportSurface(uint32_t id);
	void boSync(s8_bo *bo);
	void boFree(s8_bo *bo);
	void *boKernel(s8_bo *bo) { return (void *)(fBOs + (bo->pa - BO_PHYS)); }

	uint32_t submit(uint64_t jc, uint32_t req);
	void recover(const char *why);
	bool selfTest(void);
};

OSDefineMetaClassAndStructors(S8Mali, IOService);

class S8MaliUserClient : public IOUserClient {
	OSDeclareDefaultStructors(S8MaliUserClient);

public:
	S8Mali *fGPU;
	task_t fTask;
	s8_bo **fHandles;
	uint32_t fBOs, fSubmits, fDone;

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	void freeAll(void);
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
	    IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	s8_bo *lookup(uint64_t handle);
};

OSDefineMetaClassAndStructors(S8MaliUserClient, IOUserClient);

/* ---- bit maps ---- */

static bool
bm_test(const uint64_t *m, uint64_t i)
{
	return (m[i >> 6] >> (i & 63)) & 1;
}

static void
bm_set(uint64_t *m, uint64_t first, uint64_t n, bool v)
{
	for (uint64_t i = first; i < first + n; i++) {
		if (v) {
			m[i >> 6] |= 1ULL << (i & 63);
		} else {
			m[i >> 6] &= ~(1ULL << (i & 63));
		}
	}
}

/* First fit of n free units, start aligned to align units; -1 if none. */
static int64_t
bm_find(const uint64_t *m, uint64_t total, uint64_t n, uint64_t align)
{
	uint64_t i = 0;
	while (i + n <= total) {
		uint64_t run = 0;
		while (run < n && !bm_test(m, i + run)) {
			run++;
		}
		if (run == n) {
			return (int64_t)i;
		}
		i += run + 1;
		i = (i + align - 1) / align * align;
	}
	return -1;
}

static inline void
s8_dsb(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t
s8_ticks_us(uint64_t t)
{
	return t / 26;          /* 26 MHz timebase */
}

/* ---- hardware ---- */

bool
S8Mali::waitReg(uint32_t off, uint32_t mask, uint32_t want, uint32_t us)
{
	uint64_t end = mach_absolute_time() + (uint64_t)us * 26;
	do {
		if ((rd(off) & mask) == want) {
			return true;
		}
	} while (mach_absolute_time() < end);
	return (rd(off) & mask) == want;
}

/*
 * Samsung pmucal g3d_on (CMU_G3D PLL_CON2 = 0x30000003, PLL_CON0 bit 4,
 * MUX_CLKCMU_EMBEDDED_G3D_USER bit 4, PMU G3D CONFIGURATION = 0xf). The
 * domain is on at 260 MHz when S-Boot hands over (c29: PMU status 0x3000f,
 * PLL_G3D 0xa0a00412) unless s8gpuprobe turned it off; this only
 * repeats it if the domain is off.
 */
bool
S8Mali::powerDomainOn(void)
{
	volatile uint32_t *conf = (volatile uint32_t *)(fPmu + 0xe0);
	volatile uint32_t *stat = (volatile uint32_t *)(fPmu + 0xe4);
	volatile uint32_t *pll0 = (volatile uint32_t *)(fCmu + 0x140);
	volatile uint32_t *pll2 = (volatile uint32_t *)(fCmu + 0x148);
	volatile uint32_t *mux = (volatile uint32_t *)(fCmu + 0x100);
	uint32_t before = *stat;
	if ((before & 0xf) != 0xf) {
		*pll2 = 0x30000003u;
		*pll0 |= 1u << 4;
		*mux |= 1u << 4;
		*conf = (*conf & ~0xfu) | 0xfu;
		for (int i = 0; i < 1000 && (*stat & 0xf) != 0xf; i++) {
			IODelay(10);
		}
	}
	IOLog("S8 MALI: G3D domain 0x%x -> 0x%x, PLL_G3D 0x%x\n", before, *stat, *pll0);
	return (*stat & 0xf) == 0xf;
}

void
S8Mali::asCommand(uint32_t cmd)
{
	waitReg(AS0 + AS_STATUS, 1, 0, 100000);
	wr(AS0 + AS_COMMAND, cmd);
	waitReg(AS0 + AS_STATUS, 1, 0, 100000);
}

/* Lock the naturally aligned region around [va, va+size), then cmd. */
void
S8Mali::asLockFlush(uint64_t va, uint64_t size, uint32_t cmd)
{
	uint64_t end = va + size;
	uint64_t diff = va ^ (end - 1);
	uint32_t width = diff ? 64 - (uint32_t)__builtin_clzll(diff) : 0;
	if (width < 15) {
		width = 15;     /* AS_LOCK_REGION_MIN_SIZE 32 KiB */
	}
	width -= 1;
	uint64_t region = (va & ~((1ULL << width) - 1)) | width;
	wr(AS0 + AS_LOCKADDR_LO, (uint32_t)region);
	wr(AS0 + AS_LOCKADDR_LO + 4, (uint32_t)(region >> 32));
	asCommand(AS_CMD_LOCK);
	asCommand(cmd);
}

bool
S8Mali::hwInit(void)
{
	wr(GPU_INT_MASK, 0);
	wr(JOB_INT_MASK, 0);
	wr(MMU_INT_MASK, 0);
	wr(GPU_INT_CLEAR, 0xffffffffu);
	wr(GPU_CMD, 1);                                 /* SOFT_RESET */
	if (!waitReg(GPU_INT_RAWSTAT, 1u << 8, 1u << 8, 10000)) {
		wr(GPU_CMD, 2);                         /* HARD_RESET */
		if (!waitReg(GPU_INT_RAWSTAT, 1u << 8, 1u << 8, 10000)) {
			IOLog("S8 MALI: reset timed out\n");
			return false;
		}
	}
	wr(GPU_INT_CLEAR, 0xffffffffu);
	wr(GPU_TILER_CONFIG, rd(GPU_TILER_CONFIG) | 1); /* T76X_3953 */
	uint32_t l2 = rd(GPU_L2_PRESENT), sh = rd(GPU_SHADER_PRESENT), ti = rd(GPU_TILER_PRESENT);
	wr(GPU_L2_PWRON, l2);
	bool ok = waitReg(GPU_L2_READY, 0xffffffffu, l2, 20000);
	wr(GPU_SHADER_PWRON, sh);
	ok = waitReg(GPU_SHADER_READY, 0xffffffffu, sh, 20000) && ok;
	wr(GPU_TILER_PWRON, ti);
	ok = waitReg(GPU_TILER_READY, 0xffffffffu, ti, 20000) && ok;

	wr(MMU_INT_CLEAR, 0xffffffffu);
	asLockFlush(0, ~0ULL, AS_CMD_FLUSH_MEM);
	wr(AS0 + AS_TRANSCFG_LO, 0);                    /* legacy LPAE mode */
	wr(AS0 + AS_TRANSCFG_LO + 4, 0);
	uint64_t transtab = tablePA(0) | 4 | 3;         /* READ_INNER, ADRMODE_TABLE */
	wr(AS0 + AS_TRANSTAB_LO, (uint32_t)transtab);
	wr(AS0 + AS_TRANSTAB_LO + 4, (uint32_t)(transtab >> 32));
	wr(AS0 + AS_MEMATTR_LO, MEMATTR);
	wr(AS0 + AS_MEMATTR_LO + 4, 0);
	asCommand(AS_CMD_UPDATE);
	if (!ok) {
		IOLog("S8 MALI: power-up incomplete: L2 0x%x shaders 0x%x tiler 0x%x\n",
		    rd(GPU_L2_READY), rd(GPU_SHADER_READY), rd(GPU_TILER_READY));
	}
	fPowered = ok;
	return ok;
}

/*
 * Idle: shader cores and tiler off, then the L2 (kbase's order). The G3D domain and the
 * GPU_CONTROL block stay on; the next submit brings everything back with hwInit() (soft
 * reset, power-up, AS 0), the path the fault recovery already uses. While the cores are off
 * no MMU command is issued (mapRange/unmapRange skip the flush; hwInit flushes all).
 * Called with fLock held and no job running (submits are synchronous).
 */
void
S8Mali::powerDown(void)
{
	if (!fPowered) {
		return;
	}
	wr(GPU_SHADER_PWROFF, rd(GPU_SHADER_PRESENT));
	wr(GPU_TILER_PWROFF, rd(GPU_TILER_PRESENT));
	bool ok = waitReg(GPU_SHADER_PWRTRANS, 0xffffffffu, 0, 20000) &&
	    waitReg(GPU_TILER_PWRTRANS, 0xffffffffu, 0, 20000);
	wr(GPU_L2_PWROFF, rd(GPU_L2_PRESENT));
	ok = waitReg(GPU_L2_PWRTRANS, 0xffffffffu, 0, 20000) && ok;
	if (!ok) {
		IOLog("S8 MALI: power-down incomplete: shaders 0x%x tiler 0x%x L2 0x%x\n",
		    rd(GPU_SHADER_READY), rd(GPU_TILER_READY), rd(GPU_L2_READY));
	}
	fPowered = false;
}

void
S8Mali::idleThread(void *arg, wait_result_t wr __unused)
{
	S8Mali *gpu = (S8Mali *)arg;
	for (;;) {
		IOSleep(100);
		IOLockLock(gpu->fLock);
		if (gpu->fPowered && mach_absolute_time() - gpu->fLastUse > IDLE_OFF_TICKS) {
			gpu->powerDown();
		}
		IOLockUnlock(gpu->fLock);
	}
}

void
S8Mali::readFeatures(void)
{
	uint32_t id = rd(GPU_ID);
	fFeat[0] = id >> 16;                            /* GPU_PROD_ID */
	fFeat[1] = id & 0xffff;                         /* GPU_REVISION */
	fFeat[2] = rd(GPU_SHADER_PRESENT);
	fFeat[3] = rd(GPU_TILER_PRESENT);
	fFeat[4] = rd(GPU_L2_PRESENT);
	fFeat[5] = rd(GPU_STACK_PRESENT);
	fFeat[6] = rd(GPU_AS_PRESENT);
	fFeat[7] = rd(GPU_JS_PRESENT);
	fFeat[8] = rd(GPU_L2_FEATURES);
	fFeat[9] = rd(GPU_CORE_FEATURES);
	fFeat[10] = rd(GPU_TILER_FEATURES);
	fFeat[11] = rd(GPU_MEM_FEATURES);
	fFeat[12] = rd(GPU_MMU_FEATURES);
	fFeat[13] = rd(GPU_THREAD_FEATURES);
	fFeat[14] = rd(GPU_THREAD_MAX_THREADS);
	fFeat[15] = rd(GPU_THREAD_MAX_WG);
	fFeat[16] = rd(GPU_THREAD_MAX_BARRIER);
	fFeat[17] = rd(GPU_COHERENCY_FEATURES);
	for (int i = 0; i < 4; i++) {
		fFeat[18 + i] = rd(GPU_TEXTURE_FEATURES(i));
	}
	for (int i = 0; i < 16; i++) {
		fFeat[22 + i] = (fFeat[7] >> i) & 1 ? rd(GPU_JS_FEATURES(i)) : 0;
	}
	fFeat[38] = (uint32_t)__builtin_popcount(fFeat[4]);     /* NR_CORE_GROUPS */
	fFeat[39] = rd(GPU_THREAD_TLS_ALLOC);
	fFeat[40] = rd(GPU_AFBC_FEATURES);
}

/* ---- page tables (Mali LPAE, 4 levels, 4 KiB) ---- */

int64_t
S8Mali::allocTable(void)
{
	int64_t idx = bm_find(fTableMap, TABLE_PAGES, 1, 1);
	if (idx < 0) {
		return -1;
	}
	bm_set(fTableMap, (uint64_t)idx, 1, true);
	volatile uint64_t *t = tableVA(tablePA((uint32_t)idx));
	for (int i = 0; i < 512; i++) {
		t[i] = 0;
	}
	return idx;
}

volatile uint64_t *
S8Mali::l3For(uint64_t va, bool create)
{
	volatile uint64_t *t = tableVA(tablePA(0));
	for (int shift = 39; shift >= 21; shift -= 9) {
		volatile uint64_t *e = &t[(va >> shift) & 0x1ff];
		if ((*e & 3) != PTE_TABLE) {
			if (!create) {
				return NULL;
			}
			int64_t idx = allocTable();
			if (idx < 0) {
				return NULL;
			}
			*e = tablePA((uint32_t)idx) | PTE_TABLE;
		}
		t = tableVA(*e & PTE_ADDR);
	}
	return t;
}

bool
S8Mali::mapRange(uint64_t va, uint64_t pa, uint64_t size, bool noexec)
{
	for (uint64_t off = 0; off < size; off += 0x1000) {
		volatile uint64_t *l3 = l3For(va + off, true);
		if (l3 == NULL) {
			unmapRange(va, off);
			return false;
		}
		l3[((va + off) >> 12) & 0x1ff] = (pa + off) | PTE_LEAF | (noexec ? PTE_XN : 0);
	}
	s8_dsb();
	if (fPowered) {
		asLockFlush(va, size, AS_CMD_FLUSH_PT);
	}
	return true;
}

void
S8Mali::unmapRange(uint64_t va, uint64_t size)
{
	for (uint64_t off = 0; off < size; off += 0x1000) {
		volatile uint64_t *l3 = l3For(va + off, false);
		if (l3 != NULL) {
			l3[((va + off) >> 12) & 0x1ff] = 0;
		}
	}
	s8_dsb();
	if (size != 0 && fPowered) {
		asLockFlush(va, size, AS_CMD_FLUSH_PT);
	}
}

/* ---- buffer objects ---- */

s8_bo *
S8Mali::boCreate(uint64_t size, uint32_t flags)
{
	if (size == 0 || size > VA_END - VA_BASE) {
		return NULL;
	}
	uint64_t vsize = (size + VA_UNIT - 1) & ~(VA_UNIT - 1);
	uint64_t backed = (flags & BO_HEAP) && vsize > HEAP_BACKED_MAX ? HEAP_BACKED_MAX : vsize;
	if (backed > BO_SIZE) {
		return NULL;
	}
	int64_t unit = bm_find(fPageMap, BO_UNITS, backed / BO_UNIT, 1);
	if (unit < 0) {
		return NULL;
	}
	int64_t vunit = bm_find(fVAMap, VA_UNITS, vsize / VA_UNIT, 1);
	if (vunit < 0) {
		return NULL;
	}
	s8_bo *bo = (s8_bo *)IOMallocZero(sizeof(*bo));
	if (bo == NULL) {
		return NULL;
	}
	bo->pa = BO_PHYS + (uint64_t)unit * BO_UNIT;
	bo->backed = backed;
	bo->va = VA_BASE + (uint64_t)vunit * VA_UNIT;
	bo->vsize = vsize;
	bo->flags = flags;
	bzero(boKernel(bo), (size_t)backed);
	bo->md = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)bo->pa, (IOByteCount)backed,
	    kIODirectionInOut);
	if (bo->md == NULL || !mapRange(bo->va, bo->pa, backed, (flags & BO_NOEXEC) != 0)) {
		OSSafeReleaseNULL(bo->md);
		IOFree(bo, sizeof(*bo));
		return NULL;
	}
	bm_set(fPageMap, (uint64_t)unit, backed / BO_UNIT, true);
	bm_set(fVAMap, (uint64_t)vunit, vsize / VA_UNIT, true);
	return bo;
}

/*
 * An IOSurface's memory as a BO, without copying: its physical pages are mapped into the GPU
 * VA one 4 KiB page at a time (SweetSurface buffers are wired kernel memory, not contiguous;
 * the low-DRAM stage surfaces are contiguous). The CPU maps these pages cached, the GPU is
 * not coherent: callers use SyncBO around GPU use.
 */
s8_bo *
S8Mali::boImportSurface(uint32_t id)
{
	SweetSurface *surf = sweet_surface_copy(id);
	if (surf == NULL) {
		return NULL;
	}
	IOMemoryDescriptor *md = surf->memory;
	uint64_t size = md != NULL ? (md->getLength() + 0xfff) & ~0xfffULL : 0;
	uint64_t vsize = (size + VA_UNIT - 1) & ~(VA_UNIT - 1);
	int64_t vunit = size != 0 ? bm_find(fVAMap, VA_UNITS, vsize / VA_UNIT, 1) : -1;
	s8_bo *bo = vunit >= 0 ? (s8_bo *)IOMallocZero(sizeof(*bo)) : NULL;
	if (bo == NULL || md->prepare() != kIOReturnSuccess) {
		if (bo != NULL) {
			IOFree(bo, sizeof(*bo));
		}
		surf->release();
		return NULL;
	}
	bo->va = VA_BASE + (uint64_t)vunit * VA_UNIT;
	bo->vsize = vsize;
	bo->backed = size;
	bo->flags = BO_NOEXEC;
	bo->owner = surf;
	md->retain();
	bo->md = md;
	bool ok = true;
	for (uint64_t off = 0; ok && off < size;) {
		IOByteCount len = 0;
		addr64_t pa = md->getPhysicalSegment(off, &len, kIOMemoryMapperNone);
		len = len > size - off ? size - off : len;
		ok = pa != 0 && (pa & 0xfff) == 0 && len >= 0x1000 &&
		    mapRange(bo->va + off, pa, len & ~0xfffULL, true);
		off += len & ~0xfffULL;
	}
	if (!ok) {
		unmapRange(bo->va, size);
		md->complete();
		md->release();
		surf->release();
		IOFree(bo, sizeof(*bo));
		return NULL;
	}
	bm_set(fVAMap, (uint64_t)vunit, vsize / VA_UNIT, true);
	return bo;
}

void
S8Mali::boSync(s8_bo *bo)
{
	for (uint64_t off = 0; off < bo->backed;) {
		IOByteCount len = 0;
		addr64_t pa = bo->owner != NULL ? bo->md->getPhysicalSegment(off, &len, kIOMemoryMapperNone) :
		    bo->pa + off;
		if (bo->owner == NULL) {
			len = (IOByteCount)(bo->backed - off);
		}
		if (pa == 0 || len == 0) {
			break;
		}
		len = len > bo->backed - off ? (IOByteCount)(bo->backed - off) : len;
		/* XNU RAM only: pool BOs and the low-DRAM stage surfaces are mapped
		 * write-combined (no cache lines), and outside the physical aperture */
		if (pmap_valid_address(pa)) {
			flush_dcache64(pa, (unsigned)len, TRUE);
		}
		off += len;
	}
	s8_dsb();
}

void
S8Mali::boFree(s8_bo *bo)
{
	unmapRange(bo->va, bo->backed);
	if (bo->owner != NULL) {
		bm_set(fVAMap, (bo->va - VA_BASE) / VA_UNIT, bo->vsize / VA_UNIT, false);
		bo->md->complete();
		OSSafeReleaseNULL(bo->md);
		OSSafeReleaseNULL(bo->owner);
		IOFree(bo, sizeof(*bo));
		return;
	}
	bm_set(fPageMap, (bo->pa - BO_PHYS) / BO_UNIT, bo->backed / BO_UNIT, false);
	bm_set(fVAMap, (bo->va - VA_BASE) / VA_UNIT, bo->vsize / VA_UNIT, false);
	OSSafeReleaseNULL(bo->md);
	IOFree(bo, sizeof(*bo));
}

/* ---- jobs ---- */

/*
 * Soft reset and full re-init after a fault or a timeout. The page tables
 * live in RAM and survive; only the GPU's view of them is set up again.
 */
void
S8Mali::recover(const char *why)
{
	fResets++;
	IOLog("S8 MALI: reset (%s): GPU_FAULT 0x%x AS0 fault 0x%x at 0x%x%08x\n", why,
	    rd(GPU_FAULT_STATUS), rd(AS0 + AS_FAULTSTATUS),
	    rd(AS0 + AS_FAULTADDR_LO + 4), rd(AS0 + AS_FAULTADDR_LO));
	hwInit();
}

/*
 * Runs the job chain at GPU VA jc to completion. Fragment jobs (REQ_FS) go
 * to slot 0, everything else to slot 1, as in panfrost. Returns the JS
 * exception status: 0x01 DONE, the job's fault code (0x40 and up), the AS
 * fault type for an MMU fault (0xc0 and up), or 0x03 for a timeout. Called
 * with fLock held.
 */
uint32_t
S8Mali::submit(uint64_t jc, uint32_t req)
{
	uint32_t slot = (req & REQ_FS) ? 0 : 1;
	uint32_t done = 1u << slot, fail = 1u << (16 + slot);
	uint32_t js = JS(slot);
	uint32_t sh = rd(GPU_SHADER_PRESENT);

	if (!fPowered) {
		fPowerUps++;
		if (!hwInit()) {
			return 0x03;
		}
	}
	fLastUse = mach_absolute_time();
	fJobs++;
	wr(JOB_INT_CLEAR, done | fail);
	wr(MMU_INT_CLEAR, 0x10001);
	wr(js + JS_HEAD_NEXT_LO, (uint32_t)jc);
	wr(js + JS_HEAD_NEXT_LO + 4, (uint32_t)(jc >> 32));
	wr(js + JS_AFFINITY_NEXT_LO, sh);
	wr(js + JS_AFFINITY_NEXT_LO + 4, 0);
	wr(js + JS_CONFIG_NEXT, JS_CONFIG_DEFAULT);
	s8_dsb();
	wr(js + JS_COMMAND_NEXT, JS_CMD_START);

	uint64_t start = mach_absolute_time();
	uint64_t deadline = start + 26ULL * 2000000;    /* 2 s */
	uint32_t raw = 0, spins = 0;
	for (;;) {
		raw = rd(JOB_INT_RAWSTAT);
		if (raw & (done | fail)) {
			break;
		}
		if (rd(MMU_INT_RAWSTAT) & 0x10001) {     /* AS 0 page fault or bus error */
			uint32_t fs = rd(AS0 + AS_FAULTSTATUS);
			fFaults++;
			wr(js + JS_COMMAND_NEXT, 0);
			wr(js + JS_COMMAND, JS_CMD_HARD_STOP);
			recover("MMU fault");
			return 0xc0 | (fs & 0x3f);
		}
		if (mach_absolute_time() > deadline) {
			fFaults++;
			wr(js + JS_COMMAND_NEXT, 0);
			wr(js + JS_COMMAND, JS_CMD_HARD_STOP);
			waitReg(JOB_INT_RAWSTAT, done | fail, fail, 50000);
			recover("job timeout");
			return 0x03;
		}
		if (++spins > 2000) {
			IOSleep(1);     /* long job: stop spinning */
		}
	}
	wr(JOB_INT_CLEAR, done | fail);
	fLastUse = mach_absolute_time();
	if (raw & done) {
		return 0x01;
	}
	uint32_t status = rd(js + JS_STATUS);
	fFaults++;
	IOLog("S8 MALI: job 0x%llx on slot %u failed, JS_STATUS 0x%x\n", jc, slot, status);
	if (status >= 0x40 && status != 0x40) {
		recover("job fault");
	}
	return status ? status : 0x40;
}

/* ---- boot self-test (s8mali=2) ---- */

static void
s8_write_value_job(uint8_t *job, uint64_t next, uint16_t index, uint64_t target, uint32_t value)
{
	volatile uint32_t *w = (volatile uint32_t *)job;
	for (int i = 0; i < 16; i++) {
		w[i] = 0;
	}
	/* Job header (Mesa genxml v6): is 64b, type 2 = WRITE_VALUE, index */
	w[4] = 1u | 2u << 1 | (uint32_t)index << 16;
	w[6] = (uint32_t)next;
	w[7] = (uint32_t)(next >> 32);
	/* Write value payload: address, type 6 = immediate 32, value */
	w[8] = (uint32_t)target;
	w[9] = (uint32_t)(target >> 32);
	w[10] = 6;
	w[12] = value;
}

bool
S8Mali::selfTest(void)
{
	bool ok = true;
	char summary[160];
	s8_bo *jobs = boCreate(0x10000, BO_NOEXEC);
	s8_bo *data = boCreate(0x10000, BO_NOEXEC);
	if (jobs == NULL || data == NULL) {
		IOLog("S8 MALI: self-test: no memory\n");
		return false;
	}
	uint8_t *jk = (uint8_t *)boKernel(jobs);
	volatile uint32_t *dk = (volatile uint32_t *)boKernel(data);

	/* T1: one WRITE_VALUE job */
	dk[0] = 0;
	s8_write_value_job(jk, 0, 1, data->va, 0xc0ffee10u);
	uint32_t st1 = submit(jobs->va, 0);
	bool t1 = st1 == 1 && dk[0] == 0xc0ffee10u && jk[0] == 1;
	IOLog("S8 MALI: T1 single job: status 0x%x word 0x%x header 0x%x -> %s\n",
	    st1, dk[0], jk[0], t1 ? "ok" : "FAIL");
	ok = ok && t1;

	/* T2: a chain of 64 jobs, each writing its own word */
	for (uint32_t i = 0; i < 64; i++) {
		dk[16 + i] = 0;
		uint64_t next = i + 1 < 64 ? jobs->va + 64 * (i + 1) : 0;
		s8_write_value_job(jk + 64 * i, next, (uint16_t)(i + 1), data->va + 4 * (16 + i), 0x5a000000u | i);
	}
	uint32_t st2 = submit(jobs->va, 0);
	uint32_t good = 0;
	for (uint32_t i = 0; i < 64; i++) {
		good += dk[16 + i] == (0x5a000000u | i);
	}
	bool t2 = st2 == 1 && good == 64;
	IOLog("S8 MALI: T2 chain of 64: status 0x%x, %u/64 words -> %s\n", st2, good, t2 ? "ok" : "FAIL");
	ok = ok && t2;

	/* T3: 1000 submissions on both slots, latency */
	uint32_t bad = 0;
	uint64_t t0 = mach_absolute_time();
	for (uint32_t i = 0; i < 1000; i++) {
		s8_write_value_job(jk, 0, 1, data->va + 0x100, i);
		if (submit(jobs->va, (i & 1) ? REQ_FS : 0) != 1 || dk[0x40] != i) {
			bad++;
		}
	}
	uint64_t us = s8_ticks_us(mach_absolute_time() - t0);
	bool t3 = bad == 0;
	IOLog("S8 MALI: T3 1000 jobs: %u bad, %llu us total, %llu us each -> %s\n",
	    bad, us, us / 1000, t3 ? "ok" : "FAIL");
	ok = ok && t3;

	/* T4: a write to an unmapped address must fault, and the GPU recover */
	uint32_t resets = fResets;
	s8_write_value_job(jk, 0, 1, 0x1000, 1);
	uint32_t st4 = submit(jobs->va, 0);
	dk[0] = 0;
	s8_write_value_job(jk, 0, 1, data->va, 0xc0ffee11u);
	uint32_t st4b = submit(jobs->va, 0);
	bool t4 = st4 >= 0x40 && fResets == resets + 1 && st4b == 1 && dk[0] == 0xc0ffee11u;
	IOLog("S8 MALI: T4 fault: status 0x%x, resets %u, after reset status 0x%x word 0x%x -> %s\n",
	    st4, fResets - resets, st4b, dk[0], t4 ? "ok" : "FAIL");
	ok = ok && t4;

	/* T5: cores powered off (idle), then a job: it must power them up again */
	powerDown();
	uint32_t ups = fPowerUps;
	dk[0] = 0;
	s8_write_value_job(jk, 0, 1, data->va, 0xc0ffee12u);
	uint64_t p0 = mach_absolute_time();
	uint32_t st5 = submit(jobs->va, 0);
	uint64_t up_us = s8_ticks_us(mach_absolute_time() - p0);
	bool t5 = st5 == 1 && dk[0] == 0xc0ffee12u && fPowerUps == ups + 1 && fPowered;
	IOLog("S8 MALI: T5 power-up on submit: status 0x%x word 0x%x, %llu us -> %s\n",
	    st5, dk[0], up_us, t5 ? "ok" : "FAIL");
	ok = ok && t5;

	boFree(jobs);
	boFree(data);
	snprintf(summary, sizeof(summary), "T1 %s T2 %s T3 %s (%llu us/job) T4 %s T5 %s (%llu us)",
	    t1 ? "ok" : "fail", t2 ? "ok" : "fail", t3 ? "ok" : "fail", us / 1000, t4 ? "ok" : "fail",
	    t5 ? "ok" : "fail", up_us);
	setProperty("S8MaliSelfTest", summary);
	IOLog("S8 MALI: self-test %s: %s\n", ok ? "passed" : "FAILED", summary);
	return ok;
}

/* ---- service ---- */

bool
S8Mali::start(IOService *provider)
{
	uint32_t mode = 0;
	if (!PE_parse_boot_argn("s8mali", &mode, sizeof(mode)) || mode == 0) {
		return false;
	}
	if (!IOService::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fRegs = (volatile uint8_t *)ml_io_map(MALI_PHYS, 0x4000);
	fCmu = (volatile uint8_t *)ml_io_map(CMU_G3D_PHYS, 0x1000);
	fPmu = (volatile uint8_t *)ml_io_map(PMU_G3D_PHYS, 0x1000);
	if (fLock == NULL || fRegs == NULL || fCmu == NULL || fPmu == NULL) {
		IOLog("S8 MALI: cannot map registers\n");
		return false;
	}
	if (!powerDomainOn()) {
		IOLog("S8 MALI: G3D power domain did not come up\n");
		return false;
	}
	uint32_t id = rd(GPU_ID);
	if (((id >> 16) & 0xf00f) != 0x6000) {        /* model as panfrost: G71 = 0x6000 */
		IOLog("S8 MALI: unexpected GPU_ID 0x%x, not a Mali-G71\n", id);
		return false;
	}
	vm_offset_t pool = ml_io_map_unmappable(POOL_PHYS, POOL_SIZE, VM_WIMG_WCOMB);
	if (pool == 0) {
		IOLog("S8 MALI: cannot map the GPU memory pool\n");
		return false;
	}
	fTables = (volatile uint8_t *)pool;
	fBOs = (volatile uint8_t *)(pool + TABLES_SIZE);
	bzero((void *)fTables, (size_t)TABLES_SIZE);
	bm_set(fTableMap, 0, 1, true);                  /* level 0 root */
	s8_dsb();

	IOLockLock(fLock);
	bool up = hwInit();
	readFeatures();
	IOLog("S8 MALI: GPU_ID 0x%x shaders 0x%x L2 0x%x JS 0x%x AS 0x%x MMU 0x%x coherency 0x%x, %s\n",
	    id, fFeat[2], fFeat[4], fFeat[7], fFeat[6], fFeat[12], fFeat[17], up ? "powered" : "power-up incomplete");
	bool tested = !up || mode < 2 || selfTest();
	IOLockUnlock(fLock);
	if (!up) {
		return false;
	}

	setProperty("GPU_ID", id, 32);
	setProperty("ShaderPresent", fFeat[2], 32);
	setProperty("S8MaliPool", POOL_SIZE, 64);
	setProperty(kIOUserClientClassKey, "S8MaliUserClient");
	uint32_t idle = 1;
	PE_parse_boot_argn("s8maliidle", &idle, sizeof(idle));
	thread_t t;
	fLastUse = mach_absolute_time();
	if (idle && kernel_thread_start((thread_continue_t)idleThread, this, &t) == KERN_SUCCESS) {
		thread_deallocate(t);
	}
	registerService();
	IOLog("S8 MALI: registered (s8mali=%u)%s\n", mode, tested ? "" : ", self-test failed");
	return true;
}

/* ---- user client ---- */

bool
S8MaliUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	fHandles = (s8_bo **)IOMallocZero(sizeof(s8_bo *) * MAX_HANDLES);
	return fHandles != NULL;
}

bool
S8MaliUserClient::start(IOService *provider)
{
	fGPU = OSDynamicCast(S8Mali, provider);
	return fGPU != NULL && IOUserClient::start(provider);
}

/* Frees every BO the client still holds. */
void
S8MaliUserClient::freeAll(void)
{
	if (fHandles == NULL || fGPU == NULL) {
		return;
	}
	IOLockLock(fGPU->fLock);
	for (uint32_t i = 0; i < MAX_HANDLES; i++) {
		if (fHandles[i] != NULL) {
			fGPU->boFree(fHandles[i]);
			fHandles[i] = NULL;
		}
	}
	IOLockUnlock(fGPU->fLock);
}

IOReturn
S8MaliUserClient::clientClose(void)
{
	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("S8 MALI: %s closed: %u BOs, %u submits, %u done; GPU totals %u jobs, %u faults, %u resets, %u power-ups\n",
	    name, fBOs, fSubmits, fDone, fGPU->fJobs, fGPU->fFaults, fGPU->fResets, fGPU->fPowerUps);
	freeAll();
	terminate();
	return kIOReturnSuccess;
}

void
S8MaliUserClient::free(void)
{
	freeAll();
	if (fHandles != NULL) {
		IOFree(fHandles, sizeof(s8_bo *) * MAX_HANDLES);
		fHandles = NULL;
	}
	IOUserClient::free();
}

s8_bo *
S8MaliUserClient::lookup(uint64_t handle)
{
	return handle >= 1 && handle <= MAX_HANDLES ? fHandles[handle - 1] : NULL;
}

/* type = BO handle; map with kIOMapAnywhere | kIOMapWriteCombineCache. */
IOReturn
S8MaliUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
    IOMemoryDescriptor **memory)
{
	IOLockLock(fGPU->fLock);
	s8_bo *bo = lookup(type);
	IOMemoryDescriptor *md = bo != NULL ? bo->md : NULL;
	if (md != NULL) {
		md->retain();                   /* the caller releases it */
	}
	IOLockUnlock(fGPU->fLock);
	if (md == NULL) {
		return kIOReturnBadArgument;
	}
	*options = kIOMapWriteCombineCache;
	*memory = md;
	return kIOReturnSuccess;
}

IOReturn
S8MaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch __unused, OSObject *target __unused, void *reference __unused)
{
	static const uint8_t ins[] = { 1, 2, 1, 2, 2, 1, 2, 3, 1, 1, 0 };
	static const uint8_t outs[] = { 1, 2, 0, 1, 0, 1, 1, 0, 3, 0, 0 };
	if (selector == 10) {                           /* Log: no GPU state, no lock */
		const char *text = (const char *)args->structureInput;
		uint32_t len = args->structureInputSize;
		char name[32], line[256];
		if (text == NULL || len == 0 || len > 4096) {
			return kIOReturnBadArgument;
		}
		proc_selfname(name, sizeof(name));
		for (uint32_t at = 0; at < len;) {
			uint32_t n = 0;
			while (at + n < len && text[at + n] != '\n' && text[at + n] != 0 && n < sizeof(line) - 1) {
				line[n] = text[at + n];
				n++;
			}
			line[n] = 0;
			if (n > 0) {
				IOLog("S8 LOG %s: %s\n", name, line);
			}
			at += n;
			while (at < len && (text[at] == '\n' || text[at] == 0)) {
				at++;
			}
		}
		return kIOReturnSuccess;
	}
	if (selector >= sizeof(ins) || args->scalarInputCount != ins[selector] ||
	    args->scalarOutputCount != outs[selector]) {
		return kIOReturnBadArgument;
	}
	const uint64_t *in = args->scalarInput;
	uint64_t *out = args->scalarOutput;
	IOReturn ret = kIOReturnSuccess;

	IOLockLock(fGPU->fLock);
	switch (selector) {
	case 0:                                         /* GetParam */
		if (in[0] < 41) {
			out[0] = fGPU->fFeat[in[0]];
		} else {
			ret = kIOReturnBadArgument;
		}
		break;
	case 1: {                                       /* CreateBO */
		uint32_t h = 0;
		while (h < MAX_HANDLES && fHandles[h] != NULL) {
			h++;
		}
		s8_bo *bo = h < MAX_HANDLES ? fGPU->boCreate(in[0], (uint32_t)in[1] & (BO_NOEXEC | BO_HEAP)) : NULL;
		if (bo == NULL) {
			ret = kIOReturnNoMemory;
			break;
		}
		fHandles[h] = bo;
		fBOs++;
		out[0] = h + 1;
		out[1] = bo->va;
		break;
	}
	case 2: {                                       /* FreeBO */
		s8_bo *bo = lookup(in[0]);
		if (bo == NULL) {
			ret = kIOReturnBadArgument;
			break;
		}
		fGPU->boFree(bo);
		fHandles[in[0] - 1] = NULL;
		break;
	}
	case 3:                                         /* Submit */
		out[0] = fGPU->submit(in[0], (uint32_t)in[1]);
		fSubmits++;
		fDone += out[0] == 1;
		break;
	case 4:                                         /* WaitBO: submit is synchronous */
		ret = lookup(in[0]) != NULL ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case 5: {                                       /* GetBOOffset */
		s8_bo *bo = lookup(in[0]);
		if (bo == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			out[0] = bo->va;
		}
		break;
	}
	case 6:                                         /* Madvise: never purged */
		ret = lookup(in[0]) != NULL ? kIOReturnSuccess : kIOReturnBadArgument;
		out[0] = 1;
		break;
	case 8: {                                       /* ImportSurface */
		uint32_t h = 0;
		while (h < MAX_HANDLES && fHandles[h] != NULL) {
			h++;
		}
		s8_bo *bo = h < MAX_HANDLES && in[0] <= 0xffffffffULL ? fGPU->boImportSurface((uint32_t)in[0]) : NULL;
		if (bo == NULL) {
			ret = kIOReturnBadArgument;
			break;
		}
		fHandles[h] = bo;
		fBOs++;
		out[0] = h + 1;
		out[1] = bo->va;
		out[2] = bo->backed;
		break;
	}
	case 9: {                                       /* SyncBO */
		s8_bo *bo = lookup(in[0]);
		if (bo == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			fGPU->boSync(bo);
		}
		break;
	}
	case 7: {                                       /* Report */
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("S8 MALI: %s report %llu: 0x%llx 0x%llx\n", name, in[0], in[1], in[2]);
		break;
	}
	}
	IOLockUnlock(fGPU->fLock);
	return ret;
}

#endif /* DREAMLTE */
