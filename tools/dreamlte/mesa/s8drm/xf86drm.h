/*
 * The part of libdrm's xf86drm.h that Mesa's Panfrost (gallium driver, pan_kmod) uses, for iOS on
 * the Galaxy S8, where there is no DRM: s8_drm.c carries the panfrost requests to the kernel's
 * Mali driver (the IOKit service S8Mali, iokit/Kernel/arm/S8Mali.cpp from
 * tools/dreamlte/dreamlte_mali.cpp) as calls of its user client. Request numbers and structures
 * stay Linux's (Mesa's include/drm-uapi/panfrost_drm.h). Same idea as HackiOS's redwood-drm
 * (MIT) for freedreno.
 */
#ifndef S8_XF86DRM_H
#define S8_XF86DRM_H

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "drm-uapi/drm.h"

#define DRM_NODE_PRIMARY        0
#define DRM_NODE_RENDER         2
#define DRM_CLOEXEC             O_CLOEXEC
#define DRM_RDWR                O_RDWR

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

int drmIoctl(int fd, unsigned long request, void *arg);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmCloseBufferHandle(int fd, uint32_t handle);

/* No dma-bufs or sync files here: the submit is synchronous, so every syncobj is signalled. */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
                   unsigned flags, uint32_t *first_signaled);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
                           int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
                       uint64_t src_point, uint32_t flags);

/* Not libdrm's. */
int s8_drm_open(void);                                  /* -1 without the S8Mali service */
void *s8_drm_mmap(int fd, uint64_t offset, size_t size); /* offset from DRM_IOCTL_PANFROST_MMAP_BO */
int s8_drm_munmap(void *address, size_t size);
void s8_drm_report(uint64_t code, uint64_t a, uint64_t b); /* to the kernel log */

#endif
