/*
 * Copyright (C) 2021-2022 Roman Stratiienko (r.stratiienko@gmail.com)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

struct gbm_device;
struct gbm_bo;

struct alloc_args {
	struct gbm_device *gbm;
	uint32_t width;
	uint32_t height;
	uint32_t drm_format;
	bool use_scanout;
	bool force_linear;
	bool needs_map_stride;
	int out_fd;
	uint32_t out_stride;
	uint64_t out_modifier;
	uint32_t out_map_stride;
};

typedef void (*gbm_device_destroy_func)(struct gbm_device *gbm);
typedef struct gbm_device *(*gbm_create_device_func)(int fd);
typedef struct gbm_bo *(*gbm_bo_create_func)(struct gbm_device *gbm, uint32_t width,
					     uint32_t height, uint32_t format, uint32_t flags);
typedef struct gbm_bo *(*gbm_bo_import_func)(struct gbm_device *gbm, uint32_t type, void *buffer,
					     uint32_t flags);
typedef void *(*gbm_bo_map_func)(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t width,
				 uint32_t height, uint32_t flags, uint32_t *stride,
				 void **map_data);
typedef void (*gbm_bo_unmap_func)(struct gbm_bo *bo, void *map_data);
typedef uint32_t (*gbm_bo_get_stride_func)(struct gbm_bo *bo);
typedef int (*gbm_bo_get_fd_func)(struct gbm_bo *bo);
typedef uint64_t (*gbm_bo_get_modifier_func)(struct gbm_bo *bo);
typedef void (*gbm_bo_destroy_func)(struct gbm_bo *bo);

struct gbm_priv_ops {
	bool is_initialized;

	gbm_device_destroy_func gbm_device_destroy;
	gbm_create_device_func gbm_create_device;
	gbm_bo_create_func gbm_bo_create;
	gbm_bo_import_func gbm_bo_import;
	gbm_bo_map_func gbm_bo_map;
	gbm_bo_unmap_func gbm_bo_unmap;
	gbm_bo_get_stride_func gbm_bo_get_stride;
	gbm_bo_get_fd_func gbm_bo_get_fd;
	gbm_bo_get_modifier_func gbm_bo_get_modifier;
	gbm_bo_destroy_func gbm_bo_destroy;
};

struct gbm_ops {
	uint32_t (*get_gbm_format)(uint32_t drm_format);

	struct gbm_device *(*dev_create)(int fd);

	void (*dev_destroy)(struct gbm_device *gbm);

	// ALLOCATOR ONLY
	int (*alloc)(struct alloc_args *args);

	// MAPPER ONLY
	struct gbm_bo *(*import)(struct gbm_device *gbm, int buf_fd, uint32_t width,
				 uint32_t height, uint32_t stride, uint64_t modifier,
				 uint32_t drm_format);

	void (*free)(struct gbm_bo *bo);

	void (*map)(struct gbm_bo *bo, int w, int h, void **addr, void **map_data);

	void (*unmap)(struct gbm_bo *bo, void *map_data);
};

struct gbm_ops *get_gbm_ops();

#ifdef __cplusplus
}
#endif
