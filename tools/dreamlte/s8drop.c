/*
 * s8drop: run test programs from a "drop zone" on USERDATA without editing the iOS root.
 * The zone is raw space after the 16 GiB HFS+ root, at byte offset S8DROP_OFFSET of USERDATA
 * (iOS: /dev/rufsroot0 is the whole partition; the kernel log ring is its last 4 MiB). It is
 * written from TWRP by tools/dreamlte/drop.sh (image from tools/dreamlte/mkdrop.py):
 *
 *   header (4 KiB): "S8DROP01", u32 count, u32 reserved, then count entries of
 *     char name[48], u32 flags (1 = run it), u32 reserved, u64 offset, u64 size
 *   data at the given offsets (from the start of the zone)
 *
 * Installed once in the root (/usr/local/bin/s8drop, launchd job at boot). Files go to
 * /private/var/tmp/s8drop/; programs with flag 1 run in order, each waited for. Results reach
 * the kernel log through the S8Mali Report selector ("S8 MALI: s8drop report N"):
 *   300  zone found: count, total bytes      301  no zone (bad magic): first word read
 *   302  run: index, exit status (wait status)  303  failed to write or spawn: index, errno
 */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>

#define S8DROP_OFFSET (20ULL << 30)
#define S8DROP_MAX    (256ULL << 20)
#define DIR           "/private/var/tmp/s8drop"

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
extern char **environ;

struct entry {
	char name[48];
	uint32_t flags, reserved;
	uint64_t offset, size;
};

static io_connect_t conn;

static void
report(uint64_t code, uint64_t a, uint64_t b)
{
	uint64_t in[3] = { code, a, b };
	uint32_t n = 0;
	if (conn != 0) {
		IOConnectCallScalarMethod(conn, 7, in, 3, NULL, &n);
	}
	printf("report %llu: 0x%llx 0x%llx\n", code, a, b);
	fflush(stdout);
}

/* Reads [off, off + len) of the zone; the raw device wants 4 KiB aligned transfers. */
static int
zread(int fd, uint64_t off, void *dst, uint64_t len)
{
	uint64_t start = off & ~4095ULL, end = (off + len + 4095) & ~4095ULL;
	char *tmp = malloc(end - start);
	if (tmp == NULL) {
		return -1;
	}
	for (uint64_t done = 0; done < end - start;) {
		ssize_t n = pread(fd, tmp + done, end - start - done, (off_t)(S8DROP_OFFSET + start + done));
		if (n <= 0) {
			free(tmp);
			return -1;
		}
		done += (uint64_t)n;
	}
	memcpy(dst, tmp + (off - start), len);
	free(tmp);
	return 0;
}

int
main(void)
{
	io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("S8Mali"));
	if (svc != 0) {
		IOServiceOpen(svc, mach_task_self(), 0, &conn);
		IOObjectRelease(svc);
	}
	int fd = open("/dev/rufsroot0", O_RDONLY);
	static char hdr[4096];
	if (fd < 0 || zread(fd, 0, hdr, sizeof(hdr)) != 0 || memcmp(hdr, "S8DROP01", 8) != 0) {
		uint64_t w = 0;
		memcpy(&w, hdr, 8);
		report(301, fd < 0 ? (uint64_t)errno : w, fd);
		return 1;
	}
	uint32_t count;
	memcpy(&count, hdr + 8, 4);
	struct entry *e = (struct entry *)(hdr + 16);
	if (count > (sizeof(hdr) - 16) / sizeof(*e)) {
		report(301, count, 0);
		return 1;
	}
	uint64_t total = 0;
	for (uint32_t i = 0; i < count; i++) {
		total += e[i].size;
	}
	report(300, count, total);
	mkdir(DIR, 0755);
	chdir(DIR);
	for (uint32_t i = 0; i < count; i++) {
		e[i].name[47] = 0;
		if (strchr(e[i].name, '/') != NULL || e[i].offset + e[i].size > S8DROP_MAX) {
			report(303, i, EINVAL);
			continue;
		}
		char *data = malloc(e[i].size ? e[i].size : 1);
		int out = data ? open(e[i].name, O_WRONLY | O_CREAT | O_TRUNC, 0755) : -1;
		if (out < 0 || zread(fd, e[i].offset, data, e[i].size) != 0 ||
		    write(out, data, e[i].size) != (ssize_t)e[i].size) {
			report(303, i, errno);
		}
		if (out >= 0) {
			fchmod(out, 0755);
			close(out);
		}
		free(data);
	}
	close(fd);
	for (uint32_t i = 0; i < count; i++) {
		if (!(e[i].flags & 1)) {
			continue;
		}
		char path[128];
		snprintf(path, sizeof(path), DIR "/%s", e[i].name);
		char *argv[] = { path, NULL };
		pid_t pid;
		int rc = posix_spawn(&pid, path, NULL, NULL, argv, environ);
		if (rc != 0) {
			report(303, i, rc);
			continue;
		}
		int st = 0;
		waitpid(pid, &st, 0);
		report(302, i, (uint64_t)st);
	}
	if (conn != 0) {
		IOServiceClose(conn);
	}
	return 0;
}
