#include <linux/dma-mapping.h>

#include "gem.h"

static void gamecube_bo_destroy(struct drm_device *drm, struct gamecube_bo *bo)
{
	dma_free_writecombine(drm->dev, bo->base.size, bo->virt, bo->phys);
}

struct gamecube_bo *gamecube_bo_create(struct drm_device *drm, size_t size)
{
	struct gamecube_bo *bo;
	int err;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	size = round_up(size, PAGE_SIZE);

	bo->virt = dma_alloc_writecombine(drm->dev, size, &bo->phys,
					  GFP_KERNEL);
	if (!bo->virt) {
		err = -ENOMEM;
		goto free;
	}

	err = drm_gem_object_init(drm, &bo->base, size);
	if (err)
		goto destroy;

	err = drm_gem_create_mmap_offset(&bo->base);
	if (err)
		goto release;

	return bo;

release:
	drm_gem_object_release(&bo->base);
destroy:
	gamecube_bo_destroy(drm, bo);
free:
	kfree(bo);
	return ERR_PTR(err);
}

void gamecube_bo_free_object(struct drm_gem_object *gem)
{
	struct gamecube_bo *bo = to_gamecube_bo(gem);

	gamecube_bo_destroy(gem->dev, bo);

	drm_gem_free_mmap_offset(gem);
	drm_gem_object_release(gem);

	kfree(bo);
}
