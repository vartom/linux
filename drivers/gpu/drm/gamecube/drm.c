#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>

static const struct of_device_id gamecube_drm_match[] = {
	{ .compatible = "nintendo,hollywood-vi", },
	{ .compatible = "nintendo,flipper-vi", },
	{ }
};

struct gamecube_drm {
	void __iomem *regs;
	void __iomem *base;
};

static int gamecube_probe(struct platform_device *pdev)
{
	unsigned int width = 640;
	unsigned int height = 576;
	unsigned int stride = width * 2;
	u32 size = stride * height;
	u32 phys = 0x01698000;
	struct gamecube_drm *gc;
	struct resource *regs;
	unsigned int i, j;

	gc = devm_kzalloc(&pdev->dev, sizeof(*gc), GFP_KERNEL);
	if (!gc)
		return -ENOMEM;

	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	gc->regs = devm_ioremap_resource(&pdev->dev, regs);
	if (IS_ERR(gc->regs))
		return PTR_ERR(gc->regs);

	gc->base = devm_ioremap_nocache(&pdev->dev, phys, size);
	if (!gc->base)
		return -ENXIO;

	for (j = 0; j < height; j++) {
		u16 *ptr = gc->base + (j * width * 2);

		for (i = 0; i < width; i++)
			ptr[i] = 0xffff;
	}

	writel(0x10000000 | phys >> 5, gc->regs + 0x1c); /* top field */
	writel(0x10000000 | (phys + stride) >> 5, gc->regs + 0x24); /* bottom field */

	platform_set_drvdata(pdev, gc);

	return 0;
}

static int gamecube_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver gamecube_drm_driver = {
	.driver = {
		.name = "gamecube-drm",
		.of_match_table = gamecube_drm_match,
	},
	.probe = gamecube_probe,
	.remove = gamecube_remove,
};
module_platform_driver(gamecube_drm_driver);

MODULE_AUTHOR("Thierry Reding <thierry.reding@gmail.com>");
MODULE_DESCRIPTION("Nintendo GameCube and Wii DRM driver");
MODULE_LICENSE("GPL v2");
