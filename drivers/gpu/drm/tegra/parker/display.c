#define DEBUG

#include <linux/clk.h>
#include <linux/clk-provider.h> /* XXX */
#include <linux/component.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>

#include "display.h"

#define WIN_ACT_REQ(x) (1 << (1 + (x)))

#define DC_DISP_BLEND_BACKGROUND_COLOR 0x1390

#define DC_WINC_PRECOMP_WGRP_PIPE_CAPA 0x0000
#define DC_WINC_PRECOMP_WGRP_PIPE_CAPB 0x0004
#define DC_WINC_PRECOMP_WGRP_PIPE_CAPC 0x0008
#define DC_WINC_PRECOMP_WGRP_PIPE_CAPD 0x000c
#define DC_WINC_PRECOMP_WGRP_PIPE_CAPE 0x0010
#define DC_WINC_PRECOMP_WGRP_PIPE_CAPF 0x0014

#define DC_WINC_REG_ACT_CONTROL 0x0038
#define  DC_WINC_REG_ACT_CONTROL_HCOUNTER (1 << 0)

#define DC_WINC_PRECOMP_WGRP_DFLT_RGB_COLOR 0x0090

#define DC_WIN_WIN_OPTIONS 0x0600

#define DC_WIN_CORE_WINDOWGROUP_SET_CONTROL 0x0608
#define  OWNER_MASK (0xf << 0)
#define  OWNER(x) (((x) & 0xf) << 0)

#define DC_WIN_COLOR_DEPTH 0x060c
#define DC_WIN_POSITION 0x0610
#define DC_WIN_SIZE 0x0614

#define DC_WINBUF_START_ADDR 0x0700
#define DC_WINBUF_START_ADDR_NS 0x0704
#define DC_WINBUF_START_ADDR_U 0x0708
#define DC_WINBUF_START_ADDR_U_NS 0x070c
#define DC_WINBUF_START_ADDR_V 0x0710
#define DC_WINBUF_START_ADDR_V_NS 0x0714
#define DC_WINBUF_SURFACE_KIND 0x072c
#define DC_WINBUF_START_ADDR_HI 0x0734
#define DC_WINBUF_START_ADDR_HI_NS 0x0738
#define DC_WINBUF_START_ADDR_HI_U 0x073c
#define DC_WINBUF_START_ADDR_HI_U_NS 0x0740
#define DC_WINBUF_START_ADDR_HI_V 0x0744
#define DC_WINBUF_START_ADDR_HI_V_NS 0x0748

struct tegra_crtc_state {
	struct drm_crtc_state base;

	struct clk *parent;
	unsigned long pclk;
	unsigned int bpc;

	u32 planes;
};

static inline struct tegra_crtc_state *
to_tegra_crtc_state(struct drm_crtc_state *state)
{
	return container_of(state, struct tegra_crtc_state, base);
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

static inline u32 tegra_crtc_readl(struct tegra_crtc *tegra, unsigned int offset)
{
	u32 value = readl(tegra->regs + offset);

	dev_dbg(tegra->dev, "%08x > %08x\n", offset, value);

	return value;
}

static inline void tegra_crtc_writel(struct tegra_crtc *tegra, u32 value,
				     unsigned int offset)
{
	dev_dbg(tegra->dev, "%08x < %08x\n", offset, value);
	writel(value, tegra->regs + offset);
}

void tegra_crtc_update(struct tegra_crtc *tegra)
{
	tegra_crtc_writel(tegra, GENERAL_UPDATE, DC_CMD_STATE_CONTROL);
}

void tegra_crtc_commit(struct tegra_crtc *tegra)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(250);
	u32 value;

	//tegra_crtc_writel(tegra, GENERAL_UPDATE, DC_CMD_STATE_CONTROL);
	//tegra_crtc_readl(tegra, DC_CMD_STATE_CONTROL);

	tegra_crtc_writel(tegra, GENERAL_ACTREQ, DC_CMD_STATE_CONTROL);

	while (time_before(jiffies, timeout)) {
		value = tegra_crtc_readl(tegra, DC_CMD_STATE_CONTROL);
		if ((value & GENERAL_ACTREQ) == 0)
			break;
	}

	dev_warn(tegra->dev, "timed out waiting for general activation request\n");
}

static u32 tegra_crtc_readl_active(struct tegra_crtc *tegra,
				   unsigned int offset)
{
	unsigned long flags;
	u32 value;

	spin_lock_irqsave(&tegra->lock, flags);

	writel(READ_MUX, tegra->regs + DC_CMD_STATE_ACCESS);
	value = readl(tegra->regs + offset);
	writel(0, tegra->regs + DC_CMD_STATE_ACCESS);

	spin_unlock_irqrestore(&tegra->lock, flags);

	return value;
}

void tegra_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);
	u32 value;

	dev_dbg(tegra->dev, "> %s(crtc=%p)\n", __func__, crtc);

	value = readl(tegra->regs + DC_CMD_INT_MASK);
	value |= VBLANK_INT;
	writel(value, tegra->regs + DC_CMD_INT_MASK);

	dev_dbg(tegra->dev, "< %s()\n", __func__);
}

void tegra_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);
	u32 value;

	dev_dbg(tegra->dev, "> %s(crtc=%p)\n", __func__, crtc);

	value = readl(tegra->regs + DC_CMD_INT_MASK);
	value &= ~VBLANK_INT;
	writel(value, tegra->regs + DC_CMD_INT_MASK);

	dev_dbg(tegra->dev, "< %s()\n", __func__);
}

static int tegra_crtc_start(struct tegra_crtc *tegra)
{
	u32 value;

	value = readl(tegra->regs + DC_CMD_DISPLAY_COMMAND);
	value &= ~DISPLAY_CTRL_MODE_MASK;
	value |= DISPLAY_CTRL_MODE_C_DISPLAY;
	writel(value, tegra->regs + DC_CMD_DISPLAY_COMMAND);

	tegra_crtc_commit(tegra);

	return 0;
}

static void tegra_crtc_stop(struct tegra_crtc *tegra)
{
	u32 value;

	value = readl(tegra->regs + DC_CMD_DISPLAY_COMMAND);
	value &= ~DISPLAY_CTRL_MODE_MASK;
	writel(value, tegra->regs + DC_CMD_DISPLAY_COMMAND);

	tegra_crtc_commit(tegra);
}

static bool tegra_crtc_idle(struct tegra_crtc *tegra)
{
	u32 value;

	value = tegra_crtc_readl_active(tegra, DC_CMD_DISPLAY_COMMAND);

	return (value & DISPLAY_CTRL_MODE_MASK) == 0;
}

static int tegra_crtc_wait_idle(struct tegra_crtc *tegra,
				unsigned long timeout)
{
	timeout = jiffies + msecs_to_jiffies(timeout);

	while (time_before(jiffies, timeout)) {
		if (tegra_crtc_idle(tegra))
			return 0;

		usleep_range(1000, 2000);
	}

	dev_dbg(tegra->dev, "timeout waiting for CRTC to become idle\n");
	return -ETIMEDOUT;
}

int tegra_crtc_state_setup(struct drm_crtc *crtc,
			   struct drm_crtc_state *crtc_state,
			   unsigned int bpc, struct clk *parent,
			   unsigned long pclk)
{
	struct tegra_crtc_state *state = to_tegra_crtc_state(crtc_state);
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);
	int err = 0;

	dev_dbg(tegra->dev, "> %s(crtc=%p, crtc_state=%p, bpc=%u, parent=%p, pclk=%lu)\n",
		__func__, crtc, crtc_state, bpc, parent, pclk);

	if (!clk_has_parent(tegra->clk, parent)) {
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
	dev_dbg(tegra->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static void tegra_crtc_destroy(struct drm_crtc *crtc)
{
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);

	dev_dbg(tegra->dev, "> %s(crtc=%p)\n", __func__, crtc);

	drm_crtc_cleanup(crtc);

	dev_dbg(tegra->dev, "< %s()\n", __func__);
}

static void tegra_crtc_reset(struct drm_crtc *crtc)
{
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);
	struct tegra_crtc_state *state;

	dev_dbg(tegra->dev, "> %s(crtc=%p)\n", __func__, crtc);

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

	dev_dbg(tegra->dev, "< %s()\n", __func__);
}

static struct drm_crtc_state *
tegra_crtc_atomic_duplicate_state(struct drm_crtc *crtc)
{
	struct tegra_crtc_state *state = to_tegra_crtc_state(crtc->state);
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);
	struct tegra_crtc_state *copy;

	dev_dbg(tegra->dev, "> %s(crtc=%p)\n", __func__, crtc);

	copy = kmemdup(state, sizeof(*state), GFP_KERNEL);
	if (!copy)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &copy->base);

	dev_dbg(tegra->dev, "< %s() = %p\n", __func__, &copy->base);
	return &copy->base;
}

static void tegra_crtc_atomic_destroy_state(struct drm_crtc *crtc,
					    struct drm_crtc_state *state)
{
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);

	dev_dbg(tegra->dev, "> %s(crtc=%p, state=%p)\n", __func__, crtc, state);

	__drm_atomic_helper_crtc_destroy_state(state);
	kfree(state);

	dev_dbg(tegra->dev, "< %s()\n", __func__);
}

#ifdef CONFIG_DEBUG_FS
static const struct {
	unsigned int offset;
	const char *name;
} tegra_crtc_regs[] = {
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

static int tegra_crtc_regs_show(struct seq_file *s, void *data)
{
	struct tegra_crtc *tegra = s->private;
	unsigned int i;
	int width = 0;

	for (i = 0; i < ARRAY_SIZE(tegra_crtc_regs); i++) {
		int len = strlen(tegra_crtc_regs[i].name);

		if (len > width)
			width = len;
	}

	for (i = 0; i < ARRAY_SIZE(tegra_crtc_regs); i++) {
		u32 value = readl(tegra->regs + tegra_crtc_regs[i].offset);

		seq_printf(s, "%#06x %-*s %#010x\n", tegra_crtc_regs[i].offset,
			   width, tegra_crtc_regs[i].name, value);
	}

	return 0;
}

static int tegra_crtc_regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, tegra_crtc_regs_show, inode->i_private);
}

static const struct file_operations tegra_crtc_regs_ops = {
	.open = tegra_crtc_regs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static int tegra_crtc_late_register(struct drm_crtc *crtc)
{
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);
	struct dentry *root = crtc->debugfs_entry;
	int err = 0;

	dev_dbg(tegra->dev, "> %s(crtc=%p)\n", __func__, crtc);

#ifdef CONFIG_DEBUG_FS
	tegra->debugfs.regs = debugfs_create_file("regs", S_IRUGO, root, tegra,
						  &tegra_crtc_regs_ops);
	if (!tegra->debugfs.regs)
		dev_err(tegra->dev, "failed to create debugfs \"regs\" file\n");
#endif

	dev_dbg(tegra->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static void tegra_crtc_early_unregister(struct drm_crtc *crtc)
{
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);

#ifdef CONFIG_DEBUG_FS
	debugfs_remove(tegra->debugfs.regs);
#endif
}

static const struct drm_crtc_funcs tegra_crtc_funcs = {
	.page_flip = drm_atomic_helper_page_flip,
	.set_config = drm_atomic_helper_set_config,
	.destroy = tegra_crtc_destroy,
	.reset = tegra_crtc_reset,
	.atomic_duplicate_state = tegra_crtc_atomic_duplicate_state,
	.atomic_destroy_state = tegra_crtc_atomic_destroy_state,
	.late_register = tegra_crtc_late_register,
	.early_unregister = tegra_crtc_early_unregister,
};

static void tegra_crtc_disable(struct drm_crtc *crtc)
{
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);

	dev_dbg(tegra->dev, "> %s(crtc=%p)\n", __func__, crtc);

	if (!tegra_crtc_idle(tegra)) {
		tegra_crtc_stop(tegra);

		/*
		 * Ignore the return value, there isn't anything useful to do
		 * in case this fails.
		 */
		tegra_crtc_wait_idle(tegra, 100);
	}

	drm_crtc_vblank_off(crtc);
	pm_runtime_put(tegra->dev);

	dev_dbg(tegra->dev, "< %s()\n", __func__);
}

static void tegra_crtc_enable(struct drm_crtc *crtc)
{
	struct tegra_crtc_state *state = to_tegra_crtc_state(crtc->state);
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);
	u32 value;
	int err;

	dev_dbg(tegra->dev, "> %s(crtc=%p)\n", __func__, crtc);

	pm_runtime_get_sync(tegra->dev);

	value = 0;
	writel(value, tegra->regs + DC_CMD_INT_TYPE);

	value = REG_TMOUT_INT | VBLANK_INT | FRAME_END_INT;
	writel(value, tegra->regs + DC_CMD_INT_POLARITY);

	value = REG_TMOUT_INT | VBLANK_INT | FRAME_END_INT;
	writel(value, tegra->regs + DC_CMD_INT_ENABLE);

	value = REG_TMOUT_INT | VBLANK_INT | FRAME_END_INT;
	writel(value, tegra->regs + DC_CMD_INT_MASK);

	tegra_crtc_update(tegra);

	writel(0x00000000, tegra->regs + DC_DISP_BLEND_BACKGROUND_COLOR);

	DRM_DEBUG_KMS("module clock: %lu Hz (%s)\n", clk_get_rate(tegra->clk),
		      __clk_get_name(tegra->clk));

	err = clk_set_parent(tegra->clk, state->parent);
	if (err < 0)
		dev_err(tegra->dev, "failed to set parent clock: %d\n", err);

	DRM_DEBUG_KMS("pixel clock: %lu Hz, parent: %s\n", state->pclk,
		      __clk_get_name(state->parent));

	err = clk_set_rate(state->parent, state->pclk);
	if (err < 0)
		dev_err(tegra->dev, "failed to set clock rate to %lu Hz: %d\n",
			state->pclk, err);

	err = clk_set_rate(tegra->clk, state->pclk);
	if (err < 0)
		dev_err(tegra->dev, "failed to set clock rate to %lu Hz: %d\n",
			state->pclk, err);

	value = ((mode->vsync_end - mode->vsync_start) << 16) |
		((mode->hsync_end - mode->hsync_start) <<  0);
	writel(value, tegra->regs + DC_DISP_SYNC_WIDTH);

	value = ((mode->vtotal - mode->vsync_end) << 16) |
		((mode->htotal - mode->hsync_end) <<  0);
	writel(value, tegra->regs + DC_DISP_BACK_PORCH);

	value = ((mode->vsync_start - mode->vdisplay) << 16) |
		((mode->hsync_start - mode->hdisplay) <<  0);
	writel(value, tegra->regs + DC_DISP_FRONT_PORCH);

	value = (mode->vdisplay << 16) | mode->hdisplay;
	writel(value, tegra->regs + DC_DISP_DISP_ACTIVE);

	err = tegra_crtc_start(tegra);
	if (err < 0)
		dev_err(tegra->dev, "failed to start display controller: %d\n", err);

	drm_crtc_vblank_on(crtc);

	dev_dbg(tegra->dev, "< %s()\n", __func__);
}

static int tegra_crtc_atomic_check(struct drm_crtc *crtc,
				   struct drm_crtc_state *state)
{
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);
	int err = 0;

	dev_dbg(tegra->dev, "> %s(crtc=%p, state=%p)\n", __func__, crtc,
		state);

	dev_dbg(tegra->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static void tegra_crtc_atomic_begin(struct drm_crtc *crtc,
				    struct drm_crtc_state *old_crtc_state)
{
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);

	dev_dbg(tegra->dev, "> %s(crtc=%p, old_crtc_state=%p)\n", __func__,
		crtc, old_crtc_state);

	if (crtc->state->event) {
		crtc->state->event->pipe = drm_crtc_index(crtc);

		WARN_ON(drm_crtc_vblank_get(crtc) != 0);

		tegra->event = crtc->state->event;
		crtc->state->event = NULL;
	}

	dev_dbg(tegra->dev, "< %s()\n", __func__);
}

static void tegra_crtc_atomic_flush(struct drm_crtc *crtc,
				    struct drm_crtc_state *old_crtc_state)
{
	struct tegra_crtc_state *state = to_tegra_crtc_state(crtc->state);
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);

	dev_dbg(tegra->dev, "> %s(crtc=%p, old_crtc_state=%p)\n", __func__,
		crtc, old_crtc_state);

	writel(state->planes << 8, tegra->regs + DC_CMD_STATE_CONTROL);
	writel(state->planes, tegra->regs + DC_CMD_STATE_CONTROL);

	dev_dbg(tegra->dev, "< %s()\n", __func__);
}

static const struct drm_crtc_helper_funcs tegra_crtc_helper_funcs = {
	.disable = tegra_crtc_disable,
	.enable = tegra_crtc_enable,
	.atomic_check = tegra_crtc_atomic_check,
	.atomic_begin = tegra_crtc_atomic_begin,
	.atomic_flush = tegra_crtc_atomic_flush,
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
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);
	unsigned long possible_crtcs;
	struct tegra_cursor *cursor;
	struct drm_plane *plane;
	int err;

	dev_dbg(tegra->dev, "> %s(drm=%p, crtc=%p)\n", __func__, drm, crtc);

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
	dev_dbg(tegra->dev, "< %s() = %p\n", __func__, plane);

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
	struct tegra_window_state *copy;

	pr_debug("> %s(plane=%p)\n", __func__, plane);

	copy = kmalloc(sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &copy->base);

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

static int tegra_window_state_add(struct tegra_window *window,
				  struct drm_plane_state *state)
{
	struct drm_crtc_state *crtc_state;
	struct tegra_crtc_state *tegra;

	/* propagate errors from allocation or locking failures */
	crtc_state = drm_atomic_get_crtc_state(state->state, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	tegra = to_tegra_crtc_state(crtc_state);
	tegra->planes |= WIN_ACT_REQ(window->index);

	return 0;
}

static int tegra_window_atomic_check(struct drm_plane *plane,
				     struct drm_plane_state *state)
{
	struct tegra_window *window = to_tegra_window(plane);
	int err = 0;

	pr_debug("> %s(plane=%p, state=%p)\n", __func__, plane, state);

	/* no need for further checks if the plane is being disabled */
	if (!state->crtc)
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
	struct tegra_crtc *tegra = to_tegra_crtc(plane->state->crtc);
	struct tegra_window *window = to_tegra_window(plane);
	struct drm_crtc *crtc = plane->state->crtc;
	u32 value;

	pr_debug("> %s(plane=%p, old_state=%p)\n", __func__, plane, old_state);

	/* rien ne va plus */
	if (!plane->state->crtc || !plane->state->fb)
		goto out;

	pr_debug("  registers at: %08x\n", window->offset);
	pr_debug("  attaching to CRTC %p...\n", crtc);
	pr_debug("    %u: %s...\n", crtc->index, dev_name(tegra->dev));

	window->regs = tegra->regs + window->offset;

	value = readl(window->regs + DC_WIN_CORE_WINDOWGROUP_SET_CONTROL);
	value &= ~OWNER_MASK;
	value |= OWNER(tegra->pipe);
	writel(value, window->regs + DC_WIN_CORE_WINDOWGROUP_SET_CONTROL);

	pr_debug("  capabilities:\n");

	value = readl(window->regs + DC_WINC_PRECOMP_WGRP_PIPE_CAPA);
	pr_debug("    CGMT: %u bits\n", (value >> 20) & 0xf);
	pr_debug("    LUT: %u bits\n", (value >> 16) & 0xf);
	pr_debug("    YUV: %u bits\n", (value >> 12) & 0xf);
	pr_debug("    SCLR: %u bits\n", (value >> 8) & 0xf);
	pr_debug("    UNIT: %u bits\n", (value >> 4) & 0xf);
	pr_debug("    FULL: %u bits\n", (value >> 0) & 0xf);

	value = readl(window->regs + DC_WINC_PRECOMP_WGRP_PIPE_CAPB);
	pr_debug("    degamma: %s\n", (value & BIT(16)) ? "yes" : "no");
	pr_debug("    fp16: %s\n", (value & BIT(15)) ? "yes" : "no");
	pr_debug("    cgmt: %s\n", (value & BIT(14)) ? "yes" : "no");
	pr_debug("    LUT: %u\n", (value >> 12) & 0x3);
	pr_debug("    scaler: %u\n", (value >> 8) & 0x3);
	pr_debug("    windows: %u\n", (value >> 0) & 0x3);

	value = readl(window->regs + DC_WINC_PRECOMP_WGRP_PIPE_CAPC);
	pr_debug("    5TAP422: %u\n", (value >> 16) & 0xffff);
	pr_debug("    5TAP444: %u\n", (value >>  0) & 0xffff);

	value = readl(window->regs + DC_WINC_PRECOMP_WGRP_PIPE_CAPD);
	pr_debug("    3TAP422: %u\n", (value >> 16) & 0xffff);
	pr_debug("    3TAP444: %u\n", (value >>  0) & 0xffff);

	value = readl(window->regs + DC_WINC_PRECOMP_WGRP_PIPE_CAPE);
	pr_debug("    2TAP422: %u\n", (value >> 16) & 0xffff);
	pr_debug("    2TAP444: %u\n", (value >>  0) & 0xffff);

	value = readl(window->regs + DC_WINC_PRECOMP_WGRP_PIPE_CAPF);
	pr_debug("    1TAP422: %u\n", (value >> 16) & 0xffff);
	pr_debug("    1TAP444: %u\n", (value >>  0) & 0xffff);

	value = readl(window->regs + DC_WIN_CORE_WINDOWGROUP_SET_CONTROL);
	pr_debug("  owner: %u\n", value & 0xf);

out:
	pr_debug("< %s()\n", __func__);
}

static void tegra_window_atomic_disable(struct drm_plane *plane,
					struct drm_plane_state *old_state)
{
	struct tegra_window *window = to_tegra_window(plane);
	u32 value;

	pr_debug("> %s(plane=%p, old_state=%p)\n", __func__, plane, old_state);
	pr_debug("  detaching from CRTC %p...\n", plane->crtc);

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
	struct tegra_crtc *tegra = to_tegra_crtc(crtc);
	unsigned long possible_crtcs;
	struct tegra_window *window;
	struct drm_plane *plane;
	unsigned int index;
	int err;

	dev_dbg(tegra->dev, "> %s(drm=%p, crtc=%p)\n", __func__, drm, crtc);

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

	dev_dbg(tegra->dev, "  offset: %x\n", window->offset);

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
	dev_dbg(tegra->dev, "< %s() = %p\n", __func__, plane);

	return plane;
}

static int tegra186_display_bind(struct device *dev, struct device *master,
				 void *data)
{
	struct tegra_crtc *tegra = dev_get_drvdata(dev);
	struct drm_plane *primary, *cursor;
	struct drm_device *drm = data;
	int err;

	dev_dbg(dev, "> %s(dev=%p, master=%p, data=%p)\n", __func__, dev,
		master, data);

	/*
	writel(0, tegra->regs + DC_CMD_STATE_ACCESS);
	*/

	primary = tegra_window_create(drm, &tegra->base);
	if (IS_ERR(primary)) {
		err = PTR_ERR(primary);
		goto out;
	}

	cursor = tegra_cursor_create(drm, &tegra->base);
	if (IS_ERR(cursor)) {
		err = PTR_ERR(cursor);
		goto out;
	}

	err = drm_crtc_init_with_planes(drm, &tegra->base, primary, cursor,
					&tegra_crtc_funcs, NULL);
	if (err < 0)
		goto out;

	tegra->base.port = dev->of_node;

	drm_crtc_helper_add(&tegra->base, &tegra_crtc_helper_funcs);

out:
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static void tegra186_display_unbind(struct device *dev, struct device *master,
				    void *data)
{
	struct tegra_crtc *tegra = dev_get_drvdata(dev);

	dev_dbg(dev, "> %s(dev=%p, master=%p, data=%p)\n", __func__, dev,
		master, data);

	reset_control_assert(tegra->rst);

	dev_dbg(dev, "< %s()\n", __func__);
}

static const struct component_ops tegra186_display_ops = {
	.bind = tegra186_display_bind,
	.unbind = tegra186_display_unbind,
};

static void tegra_crtc_finish_page_flip(struct tegra_crtc *tegra)
{
	struct drm_device *drm = tegra->base.dev;
	struct drm_crtc *crtc = &tegra->base;
	unsigned long flags;

	spin_lock_irqsave(&drm->event_lock, flags);

	if (tegra->event) {
		drm_crtc_send_vblank_event(crtc, tegra->event);
		drm_crtc_vblank_put(crtc);
		tegra->event = NULL;
	}

	spin_unlock_irqrestore(&drm->event_lock, flags);
}

static irqreturn_t tegra186_display_irq(int irq, void *data)
{
	struct tegra_crtc *tegra = data;
	u32 status;

	status = readl(tegra->regs + DC_CMD_INT_STATUS);
	writel(status, tegra->regs + DC_CMD_INT_STATUS);

	if (status & VBLANK_INT) {
		drm_crtc_handle_vblank(&tegra->base);
		tegra_crtc_finish_page_flip(tegra);
	}

	return IRQ_HANDLED;
}

static int tegra186_display_parse_dt(struct tegra_crtc *crtc)
{
	u32 value = 0;
	int err;

	err = of_property_read_u32(crtc->dev->of_node, "nvidia,head", &value);
	if (err < 0) {
		dev_err(crtc->dev, "missing \"nvidia,head\" property\n");
		return -EINVAL;
	}

	crtc->pipe = value;

	return 0;
}

static int tegra186_display_probe(struct platform_device *pdev)
{
	struct tegra_crtc *crtc;
	struct resource *res;
	int err;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	crtc = devm_kzalloc(&pdev->dev, sizeof(*crtc), GFP_KERNEL);
	if (!crtc) {
		err = -ENOMEM;
		goto out;
	}

	spin_lock_init(&crtc->lock);
	crtc->dev = &pdev->dev;

	err = tegra186_display_parse_dt(crtc);
	if (err < 0)
		return err;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	crtc->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(crtc->regs)) {
		err = PTR_ERR(crtc->regs);
		goto out;
	}

	err = platform_get_irq(pdev, 0);
	if (err < 0)
		goto out;

	crtc->irq = err;

	crtc->clk = devm_clk_get(&pdev->dev, "head");
	if (IS_ERR(crtc->clk)) {
		err = PTR_ERR(crtc->clk);
		goto out;
	}

	crtc->rst = devm_reset_control_get(&pdev->dev, "head");
	if (IS_ERR(crtc->rst)) {
		err = PTR_ERR(crtc->rst);
		goto out;
	}

	if (0 && IS_ENABLED(CONFIG_DEBUG_FS)) {
		unsigned int i, width = 0;

		for (i = 0; i < ARRAY_SIZE(tegra_crtc_regs); i++) {
			int len = strlen(tegra_crtc_regs[i].name);

			if (len > width)
				width = len;
		}

		for (i = 0; i < ARRAY_SIZE(tegra_crtc_regs); i++) {
			u32 value = readl(crtc->regs + tegra_crtc_regs[i].offset);

			pr_debug("%#06x %-*s %#010x\n",
				 tegra_crtc_regs[i].offset, width,
				 tegra_crtc_regs[i].name, value);
		}
	}

	/* assert reset and disable clock */
	err = clk_prepare_enable(crtc->clk);
	if (err < 0)
		goto out;

	usleep_range(2000, 4000);

	err = reset_control_assert(crtc->rst);
	if (err < 0)
		goto out;

	usleep_range(2000, 4000);

	clk_disable_unprepare(crtc->clk);

	platform_set_drvdata(pdev, crtc);
	pm_runtime_enable(&pdev->dev);

	err = devm_request_irq(&pdev->dev, crtc->irq, tegra186_display_irq,
			       IRQF_SHARED, dev_name(&pdev->dev), crtc);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to request IRQ: %d\n", err);
		goto out;
	}

	err = component_add(&pdev->dev, &tegra186_display_ops);
	if (err < 0)
		goto out;

	goto out;

out:
	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra186_display_remove(struct platform_device *pdev)
{
	struct tegra_crtc *crtc = platform_get_drvdata(pdev);
	int err = 0;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	component_del(&pdev->dev, &tegra186_display_ops);
	devm_free_irq(&pdev->dev, crtc->irq, crtc);
	pm_runtime_disable(&pdev->dev);

	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra186_display_suspend(struct device *dev)
{
	struct tegra_crtc *tegra = dev_get_drvdata(dev);
	int err;

	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);

	err = reset_control_assert(tegra->rst);
	if (err < 0)
		goto out;

	clk_disable_unprepare(tegra->clk);

out:
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra186_display_resume(struct device *dev)
{
	struct tegra_crtc *tegra = dev_get_drvdata(dev);
	int err;

	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);

	err = clk_prepare_enable(tegra->clk);
	if (err < 0)
		goto out;

	err = reset_control_deassert(tegra->rst);
	if (err < 0) {
		clk_disable_unprepare(tegra->clk);
		goto out;
	}

out:
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return 0;
}

static const struct dev_pm_ops tegra186_display_pm_ops = {
	SET_RUNTIME_PM_OPS(tegra186_display_suspend, tegra186_display_resume,
			   NULL)
};

static const struct of_device_id tegra186_display_of_match[] = {
	{ .compatible = "nvidia,tegra186-display" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, tegra186_display_of_match);

struct platform_driver tegra186_display_driver = {
	.driver = {
		.name = "tegra186-display",
		.of_match_table = tegra186_display_of_match,
		.pm = &tegra186_display_pm_ops,
	},
	.probe = tegra186_display_probe,
	.remove = tegra186_display_remove,
};
