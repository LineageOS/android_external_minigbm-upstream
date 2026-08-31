/*
 * Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "drv_helpers.h"
#include "drv_priv.h"
#include "external/dma-buf.h"
#include "external/dma-heap.h"
#include "util.h"

/*
 * QXL and vboxvideo cannot export their local GEM objects through PRIME. Android still needs a
 * transportable, CPU-mappable client target for software rendering, while drmfb copies that target
 * into a display-driver-owned dumb framebuffer. These backends therefore allocate staging buffers
 * from the system DMA heap and must not be used as direct KMS scanout allocators.
 */

static const uint32_t staging_render_formats[] = {
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_RGB565,
};

static const uint32_t staging_texture_formats[] = {
	DRM_FORMAT_R8,
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV21,
	DRM_FORMAT_YVU420,
	DRM_FORMAT_YVU420_ANDROID,
};

struct staging_priv {
	int heap_fd;
};

static bool staging_format_supported(uint32_t format, uint64_t use_flags)
{
	uint64_t allowed = 0;

	for (size_t i = 0; i < ARRAY_SIZE(staging_render_formats); i++) {
		if (format == staging_render_formats[i])
			allowed = BO_USE_RENDER_MASK | BO_USE_SCANOUT | BO_USE_CURSOR;
	}
	for (size_t i = 0; i < ARRAY_SIZE(staging_texture_formats); i++) {
		if (format == staging_texture_formats[i])
			allowed = BO_USE_TEXTURE_MASK;
	}
	if (format == DRM_FORMAT_R8)
		allowed |= BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER |
			   BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_GPU_DATA_BUFFER |
			   BO_USE_SENSOR_DIRECT_DATA;
	if (format == DRM_FORMAT_NV12)
		allowed |= BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER |
			   BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE;
	if (format == DRM_FORMAT_NV21)
		allowed |= BO_USE_HW_VIDEO_ENCODER;

	return allowed && !(use_flags & ~allowed);
}

static int staging_bo_compute_metadata(struct bo *bo, uint32_t width, uint32_t height,
				       uint32_t format, uint64_t use_flags,
				       const uint64_t *modifiers, uint32_t count)
{
	uint32_t aligned_width = ALIGN(width, MESA_LLVMPIPE_TILE_SIZE);
	uint32_t aligned_height = ALIGN(height, MESA_LLVMPIPE_TILE_SIZE);
	uint32_t stride;
	if (!width || !height || width > MESA_LLVMPIPE_MAX_TEXTURE_2D_SIZE ||
	    height > MESA_LLVMPIPE_MAX_TEXTURE_2D_SIZE ||
	    !staging_format_supported(format, use_flags))
		return -EINVAL;

	if (count && !drv_has_modifier(modifiers, count, DRM_FORMAT_MOD_LINEAR) &&
	    !drv_has_modifier(modifiers, count, DRM_FORMAT_MOD_INVALID))
		return -EINVAL;

	stride = drv_stride_from_format(format, aligned_width, 0);
	if (!stride)
		return -EINVAL;

	if (format == DRM_FORMAT_YVU420_ANDROID)
		aligned_height = height;

	return drv_bo_from_format(bo, stride, 1, aligned_height, format);
}

static int staging_bo_create_from_metadata(struct bo *bo)
{
	struct dma_heap_allocation_data args = {
		.len = bo->meta.total_size,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};
	struct staging_priv *priv = bo->drv->priv;

	if (ioctl(priv->heap_fd, DMA_HEAP_IOCTL_ALLOC, &args))
		return -errno;

	bo->handle.fd = args.fd;
	return 0;
}

static int staging_bo_release(struct bo *bo)
{
	return close(bo->handle.fd);
}

static int staging_bo_destroy(struct bo *bo)
{
	(void)bo;
	return 0;
}

static int staging_bo_import(struct bo *bo, struct drv_import_fd_data *data)
{
	int fd;
	struct stat first;
	struct bo expected = *bo;
	uint64_t modifiers[] = { data->format_modifier };

	if (data->fds[0] < 0 || data->offsets[0] ||
	    (data->format_modifier != DRM_FORMAT_MOD_LINEAR &&
	     data->format_modifier != DRM_FORMAT_MOD_INVALID) ||
	    !staging_format_supported(data->format, data->use_flags) ||
	    fstat(data->fds[0], &first))
		return -EINVAL;
	if (staging_bo_compute_metadata(&expected, data->width, data->height, data->format,
					data->use_flags, modifiers, 1) ||
	    first.st_size < 0 || (uint64_t)first.st_size < expected.meta.total_size)
		return -EINVAL;
	for (size_t plane = 1; plane < bo->meta.num_planes; plane++) {
		struct stat current;
		if (data->fds[plane] < 0 || fstat(data->fds[plane], &current) ||
		    current.st_dev != first.st_dev || current.st_ino != first.st_ino)
			return -EINVAL;
	}
	for (size_t plane = 0; plane < bo->meta.num_planes; plane++) {
		if (data->strides[plane] != expected.meta.strides[plane] ||
		    data->offsets[plane] != expected.meta.offsets[plane])
			return -EINVAL;
	}

	fd = fcntl(data->fds[0], F_DUPFD_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	bo->handle.fd = fd;
	return 0;
}

static int staging_bo_export(struct bo *bo, size_t plane)
{
	(void)plane;
	int fd = fcntl(bo->handle.fd, F_DUPFD_CLOEXEC, 0);
	return fd >= 0 ? fd : -errno;
}

static void *staging_bo_map(struct bo *bo, struct vma *vma, uint32_t map_flags)
{
	vma->length = bo->meta.total_size;
	return mmap(NULL, vma->length, drv_get_prot(map_flags), MAP_SHARED, bo->handle.fd, 0);
}

static int staging_bo_sync(struct bo *bo, bool start, uint32_t map_flags)
{
	struct dma_buf_sync args = {
		.flags = start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END,
	};

	if (map_flags & BO_MAP_READ)
		args.flags |= DMA_BUF_SYNC_READ;
	if (map_flags & BO_MAP_WRITE)
		args.flags |= DMA_BUF_SYNC_WRITE;

	return ioctl(bo->handle.fd, DMA_BUF_IOCTL_SYNC, &args) ? -errno : 0;
}

static bool staging_bo_wait(struct bo *bo, uint32_t map_flags)
{
	struct pollfd pollfd = {
		.fd = bo->handle.fd,
		.events = map_flags & BO_MAP_WRITE ? POLLOUT : POLLIN,
	};
	int ret;

	do {
		ret = poll(&pollfd, 1, -1);
	} while (ret < 0 && (errno == EINTR || errno == EAGAIN));

	return ret > 0 && (pollfd.revents & pollfd.events);
}

static int staging_bo_invalidate(struct bo *bo, struct mapping *mapping)
{
	if (!staging_bo_wait(bo, mapping->vma->map_flags))
		return -EINVAL;
	return staging_bo_sync(bo, true, mapping->vma->map_flags);
}

static int staging_bo_flush(struct bo *bo, struct mapping *mapping)
{
	return staging_bo_sync(bo, false, mapping->vma->map_flags);
}

static int staging_init(struct driver *drv)
{
	int heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
	struct staging_priv *priv;
	struct dma_heap_allocation_data args = {
		.len = 4096,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};
	struct dma_buf_sync sync = {
		.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE,
	};
	void *map;
	int ret;

	if (heap_fd < 0) {
		drv_loge("staging backend requires /dev/dma_heap/system: %s\n", strerror(errno));
		return -errno;
	}
	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &args)) {
		int ret = -errno;
		drv_loge("failed to allocate staging probe buffer: %s\n", strerror(errno));
		close(heap_fd);
		return ret;
	}
	map = mmap(NULL, args.len, PROT_READ | PROT_WRITE, MAP_SHARED, args.fd, 0);
	if (map == MAP_FAILED) {
		int ret = -errno;
		drv_loge("failed to map staging probe buffer: %s\n", strerror(errno));
		close(args.fd);
		close(heap_fd);
		return ret;
	}
	if (ioctl(args.fd, DMA_BUF_IOCTL_SYNC, &sync)) {
		int ret = -errno;
		drv_loge("failed to begin staging probe CPU access: %s\n", strerror(errno));
		munmap(map, args.len);
		close(args.fd);
		close(heap_fd);
		return ret;
	}
	((volatile unsigned char *)map)[0] = 0;
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
	if (ioctl(args.fd, DMA_BUF_IOCTL_SYNC, &sync)) {
		int ret = -errno;
		drv_loge("failed to end staging probe CPU access: %s\n", strerror(errno));
		munmap(map, args.len);
		close(args.fd);
		close(heap_fd);
		return ret;
	}
	munmap(map, args.len);
	close(args.fd);

	priv = calloc(1, sizeof(*priv));
	if (!priv) {
		close(heap_fd);
		return -ENOMEM;
	}
	priv->heap_fd = heap_fd;
	drv->priv = priv;

	drv_add_combinations(drv, staging_render_formats, ARRAY_SIZE(staging_render_formats),
			     &LINEAR_METADATA, BO_USE_RENDER_MASK | BO_USE_SCANOUT);
	drv_add_combinations(drv, staging_texture_formats, ARRAY_SIZE(staging_texture_formats),
			     &LINEAR_METADATA, BO_USE_TEXTURE_MASK);
	drv_modify_combination(drv, DRM_FORMAT_R8, &LINEAR_METADATA,
			       BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE |
				   BO_USE_GPU_DATA_BUFFER | BO_USE_SENSOR_DIRECT_DATA);
	drv_modify_combination(drv, DRM_FORMAT_NV12, &LINEAR_METADATA,
			       BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);
	drv_modify_combination(drv, DRM_FORMAT_NV21, &LINEAR_METADATA, BO_USE_HW_VIDEO_ENCODER);

	ret = drv_modify_linear_combinations(drv);
	if (ret) {
		close(priv->heap_fd);
		free(priv);
		drv->priv = NULL;
	}
	return ret;
}

static void staging_close(struct driver *drv)
{
	struct staging_priv *priv = drv->priv;
	if (priv) {
		close(priv->heap_fd);
		free(priv);
	}
}

static uint32_t staging_get_max_texture_2d_size(struct driver *drv)
{
	(void)drv;
	return MESA_LLVMPIPE_MAX_TEXTURE_2D_SIZE;
}

#define STAGING_BACKEND(_name, _generic)                                                        \
	{                                                                                          \
		.is_generic_backend = _generic,                                                    \
		.name = _name,                                                                      \
		.init = staging_init,                                                                \
		.close = staging_close,                                                              \
		.bo_compute_metadata = staging_bo_compute_metadata,                                   \
		.bo_create_from_metadata = staging_bo_create_from_metadata,                           \
		.bo_release = staging_bo_release,                                                     \
		.bo_destroy = staging_bo_destroy,                                                     \
		.bo_import = staging_bo_import,                                                       \
		.bo_export = staging_bo_export,                                                       \
		.bo_map = staging_bo_map,                                                             \
		.bo_unmap = drv_bo_munmap,                                                            \
		.bo_invalidate = staging_bo_invalidate,                                               \
		.bo_flush = staging_bo_flush,                                                         \
		.resolve_format_and_use_flags = drv_resolve_format_and_use_flags_helper,              \
		.get_max_texture_2d_size = staging_get_max_texture_2d_size,                           \
	}

const struct backend backend_staging = STAGING_BACKEND("staging", true);
const struct backend backend_qxl = STAGING_BACKEND("qxl", false);
const struct backend backend_vboxvideo = STAGING_BACKEND("vboxvideo", false);
