/*
 * libdrm stand-in for Mesa's Panfrost on iOS on the Galaxy S8 (see xf86drm.h): the panfrost DRM
 * requests as calls of the S8Mali user client (scalar selectors, dreamlte_mali.cpp):
 *   0 GetParam [param] -> [value]      1 CreateBO [size, flags] -> [handle, gpu va]
 *   2 FreeBO [handle]                  3 Submit [jc, requirements] -> [JS status, 1 = done]
 *   4 WaitBO [handle]                  5 GetBOOffset [handle] -> [gpu va]
 *   6 Madvise [handle, madv] -> [retained]    7 Report [code, a, b]
 * A buffer's memory is the client memory of the type that is its handle. The device "file" is
 * /dev/null: one GPU, one connection per process.
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>

#include "xf86drm.h"
#include "drm-uapi/panfrost_drm.h"

/* IOKit.framework has no public headers in the iOS SDK. */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t main_port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallScalarMethod(io_connect_t connection, uint32_t selector,
    const uint64_t *input, uint32_t inputCnt, uint64_t *output, uint32_t *outputCnt);
extern kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, uint32_t options);
extern kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType,
    task_port_t fromTask, mach_vm_address_t atAddress);

#define kIOMapAnywhere          0x00000001
#define kIOMapWriteCombineCache 0x00000400
#define kIOReturnNoMemory       0xe00002bd
#define kIOReturnBadArgument    0xe00002c2

enum { SEL_PARAM, SEL_CREATE, SEL_FREE, SEL_SUBMIT, SEL_WAIT, SEL_OFFSET, SEL_MADVISE, SEL_REPORT };

static io_connect_t conn;
static int conn_fd = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_syncobj = 1;
static uint64_t submits, failed;

/* CPU mappings, to unmap by address. */
#define MAX_MAPS 8192
static struct { uintptr_t addr; uint32_t handle; } maps[MAX_MAPS];

static int
call(uint32_t sel, const uint64_t *in, uint32_t nin, uint64_t *out, uint32_t nout)
{
	uint32_t n = nout;
	kern_return_t kr = IOConnectCallScalarMethod(conn, sel, in, nin, out, &n);
	if (kr == KERN_SUCCESS) {
		return 0;
	}
	errno = kr == kIOReturnNoMemory ? ENOMEM : kr == kIOReturnBadArgument ? EINVAL : EIO;
	return -1;
}

int
s8_drm_open(void)
{
	pthread_mutex_lock(&lock);
	if (conn_fd < 0) {
		io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("S8Mali"));
		if (svc != 0) {
			if (IOServiceOpen(svc, mach_task_self(), 0, &conn) == KERN_SUCCESS) {
				conn_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
			}
			IOObjectRelease(svc);
		}
	}
	int fd = conn_fd >= 0 ? dup(conn_fd) : -1;
	pthread_mutex_unlock(&lock);
	return fd;
}

void
s8_drm_report(uint64_t code, uint64_t a, uint64_t b)
{
	uint64_t in[3] = { code, a, b };
	call(SEL_REPORT, in, 3, NULL, 0);
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	switch (request) {
	case DRM_IOCTL_PANFROST_GET_PARAM: {
		struct drm_panfrost_get_param *p = arg;
		uint64_t in = p->param;
		if (call(SEL_PARAM, &in, 1, &p->value, 1) != 0) {
			return -1;
		}
		/*
		 * Bifrost GPU_ID[31:16] is arch major/minor/rev and product major; this r0p0 G71
		 * reads 0x60a0 (arch rev 0xa). Linux panfrost matches models on & 0xf00f, and
		 * Mesa's model table has G71 as 0x6000: report the masked id.
		 */
		if (p->param == DRM_PANFROST_PARAM_GPU_PROD_ID && (p->value >> 12) >= 6) {
			p->value &= 0xf00f;
		}
		return 0;
	}
	case DRM_IOCTL_PANFROST_CREATE_BO: {
		struct drm_panfrost_create_bo *c = arg;
		uint64_t in[2] = { c->size, c->flags }, out[2];
		if (call(SEL_CREATE, in, 2, out, 2) != 0) {
			return -1;
		}
		c->handle = (uint32_t)out[0];
		c->offset = out[1];
		return 0;
	}
	case DRM_IOCTL_GEM_CLOSE: {
		struct drm_gem_close *g = arg;
		return drmCloseBufferHandle(fd, g->handle);
	}
	case DRM_IOCTL_PANFROST_MMAP_BO: {
		struct drm_panfrost_mmap_bo *m = arg;
		m->offset = (uint64_t)m->handle << 32;  /* decoded by s8_drm_mmap */
		return 0;
	}
	case DRM_IOCTL_PANFROST_GET_BO_OFFSET: {
		struct drm_panfrost_get_bo_offset *o = arg;
		uint64_t in = o->handle;
		return call(SEL_OFFSET, &in, 1, &o->offset, 1);
	}
	case DRM_IOCTL_PANFROST_WAIT_BO:
		return 0;                               /* submits are synchronous */
	case DRM_IOCTL_PANFROST_MADVISE: {
		struct drm_panfrost_madvise *m = arg;
		uint64_t in[2] = { m->handle, m->madv }, out = 1;
		int r = call(SEL_MADVISE, in, 2, &out, 1);
		m->retained = (uint32_t)out;
		return r;
	}
	case DRM_IOCTL_PANFROST_SUBMIT: {
		struct drm_panfrost_submit *s = arg;
		uint64_t in[2] = { s->jc, s->requirements }, status = 0;
		if (call(SEL_SUBMIT, in, 2, &status, 1) != 0) {
			return -1;
		}
		submits++;
		if (status != 1) {
			/* as on Linux, a job fault is not an ioctl error; logged in the kernel */
			failed++;
			if (failed <= 16 || (failed & 255) == 0) {
				s8_drm_report(0x5f, s->jc, status << 32 | (failed & 0xffffffff));
			}
		}
		return 0;
	}
	default:
		s8_drm_report(0x52, request, ENOSYS);
		errno = ENOSYS;
		return -1;
	}
}

void *
s8_drm_mmap(int fd, uint64_t offset, size_t size)
{
	(void)fd;
	uint32_t handle = (uint32_t)(offset >> 32);
	mach_vm_address_t addr = 0;
	mach_vm_size_t len = 0;
	if ((offset & 0xffffffffu) != 0 ||
	    IOConnectMapMemory64(conn, handle, mach_task_self(), &addr, &len,
	    kIOMapAnywhere | kIOMapWriteCombineCache) != KERN_SUCCESS || len < size) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_MAPS; i++) {
		if (maps[i].addr == 0) {
			maps[i].addr = (uintptr_t)addr;
			maps[i].handle = handle;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	return (void *)(uintptr_t)addr;
}

int
s8_drm_munmap(void *address, size_t size)
{
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_MAPS; i++) {
		if (maps[i].addr == (uintptr_t)address) {
			uint32_t handle = maps[i].handle;
			maps[i].addr = 0;
			pthread_mutex_unlock(&lock);
			return IOConnectUnmapMemory64(conn, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address) ==
			       KERN_SUCCESS ? 0 : -1;
		}
	}
	pthread_mutex_unlock(&lock);
	return munmap(address, size);
}

drmVersionPtr
drmGetVersion(int fd)
{
	(void)fd;
	drmVersionPtr v = calloc(1, sizeof(*v));
	if (v == NULL) {
		return NULL;
	}
	/* 1.1: HEAP/NOEXEC BOs and madvise, which is what the driver implements */
	v->version_major = 1;
	v->version_minor = 1;
	v->name = strdup("panfrost");
	v->name_len = 8;
	v->date = strdup("20261024");
	v->date_len = 8;
	v->desc = strdup("S8Mali (Galaxy S8 Mali-G71 on XNU)");
	v->desc_len = (int)strlen(v->desc);
	return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
	if (v != NULL) {
		free(v->name);
		free(v->date);
		free(v->desc);
		free(v);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	(void)fd;
	uint64_t in = handle;
	return call(SEL_FREE, &in, 1, NULL, 0);
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd; (void)handle; (void)flags; (void)prime_fd;
	errno = ENOSYS;
	return -ENOSYS;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	(void)fd; (void)prime_fd; (void)handle;
	errno = ENOSYS;
	return -ENOSYS;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd; (void)flags;
	pthread_mutex_lock(&lock);
	*handle = next_syncobj++;
	pthread_mutex_unlock(&lock);
	return 0;
}

int drmSyncobjDestroy(int fd, uint32_t handle) { (void)fd; (void)handle; return 0; }
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count) { (void)fd; (void)handles; (void)count; return 0; }

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)obj_fd;
	return drmSyncobjCreate(fd, 0, handle);
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	(void)fd; (void)handle;
	*obj_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	return *obj_fd >= 0 ? 0 : -errno;
}

int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd) { (void)fd; (void)handle; (void)sync_file_fd; return 0; }

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	return drmSyncobjHandleToFD(fd, handle, sync_file_fd);
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
               uint32_t *first_signaled)
{
	(void)fd; (void)handles; (void)num_handles; (void)timeout_nsec; (void)flags;
	if (first_signaled != NULL) {
		*first_signaled = 0;
	}
	return 0;
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
                       int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	(void)points;
	return drmSyncobjWait(fd, handles, num_handles, timeout_nsec, flags, first_signaled);
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
                   uint64_t src_point, uint32_t flags)
{
	(void)fd; (void)dst_handle; (void)dst_point; (void)src_handle; (void)src_point; (void)flags;
	return 0;
}
