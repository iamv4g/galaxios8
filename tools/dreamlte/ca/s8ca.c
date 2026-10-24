/*
 * libs8ca.dylib: puts CoreAnimation's render server (backboardd) on the Mali.
 *
 * Installed once in the iOS root (/usr/local/lib/libs8ca.dylib, `edit-live-root.sh LIVE.img ca`),
 * with DYLD_INSERT_LIBRARIES in backboardd's launchd job. dyld honours that only when the kernel
 * answers AMFI's dyld-policy request (0124, policy "AMFI"; boot-arg s8dyld=0 turns it off), and
 * a library that does not load does not stop backboardd. Without boot-arg s8ca=1 nothing is done,
 * so the daily images keep CoreAnimation's software renderer.
 *
 * With s8ca=1, in backboardd, before main():
 *   1. stderr becomes a pipe whose lines go to the kernel log through the S8Mali Log selector
 *      ("S8 LOG backboardd: ..."): what the bridge and Mesa say, and the lines below.
 *   2. It waits (at most 90 s) until s8drop (tools/dreamlte/s8drop.c, root) has unpacked this
 *      boot's drop zone into /private/var/tmp/s8drop/: ca-libOSMesa.8.dylib (Mesa Panfrost),
 *      ca-libs8gl.dylib (the EAGLContext bridge, eagl.c + gl_osmesa.c, linking the former by that
 *      path) and last ca-ready (16 bytes) whose time says it is from this boot.
 *   3. ca-libs8gl.dylib is loaded; its constructor replaces EAGLContext's methods, and the
 *      renderer CoreAnimation then gets (no Metal device -> [EAGLContext initWithAPI:3] ->
 *      CA::OGL::GLESContext) draws through Mesa Panfrost on the S8Mali driver.
 *   4. A crash in backboardd is written down (signal, pc, lr and the frame chain as unslid
 *      dyld cache addresses, for `ipsw dyld a2s`), then the signal goes on as before.
 *
 * The crash reporter comes from HackiOS's userland/src/inject.c (MIT, see LICENSE.hackios).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <unistd.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t main_port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallStructMethod(io_connect_t connection, uint32_t selector, const void *input,
    size_t inputSize, void *output, size_t *outputSize);
extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE      0x180000000UL           /* the iPhone10,3 16.7.16 cache as built */
#define DIR             "/private/var/tmp/s8drop"
#define WAIT_SECONDS    90
#define LOG_SELECTOR    10

static io_connect_t conn;
static uintptr_t slide;

static void
say(const char *format, ...)
{
	char line[512];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

/* ---- stderr into the kernel log ---------------------------------------------------------------- */

static void *
log_pump(void *arg)
{
	int fd = (int)(intptr_t)arg;
	char buf[4096];
	size_t have = 0;

	for (;;) {
		ssize_t n = read(fd, buf + have, sizeof(buf) - have);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			return NULL;
		}
		have += (size_t)n;
		/* whole lines go out; a line longer than the buffer goes out as it is */
		size_t send = have;
		while (send > 0 && buf[send - 1] != '\n') {
			send--;
		}
		if (send == 0 && have == sizeof(buf)) {
			send = have;
		}
		if (send > 0) {
			IOConnectCallStructMethod(conn, LOG_SELECTOR, buf, send, NULL, NULL);
			memmove(buf, buf + send, have - send);
			have -= send;
		}
	}
}

static void
log_to_kernel(void)
{
	int p[2];
	pthread_t thread;

	if (pipe(p) != 0) {
		return;
	}
	/* a full pipe loses lines instead of stopping the window server */
	fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL) | O_NONBLOCK);
	fcntl(p[0], F_SETFD, FD_CLOEXEC);
	fcntl(p[1], F_SETFD, FD_CLOEXEC);
	if (pthread_create(&thread, NULL, log_pump, (void *)(intptr_t)p[0]) != 0) {
		close(p[0]);
		close(p[1]);
		return;
	}
	pthread_detach(thread);
	dup2(p[1], 2);
	close(p[1]);
	setvbuf(stderr, NULL, _IOLBF, 0);
}

/* ---- the drop zone ------------------------------------------------------------------------------ */

/*
 * s8drop (root, from launchd at the same time as this) unpacks every file of the drop zone into
 * DIR, in the zone's order; the drop puts "ca-ready" last. backboardd itself may not read the raw
 * disk (c47: /dev/rufsroot0 "Permission denied"). A ca-ready written before this boot is old.
 */
static int
wait_for_drop(void)
{
	struct timeval boot;
	size_t size = sizeof(boot);
	struct stat st;

	if (sysctlbyname("kern.boottime", &boot, &size, NULL, 0) != 0) {
		return 0;
	}
	for (int i = 0; i < WAIT_SECONDS * 10; i++) {
		if (stat(DIR "/ca-ready", &st) == 0 && st.st_mtimespec.tv_sec >= boot.tv_sec && st.st_size == 16) {
			say("s8ca: drop ready after %d.%d s\n", i / 10, i % 10);
			return 1;
		}
		usleep(100000);
	}
	say("s8ca: no ca-ready of this boot in " DIR " after %d s\n", WAIT_SECONDS);
	return 0;
}

/* ---- crashes ------------------------------------------------------------------------------------ */

static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uintptr_t pc = 0, lr = 0, fp = 0;
	char frames[8 * 20 + 1] = "";
	size_t used = 0;

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
		fp = (uintptr_t)context->uc_mcontext->__ss.__fp;
	}
	for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;
	}
	say("s8ca: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n", signal_number,
	    info != NULL ? info->si_addr : NULL, pc - slide, lr - slide, frames);
	usleep(200000);                         /* (the pump thread sends the line) */
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

static void
watch_for_faults(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = say_the_fault;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
}

/* ------------------------------------------------------------------------------------------------- */

__attribute__((constructor))
static void
s8ca_init(void)
{
	const char *name = getprogname();
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1, length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);
	const char *on;
	io_service_t gpu;
	void *library;

	if (name == NULL || strcmp(name, "backboardd") != 0) {
		return;
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0 ||
	    (on = strstr(arguments, "s8ca=")) == NULL || (on != arguments && on[-1] != ' ') || on[5] == '0') {
		return;
	}
	gpu = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("S8Mali"));
	if (gpu == 0) {
		return;                         /* no GPU driver (s8mali=0): the software renderer */
	}
	IOServiceOpen(gpu, mach_task_self(), 0, &conn);
	IOObjectRelease(gpu);
	if (conn == 0) {
		return;
	}
	slide = cache != NULL ? (uintptr_t)cache - CACHE_BASE : 0;
	log_to_kernel();
	watch_for_faults();
	say("s8ca: in backboardd (pid %d), shared cache slide %#lx\n", getpid(), slide);
	if (!wait_for_drop()) {
		return;
	}
	library = dlopen(DIR "/ca-libs8gl.dylib", RTLD_NOW);
	if (library == NULL) {
		say("s8ca: %s\n", dlerror());
		return;
	}
	say("s8ca: libs8gl.dylib loaded: CoreAnimation may draw with OpenGL ES on the Mali\n");
}
