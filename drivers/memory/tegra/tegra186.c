#define DEBUG

#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <dt-bindings/memory/tegra186-mc.h>

struct tegra_mc {
	struct device *dev;
	void __iomem *regs;
};

struct tegra_mc_client {
	const char *name;
	unsigned int sid;
	struct {
		unsigned int override;
		unsigned int security;
	} regs;
};

static const struct tegra_mc_client tegra186_mc_clients[] = {
	{
		.name = "eqosr",
		.sid = 0x74,
		.regs = {
			.override = 0x470,
			.security = 0x474,
		},
	}, {
		.name = "eqosw",
		.sid = 0x74,
		.regs = {
			.override = 0x478,
			.security = 0x47c,
		},
	}, {
		.name = "ufshcr",
		.sid = 0x74,
		.regs = {
			.override = 0x480,
			.security = 0x484,
		},
	}, {
		.name = "ufshcw",
		.sid = 0x74,
		.regs = {
			.override = 0x488,
			.security = 0x48c,
		},
	}, {
		.name = "nvdisplayr",
		.sid = TEGRA186_SID_NVDISPLAY,
		.regs = {
			.override = 0x490,
			.security = 0x494,
		},
	}, {
		.name = "bpmpr",
		.sid = 0x74,
		.regs = {
			.override = 0x498,
			.security = 0x49c,
		},
	}, {
		.name = "apedmaw",
		.sid = 0x7f,
		.regs = {
			.override = 0x500,
			.security = 0x504,
		},
	}, {
		.name = "nvdisplayr1",
		.sid = TEGRA186_SID_NVDISPLAY,
		.regs = {
			.override = 0x508,
			.security = 0x50c,
		},
	}, {
		.name = "vicsrd1",
		.sid = 0x7f,
		.regs = {
			.override = 0x510,
			.security = 0x514,
		},
	}, {
		.name = "nvdecsrd1",
		.sid = 0x7f,
		.regs = {
			.override = 0x518,
			.security = 0x51c,
		},
	},
};

static int tegra186_mc_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct tegra_mc *mc;
	unsigned int i;
	int err = 0;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	mc = devm_kzalloc(&pdev->dev, sizeof(*mc), GFP_KERNEL);
	if (!mc) {
		err = -ENOMEM;
		goto out;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mc->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(mc->regs)) {
		err = PTR_ERR(mc->regs);
		goto out;
	}

	mc->dev = &pdev->dev;

	for (i = 0; i < ARRAY_SIZE(tegra186_mc_clients); i++) {
		const struct tegra_mc_client *client = &tegra186_mc_clients[i];
		u32 override, security;

		override = readl(mc->regs + client->regs.override);
		security = readl(mc->regs + client->regs.security);

		dev_dbg(&pdev->dev, "client %s: override: %x security: %x\n",
			client->name, override, security);

		dev_dbg(&pdev->dev, "setting SID %u for %s\n", client->sid,
			client->name);
		writel(client->sid, mc->regs + client->regs.override);

		override = readl(mc->regs + client->regs.override);
		security = readl(mc->regs + client->regs.security);

		dev_dbg(&pdev->dev, "client %s: override: %x security: %x\n",
			client->name, override, security);
	}

	platform_set_drvdata(pdev, mc);

out:
	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static const struct of_device_id tegra186_mc_of_match[] = {
	{ .compatible = "nvidia,tegra186-mc", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tegra186_mc_of_match);

static struct platform_driver tegra186_mc_driver = {
	.driver = {
		.name = "tegra186-mc",
		.of_match_table = tegra186_mc_of_match,
		.suppress_bind_attrs = true,
	},
	.prevent_deferred_probe = true,
	.probe = tegra186_mc_probe,
};
module_platform_driver(tegra186_mc_driver);

MODULE_AUTHOR("Thierry Reding <treding@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA Tegra186 Memory Controller driver");
MODULE_LICENSE("GPL v2");
