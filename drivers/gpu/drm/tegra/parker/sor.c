#include <linux/clk.h>
#include <linux/clk-provider.h> /* XXX */
#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>

#include "../drm.h"
#include "dc.h"

#define SOR_SUPER_STATE0 0x004
#define  SOR_SUPER_STATE0_UPDATE (1 << 0)

#define SOR_SUPER_STATE1 0x008
#define  SOR_SUPER_STATE1_ATTACHED (1 << 3)
#define  SOR_SUPER_STATE1_ASY_ORMODE_MASK (0x1 << 2)
#define  SOR_SUPER_STATE1_ASY_ORMODE_SAFE (0 << 2)
#define  SOR_SUPER_STATE1_ASY_ORMODE_NORMAL (1 << 2)
#define  SOR_SUPER_STATE1_ASY_HEAD_OPMODE_MASK (0x3 << 0)
#define  SOR_SUPER_STATE1_ASY_HEAD_OPMODE_AWAKE (2 << 0)
#define  SOR_SUPER_STATE1_ASY_HEAD_OPMODE_SNOOZE (1 << 0)
#define  SOR_SUPER_STATE1_ASY_HEAD_OPMODE_SLEEP (0 << 0)

#define SOR_STATE0 0x00c
#define  SOR_STATE0_UPDATE (1 << 0)

#define SOR_STATE1 0x010
#define  SOR_STATE1_ASY_PIXELDEPTH_MASK (0xf << 17)
#define  SOR_STATE1_ASY_PIXELDEPTH_BPP_48_444 (9 << 17)
#define  SOR_STATE1_ASY_PIXELDEPTH_BPP_36_444 (8 << 17)
#define  SOR_STATE1_ASY_PIXELDEPTH_BPP_32_422 (7 << 17)
#define  SOR_STATE1_ASY_PIXELDEPTH_BPP_30_444 (6 << 17)
#define  SOR_STATE1_ASY_PIXELDEPTH_BPP_24_444 (5 << 17)
#define  SOR_STATE1_ASY_PIXELDEPTH_BPP_24_422 (4 << 17)
#define  SOR_STATE1_ASY_PIXELDEPTH_BPP_20_422 (3 << 17)
#define  SOR_STATE1_ASY_PIXELDEPTH_BPP_18_444 (2 << 17)
#define  SOR_STATE1_ASY_PIXELDEPTH_BPP_16_422 (1 << 17)
#define  SOR_STATE1_ASY_PIXELDEPTH_DEFAULT (0 << 17)
#define  SOR_STATE1_ASY_REPLICATE_MASK (0x3 << 15)
#define  SOR_STATE1_ASY_REPLICATE_OFF (0 << 15)
#define  SOR_STATE1_ASY_REPLICATE_X2 (1 << 15)
#define  SOR_STATE1_ASY_REPLICATE_X4 (2 << 15)
#define  SOR_STATE1_ASY_DEPOL (1 << 14)
#define  SOR_STATE1_ASY_VSYNCPOL (1 << 13)
#define  SOR_STATE1_ASY_HSYNCPOL (1 << 12)
#define  SOR_STATE1_ASY_PROTOCOL_MASK (0xf << 8)
#define  SOR_STATE1_ASY_PROTOCOL_TMDS_A (1 << 8)
#define  SOR_STATE1_ASY_CRCMODE_MASK (0x3 << 6)
#define  SOR_STATE1_ASY_CRCMODE_ACTIVE (0 << 6)
#define  SOR_STATE1_ASY_CRCMODE_COMPLETE (1 << 6)
#define  SOR_STATE1_ASY_CRCMODE_INACTIVE (2 << 6)
#define  SOR_STATE1_ASY_SUBOWNER_MASK (0xf << 4)
#define  SOR_STATE1_ASY_OWNER_MASK (0xf << 0)
#define  SOR_STATE1_ASY_OWNER(x) (((x) & 0xf) << 0)

#define SOR_CRC_CNTRL 0x044
#define  SOR_CRC_CNTRL_ENABLE (1 << 0)

#define SOR_DP_DEBUG_MVID 0x048

#define SOR_CLK_CNTRL 0x04c
#define  SOR_CLK_CNTRL_DP_LINK_SPEED_MASK	(0x1f << 2)
#define  SOR_CLK_CNTRL_DP_LINK_SPEED(x)		(((x) & 0x1f) << 2)
#define  SOR_CLK_CNTRL_DP_CLK_SEL_MASK		(3 << 0)
#define  SOR_CLK_CNTRL_DP_CLK_SEL_SINGLE_PCLK	(0 << 0)
#define  SOR_CLK_CNTRL_DP_CLK_SEL_DIFF_PCLK	(1 << 0)
#define  SOR_CLK_CNTRL_DP_CLK_SEL_SINGLE_DPCLK	(2 << 0)
#define  SOR_CLK_CNTRL_DP_CLK_SEL_DIFF_DPCLK	(3 << 0)

#define SOR_CAP 0x050

#define SOR_PWR 0x054
#define  SOR_PWR_TRIGGER (1 << 31)
#define  SOR_PWR_MODE_SAFE (1 << 28)
#define  SOR_PWR_NORMAL_STATE_PU (1 << 0)

#define SOR_TEST 0x058
#define  SOR_TEST_ATTACHED (1 << 10)
#define  SOR_TEST_ACT_HEAD_OPMODE_MASK (0x3 << 8)
#define  SOR_TEST_ACT_HEAD_OPMODE_SLEEP (0 << 8)
#define  SOR_TEST_ACT_HEAD_OPMODE_SNOOZE (1 << 8)
#define  SOR_TEST_ACT_HEAD_OPMODE_AWAKE (2 << 8)

#define SOR_CSTM 0x06c
#define SOR_LVDS 0x070
#define SOR_CRCA 0x074
#define SOR_CRCB 0x078
#define SOR_BLANK 0x07c

#define SOR_SEQ_CTL 0x080
#define  SOR_SEQ_CTL_SWITCH		(1 << 30)
#define  SOR_SEQ_CTL_STATUS		(1 << 28)
#define  SOR_SEQ_CTL_PD_PC_ALT(x)	(((x) & 0xf) << 12)
#define  SOR_SEQ_CTL_PD_PC(x)		(((x) & 0xf) <<  8)
#define  SOR_SEQ_CTL_PU_PC_ALT(x)	(((x) & 0xf) <<  4)
#define  SOR_SEQ_CTL_PU_PC(x)		(((x) & 0xf) <<  0)

#define SOR_LANE_SEQ_CTL 0x084
#define  SOR_LANE_SEQ_CTL_TRIGGER		(1 << 31)
#define  SOR_LANE_SEQ_CTL_STATE_BUSY		(1 << 28)
#define  SOR_LANE_SEQ_CTL_SEQUENCE_UP		(0 << 20)
#define  SOR_LANE_SEQ_CTL_SEQUENCE_DOWN		(1 << 20)
#define  SOR_LANE_SEQ_CTL_POWER_STATE_UP	(0 << 16)
#define  SOR_LANE_SEQ_CTL_POWER_STATE_DOWN	(1 << 16)
#define  SOR_LANE_SEQ_CTL_DELAY(x)		(((x) & 0xf) << 12)

#define SOR_SEQ_INST(x) (0x088 + ((x) * 4))
#define  SOR_SEQ_INST_DRIVE_PWM_OUT_LO (1 << 23)
#define  SOR_SEQ_INST_HALT (1 << 15)
#define  SOR_SEQ_INST_WAIT_VSYNC (2 << 12)
#define  SOR_SEQ_INST_WAIT_MS (1 << 12)
#define  SOR_SEQ_INST_WAIT_US (0 << 12)
#define  SOR_SEQ_INST_WAIT(x) (((x) & 0x1ff) << 0)

#define SOR_PWM_DIV 0x0c8
#define SOR_PWM_CTL 0x0cc

#define SOR_VCRCA0 0xd0
#define SOR_VCRCA1 0xd4
#define SOR_VCRCB0 0xd8
#define SOR_VCRCB1 0xdc
#define SOR_CCRCA0 0xe0
#define SOR_CCRCA1 0xe4
#define SOR_CCRCB0 0xe8
#define SOR_CCRCB1 0xec

#define SOR_EDATAA0 0x0f0
#define SOR_EDATAA1 0x0f4
#define SOR_EDATAB0 0x0f8
#define SOR_EDATAB1 0x0fc

#define SOR_COUNTA0 0x100
#define SOR_COUNTA1 0x104
#define SOR_COUNTB0 0x108
#define SOR_COUNTB1 0x10c
#define SOR_DEBUGA0 0x110
#define SOR_DEBUGA1 0x114
#define SOR_DEBUGB0 0x118
#define SOR_DEBUGB1 0x11c

#define SOR_TRIG 0x120
#define SOR_MSCHECK 0x124

#define SOR_XBAR_CTRL 0x128
#define  SOR_XBAR_CTRL_LINK1_XSEL(input, output) ((((output) & 0x7) << ((input) * 3)) << 17)
#define  SOR_XBAR_CTRL_LINK0_XSEL(input, output) ((((output) & 0x7) << ((input) * 3)) <<  2)
#define  SOR_XBAR_CTRL_LINK_SWAP (1 << 1)
#define  SOR_XBAR_CTRL_BYPASS (1 << 0)

#define SOR_XBAR_POL 0x12c
#define  SOR_XBAR_POL_LINK1_INVERT(x) (1 << (5 + (x)))
#define  SOR_XBAR_POL_LINK0_INVERT(x) (1 << (0 + (x)))

#define SOR_DP_LINKCTL(x) (0x130 + (x) * 4)

#define SOR_LANE_DRIVE_CURRENT(x) (0x138 + (x) * 4)
#define SOR_LANE4_DRIVE_CURRENT(x) (0x140 + (x) * 4)

#define SOR_LANE_PREEMPHASIS(x) (0x148 + (x) * 4)
#define SOR_LANE4_PREEMPHASIS(x) (0x150 + (x) * 4)

#define SOR_POSTCURSOR(x) (0x158 + (x) * 4)

#define SOR_DP_CONFIG(x) (0x160 + (x) * 4)

#define SOR_DP_MN(x) (0x168 + (x) * 4)

#define SOR_DP_DEBUG(x) (0x178 + (x) * 4)

#define SOR_DP_SPARE(x) (0x180 + (x) * 4)
#define  SOR_DP_SPARE0_DISP_VIDEO_PREAMBLE (1 << 3)
#define  SOR_DP_SPARE0_SOR_CLK_SEL_MACRO (1 << 2)
#define  SOR_DP_SPARE0_PANEL_INTERNAL (1 << 1)
#define  SOR_DP_SPARE0_SEQ_ENABLE (1 << 0)

#define SOR_DP_AUDIO_CTRL 0x188
#define SOR_DP_AUDIO_HBLANK_SYMBOLS 0x18c
#define SOR_DP_AUDIO_VBLANK_SYMBOLS 0x190

#define SOR_DP_GENERIC_INFOFRAME_HEADER 0x194
#define SOR_DP_GENERIC_INFOFRAME_SUBPACK(x) (0x198 + (x) * 4)

#define SOR_DP_TPG 0x1b4
#define SOR_DP_TPG_CONFIG 0x1b8

#define SOR_DP_LQ_CSTM(x) (0x1bc + (x) * 4)

#define SOR_DP_HDCP_AN_MSB 0x1d4
#define SOR_DP_HDCP_AN_LSB 0x1d8
#define SOR_DP_HDCP_AKSV_MSB 0x1dc
#define SOR_DP_HDCP_AKSV_LSB 0x1e0
#define SOR_DP_HDCP_BKSV_MSB 0x1e4
#define SOR_DP_HDCP_BKSV_LSB 0x1e8
#define SOR_DP_HDCP_CTRL 0x1ec
#define SOR_DP_HDCP_RI 0x1f0
#define SOR_DP_HDCP_EMU(x) (0x1f4 + (x) * 4)
#define SOR_DP_HDCP_CYA 0x1fc

#define SOR_TMDS_HDCP_AN_MSB 0x200
#define SOR_TMDS_HDCP_AN_LSB 0x204
#define SOR_TMDS_HDCP_CN_MSB 0x208
#define SOR_TMDS_HDCP_CN_LSB 0x20c
#define SOR_TMDS_HDCP_AKSV_MSB 0x210
#define SOR_TMDS_HDCP_AKSV_LSB 0x214
#define SOR_TMDS_HDCP_BKSV_MSB 0x218
#define SOR_TMDS_HDCP_BKSV_LSB 0x21c
#define SOR_TMDS_HDCP_CKSV_MSB 0x220
#define SOR_TMDS_HDCP_CKSV_LSB 0x224
#define SOR_TMDS_HDCP_DKSV_MSB 0x228
#define SOR_TMDS_HDCP_DKSV_LSB 0x22c
#define SOR_TMDS_HDCP_CTRL 0x230
#define SOR_TMDS_HDCP_CMODE 0x234
#define SOR_TMDS_HDCP_MPRIME_MSB 0x238
#define SOR_TMDS_HDCP_MPRIME_LSB 0x23c
#define SOR_TMDS_HDCP_SPRIME_MSB 0x240
#define SOR_TMDS_HDCP_SPRIME_LSB2 0x244
#define SOR_TMDS_HDCP_SPRIME_LSB1 0x248
#define SOR_TMDS_HDCP_RI 0x24c
#define SOR_TMDS_HDCP_CS_MSB 0x250
#define SOR_TMDS_HDCP_CS_LSB 0x254

#define SOR_HDMI_AUDIO_EMU0 0x258
#define SOR_HDMI_AUDIO_EMU_RDATA0 0x25c
#define SOR_HDMI_AUDIO_EMU1 0x260
#define SOR_HDMI_AUDIO_EMU2 0x264

#define SOR_HDMI_AUDIO_INFOFRAME_CTRL 0x268
#define  INFOFRAME_CTRL_CHECKSUM_ENABLE (1 << 9)
#define  INFOFRAME_CTRL_SINGLE (1 << 8)
#define  INFOFRAME_CTRL_OTHER (1 << 4)
#define  INFOFRAME_CTRL_ENABLE (1 << 0)

#define SOR_HDMI_AUDIO_INFOFRAME_STATUS 0x26c

#define SOR_HDMI_AUDIO_INFOFRAME_HEADER 0x270
#define  INFOFRAME_HEADER_LEN(x) (((x) & 0xff) << 16)
#define  INFOFRAME_HEADER_VERSION(x) (((x) & 0xff) << 8)
#define  INFOFRAME_HEADER_TYPE(x) (((x) & 0xff) << 0)

#define SOR_HDMI_AUDIO_INFOFRAME_SUBPACK0_LOW 0x274
#define SOR_HDMI_AUDIO_INFOFRAME_SUBPACK0_HIGH 0x278

#define SOR_HDMI_AVI_INFOFRAME_CTRL 0x27c
#define SOR_HDMI_AVI_INFOFRAME_STATUS 0x280
#define SOR_HDMI_AVI_INFOFRAME_HEADER 0x284
#define SOR_HDMI_AVI_INFOFRAME_SUBPACK0_LOW 0x288
#define SOR_HDMI_AVI_INFOFRAME_SUBPACK0_HIGH 0x28c
#define SOR_HDMI_AVI_INFOFRAME_SUBPACK1_LOW 0x290
#define SOR_HDMI_AVI_INFOFRAME_SUBPACK1_HIGH 0x294

#define SOR_HDMI_GENERIC_CTRL 0x298
#define SOR_HDMI_GENERIC_STATUS 0x29c
#define SOR_HDMI_GENERIC_HEADER 0x2a0
#define SOR_HDMI_GENERIC_SUBPACK0_LOW 0x2a4
#define SOR_HDMI_GENERIC_SUBPACK0_HIGH 0x2a8
#define SOR_HDMI_GENERIC_SUBPACK1_LOW 0x2ac
#define SOR_HDMI_GENERIC_SUBPACK1_HIGH 0x2b0
#define SOR_HDMI_GENERIC_SUBPACK2_LOW 0x2b4
#define SOR_HDMI_GENERIC_SUBPACK2_HIGH 0x2b8
#define SOR_HDMI_GENERIC_SUBPACK3_LOW 0x2bc
#define SOR_HDMI_GENERIC_SUBPACK3_HIGH 0x2c0

#define SOR_HDMI_ACR_CTRL 0x2c4
#define SOR_HDMI_ACR_0320_SUBPACK_LOW 0x2c8
#define SOR_HDMI_ACR_0320_SUBPACK_HIGH 0x2cc
#define SOR_HDMI_ACR_0441_SUBPACK_LOW 0x2d0
#define SOR_HDMI_ACR_0441_SUBPACK_HIGH 0x2d4
#define SOR_HDMI_ACR_0882_SUBPACK_LOW 0x2d8
#define SOR_HDMI_ACR_0882_SUBPACK_HIGH 0x2dc
#define SOR_HDMI_ACR_1764_SUBPACK_LOW 0x2e0
#define SOR_HDMI_ACR_1764_SUBPACK_HIGH 0x2e4
#define SOR_HDMI_ACR_0480_SUBPACK_LOW 0x2e8
#define SOR_HDMI_ACR_0480_SUBPACK_HIGH 0x2ec
#define SOR_HDMI_ACR_0960_SUBPACK_LOW 0x2f0
#define SOR_HDMI_ACR_0960_SUBPACK_HIGH 0x2f4
#define SOR_HDMI_ACR_1920_SUBPACK_LOW 0x2f8
#define SOR_HDMI_ACR_1920_SUBPACK_HIGH 0x2fc

#define SOR_HDMI_CTRL 0x300
#define  SOR_HDMI_CTRL_ENABLE (1 << 30)
#define  SOR_HDMI_CTRL_MAX_AC_PACKET(x) (((x) & 0x1f) << 16)
#define  SOR_HDMI_CTRL_AUDIO_LAYOUT (1 << 10)
#define  SOR_HDMI_CTRL_REKEY(x) (((x) & 0x7f) << 0)

#define SOR_HDMI_VSYNC_KEEPOUT 0x304
#define SOR_HDMI_VSYNC_WINDOW 0x308

#define SOR_HDMI_GCP_CTRL 0x30c
#define SOR_HDMI_GCP_STATUS 0x310
#define SOR_HDMI_GCP_SUBPACK 0x314

#define SOR_HDMI_CHANNEL_STATUS1 0x318
#define SOR_HDMI_CHANNEL_STATUS2 0x31c

#define SOR_HDMI_EMU0 0x320
#define SOR_HDMI_EMU1 0x324
#define SOR_HDMI_EMU1_RDATA 0x328

#define SOR_HDMI_SPARE 0x32c
#define SOR_HDMI_SPDIF_CHN_STATUS1 0x330
#define SOR_HDMI_SPDIF_CHN_STATUS2 0x334

#define SOR_REFCLK 0x398
#define  SOR_REFCLK_DIV_INT(x) (((x) & 0xff) << 8)
#define  SOR_REFCLK_DIV_FRAC(x) (((x) & 0x3) << 6)

#define SOR_CRC_CONTROL 0x39c
#define SOR_INPUT_CONTROL 0x3a0
#define SOR_SCRATCH 0x3a4
#define SOR_KEY_CTRL 0x3a8
#define SOR_KEY_DEBUG(x) (0x3ac + (x) * 4)
#define SOR_KEY_HDCP_KEY(x) (0x3b8 + (x) * 4)
#define SOR_KEY_HDCP_KEY_TRIG 0x3c8
#define SOR_KEY_SKEY_INDEX 0x3cc

#define SOR_AUDIO_CNTRL 0x3f0
#define SOR_AUDIO_DEBUG 0x3f4
#define SOR_AUDIO_SPARE 0x3f8
#define SOR_AUDIO_NVAL_0320 0x3fc
#define SOR_AUDIO_NVAL_0441 0x400
#define SOR_AUDIO_NVAL_0882 0x404
#define SOR_AUDIO_NVAL_1764 0x408
#define SOR_AUDIO_NVAL_0480 0x40c
#define SOR_AUDIO_NVAL_0960 0x410
#define SOR_AUDIO_NVAL_1920 0x414

#define SOR_AUDIO_HDA_SCRATCH(x) (0x418 + (x) * 4)
#define SOR_AUDIO_HDA_CODEC_SCRATCH(x) (0x428 + (x) * 4)
#define SOR_AUDIO_HDA_ELD_BUFWR 0x430
#define SOR_AUDIO_HDA_PRESENSE 0x434
#define SOR_AUDIO_HDA_CP 0x438

#define SOR_AUDIO_AVAL_0320 0x43c
#define SOR_AUDIO_AVAL_0441 0x440
#define SOR_AUDIO_AVAL_0882 0x444
#define SOR_AUDIO_AVAL_1764 0x448
#define SOR_AUDIO_AVAL_0480 0x44c
#define SOR_AUDIO_AVAL_0960 0x450
#define SOR_AUDIO_AVAL_1920 0x454
#define SOR_AUDIO_AVAL_DEFAULT 0x458
#define SOR_AUDIO_GEN_CTRL 0x45c

#define SOR_INT_STATUS 0x470
#define SOR_INT_MASK 0x474
#define SOR_INT_ENABLE 0x478

#define SOR_TMDS_HDCP_M0_LO 0x47c
#define SOR_TMDS_HDCP_M0_HI 0x480
#define SOR_TMDS_HDCP_STATUS 0x484
#define SOR_HDACODEC_AUDIO_GEN_CTL 0x488

#define SOR_HDMI_VSI_INFOFRAME_CTRL 0x48c
#define SOR_HDMI_VSI_INFOFRAME_STATUS 0x490
#define SOR_HDMI_VSI_INFOFRAME_HEADER 0x494
#define SOR_HDMI_VSI_INFOFRAME_SUBPACK0_LOW 0x498
#define SOR_HDMI_VSI_INFOFRAME_SUBPACK0_HIGH 0x49c
#define SOR_HDMI_VSI_INFOFRAME_SUBPACK1_LOW 0x4a0
#define SOR_HDMI_VSI_INFOFRAME_SUBPACK1_HIGH 0x4a4
#define SOR_HDMI_VSI_INFOFRAME_SUBPACK2_LOW 0x4a8
#define SOR_HDMI_VSI_INFOFRAME_SUBPACK2_HIGH 0x4ac
#define SOR_HDMI_VSI_INFOFRAME_SUBPACK3_LOW 0x4b0
#define SOR_HDMI_VSI_INFOFRAME_SUBPACK3_HIGH 0x4b4

#define SOR_DP_AUDIO_CRC 0x4b8
#define SOR_DP_AUDIO_MN 0x4bc

#define SOR_HEAD_STATE0(x) (0x544 + (x) * 4)
#define  SOR_HEAD_STATE0_INTERLACED (1 << 4)
#define  SOR_HEAD_STATE0_RANGECOMPRESS (1 << 3)
#define  SOR_HEAD_STATE0_DYNRANGE_MASK (0x1 << 2)
#define  SOR_HEAD_STATE0_DYNRANGE_VESA (0 << 2)
#define  SOR_HEAD_STATE0_DYNRANGE_CEA (1 << 2)
#define  SOR_HEAD_STATE0_COLORSPACE_MASK (0x3 << 0)
#define  SOR_HEAD_STATE0_COLORSPACE_RGB (0 << 0)
#define  SOR_HEAD_STATE0_COLORSPACE_YUV_601 (1 << 0)
#define  SOR_HEAD_STATE0_COLORSPACE_YUV_709 (2 << 0)

#define SOR_HEAD_STATE1(x) (0x550 + (x) * 4)
#define SOR_HEAD_STATE2(x) (0x55c + (x) * 4)
#define SOR_HEAD_STATE3(x) (0x568 + (x) * 4)
#define SOR_HEAD_STATE4(x) (0x574 + (x) * 4)
#define SOR_HEAD_STATE5(x) (0x580 + (x) * 4)

#define SOR_PLL0 0x58c
#define  SOR_PLL0_ICHPMP_MASK (0xf << 24)
#define  SOR_PLL0_ICHPMP(x) (((x) & 0xf) << 24)
#define  SOR_PLL0_FILTER_MASK (0xf << 16)
#define  SOR_PLL0_FILTER(x) (((x) & 0xf) << 16)
#define  SOR_PLL0_VCOCAP_MASK (0xf << 8)
#define  SOR_PLL0_VCOCAP(x) (((x) & 0xf) << 8)
#define  SOR_PLL0_VCOPD BIT(2)
#define  SOR_PLL0_PWR BIT(0)

#define SOR_PLL1 0x590
#define  SOR_PLL1_LOADADJ_MASK (0xf << 20)
#define  SOR_PLL1_LOADADJ(x) (((x) & 0xf) << 20)
#define  SOR_PLL1_TMDS_TERMADJ_MASK (0xf << 9)
#define  SOR_PLL1_TMDS_TERMADJ(x) (((x) & 0xf) << 9)
#define  SOR_PLL1_TMDS_TERM (1 << 8)

#define SOR_PLL2 0x594
#define  SOR_PLL2_SEQ_PLLCAPPD_ENFORCE BIT(24)
#define  SOR_PLL2_PORT_POWERDOWN BIT(23)
#define  SOR_PLL2_BANDGAP_POWERDOWN BIT(22)
#define  SOR_PLL2_POWERDOWN_OVERRIDE BIT(18)
#define  SOR_PLL2_SEQ_PLLCAPPD BIT(17)

#define SOR_PLL3 0x598
#define  SOR_PLL3_BG_TEMP_COEF_MASK (0xf << 28)
#define  SOR_PLL3_BG_TEMP_COEF(x) (((x) & 0xf) << 28)
#define  SOR_PLL3_BG_VREF_LEVEL_MASK (0xf << 24)
#define  SOR_PLL3_BG_VREF_LEVEL(x) (((x) & 0xf) << 24)
#define  SOR_PLL3_PLLVDD_MODE BIT(13)
#define  SOR_PLL3_AVDD10_LEVEL_MASK (0xf << 8)
#define  SOR_PLL3_AVDD10_LEVEL(x) (((x) & 0xf) << 8)
#define  SOR_PLL3_AVDD14_LEVEL_MASK (0xf << 4)
#define  SOR_PLL3_AVDD14_LEVEL(x) (((x) & 0xf) << 4)

#define SOR_DP_PADCTL(x) (0x5a0 + (x) * 4)
#define  SOR_DP_PADCTL_PAD_CAL_PD BIT(23)
#define  SOR_DP_PADCTL_TX_PU_ENABLE BIT(22)
#define  SOR_DP_PADCTL_TX_PU_VALUE_MASK (0xff << 8)
#define  SOR_DP_PADCTL_TX_PU_VALUE(x) (((x) & 0xff) << 8)
#define  SOR_DP_PADCTL_PD_TXD_3 BIT(3)
#define  SOR_DP_PADCTL_PD_TXD_0 BIT(2)
#define  SOR_DP_PADCTL_PD_TXD_1 BIT(1)
#define  SOR_DP_PADCTL_PD_TXD_2 BIT(0)

#define SOR_DP_PADCTL2 0x5a8
#define  SOR_DP_PADCTL2_SPAREPLL_MASK (0xff << 24)
#define  SOR_DP_PADCTL2_SPAREPLL(x) (((x) & 0xff) << 24)

/* constants */
#define SOR_REKEY 0x38

struct tegra_sor_hdmi_settings {
	unsigned long frequency;
	u8 drive_current[4];
	u8 preemphasis[4];
	u8 vcocap;
	u8 filter;
	u8 ichpmp;
	u8 tmds_termadj;
	u8 loadadj;
	u8 avdd10_level;
	u8 avdd14_level;
	u8 bg_vref_level;
	u8 bg_temp_coef;
	u8 tx_pu_value;
	u8 sparepll;
};

static const struct tegra_sor_hdmi_settings tegra186_sor_hdmi_defaults[] = {
	{
		.frequency = 54000000,
		.drive_current = { 0x3a, 0x3a, 0x3a, 0x33 },
		.preemphasis = { 0x00, 0x00, 0x00, 0x00 },
		.vcocap = 0,
		.filter = 5,
		.ichpmp = 5,
		.tmds_termadj = 15,
		.loadadj = 3,
		.avdd10_level = 4,
		.avdd14_level = 4,
		.bg_vref_level = 8,
		.bg_temp_coef = 3,
		.tx_pu_value = 0,
		.sparepll = 0x54,
	}, {
		.frequency = 75000000,
		.drive_current = { 0x3a, 0x3a, 0x3a, 0x33 },
		.preemphasis = { 0x00, 0x00, 0x00, 0x00 },
		.vcocap = 1,
		.filter = 5,
		.ichpmp = 5,
		.tmds_termadj = 15,
		.loadadj = 3,
		.avdd10_level = 4,
		.avdd14_level = 4,
		.bg_vref_level = 8,
		.bg_temp_coef = 3,
		.tx_pu_value = 0,
		.sparepll = 0x44,
	}, {
		.frequency = 150000000,
		.drive_current = { 0x3a, 0x3a, 0x3a, 0x37 },
		.preemphasis = { 0x00, 0x00, 0x00, 0x00 },
		.vcocap = 3,
		.filter = 5,
		.ichpmp = 5,
		.tmds_termadj = 15,
		.loadadj = 3,
		.avdd10_level = 4,
		.avdd14_level = 4,
		.bg_vref_level = 8,
		.bg_temp_coef = 3,
		.tx_pu_value = 0x66 /*0*/,
		.sparepll = 0x00 /*0x34*/,
	}, {
		.frequency = 300000000,
		.drive_current = { 0x3d, 0x3d, 0x3d, 0x33 },
		.preemphasis = { 0x00, 0x00, 0x00, 0x00 },
		.vcocap = 3,
		.filter = 5,
		.ichpmp = 5,
		.tmds_termadj = 15,
		.loadadj = 3,
		.avdd10_level = 4,
		.avdd14_level = 4,
		.bg_vref_level = 8,
		.bg_temp_coef = 3,
		.tx_pu_value = 64,
		.sparepll = 0x34,
	}, {
		.frequency = 600000000,
		.drive_current = { 0x3d, 0x3d, 0x3d, 0x33 },
		.preemphasis = { 0x00, 0x00, 0x00, 0x00 },
		.vcocap = 3,
		.filter = 5,
		.ichpmp = 5,
		.tmds_termadj = 12,
		.loadadj = 3,
		.avdd10_level = 4,
		.avdd14_level = 4,
		.bg_vref_level = 8,
		.bg_temp_coef = 3,
		.tx_pu_value = 96,
		.sparepll = 0x34,
	}
};

struct tegra_sor_soc {
	unsigned int num_settings;
	const struct tegra_sor_hdmi_settings *settings;
	const u8 *xbar_cfg;
};

struct tegra_sor {
	struct host1x_client client;
	struct drm_encoder encoder;
	struct drm_connector connector;

	const struct tegra_sor_soc *soc;
	void __iomem *regs;

	struct clk *clk;
	struct clk *clk_out;
	struct clk *clk_pad;
	struct clk *clk_safe;
	struct clk *clk_parent;
	struct clk *clk_dp;

	struct reset_control *rst;

	struct device *dev;

	struct regulator *hdmi_supply;

	struct gpio_desc *hpd;
	unsigned int hpd_irq;

	struct i2c_adapter *ddc;

#ifdef CONFIG_DEBUG_FS
	struct {
		struct dentry *regs;
	} debugfs;
#endif
};

static inline struct tegra_sor *to_sor(struct host1x_client *client)
{
	return container_of(client, struct tegra_sor, client);
}

static inline struct tegra_sor *con_to_sor(struct drm_connector *connector)
{
	return container_of(connector, struct tegra_sor, connector);
}

static inline struct tegra_sor *enc_to_sor(struct drm_encoder *encoder)
{
	return container_of(encoder, struct tegra_sor, encoder);
}

struct tegra_sor_state {
	struct drm_connector_state base;

	unsigned int link_speed;
	unsigned long pclk;
	unsigned int bpc;
};

static inline struct tegra_sor_state *
to_sor_state(struct drm_connector_state *state)
{
	return container_of(state, struct tegra_sor_state, base);
}

static void tegra_sor_connector_reset(struct drm_connector *connector)
{
	struct tegra_sor *sor = con_to_sor(connector);
	struct tegra_sor_state *state;

	dev_dbg(sor->dev, "> %s(connector=%p)\n", __func__, connector);

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return;

	if (connector->state) {
		__drm_atomic_helper_connector_destroy_state(connector->state);
		kfree(connector->state);
	}

	__drm_atomic_helper_connector_reset(connector, &state->base);

	dev_dbg(sor->dev, "< %s()\n", __func__);
}

static enum drm_connector_status
tegra_sor_connector_detect(struct drm_connector *connector, bool force)
{
	struct tegra_sor *sor = con_to_sor(connector);
	enum drm_connector_status status;
	int value;

	dev_dbg(sor->dev, "> %s(connector=%p, force=%d)\n", __func__, connector, force);

	value = gpiod_get_value(sor->hpd);
	dev_dbg(sor->dev, "  value: %d\n", value);

	if (value == 0)
		status = connector_status_disconnected;
	else
		status = connector_status_connected;

	dev_dbg(sor->dev, "< %s() = %d\n", __func__, status);
	return status;
}

#ifdef CONFIG_DEBUG_FS
static const struct {
	unsigned int offset;
	const char *name;
} tegra_sor_regs[] = {
#define TEGRA_SOR_REGISTER(reg) { .offset = reg, .name = #reg }
	TEGRA_SOR_REGISTER(SOR_SUPER_STATE0),
	TEGRA_SOR_REGISTER(SOR_SUPER_STATE1),
	TEGRA_SOR_REGISTER(SOR_STATE0),
	TEGRA_SOR_REGISTER(SOR_STATE1),
	TEGRA_SOR_REGISTER(SOR_CRC_CNTRL),
	TEGRA_SOR_REGISTER(SOR_DP_DEBUG_MVID),
	TEGRA_SOR_REGISTER(SOR_CLK_CNTRL),
	TEGRA_SOR_REGISTER(SOR_CAP),
	TEGRA_SOR_REGISTER(SOR_PWR),
	TEGRA_SOR_REGISTER(SOR_TEST),
	TEGRA_SOR_REGISTER(SOR_CSTM),
	TEGRA_SOR_REGISTER(SOR_LVDS),
	TEGRA_SOR_REGISTER(SOR_CRCA),
	TEGRA_SOR_REGISTER(SOR_CRCB),
	TEGRA_SOR_REGISTER(SOR_BLANK),
	TEGRA_SOR_REGISTER(SOR_SEQ_CTL),
	TEGRA_SOR_REGISTER(SOR_LANE_SEQ_CTL),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(0)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(1)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(2)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(3)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(4)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(5)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(6)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(7)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(8)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(9)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(10)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(11)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(12)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(13)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(14)),
	TEGRA_SOR_REGISTER(SOR_SEQ_INST(15)),
	TEGRA_SOR_REGISTER(SOR_PWM_DIV),
	TEGRA_SOR_REGISTER(SOR_PWM_CTL),
	TEGRA_SOR_REGISTER(SOR_VCRCA0),
	TEGRA_SOR_REGISTER(SOR_VCRCA1),
	TEGRA_SOR_REGISTER(SOR_VCRCB0),
	TEGRA_SOR_REGISTER(SOR_VCRCB1),
	TEGRA_SOR_REGISTER(SOR_CCRCA0),
	TEGRA_SOR_REGISTER(SOR_CCRCA1),
	TEGRA_SOR_REGISTER(SOR_CCRCB0),
	TEGRA_SOR_REGISTER(SOR_CCRCB1),
	TEGRA_SOR_REGISTER(SOR_EDATAA0),
	TEGRA_SOR_REGISTER(SOR_EDATAA1),
	TEGRA_SOR_REGISTER(SOR_EDATAB0),
	TEGRA_SOR_REGISTER(SOR_EDATAB1),
	TEGRA_SOR_REGISTER(SOR_COUNTA0),
	TEGRA_SOR_REGISTER(SOR_COUNTA1),
	TEGRA_SOR_REGISTER(SOR_COUNTB0),
	TEGRA_SOR_REGISTER(SOR_COUNTB1),
	TEGRA_SOR_REGISTER(SOR_DEBUGA0),
	TEGRA_SOR_REGISTER(SOR_DEBUGA1),
	TEGRA_SOR_REGISTER(SOR_DEBUGB0),
	TEGRA_SOR_REGISTER(SOR_DEBUGB1),
	TEGRA_SOR_REGISTER(SOR_TRIG),
	TEGRA_SOR_REGISTER(SOR_MSCHECK),
	TEGRA_SOR_REGISTER(SOR_XBAR_CTRL),
	TEGRA_SOR_REGISTER(SOR_XBAR_POL),
	TEGRA_SOR_REGISTER(SOR_DP_LINKCTL(0)),
	TEGRA_SOR_REGISTER(SOR_DP_LINKCTL(1)),
	TEGRA_SOR_REGISTER(SOR_LANE_DRIVE_CURRENT(0)),
	TEGRA_SOR_REGISTER(SOR_LANE_DRIVE_CURRENT(1)),
	TEGRA_SOR_REGISTER(SOR_LANE4_DRIVE_CURRENT(0)),
	TEGRA_SOR_REGISTER(SOR_LANE4_DRIVE_CURRENT(1)),
	TEGRA_SOR_REGISTER(SOR_LANE_PREEMPHASIS(0)),
	TEGRA_SOR_REGISTER(SOR_LANE_PREEMPHASIS(1)),
	TEGRA_SOR_REGISTER(SOR_LANE4_PREEMPHASIS(0)),
	TEGRA_SOR_REGISTER(SOR_LANE4_PREEMPHASIS(1)),
	TEGRA_SOR_REGISTER(SOR_POSTCURSOR(0)),
	TEGRA_SOR_REGISTER(SOR_POSTCURSOR(1)),
	TEGRA_SOR_REGISTER(SOR_DP_CONFIG(0)),
	TEGRA_SOR_REGISTER(SOR_DP_CONFIG(1)),
	TEGRA_SOR_REGISTER(SOR_DP_MN(0)),
	TEGRA_SOR_REGISTER(SOR_DP_MN(1)),
	TEGRA_SOR_REGISTER(SOR_DP_DEBUG(0)),
	TEGRA_SOR_REGISTER(SOR_DP_DEBUG(1)),
	TEGRA_SOR_REGISTER(SOR_DP_SPARE(0)),
	TEGRA_SOR_REGISTER(SOR_DP_SPARE(1)),
	TEGRA_SOR_REGISTER(SOR_DP_AUDIO_CTRL),
	TEGRA_SOR_REGISTER(SOR_DP_AUDIO_HBLANK_SYMBOLS),
	TEGRA_SOR_REGISTER(SOR_DP_AUDIO_VBLANK_SYMBOLS),
	TEGRA_SOR_REGISTER(SOR_DP_GENERIC_INFOFRAME_HEADER),
	TEGRA_SOR_REGISTER(SOR_DP_GENERIC_INFOFRAME_SUBPACK(0)),
	TEGRA_SOR_REGISTER(SOR_DP_GENERIC_INFOFRAME_SUBPACK(1)),
	TEGRA_SOR_REGISTER(SOR_DP_GENERIC_INFOFRAME_SUBPACK(2)),
	TEGRA_SOR_REGISTER(SOR_DP_GENERIC_INFOFRAME_SUBPACK(3)),
	TEGRA_SOR_REGISTER(SOR_DP_GENERIC_INFOFRAME_SUBPACK(4)),
	TEGRA_SOR_REGISTER(SOR_DP_GENERIC_INFOFRAME_SUBPACK(5)),
	TEGRA_SOR_REGISTER(SOR_DP_GENERIC_INFOFRAME_SUBPACK(6)),
	TEGRA_SOR_REGISTER(SOR_DP_TPG),
	TEGRA_SOR_REGISTER(SOR_DP_TPG_CONFIG),
	TEGRA_SOR_REGISTER(SOR_DP_LQ_CSTM(0)),
	TEGRA_SOR_REGISTER(SOR_DP_LQ_CSTM(1)),
	TEGRA_SOR_REGISTER(SOR_DP_LQ_CSTM(2)),
	TEGRA_SOR_REGISTER(SOR_DP_HDCP_AN_MSB),
	TEGRA_SOR_REGISTER(SOR_DP_HDCP_AN_LSB),
	TEGRA_SOR_REGISTER(SOR_DP_HDCP_AKSV_MSB),
	TEGRA_SOR_REGISTER(SOR_DP_HDCP_AKSV_LSB),
	TEGRA_SOR_REGISTER(SOR_DP_HDCP_BKSV_MSB),
	TEGRA_SOR_REGISTER(SOR_DP_HDCP_BKSV_LSB),
	TEGRA_SOR_REGISTER(SOR_DP_HDCP_CTRL),
	TEGRA_SOR_REGISTER(SOR_DP_HDCP_RI),
	TEGRA_SOR_REGISTER(SOR_DP_HDCP_EMU(0)),
	TEGRA_SOR_REGISTER(SOR_DP_HDCP_EMU(1)),
	TEGRA_SOR_REGISTER(SOR_DP_HDCP_CYA),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_AN_MSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_AN_LSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_CN_MSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_CN_LSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_AKSV_MSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_AKSV_LSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_BKSV_MSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_BKSV_LSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_CKSV_MSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_CKSV_LSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_DKSV_MSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_DKSV_LSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_CTRL),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_CMODE),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_MPRIME_MSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_MPRIME_LSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_SPRIME_MSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_SPRIME_LSB2),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_SPRIME_LSB1),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_RI),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_CS_MSB),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_CS_LSB),
	TEGRA_SOR_REGISTER(SOR_HDMI_AUDIO_EMU0),
	TEGRA_SOR_REGISTER(SOR_HDMI_AUDIO_EMU_RDATA0),
	TEGRA_SOR_REGISTER(SOR_HDMI_AUDIO_EMU1),
	TEGRA_SOR_REGISTER(SOR_HDMI_AUDIO_EMU2),
	TEGRA_SOR_REGISTER(SOR_HDMI_AUDIO_INFOFRAME_CTRL),
	TEGRA_SOR_REGISTER(SOR_HDMI_AUDIO_INFOFRAME_STATUS),
	TEGRA_SOR_REGISTER(SOR_HDMI_AUDIO_INFOFRAME_HEADER),
	TEGRA_SOR_REGISTER(SOR_HDMI_AUDIO_INFOFRAME_SUBPACK0_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_AUDIO_INFOFRAME_SUBPACK0_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_AVI_INFOFRAME_CTRL),
	TEGRA_SOR_REGISTER(SOR_HDMI_AVI_INFOFRAME_STATUS),
	TEGRA_SOR_REGISTER(SOR_HDMI_AVI_INFOFRAME_HEADER),
	TEGRA_SOR_REGISTER(SOR_HDMI_AVI_INFOFRAME_SUBPACK0_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_AVI_INFOFRAME_SUBPACK0_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_AVI_INFOFRAME_SUBPACK1_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_AVI_INFOFRAME_SUBPACK1_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_GENERIC_CTRL),
	TEGRA_SOR_REGISTER(SOR_HDMI_GENERIC_STATUS),
	TEGRA_SOR_REGISTER(SOR_HDMI_GENERIC_HEADER),
	TEGRA_SOR_REGISTER(SOR_HDMI_GENERIC_SUBPACK0_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_GENERIC_SUBPACK0_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_GENERIC_SUBPACK1_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_GENERIC_SUBPACK1_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_GENERIC_SUBPACK2_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_GENERIC_SUBPACK2_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_GENERIC_SUBPACK3_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_GENERIC_SUBPACK3_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_CTRL),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_0320_SUBPACK_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_0320_SUBPACK_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_0441_SUBPACK_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_0441_SUBPACK_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_0882_SUBPACK_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_0882_SUBPACK_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_1764_SUBPACK_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_1764_SUBPACK_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_0480_SUBPACK_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_0480_SUBPACK_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_0960_SUBPACK_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_0960_SUBPACK_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_1920_SUBPACK_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_ACR_1920_SUBPACK_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_CTRL),
	TEGRA_SOR_REGISTER(SOR_HDMI_VSYNC_KEEPOUT),
	TEGRA_SOR_REGISTER(SOR_HDMI_VSYNC_WINDOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_GCP_CTRL),
	TEGRA_SOR_REGISTER(SOR_HDMI_GCP_STATUS),
	TEGRA_SOR_REGISTER(SOR_HDMI_GCP_SUBPACK),
	TEGRA_SOR_REGISTER(SOR_HDMI_CHANNEL_STATUS1),
	TEGRA_SOR_REGISTER(SOR_HDMI_CHANNEL_STATUS2),
	TEGRA_SOR_REGISTER(SOR_HDMI_EMU0),
	TEGRA_SOR_REGISTER(SOR_HDMI_EMU1),
	TEGRA_SOR_REGISTER(SOR_HDMI_EMU1_RDATA),
	TEGRA_SOR_REGISTER(SOR_HDMI_SPARE),
	TEGRA_SOR_REGISTER(SOR_HDMI_SPDIF_CHN_STATUS1),
	TEGRA_SOR_REGISTER(SOR_HDMI_SPDIF_CHN_STATUS2),
	TEGRA_SOR_REGISTER(SOR_REFCLK),
	TEGRA_SOR_REGISTER(SOR_CRC_CONTROL),
	TEGRA_SOR_REGISTER(SOR_INPUT_CONTROL),
	TEGRA_SOR_REGISTER(SOR_SCRATCH),
	TEGRA_SOR_REGISTER(SOR_KEY_CTRL),
	TEGRA_SOR_REGISTER(SOR_KEY_DEBUG(0)),
	TEGRA_SOR_REGISTER(SOR_KEY_DEBUG(1)),
	TEGRA_SOR_REGISTER(SOR_KEY_DEBUG(2)),
	TEGRA_SOR_REGISTER(SOR_KEY_HDCP_KEY(0)),
	TEGRA_SOR_REGISTER(SOR_KEY_HDCP_KEY(1)),
	TEGRA_SOR_REGISTER(SOR_KEY_HDCP_KEY(2)),
	TEGRA_SOR_REGISTER(SOR_KEY_HDCP_KEY(3)),
	TEGRA_SOR_REGISTER(SOR_KEY_HDCP_KEY_TRIG),
	TEGRA_SOR_REGISTER(SOR_KEY_SKEY_INDEX),
	TEGRA_SOR_REGISTER(SOR_AUDIO_CNTRL),
	TEGRA_SOR_REGISTER(SOR_AUDIO_DEBUG),
	TEGRA_SOR_REGISTER(SOR_AUDIO_SPARE),
	TEGRA_SOR_REGISTER(SOR_AUDIO_NVAL_0320),
	TEGRA_SOR_REGISTER(SOR_AUDIO_NVAL_0441),
	TEGRA_SOR_REGISTER(SOR_AUDIO_NVAL_0882),
	TEGRA_SOR_REGISTER(SOR_AUDIO_NVAL_1764),
	TEGRA_SOR_REGISTER(SOR_AUDIO_NVAL_0480),
	TEGRA_SOR_REGISTER(SOR_AUDIO_NVAL_0960),
	TEGRA_SOR_REGISTER(SOR_AUDIO_NVAL_1920),
	TEGRA_SOR_REGISTER(SOR_AUDIO_HDA_SCRATCH(0)),
	TEGRA_SOR_REGISTER(SOR_AUDIO_HDA_SCRATCH(1)),
	TEGRA_SOR_REGISTER(SOR_AUDIO_HDA_SCRATCH(2)),
	TEGRA_SOR_REGISTER(SOR_AUDIO_HDA_SCRATCH(3)),
	TEGRA_SOR_REGISTER(SOR_AUDIO_HDA_CODEC_SCRATCH(0)),
	TEGRA_SOR_REGISTER(SOR_AUDIO_HDA_CODEC_SCRATCH(1)),
	TEGRA_SOR_REGISTER(SOR_AUDIO_HDA_ELD_BUFWR),
	TEGRA_SOR_REGISTER(SOR_AUDIO_HDA_PRESENSE),
	TEGRA_SOR_REGISTER(SOR_AUDIO_HDA_CP),
	TEGRA_SOR_REGISTER(SOR_AUDIO_AVAL_0320),
	TEGRA_SOR_REGISTER(SOR_AUDIO_AVAL_0441),
	TEGRA_SOR_REGISTER(SOR_AUDIO_AVAL_0882),
	TEGRA_SOR_REGISTER(SOR_AUDIO_AVAL_1764),
	TEGRA_SOR_REGISTER(SOR_AUDIO_AVAL_0480),
	TEGRA_SOR_REGISTER(SOR_AUDIO_AVAL_0960),
	TEGRA_SOR_REGISTER(SOR_AUDIO_AVAL_1920),
	TEGRA_SOR_REGISTER(SOR_AUDIO_AVAL_DEFAULT),
	TEGRA_SOR_REGISTER(SOR_AUDIO_GEN_CTRL),
	TEGRA_SOR_REGISTER(SOR_INT_STATUS),
	TEGRA_SOR_REGISTER(SOR_INT_MASK),
	TEGRA_SOR_REGISTER(SOR_INT_ENABLE),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_M0_LO),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_M0_HI),
	TEGRA_SOR_REGISTER(SOR_TMDS_HDCP_STATUS),
	TEGRA_SOR_REGISTER(SOR_HDACODEC_AUDIO_GEN_CTL),
	TEGRA_SOR_REGISTER(SOR_HDMI_VSI_INFOFRAME_CTRL),
	TEGRA_SOR_REGISTER(SOR_HDMI_VSI_INFOFRAME_STATUS),
	TEGRA_SOR_REGISTER(SOR_HDMI_VSI_INFOFRAME_HEADER),
	TEGRA_SOR_REGISTER(SOR_HDMI_VSI_INFOFRAME_SUBPACK0_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_VSI_INFOFRAME_SUBPACK0_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_VSI_INFOFRAME_SUBPACK1_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_VSI_INFOFRAME_SUBPACK1_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_VSI_INFOFRAME_SUBPACK2_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_VSI_INFOFRAME_SUBPACK2_HIGH),
	TEGRA_SOR_REGISTER(SOR_HDMI_VSI_INFOFRAME_SUBPACK3_LOW),
	TEGRA_SOR_REGISTER(SOR_HDMI_VSI_INFOFRAME_SUBPACK3_HIGH),
	TEGRA_SOR_REGISTER(SOR_DP_AUDIO_CRC),
	TEGRA_SOR_REGISTER(SOR_DP_AUDIO_MN),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE0(0)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE0(1)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE0(2)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE1(0)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE1(1)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE1(2)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE2(0)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE2(1)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE2(2)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE3(0)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE3(1)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE3(2)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE4(0)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE4(1)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE4(2)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE5(0)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE5(1)),
	TEGRA_SOR_REGISTER(SOR_HEAD_STATE5(2)),
	TEGRA_SOR_REGISTER(SOR_PLL0),
	TEGRA_SOR_REGISTER(SOR_PLL1),
	TEGRA_SOR_REGISTER(SOR_PLL2),
	TEGRA_SOR_REGISTER(SOR_PLL3),
	TEGRA_SOR_REGISTER(SOR_DP_PADCTL(0)),
	TEGRA_SOR_REGISTER(SOR_DP_PADCTL(1)),
	TEGRA_SOR_REGISTER(SOR_DP_PADCTL2),
};

static int tegra_sor_regs_show(struct seq_file *s, void *data)
{
	struct tegra_sor *sor = s->private;
	unsigned int i;
	int width = 0;

	for (i = 0; i < ARRAY_SIZE(tegra_sor_regs); i++) {
		int len = strlen(tegra_sor_regs[i].name);

		if (len > width)
			width = len;
	}

	for (i = 0; i < ARRAY_SIZE(tegra_sor_regs); i++) {
		u32 value = readl(sor->regs + tegra_sor_regs[i].offset);

		seq_printf(s, "%#05x %-*s %#010x\n", tegra_sor_regs[i].offset,
			   width, tegra_sor_regs[i].name, value);
	}

	return 0;
}

static int tegra_sor_regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, tegra_sor_regs_show, inode->i_private);
}

static const struct file_operations tegra_sor_regs_ops = {
	.open = tegra_sor_regs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static int tegra_sor_connector_late_register(struct drm_connector *connector)
{
	struct dentry *root = connector->debugfs_entry;
	struct tegra_sor *sor = con_to_sor(connector);
	int err = 0;

	dev_dbg(sor->dev, "> %s(connector=%p)\n", __func__, connector);

#ifdef CONFIG_DEBUG_FS
	sor->debugfs.regs = debugfs_create_file("regs", S_IRUGO, root, sor,
						&tegra_sor_regs_ops);
	if (!sor->debugfs.regs)
		dev_err(sor->dev, "failed to create debugfs \"regs\" file\n");
#endif

	dev_dbg(sor->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static void
tegra_sor_connector_early_unregister(struct drm_connector *connector)
{
	struct tegra_sor *sor = con_to_sor(connector);

	dev_dbg(sor->dev, "> %s(connector=%p)\n", __func__, connector);

#ifdef CONFIG_DEBUG_FS
	debugfs_remove(sor->debugfs.regs);
#endif

	dev_dbg(sor->dev, "< %s()\n", __func__);
}

static void tegra_sor_connector_destroy(struct drm_connector *connector)
{
	struct tegra_sor *sor = con_to_sor(connector);

	dev_dbg(sor->dev, "> %s(connector=%p)\n", __func__, connector);

	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);

	dev_dbg(sor->dev, "< %s()\n", __func__);
}

static struct drm_connector_state *
tegra_sor_connector_duplicate_state(struct drm_connector *connector)
{
	struct tegra_sor_state *state = to_sor_state(connector->state);
	struct tegra_sor *sor = con_to_sor(connector);
	struct tegra_sor_state *copy;

	dev_dbg(sor->dev, "> %s(connector=%p)\n", __func__, connector);

	copy = kmemdup(state, sizeof(*state), GFP_KERNEL);
	if (!copy)
		return NULL;

	__drm_atomic_helper_connector_duplicate_state(connector, &copy->base);

	dev_dbg(sor->dev, "< %s() = %p\n", __func__, &copy->base);
	return &copy->base;
}

static void
tegra_sor_connector_destroy_state(struct drm_connector *connector,
				  struct drm_connector_state *state)
{
	struct tegra_sor *sor = con_to_sor(connector);

	dev_dbg(sor->dev, "> %s(connector=%p, state=%p)\n", __func__,
		connector, state);

	__drm_atomic_helper_connector_destroy_state(state);
	kfree(state);

	dev_dbg(sor->dev, "< %s()\n", __func__);
}

static const struct drm_connector_funcs tegra_sor_connector_funcs = {
	.dpms = drm_atomic_helper_connector_dpms,
	.reset = tegra_sor_connector_reset,
	.detect = tegra_sor_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.late_register = tegra_sor_connector_late_register,
	.early_unregister = tegra_sor_connector_early_unregister,
	.destroy = tegra_sor_connector_destroy,
	.atomic_duplicate_state = tegra_sor_connector_duplicate_state,
	.atomic_destroy_state = tegra_sor_connector_destroy_state,
};

static int tegra_sor_connector_get_modes(struct drm_connector *connector)
{
	struct tegra_sor *sor = con_to_sor(connector);
	struct edid *edid;
	int err = 0;

	dev_dbg(sor->dev, "> %s(connector=%p)\n", __func__, connector);
	dev_dbg(sor->dev, "  ddc: %p\n", sor->ddc);

	edid = drm_get_edid(connector, sor->ddc);

	drm_mode_connector_update_edid_property(connector, edid);

	if (edid) {
		err = drm_add_edid_modes(connector, edid);
		drm_edid_to_eld(connector, edid);
		kfree(edid);
	}

	dev_dbg(sor->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static enum drm_mode_status
tegra_sor_connector_mode_valid(struct drm_connector *connector,
			       struct drm_display_mode *mode)
{
	struct tegra_sor *sor = con_to_sor(connector);
	enum drm_mode_status status;

	dev_dbg(sor->dev, "> %s(connector=%p, mode=%p)\n", __func__, connector, mode);

	status = MODE_OK;

	if (mode->hdisplay > 1920)
		return MODE_VIRTUAL_X;

	if (mode->vdisplay > 1080)
		return MODE_VIRTUAL_Y;

	if (mode->clock > 340000)
		return MODE_NOCLOCK;

	dev_dbg(sor->dev, "< %s() = %d\n", __func__, status);
	return status;
}

static const struct drm_connector_helper_funcs tegra_sor_connector_helper_funcs = {
	.get_modes = tegra_sor_connector_get_modes,
	.mode_valid = tegra_sor_connector_mode_valid,
};

static void tegra_sor_encoder_destroy(struct drm_encoder *encoder)
{
	struct tegra_sor *sor = enc_to_sor(encoder);

	dev_dbg(sor->dev, "> %s(encoder=%p)\n", __func__, encoder);

	drm_encoder_cleanup(encoder);

	dev_dbg(sor->dev, "< %s()\n", __func__);
}

static const struct drm_encoder_funcs tegra_sor_encoder_funcs = {
	.destroy = tegra_sor_encoder_destroy,
};

static u32 tegra_sor_hdmi_subpack(const u8 *ptr, size_t size)
{
	u32 value = 0;
	size_t i;

	for (i = size; i > 0; i--)
		value = (value << 8) | ptr[i - 1];

	return value;
}

static void tegra_sor_hdmi_write_infopack(struct tegra_sor *sor,
					  const void *data, size_t size)
{
	const u8 *ptr = data;
	unsigned int offset;
	size_t i, j;
	u32 value;

	switch (ptr[0]) {
	case HDMI_INFOFRAME_TYPE_AVI:
		offset = SOR_HDMI_AVI_INFOFRAME_HEADER;
		break;

	case HDMI_INFOFRAME_TYPE_AUDIO:
		offset = SOR_HDMI_AUDIO_INFOFRAME_HEADER;
		break;

	case HDMI_INFOFRAME_TYPE_VENDOR:
		offset = SOR_HDMI_VSI_INFOFRAME_HEADER;
		break;

	default:
		dev_err(sor->dev, "unsupported infoframe type: %02x\n",
			ptr[0]);
		return;
	}

	value = INFOFRAME_HEADER_TYPE(ptr[0]) |
		INFOFRAME_HEADER_VERSION(ptr[1]) |
		INFOFRAME_HEADER_LEN(ptr[2]);
	writel(value, sor->regs + offset);
	offset += 4;

	/* each subpack contains 7 bytes, divided into:
	 * - subpack_low: bytes 0 - 3
	 * - subpack_high: bytes 4 - 6 (with byte 7 padded to 0x00
	 */
	for (i = 3, j = 0; i < size; i += 7, j += 8) {
		size_t rem = size - i, num = min_t(size_t, rem, 4);

		value = tegra_sor_hdmi_subpack(&ptr[i + 0], num);
		writel(value, sor->regs + offset);
		offset += 4;

		num = min_t(size_t, rem - num, 3);

		value = tegra_sor_hdmi_subpack(&ptr[i + 4], num);
		writel(value, sor->regs + offset);
		offset += 4;
	}
}

static int
tegra_sor_hdmi_setup_avi_infoframe(struct tegra_sor *sor,
				   const struct drm_display_mode *mode)
{
	u8 buffer[HDMI_INFOFRAME_SIZE(AVI)];
	struct hdmi_avi_infoframe frame;
	u32 value;
	int err;

	/* disable AVI infoframe */
	value = readl(sor->regs + SOR_HDMI_AVI_INFOFRAME_CTRL);
	value &= ~INFOFRAME_CTRL_SINGLE;
	value &= ~INFOFRAME_CTRL_OTHER;
	value &= ~INFOFRAME_CTRL_ENABLE;
	writel(value, sor->regs + SOR_HDMI_AVI_INFOFRAME_CTRL);

	err = drm_hdmi_avi_infoframe_from_display_mode(&frame, mode);
	if (err < 0) {
		dev_err(sor->dev, "failed to setup AVI infoframe: %d\n", err);
		return err;
	}

	err = hdmi_avi_infoframe_pack(&frame, buffer, sizeof(buffer));
	if (err < 0) {
		dev_err(sor->dev, "failed to pack AVI infoframe: %d\n", err);
		return err;
	}

	/* XXX */
	buffer[3] = 0x00;
	buffer[4] = 0x02;
	buffer[5] = 0x28;
	buffer[7] = 0x10;

	tegra_sor_hdmi_write_infopack(sor, buffer, err);

	/* enable AVI infoframe */
	value = readl(sor->regs + SOR_HDMI_AVI_INFOFRAME_CTRL);
	value |= INFOFRAME_CTRL_CHECKSUM_ENABLE;
	value |= INFOFRAME_CTRL_ENABLE;
	writel(value, sor->regs + SOR_HDMI_AVI_INFOFRAME_CTRL);

	return 0;
}

static void tegra_sor_hdmi_disable_audio_infoframe(struct tegra_sor *sor)
{
	u32 value;

	value = readl(sor->regs + SOR_HDMI_AUDIO_INFOFRAME_CTRL);
	value &= ~INFOFRAME_CTRL_ENABLE;
	writel(value, sor->regs + SOR_HDMI_AUDIO_INFOFRAME_CTRL);
}

static const struct tegra_sor_hdmi_settings *
tegra_sor_hdmi_find_settings(struct tegra_sor *sor, unsigned long frequency)
{
	unsigned int i;

	for (i = 0; i < sor->soc->num_settings; i++)
		if (frequency <= sor->soc->settings[i].frequency)
			return &sor->soc->settings[i];

	return NULL;
}

static int tegra_sor_power_up(struct tegra_sor *sor, unsigned long timeout)
{
	u32 value;
	int err;

	value = readl(sor->regs + SOR_PWR);
	value |= SOR_PWR_TRIGGER | SOR_PWR_NORMAL_STATE_PU;
	writel(value, sor->regs + SOR_PWR);

	err = readl_poll_timeout(sor->regs + SOR_PWR, value,
				 (value & SOR_PWR_TRIGGER) == 0,
				 1000, 250000);
	if (err < 0)
		return err;

	return 0;
}

static void tegra_sor_mode_set(struct tegra_sor *sor,
			       const struct drm_display_mode *mode,
			       struct tegra_sor_state *state)
{
	struct tegra_dc *dc = to_tegra_dc(sor->encoder.crtc);
	unsigned int vse, hse, vbe, hbe, vbs, hbs;
	u32 value;

	dev_dbg(sor->dev, "> %s(sor=%p, mode=%p, state=%p)\n", __func__, sor,
		mode, state);

	value = readl(sor->regs + SOR_STATE1);
	value &= ~SOR_STATE1_ASY_PIXELDEPTH_MASK;

	if (mode->flags & DRM_MODE_FLAG_PHSYNC)
		value &= ~SOR_STATE1_ASY_HSYNCPOL;

	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		value |= SOR_STATE1_ASY_HSYNCPOL;

	if (mode->flags & DRM_MODE_FLAG_PVSYNC)
		value &= ~SOR_STATE1_ASY_VSYNCPOL;

	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		value |= SOR_STATE1_ASY_VSYNCPOL;

	switch (state->bpc) {
	case 16:
		value |= SOR_STATE1_ASY_PIXELDEPTH_BPP_48_444;
		break;

	case 12:
		value |= SOR_STATE1_ASY_PIXELDEPTH_BPP_36_444;
		break;

	case 10:
		value |= SOR_STATE1_ASY_PIXELDEPTH_BPP_30_444;
		break;

	case 8:
		value |= SOR_STATE1_ASY_PIXELDEPTH_BPP_24_444;
		break;

	case 6:
		value |= SOR_STATE1_ASY_PIXELDEPTH_BPP_18_444;
		break;

	default:
		value |= SOR_STATE1_ASY_PIXELDEPTH_BPP_24_444;
		break;
	}

	writel(value, sor->regs + SOR_STATE1);

#if 1
	value = ((mode->vtotal & 0x7fff) << 16) | (mode->htotal & 0x7fff);
	writel(value, sor->regs + SOR_HEAD_STATE1(dc->pipe));

	/* sync end = sync width - 1 */
	vse = mode->vsync_end - mode->vsync_start - 1;
	hse = mode->hsync_end - mode->hsync_start - 1;

	value = ((vse & 0x7fff) << 16) | (hse & 0x7fff);
	writel(value, sor->regs + SOR_HEAD_STATE2(dc->pipe));

	/* blank end = sync end + back porch */
	vbe = vse + (mode->vtotal - mode->vsync_end);
	hbe = hse + (mode->htotal - mode->hsync_end);

	value = ((vbe & 0x7fff) << 16) | (hbe & 0x7fff);
	writel(value, sor->regs + SOR_HEAD_STATE3(dc->pipe));

	/* blank start = blank end + active */
	vbs = vbe + mode->vdisplay;
	hbs = hbe + mode->hdisplay;

	value = ((vbs & 0x7fff) << 16) | (hbs & 0x7fff);
	writel(value, sor->regs + SOR_HEAD_STATE4(dc->pipe));

	/* XXX interlacing support */
	writel(1, sor->regs + SOR_HEAD_STATE5(dc->pipe));
#endif

	dev_dbg(sor->dev, "< %s()\n", __func__);
}

static void tegra_sor_super_update(struct tegra_sor *sor)
{
	writel(0, sor->regs + SOR_SUPER_STATE0);
	writel(1, sor->regs + SOR_SUPER_STATE0);
	writel(0, sor->regs + SOR_SUPER_STATE0);
}

static void tegra_sor_update(struct tegra_sor *sor)
{
	writel(0, sor->regs + SOR_STATE0);
	writel(1, sor->regs + SOR_STATE0);
	writel(0, sor->regs + SOR_STATE0);
}

static int tegra_sor_attach(struct tegra_sor *sor)
{
	u32 value;
	int err;

	/* wake up in normal mode */
	value = readl(sor->regs + SOR_SUPER_STATE1);
	value |= SOR_SUPER_STATE1_ASY_HEAD_OPMODE_AWAKE;
	value |= SOR_SUPER_STATE1_ASY_ORMODE_NORMAL;
	writel(value, sor->regs + SOR_SUPER_STATE1);
	tegra_sor_super_update(sor);

	/* attach */
	value = readl(sor->regs + SOR_SUPER_STATE1);
	value |= SOR_SUPER_STATE1_ATTACHED;
	writel(value, sor->regs + SOR_SUPER_STATE1);
	tegra_sor_super_update(sor);

	err = readl_poll_timeout(sor->regs + SOR_TEST, value,
				 (value & SOR_TEST_ATTACHED) != 0,
				 1000, 250000);
	if (err < 0)
		return err;

	return 0;
}

static int tegra_sor_wakeup(struct tegra_sor *sor)
{
	u32 value;
	int err;

	err = readl_poll_timeout(sor->regs + SOR_TEST, value,
				 (value & SOR_TEST_ACT_HEAD_OPMODE_MASK) ==
					SOR_TEST_ACT_HEAD_OPMODE_AWAKE,
				 1000, 250000);
	if (err < 0)
		return err;

	return 0;
}

static int tegra_sor_detach(struct tegra_sor *sor)
{
	u32 value;
	int err;

	/* switch to safe mode */
	value = readl(sor->regs + SOR_SUPER_STATE1);
	value &= ~SOR_SUPER_STATE1_ASY_ORMODE_MASK;
	writel(value, sor->regs + SOR_SUPER_STATE1);

	tegra_sor_super_update(sor);

	err = readl_poll_timeout(sor->regs + SOR_PWR, value,
				 (value & SOR_PWR_MODE_SAFE) != 0,
				 1000, 250000);
	if (err < 0)
		return err;

	/* go to sleep */
	value = readl(sor->regs + SOR_SUPER_STATE1);
	value &= ~SOR_SUPER_STATE1_ASY_HEAD_OPMODE_MASK;
	writel(value, sor->regs + SOR_SUPER_STATE1);

	tegra_sor_super_update(sor);

	/* detach */
	value = readl(sor->regs + SOR_SUPER_STATE1);
	value &= ~SOR_SUPER_STATE1_ATTACHED;
	writel(value, sor->regs + SOR_SUPER_STATE1);

	tegra_sor_super_update(sor);

	err = readl_poll_timeout(sor->regs + SOR_TEST, value,
				 (value & SOR_TEST_ATTACHED) == 0,
				 1000, 250000);
	if (err < 0)
		return err;

	return 0;
}

static int tegra_sor_power_down(struct tegra_sor *sor)
{
	u32 value;
	int err;

	value = readl(sor->regs + SOR_PWR);
	value &= ~SOR_PWR_NORMAL_STATE_PU;
	value |= SOR_PWR_TRIGGER;
	writel(value, sor->regs + SOR_PWR);

	err = readl_poll_timeout(sor->regs + SOR_PWR, value,
				 (value & SOR_PWR_TRIGGER) == 0,
				 1000, 250000);
	if (err < 0)
		return err;

	/* switch to safe parent clock */
	err = clk_set_parent(sor->clk_out, sor->clk_safe);

	value = readl(sor->regs + SOR_DP_PADCTL(0));
	value &= ~(SOR_DP_PADCTL_PD_TXD_3 | SOR_DP_PADCTL_PD_TXD_0 |
		   SOR_DP_PADCTL_PD_TXD_1 | SOR_DP_PADCTL_PD_TXD_2);
	writel(value, sor->regs + SOR_DP_PADCTL(0));

	/* stop lane sequencer */
	value = SOR_LANE_SEQ_CTL_TRIGGER | SOR_LANE_SEQ_CTL_SEQUENCE_UP |
		SOR_LANE_SEQ_CTL_POWER_STATE_DOWN;
	writel(value, sor->regs + SOR_LANE_SEQ_CTL);

	err = readl_poll_timeout(sor->regs + SOR_LANE_SEQ_CTL, value,
				 (value & SOR_LANE_SEQ_CTL_TRIGGER) == 0,
				 1000, 250000);
	if (err < 0)
		return err;

	value = readl(sor->regs + SOR_PLL2);
	value |= SOR_PLL2_PORT_POWERDOWN;
	writel(value, sor->regs + SOR_PLL2);

	usleep_range(20, 100);

	value = readl(sor->regs + SOR_PLL0);
	value |= SOR_PLL0_VCOPD | SOR_PLL0_PWR;
	writel(value, sor->regs + SOR_PLL0);

	value = readl(sor->regs + SOR_PLL2);
	/* XXX not in TRM
	value |= SOR_PLL2_SEQ_PLLCAPPD;
	*/
	value |= SOR_PLL2_SEQ_PLLCAPPD_ENFORCE;
	writel(value, sor->regs + SOR_PLL2);

	usleep_range(20, 100);

	return 0;
}

static void tegra_sor_hdmi_disable(struct drm_encoder *encoder)
{
	struct tegra_dc *dc = to_tegra_dc(encoder->crtc);
	struct tegra_sor *sor = enc_to_sor(encoder);
	u32 value;
	int err;

	dev_dbg(sor->dev, "> %s(encoder=%p)\n", __func__, encoder);

	err = tegra_sor_detach(sor);
	if (err < 0)
		dev_err(sor->dev, "failed to detach SOR: %d\n", err);

	writel(0, sor->regs + SOR_STATE1);
	tegra_sor_update(sor);

	/* disable display to SOR clock */
	value = readl(dc->regs + DC_DISP_DISP_WIN_OPTIONS);
	value &= ~SOR1_TIMING_CYA;
	value &= ~SOR1_ENABLE;
	writel(value, dc->regs + DC_DISP_DISP_WIN_OPTIONS);

	tegra186_dc_commit(dc);

	err = tegra_sor_power_down(sor);
	if (err < 0)
		dev_err(sor->dev, "failed to power down SOR: %d\n", err);

	/* XXX assert E_DPD for HDMI_DP0/HDMI_DP1 */

	pm_runtime_put(sor->dev);
	dev_dbg(sor->dev, "< %s()\n", __func__);
}

static void tegra_sor_hdmi_enable(struct drm_encoder *encoder)
{
	struct tegra_dc *dc = to_tegra_dc(encoder->crtc);
	const struct tegra_sor_hdmi_settings *settings;
	struct tegra_sor *sor = enc_to_sor(encoder);
	struct drm_display_mode *mode;
	struct tegra_sor_state *state;
	unsigned int max_ac, div, i;
	unsigned long pclk;
	u32 value;
	int err;

	dev_dbg(sor->dev, "> %s(encoder=%p)\n", __func__, encoder);

	mode = &encoder->crtc->state->adjusted_mode;
	state = to_sor_state(sor->connector.state);
	pclk = mode->clock * 1000;

	pm_runtime_get_sync(sor->dev);

	/* switch to safe parent clock */
	err = clk_set_parent(sor->clk_out, sor->clk_safe);
	if (err < 0)
		dev_err(sor->dev, "failed to set safe parent clock: %d\n", err);

	div = clk_get_rate(sor->clk) / 1000000 * 4;

	/* XXX deassert E_DPD for HDMI_DP0/HDMI_DP1 */

	usleep_range(20, 100);

	value = readl(sor->regs + SOR_PLL2);
	value &= ~SOR_PLL2_BANDGAP_POWERDOWN;
	writel(value, sor->regs + SOR_PLL2);

	usleep_range(20, 100);

	value = readl(sor->regs + SOR_PLL3);
	/* XXX: doesn't match TRM */
	value &= ~SOR_PLL3_PLLVDD_MODE;
	writel(value, sor->regs + SOR_PLL3);

	value = readl(sor->regs + SOR_PLL0);
	value &= ~SOR_PLL0_VCOPD;
	value &= ~SOR_PLL0_PWR;
	writel(value, sor->regs + SOR_PLL0);

	value = readl(sor->regs + SOR_PLL2);
	value &= ~SOR_PLL2_SEQ_PLLCAPPD_ENFORCE;
	writel(value, sor->regs + SOR_PLL2);

	usleep_range(200, 400);

	value = readl(sor->regs + SOR_PLL2);
	value &= ~SOR_PLL2_POWERDOWN_OVERRIDE;
	value &= ~SOR_PLL2_PORT_POWERDOWN;
	writel(value, sor->regs + SOR_PLL2);

	usleep_range(20, 100);

	value = readl(sor->regs + SOR_DP_PADCTL(0));
	value |= SOR_DP_PADCTL_PD_TXD_3 | SOR_DP_PADCTL_PD_TXD_0 |
		 SOR_DP_PADCTL_PD_TXD_1 | SOR_DP_PADCTL_PD_TXD_2;
	writel(value, sor->regs + SOR_DP_PADCTL(0));

	while (true) {
		value = readl(sor->regs + SOR_LANE_SEQ_CTL);
		if ((value & SOR_LANE_SEQ_CTL_STATE_BUSY) == 0)
			break;

		usleep_range(250, 1000);
	}

	/* XXX: TRM says delay = 15 */
	value = SOR_LANE_SEQ_CTL_TRIGGER | SOR_LANE_SEQ_CTL_SEQUENCE_DOWN |
		SOR_LANE_SEQ_CTL_POWER_STATE_UP | SOR_LANE_SEQ_CTL_DELAY(5);
	writel(value, sor->regs + SOR_LANE_SEQ_CTL);

	while (true) {
		value = readl(sor->regs + SOR_LANE_SEQ_CTL);
		if ((value & SOR_LANE_SEQ_CTL_TRIGGER) == 0)
			break;

		usleep_range(250, 1000);
	}

	value = readl(sor->regs + SOR_CLK_CNTRL);
	value &= ~SOR_CLK_CNTRL_DP_LINK_SPEED_MASK;
	value |= SOR_CLK_CNTRL_DP_LINK_SPEED(state->link_speed);
	value &= ~SOR_CLK_CNTRL_DP_CLK_SEL_MASK;
	value |= SOR_CLK_CNTRL_DP_CLK_SEL_SINGLE_PCLK;
	writel(value, sor->regs + SOR_CLK_CNTRL);

	/* SOR brick PLL stabilization time */
	usleep_range(250, 1000);

	/* XXX: not in TRM */
	value = readl(sor->regs + SOR_DP_SPARE(0));
	value &= ~SOR_DP_SPARE0_DISP_VIDEO_PREAMBLE;
	value |= SOR_DP_SPARE0_SOR_CLK_SEL_MACRO;
	value &= ~SOR_DP_SPARE0_PANEL_INTERNAL;
	if (0)
		value |= SOR_DP_SPARE0_SEQ_ENABLE;
	else
		value &= ~SOR_DP_SPARE0_SEQ_ENABLE;
	writel(value, sor->regs + SOR_DP_SPARE(0));

	/* enable sequencer */
	value = SOR_SEQ_CTL_PU_PC(0) | SOR_SEQ_CTL_PU_PC_ALT(0) |
		SOR_SEQ_CTL_PD_PC(8) | SOR_SEQ_CTL_PD_PC_ALT(8);
	writel(value, sor->regs + SOR_SEQ_CTL);

	/* sequencer settings for HDMI */
	value = SOR_SEQ_INST_DRIVE_PWM_OUT_LO | SOR_SEQ_INST_HALT |
		SOR_SEQ_INST_WAIT_VSYNC | SOR_SEQ_INST_WAIT(1);
	writel(value, sor->regs + SOR_SEQ_INST(0));
	if (0) {
	writel(value, sor->regs + SOR_SEQ_INST(8));
	}

	/* program the reference clock (XXX not in TRM) */
	if (0) {
		value = SOR_REFCLK_DIV_INT(div) | SOR_REFCLK_DIV_FRAC(div);
		writel(value, sor->regs + SOR_REFCLK);
	}

	/* program the crossbar (XXX not in TRM) */
	if (0) {
	for (value = 0, i = 0; i < 5; i++)
		value |= SOR_XBAR_CTRL_LINK0_XSEL(i, sor->soc->xbar_cfg[i]) |
			 SOR_XBAR_CTRL_LINK1_XSEL(i, i);
	} else {
		value = 0x8d111a20;
	}

	writel(0x00000000, sor->regs + SOR_XBAR_POL);
	writel(value, sor->regs + SOR_XBAR_CTRL);

	/* switch to pad macro feedback clock */
	err = clk_set_parent(sor->clk, sor->clk_parent);
	if (err < 0) {
		dev_err(sor->dev, "failed to use %s as input for module clock: %d\n", __clk_get_name(sor->clk_parent), err);
	}

	DRM_DEBUG_KMS("setting clock to %lu Hz, mode: %lu Hz\n", state->pclk,
		      pclk);

	err = clk_set_rate(sor->clk, state->pclk);
	if (err < 0) {
		dev_err(sor->dev, "failed to set %s to %lu Hz: %d\n", __clk_get_name(sor->clk), pclk, err);
	}

	err = clk_set_parent(sor->clk_out, sor->clk_pad);
	if (err < 0) {
		dev_err(sor->dev, "failed to use %s as input for output clock: %d\n", __clk_get_name(sor->clk_pad), err);
	}

	/* XXX: not in the TRM */
	max_ac = ((mode->htotal - mode->hdisplay) - SOR_REKEY - 18) / 32;

	value = SOR_HDMI_CTRL_ENABLE | SOR_HDMI_CTRL_MAX_AC_PACKET(max_ac) |
		SOR_HDMI_CTRL_AUDIO_LAYOUT | SOR_HDMI_CTRL_REKEY(SOR_REKEY);
	writel(value, sor->regs + SOR_HDMI_CTRL);

	/* infoframe setup */
	err = tegra_sor_hdmi_setup_avi_infoframe(sor, mode);
	if (err < 0)
		dev_err(sor->dev, "failed to setup AVI infoframe: %d\n", err);

	/* XXX HDMI audio support not implemented yet */
	tegra_sor_hdmi_disable_audio_infoframe(sor);

	/* use TMDS protocol for HDMI */
	value = readl(sor->regs + SOR_STATE1);
	value &= ~SOR_STATE1_ASY_PROTOCOL_MASK;
	value |= SOR_STATE1_ASY_PROTOCOL_TMDS_A;
	writel(value, sor->regs + SOR_STATE1);

	/* power up pad calibration */
	value = readl(sor->regs + SOR_DP_PADCTL(0));
	value &= ~SOR_DP_PADCTL_PAD_CAL_PD;
	writel(value, sor->regs + SOR_DP_PADCTL(0));

	/* production settings */
	settings = tegra_sor_hdmi_find_settings(sor, mode->clock * 1000);
	if (!settings) {
		dev_err(sor->dev, "no settings for pixel clock %d Hz\n",
			mode->clock * 1000);
		return;
	}

	value = readl(sor->regs + SOR_PLL0);
	value &= ~SOR_PLL0_ICHPMP_MASK;
	value |= SOR_PLL0_ICHPMP(settings->ichpmp);
	value &= ~SOR_PLL0_FILTER_MASK;
	value |= SOR_PLL0_FILTER(settings->filter);
	value &= ~SOR_PLL0_VCOCAP_MASK;
	value |= SOR_PLL0_VCOCAP(settings->vcocap);
	writel(value, sor->regs + SOR_PLL0);

	value = readl(sor->regs + SOR_PLL1);
	value &= ~SOR_PLL1_LOADADJ_MASK;
	value |= SOR_PLL1_LOADADJ(settings->loadadj);
	value &= ~SOR_PLL1_TMDS_TERMADJ_MASK;
	value |= SOR_PLL1_TMDS_TERMADJ(settings->tmds_termadj);
	value |= SOR_PLL1_TMDS_TERM;
	writel(value, sor->regs + SOR_PLL1);

	value = readl(sor->regs + SOR_PLL3);
	value &= ~SOR_PLL3_BG_TEMP_COEF_MASK;
	value |= SOR_PLL3_BG_TEMP_COEF(settings->bg_temp_coef);
	value &= ~SOR_PLL3_BG_VREF_LEVEL_MASK;
	value |= SOR_PLL3_BG_VREF_LEVEL(settings->bg_vref_level);
	value &= ~SOR_PLL3_AVDD10_LEVEL_MASK;
	value |= SOR_PLL3_AVDD10_LEVEL(settings->avdd10_level);
	value &= ~SOR_PLL3_AVDD14_LEVEL_MASK;
	value |= SOR_PLL3_AVDD14_LEVEL(settings->avdd14_level);
	writel(value, sor->regs + SOR_PLL3);

	value = settings->drive_current[3] << 24 |
		settings->drive_current[2] << 16 |
		settings->drive_current[1] <<  8 |
		settings->drive_current[0] <<  0;
	writel(value, sor->regs + SOR_LANE_DRIVE_CURRENT(0));

	value = settings->preemphasis[3] << 24 |
		settings->preemphasis[2] << 16 |
		settings->preemphasis[1] <<  8 |
		settings->preemphasis[0] <<  0;
	writel(value, sor->regs + SOR_LANE_PREEMPHASIS(0));

	value = readl(sor->regs + SOR_DP_PADCTL(0));
	value &= ~SOR_DP_PADCTL_TX_PU_VALUE_MASK;
	value |= SOR_DP_PADCTL_TX_PU_ENABLE;
	value |= SOR_DP_PADCTL_TX_PU_VALUE(settings->tx_pu_value);
	writel(value, sor->regs + SOR_DP_PADCTL(0));

	value = readl(sor->regs + SOR_DP_PADCTL2);
	value &= ~SOR_DP_PADCTL2_SPAREPLL_MASK;
	value |= SOR_DP_PADCTL2_SPAREPLL(settings->sparepll);
	writel(value, sor->regs + SOR_DP_PADCTL2);

	/* power down pad calibration */
	value = readl(sor->regs + SOR_DP_PADCTL(0));
	value |= SOR_DP_PADCTL_PAD_CAL_PD;
	writel(value, sor->regs + SOR_DP_PADCTL(0));

	/* miscellaneous display controller settings */
	value = readl(dc->regs + DC_DISP_DISP_COLOR_CONTROL);
	value &= ~DITHER_CONTROL_MASK;
	value &= ~BASE_COLOR_SIZE_MASK;

	switch (state->bpc) {
	case 6:
		value |= BASE_COLOR_SIZE_666;
		break;

	case 8:
		value |= BASE_COLOR_SIZE_888;
		break;

	case 10:
		value |= BASE_COLOR_SIZE_101010;
		break;

	case 12:
		value |= BASE_COLOR_SIZE_121212;
		break;

	default:
		WARN(1, "%u bits-per-color not supported\n", state->bpc);
		value |= BASE_COLOR_SIZE_888;
		break;
	}

	writel(value, dc->regs + DC_DISP_DISP_COLOR_CONTROL);

	/* set display head owner */
	value = readl(sor->regs + SOR_STATE1);
	value &= ~SOR_STATE1_ASY_OWNER_MASK;
	value |= SOR_STATE1_ASY_OWNER(1 + dc->pipe);
	writel(value, sor->regs + SOR_STATE1);

	err = tegra_sor_power_up(sor, 250);
	if (err < 0)
		dev_err(sor->dev, "failed to power up SOR: %d\n", err);

	/* configure dynamic range of output */
	value = readl(sor->regs + SOR_HEAD_STATE0(dc->pipe));
	value &= ~SOR_HEAD_STATE0_RANGECOMPRESS;
	value &= ~SOR_HEAD_STATE0_DYNRANGE_MASK;
	writel(value, sor->regs + SOR_HEAD_STATE0(dc->pipe));

	/* configure colorspace */
	value = readl(sor->regs + SOR_HEAD_STATE0(dc->pipe));
	value &= ~SOR_HEAD_STATE0_COLORSPACE_MASK;
	value |= SOR_HEAD_STATE0_COLORSPACE_RGB;
	writel(value, sor->regs + SOR_HEAD_STATE0(dc->pipe));

	tegra_sor_mode_set(sor, mode, state);

	tegra_sor_update(sor);

	/* program preamble timing in SOR (XXX) */
	value = readl(sor->regs + SOR_DP_SPARE(0));
	value &= ~SOR_DP_SPARE0_DISP_VIDEO_PREAMBLE;
	writel(value, sor->regs + SOR_DP_SPARE(0));

	err = tegra_sor_attach(sor);
	if (err < 0)
		dev_err(sor->dev, "failed to attach SOR: %d\n", err);

	/* enable display to SOR clock and generate HDMI preamble */
	value = readl(dc->regs + DC_DISP_DISP_WIN_OPTIONS);
	value |= SOR1_ENABLE | SOR1_TIMING_CYA;
	writel(value, dc->regs + DC_DISP_DISP_WIN_OPTIONS);

	tegra186_dc_commit(dc);

	err = tegra_sor_wakeup(sor);
	if (err < 0)
		dev_err(sor->dev, "failed to wakeup SOR: %d\n", err);

	dev_dbg(sor->dev, "< %s()\n", __func__);
}

static int tegra_sor_hdmi_atomic_check(struct drm_encoder *encoder,
				       struct drm_crtc_state *crtc_state,
				       struct drm_connector_state *conn_state)
{
	struct tegra_sor_state *state = to_sor_state(conn_state);
	struct tegra_sor *sor = enc_to_sor(encoder);
	struct drm_display_info *info;
	unsigned long pclk;
	int err = 0;

	dev_dbg(sor->dev, "> %s(encoder=%p, crtc_state=%p, conn_state=%p)\n",
		__func__, encoder, crtc_state, conn_state);

	pclk = crtc_state->mode.clock * 1000;
	info = &sor->connector.display_info;

	/* XXX set pixel clock rate to 3/2 for YUV modes */

	/*
	 * For HBR2 modes the SOR brick needs to use the x20 multiplier, so
	 * the pixel clock must be corrected accordingly.
	 */
	if (pclk >= 340000000) {
		state->link_speed = 20;
		state->pclk = pclk / 2;
	} else {
		state->link_speed = 10;
		state->pclk = pclk;
	}

	err = tegra_dc_state_setup(conn_state->crtc, crtc_state, info->bpc,
				   sor->clk_parent, pclk);
	if (err < 0) {
		dev_err(sor->dev, "failed to setup CRTC state: %d\n", err);
		return err;
	}

	switch (info->bpc) {
	case 6:
	case 8:
		state->bpc = info->bpc;
		break;

	default:
		DRM_DEBUG_KMS("%u bits-per-color not supported\n", info->bpc);
		state->bpc = 8;
		break;
	}

	dev_dbg(sor->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static const struct drm_encoder_helper_funcs tegra_sor_hdmi_helper_funcs = {
	.disable = tegra_sor_hdmi_disable,
	.enable = tegra_sor_hdmi_enable,
	.atomic_check = tegra_sor_hdmi_atomic_check,
};

static int tegra_sor_init(struct host1x_client *client)
{
	struct drm_device *drm = dev_get_drvdata(client->parent);
	struct tegra_sor *sor = to_sor(client);
	struct device_node *np;
	struct drm_crtc *crtc;
	unsigned int i;
	int err = 0;

	dev_dbg(client->dev, "> %s(client=%p)\n", __func__, client);

	drm_connector_init(drm, &sor->connector, &tegra_sor_connector_funcs,
			   DRM_MODE_CONNECTOR_HDMIA);
	drm_connector_helper_add(&sor->connector,
				 &tegra_sor_connector_helper_funcs);

	drm_encoder_init(drm, &sor->encoder, &tegra_sor_encoder_funcs,
			 DRM_MODE_ENCODER_TMDS, NULL);
	drm_encoder_helper_add(&sor->encoder, &tegra_sor_hdmi_helper_funcs);

	drm_mode_connector_attach_encoder(&sor->connector, &sor->encoder);

#if 0
	for (i = 0; ; i++) {
		np = of_parse_phandle(client->dev->of_node, "inputs", i);
		if (!np)
			break;

		drm_for_each_crtc(crtc, drm) {
			if (crtc->port == np)
				sor->encoder.possible_crtcs |= drm_crtc_mask(crtc);
		}

		of_node_put(np);
	}
#else
	pr_info("  client: %s\n", client->dev->of_node->full_name);

	drm_for_each_crtc(crtc, drm) {
		pr_info("    trying CRTC %s...\n", crtc->port->full_name);

		for (i = 0; ; i++) {
			np = of_parse_phandle(crtc->port, "nvidia,outputs", i);
			if (!np)
				break;

			pr_info("      %s...\n", np->full_name);

			if (np == client->dev->of_node) {
				pr_info("        match\n");
				sor->encoder.possible_crtcs |= drm_crtc_mask(crtc);
			}

			of_node_put(np);
		}
	}
#endif

	sor->connector.polled = DRM_CONNECTOR_POLL_HPD;

	dev_dbg(client->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_sor_exit(struct host1x_client *client)
{
	int err = 0;

	dev_dbg(client->dev, "> %s(client=%p)\n", __func__, client);

	dev_dbg(client->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static const struct host1x_client_ops tegra_sor_ops = {
	.init = tegra_sor_init,
	.exit = tegra_sor_exit,
};

static irqreturn_t tegra_sor_hpd_irq(int irq, void *data)
{
	irqreturn_t status = IRQ_HANDLED;
	struct tegra_sor *sor = data;

	dev_dbg(sor->dev, "> %s(irq=%d, data=%p)\n", __func__, irq, data);

	drm_helper_hpd_irq_event(sor->connector.dev);

	dev_dbg(sor->dev, "< %s() = %d\n", __func__, status);
	return status;
}

static int tegra_sor_probe(struct platform_device *pdev)
{
	struct device_node *ddc;
	struct tegra_sor *sor;
	struct resource *res;
	unsigned long flags;
	int err;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	sor = devm_kzalloc(&pdev->dev, sizeof(*sor), GFP_KERNEL);
	if (!sor) {
		err = -ENOMEM;
		goto out;
	}

	sor->soc = of_device_get_match_data(&pdev->dev);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	sor->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(sor->regs)) {
		err = PTR_ERR(sor->regs);
		goto out;
	}

	sor->clk = devm_clk_get(&pdev->dev, "sor");
	if (IS_ERR(sor->clk)) {
		err = PTR_ERR(sor->clk);
		goto out;
	}

	dev_dbg(&pdev->dev, "sor: %s\n", __clk_get_name(sor->clk));

	err = clk_prepare_enable(sor->clk);
	if (err < 0)
		dev_err(&pdev->dev, "failed to enable module clock: %d\n", err);

	sor->clk_out = devm_clk_get(&pdev->dev, "out");
	if (IS_ERR(sor->clk_out)) {
		err = PTR_ERR(sor->clk_out);
		goto out;
	}

	dev_dbg(&pdev->dev, "out: %s\n", __clk_get_name(sor->clk_out));

	err = clk_prepare_enable(sor->clk_out);
	if (err < 0)
		dev_err(&pdev->dev, "failed to enable output clock: %d\n", err);

	sor->clk_pad = devm_clk_get(&pdev->dev, "pad");
	if (IS_ERR(sor->clk_pad)) {
		err = PTR_ERR(sor->clk_pad);
		goto out;
	}

	dev_dbg(&pdev->dev, "pad: %s\n", __clk_get_name(sor->clk_pad));

	err = clk_prepare_enable(sor->clk_pad);
	if (err < 0)
		dev_err(&pdev->dev, "failed to enable pad clock: %d\n", err);

	sor->clk_safe = devm_clk_get(&pdev->dev, "safe");
	if (IS_ERR(sor->clk_safe)) {
		err = PTR_ERR(sor->clk_safe);
		goto out;
	}

	dev_dbg(&pdev->dev, "safe: %s\n", __clk_get_name(sor->clk_safe));

	err = clk_prepare_enable(sor->clk_safe);
	if (err < 0)
		dev_err(&pdev->dev, "failed to enable safe clock: %d\n", err);

	sor->clk_parent = devm_clk_get(&pdev->dev, "parent");
	if (IS_ERR(sor->clk_parent)) {
		err = PTR_ERR(sor->clk_parent);
		goto out;
	}

	sor->clk_dp = devm_clk_get(&pdev->dev, "dp");
	if (IS_ERR(sor->clk_dp)) {
		err = PTR_ERR(sor->clk_dp);
		goto out;
	}

	sor->rst = devm_reset_control_get(&pdev->dev, "sor");
	if (IS_ERR(sor->rst)) {
		err = PTR_ERR(sor->rst);
		goto out;
	}

	if (0 && IS_ENABLED(CONFIG_DEBUG_FS)) {
		unsigned int i, width = 0;

		for (i = 0; i < ARRAY_SIZE(tegra_sor_regs); i++) {
			int len = strlen(tegra_sor_regs[i].name);

			if (len > width)
				width = len;
		}

		for (i = 0; i < ARRAY_SIZE(tegra_sor_regs); i++) {
			pr_debug("%#05x %-*s %#010x\n",
				 tegra_sor_regs[i].offset, width,
				 tegra_sor_regs[i].name,
				 readl(sor->regs + tegra_sor_regs[i].offset));
		}
	}

	sor->hdmi_supply = devm_regulator_get(&pdev->dev, "hdmi");
	if (IS_ERR(sor->hdmi_supply)) {
		err = PTR_ERR(sor->hdmi_supply);
		dev_err(sor->dev, "cannot get HDMI supply: %d\n", err);
		goto out;
	}

	err = regulator_enable(sor->hdmi_supply);
	if (err < 0) {
		dev_err(sor->dev, "failed to enable HDMI supply: %d\n", err);
		goto out;
	}

	sor->hpd = devm_gpiod_get(&pdev->dev, "hpd", GPIOD_IN);
	if (IS_ERR(sor->hpd)) {
		err = PTR_ERR(sor->hpd);
		dev_err(&pdev->dev, "failed to get HPD GPIO: %d\n", err);
		goto out;
	}

	dev_dbg(&pdev->dev, "HPD: %d\n", gpiod_get_value(sor->hpd));

	ddc = of_parse_phandle(pdev->dev.of_node, "ddc-i2c-bus", 0);
	if (ddc) {
		dev_dbg(&pdev->dev, "DDC: %s\n", ddc->full_name);
		sor->ddc = of_find_i2c_adapter_by_node(ddc);
		if (!sor->ddc) {
			err = -EPROBE_DEFER;
			of_node_put(ddc);
			goto out;
		}

		dev_dbg(&pdev->dev, "  adapter: %s\n", dev_name(&sor->ddc->dev));
		of_node_put(ddc);
	}

	err = gpiod_to_irq(sor->hpd);
	if (err < 0)
		goto out;

	flags = IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT;
	sor->hpd_irq = err;

	err = request_threaded_irq(sor->hpd_irq, NULL, tegra_sor_hpd_irq,
				   flags, "hpd", sor);
	if (err < 0) {
		dev_err(sor->dev, "failed to request IRQ: %d\n", err);
		goto out;
	}

	platform_set_drvdata(pdev, sor);
	sor->dev = &pdev->dev;

	INIT_LIST_HEAD(&sor->client.list);
	sor->client.ops = &tegra_sor_ops;
	sor->client.dev = &pdev->dev;

	err = host1x_client_register(&sor->client);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to register host1x client: %d\n",
			err);
		free_irq(sor->hpd_irq, sor);
	}

out:
	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_sor_remove(struct platform_device *pdev)
{
	struct tegra_sor *sor = platform_get_drvdata(pdev);
	int err = 0;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	err = host1x_client_unregister(&sor->client);
	if (err < 0)
		dev_err(&pdev->dev, "failed to unregister host1x client: %d\n",
			err);

	free_irq(sor->hpd_irq, sor);

	if (sor->ddc)
		put_device(&sor->ddc->dev);

	regulator_disable(sor->hdmi_supply);

	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_sor_suspend(struct device *dev)
{
	int err = 0;
	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_sor_resume(struct device *dev)
{
	int err = 0;
	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static const struct dev_pm_ops tegra_sor_pm_ops = {
	SET_RUNTIME_PM_OPS(tegra_sor_suspend, tegra_sor_resume, NULL)
};

static const u8 tegra186_sor_xbar_cfg[5] = {
	2, 1, 0, 3, 4
};

static const struct tegra_sor_soc tegra186_sor_soc = {
	.num_settings = ARRAY_SIZE(tegra186_sor_hdmi_defaults),
	.settings = tegra186_sor_hdmi_defaults,
	.xbar_cfg = tegra186_sor_xbar_cfg,
};

static const struct of_device_id tegra186_sor_of_match[] = {
	{ .compatible = "nvidia,tegra186-sor", .data = &tegra186_sor_soc },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, tegra186_sor_of_match);

struct platform_driver tegra186_sor_driver = {
	.driver = {
		.name = "tegra186-sor",
		.of_match_table = tegra186_sor_of_match,
		.pm = &tegra_sor_pm_ops,
	},
	.probe = tegra_sor_probe,
	.remove = tegra_sor_remove,
};
