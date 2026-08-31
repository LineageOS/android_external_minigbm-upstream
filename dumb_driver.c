/*
 * Copyright 2020 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drv_helpers.h"
#include "drv_priv.h"
#include "util.h"

#define INIT_DUMB_DRIVER_WITH_NAME(driver, _name)                                                  \
	const struct backend backend_##driver = {                                                  \
		.is_generic_backend = true,                                                        \
		.name = _name,                                                                     \
		.init = dumb_driver_init,                                                          \
		.bo_create = drv_dumb_bo_create,                                                   \
		.bo_create_with_modifiers = dumb_bo_create_with_modifiers,                         \
		.bo_destroy = drv_dumb_bo_destroy,                                                 \
		.bo_import = drv_prime_bo_import,                                                  \
		.bo_export = drv_prime_bo_export,                                                  \
		.bo_map = drv_dumb_bo_map,                                                         \
		.bo_unmap = drv_bo_munmap,                                                         \
		.resolve_format_and_use_flags = drv_resolve_format_and_use_flags_helper,           \
	};

#define INIT_DUMB_DRIVER(driver) INIT_DUMB_DRIVER_WITH_NAME(driver, #driver)

static const uint32_t scanout_render_formats_32[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
						      DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888 };

static const uint32_t scanout_render_formats_24[] = { DRM_FORMAT_BGR888 };

static const uint32_t scanout_render_formats_16[] = { DRM_FORMAT_RGB565 };

static const uint32_t texture_only_formats[] = { DRM_FORMAT_R8, DRM_FORMAT_NV12, DRM_FORMAT_NV21,
						 DRM_FORMAT_YVU420, DRM_FORMAT_YVU420_ANDROID };

static int dumb_driver_check_caps(struct driver *drv)
{
	uint64_t dumb_cap = 0;
	uint64_t prime_cap = 0;

	if (drv->fd < 0 || drmGetCap(drv->fd, DRM_CAP_DUMB_BUFFER, &dumb_cap) || !dumb_cap ||
	    drmGetCap(drv->fd, DRM_CAP_PRIME, &prime_cap) ||
	    (prime_cap & (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)) !=
		    (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)) {
		drv_loge("dumb backend requires dumb buffers and PRIME import/export\n");
		return -ENODEV;
	}

	return 0;
}

static int dumb_driver_probe(struct driver *drv, uint32_t bpp)
{
	uint32_t handle = 0;
	uint32_t imported_handle = 0;
	uint32_t pitch = 0;
	uint64_t size = 0;
	uint64_t map_offset = 0;
	int prime_fd = -1;
	void *map = MAP_FAILED;
	int ret = -ENODEV;

	if (drmModeCreateDumbBuffer(drv->fd, 64, 64, bpp, 0, &handle, &pitch, &size)) {
		drv_loge("failed to create %u-bpp probe dumb buffer\n", bpp);
		goto out;
	}
	if (pitch < DIV_ROUND_UP(64 * bpp, 8) || size < (uint64_t)pitch * 64) {
		drv_loge("%u-bpp probe dumb buffer has an invalid pitch or size\n", bpp);
		goto out;
	}

	if (drmPrimeHandleToFD(drv->fd, handle, DRM_CLOEXEC | DRM_RDWR, &prime_fd) &&
	    drmPrimeHandleToFD(drv->fd, handle, DRM_CLOEXEC, &prime_fd)) {
		drv_loge("failed to export probe dumb buffer\n");
		goto out;
	}

	if (lseek(prime_fd, 0, SEEK_END) < (off_t)size) {
		drv_loge("probe dma-buf does not expose its allocation size\n");
		goto out;
	}

	if (drmPrimeFDToHandle(drv->fd, prime_fd, &imported_handle)) {
		drv_loge("failed to import probe dma-buf\n");
		goto out;
	}

	if (drmModeMapDumbBuffer(drv->fd, handle, &map_offset)) {
		drv_loge("failed to get probe dumb buffer map offset\n");
		goto out;
	}

	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, drv->fd, map_offset);
	if (map == MAP_FAILED) {
		drv_loge("failed to map probe dumb buffer\n");
		goto out;
	}

	((volatile unsigned char *)map)[0] = 0;
	((volatile unsigned char *)map)[size - 1] = 0;
	ret = 0;

out:
	if (map != MAP_FAILED)
		munmap(map, size);
	if (imported_handle && imported_handle != handle)
		drmCloseBufferHandle(drv->fd, imported_handle);
	if (prime_fd >= 0)
		close(prime_fd);
	if (handle)
		drmModeDestroyDumbBuffer(drv->fd, handle);

	return ret;
}

static int dumb_driver_init(struct driver *drv)
{
	int ret = dumb_driver_check_caps(drv);

	if (ret)
		return ret;

	ret = dumb_driver_probe(drv, 32);
	if (ret)
		return ret;

	drv_add_combinations(drv, scanout_render_formats_32, ARRAY_SIZE(scanout_render_formats_32),
			     &LINEAR_METADATA, BO_USE_RENDER_MASK | BO_USE_SCANOUT);

	if (!dumb_driver_probe(drv, 24))
		drv_add_combinations(drv, scanout_render_formats_24,
				     ARRAY_SIZE(scanout_render_formats_24), &LINEAR_METADATA,
				     BO_USE_RENDER_MASK | BO_USE_SCANOUT);

	if (!dumb_driver_probe(drv, 16))
		drv_add_combinations(drv, scanout_render_formats_16,
				     ARRAY_SIZE(scanout_render_formats_16), &LINEAR_METADATA,
				     BO_USE_RENDER_MASK | BO_USE_SCANOUT);

	if (!dumb_driver_probe(drv, 8)) {
		drv_add_combinations(drv, texture_only_formats, ARRAY_SIZE(texture_only_formats),
				     &LINEAR_METADATA, BO_USE_TEXTURE_MASK);

		drv_modify_combination(drv, DRM_FORMAT_R8, &LINEAR_METADATA,
				       BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER |
					   BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE |
					   BO_USE_GPU_DATA_BUFFER | BO_USE_SENSOR_DIRECT_DATA);
		drv_modify_combination(drv, DRM_FORMAT_NV12, &LINEAR_METADATA,
				       BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER |
					   BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);
		drv_modify_combination(drv, DRM_FORMAT_NV21, &LINEAR_METADATA,
				       BO_USE_HW_VIDEO_ENCODER);
	}

	return drv_modify_linear_combinations(drv);
}

static int dumb_bo_create_with_modifiers(struct bo *bo, uint32_t width, uint32_t height,
					 uint32_t format, const uint64_t *modifiers, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		if (modifiers[i] == DRM_FORMAT_MOD_LINEAR) {
			return drv_dumb_bo_create(bo, width, height, format, 0);
		}
	}

	return -EINVAL;
}

INIT_DUMB_DRIVER(dumb_generic)
INIT_DUMB_DRIVER(evdi)
INIT_DUMB_DRIVER(komeda)
INIT_DUMB_DRIVER(marvell)
INIT_DUMB_DRIVER(meson)
INIT_DUMB_DRIVER(nouveau)
INIT_DUMB_DRIVER_WITH_NAME(nvidia, "nvidia-drm")
INIT_DUMB_DRIVER(radeon)
INIT_DUMB_DRIVER_WITH_NAME(sun4i_drm, "sun4i-drm")
INIT_DUMB_DRIVER(synaptics)
INIT_DUMB_DRIVER(tegra)
INIT_DUMB_DRIVER(udl)
INIT_DUMB_DRIVER(vkms)

#ifndef DRV_ROCKCHIP
INIT_DUMB_DRIVER(rockchip)
#endif
#ifndef DRV_MEDIATEK
INIT_DUMB_DRIVER(mediatek)
#endif
