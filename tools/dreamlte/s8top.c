/*
 * s8top: which processes (and kernel threads) use the CPU, with nobody at the phone.
 *
 * Run from the drop zone by s8drop (root); it goes to the background at once so the programs
 * after it in the drop still run. Boot-arg s8top=START+LEN[,START+LEN...] (seconds after boot,
 * at most 4 windows; default 300+30): over each window it measures the CPU time of every
 * process (proc_pidinfo PROC_PIDTASKINFO) and of kernel_task's threads (PROC_PIDTHREADINFO),
 * and reports the busiest through the S8Mali Report selector, so they land in the kernel log:
 *
 *   900: window index | length ms << 32, all processes' CPU ms in the window
 *   901+k (k < 10): pid | CPU ms << 32, first 8 bytes of the process name (little-endian)
 *   920+k (k < 6): thread index | CPU ms << 32, first 8 bytes of the kernel thread's name
 *   939: kernel_task threads could not be read (errno)
 *
 * Boot-arg s8stop=NAME[,NAME...]@T (at most 4 names): at T seconds after boot, SIGSTOP every process
 * with that name (a stopped process uses no CPU and launchd does not start it again), to see
 * what its absence changes: 940 pid, first 8 bytes of the name.
 *
 *   xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -o s8top s8top.c \
 *       -framework IOKit -framework CoreFoundation
 *   codesign -s - s8top
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t main_port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallScalarMethod(io_connect_t connection, uint32_t selector,
    const uint64_t *input, uint32_t inputCnt, uint64_t *output, uint32_t *outputCnt);

/* libproc / sys/proc_info.h (not in the iOS SDK; as in the macOS SDK) */
#define MAXTHREADNAMESIZE       64
#define PROC_PIDTASKINFO        4
#define PROC_PIDTHREADINFO      5
#define PROC_PIDLISTTHREADS     6
struct proc_taskinfo {
	uint64_t pti_virtual_size, pti_resident_size, pti_total_user, pti_total_system;
	uint64_t pti_threads_user, pti_threads_system;
	int32_t pti_policy, pti_faults, pti_pageins, pti_cow_faults, pti_messages_sent;
	int32_t pti_messages_received, pti_syscalls_mach, pti_syscalls_unix, pti_csw;
	int32_t pti_threadnum, pti_numrunning, pti_priority;
};
struct proc_threadinfo {
	uint64_t pth_user_time, pth_system_time;
	int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time;
	int32_t pth_curpri, pth_priority, pth_maxpriority;
	char pth_name[MAXTHREADNAMESIZE];
};
extern int proc_listallpids(void *buffer, int buffersize);
extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);

#define MAXP 1024
#define MAXT 512

static io_connect_t conn;
static mach_timebase_info_data_t tb;

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

static uint64_t
name8(const char *s)
{
	uint64_t v = 0;
	memcpy(&v, s, strnlen(s, 8));
	return v;
}

struct sample {
	int n;
	int pid[MAXP];
	uint64_t ns[MAXP];
	int nt;
	uint64_t tid[MAXT];
	uint64_t tns[MAXT];
	char tname[MAXT][MAXTHREADNAMESIZE];
	int terr;
};

/* Task times are Mach time units on arm64; thread times are nanoseconds. */
static void
take(struct sample *s)
{
	static int pids[MAXP];
	int n = proc_listallpids(pids, sizeof(pids));
	s->n = 0;
	for (int i = 0; i < n && s->n < MAXP; i++) {
		struct proc_taskinfo ti;
		if (proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) == sizeof(ti)) {
			s->pid[s->n] = pids[i];
			s->ns[s->n++] = (ti.pti_total_user + ti.pti_total_system) * tb.numer / tb.denom;
		}
	}
	static uint64_t tids[MAXT];
	int bytes = proc_pidinfo(0, PROC_PIDLISTTHREADS, 0, tids, sizeof(tids));
	s->nt = 0;
	s->terr = bytes <= 0 ? errno : 0;
	for (int i = 0; i < bytes / (int)sizeof(uint64_t) && s->nt < MAXT; i++) {
		struct proc_threadinfo th;
		if (proc_pidinfo(0, PROC_PIDTHREADINFO, tids[i], &th, sizeof(th)) == sizeof(th)) {
			s->tid[s->nt] = tids[i];
			s->tns[s->nt] = th.pth_user_time + th.pth_system_time;
			memcpy(s->tname[s->nt++], th.pth_name, MAXTHREADNAMESIZE);
		}
	}
}

static struct sample a, b;

static void
window(int w, long start, long len)
{
	while (uptime() < start) {
		sleep(1);
	}
	uint64_t t0 = mach_absolute_time();
	take(&a);
	sleep((unsigned)len);
	take(&b);
	uint64_t ms = (mach_absolute_time() - t0) * tb.numer / tb.denom / 1000000;

	/* per-process deltas, busiest first */
	static uint64_t d[MAXP];
	uint64_t total = 0;
	for (int i = 0; i < b.n; i++) {
		d[i] = b.ns[i];
		for (int j = 0; j < a.n; j++) {
			if (a.pid[j] == b.pid[i]) {
				d[i] = b.ns[i] > a.ns[j] ? b.ns[i] - a.ns[j] : 0;
				break;
			}
		}
		total += d[i];
	}
	report(900, (uint64_t)w | ms << 32, total / 1000000);
	for (int k = 0; k < 10; k++) {
		int best = -1;
		for (int i = 0; i < b.n; i++) {
			if (d[i] != 0 && (best < 0 || d[i] > d[best])) {
				best = i;
			}
		}
		if (best < 0) {
			break;
		}
		char name[64] = "";
		if (b.pid[best] == 0) {
			strcpy(name, "kernel");
		} else {
			proc_name(b.pid[best], name, sizeof(name));
		}
		report(901 + k, (uint64_t)b.pid[best] | (d[best] / 1000000) << 32, name8(name));
		d[best] = 0;
	}

	/* kernel_task threads */
	if (b.terr != 0 || b.nt == 0) {
		report(939, b.terr, b.nt);
		return;
	}
	static uint64_t td[MAXT];
	for (int i = 0; i < b.nt; i++) {
		td[i] = b.tns[i];
		for (int j = 0; j < a.nt; j++) {
			if (a.tid[j] == b.tid[i]) {
				td[i] = b.tns[i] > a.tns[j] ? b.tns[i] - a.tns[j] : 0;
				break;
			}
		}
	}
	for (int k = 0; k < 6; k++) {
		int best = -1;
		for (int i = 0; i < b.nt; i++) {
			if (td[i] != 0 && (best < 0 || td[i] > td[best])) {
				best = i;
			}
		}
		if (best < 0) {
			break;
		}
		report(920 + k, (uint64_t)best | (td[best] / 1000000) << 32, name8(b.tname[best]));
		td[best] = 0;
	}
}

int
main(void)
{
	if (fork() != 0) {
		return 0;
	}
	mach_timebase_info(&tb);
	io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("S8Mali"));
	if (svc != 0) {
		IOServiceOpen(svc, mach_task_self(), 0, &conn);
		IOObjectRelease(svc);
	}
	long start[4] = { 300 }, len[4] = { 30 };
	int windows = 1;
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *at;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
	    (at = strstr(arguments, "s8top=")) != NULL) {
		char *end;
		for (windows = 0, at += 6; windows < 4 && *at >= '0' && *at <= '9'; windows++) {
			start[windows] = strtol(at, &end, 10);
			len[windows] = *end == '+' ? strtol(end + 1, &end, 10) : 30;
			at = *end == ',' ? end + 1 : end;
		}
	}
	if ((at = strstr(arguments, "s8stop=")) != NULL) {
		char names[4][32] = { "" };
		int nn = 0;
		at += 7;
		while (nn < 4 && *at && *at != ' ' && *at != '@') {
			size_t l = strcspn(at, ",@ ");
			snprintf(names[nn++], sizeof(names[0]), "%.*s", (int)(l < 31 ? l : 31), at);
			at += l;
			at += *at == ',';
		}
		long when = *at == '@' ? strtol(at + 1, NULL, 10) : 120;
		while (uptime() < when) {
			sleep(1);
		}
		static int pids[MAXP];
		int n = proc_listallpids(pids, sizeof(pids));
		for (int i = 0; i < n; i++) {
			char name[64] = "";
			proc_name(pids[i], name, sizeof(name));
			for (int k = 0; k < nn; k++) {
				if (strcmp(name, names[k]) == 0 && kill(pids[i], SIGSTOP) == 0) {
					report(940, pids[i], name8(name));
				}
			}
		}
	}
	for (int w = 0; w < windows; w++) {
		window(w, start[w], len[w]);
	}
	return 0;
}
