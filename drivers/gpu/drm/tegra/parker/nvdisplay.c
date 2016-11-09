#define DEBUG

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>

#include "display.h"

struct tegra186_nvdisplay {
	struct clk *clk_disp;
	struct clk *clk_dsc;
	struct clk *clk_hub;
	struct reset_control *rst;
};

struct tegra186_drm {
	struct tegra186_nvdisplay *nvdisplay;
	struct drm_device *drm;
	struct drm_fbdev_cma *fbdev;

	struct {
		struct drm_atomic_state *state;
		struct work_struct work;
		struct mutex lock;
	} commit;
};

static int compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static void tegra186_drm_lastclose(struct drm_device *drm)
{
	dev_dbg(drm->dev, "> %s(drm=%p)\n", __func__, drm);
	dev_dbg(drm->dev, "< %s()\n", __func__);
}

static struct drm_crtc *drm_crtc_get(struct drm_device *drm, unsigned int pipe)
{
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, drm) {
		if (pipe == drm_crtc_index(crtc))
			return crtc;
	}

	return NULL;
}

static int tegra186_drm_enable_vblank(struct drm_device *drm,
				      unsigned int pipe)
{
	struct drm_crtc *crtc;
	int err = 0;

	dev_dbg(drm->dev, "> %s(drm=%p, pipe=%u)\n", __func__, drm, pipe);

	crtc = drm_crtc_get(drm, pipe);
	if (!crtc) {
		err = -ENODEV;
		goto out;
	}

	tegra_crtc_enable_vblank(crtc);

out:
	dev_dbg(drm->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static void tegra186_drm_disable_vblank(struct drm_device *drm,
					unsigned int pipe)
{
	struct drm_crtc *crtc;

	dev_dbg(drm->dev, "> %s(drm=%p, pipe=%u)\n", __func__, drm, pipe);

	crtc = drm_crtc_get(drm, pipe);
	if (!crtc)
		return;

	tegra_crtc_disable_vblank(crtc);

	dev_dbg(drm->dev, "< %s()\n", __func__);
}

#ifdef CONFIG_DEBUG_FS
static int tegra186_drm_debugfs_init(struct drm_minor *minor)
{
	struct drm_device *drm = minor->dev;
	int err = 0;

	dev_dbg(drm->dev, "> %s(minor=%p)\n", __func__, minor);
	dev_dbg(drm->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static void tegra186_drm_debugfs_cleanup(struct drm_minor *minor)
{
	struct drm_device *drm = minor->dev;

	dev_dbg(drm->dev, "> %s(minor=%p)\n", __func__, minor);
	dev_dbg(drm->dev, "< %s()\n", __func__);
}
#endif

static const struct file_operations tegra186_drm_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = drm_gem_cma_mmap,
};

static struct drm_driver tegra186_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
			   DRIVER_ATOMIC,
	.lastclose = tegra186_drm_lastclose,
	.get_vblank_counter = drm_vblank_no_hw_counter,
	.enable_vblank = tegra186_drm_enable_vblank,
	.disable_vblank = tegra186_drm_disable_vblank,
	.gem_free_object_unlocked = drm_gem_cma_free_object,
	.gem_vm_ops = &drm_gem_cma_vm_ops,
	.dumb_create = drm_gem_cma_dumb_create,
	.dumb_map_offset = drm_gem_cma_dumb_map_offset,
	.dumb_destroy = drm_gem_dumb_destroy,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_export = drm_gem_prime_export,
	.gem_prime_import = drm_gem_prime_import,
	.gem_prime_get_sg_table = drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap = drm_gem_cma_prime_vmap,
	.gem_prime_vunmap = drm_gem_cma_prime_vunmap,
	.gem_prime_mmap = drm_gem_cma_prime_mmap,
#ifdef CONFIG_DEBUG_FS
	.debugfs_init = tegra186_drm_debugfs_init,
	.debugfs_cleanup = tegra186_drm_debugfs_cleanup,
#endif
	.fops = &tegra186_drm_fops,
	.name = "tegra",
	.desc = "NVIDIA Tegra186 DRM",
	.date = "20161024",
	.major = 0,
	.minor = 0,
	.patchlevel = 0,
};

static void tegra186_drm_output_poll_changed(struct drm_device *drm)
{
	struct tegra186_drm *tegra = drm->dev_private;

	drm_fbdev_cma_hotplug_event(tegra->fbdev);
}

static void tegra_atomic_schedule(struct tegra186_drm *tegra,
				  struct drm_atomic_state *state)
{
	tegra->commit.state = state;
	schedule_work(&tegra->commit.work);
}

static void tegra_atomic_complete(struct tegra186_drm *tegra,
				  struct drm_atomic_state *state)
{
	struct drm_device *drm = tegra->drm;

	/*
	 * Everything below can be run asynchronously without the need to grab
	 * any modeset locks at all under one condition: It must be guaranteed
	 * that the asynchronous work has either been cancelled (if the driver
	 * supports it, which at least requires that the framebuffers get
	 * cleaned up with drm_atomic_helper_cleanup_planes()) or completed
	 * before the new state gets committed on the software side with
	 * drm_atomic_helper_swap_state().
	 *
	 * This scheme allows new atomic state updates to be prepared and
	 * checked in parallel to the asynchronous completion of the previous
	 * update. Which is important since compositors need to figure out the
	 * composition of the next frame right after having submitted the
	 * current layout.
	 */

	drm_atomic_helper_commit_modeset_disables(drm, state);
	drm_atomic_helper_commit_modeset_enables(drm, state);
	drm_atomic_helper_commit_planes(drm, state,
					DRM_PLANE_COMMIT_ACTIVE_ONLY);

	drm_atomic_helper_wait_for_vblanks(drm, state);

	drm_atomic_helper_cleanup_planes(drm, state);
	drm_atomic_state_put(state);
}

static void tegra_atomic_work(struct work_struct *work)
{
	struct tegra186_drm *tegra = container_of(work, struct tegra186_drm,
						  commit.work);

	tegra_atomic_complete(tegra, tegra->commit.state);
}

static int tegra_atomic_commit(struct drm_device *drm,
			       struct drm_atomic_state *state,
			       bool nonblock)
{
	struct tegra186_drm *tegra = drm->dev_private;
	int err;

	err = drm_atomic_helper_prepare_planes(drm, state);
	if (err < 0)
		return err;

	/* serialize outstanding nonblocking commits */
	mutex_lock(&tegra->commit.lock);
	flush_work(&tegra->commit.work);

	/*
	 * This is the point of no return - everything below never fails except
	 * when the HW goes bonghits. Which means we can commit the new state of
	 * the software side now.
	 */

	drm_atomic_helper_swap_state(state, true);

	drm_atomic_state_get(state);

	if (nonblock)
		tegra_atomic_schedule(tegra, state);
	else
		tegra_atomic_complete(tegra, state);

	mutex_unlock(&tegra->commit.lock);
	return 0;
}

static const struct drm_mode_config_funcs tegra186_drm_mode_config_funcs = {
	.fb_create = drm_fb_cma_create,
	.output_poll_changed = tegra186_drm_output_poll_changed,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = tegra_atomic_commit,
};

static int tegra186_nvdisplay_bind(struct device *dev)
{
	struct tegra186_drm *tegra;
	struct drm_device *drm;
	int err = 0;

	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);

	tegra = devm_kzalloc(dev, sizeof(*tegra), GFP_KERNEL);
	if (!tegra) {
		err = -ENOMEM;
		goto out;
	}

	mutex_init(&tegra->commit.lock);
	INIT_WORK(&tegra->commit.work, tegra_atomic_work);

	drm = drm_dev_alloc(&tegra186_drm_driver, dev);
	if (IS_ERR(drm)) {
		err = PTR_ERR(drm);
		goto out;
	}

	tegra->nvdisplay = dev_get_drvdata(dev);
	dev_set_drvdata(dev, drm);
	drm->dev_private = tegra;
	tegra->drm = drm;

	drm_mode_config_init(drm);
	drm->mode_config.min_width = 0;
	drm->mode_config.max_width = 8192;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_height = 8192;
	drm->mode_config.funcs = &tegra186_drm_mode_config_funcs;

	pm_runtime_get_sync(dev);

	err = component_bind_all(dev, drm);
	if (err < 0)
		goto unref;

	/*
	 * We don't use the drm_irq_install() helpers provided by the DRM
	 * core, so we need to set this manually in order to allow the
	 * DRM_IOCTL_WAIT_VBLANK to operate correctly.
	 */
	drm->irq_enabled = true;

	err = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (err < 0) {
		dev_err(dev, "failed to initialized VBLANK: %d\n", err);
		goto unbind;
	}

	drm_mode_config_reset(drm);
	drm_kms_helper_poll_init(drm);

	dev_dbg(drm->dev, "CRTCs: %u connectors: %u\n", drm->mode_config.num_crtc, drm->mode_config.num_connector);

	tegra->fbdev = drm_fbdev_cma_init(drm, 32, drm->mode_config.num_crtc,
					  drm->mode_config.num_connector);
	dev_dbg(drm->dev, "fbdev: %p\n", tegra->fbdev);
	if (IS_ERR(tegra->fbdev)) {
		err = PTR_ERR(tegra->fbdev);
		goto cleanup;
	}

	err = drm_dev_register(drm, 0);
	if (err < 0)
		goto cleanup;

	return 0;

cleanup:
	if (!IS_ERR(tegra->fbdev))
		drm_fbdev_cma_fini(tegra->fbdev);

	drm_kms_helper_poll_fini(drm);
	drm_mode_config_cleanup(drm);
	drm_vblank_cleanup(drm);
unbind:
	component_unbind_all(dev, drm);
unref:
	pm_runtime_put(dev);
	dev_set_drvdata(dev, NULL);
	drm_dev_unref(drm);
out:
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static void tegra186_nvdisplay_unbind(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct tegra186_drm *tegra = drm->dev_private;

	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);

	drm_dev_unregister(drm);

	dev_dbg(dev, "  fbdev: %p\n", tegra->fbdev);
	if (!IS_ERR(tegra->fbdev))
		drm_fbdev_cma_fini(tegra->fbdev);

	drm_kms_helper_poll_fini(drm);
	drm_mode_config_cleanup(drm);
	drm_vblank_cleanup(drm);

	component_unbind_all(dev, drm);

	pm_runtime_put(dev);
	dev_set_drvdata(dev, tegra->nvdisplay);
	drm_dev_unref(drm);

	dev_dbg(dev, "< %s()\n", __func__);
}

static const struct component_master_ops tegra186_nvdisplay_master_ops = {
	.bind = tegra186_nvdisplay_bind,
	.unbind = tegra186_nvdisplay_unbind,
};

static int tegra186_nvdisplay_probe(struct platform_device *pdev)
{
	struct tegra186_nvdisplay *nvdisplay;
	struct component_match *match = NULL;
	struct device_node *np;
	int err = 0;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	nvdisplay = devm_kzalloc(&pdev->dev, sizeof(*nvdisplay), GFP_KERNEL);
	if (!nvdisplay) {
		err = -ENOMEM;
		goto out;
	}

	nvdisplay->clk_disp = devm_clk_get(&pdev->dev, "disp");
	if (IS_ERR(nvdisplay->clk_disp)) {
		err = PTR_ERR(nvdisplay->clk_disp);
		goto out;
	}

	nvdisplay->clk_dsc = devm_clk_get(&pdev->dev, "dsc");
	if (IS_ERR(nvdisplay->clk_dsc)) {
		err = PTR_ERR(nvdisplay->clk_dsc);
		goto out;
	}

	nvdisplay->clk_hub = devm_clk_get(&pdev->dev, "hub");
	if (IS_ERR(nvdisplay->clk_hub)) {
		err = PTR_ERR(nvdisplay->clk_hub);
		goto out;
	}

	nvdisplay->rst = devm_reset_control_get(&pdev->dev, "misc");
	if (IS_ERR(nvdisplay->rst)) {
		err = PTR_ERR(nvdisplay->rst);
		goto out;
	}

	/* XXX: enable clock across reset? */
	reset_control_assert(nvdisplay->rst);

	platform_set_drvdata(pdev, nvdisplay);
	pm_runtime_enable(&pdev->dev);

	for_each_available_child_of_node(pdev->dev.of_node, np)
		component_match_add(&pdev->dev, &match, compare_of, np);

	for_each_available_child_of_node(pdev->dev.of_node, np) {
		struct device_node *output;
		unsigned int i;

		dev_dbg(&pdev->dev, "  %s\n", np->full_name);

		for (i = 0;; i++) {
			output = of_parse_phandle(np, "outputs", i);
			if (!output)
				break;

			if (!of_device_is_available(output)) {
				of_node_put(output);
				continue;
			}

			dev_dbg(&pdev->dev, "    %s\n", output->full_name);
			component_match_add(&pdev->dev, &match, compare_of,
					    output);
			of_node_put(output);
		}
	}

	err = component_master_add_with_match(&pdev->dev,
					      &tegra186_nvdisplay_master_ops,
					      match);

out:
	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra186_nvdisplay_remove(struct platform_device *pdev)
{
	int err = 0;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	component_master_del(&pdev->dev, &tegra186_nvdisplay_master_ops);
	pm_runtime_disable(&pdev->dev);

	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra186_nvdisplay_suspend(struct device *dev)
{
	struct tegra186_nvdisplay *nvdisplay = dev_get_drvdata(dev);
	int err;

	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);

	err = reset_control_assert(nvdisplay->rst);
	if (err < 0)
		goto out;

	clk_disable_unprepare(nvdisplay->clk_hub);
	clk_disable_unprepare(nvdisplay->clk_dsc);
	clk_disable_unprepare(nvdisplay->clk_disp);

out:
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra186_nvdisplay_resume(struct device *dev)
{
	struct tegra186_nvdisplay *nvdisplay = dev_get_drvdata(dev);
	int err;

	dev_dbg(dev, "> %s(dev=%p)\n", __func__, dev);

	err = clk_prepare_enable(nvdisplay->clk_disp);
	if (err < 0)
		goto out;

	err = clk_prepare_enable(nvdisplay->clk_dsc);
	if (err < 0)
		goto disable_disp;

	err = clk_prepare_enable(nvdisplay->clk_hub);
	if (err < 0)
		goto disable_dsc;

	err = reset_control_deassert(nvdisplay->rst);
	if (err < 0)
		goto disable_hub;

	goto out;

disable_hub:
	clk_disable_unprepare(nvdisplay->clk_hub);
disable_dsc:
	clk_disable_unprepare(nvdisplay->clk_dsc);
disable_disp:
	clk_disable_unprepare(nvdisplay->clk_disp);
out:
	dev_dbg(dev, "< %s() = %d\n", __func__, err);
	return err;
}

static SIMPLE_DEV_PM_OPS(tegra186_nvdisplay_pm_ops,
			 tegra186_nvdisplay_suspend,
			 tegra186_nvdisplay_resume);

static const struct of_device_id tegra186_nvdisplay_of_match[] = {
	{ .compatible = "nvidia,tegra186-nvdisplay" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tegra186_nvdisplay_of_match);

struct platform_driver tegra186_nvdisplay_driver = {
	.driver = {
		.name = "tegra186-nvdisplay",
		.pm = &tegra186_nvdisplay_pm_ops,
		.of_match_table = tegra186_nvdisplay_of_match,
	},
	.probe = tegra186_nvdisplay_probe,
	.remove = tegra186_nvdisplay_remove,
};
