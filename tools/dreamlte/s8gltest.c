/*
 * s8gltest: OpenGL on the Galaxy S8's Mali-G71 from iOS user space (GPU milestone 4).
 * OSMesa from tools/dreamlte/mesa/mk-mesa.sh (Panfrost on the S8Mali kernel driver) renders
 * into memory; the pixels are read back and checked. Results go to stdout and, as reports of
 * the S8Mali user client, to the kernel log ("S8 MALI: s8gltest report N: a b"):
 *    1  GL_RENDERER, first 16 bytes (two little-endian words)
 *    2  clear: pixel (0,0) RGBA, 1 if it is the clear colour
 *    3  triangle: centre pixel RGBA, corner pixel RGBA
 *    4  triangle check: 1 ok, number of wrong probe pixels
 *    5  frames: count, microseconds per frame (clear + 64 triangles + glFinish, 1125x2436)
 *    6  glGetError at the end, OSMesa context (1 = made)
 *    8  FBO frames (render target texture 1125x2436, clear + 64 triangles + glFinish, no copy
 *       to the caller): count, microseconds per frame
 *    9  soak, 90 s of FBO frames: frames, max microseconds of one frame
 *   10  soak: wrong probe pixels (glReadPixels of one pixel every 50 frames), checks
 *    7  memorystatus_control (raise the jetsam limit to 1 GiB): return value, errno
 *  200+ stderr (Mesa's messages), 16 bytes per report
 *  400+ on SIGABRT/SIGSEGV/SIGBUS: frame i, (image: 0 s8gltest, 1 libOSMesa, 2 other) << 56 |
 *       offset in that image (atos -o IMAGE -l 0 OFFSET), then 499: signal
 *  100 / 99  all checks passed / something failed
 *
 *   xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 \
 *       -I research/mesa-ios/src/mesa-25.0.7/include -o s8gltest s8gltest.c \
 *       research/mesa-ios/out/libOSMesa.8.dylib -framework IOKit
 *   codesign -s - s8gltest
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/osmesa.h>
#include <GL/gl.h>
#include <GL/glext.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t main_port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallScalarMethod(io_connect_t connection, uint32_t selector,
    const uint64_t *input, uint32_t inputCnt, uint64_t *output, uint32_t *outputCnt);

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

/* Mesa's messages (stderr, redirected to a file) into the kernel log. */
static void
report_stderr(const char *path)
{
	fflush(stderr);
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return;
	}
	char buf[4096];
	ssize_t n = read(fd, buf, sizeof(buf));
	close(fd);
	for (ssize_t off = 0; off < n; off += 16) {
		uint64_t w[2] = { 0, 0 };
		memcpy(w, buf + off, n - off < 16 ? (size_t)(n - off) : 16);
		report(200 + (uint64_t)off / 16, w[0], w[1]);
	}
}

static const char *errpath = "/private/var/tmp/s8gltest.err";

static void
on_fatal(int sig)
{
	void *frames[32];
	int n = backtrace(frames, 32);
	for (int i = 0; i < n; i++) {
		Dl_info info;
		uint64_t image = 2, off = (uint64_t)(uintptr_t)frames[i];
		if (dladdr(frames[i], &info) && info.dli_fbase != NULL) {
			off = (uint64_t)((uintptr_t)frames[i] - (uintptr_t)info.dli_fbase);
			image = strstr(info.dli_fname, "s8gltest") ? 0 : strstr(info.dli_fname, "OSMesa") ? 1 : 2;
		}
		report(400 + (uint64_t)i, image << 56 | off, 0);
	}
	report_stderr(errpath);
	report(499, (uint64_t)sig, 0);
	signal(sig, SIG_DFL);
	raise(sig);
}

extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t size);
#define MEMORYSTATUS_CMD_SET_JETSAM_TASK_LIMIT 6

static uint32_t
px(const uint32_t *buf, int w, int x, int y)
{
	return buf[(size_t)y * w + x];
}

/* RGBA bytes in memory: R first; read as a little-endian word, R is the low byte. */
#define RED     0xff0000ffu
#define GREEN   0xff00ff00u

int
main(void)
{
	io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("S8Mali"));
	if (svc != 0) {
		IOServiceOpen(svc, mach_task_self(), 0, &conn);
		IOObjectRelease(svc);
	}

	/* launchd jobs get a small default jetsam limit (c33: killed, per-process limit) */
	int mrc = memorystatus_control(MEMORYSTATUS_CMD_SET_JETSAM_TASK_LIMIT, getpid(), 1024, NULL, 0);
	report(7, (uint64_t)(int64_t)mrc, mrc ? errno : 0);
	signal(SIGABRT, on_fatal);
	signal(SIGSEGV, on_fatal);
	signal(SIGBUS, on_fatal);
	int efd = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (efd >= 0) {
		dup2(efd, 2);
		close(efd);
	}
	setenv("MESA_LOG_LEVEL", "info", 0);

	const int W = 256, H = 256;
	uint32_t *buf = calloc((size_t)W * H, 4);
	OSMesaContext ctx = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, NULL);
	int ok = ctx != NULL && buf != NULL && OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, W, H);
	if (!ok) {
		report(6, 0, 0);
		report_stderr(errpath);
		report(99, 0, 0);
		return 1;
	}
	const char *renderer = (const char *)glGetString(GL_RENDERER);
	char r16[17] = { 0 };
	strncpy(r16, renderer ? renderer : "?", 16);
	uint64_t w0, w1;
	memcpy(&w0, r16, 8);
	memcpy(&w1, r16 + 8, 8);
	report(1, w0, w1);
	printf("GL_RENDERER %s\nGL_VERSION %s\n", renderer, (const char *)glGetString(GL_VERSION));
	int gpu = renderer != NULL && strstr(renderer, "Mali") != NULL;

	/* clear */
	glViewport(0, 0, W, H);
	glClearColor(1, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	uint32_t c0 = px(buf, W, 0, 0);
	report(2, c0, c0 == RED);
	ok = ok && c0 == RED;

	/* a green triangle over the lower middle: (-0.5,-0.5) (0.5,-0.5) (0,0.5) */
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glColor4f(0, 1, 0, 1);
	glBegin(GL_TRIANGLES);
	glVertex2f(-0.5f, -0.5f);
	glVertex2f(0.5f, -0.5f);
	glVertex2f(0.0f, 0.5f);
	glEnd();
	glFinish();
	uint32_t centre = px(buf, W, W / 2, H / 2), corner = px(buf, W, 4, H - 5);
	report(3, centre, corner);
	/* probes: inside the triangle green, outside red */
	static const int in_probe[][2] = { { 128, 100 }, { 100, 80 }, { 156, 80 }, { 128, 150 } };
	static const int out_probe[][2] = { { 10, 10 }, { 245, 10 }, { 10, 245 }, { 245, 245 }, { 60, 200 }, { 196, 200 } };
	int wrong = 0;
	for (unsigned i = 0; i < sizeof(in_probe) / sizeof(in_probe[0]); i++) {
		wrong += px(buf, W, in_probe[i][0], in_probe[i][1]) != GREEN;
	}
	for (unsigned i = 0; i < sizeof(out_probe) / sizeof(out_probe[0]); i++) {
		wrong += px(buf, W, out_probe[i][0], out_probe[i][1]) != RED;
	}
	report(4, wrong == 0, wrong);
	ok = ok && wrong == 0;

	/* frames at the presentation size */
	const int FW = 1125, FH = 2436;
	uint32_t *frame = calloc((size_t)FW * FH, 4);
	uint64_t frames = 0, us = 0;
	if (frame != NULL && OSMesaMakeCurrent(ctx, frame, GL_UNSIGNED_BYTE, FW, FH)) {
		mach_timebase_info_data_t tb;
		mach_timebase_info(&tb);
		glViewport(0, 0, FW, FH);
		uint64_t t0 = mach_absolute_time();
		for (frames = 0; frames < 120; frames++) {
			glClearColor(0, 0, (float)(frames & 15) / 15.0f, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			glBegin(GL_TRIANGLES);
			for (int i = 0; i < 64; i++) {
				float x = -1.0f + (float)(i % 8) * 0.25f, y = -1.0f + (float)(i / 8) * 0.25f;
				glColor4f((float)(i & 3) / 3.0f, 1, (float)(frames & 7) / 7.0f, 1);
				glVertex2f(x, y);
				glVertex2f(x + 0.25f, y);
				glVertex2f(x, y + 0.25f);
			}
			glEnd();
			glFinish();
		}
		us = (mach_absolute_time() - t0) * tb.numer / tb.denom / 1000 / frames;
	}
	report(5, frames, us);

	/* the same frames into a texture: the GPU's own time, without OSMesa's copy out */
	GLuint fbo = 0, tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FW, FH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	uint64_t fbo_frames = 0, fbo_us = 0, soak = 0, soak_max = 0, soak_bad = 0, soak_checks = 0;
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
		mach_timebase_info_data_t tb;
		mach_timebase_info(&tb);
		glViewport(0, 0, FW, FH);
		uint64_t t0 = mach_absolute_time(), end = t0 + 90ULL * 1000000000ULL * tb.denom / tb.numer;
		for (uint64_t f = 0;; f++) {
			uint64_t ts = mach_absolute_time();
			if (f == 300) {
				fbo_frames = 300;
				fbo_us = (ts - t0) * tb.numer / tb.denom / 1000 / 300;
			}
			if (ts > end) {
				soak = f;
				break;
			}
			float blue = (float)(f & 15) / 15.0f;
			glClearColor(0, 0, blue, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			glBegin(GL_TRIANGLES);
			for (int i = 0; i < 64; i++) {
				float x = -1.0f + (float)(i % 8) * 0.25f, y = -1.0f + (float)(i / 8) * 0.25f;
				glColor4f(1, 1, 0, 1);
				glVertex2f(x, y);
				glVertex2f(x + 0.25f, y);
				glVertex2f(x, y + 0.25f);
			}
			glEnd();
			if (f % 50 == 0) {
				/* (8, 8) is inside the first triangle: yellow; the top-right corner of
				 * each cell is outside: the clear colour */
				uint32_t a = 0, b = 0;
				glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &a);
				glReadPixels(FW / 8 - 4, FH / 8 - 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &b);
				uint32_t want_b = 0xff000000u | (uint32_t)(blue * 255.0f + 0.5f) << 16;
				soak_bad += (a != 0xff00ffffu) + (b != want_b);
				if (soak_checks == 0 && (a != 0xff00ffffu || b != want_b)) {
					report(11, a, b);
				}
				soak_checks++;
			} else {
				glFinish();
			}
			uint64_t dt = (mach_absolute_time() - ts) * tb.numer / tb.denom / 1000;
			soak_max = dt > soak_max ? dt : soak_max;
		}
	}
	report(8, fbo_frames, fbo_us);
	report(9, soak, soak_max);
	report(10, soak_bad, soak_checks);
	ok = ok && fbo_frames == 300 && soak_bad == 0;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	GLenum err = glGetError();
	report(6, err, 1);
	ok = ok && gpu && err == GL_NO_ERROR;
	report_stderr(errpath);
	report(ok ? 100 : 99, gpu, ok);
	OSMesaDestroyContext(ctx);
	return ok ? 0 : 2;
}
