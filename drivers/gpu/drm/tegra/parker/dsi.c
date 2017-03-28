#include <linux/host1x.h>
#include <linux/module.h>
#include <linux/platform_device.h>

struct tegra_dsi {
	struct host1x_client client;
};

static int tegra_dsi_init(struct host1x_client *client)
{
	int err = 0;

	dev_dbg(client->dev, "> %s(client=%p)\n", __func__, client);
	dev_dbg(client->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_dsi_exit(struct host1x_client *client)
{
	int err = 0;

	dev_dbg(client->dev, "> %s(client=%p)\n", __func__, client);
	dev_dbg(client->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static const struct host1x_client_ops tegra_dsi_ops = {
	.init = tegra_dsi_init,
	.exit = tegra_dsi_exit,
};

static int tegra_dsi_probe(struct platform_device *pdev)
{
	struct tegra_dsi *dsi;
	int err;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	dsi = devm_kzalloc(&pdev->dev, sizeof(*dsi), GFP_KERNEL);
	if (!dsi) {
		err = -ENOMEM;
		goto out;
	}

	platform_set_drvdata(pdev, dsi);

	INIT_LIST_HEAD(&dsi->client.list);
	dsi->client.ops = &tegra_dsi_ops;
	dsi->client.dev = &pdev->dev;

	err = host1x_client_register(&dsi->client);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to register host1x client: %d\n",
			err);
		goto out;
	}

out:
	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return 0;
}

static int tegra_dsi_remove(struct platform_device *pdev)
{
	struct tegra_dsi *dsi = platform_get_drvdata(pdev);
	int err;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	err = host1x_client_unregister(&dsi->client);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to unregister host1x client: %d\n",
			err);
		goto out;
	}

out:
	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_dsi_suspend(struct device *dev)
{
	int err = 0;
	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_dsi_resume(struct device *dev)
{
	int err = 0;
	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static const struct dev_pm_ops tegra_dsi_pm_ops = {
	SET_RUNTIME_PM_OPS(tegra_dsi_suspend, tegra_dsi_resume, NULL)
};

static const struct of_device_id tegra186_dsi_of_match[] = {
	{ .compatible = "nvidia,tegra186-dsi" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, tegra186_dsi_of_match);

struct platform_driver tegra186_dsi_driver = {
	.driver = {
		.name = "tegra186-dsi",
		.of_match_table = tegra186_dsi_of_match,
		.pm = &tegra_dsi_pm_ops,
	},
	.probe = tegra_dsi_probe,
	.remove = tegra_dsi_remove,
};
