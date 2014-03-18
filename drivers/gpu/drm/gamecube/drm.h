#ifndef DRM_GAMECUBE_DRM_H
#define DRM_GAMECUBE_DRM_H

#include <drm/drmP.h>
#include <drm/drm_fb_helper.h>

struct gamecube_fb;

#ifdef CONFIG_DRM_GAMECUBE_FBDEV
struct gamecube_fbdev {
	struct drm_fb_helper base;
	struct gamecube_fb *fb;
};
#endif

struct gamecube {
	struct drm_device *drm;

	void __iomem *regs;
	void __iomem *base;

#ifdef CONFIG_DRM_GAMECUBE_FBDEV
	struct gamecube_fbdev *fbdev;
#endif
};

#endif
