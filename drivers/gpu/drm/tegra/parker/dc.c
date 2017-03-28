#include <linux/clk.h>
#include <linux/clk-provider.h> /* XXX */
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>

#include "../drm.h"
#include "dc.h"

struct tegra_dc_state {
	struct drm_crtc_state base;

	struct clk *parent;
	unsigned long pclk;
	unsigned int bpc;

	u32 planes;
};

static inline struct tegra_dc_state *
to_tegra_dc_state(struct drm_crtc_state *state)
{
	return container_of(state, struct tegra_dc_state, base);
}

struct tegra_window {
	struct drm_plane base;
	unsigned int offset;
	unsigned int index;
	void __iomem *regs;
};

static inline struct tegra_window *to_tegra_window(struct drm_plane *plane)
{
	return container_of(plane, struct tegra_window, base);
}

struct tegra_window_state {
	struct drm_plane_state base;

	u32 format;
};

static inline struct tegra_window_state *
to_tegra_window_state(struct drm_plane_state *state)
{
	return container_of(state, struct tegra_window_state, base);
}

struct tegra_cursor {
	struct drm_plane base;
};

static inline struct tegra_cursor *to_tegra_cursor(struct drm_plane *plane)
{
	return container_of(plane, struct tegra_cursor, base);
}

struct tegra_cursor_state {
	struct drm_plane_state base;
};

static inline u32 tegra_dc_readl(struct tegra_dc *dc, unsigned int offset)
{
	u32 value = readl(dc->regs + offset);

	dev_dbg(dc->dev, "%08x > %08x\n", offset, value);

	return value;
}

static inline void tegra_dc_writel(struct tegra_dc *dc, u32 value,
				   unsigned int offset)
{
	dev_dbg(dc->dev, "%08x < %08x\n", offset, value);
	writel(value, dc->regs + offset);
}

static void tegra_dc_update(struct tegra_dc *dc)
{
	writel(GENERAL_UPDATE, dc->regs + DC_CMD_STATE_CONTROL);
}

void tegra186_dc_commit(struct tegra_dc *dc)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(250);
	u32 value;

	writel(GENERAL_ACTREQ, dc->regs + DC_CMD_STATE_CONTROL);

	while (time_before(jiffies, timeout)) {
		value = readl(dc->regs + DC_CMD_STATE_CONTROL);
		if ((value & GENERAL_ACTREQ) == 0)
			return;
	}

	dev_warn(dc->dev, "timed out waiting for general activation request\n");
}

static u32 tegra_dc_readl_active(struct tegra_dc *dc, unsigned int offset)
{
	unsigned long flags;
	u32 value;

	spin_lock_irqsave(&dc->lock, flags);

	writel(READ_MUX, dc->regs + DC_CMD_STATE_ACCESS);
	value = readl(dc->regs + offset);
	writel(0, dc->regs + DC_CMD_STATE_ACCESS);

	spin_unlock_irqrestore(&dc->lock, flags);

	return value;
}

static int tegra_dc_start(struct tegra_dc *dc)
{
	u32 value;

	value = readl(dc->regs + DC_CMD_DISPLAY_COMMAND);
	value &= ~DISPLAY_CTRL_MODE_MASK;
	value |= DISPLAY_CTRL_MODE_C_DISPLAY;
	writel(value, dc->regs + DC_CMD_DISPLAY_COMMAND);

	tegra186_dc_commit(dc);

	return 0;
}

static void tegra_dc_stop(struct tegra_dc *dc)
{
	u32 value;

	value = readl(dc->regs + DC_CMD_DISPLAY_COMMAND);
	value &= ~DISPLAY_CTRL_MODE_MASK;
	writel(value, dc->regs + DC_CMD_DISPLAY_COMMAND);

	tegra186_dc_commit(dc);
}

static bool tegra_dc_idle(struct tegra_dc *dc)
{
	u32 value;

	value = tegra_dc_readl_active(dc, DC_CMD_DISPLAY_COMMAND);

	return (value & DISPLAY_CTRL_MODE_MASK) == 0;
}

static int tegra_dc_wait_idle(struct tegra_dc *dc, unsigned long timeout)
{
	timeout = jiffies + msecs_to_jiffies(timeout);

	while (time_before(jiffies, timeout)) {
		if (tegra_dc_idle(dc))
			return 0;

		usleep_range(1000, 2000);
	}

	dev_dbg(dc->dev, "timeout waiting for CRTC to become idle\n");
	return -ETIMEDOUT;
}

int tegra_dc_state_setup(struct drm_crtc *crtc,
			 struct drm_crtc_state *crtc_state,
			 unsigned int bpc, struct clk *parent,
			 unsigned long pclk)
{
	struct tegra_dc_state *state = to_tegra_dc_state(crtc_state);
	struct tegra_dc *dc = to_tegra_dc(crtc);
	int err = 0;

	dev_dbg(dc->dev, "> %s(crtc=%p, crtc_state=%p, bpc=%u, parent=%p, pclk=%lu)\n",
		__func__, crtc, crtc_state, bpc, parent, pclk);

	if (!clk_has_parent(dc->clk, parent)) {
		err = -EINVAL;
		goto out;
	}

	switch (bpc) {
	case 6:
	case 8:
		state->bpc = bpc;
		break;

	default:
		DRM_DEBUG_KMS("%u bits-per-color not supported\n", bpc);
		state->bpc = 8;
		break;
	}

	state->parent = parent;
	state->pclk = pclk;

out:
	dev_dbg(dc->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static void tegra_dc_destroy(struct drm_crtc *crtc)
{
	struct tegra_dc *dc = to_tegra_dc(crtc);

	dev_dbg(dc->dev, "> %s(crtc=%p)\n", __func__, crtc);

	drm_crtc_cleanup(crtc);

	dev_dbg(dc->dev, "< %s()\n", __func__);
}

static void tegra_dc_reset(struct drm_crtc *crtc)
{
	struct tegra_dc *dc = to_tegra_dc(crtc);
	struct tegra_dc_state *state;

	dev_dbg(dc->dev, "> %s(crtc=%p)\n", __func__, crtc);

	if (crtc->state)
		__drm_atomic_helper_crtc_destroy_state(crtc->state);

	kfree(crtc->state);
	crtc->state = NULL;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state) {
		crtc->state = &state->base;
		crtc->state->crtc = crtc;
	}

	drm_crtc_vblank_reset(crtc);

	dev_dbg(dc->dev, "< %s()\n", __func__);
}

static struct drm_crtc_state *
tegra_dc_atomic_duplicate_state(struct drm_crtc *crtc)
{
	struct tegra_dc_state *state = to_tegra_dc_state(crtc->state);
	struct tegra_dc *dc = to_tegra_dc(crtc);
	struct tegra_dc_state *copy;

	dev_dbg(dc->dev, "> %s(crtc=%p)\n", __func__, crtc);

	copy = kmemdup(state, sizeof(*state), GFP_KERNEL);
	if (!copy)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &copy->base);

	dev_dbg(dc->dev, "< %s() = %p\n", __func__, &copy->base);
	return &copy->base;
}

static void tegra_dc_atomic_destroy_state(struct drm_crtc *crtc,
					    struct drm_crtc_state *state)
{
	struct tegra_dc *dc = to_tegra_dc(crtc);

	dev_dbg(dc->dev, "> %s(crtc=%p, state=%p)\n", __func__, crtc, state);

	__drm_atomic_helper_crtc_destroy_state(state);
	kfree(state);

	dev_dbg(dc->dev, "< %s()\n", __func__);
}

#ifdef CONFIG_DEBUG_FS
static const struct tegra_dc_register {
	unsigned int offset;
	const char *name;
} tegra186_dc_regs[] = {
#define TEGRA_CRTC_REGISTER(reg) { .offset = reg, .name = #reg }
	TEGRA_CRTC_REGISTER(DC_CMD_DISPLAY_COMMAND),
	TEGRA_CRTC_REGISTER(DC_CMD_REG_PFE_HEAD_DEBUG),
	TEGRA_CRTC_REGISTER(DC_CMD_INT_STATUS),
	TEGRA_CRTC_REGISTER(DC_CMD_INT_MASK),
	TEGRA_CRTC_REGISTER(DC_CMD_INT_ENABLE),
	TEGRA_CRTC_REGISTER(DC_CMD_STATE_ACCESS),
	TEGRA_CRTC_REGISTER(DC_CMD_STATE_CONTROL),
	TEGRA_CRTC_REGISTER(DC_DISP_DISP_SIGNAL_OPTIONS),
	TEGRA_CRTC_REGISTER(DC_DISP_DISP_WIN_OPTIONS),
	TEGRA_CRTC_REGISTER(DC_DISP_CORE_SOR_SET_CONTROL),
	TEGRA_CRTC_REGISTER(DC_DISP_CORE_SOR1_SET_CONTROL),
	TEGRA_CRTC_REGISTER(DC_DISP_CORE_DSI_SET_CONTROL),
	TEGRA_CRTC_REGISTER(DC_DISP_SYNC_WIDTH),
	TEGRA_CRTC_REGISTER(DC_DISP_BACK_PORCH),
	TEGRA_CRTC_REGISTER(DC_DISP_DISP_ACTIVE),
	TEGRA_CRTC_REGISTER(DC_DISP_FRONT_PORCH),
	TEGRA_CRTC_REGISTER(DC_DISP_DISP_COLOR_CONTROL),
};

static int tegra_dc_regs_show(struct seq_file *s, void *data)
{
	struct tegra_dc *dc = s->private;
	unsigned int i;
	int width = 0;

	for (i = 0; i < ARRAY_SIZE(tegra186_dc_regs); i++) {
		int len = strlen(tegra186_dc_regs[i].name);

		if (len > width)
			width = len;
	}

	for (i = 0; i < ARRAY_SIZE(tegra186_dc_regs); i++) {
		const struct tegra_dc_register *reg = &tegra186_dc_regs[i];
		u32 value = readl(dc->regs + reg->offset);

		seq_printf(s, "%#06x %-*s %#010x\n", reg->offset, width,
			   reg->name, value);
	}

	return 0;
}

static int tegra_dc_regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, tegra_dc_regs_show, inode->i_private);
}

static const struct file_operations tegra_dc_regs_ops = {
	.open = tegra_dc_regs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static int tegra_dc_late_register(struct drm_crtc *crtc)
{
	struct tegra_dc *dc = to_tegra_dc(crtc);
	struct dentry *root = crtc->debugfs_entry;
	int err = 0;

	dev_dbg(dc->dev, "> %s(crtc=%p)\n", __func__, crtc);

#ifdef CONFIG_DEBUG_FS
	dc->debugfs.regs = debugfs_create_file("regs", S_IRUGO, root, dc,
					       &tegra_dc_regs_ops);
	if (!dc->debugfs.regs)
		dev_err(dc->dev, "failed to create debugfs \"regs\" file\n");
#endif

	dev_dbg(dc->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static void tegra_dc_early_unregister(struct drm_crtc *crtc)
{
	struct tegra_dc *dc = to_tegra_dc(crtc);

#ifdef CONFIG_DEBUG_FS
	debugfs_remove(dc->debugfs.regs);
#endif
}

static u32 tegra_dc_get_vblank_counter(struct drm_crtc *crtc)
{
	struct tegra_dc *dc = to_tegra_dc(crtc);

	/*
	if (dc->syncpt)
		return host1x_syncpt_read(dc->syncpt);
	*/

	/* fallback to software emulated VBLANK counter */
	return drm_crtc_vblank_count(&dc->base);
}

static int tegra_dc_enable_vblank(struct drm_crtc *crtc)
{
	struct tegra_dc *dc = to_tegra_dc(crtc);
	u32 value;

	dev_dbg(dc->dev, "> %s(crtc=%p)\n", __func__, crtc);

	value = readl(dc->regs + DC_CMD_INT_MASK);
	value |= VBLANK_INT;
	writel(value, dc->regs + DC_CMD_INT_MASK);

	dev_dbg(dc->dev, "< %s()\n", __func__);

	return 0;
}

static void tegra_dc_disable_vblank(struct drm_crtc *crtc)
{
	struct tegra_dc *dc = to_tegra_dc(crtc);
	u32 value;

	dev_dbg(dc->dev, "> %s(crtc=%p)\n", __func__, crtc);

	value = readl(dc->regs + DC_CMD_INT_MASK);
	value &= ~VBLANK_INT;
	writel(value, dc->regs + DC_CMD_INT_MASK);

	dev_dbg(dc->dev, "< %s()\n", __func__);
}

static const struct drm_crtc_funcs tegra_dc_funcs = {
	.page_flip = drm_atomic_helper_page_flip,
	.set_config = drm_atomic_helper_set_config,
	.destroy = tegra_dc_destroy,
	.reset = tegra_dc_reset,
	.atomic_duplicate_state = tegra_dc_atomic_duplicate_state,
	.atomic_destroy_state = tegra_dc_atomic_destroy_state,
	.late_register = tegra_dc_late_register,
	.early_unregister = tegra_dc_early_unregister,
	.get_vblank_counter = tegra_dc_get_vblank_counter,
	.enable_vblank = tegra_dc_enable_vblank,
	.disable_vblank = tegra_dc_disable_vblank,
};

static void tegra_dc_disable(struct drm_crtc *crtc,
			     struct drm_crtc_state *old_crtc_state)
{
	struct tegra_dc *dc = to_tegra_dc(crtc);

	dev_dbg(dc->dev, "> %s(crtc=%p)\n", __func__, crtc);

	drm_atomic_helper_disable_planes_on_crtc(old_crtc_state, true);

	if (!tegra_dc_idle(dc)) {
		tegra_dc_stop(dc);

		/*
		 * Ignore the return value, there isn't anything useful to do
		 * in case this fails.
		 */
		tegra_dc_wait_idle(dc, 100);
	}

	drm_crtc_vblank_off(crtc);
	pm_runtime_put_sync(dc->dev);

	dev_dbg(dc->dev, "< %s()\n", __func__);
}

static void tegra_dc_enable(struct drm_crtc *crtc)
{
	struct tegra_dc_state *state = to_tegra_dc_state(crtc->state);
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	struct tegra_dc *dc = to_tegra_dc(crtc);
	u32 value;
	int err;

	dev_dbg(dc->dev, "> %s(crtc=%p)\n", __func__, crtc);

	pm_runtime_get_sync(dc->dev);

	value = 0;
	writel(value, dc->regs + DC_CMD_INT_TYPE);

	value = REG_TMOUT_INT | VBLANK_INT | FRAME_END_INT;
	writel(value, dc->regs + DC_CMD_INT_POLARITY);

	value = REG_TMOUT_INT | VBLANK_INT | FRAME_END_INT;
	writel(value, dc->regs + DC_CMD_INT_ENABLE);

	value = REG_TMOUT_INT | VBLANK_INT | FRAME_END_INT;
	writel(value, dc->regs + DC_CMD_INT_MASK);

	tegra_dc_update(dc);

	writel(0x00000000, dc->regs + DC_DISP_BLEND_BACKGROUND_COLOR);

	DRM_DEBUG_KMS("module clock: %lu Hz (%s)\n", clk_get_rate(dc->clk),
		      __clk_get_name(dc->clk));

	err = clk_set_parent(dc->clk, state->parent);
	if (err < 0)
		dev_err(dc->dev, "failed to set parent clock: %d\n", err);

	DRM_DEBUG_KMS("pixel clock: %lu Hz, parent: %s\n", state->pclk,
		      __clk_get_name(state->parent));

	err = clk_set_rate(state->parent, state->pclk);
	if (err < 0)
		dev_err(dc->dev, "failed to set clock rate to %lu Hz: %d\n",
			state->pclk, err);

	err = clk_set_rate(dc->clk, state->pclk);
	if (err < 0)
		dev_err(dc->dev, "failed to set clock rate to %lu Hz: %d\n",
			state->pclk, err);

	value = ((mode->vsync_end - mode->vsync_start) << 16) |
		((mode->hsync_end - mode->hsync_start) <<  0);
	writel(value, dc->regs + DC_DISP_SYNC_WIDTH);

	value = ((mode->vtotal - mode->vsync_end) << 16) |
		((mode->htotal - mode->hsync_end) <<  0);
	writel(value, dc->regs + DC_DISP_BACK_PORCH);

	value = ((mode->vsync_start - mode->vdisplay) << 16) |
		((mode->hsync_start - mode->hdisplay) <<  0);
	writel(value, dc->regs + DC_DISP_FRONT_PORCH);

	value = (mode->vdisplay << 16) | mode->hdisplay;
	writel(value, dc->regs + DC_DISP_DISP_ACTIVE);

	err = tegra_dc_start(dc);
	if (err < 0)
		dev_err(dc->dev, "failed to start display controller: %d\n", err);

	drm_crtc_vblank_on(crtc);

	dev_dbg(dc->dev, "< %s()\n", __func__);
}

static int tegra_dc_atomic_check(struct drm_crtc *crtc,
				 struct drm_crtc_state *state)
{
	struct tegra_dc *dc = to_tegra_dc(crtc);
	int err = 0;

	dev_dbg(dc->dev, "> %s(crtc=%p, state=%p)\n", __func__, crtc, state);

	dev_dbg(dc->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static void tegra_dc_atomic_begin(struct drm_crtc *crtc,
				  struct drm_crtc_state *old_crtc_state)
{
	struct tegra_dc *dc = to_tegra_dc(crtc);

	dev_dbg(dc->dev, "> %s(crtc=%p, old_crtc_state=%p)\n", __func__,
		crtc, old_crtc_state);

	if (crtc->state->event) {
		crtc->state->event->pipe = drm_crtc_index(crtc);

		WARN_ON(drm_crtc_vblank_get(crtc) != 0);

		dc->event = crtc->state->event;
		crtc->state->event = NULL;
	}

	dev_dbg(dc->dev, "< %s()\n", __func__);
}

static void tegra_dc_atomic_flush(struct drm_crtc *crtc,
				  struct drm_crtc_state *old_crtc_state)
{
	struct tegra_dc_state *state = to_tegra_dc_state(crtc->state);
	struct tegra_dc *dc = to_tegra_dc(crtc);

	dev_dbg(dc->dev, "> %s(crtc=%p, old_crtc_state=%p)\n", __func__,
		crtc, old_crtc_state);

	tegra_dc_writel(dc, state->planes << 8, DC_CMD_STATE_CONTROL);
	tegra_dc_writel(dc, state->planes, DC_CMD_STATE_CONTROL);

	dev_dbg(dc->dev, "< %s()\n", __func__);
}

static const struct drm_crtc_helper_funcs tegra_dc_helper_funcs = {
	.atomic_disable = tegra_dc_disable,
	.enable = tegra_dc_enable,
	.atomic_check = tegra_dc_atomic_check,
	.atomic_begin = tegra_dc_atomic_begin,
	.atomic_flush = tegra_dc_atomic_flush,
};

static const u32 tegra_cursor_formats[] = {
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGB565,
};

static void tegra_cursor_destroy(struct drm_plane *plane)
{
	struct tegra_cursor *cursor = to_tegra_cursor(plane);

	pr_debug("> %s(plane=%p)\n", __func__, plane);

	drm_plane_cleanup(plane);
	kfree(cursor);

	pr_debug("< %s()\n", __func__);
}

static void tegra_cursor_reset(struct drm_plane *plane)
{
	struct tegra_cursor_state *state;

	pr_debug("> %s(plane=%p)\n", __func__, plane);

	if (plane->state)
		__drm_atomic_helper_plane_destroy_state(plane->state);

	kfree(plane->state);
	plane->state = NULL;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state) {
		plane->state = &state->base;
		plane->state->plane = plane;
	}

	pr_debug("< %s()\n", __func__);
}

static struct drm_plane_state *
tegra_cursor_atomic_duplicate_state(struct drm_plane *plane)
{
	struct tegra_cursor_state *copy;

	pr_debug("> %s(plane=%p)\n", __func__, plane);

	copy = kmalloc(sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &copy->base);

	pr_debug("< %s() = %p\n", __func__, &copy->base);
	return &copy->base;
}

static void tegra_cursor_atomic_destroy_state(struct drm_plane *plane,
					      struct drm_plane_state *state)
{
	pr_debug("> %s(plane=%p, state=%p)\n", __func__, plane, state);
	__drm_atomic_helper_plane_destroy_state(state);
	kfree(state);
	pr_debug("< %s()\n", __func__);
}

static const struct drm_plane_funcs tegra_cursor_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = tegra_cursor_destroy,
	.reset = tegra_cursor_reset,
	.atomic_duplicate_state = tegra_cursor_atomic_duplicate_state,
	.atomic_destroy_state = tegra_cursor_atomic_destroy_state,
};

static int tegra_cursor_atomic_check(struct drm_plane *plane,
				     struct drm_plane_state *state)
{
	pr_debug("> %s(plane=%p, state=%p)\n", __func__, plane, state);
	pr_debug("< %s()\n", __func__);

	return 0;
}

static void tegra_cursor_atomic_update(struct drm_plane *plane,
				       struct drm_plane_state *old_state)
{
	pr_debug("> %s(plane=%p, old_state=%p)\n", __func__, plane, old_state);
	pr_debug("< %s()\n", __func__);
}

static void tegra_cursor_atomic_disable(struct drm_plane *plane,
					struct drm_plane_state *old_state)
{
	pr_debug("> %s(plane=%p, old_state=%p)\n", __func__, plane, old_state);
	pr_debug("< %s()\n", __func__);
}

static const struct drm_plane_helper_funcs tegra_cursor_helper_funcs = {
	.atomic_check = tegra_cursor_atomic_check,
	.atomic_update = tegra_cursor_atomic_update,
	.atomic_disable = tegra_cursor_atomic_disable,
};

static struct drm_plane *tegra_cursor_create(struct drm_device *drm,
					     struct drm_crtc *crtc)
{
	struct tegra_dc *dc = to_tegra_dc(crtc);
	unsigned long possible_crtcs;
	struct tegra_cursor *cursor;
	struct drm_plane *plane;
	int err;

	dev_dbg(dc->dev, "> %s(drm=%p, crtc=%p)\n", __func__, drm, crtc);

	cursor = kzalloc(sizeof(*cursor), GFP_KERNEL);
	if (!cursor) {
		plane = ERR_PTR(-ENOMEM);
		goto out;
	}

	possible_crtcs = drm_crtc_mask(crtc);

	err = drm_universal_plane_init(drm, &cursor->base, possible_crtcs,
				       &tegra_cursor_funcs,
				       tegra_cursor_formats,
				       ARRAY_SIZE(tegra_cursor_formats),
				       DRM_PLANE_TYPE_CURSOR, NULL);
	if (err < 0) {
		plane = ERR_PTR(err);
		kfree(cursor);
		goto out;
	}

	plane = &cursor->base;

	drm_plane_helper_add(&cursor->base, &tegra_cursor_helper_funcs);

out:
	dev_dbg(dc->dev, "< %s() = %p\n", __func__, plane);

	return plane;
}

static const u32 tegra_window_formats[] = {
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGB565,
};

static void tegra_window_destroy(struct drm_plane *plane)
{
	struct tegra_window *window = to_tegra_window(plane);

	pr_debug("> %s(plane=%p)\n", __func__, plane);

	drm_plane_cleanup(plane);
	kfree(window);

	pr_debug("< %s()\n", __func__);
}

static void tegra_window_reset(struct drm_plane *plane)
{
	struct tegra_window_state *state;

	pr_debug("> %s(plane=%p)\n", __func__, plane);

	if (plane->state)
		__drm_atomic_helper_plane_destroy_state(plane->state);

	kfree(plane->state);
	plane->state = NULL;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state) {
		plane->state = &state->base;
		plane->state->plane = plane;
	}

	pr_debug("< %s()\n", __func__);
}

static struct drm_plane_state *
tegra_window_atomic_duplicate_state(struct drm_plane *plane)
{
	struct tegra_window_state *state = to_tegra_window_state(plane->state);
	struct tegra_window_state *copy;

	pr_debug("> %s(plane=%p)\n", __func__, plane);

	copy = kmalloc(sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &copy->base);
	copy->format = state->format;

	pr_debug("< %s() = %p\n", __func__, &copy->base);
	return &copy->base;
}

static void tegra_window_atomic_destroy_state(struct drm_plane *plane,
					      struct drm_plane_state *state)
{
	pr_debug("> %s(plane=%p, state=%p)\n", __func__, plane, state);
	__drm_atomic_helper_plane_destroy_state(state);
	kfree(state);
	pr_debug("< %s()\n", __func__);
}

static const struct drm_plane_funcs tegra_window_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = tegra_window_destroy,
	.reset = tegra_window_reset,
	.atomic_duplicate_state = tegra_window_atomic_duplicate_state,
	.atomic_destroy_state = tegra_window_atomic_destroy_state,
};

static int tegra_window_state_remove(struct tegra_window *window,
				     struct drm_plane_state *state)
{
	struct drm_plane_state *old_state = window->base.state;
	struct drm_crtc_state *crtc_state;
	struct tegra_dc_state *tegra;

	pr_debug("> %s(window=%p, state=%p)\n", __func__, window, state);

	crtc_state = drm_atomic_get_crtc_state(state->state, old_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	tegra = to_tegra_dc_state(crtc_state);
	tegra->planes |= WIN_ACT_REQ(window->index);

	pr_debug("  planes: %x\n", tegra->planes);
	pr_debug("< %s()\n", __func__);
	return 0;
}

static int tegra_window_state_add(struct tegra_window *window,
				  struct drm_plane_state *state)
{
	struct drm_crtc_state *crtc_state;
	struct tegra_dc_state *tegra;

	pr_debug("> %s(window=%p, state=%p)\n", __func__, window, state);

	/* propagate errors from allocation or locking failures */
	crtc_state = drm_atomic_get_crtc_state(state->state, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	tegra = to_tegra_dc_state(crtc_state);
	tegra->planes |= WIN_ACT_REQ(window->index);

	pr_debug("  planes: %x\n", tegra->planes);
	pr_debug("< %s()\n", __func__);
	return 0;
}

static int tegra_window_get_format(u32 fourcc, u32 *format)
{
	switch (fourcc) {
	case DRM_FORMAT_XBGR8888:
		*format = COLOR_DEPTH_R8G8B8A8;
		break;

	case DRM_FORMAT_XRGB8888:
		*format = COLOR_DEPTH_B8G8R8A8;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int tegra_window_atomic_check(struct drm_plane *plane,
				     struct drm_plane_state *state)
{
	struct tegra_window_state *window_state = to_tegra_window_state(state);
	struct tegra_window *window = to_tegra_window(plane);
	int err = 0;

	pr_debug("> %s(plane=%p, state=%p)\n", __func__, plane, state);

	/* no need for further checks if the plane is being disabled */
	if (!state->crtc) {
		pr_debug("  plane is being disabled: %p\n", plane->state->crtc);

		if (0 && plane->state->crtc)
			err = tegra_window_state_remove(window, state);

		goto out;
	}

	err = tegra_window_get_format(state->fb->format->format,
				      &window_state->format);
	if (err < 0)
		goto out;

	err = tegra_window_state_add(window, state);
	if (err < 0)
		goto out;

out:
	pr_debug("< %s() = %d\n", __func__, err);
	return err;
}

static void tegra_window_atomic_update(struct drm_plane *plane,
				       struct drm_plane_state *old_state)
{
	struct tegra_dc *dc = to_tegra_dc(plane->state->crtc);
	struct tegra_window *window = to_tegra_window(plane);
	struct drm_plane_state *state = plane->state;
	struct drm_crtc *crtc = plane->state->crtc;
	struct tegra_window_state *window_state;
	struct tegra_bo *bo;
	dma_addr_t base;
	u32 value;

	pr_debug("> %s(plane=%p, old_state=%p)\n", __func__, plane, old_state);

	window_state = to_tegra_window_state(state);

	/* rien ne va plus */
	if (!state->crtc || !state->fb)
		goto out;

	pr_debug("  registers at: %08x\n", window->offset);
	pr_debug("  attaching to CRTC %p...\n", dc);
	pr_debug("    %u: %s...\n", crtc->index, dev_name(dc->dev));

	window->regs = dc->regs + window->offset;

	value = readl(window->regs + DC_WIN_CORE_WINDOWGROUP_SET_CONTROL);
	value &= ~OWNER_MASK;
	value |= OWNER(dc->pipe);
	writel(value, window->regs + DC_WIN_CORE_WINDOWGROUP_SET_CONTROL);

	bo = tegra_fb_get_plane(state->fb, 0);
	base = bo->paddr;

	pr_debug("  base: %pad\n", &base);

	writel(window_state->format, window->regs + DC_WIN_COLOR_DEPTH);

	value = V_POSITION(state->crtc_y) | H_POSITION(state->crtc_x);
	writel(value, window->regs + DC_WIN_POSITION);

	value = V_SIZE(state->crtc_h) | H_SIZE(state->crtc_w);
	writel(value, window->regs + DC_WIN_SIZE);

	value = WIN_ENABLE;
	writel(value, window->regs + DC_WIN_WIN_OPTIONS);

	value = V_SIZE(state->crtc_h) | H_SIZE(state->crtc_w);
	writel(value, window->regs + DC_WIN_CROPPED_SIZE);

	writel(upper_32_bits(base), window->regs + DC_WINBUF_START_ADDR_HI);
	writel(lower_32_bits(base), window->regs + DC_WINBUF_START_ADDR);

	value = PITCH(state->fb->pitches[0]);
	writel(value, window->regs + DC_WIN_PLANAR_STORAGE);

	/* XXX */
	value = 0;
	writel(value, window->regs + DC_WIN_SET_PARAMS);

	value = OFFSET_X(state->crtc_y) | OFFSET_Y(state->crtc_x);
	writel(value, window->regs + DC_WINBUF_CROPPED_POINT);

	/* XXX */
	value = BLOCK_HEIGHT_1 | SURFACE_KIND_PITCH;
	writel(value, window->regs + DC_WINBUF_SURFACE_KIND);

out:
	pr_debug("< %s()\n", __func__);
}

static void tegra_window_atomic_disable(struct drm_plane *plane,
					struct drm_plane_state *old_state)
{
	struct tegra_dc *dc = to_tegra_dc(plane->crtc);
	struct tegra_window *window = to_tegra_window(plane);
	u32 value;

	pr_debug("> %s(plane=%p, old_state=%p)\n", __func__, plane, old_state);
	pr_debug("  detaching from CRTC %p...\n", plane->crtc);
	pr_debug("    %u: %s...\n", plane->crtc->index, dev_name(dc->dev));

	value = readl(window->regs + DC_WIN_WIN_OPTIONS);
	value &= ~WIN_ENABLE;
	writel(value, window->regs + DC_WIN_WIN_OPTIONS);

	value = readl(window->regs + DC_WIN_CORE_WINDOWGROUP_SET_CONTROL);
	value &= ~OWNER_MASK;
	writel(value, window->regs + DC_WIN_CORE_WINDOWGROUP_SET_CONTROL);

	window->regs = NULL;

	pr_debug("< %s()\n", __func__);
}

static const struct drm_plane_helper_funcs tegra_window_helper_funcs = {
	.atomic_check = tegra_window_atomic_check,
	.atomic_update = tegra_window_atomic_update,
	.atomic_disable = tegra_window_atomic_disable,
};

static struct drm_plane *tegra_window_create(struct drm_device *drm,
					     struct drm_crtc *crtc)
{
	struct tegra_dc *dc = to_tegra_dc(crtc);
	unsigned long possible_crtcs;
	struct tegra_window *window;
	struct drm_plane *plane;
	unsigned int index;
	int err;

	dev_dbg(dc->dev, "> %s(drm=%p, crtc=%p)\n", __func__, drm, crtc);

	window = kzalloc(sizeof(*window), GFP_KERNEL);
	if (!window) {
		plane = ERR_PTR(-ENOMEM);
		goto out;
	}

	possible_crtcs = drm_crtc_mask(crtc);
	index = drm_crtc_index(crtc);

	/* XXX decouple this from the CRTC index */
	window->offset = 0x2800 + 0x0c00 * index;
	window->index = index;

	dev_dbg(dc->dev, "  offset: %x\n", window->offset);

	/* XXX: this should really be DRM_PLANE_TYPE_OVERLAY */
	err = drm_universal_plane_init(drm, &window->base, possible_crtcs,
				       &tegra_window_funcs,
				       tegra_window_formats,
				       ARRAY_SIZE(tegra_window_formats),
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (err < 0) {
		plane = ERR_PTR(err);
		kfree(window);
		goto out;
	}

	plane = &window->base;

	drm_plane_helper_add(&window->base, &tegra_window_helper_funcs);

out:
	dev_dbg(dc->dev, "< %s() = %p\n", __func__, plane);

	return plane;
}

static int tegra_dc_init(struct host1x_client *client)
{
	struct drm_device *drm = dev_get_drvdata(client->parent);
	struct tegra_dc *dc = host1x_client_to_tegra_dc(client);
	struct tegra_drm *tegra = drm->dev_private;
	struct drm_plane *primary, *cursor;
	int err;

	dev_dbg(client->dev, "> %s(client=%p)\n", __func__, client);

	/*
	writel(0, dc->regs + DC_CMD_STATE_ACCESS);
	*/

	primary = tegra_window_create(drm, &dc->base);
	if (IS_ERR(primary)) {
		err = PTR_ERR(primary);
		goto out;
	}

	cursor = tegra_cursor_create(drm, &dc->base);
	if (IS_ERR(cursor)) {
		err = PTR_ERR(cursor);
		goto out;
	}

	err = drm_crtc_init_with_planes(drm, &dc->base, primary, cursor,
					&tegra_dc_funcs, NULL);
	if (err < 0)
		goto out;

	dc->base.port = client->dev->of_node;

	drm_crtc_helper_add(&dc->base, &tegra_dc_helper_funcs);

	/* XXX */
	tegra->pitch_align = 64;

out:
	dev_dbg(client->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_dc_exit(struct host1x_client *client)
{
	struct tegra_dc *dc = host1x_client_to_tegra_dc(client);
	int err = 0;

	dev_dbg(client->dev, "> %s(client=%p)\n", __func__, client);

	reset_control_assert(dc->rst);

	dev_dbg(client->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static const struct host1x_client_ops tegra_dc_ops = {
	.init = tegra_dc_init,
	.exit = tegra_dc_exit,
};

static void tegra_dc_finish_page_flip(struct tegra_dc *dc)
{
	struct drm_device *drm = dc->base.dev;
	struct drm_crtc *crtc = &dc->base;
	unsigned long flags;

	spin_lock_irqsave(&drm->event_lock, flags);

	if (dc->event) {
		drm_crtc_send_vblank_event(crtc, dc->event);
		drm_crtc_vblank_put(crtc);
		dc->event = NULL;
	}

	spin_unlock_irqrestore(&drm->event_lock, flags);
}

static irqreturn_t tegra_dc_irq(int irq, void *data)
{
	struct tegra_dc *dc = data;
	u32 status;

	status = readl(dc->regs + DC_CMD_INT_STATUS);
	writel(status, dc->regs + DC_CMD_INT_STATUS);

	if (status & VBLANK_INT) {
		drm_crtc_handle_vblank(&dc->base);
		tegra_dc_finish_page_flip(dc);
	}

	return IRQ_HANDLED;
}

static int tegra_dc_parse_dt(struct tegra_dc *dc)
{
	u32 value = 0;
	int err;

	err = of_property_read_u32(dc->dev->of_node, "nvidia,head", &value);
	if (err < 0) {
		dev_err(dc->dev, "missing \"nvidia,head\" property\n");
		return -EINVAL;
	}

	dc->pipe = value;

	return 0;
}

static int tegra_dc_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct tegra_dc *dc;
	int err;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	dc = devm_kzalloc(&pdev->dev, sizeof(*dc), GFP_KERNEL);
	if (!dc) {
		err = -ENOMEM;
		goto out;
	}

	spin_lock_init(&dc->lock);
	dc->dev = &pdev->dev;

	err = tegra_dc_parse_dt(dc);
	if (err < 0)
		return err;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dc->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(dc->regs)) {
		err = PTR_ERR(dc->regs);
		goto out;
	}

	err = platform_get_irq(pdev, 0);
	if (err < 0)
		goto out;

	dc->irq = err;

	dc->clk = devm_clk_get(&pdev->dev, "head");
	if (IS_ERR(dc->clk)) {
		err = PTR_ERR(dc->clk);
		goto out;
	}

	dc->rst = devm_reset_control_get(&pdev->dev, "head");
	if (IS_ERR(dc->rst)) {
		err = PTR_ERR(dc->rst);
		goto out;
	}

	if (0 && IS_ENABLED(CONFIG_DEBUG_FS)) {
		unsigned int i, width = 0;

		for (i = 0; i < ARRAY_SIZE(tegra186_dc_regs); i++) {
			int len = strlen(tegra186_dc_regs[i].name);

			if (len > width)
				width = len;
		}

		for (i = 0; i < ARRAY_SIZE(tegra186_dc_regs); i++) {
			const struct tegra_dc_register *reg = &tegra186_dc_regs[i];
			u32 value = readl(dc->regs + reg->offset);

			pr_debug("%#06x %-*s %#010x\n", reg->offset, width,
				 reg->name, value);
		}
	}

	/* assert reset and disable clock */
	err = clk_prepare_enable(dc->clk);
	if (err < 0)
		goto out;

	usleep_range(2000, 4000);

	err = reset_control_assert(dc->rst);
	if (err < 0)
		goto out;

	usleep_range(2000, 4000);

	clk_disable_unprepare(dc->clk);

	platform_set_drvdata(pdev, dc);
	pm_runtime_enable(&pdev->dev);
	//pm_runtime_get_sync(&pdev->dev); /* XXX */

	err = devm_request_irq(&pdev->dev, dc->irq, tegra_dc_irq, IRQF_SHARED,
			       dev_name(&pdev->dev), dc);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to request IRQ: %d\n", err);
		goto out;
	}

	INIT_LIST_HEAD(&dc->client.list);
	dc->client.ops = &tegra_dc_ops;
	dc->client.dev = &pdev->dev;

	err = host1x_client_register(&dc->client);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to register host1x client: %d\n",
			err);
		goto out;
	}

out:
	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_dc_remove(struct platform_device *pdev)
{
	struct tegra_dc *dc = platform_get_drvdata(pdev);
	int err = 0;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	err = host1x_client_unregister(&dc->client);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to unregister host1x client: %d\n",
			err);
		goto out;
	}

	devm_free_irq(&pdev->dev, dc->irq, dc);
	//pm_runtime_put(&pdev->dev); /* XXX */
	pm_runtime_disable(&pdev->dev);

out:
	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_dc_suspend(struct device *dev)
{
	struct tegra_dc *dc = dev_get_drvdata(dev);
	int err;

	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);

	err = reset_control_assert(dc->rst);
	if (err < 0)
		goto out;

	clk_disable_unprepare(dc->clk);

out:
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_dc_resume(struct device *dev)
{
	struct tegra_dc *dc = dev_get_drvdata(dev);
	int err;

	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);

	err = clk_prepare_enable(dc->clk);
	if (err < 0)
		goto out;

	err = reset_control_deassert(dc->rst);
	if (err < 0) {
		clk_disable_unprepare(dc->clk);
		goto out;
	}

out:
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return 0;
}

static const struct dev_pm_ops tegra_dc_pm_ops = {
	SET_RUNTIME_PM_OPS(tegra_dc_suspend, tegra_dc_resume, NULL)
};

static const struct of_device_id tegra186_dc_of_match[] = {
	{ .compatible = "nvidia,tegra186-dc" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, tegra186_dc_of_match);

struct platform_driver tegra186_dc_driver = {
	.driver = {
		.name = "tegra186-dc",
		.of_match_table = tegra186_dc_of_match,
		.pm = &tegra_dc_pm_ops,
	},
	.probe = tegra_dc_probe,
	.remove = tegra_dc_remove,
};
