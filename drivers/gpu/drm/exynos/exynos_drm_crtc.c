/* exynos_drm_crtc.c
 *
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

#include "exynos_drm_crtc.h"
#include "exynos_drm_drv.h"
#include "exynos_drm_fb.h"
#include "exynos_drm_encoder.h"
#include "exynos_drm_gem.h"
#include "exynos_trace.h"

static void exynos_drm_crtc_apply(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct exynos_drm_overlay *overlay = &exynos_crtc->overlay;

	exynos_drm_fn_encoder(crtc, overlay,
			exynos_drm_encoder_crtc_mode_set);
	exynos_drm_fn_encoder(crtc, &exynos_crtc->pipe,
			exynos_drm_encoder_crtc_commit);
}

void exynos_drm_overlay_update(struct exynos_drm_overlay *overlay,
			       struct drm_framebuffer *fb,
			       struct drm_display_mode *mode,
			       struct exynos_drm_crtc_pos *pos)
{
	struct exynos_drm_gem_buf *buffer;
	unsigned int actual_w;
	unsigned int actual_h;
	int nr = exynos_drm_format_num_buffers(fb->pixel_format);
	int i;

	for (i = 0; i < nr; i++) {
		buffer = exynos_drm_fb_buffer(fb, i);

		overlay->dma_addr[i] = buffer->dma_addr;
		overlay->vaddr[i] = buffer->kvaddr;

		DRM_DEBUG_KMS("buffer: %d, vaddr = 0x%lx, dma_addr = 0x%lx\n",
				i, (unsigned long)overlay->vaddr[i],
				(unsigned long)overlay->dma_addr[i]);
	}

	actual_w = min((mode->hdisplay - pos->crtc_x), pos->crtc_w);
	actual_h = min((mode->vdisplay - pos->crtc_y), pos->crtc_h);

	/* set drm framebuffer data. */
	overlay->fb_x = pos->fb_x;
	overlay->fb_y = pos->fb_y;
	overlay->fb_width = min(pos->fb_w, actual_w);
	overlay->fb_height = min(pos->fb_h, actual_h);
	overlay->fb_pitch = fb->pitches[0];
	overlay->bpp = fb->bits_per_pixel;
	overlay->pixel_format = fb->pixel_format;

	/* set overlay range to be displayed. */
	overlay->crtc_x = pos->crtc_x;
	overlay->crtc_y = pos->crtc_y;
	overlay->crtc_width = actual_w;
	overlay->crtc_height = actual_h;
	overlay->crtc_htotal = mode->crtc_htotal;
	overlay->crtc_hsync_len = mode->hsync_end - mode->hsync_start;
	overlay->crtc_vtotal = mode->crtc_vtotal;
	overlay->crtc_vsync_len = mode->vsync_end - mode->vsync_start;

	/* set drm mode data. */
	overlay->mode_width = mode->hdisplay;
	overlay->mode_height = mode->vdisplay;
	overlay->refresh = mode->vrefresh;
	overlay->scan_flag = mode->flags;

	DRM_DEBUG_KMS("overlay : offset_x/y(%d,%d), width/height(%d,%d)",
			overlay->crtc_x, overlay->crtc_y,
			overlay->crtc_width, overlay->crtc_height);
}

static void exynos_drm_crtc_update(struct drm_crtc *crtc,
				   struct drm_framebuffer *fb)
{
	struct exynos_drm_crtc *exynos_crtc;
	struct exynos_drm_overlay *overlay;
	struct exynos_drm_crtc_pos pos;
	struct drm_display_mode *mode = &crtc->mode;

	exynos_crtc = to_exynos_crtc(crtc);
	overlay = &exynos_crtc->overlay;

	memset(&pos, 0, sizeof(struct exynos_drm_crtc_pos));

	/* it means the offset of framebuffer to be displayed. */
	pos.fb_x = crtc->x;
	pos.fb_y = crtc->y;
	pos.fb_w = fb->width;
	pos.fb_h = fb->height;

	/* OSD position to be displayed. */
	pos.crtc_x = 0;
	pos.crtc_y = 0;
	pos.crtc_w = fb->width - crtc->x;
	pos.crtc_h = fb->height - crtc->y;

	exynos_drm_overlay_update(overlay, fb, mode, &pos);
}

static void exynos_drm_crtc_dpms(struct drm_crtc *crtc, int mode)
{
	struct drm_device *dev = crtc->dev;
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	DRM_DEBUG_KMS("crtc[%d] mode[%d]\n", crtc->base.id, mode);

	if (exynos_crtc->dpms == mode) {
		DRM_DEBUG_KMS("desired dpms mode is same as previous one.\n");
		return;
	}

	mutex_lock(&dev->struct_mutex);

	switch (mode) {
	case DRM_MODE_DPMS_ON:
		exynos_drm_fn_encoder(crtc, &mode,
				exynos_drm_encoder_crtc_dpms);
		exynos_crtc->dpms = mode;
		break;
	case DRM_MODE_DPMS_STANDBY:
	case DRM_MODE_DPMS_SUSPEND:
	case DRM_MODE_DPMS_OFF:
		exynos_drm_fn_encoder(crtc, &mode,
				exynos_drm_encoder_crtc_dpms);
		exynos_crtc->dpms = mode;
		break;
	default:
		DRM_ERROR("unspecified mode %d\n", mode);
		break;
	}

	mutex_unlock(&dev->struct_mutex);
}

static void exynos_drm_crtc_disable(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct exynos_drm_overlay *overlay = &exynos_crtc->overlay;
	int win = overlay->zpos;

	exynos_drm_fn_encoder(crtc, &win,
		exynos_drm_encoder_crtc_disable);
}

static void exynos_drm_crtc_prepare(struct drm_crtc *crtc)
{
	DRM_DEBUG_KMS("%s\n", __FILE__);

	/* drm framework doesn't check NULL. */
}

static void exynos_drm_crtc_commit(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	DRM_DEBUG_KMS("%s\n", __FILE__);

	/*
	 * when set_crtc is requested from user or at booting time,
	 * crtc->commit would be called without dpms call so if dpms is
	 * no power on then crtc->dpms should be called
	 * with DRM_MODE_DPMS_ON for the hardware power to be on.
	 */
	if (exynos_crtc->dpms != DRM_MODE_DPMS_ON) {
		int mode = DRM_MODE_DPMS_ON;

		/*
		 * TODO(seanpaul): This has the nasty habit of calling the
		 * underlying dpms/power callbacks twice on boot. This code
		 * needs to be cleaned up so this doesn't happen.
		 */

		/*
		 * enable hardware(power on) to all encoders hdmi connected
		 * to current crtc.
		 */
		exynos_drm_crtc_dpms(crtc, mode);
		/*
		 * enable dma to all encoders connected to current crtc and
		 * lcd panel.
		 */
		exynos_drm_fn_encoder(crtc, &mode,
					exynos_drm_encoder_dpms_from_crtc);
	}

	exynos_drm_fn_encoder(crtc, &exynos_crtc->pipe,
			exynos_drm_encoder_crtc_commit);
}

static bool
exynos_drm_crtc_mode_fixup(struct drm_crtc *crtc,
			    struct drm_display_mode *mode,
			    struct drm_display_mode *adjusted_mode)
{
	DRM_DEBUG_KMS("%s\n", __FILE__);

	/* drm framework doesn't check NULL */
	return true;
}

static int
exynos_drm_crtc_mode_set(struct drm_crtc *crtc, struct drm_display_mode *mode,
			  struct drm_display_mode *adjusted_mode, int x, int y,
			  struct drm_framebuffer *old_fb)
{
	struct drm_framebuffer *fb = crtc->fb;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (!fb)
		return -EINVAL;

	/*
	 * copy the mode data adjusted by mode_fixup() into crtc->mode
	 * so that hardware can be seet to proper mode.
	 */
	memcpy(&crtc->mode, adjusted_mode, sizeof(*adjusted_mode));

	exynos_drm_crtc_update(crtc, fb);

	return 0;
}

static int exynos_drm_crtc_mode_set_base(struct drm_crtc *crtc, int x, int y,
					  struct drm_framebuffer *old_fb)
{
	struct drm_framebuffer *fb = crtc->fb;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (!fb)
		return -EINVAL;

	exynos_drm_crtc_update(crtc, fb);
	exynos_drm_crtc_apply(crtc);

	return 0;
}

static void exynos_drm_crtc_load_lut(struct drm_crtc *crtc)
{
	DRM_DEBUG_KMS("%s\n", __FILE__);
	/* drm framework doesn't check NULL */
}

static struct drm_crtc_helper_funcs exynos_crtc_helper_funcs = {
	.dpms		= exynos_drm_crtc_dpms,
	.disable	= exynos_drm_crtc_disable,
	.prepare	= exynos_drm_crtc_prepare,
	.commit		= exynos_drm_crtc_commit,
	.mode_fixup	= exynos_drm_crtc_mode_fixup,
	.mode_set	= exynos_drm_crtc_mode_set,
	.mode_set_base	= exynos_drm_crtc_mode_set_base,
	.load_lut	= exynos_drm_crtc_load_lut,
};

#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
void exynos_drm_kds_callback(void *callback_parameter, void *callback_extra_parameter)
{
	struct drm_framebuffer *fb = callback_parameter;
	struct exynos_drm_fb *exynos_fb = to_exynos_fb(fb);
	struct drm_crtc *crtc =  exynos_fb->crtc;
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	struct kds_resource_set **pkds = callback_extra_parameter;
	struct kds_resource_set *prev_kds;
	struct drm_framebuffer *prev_fb;
	unsigned long flags;

	exynos_drm_crtc_update(crtc, fb);
	exynos_drm_crtc_apply(crtc);

	spin_lock_irqsave(&dev->event_lock, flags);
	prev_kds = exynos_crtc->pending_kds;
	prev_fb = exynos_crtc->pending_fb;
	exynos_crtc->pending_kds = *pkds;
	exynos_crtc->pending_fb = fb;
	*pkds = NULL;
	if (prev_fb)
		exynos_crtc->flip_in_flight--;
	spin_unlock_irqrestore(&dev->event_lock, flags);

	if (prev_fb) {
		DRM_ERROR("previous work detected\n");
		exynos_drm_fb_put(to_exynos_fb(prev_fb));
		if (prev_kds)
			kds_resource_set_release(&prev_kds);
	} else {
		BUG_ON(atomic_read(&exynos_crtc->flip_pending));
		BUG_ON(prev_kds);
		atomic_set(&exynos_crtc->flip_pending, 1);
	}
}
#endif

static void exynos_drm_crtc_flip_complete(struct drm_pending_vblank_event *e)
{
	struct timeval now;

	do_gettimeofday(&now);
	e->event.sequence = 0;
	e->event.tv_sec = now.tv_sec;
	e->event.tv_usec = now.tv_usec;
	list_add_tail(&e->base.link, &e->base.file_priv->event_list);
	wake_up_interruptible(&e->base.file_priv->event_wait);
	trace_exynos_fake_flip_complete(e->pipe);
}

static int exynos_drm_crtc_page_flip(struct drm_crtc *crtc,
				     struct drm_framebuffer *fb,
				     struct drm_pending_vblank_event *event)
{
	struct drm_device *dev = crtc->dev;
	struct exynos_drm_private *dev_priv = dev->dev_private;
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	unsigned long flags;
	int ret;
#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
	struct exynos_drm_fb *exynos_fb = to_exynos_fb(fb);
	struct exynos_drm_gem_obj *gem_ob = (struct exynos_drm_gem_obj *)exynos_fb->exynos_gem_obj[0];
	struct kds_resource_set **pkds;
	struct drm_pending_vblank_event *event_to_send;
#endif
	DRM_DEBUG_KMS("%s\n", __FILE__);

	/*
	 * the pipe from user always is 0 so we can set pipe number
	 * of current owner to event.
	 */
	if (event)
		event->pipe = exynos_crtc->pipe;

	ret = drm_vblank_get(dev, exynos_crtc->pipe);
	if (ret) {
		DRM_ERROR("Unable to get vblank\n");
		return -EINVAL;
	}

#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
	spin_lock_irqsave(&dev->event_lock, flags);
	if (exynos_crtc->flip_in_flight > 1) {
		spin_unlock_irqrestore(&dev->event_lock, flags);
		DRM_DEBUG_DRIVER("flip queue: crtc already busy\n");
		ret = -EBUSY;
		goto fail_max_in_flight;
	}
	/* Signal previous flip event. Or if none in flight signal current. */
	if (exynos_crtc->flip_in_flight) {
		event_to_send = exynos_crtc->event;
		exynos_crtc->event = event;
	} else {
		event_to_send = event;
		exynos_crtc->event = NULL;
	}
	pkds = &exynos_crtc->future_kds;
	if (*pkds)
		pkds = &exynos_crtc->future_kds_extra;
	*pkds = ERR_PTR(-EINVAL); /* Make it non-NULL */
	exynos_crtc->flip_in_flight++;
	spin_unlock_irqrestore(&dev->event_lock, flags);
#endif

	mutex_lock(&dev->struct_mutex);

	crtc->fb = fb;

	mutex_unlock(&dev->struct_mutex);

#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
	exynos_fb->crtc = crtc;
	if (gem_ob->base.export_dma_buf) {
		struct dma_buf *buf = gem_ob->base.export_dma_buf;
		unsigned long shared = 0UL;
		struct kds_resource *res_list = get_dma_buf_kds_resource(buf);

		/*
		 * If we don't already have a reference to the dma_buf,
		 * grab one now. We'll release it in exynos_drm_fb_destory().
		 */
		if (!exynos_fb->dma_buf) {
			get_dma_buf(buf);
			exynos_fb->dma_buf = buf;
		}
		BUG_ON(exynos_fb->dma_buf !=  buf);

		/* Waiting for the KDS resource*/
		ret = kds_async_waitall(pkds, KDS_FLAG_LOCKED_WAIT,
					&dev_priv->kds_cb, fb, pkds, 1,
					&shared, &res_list);
		if (ret) {
			DRM_ERROR("kds_async_waitall failed: %d\n", ret);
			goto fail_kds;
		}
	} else {
		*pkds = NULL;
		DRM_ERROR("flipping a non-kds buffer\n");
		exynos_drm_kds_callback(fb, pkds);
	}

	if (event_to_send) {
		spin_lock_irqsave(&dev->event_lock, flags);
		exynos_drm_crtc_flip_complete(event_to_send);
		spin_unlock_irqrestore(&dev->event_lock, flags);
	}
#endif

	exynos_drm_fb_get(exynos_fb);

	trace_exynos_flip_request(exynos_crtc->pipe);

	return 0;

fail_kds:
	*pkds = NULL;
	spin_lock_irqsave(&dev->event_lock, flags);
	exynos_crtc->flip_in_flight--;
	spin_unlock_irqrestore(&dev->event_lock, flags);
fail_max_in_flight:
	drm_vblank_put(dev, exynos_crtc->pipe);
	return ret;
}

void exynos_drm_crtc_finish_pageflip(struct drm_device *drm_dev, int crtc_idx)
{
	struct exynos_drm_private *dev_priv = drm_dev->dev_private;
	struct drm_crtc *crtc = dev_priv->crtc[crtc_idx];
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct kds_resource_set *kds;
	struct drm_framebuffer *fb;
	unsigned long flags;

	/* set wait vsync event to zero and wake up queue. */
	atomic_set(&dev_priv->wait_vsync_event, 0);
	DRM_WAKEUP(&dev_priv->wait_vsync_queue);

	if (!atomic_cmpxchg(&exynos_crtc->flip_pending, 1, 0))
		return;

	trace_exynos_flip_complete(crtc_idx);

	spin_lock_irqsave(&drm_dev->event_lock, flags);
	if (exynos_crtc->event) {
		exynos_drm_crtc_flip_complete(exynos_crtc->event);
		exynos_crtc->event = NULL;
	}
	kds = exynos_crtc->current_kds;
	exynos_crtc->current_kds = exynos_crtc->pending_kds;
	exynos_crtc->pending_kds = NULL;
	fb = exynos_crtc->current_fb;
	exynos_crtc->current_fb = exynos_crtc->pending_fb;
	exynos_crtc->pending_fb = NULL;
	exynos_crtc->flip_in_flight--;
	spin_unlock_irqrestore(&drm_dev->event_lock, flags);

	if (fb)
		exynos_drm_fb_put(to_exynos_fb(fb));
	if (kds)
		kds_resource_set_release(&kds);

	drm_vblank_put(drm_dev, crtc_idx);
}

static void exynos_drm_crtc_destroy(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct exynos_drm_private *private = crtc->dev->dev_private;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	private->crtc[exynos_crtc->pipe] = NULL;

	drm_crtc_cleanup(crtc);
	kfree(exynos_crtc);
}

static struct drm_crtc_funcs exynos_crtc_funcs = {
	.set_config	= drm_crtc_helper_set_config,
	.page_flip	= exynos_drm_crtc_page_flip,
	.destroy	= exynos_drm_crtc_destroy,
};

struct exynos_drm_overlay *get_exynos_drm_overlay(struct drm_device *dev,
		struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	return &exynos_crtc->overlay;
}

int exynos_drm_crtc_create(struct drm_device *dev, unsigned int nr)
{
	struct exynos_drm_crtc *exynos_crtc;
	struct exynos_drm_private *private = dev->dev_private;
	struct drm_crtc *crtc;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	exynos_crtc = kzalloc(sizeof(*exynos_crtc), GFP_KERNEL);
	if (!exynos_crtc) {
		DRM_ERROR("failed to allocate exynos crtc\n");
		return -ENOMEM;
	}

	exynos_crtc->pipe = nr;
	exynos_crtc->dpms = DRM_MODE_DPMS_OFF;
	exynos_crtc->overlay.zpos = DEFAULT_ZPOS;
	crtc = &exynos_crtc->drm_crtc;

	private->crtc[nr] = crtc;

	drm_crtc_init(dev, crtc, &exynos_crtc_funcs);
	drm_crtc_helper_add(crtc, &exynos_crtc_helper_funcs);

	return 0;
}

int exynos_drm_crtc_enable_vblank(struct drm_device *dev, int crtc)
{
	struct exynos_drm_private *private = dev->dev_private;
	struct exynos_drm_crtc *exynos_crtc =
		to_exynos_crtc(private->crtc[crtc]);

	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (exynos_crtc->dpms != DRM_MODE_DPMS_ON)
		return -EPERM;

	exynos_drm_fn_encoder(private->crtc[crtc], &crtc,
			exynos_drm_enable_vblank);

	return 0;
}

void exynos_drm_crtc_disable_vblank(struct drm_device *dev, int crtc)
{
	struct exynos_drm_private *private = dev->dev_private;
	struct exynos_drm_crtc *exynos_crtc =
		to_exynos_crtc(private->crtc[crtc]);

	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (exynos_crtc->dpms != DRM_MODE_DPMS_ON)
		return;

	exynos_drm_fn_encoder(private->crtc[crtc], &crtc,
			exynos_drm_disable_vblank);
}
