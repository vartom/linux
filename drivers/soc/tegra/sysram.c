#define DEBUG

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

struct tegra_sysram {
	struct resource mem;
	struct device *dev;

	struct reserved_mem *regions;
	unsigned int num_regions;
};

static int tegra_sysram_dma_device_init(struct reserved_mem *region,
					struct device *dev)
{
	unsigned long flags = DMA_MEMORY_IO | DMA_MEMORY_EXCLUSIVE;
	struct tegra_sysram *sysram = region->priv;
	int err = 0;

	dev_dbg(sysram->dev, "> %s(region=%p, dev=%p)\n", __func__, region, dev);

	err = dma_declare_coherent_memory(dev, region->base, region->base,
					  region->size, flags);
	if (err == 0) {
		err = -ENODEV;
		goto out;
	}

out:
	dev_dbg(sysram->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static void tegra_sysram_dma_device_release(struct reserved_mem *region,
					    struct device *dev)
{
	struct tegra_sysram *sysram = region->priv;

	dev_dbg(sysram->dev, "> %s(region=%p, dev=%p)\n", __func__, region, dev);

	dma_release_declared_memory(dev);

	dev_dbg(sysram->dev, "< %s()\n", __func__);
}

static const struct reserved_mem_ops tegra_sysram_dma_ops = {
	.device_init = tegra_sysram_dma_device_init,
	.device_release = tegra_sysram_dma_device_release,
};

static int tegra_sysram_region_init(struct tegra_sysram *sysram,
				    struct reserved_mem *region,
				    struct device_node *np)
{
	struct resource res;
	int err;

	dev_dbg(sysram->dev, "  %s:\n", np->full_name);

	err = of_address_to_resource(np, 0, &res);
	if (err < 0)
		return err;

	dev_dbg(sysram->dev, "    %pR\n", &res);

	region->name = np->name;
	region->base = res.start;
	region->size = resource_size(&res);
	region->ops = &tegra_sysram_dma_ops;
	region->priv = sysram;

	dev_dbg(sysram->dev, "created DMA memory pool at %pa, size %lu KiB\n",
		&region->base, (unsigned long)region->size / SZ_1K);

	return 0;
}

static int tegra_sysram_probe(struct platform_device *pdev)
{
	struct device_node *parent = pdev->dev.of_node;
	struct tegra_sysram *sysram;
	struct device_node *np;
	phys_addr_t base, size;
	unsigned int i = 0;
	int err = 0;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	sysram = devm_kzalloc(&pdev->dev, sizeof(*sysram), GFP_KERNEL);
	if (!sysram) {
		err = -ENOMEM;
		goto out;
	}

	sysram->dev = &pdev->dev;

	err = of_address_to_resource(parent, 0, &sysram->mem);
	if (err < 0)
		goto out;

	size = resource_size(&sysram->mem);
	base = sysram->mem.start;

	sysram->num_regions = of_get_available_child_count(parent);
	if (sysram->num_regions == 0)
		goto out;

	sysram->regions = devm_kcalloc(&pdev->dev, sysram->num_regions,
					sizeof(struct reserved_mem),
					GFP_KERNEL);
	if (!sysram->regions) {
		err = -ENOMEM;
		goto out;
	}

	for_each_available_child_of_node(parent, np) {
		err = tegra_sysram_region_init(sysram, &sysram->regions[i],
					       np);
		if (err < 0) {
			/* TODO: remove already added regions */
			goto out;
		}
	}

	platform_set_drvdata(pdev, sysram);

out:
	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_sysram_remove(struct platform_device *pdev)
{
	int err = 0;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);

	return err;
}

static const struct of_device_id tegra_sysram_matches[] = {
	{ .compatible = "nvidia,tegra186-sysram", },
	{ /* sentinel */ }
};

static struct platform_driver tegra_sysram_driver = {
	.driver = {
		.name = "tegra-sysram",
		.of_match_table = tegra_sysram_matches,
	},
	.probe = tegra_sysram_probe,
	.remove = tegra_sysram_remove,

};
module_platform_driver(tegra_sysram_driver);
