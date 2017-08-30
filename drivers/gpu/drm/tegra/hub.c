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

struct tegra_display {
	struct host1x_client client;
	struct clk *clk_disp;
	struct clk *clk_dsc;
	struct clk *clk_hub;
	struct reset_control *rst;
};

static int tegra_display_init(struct host1x_client *client)
{
	return 0;
}

static int tegra_display_exit(struct host1x_client *client)
{
	return 0;
}

static const struct host1x_client_ops tegra_display_ops = {
	.init = tegra_display_init,
	.exit = tegra_display_exit,
};

static int tegra_display_probe(struct platform_device *pdev)
{
	struct tegra_display *display;
	int err;

	display = devm_kzalloc(&pdev->dev, sizeof(*display), GFP_KERNEL);
	if (!display)
		return -ENOMEM;

	display->clk_disp = devm_clk_get(&pdev->dev, "disp");
	if (IS_ERR(display->clk_disp)) {
		err = PTR_ERR(display->clk_disp);
		return err;
	}

	display->clk_dsc = devm_clk_get(&pdev->dev, "dsc");
	if (IS_ERR(display->clk_dsc)) {
		err = PTR_ERR(display->clk_dsc);
		return err;
	}

	display->clk_hub = devm_clk_get(&pdev->dev, "hub");
	if (IS_ERR(display->clk_hub)) {
		err = PTR_ERR(display->clk_hub);
		return err;
	}

	display->rst = devm_reset_control_get(&pdev->dev, "misc");
	if (IS_ERR(display->rst)) {
		err = PTR_ERR(display->rst);
		return err;
	}

	/* XXX: enable clock across reset? */
	err = reset_control_assert(display->rst);
	if (err < 0)
		return err;

	platform_set_drvdata(pdev, display);
	pm_runtime_enable(&pdev->dev);

	INIT_LIST_HEAD(&display->client.list);
	display->client.ops = &tegra_display_ops;
	display->client.dev = &pdev->dev;

	err = host1x_client_register(&display->client);
	if (err < 0)
		dev_err(&pdev->dev, "failed to register host1x client: %d\n",
			err);

	return err;
}

static int tegra_display_remove(struct platform_device *pdev)
{
	struct tegra_display *display = platform_get_drvdata(pdev);
	int err;

	err = host1x_client_unregister(&display->client);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to unregister host1x client: %d\n",
			err);
	}

	pm_runtime_disable(&pdev->dev);

	return err;
}

static int tegra_display_suspend(struct device *dev)
{
	struct tegra_display *display = dev_get_drvdata(dev);
	int err;

	err = reset_control_assert(display->rst);
	if (err < 0)
		return err;

	clk_disable_unprepare(display->clk_hub);
	clk_disable_unprepare(display->clk_dsc);
	clk_disable_unprepare(display->clk_disp);

	return 0;
}

static int tegra_display_resume(struct device *dev)
{
	struct tegra_display *display = dev_get_drvdata(dev);
	int err;

	err = clk_prepare_enable(display->clk_disp);
	if (err < 0)
		return err;

	err = clk_prepare_enable(display->clk_dsc);
	if (err < 0)
		goto disable_disp;

	err = clk_prepare_enable(display->clk_hub);
	if (err < 0)
		goto disable_dsc;

	err = reset_control_deassert(display->rst);
	if (err < 0)
		goto disable_hub;

	return 0;

disable_hub:
	clk_disable_unprepare(display->clk_hub);
disable_dsc:
	clk_disable_unprepare(display->clk_dsc);
disable_disp:
	clk_disable_unprepare(display->clk_disp);
	return err;
}

static const struct dev_pm_ops tegra_display_pm_ops = {
	SET_RUNTIME_PM_OPS(tegra_display_suspend, tegra_display_resume, NULL)
};

static const struct of_device_id tegra_display_of_match[] = {
	{ .compatible = "nvidia,tegra186-display" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tegra_display_of_match);

struct platform_driver tegra_display_driver = {
	.driver = {
		.name = "tegra-display",
		.of_match_table = tegra_display_of_match,
		.pm = &tegra_display_pm_ops,
	},
	.probe = tegra_display_probe,
	.remove = tegra_display_remove,
};
