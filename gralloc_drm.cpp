/*
 * Copyright (C) 2010-2011 Chia-I Wu <olvaffe@gmail.com>
 * Copyright (C) 2010-2011 LunarG Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#define LOG_TAG "GRALLOC-DRM"

#include <cutils/log.h>
#include <cutils/atomic.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <tuple>
#include <unordered_map>

#include <android/gralloc_handle.h>

#include "gralloc_drm.h"
#include "gralloc_drm_priv.h"

#define unlikely(x) __builtin_expect(!!(x), 0)

static std::unordered_map<buffer_handle_t, struct gralloc_drm_bo_t *> drm_bo_handle_map;

static int32_t gralloc_drm_pid = 0;

/*
 * Return the pid of the process.
 */
static int gralloc_drm_get_pid(void)
{
	if (unlikely(!gralloc_drm_pid))
		android_atomic_write((int32_t) getpid(), &gralloc_drm_pid);

	return gralloc_drm_pid;
}

/*
 * Create the driver for a DRM fd.
 */
static struct gralloc_drm_drv_t *
init_drv_from_fd(int fd)
{
	struct gralloc_drm_drv_t *drv = NULL;
	drmVersionPtr version;

	/* get the kernel module name */
	version = drmGetVersion(fd);
	if (!version) {
		ALOGE("invalid DRM fd");
		return NULL;
	}

	if (version->name) {
#ifdef ENABLE_FREEDRENO
		if (!strcmp(version->name, "msm")) {
			drv = gralloc_drm_drv_create_for_freedreno(fd);
			ALOGI_IF(drv, "create freedreno for driver msm");
		} else
#endif
#ifdef ENABLE_INTEL
		if (!strcmp(version->name, "i915")) {
			drv = gralloc_drm_drv_create_for_intel(fd);
			ALOGI_IF(drv, "create intel for driver i915");
		} else
#endif
#ifdef ENABLE_RADEON
		if (!strcmp(version->name, "radeon")) {
			drv = gralloc_drm_drv_create_for_radeon(fd);
			ALOGI_IF(drv, "create radeon for driver radeon");
		} else
#endif
#ifdef ENABLE_NOUVEAU
		if (!strcmp(version->name, "nouveau")) {
			drv = gralloc_drm_drv_create_for_nouveau(fd);
			ALOGI_IF(drv, "create nouveau for driver nouveau");
		} else
#endif
#ifdef ENABLE_PIPE
		{
			drv = gralloc_drm_drv_create_for_pipe(fd, version->name);
			ALOGI_IF(drv, "create pipe for driver %s", version->name);
		}
#endif
		if (!drv) {
			ALOGE("unsupported driver: %s", (version->name) ?
					version->name : "NULL");
		}
	}

	drmFreeVersion(version);

	return drv;
}

static const char *fbdrv_map[][2] = {
	{ "amdgpudrmfb",  "amdgpu" },
	{ "inteldrmfb",   "i915" },
	{ "nouveaufb",    "nouveau" },
	{ "radeondrmfb",  "radeon" },
	{ "svgadrmfb",    "vmwgfx" },
	{ "virtiodrmfb",  "virtio_gpu" },
};

/*
 * Create a DRM device object.
 */
struct gralloc_drm_t *gralloc_drm_create(void)
{
	struct gralloc_drm_t *drm;
	FILE *fb;
	unsigned card, ret = 0;
	char buf[64];

	if ((fb = fopen("/proc/fb", "r"))) {
		ret = fscanf(fb, "%u %s", &card, buf);
		fclose(fb);
	}
	if (ret != 2) {
		ALOGE("failed to open /proc/fb");
		return NULL;
	}

	drm = new gralloc_drm_t;
	if (!drm)
		return NULL;

	drm->fd = -1;
	for (card = 0; card < sizeof(fbdrv_map) / sizeof(const char *) / 2; ++card) {
		if (!strcmp(buf, fbdrv_map[card][0])) {
			drm->fd = drmOpen(fbdrv_map[card][1], NULL);
			ALOGD("drmOpen %s: %d", fbdrv_map[card][1], drm->fd);
			break;
		}
	}

	if (drm->fd < 0) {
		ALOGE("failed to open driver for %s", buf);
	} else {
		drm->drv = init_drv_from_fd(drm->fd);
	}

	if (!drm->drv) {
		close(drm->fd);
		delete drm;
		return NULL;
	}

	return drm;
}

/*
 * Destroy a DRM device object.
 */
void gralloc_drm_destroy(struct gralloc_drm_t *drm)
{
	if (drm->drv)
		drm->drv->destroy(drm->drv);
	close(drm->fd);
	delete drm;
}

/*
 * Get the file descriptor of a DRM device object.
 */
int gralloc_drm_get_fd(struct gralloc_drm_t *drm)
{
	return drm->fd;
}

/*
 * Get the magic for authentication.
 */
int gralloc_drm_get_magic(struct gralloc_drm_t *drm, int32_t *magic)
{
	return drmGetMagic(drm->fd, (drm_magic_t *) magic);
}

/*
 * Authenticate a magic.
 */
int gralloc_drm_auth_magic(struct gralloc_drm_t *drm, int32_t magic)
{
	return drmAuthMagic(drm->fd, (drm_magic_t) magic);
}

/*
 * Set as the master of a DRM device.
 */
int gralloc_drm_set_master(struct gralloc_drm_t *drm)
{
	ALOGD("set master");
	drmSetMaster(drm->fd);
	drm->first_post = 1;

	return 0;
}

/*
 * Drop from the master of a DRM device.
 */
void gralloc_drm_drop_master(struct gralloc_drm_t *drm)
{
	drmDropMaster(drm->fd);
}

/*
 * Validate a buffer handle and return the associated bo.
 */
static struct gralloc_drm_bo_t *validate_handle(buffer_handle_t _handle,
		struct gralloc_drm_t *drm)
{
	if (drm_bo_handle_map.count(_handle)) {
		gralloc_drm_bo_t *bo = drm_bo_handle_map[_handle];
		return bo;
	}

	/* check only */
	if (!drm)
		return NULL;

	struct gralloc_handle_t *handle = gralloc_handle(_handle);
	if (!handle)
		return NULL;

	/* create the struct gralloc_drm_bo_t locally */
	ALOGE("handle: name=%d pfd=%d\n", handle->name, handle->prime_fd);
	struct gralloc_drm_bo_t *bo;
	if (handle->name || handle->prime_fd >= 0)
		bo = drm->drv->alloc(drm->drv, handle);
	else /* an invalid handle */
		return NULL;

	bo->drm = drm;
	bo->imported = 1;
	bo->handle = handle;
	bo->refcount = 1;

	return bo;
}

/*
 * Register a buffer handle.
 */
int gralloc_drm_handle_register(buffer_handle_t handle, struct gralloc_drm_t *drm)
{
	struct gralloc_drm_bo_t *bo;

	bo = validate_handle(handle, drm);
	if (!bo)
		return -EINVAL;

	bo->refcount++;

	return 0;
}

/*
 * Unregister a buffer handle.  It is no-op for handles created locally.
 */
int gralloc_drm_handle_unregister(buffer_handle_t handle)
{
	struct gralloc_drm_bo_t *bo;

	bo = validate_handle(handle, NULL);
	if (!bo)
		return -EINVAL;

	gralloc_drm_bo_decref(bo);
	if (bo->imported)
		gralloc_drm_bo_decref(bo);

	return 0;
}

/*
 * Create a bo.
 */
struct gralloc_drm_bo_t *gralloc_drm_bo_create(struct gralloc_drm_t *drm,
		int width, int height, int format, int usage)
{
	struct gralloc_drm_bo_t *bo;
	struct gralloc_handle_t *handle;

	handle = gralloc_handle(gralloc_handle_create(width, height, format, usage));
	if (!handle)
		return NULL;

	bo = drm->drv->alloc(drm->drv, handle);
	if (!bo) {
		delete handle;
		return NULL;
	}

	bo->drm = drm;
	bo->imported = 0;
	bo->handle = handle;
	bo->fb_id = 0;
	bo->refcount = 1;

	return bo;
}

/*
 * Destroy a bo.
 */
static void gralloc_drm_bo_destroy(struct gralloc_drm_bo_t *bo)
{
	struct gralloc_drm_handle_t *handle = bo->handle;
	int imported = bo->imported;

	/* gralloc still has a reference */
	if (bo->refcount)
		return;

	gralloc_drm_bo_rm_fb(bo);

	bo->drm->drv->free(bo->drm->drv, bo);
	if (!imported) {
		delete handle;
	}
}

/*
 * Decrease refcount, if no refs anymore then destroy.
 */
void gralloc_drm_bo_decref(struct gralloc_drm_bo_t *bo)
{
	if (!--bo->refcount)
		gralloc_drm_bo_destroy(bo);
}

/*
 * Return the bo of a registered handle.
 */
struct gralloc_drm_bo_t *gralloc_drm_bo_from_handle(buffer_handle_t handle)
{
	return validate_handle(handle, NULL);
}

/*
 * Get the buffer handle and stride of a bo.
 */
buffer_handle_t gralloc_drm_bo_get_handle(struct gralloc_drm_bo_t *bo, int *stride)
{
	if (stride)
		*stride = bo->handle->stride;
	return &bo->handle->base;
}

int gralloc_drm_get_gem_handle(buffer_handle_t _handle)
{
	struct gralloc_drm_handle_t *handle = gralloc_handle(_handle);
	return (handle) ? handle->name : 0;
}

int gralloc_drm_get_prime_fd(buffer_handle_t _handle)
{
	struct gralloc_drm_handle_t *handle = gralloc_handle(_handle);
	return (handle) ? handle->prime_fd : -1;
}

/*
 * Query YUV component offsets for a buffer handle
 */
void gralloc_drm_resolve_format(buffer_handle_t _handle,
	uint32_t *pitches, uint32_t *offsets, uint32_t *handles)
{
	struct gralloc_drm_handle_t *handle = gralloc_handle(_handle);
	struct gralloc_drm_bo_t *bo = drm_bo_handle_map[_handle];
	struct gralloc_drm_t *drm = bo->drm;

	/* if handle exists and driver implements resolve_format */
	if (handle && drm->drv->resolve_format)
		drm->drv->resolve_format(drm->drv, bo,
			pitches, offsets, handles);
}

/*
 * Lock a bo.  XXX thread-safety?
 */
int gralloc_drm_bo_lock(struct gralloc_drm_bo_t *bo,
		uint32_t usage, int x, int y, int w, int h,
		void **addr)
{
	if ((bo->handle->usage & usage) != usage) {
		/* make FB special for testing software renderer with */
		if (!(bo->handle->usage & (
				GRALLOC_USAGE_SW_READ_OFTEN |
				GRALLOC_USAGE_HW_FB |
				GRALLOC_USAGE_HW_TEXTURE |
				GRALLOC_USAGE_HW_VIDEO_ENCODER))) {
			ALOGE("bo.usage:x%X/usage:x%X is not GRALLOC_USAGE_HW_{FB,TEXTURE,VIDEO_ENCODER}",
					bo->handle->usage, usage);
			return -EINVAL;
		}
	}

	/* allow multiple locks with compatible usages */
	if (bo->lock_count && (bo->locked_for & usage) != usage)
		return -EINVAL;

	usage |= bo->locked_for;

	if (usage & (GRALLOC_USAGE_SW_WRITE_MASK |
		     GRALLOC_USAGE_SW_READ_MASK)) {
		/* the driver is supposed to wait for the bo */
		int write = !!(usage & GRALLOC_USAGE_SW_WRITE_MASK);
		int err = bo->drm->drv->map(bo->drm->drv, bo,
				x, y, w, h, write, addr);
		if (err)
			return err;
	}
	else {
		/* kernel handles the synchronization here */
	}

	bo->lock_count++;
	bo->locked_for |= usage;

	return 0;
}

/*
 * Unlock a bo.
 */
void gralloc_drm_bo_unlock(struct gralloc_drm_bo_t *bo)
{
	int mapped = bo->locked_for &
		(GRALLOC_USAGE_SW_WRITE_MASK | GRALLOC_USAGE_SW_READ_MASK);

	if (!bo->lock_count)
		return;

	if (mapped)
		bo->drm->drv->unmap(bo->drm->drv, bo);

	bo->lock_count--;
	if (!bo->lock_count)
		bo->locked_for = 0;
}
