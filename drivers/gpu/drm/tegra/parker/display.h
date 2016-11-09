#ifndef DRM_TEGRA_PARKER_DISPLAY_H
#define DRM_TEGRA_PARKER_DISPLAY_H

#define DC_CMD_DISPLAY_COMMAND 0x00c8
#define  DISPLAY_CTRL_MODE_STOP (0x0 << 5)
#define  DISPLAY_CTRL_MODE_C_DISPLAY (0x1 << 5)
#define  DISPLAY_CTRL_MODE_NC_DISPLAY (0x2 << 5)
#define  DISPLAY_CTRL_MODE_MASK (0x3 << 5)

#define DC_CMD_REG_PFE_HEAD_DEBUG 0x00cc

#define DC_CMD_INT_STATUS   0x00dc
#define DC_CMD_INT_MASK     0x00e0
#define DC_CMD_INT_ENABLE   0x00e4
#define DC_CMD_INT_TYPE     0x00e8
#define DC_CMD_INT_POLARITY 0x00ec
#define  REG_TMOUT_INT BIT(7)
#define  VBLANK_INT BIT(2)
#define  FRAME_END_INT BIT(1)

#define DC_CMD_STATE_ACCESS 0x100
#define  READ_MUX BIT(2)
#define  WRITE_MUX BIT(0)

#define DC_CMD_STATE_CONTROL 0x104
#define  GENERAL_UPDATE BIT(8)
#define  GENERAL_ACTREQ BIT(0)

#define DC_DISP_DISP_SIGNAL_OPTIONS 0x1000

#define DC_DISP_DISP_WIN_OPTIONS 0x1008
#define  DSI_ENABLE BIT(29)
#define  SOR1_TIMING_CYA BIT(27)
#define  SOR1_ENABLE BIT(26)
#define  SOR_ENABLE BIT(25)
#define  CURSOR_ENABLE BIT(16)

#define DC_DISP_CORE_SOR_SET_CONTROL 0x100c
#define DC_DISP_CORE_SOR1_SET_CONTROL 0x1010
#define DC_DISP_CORE_DSI_SET_CONTROL 0x1014

#define DC_DISP_SYNC_WIDTH 0x101c
#define DC_DISP_BACK_PORCH 0x1020
#define DC_DISP_DISP_ACTIVE 0x1024
#define DC_DISP_FRONT_PORCH 0x1028

#define DC_DISP_DISP_COLOR_CONTROL 0x10c0
#define  DITHER_CONTROL_MASK (0x3 << 8)
#define  DITHER_CONTROL_DISABLE (0 << 8)
#define  BASE_COLOR_SIZE_MASK (0xf << 0)
#define  BASE_COLOR_SIZE_666 (0x0 << 0)
#define  BASE_COLOR_SIZE_888 (0x8 << 0)
#define  BASE_COLOR_SIZE_101010 (0xa << 0)
#define  BASE_COLOR_SIZE_121212 (0xc << 0)

struct clk;
struct drm_crtc;
struct drm_crtc_state;

int tegra_crtc_state_setup(struct drm_crtc *crtc,
			   struct drm_crtc_state *crtc_state,
			   unsigned int bpc, struct clk *parent,
			   unsigned long pclk);

void tegra_crtc_enable_vblank(struct drm_crtc *crtc);
void tegra_crtc_disable_vblank(struct drm_crtc *crtc);

struct tegra_crtc {
	struct drm_crtc base;
	struct spinlock lock;
	struct device *dev;
	unsigned int pipe;

	void __iomem *regs;
	unsigned int irq;

	struct reset_control *rst;
	struct clk *clk;

	/* page-flip handling */
	struct drm_pending_vblank_event *event;

#ifdef CONFIG_DEBUG_FS
	struct {
		struct dentry *regs;
	} debugfs;
#endif
};

static inline struct tegra_crtc *to_tegra_crtc(struct drm_crtc *crtc)
{
	return container_of(crtc, struct tegra_crtc, base);
}

void tegra_crtc_commit(struct tegra_crtc *crtc);

#endif
