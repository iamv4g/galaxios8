/*
 * s8shot: screenshots of what CoreAnimation drew, without a person at the phone.
 *
 * Run from the drop zone by s8drop (root). At the given seconds after boot (arguments, or else
 * boot-arg s8shot=T1,T2,... (at most 8); default 120, 200 and 270) it looks up the IOSurfaces with IDs 1-64, takes those of the screen's size
 * (1125x2436 or 1440x2960, BGRA: the window server's framebuffer surfaces) and writes them raw
 * to USERDATA through /dev/rufsroot0 at 21 GiB (after the drop zone at 20 GiB, far from the
 * kernel log ring at the end), where tools/dreamlte/pull-shot.py reads them from TWRP:
 *
 *   slot k (k = 0..7) at 21 GiB + k * 16 MiB:
 *     4 KiB header "S8SHOT01", u32 surface id, width, height, bytes per row, pixel format,
 *       seconds since boot, shot index; then height * bytes-per-row pixel bytes
 *
 * Results in the kernel log as "S8 MALI: s8shot report N" (600 shot: index | id << 8,
 * uptime; 601 nothing of the screen's size; 602 write failed: errno).
 *
 *   xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -o s8shot s8shot.c \
 *       -framework IOSurface -framework IOKit -framework CoreFoundation
 *   codesign -s - s8shot
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t main_port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallScalarMethod(io_connect_t connection, uint32_t selector,
    const uint64_t *input, uint32_t inputCnt, uint64_t *output, uint32_t *outputCnt);

#define SHOT_OFFSET     (21ULL << 30)
#define SLOT_SIZE       (16ULL << 20)
#define SLOTS           8

static io_connect_t conn;

static void
report(uint64_t code, uint64_t a, uint64_t b)
{
	uint64_t in[3] = { code, a, b };
	uint32_t n = 0;
	if (conn != 0) {
		IOConnectCallScalarMethod(conn, 7, in, 3, NULL, &n);
	}
}

static long
uptime(void)
{
	struct timeval boot, now;
	size_t size = sizeof(boot);
	sysctlbyname("kern.boottime", &boot, &size, NULL, 0);
	gettimeofday(&now, NULL);
	return now.tv_sec - boot.tv_sec;
}

int
main(int argc, char **argv)
{
	io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("S8Mali"));
	if (svc != 0) {
		IOServiceOpen(svc, mach_task_self(), 0, &conn);
		IOObjectRelease(svc);
	}
	int fd = open("/dev/rufsroot0", O_RDWR);
	if (fd < 0) {
		report(602, errno, 0);
		return 1;
	}
	unsigned slot = 0;
	long times[8] = { 120, 200, 270 };
	int shots = 3;
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *at;
	if (argc > 1) {
		for (shots = 0; shots < argc - 1 && shots < 8; shots++) {
			times[shots] = strtol(argv[shots + 1], NULL, 10);
		}
	} else if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
	    (at = strstr(arguments, "s8shot=")) != NULL) {
		char *end;
		for (shots = 0, at += 7; shots < 8 && *at >= '0' && *at <= '9'; shots++) {
			times[shots] = strtol(at, &end, 10);
			at = *end == ',' ? end + 1 : end;
		}
	}
	for (int a = 0; a < shots; a++) {
		long when = times[a];
		while (uptime() < when) {
			sleep(1);
		}
		int found = 0;
		for (uint32_t id = 1; id <= 64 && slot < SLOTS; id++) {
			IOSurfaceRef s = IOSurfaceLookup(id);
			if (s == NULL) {
				continue;
			}
			size_t w = IOSurfaceGetWidth(s), h = IOSurfaceGetHeight(s), bpr = IOSurfaceGetBytesPerRow(s);
			uint32_t fmt = IOSurfaceGetPixelFormat(s);
			if (!((w == 1125 && h == 2436) || (w == 1440 && h == 2960)) || fmt != 'BGRA' ||
			    4096 + h * bpr > SLOT_SIZE) {
				CFRelease(s);
				continue;
			}
			size_t len = (4096 + h * bpr + 4095) & ~4095UL;
			char *buf = calloc(1, len);
			uint32_t hdr[8] = { id, (uint32_t)w, (uint32_t)h, (uint32_t)bpr, fmt, (uint32_t)uptime(), slot, 0 };
			memcpy(buf, "S8SHOT01", 8);
			memcpy(buf + 8, hdr, sizeof(hdr));
			IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
			memcpy(buf + 4096, IOSurfaceGetBaseAddress(s), h * bpr);
			IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
			ssize_t n = pwrite(fd, buf, len, (off_t)(SHOT_OFFSET + slot * SLOT_SIZE));
			if (n != (ssize_t)len) {
				report(602, errno, slot);
			} else {
				report(600, slot | id << 8, hdr[5]);
				slot++;
				found++;
			}
			free(buf);
			CFRelease(s);
		}
		if (!found) {
			report(601, (uint64_t)uptime(), 0);
		}
	}
	fsync(fd);
	close(fd);
	return 0;
}
