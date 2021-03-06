/* exynos_drm_core.c
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 * Author:
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
#include "exynos_drm_drv.h"
#include "exynos_drm_encoder.h"
#include "exynos_drm_connector.h"
#include "exynos_drm_fbdev.h"
#include "exynos_drm_display.h"

static LIST_HEAD(exynos_drm_subdrv_list);
static struct drm_device *drm_dev;
#ifdef CONFIG_EXYNOS_IOMMU
struct dma_iommu_mapping *exynos_drm_common_mapping;
#endif

static int exynos_drm_subdrv_probe(struct drm_device *dev,
					struct exynos_drm_subdrv *subdrv)
{
	struct drm_encoder *encoder;
	struct drm_connector *connector;

	DRM_DEBUG_DRIVER("[DEV:%s] [SUBDRV:%s]\n", dev->devname,
			subdrv_name(subdrv));

	if (subdrv->probe) {
		int ret;

		/*
		 * this probe callback would be called by sub driver
		 * after setting of all resources to this sub driver,
		 * such as clock, irq and register map are done or by load()
		 * of exynos drm driver.
		 *
		 * P.S. note that this driver is considered for modularization.
		 */
		ret = subdrv->probe(dev, subdrv);
		if (ret)
			return ret;
	}

	if (!subdrv->display)
		return 0;

	/* create and initialize a encoder for this sub driver. */
	encoder = exynos_drm_encoder_create(dev, subdrv->display,
			(1 << EXYNOS_DRM_DISPLAY_NUM_DISPLAYS) - 1);
	if (!encoder) {
		DRM_ERROR("failed to create encoder\n");
		return -EFAULT;
	}
	DRM_DEBUG_DRIVER("[DEV:%s] created [ENCODER:%d:%s]\n", dev->devname,
			DRM_BASE_ID(encoder), drm_get_encoder_name(encoder));

	/*
	 * create and initialize a connector for this sub driver and
	 * attach the encoder created above to the connector.
	 */
	connector = exynos_drm_connector_create(dev, encoder);
	if (!connector) {
		DRM_ERROR("failed to create connector\n");
		encoder->funcs->destroy(encoder);
		return -EFAULT;
	}
	DRM_DEBUG_DRIVER("[DEV:%s] created [CONNECTOR:%d:%s]\n", dev->devname,
			DRM_BASE_ID(connector),
			drm_get_connector_name(connector));

	subdrv->encoder = encoder;
	subdrv->connector = connector;

	return 0;
}

static void exynos_drm_subdrv_remove(struct drm_device *dev,
				      struct exynos_drm_subdrv *subdrv)
{
	DRM_DEBUG_DRIVER("[DEV:%s] [SUBDRV:%s]\n", dev->devname,
			subdrv_name(subdrv));

	if (subdrv->remove)
		subdrv->remove(dev);

	if (subdrv->encoder) {
		struct drm_encoder *encoder = subdrv->encoder;
		encoder->funcs->destroy(encoder);
		subdrv->encoder = NULL;
	}

	if (subdrv->connector) {
		struct drm_connector *connector = subdrv->connector;
		connector->funcs->destroy(connector);
		subdrv->connector = NULL;
	}
}

int exynos_drm_device_register(struct drm_device *dev)
{
	struct exynos_drm_subdrv *subdrv, *n;
	int err;

	DRM_DEBUG_DRIVER("[DEV:%s]\n", dev->devname);

	if (!dev)
		return -EINVAL;

	drm_dev = dev;

	list_for_each_entry_safe(subdrv, n, &exynos_drm_subdrv_list, list) {
		subdrv->drm_dev = dev;
		err = exynos_drm_subdrv_probe(dev, subdrv);
		if (err) {
			DRM_DEBUG("exynos drm subdrv probe failed.\n");
			list_del(&subdrv->list);
		}
	}

	return 0;
}

int exynos_drm_device_unregister(struct drm_device *dev)
{
	struct exynos_drm_subdrv *subdrv;

	DRM_DEBUG_DRIVER("[DEV:%s]\n", dev->devname);

	if (!dev) {
		WARN(1, "Unexpected drm device unregister!\n");
		return -EINVAL;
	}

	list_for_each_entry(subdrv, &exynos_drm_subdrv_list, list)
		exynos_drm_subdrv_remove(dev, subdrv);

	drm_dev = NULL;

	return 0;
}

int exynos_drm_subdrv_register(struct exynos_drm_subdrv *subdrv)
{
	DRM_DEBUG_DRIVER("%s\n", subdrv_name(subdrv));

	if (!subdrv)
		return -EINVAL;

	list_add_tail(&subdrv->list, &exynos_drm_subdrv_list);

	return 0;
}

int exynos_drm_subdrv_unregister(struct exynos_drm_subdrv *subdrv)
{
	DRM_DEBUG_DRIVER("%s\n", subdrv_name(subdrv));

	if (!subdrv)
		return -EINVAL;

	list_del(&subdrv->list);

	return 0;
}

int exynos_drm_subdrv_open(struct drm_device *dev, struct drm_file *file)
{
	struct exynos_drm_subdrv *subdrv;
	int ret;

	DRM_DEBUG_DRIVER("[DEV:%s]\n", dev->devname);

	list_for_each_entry(subdrv, &exynos_drm_subdrv_list, list) {
		if (subdrv->open) {
			ret = subdrv->open(dev, subdrv->dev, file);
			if (ret)
				goto err;
		}
	}

	return 0;

err:
	list_for_each_entry_reverse(subdrv, &subdrv->list, list) {
		if (subdrv->close)
			subdrv->close(dev, subdrv->dev, file);
	}
	return ret;
}

void exynos_drm_subdrv_close(struct drm_device *dev, struct drm_file *file)
{
	struct exynos_drm_subdrv *subdrv;

	DRM_DEBUG_DRIVER("[DEV:%s]\n", dev->devname);

	list_for_each_entry(subdrv, &exynos_drm_subdrv_list, list) {
		if (subdrv->close)
			subdrv->close(dev, subdrv->dev, file);
	}
}
