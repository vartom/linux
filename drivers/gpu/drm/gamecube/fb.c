#include <linux/err.h>

#include "drm.h"
#include "gem.h"

#include <drm/drm_crtc_helper.h>

struct gamecube_fb {
	struct drm_framebuffer base;
	struct gamecube_bo **planes;
	unsigned int num_planes;
};

static inline struct gamecube_fb *to_gamecube_fb(struct drm_framebuffer *fb)
{
	return container_of(fb, struct gamecube_fb, base);
}

#ifdef CONFIG_DRM_GAMECUBE_FBDEV
static inline struct gamecube_fbdev *
to_gamecube_fbdev(struct drm_fb_helper *helper)
{
	return container_of(helper, struct gamecube_fbdev, base);
}
#endif

static void gamecube_fb_destroy(struct drm_framebuffer *framebuffer)
{
	struct gamecube_fb *fb = to_gamecube_fb(framebuffer);
	unsigned int i;

	for (i = 0; i < fb->num_planes; i++) {
		struct gamecube_bo *bo = fb->planes[i];

		if (bo)
			drm_gem_object_unreference_unlocked(&bo->base);
	}

	drm_framebuffer_cleanup(framebuffer);
	kfree(fb->planes);
	kfree(fb);
}

static int gamecube_fb_create_handle(struct drm_framebuffer *framebuffer,
				     struct drm_file *file,
				     unsigned int *handle)
{
	struct gamecube_fb *fb = to_gamecube_fb(framebuffer);
	struct drm_gem_object *gem = &fb->planes[0]->base;

	return drm_gem_handle_create(file, gem, handle);
}

static struct drm_framebuffer_funcs gamecube_fb_funcs = {
	.destroy = gamecube_fb_destroy,
	.create_handle = gamecube_fb_create_handle,
};

static struct gamecube_fb *gamecube_fb_alloc(struct drm_device *drm,
					     struct drm_mode_fb_cmd2 *mode_cmd,
					     struct gamecube_bo **planes,
					     unsigned int num_planes)
{
	struct gamecube_fb *fb;
	unsigned int i;
	int err;

	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (!fb)
		return ERR_PTR(-ENOMEM);

	fb->planes = kzalloc(num_planes * sizeof(*planes), GFP_KERNEL);
	if (!fb->planes) {
		err = -ENOMEM;
		goto free;
	}

	fb->num_planes = num_planes;

	drm_helper_mode_fill_fb_struct(&fb->base, mode_cmd);

	for (i = 0; i < fb->num_planes; i++)
		fb->planes[i] = planes[i];

	err = drm_framebuffer_init(drm, &fb->base, &gamecube_fb_funcs);
	if (err < 0)
		goto free;

	return fb;

free:
	kfree(fb->planes);
	kfree(fb);
	return ERR_PTR(err);
}

static struct drm_framebuffer *gamecube_fb_create(struct drm_device *drm,
						  struct drm_file *file,
						  struct drm_mode_fb_cmd2 *cmd)
{
	struct gamecube_bo *planes[4];
	unsigned int hsub, vsub, i;
	struct drm_gem_object *gem;
	struct gamecube_fb *fb;
	int err;

	hsub = drm_format_horz_chroma_subsampling(cmd->pixel_format);
	vsub = drm_format_vert_chroma_subsampling(cmd->pixel_format);

	for (i = 0; i < drm_format_num_planes(cmd->pixel_format); i++) {
		unsigned int width = cmd->width / (i ? hsub : 1);
		unsigned int height = cmd->height / (i ? vsub : 1);
		unsigned int size, bpp;

		gem = drm_gem_object_lookup(drm, file, cmd->handles[i]);
		if (!gem) {
			err = -ENXIO;
			goto unref;
		}

		bpp = drm_format_plane_cpp(cmd->pixel_format, i);

		size = (height - 1) * cmd->pitches[i] +
		       width * bpp + cmd->offsets[i];

		if (gem->size < size) {
			err = -EINVAL;
			goto unref;
		}

		planes[i] = to_gamecube_bo(gem);
	}

	fb = gamecube_fb_alloc(drm, cmd, planes, i);
	if (IS_ERR(fb)) {
		err = PTR_ERR(fb);
		goto unref;
	}

	return &fb->base;

unref:
	while (i--)
		drm_gem_object_unreference_unlocked(&planes[i]->base);

	return ERR_PTR(err);
}

#ifdef CONFIG_DRM_GAMECUBE_FBDEV
static struct fb_ops gamecube_fb_ops = {
	.owner = THIS_MODULE,
	.fb_fillrect = sys_fillrect,
	.fb_copyarea = sys_copyarea,
	.fb_imageblit = sys_imageblit,
	.fb_check_var = drm_fb_helper_check_var,
	.fb_set_par = drm_fb_helper_set_par,
	.fb_blank = drm_fb_helper_blank,
	.fb_pan_display = drm_fb_helper_pan_display,
	.fb_setcmap = drm_fb_helper_setcmap,
};

static int gamecube_fbdev_probe(struct drm_fb_helper *helper,
				struct drm_fb_helper_surface_size *sizes)
{
	struct gamecube_fbdev *fbdev = to_gamecube_fbdev(helper);
	struct drm_device *drm = helper->dev;
	struct drm_mode_fb_cmd2 cmd = { 0, };
	unsigned int bytes_per_pixel;
	struct drm_framebuffer *fb;
	struct gamecube_bo *bo;
	struct fb_info *info;
	unsigned long offset;
	size_t size;
	int err;

	bytes_per_pixel = DIV_ROUND_UP(sizes->surface_bpp, 8);

	cmd.width = sizes->surface_width;
	cmd.height = sizes->surface_height;
	cmd.pitches[0] = sizes->surface_width * bytes_per_pixel;
	cmd.pixel_format = drm_mode_legacy_fb_format(sizes->surface_bpp,
						     sizes->surface_depth);
	size = cmd.pitches[0] * cmd.height;

	bo = gamecube_bo_create(drm, size);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	info = framebuffer_alloc(0, drm->dev);
	if (!info) {
		gamecube_bo_free_object(&bo->base);
		return -ENOMEM;
	}

	fbdev->fb = gamecube_fb_alloc(drm, &cmd, &bo, 1);
	if (IS_ERR(fbdev->fb)) {
		err = PTR_ERR(fbdev->fb);
		goto release;
	}

	fb = &fbdev->fb->base;
	helper->fb = fb;
	helper->fbdev = info;

	info->par = helper;
	info->flags = FBINFO_FLAG_DEFAULT;
	info->fbops = &gamecube_fb_ops;

	err = fb_alloc_cmap(&info->cmap, 256, 0);
	if (err < 0)
		goto destroy;

	drm_fb_helper_fill_fix(info, fb->pitches[0], fb->depth);
	drm_fb_helper_fill_var(info, helper, fb->width, fb->height);

	offset = info->var.xoffset * bytes_per_pixel +
		 info->var.yoffset * fb->pitches[0];

	drm->mode_config.fb_base = bo->phys;
	info->screen_base = (void __iomem *)bo->virt + offset;
	info->screen_size = size;
	info->fix.smem_start = (unsigned long)(bo->phys + offset);
	info->fix.smem_len = size;

	return 0;

destroy:
	drm_framebuffer_unregister_private(fb);
	gamecube_fb_destroy(fb);
release:
	framebuffer_release(info);
	return err;
}

static struct drm_fb_helper_funcs gamecube_fb_helper_funcs = {
	.fb_probe = gamecube_fbdev_probe,
};

static struct gamecube_fbdev *gamecube_fbdev_create(struct drm_device *drm,
						    unsigned int preferred_bpp,
						    unsigned int num_crtc,
						    unsigned int max_connectors)
{
	struct gamecube_fbdev *fbdev;
	int err;

	fbdev = kzalloc(sizeof(*fbdev), GFP_KERNEL);
	if (!fbdev)
		return ERR_PTR(-ENOMEM);

	fbdev->base.funcs = &gamecube_fb_helper_funcs;

	err = drm_fb_helper_init(drm, &fbdev->base, num_crtc, max_connectors);
	if (err < 0)
		goto free;

	err = drm_fb_helper_single_add_all_connectors(&fbdev->base);
	if (err < 0)
		goto fini;

	drm_helper_disable_unused_functions(drm);

	err = drm_fb_helper_initial_config(&fbdev->base, preferred_bpp);
	if (err < 0)
		goto fini;

	return fbdev;

fini:
	drm_fb_helper_fini(&fbdev->base);
free:
	kfree(fbdev);
	return ERR_PTR(err);
}

static void gamecube_fbdev_free(struct gamecube_fbdev *fbdev)
{
	struct fb_info *info = fbdev->base.fbdev;
	int err;

	if (info) {
		err = unregister_framebuffer(info);
		if (err < 0)
			DRM_DEBUG_KMS("failed to unregister framebuffer\n");

		if (info->cmap.len)
			fb_dealloc_cmap(&info->cmap);

		framebuffer_release(info);
	}

	if (fbdev->fb) {
		drm_framebuffer_unregister_private(&fbdev->fb->base);
		gamecube_fb_destroy(&fbdev->fb->base);
	}

	drm_fb_helper_fini(&fbdev->base);
	kfree(fbdev);
}

static void gamecube_fb_output_poll_changed(struct drm_device *drm)
{
}
#endif

static const struct drm_mode_config_funcs gamecube_drm_mode_funcs = {
	.fb_create = gamecube_fb_create,
#ifdef CONFIG_DRM_GAMECUBE_FBDEV
	.output_poll_changed = gamecube_fb_output_poll_changed,
#endif
};

int gamecube_drm_fb_init(struct drm_device *drm)
{
#ifdef CONFIG_DRM_GAMECUBE_FBDEV
	struct gamecube *gc = drm->dev_private;
#endif

	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;

	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;

	drm->mode_config.funcs = &gamecube_drm_mode_funcs;

#ifdef CONFIG_DRM_GAMECUBE_FBDEV
	gc->fbdev = gamecube_fbdev_create(drm, 32, drm->mode_config.num_crtc,
					  drm->mode_config.num_connector);
	if (IS_ERR(gc->fbdev))
		return PTR_ERR(gc->fbdev);
#endif

	return 0;
}

void gamecube_drm_fb_exit(struct drm_device *drm)
{
#ifdef CONFIG_DRM_GAMECUBE_FBDEV
	struct gamecube *gc = drm->dev_private;

	gamecube_fbdev_free(gc->fbdev);
#endif
}
