#define DEBUG

#include <linux/component.h>
#include <linux/module.h>
#include <linux/platform_device.h>

static int tegra186_dsi_bind(struct device *dev, struct device *master,
			     void *data)
{
	int err = 0;
	dev_dbg(dev, "> %s(dev=%p, master=%p, data=%p)\n", __func__, dev,
		master, data);
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static void tegra186_dsi_unbind(struct device *dev, struct device *master,
				void *data)
{
	dev_dbg(dev, "> %s(dev=%p, master=%p, data=%p)\n", __func__, dev,
		master, data);
	dev_dbg(dev, "< %s()\n", __func__);
}

static const struct component_ops tegra186_dsi_ops = {
	.bind = tegra186_dsi_bind,
	.unbind = tegra186_dsi_unbind,
};

static int tegra186_dsi_probe(struct platform_device *pdev)
{
	int err;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	err = component_add(&pdev->dev, &tegra186_dsi_ops);

	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return 0;
}

static int tegra186_dsi_remove(struct platform_device *pdev)
{
	int err = 0;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	component_del(&pdev->dev, &tegra186_dsi_ops);

	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra186_dsi_suspend(struct device *dev)
{
	int err = 0;
	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra186_dsi_resume(struct device *dev)
{
	int err = 0;
	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static const struct dev_pm_ops tegra186_dsi_pm_ops = {
	SET_RUNTIME_PM_OPS(tegra186_dsi_suspend, tegra186_dsi_resume,
			   NULL)
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
		.pm = &tegra186_dsi_pm_ops,
	},
	.probe = tegra186_dsi_probe,
	.remove = tegra186_dsi_remove,
};
