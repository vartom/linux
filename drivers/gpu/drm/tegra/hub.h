/*
 * Copyright (C) 2017 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef TEGRA_HUB_H
#define TEGRA_HUB_H 1

#include <drm/drmP.h>
#include <drm/drm_plane.h>

struct tegra_display_hub {
	struct host1x_client client;
	struct clk *clk_disp;
	struct clk *clk_dsc;
	struct clk *clk_hub;
	struct reset_control *rst;

	struct reset_control *rst_wgrp[6];
};

static inline struct tegra_display_hub *
to_tegra_display_hub(struct host1x_client *client)
{
	return container_of(client, struct tegra_display_hub, client);
}

struct tegra_dc;

struct drm_plane *tegra_shared_plane_create(struct drm_device *drm,
					    struct tegra_dc *dc,
					    unsigned int index,
					    enum drm_plane_type type);

int tegra_display_hub_atomic_check(struct drm_device *drm,
				   struct drm_atomic_state *state);
void tegra_display_hub_atomic_commit(struct drm_device *drm,
				     struct drm_atomic_state *state);

#endif /* TEGRA_HUB_H */
