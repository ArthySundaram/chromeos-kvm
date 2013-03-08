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
#include "drm_crtc_helper.h"

#include <drm/exynos_drm.h>
#include "exynos_drm_drv.h"
#include "exynos_drm_encoder.h"
#include "exynos_drm_display.h"

#define MAX_EDID 256
#define to_exynos_connector(x)	container_of(x, struct exynos_drm_connector,\
				drm_connector)

struct exynos_drm_connector {
	struct drm_connector	drm_connector;
	uint32_t		encoder_id;
	struct exynos_drm_display *display;
};

/* convert exynos_video_timings to drm_display_mode */
static inline void
convert_to_display_mode(struct drm_display_mode *mode,
			struct exynos_drm_panel_info *panel)
{
	struct fb_videomode *timing = &panel->timing;
	DRM_DEBUG_KMS("[MODE:%d:%s]\n", DRM_BASE_ID(mode), mode->name);

	mode->clock = timing->pixclock / 1000;
	mode->vrefresh = timing->refresh;

	mode->hdisplay = timing->xres;
	mode->hsync_start = mode->hdisplay + timing->right_margin;
	mode->hsync_end = mode->hsync_start + timing->hsync_len;
	mode->htotal = mode->hsync_end + timing->left_margin;

	mode->vdisplay = timing->yres;
	mode->vsync_start = mode->vdisplay + timing->lower_margin;
	mode->vsync_end = mode->vsync_start + timing->vsync_len;
	mode->vtotal = mode->vsync_end + timing->upper_margin;
	mode->width_mm = panel->width_mm;
	mode->height_mm = panel->height_mm;

	if (timing->vmode & FB_VMODE_INTERLACED)
		mode->flags |= DRM_MODE_FLAG_INTERLACE;

	if (timing->vmode & FB_VMODE_DOUBLE)
		mode->flags |= DRM_MODE_FLAG_DBLSCAN;
}

/* convert drm_display_mode to exynos_video_timings */
static inline void
convert_to_video_timing(struct fb_videomode *timing,
			struct drm_display_mode *mode)
{
	DRM_DEBUG_KMS("[MODE:%d:%s]\n", DRM_BASE_ID(mode), mode->name);

	memset(timing, 0, sizeof(*timing));

	timing->pixclock = mode->clock * 1000;
	timing->refresh = drm_mode_vrefresh(mode);

	timing->xres = mode->hdisplay;
	timing->right_margin = mode->hsync_start - mode->hdisplay;
	timing->hsync_len = mode->hsync_end - mode->hsync_start;
	timing->left_margin = mode->htotal - mode->hsync_end;

	timing->yres = mode->vdisplay;
	timing->lower_margin = mode->vsync_start - mode->vdisplay;
	timing->vsync_len = mode->vsync_end - mode->vsync_start;
	timing->upper_margin = mode->vtotal - mode->vsync_end;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		timing->vmode = FB_VMODE_INTERLACED;
	else
		timing->vmode = FB_VMODE_NONINTERLACED;

	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		timing->vmode |= FB_VMODE_DOUBLE;
}

static struct exynos_drm_display *display_from_connector(
		struct drm_connector *connector)
{
	return to_exynos_connector(connector)->display;
}

static int exynos_drm_connector_get_edid(struct drm_connector *connector)
{
	struct exynos_drm_display *display = display_from_connector(connector);
	int ret;
	struct edid *edid;

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s]\n", DRM_BASE_ID(connector),
			drm_get_connector_name(connector));

	if (!display->panel_ops->get_edid)
		return -EINVAL;

	edid = display->panel_ops->get_edid(display->panel_ctx, connector);
	if (IS_ERR_OR_NULL(edid)) {
		ret = PTR_ERR(edid);
		edid = NULL;
		DRM_ERROR("Panel operation get_edid failed %d\n", ret);
		goto out;
	}

	ret = drm_mode_connector_update_edid_property(connector, edid);
	if (ret) {
		DRM_ERROR("update edid property failed(%d)\n", ret);
		goto out;
	}

	ret = drm_add_edid_modes(connector, edid);
	if (ret < 0) {
		DRM_ERROR("Add edid modes failed %d\n", ret);
		goto out;
	}

out:
	connector->display_info.raw_edid = NULL;
	kfree(edid);
	return ret;
}

static int exynos_drm_connector_get_panel(struct drm_connector *connector)
{
	struct exynos_drm_display *display = display_from_connector(connector);
	struct drm_display_mode *mode;
	struct exynos_drm_panel_info *panel;

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s]\n", DRM_BASE_ID(connector),
			drm_get_connector_name(connector));

	if (!display->controller_ops->get_panel)
		return -EINVAL;

	panel = display->controller_ops->get_panel(display->controller_ctx);

	mode = drm_mode_create(connector->dev);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	convert_to_display_mode(mode, panel);
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 1;
}

static int exynos_drm_connector_get_modes(struct drm_connector *connector)
{
	int ret;

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s]\n", DRM_BASE_ID(connector),
			drm_get_connector_name(connector));

	/*
	 * First try getting modes from EDID. If that doesn't yield any results,
	 * fall back to the panel call.
	 */
	ret = exynos_drm_connector_get_edid(connector);
	if (ret > 0)
		return ret;

	return exynos_drm_connector_get_panel(connector);
}

static int exynos_drm_connector_mode_valid(struct drm_connector *connector,
					    struct drm_display_mode *mode)
{
	struct exynos_drm_display *display = display_from_connector(connector);
	struct fb_videomode timing;
	int ret = MODE_BAD;

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s] [MODE:%d:%s]\n",
			DRM_BASE_ID(connector),
			drm_get_connector_name(connector), DRM_BASE_ID(mode),
			mode->name);

	convert_to_video_timing(&timing, mode);

	if (!display->panel_ops->check_timing)
		return ret;

	if (!display->panel_ops->check_timing(display->panel_ctx, &timing))
		ret = MODE_OK;

	return ret;
}

struct drm_encoder *exynos_drm_best_encoder(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	struct exynos_drm_connector *exynos_connector =
					to_exynos_connector(connector);
	struct drm_mode_object *obj;
	struct drm_encoder *encoder;

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s]\n", DRM_BASE_ID(connector),
			drm_get_connector_name(connector));

	obj = drm_mode_object_find(dev, exynos_connector->encoder_id,
				   DRM_MODE_OBJECT_ENCODER);
	if (!obj) {
		DRM_DEBUG_KMS("Unknown ENCODER ID %d\n",
				exynos_connector->encoder_id);
		return NULL;
	}

	encoder = obj_to_encoder(obj);

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s] == [ENCODER:%d:%s]\n",
			DRM_BASE_ID(connector),
			drm_get_connector_name(connector),
			DRM_BASE_ID(encoder), drm_get_encoder_name(encoder));

	return encoder;
}

struct drm_bridge *exynos_drm_best_bridge(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	struct drm_bridge *bridge;

	list_for_each_entry(bridge, &dev->mode_config.bridge_list, head) {
		if (bridge->connector_type == connector->connector_type)
			return bridge;
	}
	return NULL;
}

static struct drm_connector_helper_funcs exynos_connector_helper_funcs = {
	.get_modes	= exynos_drm_connector_get_modes,
	.mode_valid	= exynos_drm_connector_mode_valid,
	.best_encoder	= exynos_drm_best_encoder,
	.best_bridge	= exynos_drm_best_bridge,
};

static int exynos_drm_connector_fill_modes(struct drm_connector *connector,
				unsigned int max_width, unsigned int max_height)
{
	struct exynos_drm_display *display = display_from_connector(connector);
	unsigned int width, height;

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s] %d x %d\n", DRM_BASE_ID(connector),
			drm_get_connector_name(connector), max_width,
			max_height);

	width = max_width;
	height = max_height;

	/*
	 * If the specific driver wants to find desired_mode using maximum
	 * resolution then get max width and height from that driver.
	 */
	if (display->panel_ops->get_max_res)
		display->panel_ops->get_max_res(display->panel_ctx, &width,
				&height);

	return drm_helper_probe_single_connector_modes(connector, width,
							height);
}

/* get detection status of display device. */
static enum drm_connector_status
exynos_drm_connector_detect(struct drm_connector *connector, bool force)
{
	struct exynos_drm_display *display = display_from_connector(connector);
	enum drm_connector_status status;

	if (display->panel_ops->is_connected &&
	    display->panel_ops->is_connected(display->panel_ctx))
		status = connector_status_connected;
	else
		status = connector_status_disconnected;

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s] force: %d == %s\n",
			DRM_BASE_ID(connector),
			drm_get_connector_name(connector), force,
			drm_get_connector_status_name(status));

	return status;
}

static void exynos_drm_connector_destroy(struct drm_connector *connector)
{
	struct exynos_drm_connector *exynos_connector =
		to_exynos_connector(connector);

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s]\n", DRM_BASE_ID(connector),
			drm_get_connector_name(connector));

	drm_sysfs_connector_remove(connector);
	drm_connector_cleanup(connector);
	kfree(exynos_connector);
}

static struct drm_connector_funcs exynos_connector_funcs = {
	.dpms		= drm_helper_connector_dpms,
	.fill_modes	= exynos_drm_connector_fill_modes,
	.detect		= exynos_drm_connector_detect,
	.destroy	= exynos_drm_connector_destroy,
};

struct drm_connector *exynos_drm_connector_create(struct drm_device *dev,
						   struct drm_encoder *encoder)
{
	struct exynos_drm_connector *exynos_connector;
	struct exynos_drm_display *display = exynos_drm_get_display(encoder);
	struct drm_connector *connector;
	int type;
	int err;

	DRM_DEBUG_KMS("[DEV:%s] [ENCODER:%d:%s]\n", dev->devname,
			DRM_BASE_ID(encoder), drm_get_encoder_name(encoder));

	exynos_connector = kzalloc(sizeof(*exynos_connector), GFP_KERNEL);
	if (!exynos_connector) {
		DRM_ERROR("failed to allocate connector\n");
		return NULL;
	}

	connector = &exynos_connector->drm_connector;
	connector->dpms = DRM_MODE_DPMS_OFF;

	switch (display->display_type) {
	case EXYNOS_DRM_DISPLAY_TYPE_MIXER:
		type = DRM_MODE_CONNECTOR_HDMIA;
		connector->interlace_allowed = true;
		connector->polled = DRM_CONNECTOR_POLL_HPD;
		break;
	case EXYNOS_DRM_DISPLAY_TYPE_VIDI:
		type = DRM_MODE_CONNECTOR_VIRTUAL;
		connector->polled = DRM_CONNECTOR_POLL_HPD;
		break;
	case EXYNOS_DRM_DISPLAY_TYPE_FIMD:
		type = DRM_MODE_CONNECTOR_eDP;
		connector->polled = DRM_CONNECTOR_POLL_HPD;
		break;
	default:
		type = DRM_MODE_CONNECTOR_Unknown;
		break;
	}

	drm_connector_init(dev, connector, &exynos_connector_funcs, type);
	drm_connector_helper_add(connector, &exynos_connector_helper_funcs);

	err = drm_sysfs_connector_add(connector);
	if (err)
		goto err_connector;

	exynos_connector->encoder_id = encoder->base.id;
	exynos_connector->display = display;
	connector->encoder = encoder;

	err = drm_mode_connector_attach_encoder(connector, encoder);
	if (err) {
		DRM_ERROR("failed to attach a connector to a encoder\n");
		goto err_sysfs;
	}

	DRM_DEBUG_KMS("New [CONNECTOR:%d:%s] attached to [ENCODER:%d:%s]\n",
			DRM_BASE_ID(connector),
			drm_get_connector_name(connector),
			DRM_BASE_ID(encoder), drm_get_encoder_name(encoder));

	return connector;

err_sysfs:
	drm_sysfs_connector_remove(connector);
err_connector:
	drm_connector_cleanup(connector);
	kfree(exynos_connector);
	return NULL;
}
