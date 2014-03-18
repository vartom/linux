#include <linux/console.h>
#include <linux/font.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "drm.h"

static const struct of_device_id gamecube_match[] = {
	{ .compatible = "nintendo,hollywood-vi", },
	{ .compatible = "nintendo,flipper-vi", },
	{ }
};

void wii_disable_slot_led(void);
void wii_enable_slot_led(void);

struct gamecube_console {
	struct console console;
	bool initialized;
	const struct font_desc *font;
	void __iomem *base;
	unsigned int width;
	unsigned int height;
	unsigned int stride;
	unsigned int x;
	unsigned int y;
};

#define WHITE 0xff80ff80
#define BLACK 0x00800080

static void gamecube_console_putc(struct gamecube_console *console, char c)
{
	const struct font_desc *font = console->font;
	const u8 *glyph;
	int i, j;
	u32 *ptr;

	switch (c) {
	case '\n':
		console->y += font->height;
		console->x = 0;
		break;

	default:
		ptr = console->base + console->stride * console->y + console->x * 2;
		glyph = font->data + c * font->height;

		for (j = 0; j < font->height; j++) {
			for (i = 0; i < font->width; i += 2) {
				u8 byte = glyph[j + i / 8];
				u32 color;

				if (byte & (1 << (font->width - (i + 0) % 8)))
					color = WHITE & 0xffff00ff;
				else
					color = BLACK & 0xffff00ff;

				if (byte & (1 << (font->width - (i + 1) % 8)))
					color |= WHITE & 0x0000ff00;
				else
					color |= BLACK & 0x0000ff00;

				ptr[i / 2] = color;
			}

			ptr += console->stride / 4;
		}

		console->x += font->width;
		break;
	}

	if (console->x + font->width > console->width) {
		console->y += font->height;
		console->x = 0;
	}
}

static void gamecube_console_write(struct console *console, const char *b,
				   unsigned count)
{
	struct gamecube_console *gc = container_of(console, struct gamecube_console, console);

	while (count--)
		gamecube_console_putc(gc, *b++);
}

static struct gamecube_console console = {
	.console = {
		.name = "gamecube",
		.write = gamecube_console_write,
		.flags = CON_PRINTBUFFER,
		.index = -1,
	},
};

static void gamecube_console_puts(struct gamecube_console *console, const char *s)
{
	while (*s)
		gamecube_console_putc(console, *s++);
}

void gamecube_console_printk(const char *text, size_t len)
{
	if (console.initialized) {
		while (len--)
			gamecube_console_putc(&console, *text++);
	}
}

static int gamecube_drm_load(struct drm_device *drm, unsigned long flags)
{
	struct gamecube *gc = platform_get_drvdata(drm->platformdev);

	drm->dev_private = gc;
	gc->drm = drm;

	return 0;
}

static int gamecube_drm_unload(struct drm_device *drm)
{
	return 0;
}

static int gamecube_drm_open(struct drm_device *drm, struct drm_file *file)
{
	return 0;
}

static void gamecube_drm_preclose(struct drm_device *drm, struct drm_file *file)
{
}

static void gamecube_drm_lastclose(struct drm_device *drm)
{
}

static u32 gamecube_drm_get_vblank_counter(struct drm_device *drm, int crtc)
{
	return drm_vblank_count(drm, crtc);
}

static int gamecube_drm_enable_vblank(struct drm_device *drm, int pipe)
{
	return 0;
}

static void gamecube_drm_disable_vblank(struct drm_device *drm, int pipe)
{
}

#ifdef CONFIG_DEBUG_FS
static int gamecube_debugfs_init(struct drm_minor *minor)
{
	return 0;
}

static void gamecube_debugfs_cleanup(struct drm_minor *minor)
{
}
#endif

static void gamecube_bo_free_object(struct drm_gem_object *gem)
{
}

static const struct vm_operations_struct gamecube_bo_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static int gamecube_bo_dumb_create(struct drm_file *file, struct drm_device *drm,
				   struct drm_mode_create_dumb *args)
{
	return 0;
}

static int gamecube_bo_dumb_map_offset(struct drm_file *file, struct drm_device *drm, uint32_t handle, uint64_t *offset)
{
	return 0;
}

static const struct drm_ioctl_desc gamecube_drm_ioctls[] = {
};

static int gamecube_drm_mmap(struct file *file, struct vm_area_struct *vma)
{
	return 0;
}

static const struct file_operations gamecube_drm_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.mmap = gamecube_drm_mmap,
	.poll = drm_poll,
	.read = drm_read,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
	.llseek = noop_llseek,
};

static struct drm_driver gamecube_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM,
	.load = gamecube_drm_load,
	.unload = gamecube_drm_unload,
	.open = gamecube_drm_open,
	.preclose = gamecube_drm_preclose,
	.lastclose = gamecube_drm_lastclose,

	.get_vblank_counter = gamecube_drm_get_vblank_counter,
	.enable_vblank = gamecube_drm_enable_vblank,
	.disable_vblank = gamecube_drm_disable_vblank,

#if defined(CONFIG_DEBUG_FS)
	.debugfs_init = gamecube_debugfs_init,
	.debugfs_cleanup = gamecube_debugfs_cleanup,
#endif

	.gem_free_object = gamecube_bo_free_object,
	.gem_vm_ops = &gamecube_bo_vm_ops,

	.dumb_create = gamecube_bo_dumb_create,
	.dumb_map_offset = gamecube_bo_dumb_map_offset,
	.dumb_destroy = drm_gem_dumb_destroy,

	.ioctls = gamecube_drm_ioctls,
	.num_ioctls = ARRAY_SIZE(gamecube_drm_ioctls),
	.fops = &gamecube_drm_fops,

	.name = "gamecube",
	.desc = "Nintendo GameCube/Wii graphics",
	.date = "20140306",
	.major = 0,
	.minor = 0,
	.patchlevel = 0,
};

static int gamecube_probe(struct platform_device *pdev)
{
	unsigned int width = 640;
	unsigned int height = 480;
	unsigned int stride = width * 2;
	u32 size = stride * height;
	u32 phys = 0x01698000;
	struct resource *regs;
	struct gamecube *gc;
	unsigned int i, j;
	int err;

	wii_disable_slot_led();

	gc = devm_kzalloc(&pdev->dev, sizeof(*gc), GFP_KERNEL);
	if (!gc)
		return -ENOMEM;

	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
#if 0
	gc->regs = devm_ioremap_resource(&pdev->dev, regs);
	if (IS_ERR(gc->regs))
		return PTR_ERR(gc->regs);
#else
	gc->regs = ioremap_nocache(0x0c002000, 0x100);
	if (!gc->regs)
		return -ENXIO;
#endif

#if 0
	gc->base = devm_ioremap_nocache(&pdev->dev, phys, size);
	if (!gc->base)
		return -ENXIO;
#else
	gc->base = ioremap(phys, size);
	if (!gc->base)
		return -ENXIO;
#endif

	for (j = 0; j < height; j++) {
		u16 *ptr = gc->base + (j * width * 2);

		for (i = 0; i < width; i++)
			ptr[i] = 0x0080;
	}

	if (1) {
		static const u32 regs[] = {
			0x0F060001, 0x476901AD, 0x02EA5140, 0x00030018,
			0x00020019, 0x410C410C, 0x40ED40ED, 0x00435A4E,
			0x00000000, 0x00435A4E, 0x00000000, 0x00000000,
			0x110701AE, 0x10010001, 0x00010001, 0x00010001,
			0x00000000, 0x00000000, 0x28500100, 0x1AE771F0,
			0x0DB4A574, 0x00C1188E, 0xC4C0CBE2, 0xFCECDECF,
			0x13130F08, 0x00080C0F, 0x00FF0000, 0x00000000,
			0x02800000, 0x000000FF, 0x00FF00FF, 0x00FF00FF
		};
		unsigned int i;

		for (i = 0; i < 7; i++)
			out_be32(gc->regs + i * 4, regs[i]);

		out_be32(gc->regs + 0x20, regs[0x20 / 4]); /* TFBR */
		out_be32(gc->regs + 0x28, regs[0x28 / 4]); /* BFBR */
		out_be32(gc->regs + 0x2c, regs[0x2c / 4]); /* DPV */

		for (i = 16; i < 32; i++)
			out_be32(gc->regs + i * 4, regs[i]);

		out_be32(gc->regs + 0x1c, 0x10000000 | (phys >> 5)); /* TFBL */
		phys += stride;
		out_be32(gc->regs + 0x24, 0x10000000 | (phys >> 5)); /* BFBL */
	}

	platform_set_drvdata(pdev, gc);
	wii_enable_slot_led();

	console.font = get_default_font(width, height, 8, 16);
	console.base = gc->base;
	console.width = width;
	console.height = height;
	console.stride = stride;
	console.x = 0;
	console.y = 16;
	console.initialized = true;

	register_console(&console.console);
	gamecube_console_puts(&console, "Hello, World!\n");
	//gamecube_console_puts(&console, "Test\n");
	printk("  font: %s\n", console.font->name);

	err = drm_platform_init(&gamecube_drm_driver, pdev);
	if (err < 0)
		return err;

	printk("< %s()\n", __func__);

	return 0;
}

static int gamecube_remove(struct platform_device *pdev)
{
	struct gamecube *gc = platform_get_drvdata(pdev);

	drm_put_dev(gc->drm);

	return 0;
}

static struct platform_driver gamecube_driver = {
	.driver = {
		.name = "gamecube-drm",
		.of_match_table = gamecube_match,
	},
	.probe = gamecube_probe,
	.remove = gamecube_remove,
};
module_platform_driver(gamecube_driver);

MODULE_AUTHOR("Thierry Reding <thierry.reding@gmail.com>");
MODULE_DESCRIPTION("Nintendo GameCube and Wii DRM driver");
MODULE_LICENSE("GPL v2");
