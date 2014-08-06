/*
 * Copyright (C) 2014 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

#include <linux/host1x.h>

struct sharp_panel {
	struct drm_panel base;
	struct mipi_dsi_device *primary;
	struct mipi_dsi_device *secondary;
	struct mipi_dsi_device *dsi;

	struct backlight_device *backlight;
	struct regulator *supply;

	bool prepared;
	bool enabled;

	const struct drm_display_mode *mode;
};

static inline struct sharp_panel *to_sharp_panel(struct drm_panel *panel)
{
	return container_of(panel, struct sharp_panel, base);
}

static int sharp_panel_write(struct sharp_panel *sharp, u16 offset, u8 value)
{
	u8 payload[3] = { offset >> 8, offset & 0xff, value };
	ssize_t err;

	err = mipi_dsi_generic_write(sharp->dsi, payload, sizeof(payload));
	if (err < 0)
		dev_err(&sharp->dsi->dev, "failed to write %02x to %04x: %d\n",
			value, offset, err);

	err = mipi_dsi_dcs_nop(sharp->dsi);
	if (err < 0)
		dev_err(&sharp->dsi->dev, "failed to send DCS nop: %d\n", err);

	usleep_range(10, 20);

	return err;
}

static __maybe_unused int sharp_panel_read(struct sharp_panel *sharp,
					   u16 offset, u8 *value)
{
	ssize_t err;

	cpu_to_be16s(&offset);

	err = mipi_dsi_generic_read(sharp->dsi, &offset, sizeof(offset), value,
				    sizeof(*value));
	if (err < 0)
		dev_err(&sharp->dsi->dev, "failed to read from %04x: %d\n",
			offset, err);

	return err;
}

static int sharp_panel_disable(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);

	if (!sharp->enabled)
		return 0;

	if (sharp->backlight) {
		sharp->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(sharp->backlight);
	}

	sharp->enabled = false;

	return 0;
}

static int sharp_panel_unprepare(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);
	int err;

	if (!sharp->prepared)
		return 0;

	err = mipi_dsi_dcs_set_display_off(sharp->dsi);
	if (err < 0)
		dev_err(panel->dev, "failed to set display off: %d\n", err);

	err = mipi_dsi_dcs_enter_sleep_mode(sharp->dsi);
	if (err < 0)
		dev_err(panel->dev, "failed to enter sleep mode: %d\n", err);

	msleep(120);

	regulator_disable(sharp->supply);

	sharp->prepared = false;

	return 0;
}

static int sharp_setup_symmetrical_split(struct mipi_dsi_device *left,
					 struct mipi_dsi_device *right,
					 const struct drm_display_mode *mode)
{
	int err;

	err = mipi_dsi_dcs_set_column_address(left, 0, mode->hdisplay / 2 - 1);
	if (err < 0)
		dev_err(&left->dev, "failed to set column address: %d\n", err);

	err = mipi_dsi_dcs_set_page_address(left, 0, mode->vdisplay - 1);
	if (err < 0)
		dev_err(&left->dev, "failed to set page address: %d\n", err);

	err = mipi_dsi_dcs_set_column_address(right, mode->hdisplay / 2,
					      mode->hdisplay - 1);
	if (err < 0)
		dev_err(&right->dev, "failed to set column address: %d\n", err);

	err = mipi_dsi_dcs_set_page_address(right, 0, mode->vdisplay - 1);
	if (err < 0)
		dev_err(&right->dev, "failed to set page address: %d\n", err);

	return 0;
}

static int sharp_panel_prepare(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);
	int err;

	if (sharp->prepared)
		return 0;

	err = regulator_enable(sharp->supply);
	if (err < 0)
		return err;

	msleep(10);

	err = mipi_dsi_dcs_soft_reset(sharp->dsi);
	if (err < 0)
		dev_err(panel->dev, "soft reset failed: %d\n", err);

	msleep(120);

	err = mipi_dsi_dcs_exit_sleep_mode(sharp->dsi);
	if (err < 0)
		dev_err(panel->dev, "failed to exit sleep mode: %d\n", err);

	/*
	 * The MIPI DCS specification mandates this delay only between the
	 * exit_sleep_mode and enter_sleep_mode commands, so it isn't strictly
	 * necessary here.
	 */
	//msleep(120);

	/* set left-right mode */
	err = sharp_panel_write(sharp, 0x1000, 0x2a);
	if (err < 0)
		dev_err(panel->dev, "failed to set left-right mode: %d\n", err);

	/* enable command mode */
	err = sharp_panel_write(sharp, 0x1001, 0x01);
	if (err < 0)
		dev_err(panel->dev, "failed to enable command mode: %d\n", err);

	err = mipi_dsi_dcs_set_pixel_format(sharp->dsi, MIPI_DCS_PIXEL_FMT_24BIT);
	if (err < 0)
		dev_err(panel->dev, "failed to set pixel format: %d\n", err);

	err = sharp_setup_symmetrical_split(sharp->primary, sharp->secondary,
					    sharp->mode);
	if (err < 0)
		dev_err(panel->dev, "failed to set up symmetrical split: %d\n",
			err);

	err = mipi_dsi_dcs_set_display_on(sharp->dsi);
	if (err < 0)
		dev_err(panel->dev, "failed to set display on: %d\n", err);

	sharp->prepared = true;

	return err;
}

static int sharp_panel_enable(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);

	if (sharp->enabled)
		return 0;

	if (sharp->backlight) {
		sharp->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(sharp->backlight);
	}

	sharp->enabled = true;

	return 0;
}

static const struct drm_display_mode default_mode = {
	.clock = 278000,
	.hdisplay = 2560,
	.hsync_start = 2560 + 128,
	.hsync_end = 2560 + 128 + 64,
	.htotal = 2560 + 128 + 64 + 64,
	.vdisplay = 1600,
	.vsync_start = 1600 + 4,
	.vsync_end = 1600 + 4 + 8,
	.vtotal = 1600 + 4 + 8 + 32,
	.vrefresh = 60,
};

static int sharp_panel_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		dev_err(panel->drm->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	drm_mode_probed_add(panel->connector, mode);

	panel->connector->display_info.width_mm = 217;
	panel->connector->display_info.height_mm = 136;

	return 1;
}

static const struct drm_panel_funcs sharp_panel_funcs = {
	.disable = sharp_panel_disable,
	.unprepare = sharp_panel_unprepare,
	.prepare = sharp_panel_prepare,
	.enable = sharp_panel_enable,
	.get_modes = sharp_panel_get_modes,
};

static const struct of_device_id sharp_of_match[] = {
	{ .compatible = "sharp,lq101r1sx01", },
	{ }
};
MODULE_DEVICE_TABLE(of, sharp_of_match);

static int sharp_panel_probe(struct mipi_dsi_device *dsi)
{
	struct sharp_panel *sharp;
	struct device_node *np;
	int err;

	sharp = devm_kzalloc(&dsi->dev, sizeof(*sharp), GFP_KERNEL);
	if (!sharp)
		return -ENOMEM;

	sharp->supply = devm_regulator_get(&dsi->dev, "power");
	if (IS_ERR(sharp->supply))
		return PTR_ERR(sharp->supply);

	dsi->lanes = 8;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = 0;

	sharp->mode = &default_mode;
	sharp->dsi = dsi;

	np = of_parse_phandle(dsi->dev.of_node, "backlight", 0);
	if (np) {
		sharp->backlight = of_find_backlight_by_node(np);
		of_node_put(np);

		if (!sharp->backlight)
			return -EPROBE_DEFER;
	}

	sharp->primary = dsi;

	np = of_parse_phandle(dsi->dev.of_node, "secondary", 0);
	if (np) {
		sharp->secondary = of_find_mipi_dsi_by_node(np);
		of_node_put(np);

		if (!sharp->secondary)
			return -EPROBE_DEFER;
	}

	drm_panel_init(&sharp->base);
	sharp->base.dev = &dsi->dev;
	sharp->base.funcs = &sharp_panel_funcs;

	err = drm_panel_add(&sharp->base);
	if (err < 0)
		goto free_backlight;

	mipi_dsi_set_drvdata(dsi, sharp);

	err = mipi_dsi_attach(dsi);
	if (err < 0)
		goto remove_panel;

	return 0;

remove_panel:
	drm_panel_remove(&sharp->base);
free_backlight:
	if (sharp->backlight)
		put_device(&sharp->backlight->dev);

	return err;
}

static int sharp_panel_remove(struct mipi_dsi_device *dsi)
{
	struct sharp_panel *sharp = mipi_dsi_get_drvdata(dsi);
	int err;

	sharp_panel_disable(&sharp->base);

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	drm_panel_detach(&sharp->base);
	drm_panel_remove(&sharp->base);

	if (sharp->backlight)
		put_device(&sharp->backlight->dev);

	put_device(&sharp->secondary->dev);

	return 0;
}

static void sharp_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct sharp_panel *sharp = mipi_dsi_get_drvdata(dsi);

	sharp_panel_disable(&sharp->base);
}

static struct mipi_dsi_driver sharp_panel_driver = {
	.driver = {
		.name = "panel-sharp-lq101r1sx01",
		.of_match_table = sharp_of_match,
	},
	.probe = sharp_panel_probe,
	.remove = sharp_panel_remove,
	.shutdown = sharp_panel_shutdown,
};
module_mipi_dsi_driver(sharp_panel_driver);

MODULE_AUTHOR("Thierry Reding <treding@nvidia.com>");
MODULE_DESCRIPTION("Sharp LQ101R1SX01 panel driver");
MODULE_LICENSE("GPL v2");
