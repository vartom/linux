#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include "../drm.h"

struct tegra_dpaux {
	void __iomem *regs;
	struct clk *clk;
	struct reset_control *rst;
};

static int tegra186_dpaux_probe(struct platform_device *pdev)
{
	struct tegra_dpaux *dpaux;
	struct resource *res;
	int err = 0;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	dpaux = devm_kzalloc(&pdev->dev, sizeof(*dpaux), GFP_KERNEL);
	if (!dpaux) {
		err = -ENOMEM;
		goto out;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dpaux->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(dpaux->regs)) {
		err = PTR_ERR(dpaux->regs);
		goto out;
	}

	dpaux->clk = devm_clk_get(&pdev->dev, "dpaux");
	if (IS_ERR(dpaux->clk)) {
		err = PTR_ERR(dpaux->clk);
		goto out;
	}

	dpaux->rst = devm_reset_control_get(&pdev->dev, "dpaux");
	if (IS_ERR(dpaux->rst)) {
		err = PTR_ERR(dpaux->rst);
		goto out;
	}

	/* assert reset and disable clock */
	err = clk_prepare_enable(dpaux->clk);
	if (err < 0)
		goto out;

	usleep_range(2000, 4000);

	err = reset_control_assert(dpaux->rst);
	if (err < 0)
		goto out;

	usleep_range(2000, 4000);

	clk_disable_unprepare(dpaux->clk);

	platform_set_drvdata(pdev, dpaux);
	pm_runtime_enable(&pdev->dev);

	/* we need to keep this on, otherwise HDMI DDC won't work */
	pm_runtime_get_sync(&pdev->dev);

	/* configure for HDMI DDC */
	if (1) {
		u32 value;

		value = (1 << 15) | (1 << 14) | (1 << 0);
		writel(value, dpaux->regs + 0x124);

		value = readl(dpaux->regs + 0x134);
		value &= ~(1 << 0);
		writel(value, dpaux->regs + 0x134);
	}

out:
	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra186_dpaux_remove(struct platform_device *pdev)
{
	int err = 0;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	pm_runtime_put(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra186_dpaux_suspend(struct device *dev)
{
	struct tegra_dpaux *dpaux = dev_get_drvdata(dev);
	int err;

	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);

	err = reset_control_assert(dpaux->rst);
	if (err < 0)
		goto out;

	clk_disable_unprepare(dpaux->clk);

out:
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra186_dpaux_resume(struct device *dev)
{
	struct tegra_dpaux *dpaux = dev_get_drvdata(dev);
	int err;

	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);

	err = clk_prepare_enable(dpaux->clk);
	if (err < 0)
		goto out;

	err = reset_control_deassert(dpaux->rst);
	if (err < 0) {
		clk_disable_unprepare(dpaux->clk);
		goto out;
	}

out:
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static const struct dev_pm_ops tegra186_dpaux_pm_ops = {
	SET_RUNTIME_PM_OPS(tegra186_dpaux_suspend, tegra186_dpaux_resume,
			   NULL)
};

static const struct of_device_id tegra186_dpaux_of_match[] = {
	{ .compatible = "nvidia,tegra186-dpaux" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tegra186_dpaux_of_match);

struct platform_driver tegra186_dpaux_driver = {
	.driver = {
		.name = "tegra186-dpaux",
		.of_match_table = tegra186_dpaux_of_match,
		.pm = &tegra186_dpaux_pm_ops,
	},
	.probe = tegra186_dpaux_probe,
	.remove = tegra186_dpaux_remove,
};
