#include <linux/clk.h>
#include <linux/clk-provider.h> /* XXX */
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

#include "../drm.h"
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
	int err = 0;

	dev_dbg(client->dev, "> %s(client=%p)\n", __func__, client);

	dev_dbg(client->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_display_exit(struct host1x_client *client)
{
	int err = 0;

	dev_dbg(client->dev, "> %s(client=%p)\n", __func__, client);

	dev_dbg(client->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static const struct host1x_client_ops tegra_display_ops = {
	.init = tegra_display_init,
	.exit = tegra_display_exit,
};

static int tegra_display_probe(struct platform_device *pdev)
{
	struct tegra_display *display;
	int err;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	display = devm_kzalloc(&pdev->dev, sizeof(*display), GFP_KERNEL);
	if (!display) {
		err = -ENOMEM;
		goto out;
	}

	display->clk_disp = devm_clk_get(&pdev->dev, "disp");
	if (IS_ERR(display->clk_disp)) {
		err = PTR_ERR(display->clk_disp);
		goto out;
	}

	if (1) {
		struct clk *clk = clk_get_parent(display->clk_disp);

		dev_dbg(&pdev->dev, "  clk: %s parent: %s\n", __clk_get_name(display->clk_disp), __clk_get_name(clk));
	}

	display->clk_dsc = devm_clk_get(&pdev->dev, "dsc");
	if (IS_ERR(display->clk_dsc)) {
		err = PTR_ERR(display->clk_dsc);
		goto out;
	}

	display->clk_hub = devm_clk_get(&pdev->dev, "hub");
	if (IS_ERR(display->clk_hub)) {
		err = PTR_ERR(display->clk_hub);
		goto out;
	}

	display->rst = devm_reset_control_get(&pdev->dev, "misc");
	if (IS_ERR(display->rst)) {
		err = PTR_ERR(display->rst);
		goto out;
	}

	/* XXX: enable clock across reset? */
	reset_control_assert(display->rst);

	platform_set_drvdata(pdev, display);
	pm_runtime_enable(&pdev->dev);

	INIT_LIST_HEAD(&display->client.list);
	display->client.ops = &tegra_display_ops;
	display->client.dev = &pdev->dev;

	err = host1x_client_register(&display->client);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to register host1x client: %d\n",
			err);
		goto out;
	}

	err = pm_runtime_get_sync(&pdev->dev); /* XXX */
	dev_dbg(&pdev->dev, "pm_runtime_get_sync(): %d\n", err);

out:
	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_display_remove(struct platform_device *pdev)
{
	struct tegra_display *display = platform_get_drvdata(pdev);
	int err = 0;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	err = host1x_client_unregister(&display->client);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to unregister host1x client: %d\n",
			err);
	}

	pm_runtime_put(&pdev->dev); /* XXX */
	pm_runtime_disable(&pdev->dev);

	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_display_suspend(struct device *dev)
{
	struct tegra_display *display = dev_get_drvdata(dev);
	int err;

	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);

	err = reset_control_assert(display->rst);
	if (err < 0)
		goto out;

	clk_disable_unprepare(display->clk_hub);
	clk_disable_unprepare(display->clk_dsc);
	clk_disable_unprepare(display->clk_disp);

out:
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_display_resume(struct device *dev)
{
	struct tegra_display *display = dev_get_drvdata(dev);
	int err;

	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);

	err = clk_prepare_enable(display->clk_disp);
	if (err < 0)
		goto out;

	err = clk_prepare_enable(display->clk_dsc);
	if (err < 0)
		goto disable_disp;

	err = clk_prepare_enable(display->clk_hub);
	if (err < 0)
		goto disable_dsc;

	err = reset_control_deassert(display->rst);
	if (err < 0)
		goto disable_hub;

	goto out;

disable_hub:
	clk_disable_unprepare(display->clk_hub);
disable_dsc:
	clk_disable_unprepare(display->clk_dsc);
disable_disp:
	clk_disable_unprepare(display->clk_disp);
out:
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static const struct dev_pm_ops tegra_display_pm_ops = {
	SET_RUNTIME_PM_OPS(tegra_display_suspend, tegra_display_resume, NULL)
};

static const struct of_device_id tegra186_display_of_match[] = {
	{ .compatible = "nvidia,tegra186-display" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tegra186_display_of_match);

struct platform_driver tegra186_display_driver = {
	.driver = {
		.name = "tegra186-display",
		.of_match_table = tegra186_display_of_match,
		.pm = &tegra_display_pm_ops,
	},
	.probe = tegra_display_probe,
	.remove = tegra_display_remove,
};
