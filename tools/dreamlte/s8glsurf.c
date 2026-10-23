/*
 * s8glsurf: OpenGL on the Mali renders straight into an IOSurface (the memory iOS composites).
 * OSMesa + Panfrost (tools/dreamlte/mesa): glEGLImageTargetTexture2DOES with an IOSurface ID
 * makes the surface a texture, attached to an FBO it is the render target; the CPU checks the
 * pixels through the surface's own mapping. Reports ("S8 MALI: s8glsurf report N: a b"):
 *    1  surface ID, bytes per row        2  FBO status, glGetError after the import
 *    3  centre pixel, corner pixel (BGRA memory read as a word: 0xAARRGGBB)
 *    4  probes right (of 7), first wrong value
 *    5  frames into the surface (clear + 64 triangles + glFinish): count, us per frame
 *  100 / 99  passed / failed;  200+  Mesa's stderr
 *
 *   xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 \
 *       -I research/mesa-ios/src/mesa-25.0.7/include -o s8glsurf s8glsurf.c libOSMesa.8.dylib \
 *       -framework IOKit -framework IOSurface -framework CoreFoundation
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
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
static const char *errpath = "/private/var/tmp/s8glsurf.err";

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

static void
report_stderr(void)
{
	fflush(stderr);
	int fd = open(errpath, O_RDONLY);
	if (fd < 0) {
		return;
	}
	char buf[2048];
	ssize_t n = read(fd, buf, sizeof(buf));
	close(fd);
	for (ssize_t off = 0; off < n; off += 16) {
		uint64_t w[2] = { 0, 0 };
		memcpy(w, buf + off, n - off < 16 ? (size_t)(n - off) : 16);
		report(200 + (uint64_t)off / 16, w[0], w[1]);
	}
}

static void
put_num(CFMutableDictionaryRef d, CFStringRef key, int32_t v)
{
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
	CFDictionarySetValue(d, key, n);
	CFRelease(n);
}

typedef void (*target_fn)(GLenum target, void *image);

int
main(void)
{
	io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("S8Mali"));
	if (svc != 0) {
		IOServiceOpen(svc, mach_task_self(), 0, &conn);
		IOObjectRelease(svc);
	}
	int efd = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (efd >= 0) {
		dup2(efd, 2);
		close(efd);
	}

	const int W = 1125, H = 2436;
	CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	put_num(props, kIOSurfaceWidth, W);
	put_num(props, kIOSurfaceHeight, H);
	put_num(props, kIOSurfaceBytesPerElement, 4);
	put_num(props, kIOSurfacePixelFormat, 0x42475241);       /* 'BGRA' */
	IOSurfaceRef surf = IOSurfaceCreate(props);
	CFRelease(props);
	if (surf == NULL) {
		report(99, 1, 0);
		return 1;
	}
	size_t bpr = IOSurfaceGetBytesPerRow(surf);
	report(1, IOSurfaceGetID(surf), bpr);

	static uint32_t dummy[16 * 16];
	OSMesaContext ctx = OSMesaCreateContextExt(OSMESA_RGBA, 0, 0, 0, NULL);
	if (ctx == NULL || !OSMesaMakeCurrent(ctx, dummy, GL_UNSIGNED_BYTE, 16, 16)) {
		report_stderr();
		report(99, 2, 0);
		return 2;
	}
	target_fn image_target = (target_fn)OSMesaGetProcAddress("glEGLImageTargetTexture2DOES");
	GLuint tex = 0, fbo = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	if (image_target != NULL) {
		image_target(GL_TEXTURE_2D, (void *)(uintptr_t)IOSurfaceGetID(surf));
	}
	GLenum err = glGetError();
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	GLenum fs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	report(2, fs, (uint64_t)err << 8 | (image_target != NULL));
	if (fs != GL_FRAMEBUFFER_COMPLETE) {
		report_stderr();
		report(99, 3, 0);
		return 3;
	}

	glViewport(0, 0, W, H);
	glClearColor(1, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
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

	IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
	const uint8_t *base = IOSurfaceGetBaseAddress(surf);
#define PX(x, y) (*(const uint32_t *)(base + (size_t)(y) * bpr + 4 * (size_t)(x)))
	const uint32_t RED = 0xffff0000u, GREEN = 0xff00ff00u;
	uint32_t centre = PX(W / 2, H / 2), corner = PX(2, 2);
	report(3, centre, corner);
	const int probes[7][3] = {
		{ W / 2, H / 2, 1 }, { 2, 2, 0 }, { W - 3, 2, 0 }, { 2, H - 3, 0 }, { W - 3, H - 3, 0 },
		{ W / 10, H / 2, 0 }, { W - W / 10, H / 2, 0 },
	};
	int right = 0;
	uint32_t first_bad = 0;
	for (int i = 0; i < 7; i++) {
		uint32_t v = PX(probes[i][0], probes[i][1]);
		if (v == (probes[i][2] ? GREEN : RED)) {
			right++;
		} else if (first_bad == 0) {
			first_bad = v;
		}
	}
	IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
	report(4, right, first_bad);

	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	uint64_t t0 = mach_absolute_time();
	int frames;
	for (frames = 0; frames < 120; frames++) {
		glClearColor(0, 0, (float)(frames & 15) / 15.0f, 1);
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
		glFinish();
	}
	uint64_t us = (mach_absolute_time() - t0) * tb.numer / tb.denom / 1000 / (uint64_t)frames;
	report(5, (uint64_t)frames, us);
	err = glGetError();
	int ok = right == 7 && err == GL_NO_ERROR;
	report_stderr();
	report(ok ? 100 : 99, ok, err);
	OSMesaDestroyContext(ctx);
	CFRelease(surf);
	return ok ? 0 : 4;
}
