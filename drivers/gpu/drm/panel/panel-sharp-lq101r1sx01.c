/*
 * Copyright (C) 2014 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define DEBUG

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
struct tegra_output;
struct clk;
struct tegra_output_ops {
	int (*enable)(struct tegra_output *output);
	int (*disable)(struct tegra_output *output);
	int (*setup_clock)(struct tegra_output *output, struct clk *clk,
			   unsigned long pclk, unsigned int *div);
	int (*check_mode)(struct tegra_output *output,
			  struct drm_display_mode *mode,
			  enum drm_mode_status *status);
	enum drm_connector_status (*detect)(struct tegra_output *output);
};

enum tegra_output_type {
	TEGRA_OUTPUT_RGB,
	TEGRA_OUTPUT_HDMI,
	TEGRA_OUTPUT_DSI,
	TEGRA_OUTPUT_EDP,
};

struct tegra_output {
	struct device_node *of_node;
	struct device *dev;

	const struct tegra_output_ops *ops;
	enum tegra_output_type type;

	struct drm_panel *panel;
	struct i2c_adapter *ddc;
	const struct edid *edid;
	unsigned int hpd_irq;
	int hpd_gpio;

	struct drm_encoder encoder;
	struct drm_connector connector;
};
struct tegra_dsi {
	struct host1x_client client;
	struct tegra_output output;
	struct device *dev;

	void __iomem *regs;

	struct reset_control *rst;
	struct clk *clk_parent;
	struct clk *clk_lp;
	struct clk *clk;

	struct drm_info_list *debugfs_files;
	struct drm_minor *minor;
	struct dentry *debugfs;

	unsigned long flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;

	struct tegra_mipi_device *mipi;
	struct mipi_dsi_host host;

	struct regulator *vdd;
	bool enabled;

	unsigned int video_fifo_depth;
	unsigned int host_fifo_depth;

	/* for ganged-mode support */
	struct tegra_dsi *slave;
	struct list_head list;
	bool ganged_mode;
};

static inline struct tegra_dsi *host_to_tegra(struct mipi_dsi_host *host)
{
	return container_of(host, struct tegra_dsi, host);
}

struct sharp_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;

	struct backlight_device *backlight;
	struct gpio_desc *enable_gpio;
	struct regulator *supply;
};

static inline struct sharp_panel *to_sharp_panel(struct drm_panel *panel)
{
	return container_of(panel, struct sharp_panel, base);
}

int mipi_dsi_set_maximum_return_packet_size(struct mipi_dsi_device *dsi,
					    u16 value)
{
	u8 tx[2] = { value & 0xff, value >> 8 };
	struct mipi_dsi_msg msg;

	memset(&msg, 0, sizeof(msg));
	msg.channel = dsi->channel;
	msg.type = MIPI_DSI_SET_MAXIMUM_RETURN_PACKET_SIZE;
	msg.tx_len = sizeof(tx);
	msg.tx_buf = tx;

	return dsi->host->ops->transfer(dsi->host, &msg);
}

static int mipi_dsi_dcs_set_pixel_format(struct mipi_dsi_device *dsi, u8 format)
{
	int err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_PIXEL_FORMAT, &format,
				 sizeof(format));
	if (err < 0)
		dev_err(&dsi->dev, "failed to set pixel format: %d\n", err);

	return err;
}

static int mipi_dsi_dcs_get_pixel_format(struct mipi_dsi_device *dsi, u8 *format)
{
	int err;

	err = mipi_dsi_dcs_read(dsi, MIPI_DCS_GET_PIXEL_FORMAT, format,
				sizeof(*format));
	if (err < 0)
		dev_err(&dsi->dev, "failed to get pixel format: %d\n", err);

	return err;
}

static int mipi_dsi_dcs_get_power_mode(struct mipi_dsi_device *dsi, u8 *mode)
{
	int err;

	err = mipi_dsi_dcs_read(dsi, MIPI_DCS_GET_POWER_MODE, mode,
				sizeof(*mode));
	if (err < 0) {
		dev_err(&dsi->dev, "failed to get power mode: %d\n", err);
		return err;
	}

	return 0;
}

static int mipi_dsi_dcs_read_ddb(struct mipi_dsi_device *dsi)
{
	u8 header[5];
	ssize_t err;

	err = mipi_dsi_set_maximum_return_packet_size(dsi, 8);
	if (err < 0) {
		dev_err(&dsi->dev, "failed to set maximum return packet size: %d\n", err);
		return err;
	}

	err = mipi_dsi_dcs_read(dsi, MIPI_DCS_READ_DDB_START, header,
				sizeof(header));
	if (err < 0) {
		dev_err(&dsi->dev, "failed to read DDB: %zd\n", err);
		return err;
	}

	dev_info(&dsi->dev, "DDB header: %*ph\n", sizeof(header), header);

	return 0;
}

static int mipi_dsi_dcs_soft_reset(struct mipi_dsi_device *dsi)
{
	int err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SOFT_RESET, NULL, 0);
	if (err < 0)
		dev_err(&dsi->dev, "DCS soft reset failed: %d\n", err);

	return err;
}

static int mipi_dsi_dcs_nop(struct mipi_dsi_device *dsi)
{
	int err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_NOP, NULL, 0);
	if (err < 0)
		dev_err(&dsi->dev, "DCS NOP failed: %d\n", err);

	return err;
}

static int mipi_dsi_dcs_set_tear_on(struct mipi_dsi_device *dsi, u8 mode)
{
	int err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_TEAR_ON, &mode, 1);
	if (err < 0)
		dev_err(&dsi->dev, "failed to set tear on: %d\n", err);

	return err;
}

static int mipi_dsi_dcs_exit_sleep_mode(struct mipi_dsi_device *dsi)
{
	int err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_EXIT_SLEEP_MODE, NULL, 0);
	if (err < 0)
		dev_err(&dsi->dev, "failed to exit sleep mode: %d\n", err);

	return err;
}

static int mipi_dsi_dcs_set_display_on(struct mipi_dsi_device *dsi)
{
	int err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_ON, NULL, 0);
	if (err < 0)
		dev_err(&dsi->dev, "failed to set display on: %d\n", err);

	return err;
}

static int mipi_dsi_dcs_set_display_off(struct mipi_dsi_device *dsi)
{
	int err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_OFF, NULL, 0);
	if (err < 0)
		dev_err(&dsi->dev, "failed to set display off: %d\n", err);

	return err;
}

static int mipi_dsi_dcs_set_column_address(struct mipi_dsi_device *dsi,
					   u16 start, u16 end)
{
	u8 payload[4] = { start >> 8, start & 0xff, end >> 8, end & 0xff };
	int err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_COLUMN_ADDRESS, payload,
				 sizeof(payload));
	if (err < 0)
		dev_err(&dsi->dev, "failed to set column address: %d\n", err);

	return err;
}

static int mipi_dsi_dcs_set_page_address(struct mipi_dsi_device *dsi,
					 u16 start, u16 end)
{
	u8 payload[4] = { start >> 8, start & 0xff, end >> 8, end & 0xff };
	int err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_PAGE_ADDRESS, payload,
				 sizeof(payload));
	if (err < 0)
		dev_err(&dsi->dev, "failed to set page address: %d\n", err);

	return err;
}

static ssize_t mipi_dsi_generic_write(struct mipi_dsi_device *dsi,
				      const void *payload, size_t size)
{
	struct mipi_dsi_msg msg;
	ssize_t err;
	u8 *tx;

	dev_dbg(&dsi->dev, "> %s(dsi=%p, payload=%p, size=%zu)\n", __func__,
		dsi, payload, size);

	memset(&msg, 0, sizeof(msg));
	msg.channel = dsi->channel;
	msg.flags = MIPI_DSI_MSG_REQ_ACK;

	if (size > 2) {
		tx = kmalloc(2 + size, GFP_KERNEL);
		if (!tx)
			return -ENOMEM;

		tx[0] = (size >> 0) & 0xff;
		tx[1] = (size >> 8) & 0xff;

		memcpy(tx + 2, payload, size);

		msg.tx_len = 2 + size;
		msg.tx_buf = tx;
	} else {
		msg.tx_buf = payload;
		msg.tx_len = size;
	}

	switch (size) {
	case 0:
		msg.type = MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM;
		break;

	case 1:
		msg.type = MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM;
		break;

	case 2:
		msg.type = MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM;
		break;

	default:
		msg.type = MIPI_DSI_GENERIC_LONG_WRITE;
		break;
	}

	err = dsi->host->ops->transfer(dsi->host, &msg);

	if (size > 2)
		kfree(tx);

	dev_dbg(&dsi->dev, "< %s() = %zd\n", __func__, err);
	return err;
}

static ssize_t mipi_dsi_generic_read(struct mipi_dsi_device *dsi,
				     const void *params, size_t num_params,
				     void *data, size_t size)
{
	struct mipi_dsi_msg msg;

	memset(&msg, 0, sizeof(msg));
	msg.channel = dsi->channel;
	msg.tx_len = num_params;
	msg.tx_buf = params;
	msg.rx_len = size;
	msg.rx_buf = data;

	switch (num_params) {
	case 0:
		msg.type = MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM;
		break;

	case 1:
		msg.type = MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM;
		break;

	case 2:
		msg.type = MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM;
		break;

	default:
		return -EINVAL;
	}

	return dsi->host->ops->transfer(dsi->host, &msg);
}

static int sharp_panel_disable(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);
	int err = 0;

	dev_dbg(panel->dev, "> %s(panel=%p)\n", __func__, panel);

	if (0) {
		err = mipi_dsi_dcs_set_display_off(sharp->dsi);
		if (err < 0)
			dev_err(panel->dev, "failed to set display off: %d\n", err);
	}

	dev_dbg(panel->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int sharp_panel_unprepare(struct drm_panel *panel)
{
	int err = 0;
	dev_dbg(panel->dev, "> %s(panel=%p)\n", __func__, panel);
	dev_dbg(panel->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int sharp_panel_write(struct sharp_panel *sharp, u16 offset, u8 value)
{
	u8 payload[3] = { offset >> 8, offset & 0xff, value };
	ssize_t err;

	err = mipi_dsi_generic_write(sharp->dsi, payload, sizeof(payload));
	if (err < 0)
		dev_err(&sharp->dsi->dev, "failed to write %02x to %04x: %d\n", value, offset, err);

	err = mipi_dsi_dcs_nop(sharp->dsi);
	if (err < 0)
		dev_err(&sharp->dsi->dev, "failed to send DCS nop: %d\n", err);

	usleep_range(10, 20);

	return err;
}

static int sharp_panel_read(struct sharp_panel *sharp, u16 offset, u8 *value)
{
	ssize_t err;

	cpu_to_be16s(&offset);

	err = mipi_dsi_generic_read(sharp->dsi, &offset, sizeof(offset), value,
				    sizeof(*value));
	if (err < 0)
		dev_err(&sharp->dsi->dev, "failed to read from %04x: %d\n", offset, err);

	return err;
}

static int sharp_panel_prepare(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);
	struct tegra_dsi *primary = host_to_tegra(sharp->dsi->host);
	struct tegra_dsi *secondary = primary->slave;
	u8 mode = 0, format = 0;
	int err;

	dev_dbg(panel->dev, "> %s(panel=%p)\n", __func__, panel);

	err = regulator_enable(sharp->supply);
	if (err < 0)
		return err;

	usleep_range(10000, 20000);

	err = regulator_disable(sharp->supply);
	if (err < 0)
		return err;

	usleep_range(10000, 20000);

	err = regulator_enable(sharp->supply);
	if (err < 0)
		return err;

	usleep_range(20000, 40000);

	/*
	gpiod_set_value(sharp->enable_gpio, 1);

	usleep_range(20000, 40000);
	*/

	err = mipi_dsi_dcs_soft_reset(sharp->dsi);
	if (err < 0)
		dev_err(panel->dev, "soft reset failed: %d\n", err);

	usleep_range(20000, 40000);

	/* set left-right mode */
	err = sharp_panel_write(sharp, 0x1000, 0x2a);
	if (err < 0)
		dev_err(panel->dev, "failed to set left-right mode: %d\n", err);

	usleep_range(20000, 40000);

	/* enable command mode */
	err = sharp_panel_write(sharp, 0x1001, 0x01);
	if (err < 0)
		dev_err(panel->dev, "failed to enable command mode: %d\n", err);

	usleep_range(20000, 40000);

	if (1) {
		u8 value;

		err = mipi_dsi_set_maximum_return_packet_size(sharp->dsi, 8);
		if (err < 0)
			dev_err(panel->dev, "failed to set maximum return packet size: %d\n", err);

		err = sharp_panel_read(sharp, 0x1000, &value);
		if (err < 0)
			dev_err(panel->dev, "failed to read 0x1000: %d\n", err);
		else
			dev_dbg(panel->dev, "0x1000: %02x\n", value);

		err = sharp_panel_read(sharp, 0x1001, &value);
		if (err < 0)
			dev_err(panel->dev, "failed to read 0x1001: %d\n", err);
		else
			dev_dbg(panel->dev, "0x1001: %02x\n", value);

		err = sharp_panel_read(sharp, 0x001f, &value);
		if (err < 0)
			dev_err(panel->dev, "failed to read 0x001f: %d\n", err);
		else
			dev_dbg(panel->dev, "0x001f: %02x\n", value);
	}

	/*
	err = mipi_dsi_dcs_get_power_mode(sharp->dsi, &mode);
	if (err < 0)
		dev_err(panel->dev, "failed to get power mode: %d\n", err);
	else
		dev_dbg(panel->dev, "power mode: %02x\n", mode);
	*/

	err = mipi_dsi_dcs_set_pixel_format(sharp->dsi, 0x07);
	if (err < 0)
		dev_err(panel->dev, "failed to set pixel format: %d\n", err);

	usleep_range(20000, 40000);

	/*
	err = mipi_dsi_dcs_get_pixel_format(sharp->dsi, &format);
	if (err < 0)
		dev_err(panel->dev, "failed to get pixel format: %d\n", err);
	else
		dev_dbg(panel->dev, "pixel format: %02x\n", format);
	*/

	/**/
	err = mipi_dsi_dcs_set_column_address(sharp->dsi, 0, 1279);
	if (err < 0)
		dev_err(panel->dev, "failed to set column address: %d\n", err);

	usleep_range(20000, 40000);

	err = mipi_dsi_dcs_set_page_address(sharp->dsi, 0, 1599);
	if (err < 0)
		dev_err(panel->dev, "failed to set page address: %d\n", err);

	usleep_range(20000, 40000);
	/**/

	if (1) {
		const u8 set_column_address[] = { 0x05, 0x00, 0x2a, 0x05, 0x00, 0x09, 0xff };
		const u8 set_page_address[] = { 0x05, 0x00, 0x2b, 0x00, 0x00, 0x06, 0x3f };
		struct mipi_dsi_msg msg;
		ssize_t err;

		memset(&msg, 0, sizeof(msg));
		msg.channel = 0;
		msg.type = MIPI_DSI_DCS_LONG_WRITE;
		msg.flags = MIPI_DSI_MSG_REQ_ACK;
		msg.tx_len = sizeof(set_column_address);
		msg.tx_buf = set_column_address;

		err = secondary->host.ops->transfer(&secondary->host, &msg);

		usleep_range(20000, 40000);

		memset(&msg, 0, sizeof(msg));
		msg.channel = 0;
		msg.type = MIPI_DSI_DCS_LONG_WRITE;
		msg.flags = MIPI_DSI_MSG_REQ_ACK;
		msg.tx_len = sizeof(set_page_address);
		msg.tx_buf = set_page_address;

		err = secondary->host.ops->transfer(&secondary->host, &msg);

		usleep_range(20000, 40000);
	}

	err = mipi_dsi_dcs_exit_sleep_mode(sharp->dsi);
	if (err < 0)
		dev_err(panel->dev, "failed to exit sleep mode: %d\n", err);

	usleep_range(20000, 40000);

	err = mipi_dsi_dcs_set_display_on(sharp->dsi);
	if (err < 0)
		dev_err(panel->dev, "failed to set display on: %d\n", err);

	usleep_range(20000, 40000);

	if (0) {
		unsigned int i, j, chunks = 4;
		size_t size;
		u8 command;
		u32 *data;

		size = 2560 * 3 / chunks;

		data = kmalloc(size, GFP_KERNEL);
		if (!data)
			return -ENOMEM;

		for (i = 0; i < size / (4 * 3); i++) {
			data[i * 3 + 0] = 0xff0000ff;
			data[i * 3 + 1] = 0x00ff0000;
			data[i * 3 + 2] = 0x0000ff00;
		}

		command = MIPI_DCS_WRITE_MEMORY_START;

		for (j = 0; j < 64 /*1600*/; j++) {
			for (i = 0; i < chunks; i++) {
				err = mipi_dsi_dcs_write(sharp->dsi, command,
							 data, size);
				if (err < 0)
					dev_err(panel->dev, "failed to write data: %d\n", err);

				command = MIPI_DCS_WRITE_MEMORY_CONTINUE;
			}
		}

		kfree(data);
	}

	dev_dbg(panel->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int sharp_panel_enable(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);
	int err = 0;

	dev_dbg(panel->dev, "> %s(panel=%p)\n", __func__, panel);

	if (0) {
		err = mipi_dsi_dcs_set_display_on(sharp->dsi);
		if (err < 0)
			dev_err(panel->dev, "failed to set display on: %d\n", err);
	}

	dev_dbg(panel->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static const struct drm_display_mode sharp_panel_mode = {
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
	int err = 0;

	dev_dbg(panel->dev, "> %s(panel=%p)\n", __func__, panel);

	mode = drm_mode_duplicate(panel->drm, &sharp_panel_mode);
	if (!mode) {
		dev_err(panel->drm->dev, "failed to add mode %ux%ux@%u\n",
			sharp_panel_mode.hdisplay, sharp_panel_mode.vdisplay,
			sharp_panel_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	drm_mode_probed_add(panel->connector, mode);
	err = 1;

	panel->connector->display_info.width_mm = 217;
	panel->connector->display_info.height_mm = 136;

	dev_dbg(panel->dev, "< %s() = %d\n", __func__, err);
	return err;
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
	struct sharp_panel *panel;
	struct device_node *np;
	u32 value;
	int err;

	panel = devm_kzalloc(&dsi->dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	panel->supply = devm_regulator_get(&dsi->dev, "power");
	if (IS_ERR(panel->supply))
		return PTR_ERR(panel->supply);

	panel->enable_gpio = devm_gpiod_get_optional(&dsi->dev, "enable");
	if (IS_ERR(panel->enable_gpio))
		return PTR_ERR(panel->enable_gpio);

	if (panel->enable_gpio) {
		err = gpiod_direction_output(panel->enable_gpio, 0);
		if (err < 0) {
			dev_err(&dsi->dev, "failed to setup GPIO: %d\n", err);
			return err;
		}
	}

	dsi->lanes = 8;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = 0;

	panel->dsi = dsi;

	np = of_parse_phandle(dsi->dev.of_node, "backlight", 0);
	if (np) {
		panel->backlight = of_find_backlight_by_node(np);
		of_node_put(np);

		if (!panel->backlight)
			return -EPROBE_DEFER;
	}

	drm_panel_init(&panel->base);
	panel->base.dev = &dsi->dev;
	panel->base.funcs = &sharp_panel_funcs;

	err = drm_panel_add(&panel->base);
	if (err < 0)
		goto free_backlight;

	mipi_dsi_set_drvdata(dsi, panel);

	return mipi_dsi_attach(dsi);

free_backlight:
	if (panel->backlight)
		put_device(&panel->backlight->dev);

	return err;
}

static int sharp_panel_remove(struct mipi_dsi_device *dsi)
{
	struct sharp_panel *panel = mipi_dsi_get_drvdata(dsi);
	int err;

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	drm_panel_detach(&panel->base);
	drm_panel_remove(&panel->base);

	if (panel->backlight)
		put_device(&panel->backlight->dev);

	return 0;
}

static void sharp_panel_shutdown(struct mipi_dsi_device *dsi)
{
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
