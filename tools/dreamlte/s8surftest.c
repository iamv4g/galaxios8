/*
 * s8surftest: the Mali writes into an IOSurface without a copy (GPU milestone 5 building block).
 * An IOSurface (the memory CoreAnimation and backboardd render into) is created, filled by the
 * CPU, imported into the GPU's address space (S8Mali selector 8), and a chain of WRITE_VALUE
 * jobs writes a diagonal into it. The CPU then reads it back through the surface's own mapping.
 * Cache maintenance around the GPU work: S8Mali selector 9 (SyncBO).
 * Reports ("S8 MALI: s8surftest report N: a b"):
 *    1  surface: ID, bytes per row        2  import: GPU VA, size
 *    3  jobs: JS status, diagonal words right (of 256)
 *    4  untouched pixels still the CPU's fill (of 256 probes), first wrong value
 *  100 / 99  passed / failed
 *
 *   xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -o s8surftest \
 *       s8surftest.c -framework IOKit -framework IOSurface -framework CoreFoundation
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t main_port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallScalarMethod(io_connect_t connection, uint32_t selector,
    const uint64_t *input, uint32_t inputCnt, uint64_t *output, uint32_t *outputCnt);
extern kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, uint32_t options);

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
	call(7, in, 3, NULL, 0);
	printf("report %llu: 0x%llx 0x%llx\n", code, a, b);
	fflush(stdout);
}

static void
put_num(CFMutableDictionaryRef d, CFStringRef key, int32_t v)
{
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
	CFDictionarySetValue(d, key, n);
	CFRelease(n);
}

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

int
main(void)
{
	io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("S8Mali"));
	if (svc == 0 || IOServiceOpen(svc, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
		printf("no S8Mali\n");
		return 1;
	}
	IOObjectRelease(svc);

	const int W = 256, H = 256;
	CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	put_num(props, kIOSurfaceWidth, W);
	put_num(props, kIOSurfaceHeight, H);
	put_num(props, kIOSurfaceBytesPerElement, 4);
	put_num(props, kIOSurfacePixelFormat, 'BGRA');
	IOSurfaceRef surf = IOSurfaceCreate(props);
	CFRelease(props);
	if (surf == NULL) {
		report(99, 1, 0);
		return 2;
	}
	uint32_t id = IOSurfaceGetID(surf);
	size_t bpr = IOSurfaceGetBytesPerRow(surf);
	report(1, id, bpr);

	/* the CPU fills it */
	IOSurfaceLock(surf, 0, NULL);
	uint8_t *base = IOSurfaceGetBaseAddress(surf);
	for (int y = 0; y < H; y++) {
		uint32_t *row = (uint32_t *)(base + (size_t)y * bpr);
		for (int x = 0; x < W; x++) {
			row[x] = 0x11111111u;
		}
	}
	IOSurfaceUnlock(surf, 0, NULL);

	/* the GPU sees it */
	uint64_t in = id, out[3] = { 0 };
	if (call(8, &in, 1, out, 3) != KERN_SUCCESS) {
		report(99, 2, 0);
		return 3;
	}
	uint64_t shandle = out[0], sva = out[1];
	report(2, sva, out[2]);
	call(9, &shandle, 1, NULL, 0);                  /* CPU writes out of the caches */

	/* 256 jobs, one diagonal pixel each */
	uint64_t cin[2] = { 0x10000, 1 }, cout[2];
	if (call(1, cin, 2, cout, 2) != KERN_SUCCESS) {
		report(99, 3, 0);
		return 4;
	}
	uint64_t jhandle = cout[0], jva = cout[1];
	mach_vm_address_t jaddr = 0;
	mach_vm_size_t jlen = 0;
	if (IOConnectMapMemory64(conn, (uint32_t)jhandle, mach_task_self(), &jaddr, &jlen, 1 | 0x400) != KERN_SUCCESS) {
		report(99, 4, 0);
		return 5;
	}
	volatile uint32_t *jobs = (volatile uint32_t *)jaddr;
	for (int i = 0; i < 256; i++) {
		uint64_t next = i + 1 < 256 ? jva + 64ULL * (i + 1) : 0;
		write_value_job(jobs + 16 * i, next, (uint16_t)(i + 1), sva + (uint64_t)i * bpr + 4ULL * i,
		    0xff00ff00u | (uint32_t)i << 16);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	uint64_t sin[2] = { jva, 0 }, status = 0;
	call(3, sin, 2, &status, 1);
	call(9, &shandle, 1, NULL, 0);                  /* drop stale lines before the CPU reads */

	IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
	int diag = 0, keep = 0;
	uint32_t first_bad = 0;
	for (int i = 0; i < 256; i++) {
		uint32_t *row = (uint32_t *)(base + (size_t)i * bpr);
		diag += row[i] == (0xff00ff00u | (uint32_t)i << 16);
		uint32_t other = row[(i + 7) % 256];
		if (other == 0x11111111u) {
			keep++;
		} else if (first_bad == 0) {
			first_bad = other;
		}
	}
	IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
	report(3, status, diag);
	report(4, keep, first_bad);
	int ok = status == 1 && diag == 256 && keep == 256;
	report(ok ? 100 : 99, ok, 0);
	IOServiceClose(conn);
	CFRelease(surf);
	return ok ? 0 : 6;
}
