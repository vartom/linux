#ifndef DRM_TEGRA_PARKER_DC_H
#define DRM_TEGRA_PARKER_DC_H

#include <linux/host1x.h>

struct clk;
struct drm_crtc;
struct drm_crtc_state;

int tegra_dc_state_setup(struct drm_crtc *crtc,
			 struct drm_crtc_state *crtc_state,
			 unsigned int bpc, struct clk *parent,
			 unsigned long pclk);

struct tegra_dc {
	struct host1x_client client;
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

static inline struct tegra_dc *to_tegra_dc(struct drm_crtc *crtc)
{
	return container_of(crtc, struct tegra_dc, base);
}

static inline struct tegra_dc *
host1x_client_to_tegra_dc(struct host1x_client *client)
{
	return container_of(client, struct tegra_dc, client);
}

void tegra186_dc_commit(struct tegra_dc *crtc);

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

#define  WIN_ACT_REQ(x) (1 << (1 + (x)))

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
#define  WIN_ENABLE (1 << 30)

#define DC_WIN_CORE_WINDOWGROUP_SET_CONTROL 0x0608
#define  OWNER_MASK (0xf << 0)
#define  OWNER(x) (((x) & 0xf) << 0)

#define DC_WIN_COLOR_DEPTH 0x060c
#define  COLOR_DEPTH_B8G8R8A8 12
#define  COLOR_DEPTH_R8G8B8A8 13

#define DC_WIN_POSITION 0x0610
#define  V_POSITION(x) (((x) & 0x7fff) << 16)
#define  H_POSITION(x) (((x) & 0x7fff) << 0)

#define DC_WIN_SIZE 0x0614
#define  V_SIZE(x) (((x) & 0x7fff) << 16)
#define  H_SIZE(x) (((x) & 0x7fff) << 0)

#define DC_WIN_CROPPED_SIZE 0x0618

#define DC_WIN_PLANAR_STORAGE 0x0624
#define  PITCH(x) (((x) >> 6) & 0x1fff)

#define DC_WIN_SET_PARAMS 0x0634

#define DC_WINBUF_START_ADDR 0x0700
#define DC_WINBUF_START_ADDR_NS 0x0704
#define DC_WINBUF_START_ADDR_U 0x0708
#define DC_WINBUF_START_ADDR_U_NS 0x070c
#define DC_WINBUF_START_ADDR_V 0x0710
#define DC_WINBUF_START_ADDR_V_NS 0x0714

#define DC_WINBUF_CROPPED_POINT 0x0718
#define  OFFSET_Y(x) (((x) & 0xffff) << 16)
#define  OFFSET_X(x) (((x) & 0xffff) << 0)

#define DC_WINBUF_SURFACE_KIND 0x072c
#define  BLOCK_HEIGHT_1  (0 << 4)
#define  BLOCK_HEIGHT_2  (1 << 4)
#define  BLOCK_HEIGHT_4  (2 << 4)
#define  BLOCK_HEIGHT_8  (3 << 4)
#define  BLOCK_HEIGHT_16 (4 << 4)
#define  BLOCK_HEIGHT_32 (5 << 4)
#define  SURFACE_KIND_PITCH (0 << 0)
#define  SURFACE_KIND_TILED (1 << 0)
#define  SURFACE_KIND_BLOCK (2 << 0)

#define DC_WINBUF_START_ADDR_HI 0x0734
#define DC_WINBUF_START_ADDR_HI_NS 0x0738
#define DC_WINBUF_START_ADDR_HI_U 0x073c
#define DC_WINBUF_START_ADDR_HI_U_NS 0x0740
#define DC_WINBUF_START_ADDR_HI_V 0x0744
#define DC_WINBUF_START_ADDR_HI_V_NS 0x0748

#endif
