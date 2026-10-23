/*
 * s8malitest: user-space check of the S8Mali user client (GPU milestone 3).
 * Opens the S8Mali service, creates and maps buffer objects, builds
 * WRITE_VALUE jobs in mapped memory, submits them through the user client
 * and checks the words the GPU wrote. Results go to stdout and, through the
 * Report selector, to the kernel log ("S8 MALI: s8malitest report ...").
 *
 *   xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 \
 *       -framework IOKit -framework CoreFoundation -o s8malitest s8malitest.c
 *   codesign -s - s8malitest
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>

/* IOKit.framework has no public headers in the iOS SDK. */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern const mach_port_t kIOMainPortDefault;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t main, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallScalarMethod(io_connect_t connection, uint32_t selector,
    const uint64_t *input, uint32_t inputCnt, uint64_t *output, uint32_t *outputCnt);
extern kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, uint32_t options);

#define kIOMapAnywhere          0x00000001
#define kIOMapWriteCombineCache 0x00000400

enum { SEL_PARAM, SEL_CREATE, SEL_FREE, SEL_SUBMIT, SEL_WAIT, SEL_OFFSET, SEL_MADVISE, SEL_REPORT };

static io_connect_t conn;

static kern_return_t
call(uint32_t sel, const uint64_t *in, uint32_t nin, uint64_t *out, uint32_t nout)
{
	uint32_t n = nout;
	return IOConnectCallScalarMethod(conn, sel, in, nin, out, &n);
}

static void
report(uint64_t code, uint64_t a, uint64_t b)
{
	uint64_t in[3] = { code, a, b };
	call(SEL_REPORT, in, 3, NULL, 0);
	printf("report %llu: 0x%llx 0x%llx\n", code, a, b);
}

struct bo {
	uint64_t handle, va;
	volatile uint32_t *cpu;
};

static int
bo_create(struct bo *bo, uint64_t size, uint64_t flags)
{
	uint64_t in[2] = { size, flags }, out[2];
	if (call(SEL_CREATE, in, 2, out, 2) != KERN_SUCCESS) {
		return -1;
	}
	bo->handle = out[0];
	bo->va = out[1];
	mach_vm_address_t addr = 0;
	mach_vm_size_t len = 0;
	if (IOConnectMapMemory64(conn, (uint32_t)bo->handle, mach_task_self(), &addr, &len,
	    kIOMapAnywhere | kIOMapWriteCombineCache) != KERN_SUCCESS) {
		return -1;
	}
	bo->cpu = (volatile uint32_t *)addr;
	return 0;
}

/* Mesa genxml v6 job header (is 64b, type 2 = WRITE_VALUE) + payload. */
static void
write_value_job(volatile uint32_t *w, uint64_t next, uint16_t index, uint64_t target, uint32_t value)
{
	for (int i = 0; i < 16; i++) {
		w[i] = 0;
	}
	w[4] = 1u | 2u << 1 | (uint32_t)index << 16;
	w[6] = (uint32_t)next;
	w[7] = (uint32_t)(next >> 32);
	w[8] = (uint32_t)target;
	w[9] = (uint32_t)(target >> 32);
	w[10] = 6;
	w[12] = value;
}

static uint64_t
submit(uint64_t jc, uint64_t req)
{
	uint64_t in[2] = { jc, req }, out[1] = { 0 };
	__asm__ volatile ("dsb sy" ::: "memory");
	if (call(SEL_SUBMIT, in, 2, out, 1) != KERN_SUCCESS) {
		return 0xdead;
	}
	return out[0];
}

int
main(void)
{
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("S8Mali"));
	if (svc == 0) {
		printf("no S8Mali service\n");
		return 1;
	}
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS) {
		printf("IOServiceOpen: 0x%x\n", kr);
		return 2;
	}

	uint64_t p, v[3];
	for (p = 0; p < 3; p++) {
		uint64_t in = p == 0 ? 0 : p == 1 ? 1 : 2;     /* PROD_ID, REVISION, SHADER_PRESENT */
		if (call(SEL_PARAM, &in, 1, &v[p], 1) != KERN_SUCCESS) {
			v[p] = ~0ULL;
		}
	}
	report(1, v[0] << 16 | v[1], v[2]);

	struct bo jobs, data;
	if (bo_create(&jobs, 0x10000, 1) != 0 || bo_create(&data, 0x10000, 1) != 0) {
		report(90, 0, 0);
		IOServiceClose(conn);
		return 3;
	}
	report(2, jobs.va, data.va);

	/* one job */
	data.cpu[0] = 0;
	write_value_job(jobs.cpu, 0, 1, data.va, 0x5eed0001u);
	uint64_t st = submit(jobs.va, 0);
	report(3, st, data.cpu[0]);
	int ok = st == 1 && data.cpu[0] == 0x5eed0001u;

	/* 2000 jobs on both slots, timed */
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	uint64_t bad = 0, t0 = mach_absolute_time();
	for (uint32_t i = 0; i < 2000; i++) {
		write_value_job(jobs.cpu, 0, 1, data.va + 0x40, i);
		if (submit(jobs.va, i & 1) != 1 || data.cpu[0x10] != i) {
			bad++;
		}
	}
	uint64_t ns = (mach_absolute_time() - t0) * tb.numer / tb.denom;
	report(4, bad, ns / 2000);
	ok = ok && bad == 0;

	/* a buffer large enough for a frame: 1440x2960x4 */
	struct bo big;
	int bigok = bo_create(&big, 1440 * 2960 * 4, 1) == 0;
	if (bigok) {
		big.cpu[0] = 0;
		big.cpu[1440 * 2960 - 1] = 0;
		write_value_job(jobs.cpu, jobs.va + 64, 1, big.va, 0xf00d0001u);
		write_value_job(jobs.cpu + 16, 0, 2, big.va + 4ULL * (1440 * 2960 - 1), 0xf00d0002u);
		st = submit(jobs.va, 0);
		bigok = st == 1 && big.cpu[0] == 0xf00d0001u && big.cpu[1440 * 2960 - 1] == 0xf00d0002u;
		report(5, st, bigok);
	} else {
		report(91, 0, 0);
	}
	ok = ok && bigok;

	report(ok ? 100 : 99, ok, 0);
	IOServiceClose(conn);
	return ok ? 0 : 4;
}
