/*
 * Copyright (C) 2017 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/host1x.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>

#include "drm.h"
#include "dc.h"
#include "plane.h"

static const u32 tegra_shared_plane_formats[] = {
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGB565,
};

static unsigned int tegra_plane_offset(struct tegra_plane *plane,
				       unsigned int offset)
{
	if (offset >= 0x500 && offset <= 0x581) {
		offset = 0x000 + (offset - 0x500);
		return plane->offset + offset;
	}

	if (offset >= 0x700 && offset <= 0x73c) {
		offset = 0x180 + (offset - 0x700);
		return plane->offset + offset;
	}

	if (offset >= 0x800 && offset <= 0x83e) {
		offset = 0x1c0 + (offset - 0x800);
		return plane->offset + offset;
	}

	dev_WARN(plane->dc->dev, "invalid offset: %x\n", offset);

	return plane->offset + offset;
}

static u32 tegra_plane_readl(struct tegra_plane *plane, unsigned int offset)
{
	return tegra_dc_readl(plane->dc, tegra_plane_offset(plane, offset));
}

static void tegra_plane_writel(struct tegra_plane *plane, u32 value,
			       unsigned int offset)
{
	tegra_dc_writel(plane->dc, value, tegra_plane_offset(plane, offset));
}

static int tegra_dc_add_window(struct tegra_dc *dc,
			       struct tegra_plane *plane)
{
	unsigned int owner;
	u32 value;

	dev_dbg(dc->dev, "> %s(dc=%p, plane=%p)\n", __func__, dc, plane);

	/* assume we'll be able to succeed */
	plane->dc = dc;

	value = tegra_plane_readl(plane, DC_WIN_CORE_WINDOWGROUP_SET_CONTROL);
	owner = value & OWNER_MASK;

	if (owner != OWNER_MASK && owner != dc->pipe) {
		dev_WARN(dc->dev, "window %u already assigned to head %u\n",
			 plane->index, owner);
		/* make sure we're removed from the DC */
		plane->dc = NULL;
		return -EBUSY;
	}

	value &= ~OWNER_MASK;
	value |= OWNER(dc->pipe);
	tegra_plane_writel(plane, value, DC_WIN_CORE_WINDOWGROUP_SET_CONTROL);

#define DC_WIN_CORE_IHUB_LINEBUF_CONFIG 0x563

	value = tegra_plane_readl(plane, DC_WIN_CORE_IHUB_LINEBUF_CONFIG);
	value &= ~(1 << 14);
	tegra_plane_writel(plane, value, DC_WIN_CORE_IHUB_LINEBUF_CONFIG);

#define DC_WIN_CORE_IHUB_THREAD_GROUP 0x568

	value = tegra_plane_readl(plane, DC_WIN_CORE_IHUB_THREAD_GROUP);
	value |= (plane->index & 0x1f) << 1;
	value |= 1 << 0;
	tegra_plane_writel(plane, value, DC_WIN_CORE_IHUB_THREAD_GROUP);

	dev_dbg(dc->dev, "< %s()\n", __func__);
	return 0;
}

static void tegra_dc_remove_window(struct tegra_dc *dc,
				   struct tegra_plane *plane)
{
	u32 value;

	dev_dbg(dc->dev, "> %s(dc=%p, plane=%p)\n", __func__, dc, plane);

	value = tegra_plane_readl(plane, DC_WIN_CORE_WINDOWGROUP_SET_CONTROL);
	value |= OWNER_MASK;
	tegra_plane_writel(plane, value, DC_WIN_CORE_WINDOWGROUP_SET_CONTROL);

	plane->dc = NULL;

	dev_dbg(dc->dev, "< %s()\n", __func__);
}

static int tegra_shared_plane_atomic_check(struct drm_plane *plane,
					   struct drm_plane_state *state)
{
	struct tegra_plane_state *plane_state = to_tegra_plane_state(state);
	struct tegra_bo_tiling *tiling = &plane_state->tiling;
	struct tegra_plane *tegra = to_tegra_plane(plane);
	struct tegra_dc *dc = to_tegra_dc(state->crtc);
	int err;

	pr_debug("> %s(plane=%p, state=%p)\n", __func__, plane, state);

	/* no need for further checks if the plane is being disabled */
	if (!state->crtc)
		return 0;

	err = tegra_plane_format(state->fb->format->format,
				 &plane_state->format,
				 &plane_state->swap);
	if (err < 0)
		return err;

	err = tegra_fb_get_tiling(state->fb, tiling);
	if (err < 0)
		return err;

	if (tiling->mode == TEGRA_BO_TILING_MODE_BLOCK &&
	    !dc->soc->supports_block_linear) {
		DRM_ERROR("hardware doesn't support block linear mode\n");
		return -EINVAL;
	}

	/*
	 * Tegra doesn't support different strides for U and V planes so we
	 * error out if the user tries to display a framebuffer with such a
	 * configuration.
	 */
	if (state->fb->format->num_planes > 2) {
		if (state->fb->pitches[2] != state->fb->pitches[1]) {
			DRM_ERROR("unsupported UV-plane configuration\n");
			return -EINVAL;
		}
	}

	err = tegra_plane_state_add(tegra, state);
	if (err < 0)
		return err;

	pr_debug("< %s()\n", __func__);
	return 0;
}

static void tegra_shared_plane_atomic_disable(struct drm_plane *plane,
					      struct drm_plane_state *old_state)
{
	struct tegra_dc *dc = to_tegra_dc(old_state->crtc);
	struct tegra_plane *p = to_tegra_plane(plane);
	u32 value;

	pr_debug("> %s(plane=%p, old_state=%p)\n", __func__, plane, old_state);

	/* rien ne va plus */
	if (!old_state || !old_state->crtc)
		return;

	value = tegra_plane_readl(p, DC_WIN_WIN_OPTIONS);
	value &= ~WIN_ENABLE;
	tegra_plane_writel(p, value, DC_WIN_WIN_OPTIONS);

	tegra_dc_remove_window(dc, p);

	pr_debug("< %s()\n", __func__);
}

static void tegra_shared_plane_atomic_update(struct drm_plane *plane,
					     struct drm_plane_state *old_state)
{
	struct tegra_plane_state *state = to_tegra_plane_state(plane->state);
	struct tegra_dc *dc = to_tegra_dc(plane->state->crtc);
	struct drm_framebuffer *fb = plane->state->fb;
	struct tegra_plane *p = to_tegra_plane(plane);
	struct tegra_bo *bo;
	dma_addr_t base;
	u32 value;
	int err;

	pr_debug("> %s(plane=%p, old_state=%p)\n", __func__, plane, old_state);
	pr_debug("  offset: %x\n", p->offset);
	pr_debug("  index: %u\n", p->index);

	/* rien ne va plus */
	if (!plane->state->crtc || !plane->state->fb)
		return;

	if (!plane->state->visible)
		return tegra_shared_plane_atomic_disable(plane, old_state);

	err = tegra_dc_add_window(dc, p);
	if (err < 0)
		return;

	bo = tegra_fb_get_plane(fb, 0);
	base = bo->paddr;

	tegra_plane_writel(p, state->format, DC_WIN_COLOR_DEPTH);

	value = V_POSITION(plane->state->crtc_y) |
		H_POSITION(plane->state->crtc_x);
	tegra_plane_writel(p, value, DC_WIN_POSITION);

	value = V_SIZE(plane->state->crtc_h) | H_SIZE(plane->state->crtc_w);
	tegra_plane_writel(p, value, DC_WIN_SIZE);

	value = WIN_ENABLE;
	tegra_plane_writel(p, value, DC_WIN_WIN_OPTIONS);

	value = V_SIZE(plane->state->crtc_h) | H_SIZE(plane->state->crtc_w);
	tegra_plane_writel(p, value, DC_WIN_CROPPED_SIZE);

	tegra_plane_writel(p, upper_32_bits(base), DC_WINBUF_START_ADDR_HI);
	tegra_plane_writel(p, lower_32_bits(base), DC_WINBUF_START_ADDR);

	value = PITCH(fb->pitches[0]);
	tegra_plane_writel(p, value, DC_WIN_PLANAR_STORAGE);

	/* XXX */
	value = 0;
	tegra_plane_writel(p, value, DC_WIN_SET_PARAMS);

	value = OFFSET_X(plane->state->crtc_y) |
		OFFSET_Y(plane->state->crtc_x);
	tegra_plane_writel(p, value, DC_WINBUF_CROPPED_POINT);

	if (dc->soc->supports_block_linear) {
		unsigned long height = state->tiling.value;

		/* XXX */
		switch (state->tiling.mode) {
		case TEGRA_BO_TILING_MODE_PITCH:
			value = DC_WINBUF_SURFACE_KIND_BLOCK_HEIGHT(0) |
				DC_WINBUF_SURFACE_KIND_PITCH;
			break;

		/* XXX not supported on Tegra186 and later */
		case TEGRA_BO_TILING_MODE_TILED:
			value = DC_WINBUF_SURFACE_KIND_TILED;
			break;

		case TEGRA_BO_TILING_MODE_BLOCK:
			value = DC_WINBUF_SURFACE_KIND_BLOCK_HEIGHT(height) |
				DC_WINBUF_SURFACE_KIND_BLOCK;
			break;
		}

		tegra_plane_writel(p, value, DC_WINBUF_SURFACE_KIND);
	}

	if (0) {
#define DC_WIN_BLEND_MATCH_SELECT 0x717

	value = 0 << 12 | 2 << 8 | 6 << 4 | 5 << 0;
	tegra_plane_writel(p, value, DC_WIN_BLEND_MATCH_SELECT);

#define DC_WIN_BLEND_NOMATCH_SELECT 0x718

	value = 0 << 12 | 2 << 8 | 6 << 4 | 5 << 0;
	tegra_plane_writel(p, value, DC_WIN_BLEND_NOMATCH_SELECT);

#define DC_WIN_BLEND_LAYER_CONTROL 0x716

	if (0)
		value = (plane->index & 0xff) << 0 | (1 << 24);
	else
		value = 0xff << 16 | 0xff << 8 | (0xff - plane->index) << 0;

	tegra_plane_writel(p, value, DC_WIN_BLEND_LAYER_CONTROL);
	}

	pr_debug("< %s()\n", __func__);
}

static const struct drm_plane_helper_funcs tegra_shared_plane_helper_funcs = {
	.atomic_check = tegra_shared_plane_atomic_check,
	.atomic_update = tegra_shared_plane_atomic_update,
	.atomic_disable = tegra_shared_plane_atomic_disable,
};

struct drm_plane *tegra_shared_plane_create(struct drm_device *drm,
					    struct tegra_dc *dc,
					    unsigned int index,
					    enum drm_plane_type type)
{
	/* planes can be assigned to arbitrary CRTCs */
	unsigned int possible_crtcs = 0x7;
	struct tegra_plane *plane;
	unsigned int num_formats;
	const u32 *formats;
	int err;

	plane = kzalloc(sizeof(*plane), GFP_KERNEL);
	if (!plane)
		return ERR_PTR(-ENOMEM);

	plane->offset = 0x0a00 + 0x0300 * index;
	plane->index = index;

	num_formats = ARRAY_SIZE(tegra_shared_plane_formats);
	formats = tegra_shared_plane_formats;

	err = drm_universal_plane_init(drm, &plane->base, possible_crtcs,
				       &tegra_plane_funcs, formats,
				       num_formats, NULL, type, NULL);
	if (err < 0) {
		kfree(plane);
		return ERR_PTR(err);
	}

	drm_plane_helper_add(&plane->base, &tegra_shared_plane_helper_funcs);

	return &plane->base;
}

int tegra_display_hub_atomic_check(struct drm_device *drm,
				   struct drm_atomic_state *state)
{
	struct tegra_atomic_state *s = to_tegra_atomic_state(state);
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	unsigned int i;

	/*
	 * The display hub display clock needs to be fed by the display clock
	 * with the highest frequency to ensure proper functioning of all the
	 * displays.
	 */
	for_each_crtc_in_state(state, crtc, crtc_state, i) {
		struct tegra_dc *dc = to_tegra_dc(crtc);

		if (crtc_state->active) {
			unsigned long rate = clk_get_rate(dc->clk);

			if (!s->clk_disp || rate > clk_get_rate(s->clk_disp))
				s->clk_disp = dc->clk;
		}
	}

	return 0;
}

void tegra_display_hub_atomic_commit(struct drm_device *drm,
				     struct drm_atomic_state *state)
{
	struct tegra_atomic_state *s = to_tegra_atomic_state(state);
	struct tegra_drm *tegra = drm->dev_private;
	struct tegra_display_hub *hub = tegra->hub;

	if (s->clk_disp)
		clk_set_parent(hub->clk_disp, s->clk_disp);
}

static int tegra_display_hub_init(struct host1x_client *client)
{
	struct tegra_display_hub *hub = to_tegra_display_hub(client);
	struct drm_device *drm = dev_get_drvdata(client->parent);
	struct tegra_drm *tegra = drm->dev_private;
	struct drm_plane *plane;
	unsigned int i;

	/*
	 * The first three planes are statically assigned as primary planes
	 * for each of the CRTCs.
	 *
	 * XXX Enable DRM to use any overlay plane as primary, so that we can
	 * create all of the planes as equal and dynamically assign them to
	 * serve as primaries for a CRTC.
	 */
	for (i = 3; i < 6; i++) {
		plane = tegra_shared_plane_create(drm, NULL, i,
						  DRM_PLANE_TYPE_OVERLAY);
		if (IS_ERR(plane))
			return PTR_ERR(plane);
	}

	tegra->hub = hub;

	return 0;
}

static int tegra_display_hub_exit(struct host1x_client *client)
{
	struct drm_device *drm = dev_get_drvdata(client->parent);
	struct tegra_drm *tegra = drm->dev_private;

	tegra->hub = NULL;

	return 0;
}

static const struct host1x_client_ops tegra_display_hub_ops = {
	.init = tegra_display_hub_init,
	.exit = tegra_display_hub_exit,
};

static int tegra_display_hub_probe(struct platform_device *pdev)
{
	struct tegra_display_hub *hub;
	unsigned int i;
	int err;

	hub = devm_kzalloc(&pdev->dev, sizeof(*hub), GFP_KERNEL);
	if (!hub)
		return -ENOMEM;

	hub->clk_disp = devm_clk_get(&pdev->dev, "disp");
	if (IS_ERR(hub->clk_disp)) {
		err = PTR_ERR(hub->clk_disp);
		return err;
	}

	hub->clk_dsc = devm_clk_get(&pdev->dev, "dsc");
	if (IS_ERR(hub->clk_dsc)) {
		err = PTR_ERR(hub->clk_dsc);
		return err;
	}

	hub->clk_hub = devm_clk_get(&pdev->dev, "hub");
	if (IS_ERR(hub->clk_hub)) {
		err = PTR_ERR(hub->clk_hub);
		return err;
	}

	hub->rst = devm_reset_control_get(&pdev->dev, "misc");
	if (IS_ERR(hub->rst)) {
		err = PTR_ERR(hub->rst);
		return err;
	}

	for (i = 0; i < 6; i++) {
		char id[8];

		snprintf(id, sizeof(id), "wgrp%u", i);

		hub->rst_wgrp[i] = devm_reset_control_get(&pdev->dev, id);
		if (IS_ERR(hub->rst_wgrp[i]))
			return PTR_ERR(hub->rst_wgrp[i]);

		err = reset_control_assert(hub->rst_wgrp[i]);
		if (err < 0)
			return err;
	}

	/* XXX: enable clock across reset? */
	err = reset_control_assert(hub->rst);
	if (err < 0)
		return err;

	platform_set_drvdata(pdev, hub);
	pm_runtime_enable(&pdev->dev);

	INIT_LIST_HEAD(&hub->client.list);
	hub->client.ops = &tegra_display_hub_ops;
	hub->client.dev = &pdev->dev;

	err = host1x_client_register(&hub->client);
	if (err < 0)
		dev_err(&pdev->dev, "failed to register host1x client: %d\n",
			err);

	return err;
}

static int tegra_display_hub_remove(struct platform_device *pdev)
{
	struct tegra_display_hub *hub = platform_get_drvdata(pdev);
	int err;

	err = host1x_client_unregister(&hub->client);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to unregister host1x client: %d\n",
			err);
	}

	pm_runtime_disable(&pdev->dev);

	return err;
}

static int tegra_display_hub_suspend(struct device *dev)
{
	struct tegra_display_hub *hub = dev_get_drvdata(dev);
	unsigned int i;
	int err;

	for (i = 0; i < 6; i++) {
		err = reset_control_assert(hub->rst_wgrp[i]);
		if (err < 0)
			goto deassert;
	}

	err = reset_control_assert(hub->rst);
	if (err < 0)
		goto deassert;

	clk_disable_unprepare(hub->clk_hub);
	clk_disable_unprepare(hub->clk_dsc);
	clk_disable_unprepare(hub->clk_disp);

	return 0;

deassert:
	while (--i)
		reset_control_deassert(hub->rst_wgrp[i]);

	return err;
}

static int tegra_display_hub_resume(struct device *dev)
{
	struct tegra_display_hub *hub = dev_get_drvdata(dev);
	unsigned int i;
	int err;

	err = clk_prepare_enable(hub->clk_disp);
	if (err < 0)
		return err;

	err = clk_prepare_enable(hub->clk_dsc);
	if (err < 0)
		goto disable_disp;

	err = clk_prepare_enable(hub->clk_hub);
	if (err < 0)
		goto disable_dsc;

	err = reset_control_deassert(hub->rst);
	if (err < 0)
		goto disable_hub;

	for (i = 0; i < 6; i++) {
		err = reset_control_deassert(hub->rst_wgrp[i]);
		if (err < 0)
			goto assert;
	}

	return 0;

assert:
	while (--i)
		reset_control_assert(hub->rst_wgrp[i]);

	reset_control_assert(hub->rst);
disable_hub:
	clk_disable_unprepare(hub->clk_hub);
disable_dsc:
	clk_disable_unprepare(hub->clk_dsc);
disable_disp:
	clk_disable_unprepare(hub->clk_disp);
	return err;
}

static const struct dev_pm_ops tegra_display_hub_pm_ops = {
	SET_RUNTIME_PM_OPS(tegra_display_hub_suspend,
			   tegra_display_hub_resume, NULL)
};

static const struct of_device_id tegra_display_hub_of_match[] = {
	{ .compatible = "nvidia,tegra186-display" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tegra_display_hub_of_match);

struct platform_driver tegra_display_hub_driver = {
	.driver = {
		.name = "tegra-display-hub",
		.of_match_table = tegra_display_hub_of_match,
		.pm = &tegra_display_hub_pm_ops,
	},
	.probe = tegra_display_hub_probe,
	.remove = tegra_display_hub_remove,
};
