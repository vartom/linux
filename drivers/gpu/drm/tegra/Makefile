ccflags-$(CONFIG_DRM_TEGRA_DEBUG) += -DDEBUG

tegra-drm-y := \
	drm.o \
	gem.o \
	fb.o \
	dc.o \
	output.o \
	rgb.o \
	hdmi.o \
	mipi-phy.o \
	dsi.o \
	sor.o \
	dpaux.o \
	gr2d.o \
	gr3d.o \
	falcon.o \
	vic.o

tegra-drm-$(CONFIG_ARCH_TEGRA_186_SOC) += \
	parker/display.o \
	parker/dc.o \
	parker/dpaux.o \
	parker/dsi.o \
	parker/sor.o

obj-$(CONFIG_DRM_TEGRA) += tegra-drm.o
