#ifndef DRM_GAMECUBE_GEM_H
#define DRM_GAMECUBE_GEM_H

#include <drm/drmP.h>

struct gamecube_bo {
	struct drm_gem_object base;
	dma_addr_t phys;
	void *virt;
};

static inline struct gamecube_bo *to_gamecube_bo(struct drm_gem_object *gem)
{
	return container_of(gem, struct gamecube_bo, base);
}

struct gamecube_bo *gamecube_bo_create(struct drm_device *drm, size_t size);
void gamecube_bo_free_object(struct drm_gem_object *gem);

#endif
