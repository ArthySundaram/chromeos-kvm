/*
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 * Authors:
 *	Inki Dae <inki.dae@samsung.com>
 *	Joonyoung Shim <jy0922.shim@samsung.com>
 *	Seung-Woo Kim <sw0312.kim@samsung.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "drmP.h"
#include "drm.h"
#include "drm_crtc_helper.h"

#include <drm/exynos_drm.h>

#include "exynos_drm_drv.h"
#include "exynos_drm_crtc.h"
#include "exynos_drm_encoder.h"
#include "exynos_drm_fbdev.h"
#include "exynos_drm_fb.h"
#include "exynos_drm_gem.h"
#include "exynos_drm_plane.h"
#include "exynos_drm_vidi.h"
#include "exynos_drm_dmabuf.h"
#include "exynos_drm_display.h"

#define DRIVER_NAME	"exynos"
#define DRIVER_DESC	"Samsung SoC DRM"
#define DRIVER_DATE	"20110530"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

#define VBLANK_OFF_DELAY	50000

static int exynos_drm_load(struct drm_device *dev, unsigned long flags)
{
	struct exynos_drm_private *private;
	int ret;
	int nr;

	DRM_DEBUG_DRIVER("%s\n", __FILE__);

	private = kzalloc(sizeof(struct exynos_drm_private), GFP_KERNEL);
	if (!private) {
		DRM_ERROR("failed to allocate private\n");
		return -ENOMEM;
	}

#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
	if (kds_callback_init(&private->kds_cb, 1,
			      exynos_drm_kds_callback) < 0) {
		DRM_ERROR("kds alloc queue failed.\n");
		ret = -ENOMEM;
		goto err_kds;
	}
#endif

	DRM_INIT_WAITQUEUE(&private->wait_vsync_queue);

	dev->dev_private = (void *)private;

	drm_mode_config_init(dev);

	/* init kms poll for handling hpd */
	drm_kms_helper_poll_init(dev);

	exynos_drm_mode_config_init(dev);

	/*
	 * EXYNOS4 is enough to have two CRTCs and each crtc would be used
	 * without dependency of hardware.
	 */
	for (nr = 0; nr < MAX_CRTC; nr++) {
		ret = exynos_drm_crtc_create(dev, nr);
		if (ret)
			goto err_crtc;
	}

	for (nr = 0; nr < MAX_PLANE; nr++) {
		ret = exynos_plane_init(dev, nr);
		if (ret)
			goto err_crtc;
	}

	ret = drm_vblank_init(dev, MAX_CRTC);
	if (ret)
		goto err_crtc;

	/*
	 * probe sub drivers such as display controller and hdmi driver,
	 * that were registered at probe() of platform driver
	 * to the sub driver and create encoder and connector for them.
	 */
	ret = exynos_drm_device_register(dev);
	if (ret)
		goto err_vblank;

	/* setup possible_clones. */
	exynos_drm_encoder_setup(dev);

	/*
	 * create and configure fb helper and also exynos specific
	 * fbdev object.
	 */
	ret = exynos_drm_fbdev_init(dev);
	if (ret) {
		DRM_ERROR("failed to initialize drm fbdev\n");
		goto err_drm_device;
	}

	drm_vblank_offdelay = VBLANK_OFF_DELAY;

	return 0;

err_drm_device:
	exynos_drm_device_unregister(dev);
err_vblank:
	drm_vblank_cleanup(dev);
err_crtc:
	drm_mode_config_cleanup(dev);
#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
	kds_callback_term(&private->kds_cb);
err_kds:
#endif
	kfree(private);

	return ret;
}

static int exynos_drm_unload(struct drm_device *dev)
{
	struct exynos_drm_private *private = dev->dev_private;

	DRM_DEBUG_DRIVER("%s\n", __FILE__);

	exynos_drm_fbdev_fini(dev);
	exynos_drm_device_unregister(dev);
	drm_vblank_cleanup(dev);
	drm_kms_helper_poll_fini(dev);
	drm_mode_config_cleanup(dev);
#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
	kds_callback_term(&private->kds_cb);
#endif
	kfree(private);

	dev->dev_private = NULL;

	return 0;
}

static int exynos_drm_open(struct drm_device *dev, struct drm_file *file)
{
	struct exynos_drm_file_private *file_private;

	DRM_DEBUG_DRIVER("%s\n", __FILE__);

	file_private = kzalloc(sizeof(*file_private), GFP_KERNEL);
	if (!file_private) {
		DRM_ERROR("failed to allocate exynos_drm_file_private\n");
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&file_private->gem_cpu_acquire_list);

	file->driver_priv = file_private;

	return exynos_drm_subdrv_open(dev, file);
}

static void exynos_drm_preclose(struct drm_device *dev,
					struct drm_file *file)
{
	struct exynos_drm_file_private *file_private = file->driver_priv;
	struct exynos_drm_gem_obj_node *cur, *d;

	DRM_DEBUG_DRIVER("%s\n", __FILE__);

	mutex_lock(&dev->struct_mutex);
	/* release kds resource sets for outstanding GEM object acquires */
	list_for_each_entry_safe(cur, d,
			&file_private->gem_cpu_acquire_list, list) {
#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
		BUG_ON(cur->exynos_gem_obj->resource_set == NULL);
		kds_resource_set_release(&cur->exynos_gem_obj->resource_set);
#endif
		drm_gem_object_unreference(&cur->exynos_gem_obj->base);
		kfree(cur);
	}
	mutex_unlock(&dev->struct_mutex);
	INIT_LIST_HEAD(&file_private->gem_cpu_acquire_list);

	exynos_drm_subdrv_close(dev, file);
}

static void exynos_drm_postclose(struct drm_device *dev, struct drm_file *file)
{
	DRM_DEBUG_DRIVER("%s\n", __FILE__);

	if (!file->driver_priv)
		return;

	kfree(file->driver_priv);
	file->driver_priv = NULL;
}

static void exynos_drm_lastclose(struct drm_device *dev)
{
	DRM_DEBUG_DRIVER("%s\n", __FILE__);

	exynos_drm_fbdev_restore_mode(dev);
}

static struct vm_operations_struct exynos_drm_gem_vm_ops = {
	.fault = exynos_drm_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static struct drm_ioctl_desc exynos_ioctls[] = {
	DRM_IOCTL_DEF_DRV(EXYNOS_GEM_CREATE, exynos_drm_gem_create_ioctl,
			DRM_UNLOCKED | DRM_AUTH),
	DRM_IOCTL_DEF_DRV(EXYNOS_GEM_MAP_OFFSET,
			exynos_drm_gem_map_offset_ioctl, DRM_UNLOCKED |
			DRM_AUTH),
	DRM_IOCTL_DEF_DRV(EXYNOS_GEM_MMAP,
			exynos_drm_gem_mmap_ioctl, DRM_UNLOCKED | DRM_AUTH),
	DRM_IOCTL_DEF_DRV(EXYNOS_PLANE_SET_ZPOS, exynos_plane_set_zpos_ioctl,
			DRM_UNLOCKED | DRM_AUTH),
	DRM_IOCTL_DEF_DRV(EXYNOS_VIDI_CONNECTION,
			vidi_connection_ioctl, DRM_UNLOCKED | DRM_AUTH),
	DRM_IOCTL_DEF_DRV(EXYNOS_GEM_CPU_ACQUIRE,
			exynos_drm_gem_cpu_acquire_ioctl,
			DRM_UNLOCKED | DRM_AUTH),
	DRM_IOCTL_DEF_DRV(EXYNOS_GEM_CPU_RELEASE,
			exynos_drm_gem_cpu_release_ioctl,
			DRM_UNLOCKED | DRM_AUTH),
};

static const struct file_operations exynos_drm_driver_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.mmap		= exynos_drm_gem_mmap,
	.poll		= drm_poll,
	.read		= drm_read,
	.unlocked_ioctl	= drm_ioctl,
	.release	= drm_release,
};

static struct drm_driver exynos_drm_driver = {
	.driver_features	= DRIVER_HAVE_IRQ | DRIVER_BUS_PLATFORM |
				  DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME,
	.load			= exynos_drm_load,
	.unload			= exynos_drm_unload,
	.open			= exynos_drm_open,
	.preclose		= exynos_drm_preclose,
	.lastclose		= exynos_drm_lastclose,
	.postclose		= exynos_drm_postclose,
	.get_vblank_counter	= drm_vblank_count,
	.enable_vblank		= exynos_drm_crtc_enable_vblank,
	.disable_vblank		= exynos_drm_crtc_disable_vblank,
#if defined(CONFIG_DEBUG_FS)
	.debugfs_init		= exynos_drm_debugfs_init,
	.debugfs_cleanup	= exynos_drm_debugfs_cleanup,
#endif
	.gem_free_object	= exynos_drm_gem_free_object,
	.gem_vm_ops		= &exynos_drm_gem_vm_ops,
	.dumb_create		= exynos_drm_gem_dumb_create,
	.dumb_map_offset	= exynos_drm_gem_dumb_map_offset,
	.dumb_destroy		= exynos_drm_gem_dumb_destroy,
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	.gem_prime_export	= exynos_dmabuf_prime_export,
	.gem_prime_import	= exynos_dmabuf_prime_import,
	.ioctls			= exynos_ioctls,
	.fops			= &exynos_drm_driver_fops,
	.name	= DRIVER_NAME,
	.desc	= DRIVER_DESC,
	.date	= DRIVER_DATE,
	.major	= DRIVER_MAJOR,
	.minor	= DRIVER_MINOR,
};

#ifdef CONFIG_EXYNOS_IOMMU
static int iommu_init(struct platform_device *pdev)
{
	/* DRM device expects a IOMMU mapping to be already
	 * created in FIMD. Else this function should
	 * throw an error.
	 */
	if (exynos_drm_common_mapping==NULL) {
		printk(KERN_ERR "exynos drm common mapping is invalid\n");
		return -1;
	}

	/*
	 * The ordering in Makefile warrants that this is initialized after
	 * FIMD, so only just ensure that it works as expected and we are
	 * reusing the mapping originally created in exynos_drm_fimd.c.
	 */
	WARN_ON(!exynos_drm_common_mapping);
	if (!s5p_create_iommu_mapping(&pdev->dev, 0,
				0, 0, exynos_drm_common_mapping)) {
		printk(KERN_ERR "failed to create IOMMU mapping\n");
		return -1;
	}

	return 0;
}

static void iommu_deinit(struct platform_device *pdev)
{
	/* detach the device and mapping */
	s5p_destroy_iommu_mapping(&pdev->dev);
	DRM_DEBUG("released the IOMMU mapping\n");

	return;
}
#endif

static int exynos_drm_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	DRM_DEBUG_DRIVER("%s\n", __FILE__);

#ifdef CONFIG_EXYNOS_IOMMU
	if (iommu_init(pdev)) {
		DRM_ERROR("failed to initialize IOMMU\n");
		return -ENODEV;
	}
#endif

	exynos_drm_driver.num_ioctls = DRM_ARRAY_SIZE(exynos_ioctls);

	pm_vt_switch_required(dev, false);
	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	ret = drm_platform_init(&exynos_drm_driver, pdev);
#ifdef CONFIG_EXYNOS_IOMMU
	if (ret)
		iommu_deinit(pdev);
#endif

	return ret;
}

static int __devexit exynos_drm_platform_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	DRM_DEBUG_DRIVER("%s\n", __FILE__);

	pm_vt_switch_unregister(dev);
	pm_runtime_disable(dev);

	drm_platform_exit(&exynos_drm_driver, pdev);

#ifdef CONFIG_EXYNOS_IOMMU
	iommu_deinit(pdev);
#endif
	return 0;
}

/* TODO (seanpaul): Once we remove platform drivers, we'll be calling the
 * various panel/controller init functions directly. These init functions will
 * return to us the ops and context, so we can get rid of these attach
 * functions. Once the attach functions are gone, we can move this array of
 * display pointers into the drm device's platform data.
 *
 * For now, we'll use a global to keep track of things.
 */
static struct exynos_drm_display *displays[EXYNOS_DRM_DISPLAY_NUM_DISPLAYS];

void exynos_display_attach_panel(enum exynos_drm_display_type type,
		struct exynos_panel_ops *ops, void *ctx)
{
	int i;
	for (i = 0; i < EXYNOS_DRM_DISPLAY_NUM_DISPLAYS; i++) {
		if (displays[i]->display_type == type) {
			displays[i]->panel_ctx = ctx;
			displays[i]->panel_ops = ops;
			return;
		}
	}
}

void exynos_display_attach_controller(enum exynos_drm_display_type type,
		struct exynos_controller_ops *ops, void *ctx)
{
	int i;
	for (i = 0; i < EXYNOS_DRM_DISPLAY_NUM_DISPLAYS; i++) {
		if (displays[i]->display_type == type) {
			displays[i]->controller_ctx = ctx;
			displays[i]->controller_ops = ops;
			return;
		}
	}
}

static int display_subdrv_probe(struct drm_device *drm_dev,
		struct exynos_drm_subdrv *subdrv)
{
	struct exynos_drm_display *display = subdrv->display;
	int ret;

	if (!display->controller_ops || !display->panel_ops)
		return -EINVAL;

	if (display->controller_ops->subdrv_probe) {
		ret = display->controller_ops->subdrv_probe(
				display->controller_ctx, drm_dev);
		if (ret)
			return ret;
	}

	if (display->panel_ops->subdrv_probe) {
		ret = display->panel_ops->subdrv_probe(display->panel_ctx,
				drm_dev);
		if (ret)
			return ret;
	}

	return 0;
}

int exynos_display_init(struct exynos_drm_display *display,
		enum exynos_drm_display_type type)
{
	struct exynos_drm_subdrv *subdrv;

	subdrv = kzalloc(sizeof(*subdrv), GFP_KERNEL);
	if (!subdrv) {
		DRM_ERROR("Failed to allocate display subdrv\n");
		return -ENOMEM;
	}

	display->display_type = type;
	display->pipe = -1;
	display->subdrv = subdrv;

	subdrv->probe = display_subdrv_probe;
	subdrv->display = display;
	exynos_drm_subdrv_register(subdrv);

	return 0;
}

void exynos_display_remove(struct exynos_drm_display *display)
{
	if (display->subdrv) {
		exynos_drm_subdrv_unregister(display->subdrv);
		kfree(display->subdrv);
	}
}

static int exynos_drm_resume_displays(void)
{
	int i;

	for (i = 0; i < EXYNOS_DRM_DISPLAY_NUM_DISPLAYS; i++) {
		struct exynos_drm_display *display = displays[i];
		struct drm_encoder *encoder = display->subdrv->encoder;

		if (!encoder)
			continue;

		exynos_drm_encoder_dpms(encoder, display->suspend_dpms);
	}
	return 0;
}

static int exynos_drm_suspend_displays(void)
{
	int i;

	for (i = 0; i < EXYNOS_DRM_DISPLAY_NUM_DISPLAYS; i++) {
		struct exynos_drm_display *display = displays[i];
		struct drm_encoder *encoder = display->subdrv->encoder;

		if (!encoder)
			continue;

		display->suspend_dpms = exynos_drm_encoder_get_dpms(encoder);
		exynos_drm_encoder_dpms(encoder, DRM_MODE_DPMS_OFF);
	}
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int exynos_drm_suspend(struct device *dev)
{
	if (pm_runtime_suspended(dev))
		return 0;

	return exynos_drm_suspend_displays();
}

static int exynos_drm_resume(struct device *dev)
{
	if (pm_runtime_suspended(dev))
		return 0;

	return exynos_drm_resume_displays();
}
#endif

#ifdef CONFIG_PM_RUNTIME
static int exynos_drm_runtime_resume(struct device *dev)
{
	return exynos_drm_resume_displays();
}

static int exynos_drm_runtime_suspend(struct device *dev)
{
	return exynos_drm_suspend_displays();
}
#endif

static const struct dev_pm_ops drm_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(exynos_drm_suspend, exynos_drm_resume)
	SET_RUNTIME_PM_OPS(exynos_drm_runtime_suspend,
			exynos_drm_runtime_resume, NULL)
};

static struct platform_driver exynos_drm_platform_driver = {
	.probe		= exynos_drm_platform_probe,
	.remove		= __devexit_p(exynos_drm_platform_remove),
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "exynos-drm",
		.pm	= &drm_pm_ops,
	},
};

static int __init exynos_drm_init(void)
{
	int ret, i;

	DRM_DEBUG_DRIVER("%s\n", __FILE__);

	for (i = 0; i < EXYNOS_DRM_DISPLAY_NUM_DISPLAYS; i++) {
		displays[i] = kzalloc(sizeof(*displays[i]), GFP_KERNEL);
		if (!displays[i]) {
			ret = -ENOMEM;
			goto out_display;
		}

		ret = exynos_display_init(displays[i], i);
		if (ret)
			goto out_display;
	}

#ifdef CONFIG_DRM_EXYNOS_FIMD
	ret = platform_driver_register(&fimd_driver);
	if (ret < 0)
		goto out_fimd;
#endif

#ifdef CONFIG_DRM_EXYNOS_DP
	ret = platform_driver_register(&dp_driver);
	if (ret < 0)
		goto out_dp_driver;
#endif

#ifdef CONFIG_DRM_EXYNOS_HDMI
	ret = platform_driver_register(&hdmi_driver);
	if (ret < 0)
		goto out_hdmi;
	ret = platform_driver_register(&mixer_driver);
	if (ret < 0)
		goto out_mixer;
#endif

#ifdef CONFIG_DRM_EXYNOS_VIDI
	ret = platform_driver_register(&vidi_driver);
	if (ret < 0)
		goto out_vidi;
#endif

	ret = platform_driver_register(&exynos_drm_platform_driver);
	if (ret < 0)
		goto out;

	return 0;

out:
#ifdef CONFIG_DRM_EXYNOS_VIDI
out_vidi:
	platform_driver_unregister(&vidi_driver);
#endif

#ifdef CONFIG_DRM_EXYNOS_HDMI
	platform_driver_unregister(&mixer_driver);
out_mixer:
	platform_driver_unregister(&hdmi_driver);
out_hdmi:
#endif

	platform_driver_unregister(&dp_driver);
out_dp_driver:
#ifdef CONFIG_DRM_EXYNOS_FIMD
	platform_driver_unregister(&fimd_driver);
out_fimd:
#endif
out_display:
	for (i = 0; i < EXYNOS_DRM_DISPLAY_NUM_DISPLAYS; i++) {
		if (!displays[i])
			continue;

		exynos_display_remove(displays[i]);
		kfree(displays[i]);
	}
	return ret;
}

static void __exit exynos_drm_exit(void)
{
	int i;

	DRM_DEBUG_DRIVER("%s\n", __FILE__);

	platform_driver_unregister(&exynos_drm_platform_driver);

#ifdef CONFIG_DRM_EXYNOS_HDMI
	platform_driver_unregister(&mixer_driver);
	platform_driver_unregister(&hdmi_driver);
#endif

#ifdef CONFIG_DRM_EXYNOS_VIDI
	platform_driver_unregister(&vidi_driver);
#endif

	platform_driver_unregister(&dp_driver);
#ifdef CONFIG_DRM_EXYNOS_FIMD
	platform_driver_unregister(&fimd_driver);
#endif

	for (i = 0; i < EXYNOS_DRM_DISPLAY_NUM_DISPLAYS; i++) {
		if (!displays[i])
			continue;

		exynos_display_remove(displays[i]);
		kfree(displays[i]);
	}
}

module_init(exynos_drm_init);
module_exit(exynos_drm_exit);

MODULE_AUTHOR("Inki Dae <inki.dae@samsung.com>");
MODULE_AUTHOR("Joonyoung Shim <jy0922.shim@samsung.com>");
MODULE_AUTHOR("Seung-Woo Kim <sw0312.kim@samsung.com>");
MODULE_DESCRIPTION("Samsung SoC DRM Driver");
MODULE_LICENSE("GPL");
