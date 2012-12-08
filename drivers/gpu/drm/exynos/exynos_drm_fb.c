/* exynos_drm_fb.c
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
#include "drm_crtc.h"
#include "drm_crtc_helper.h"
#include "drm_fb_helper.h"

#include "exynos_drm_drv.h"
#include "exynos_drm_fb.h"
#include "exynos_drm_gem.h"
#include "exynos_drm_crtc.h"

/* Helper functions to push things in/out of the iommu. */
static int exynos_drm_fb_map(struct drm_framebuffer *fb)
{
	struct exynos_drm_fb *exynos_fb = to_exynos_fb(fb);
	struct exynos_drm_gem_obj *gem_ob = exynos_fb->exynos_gem_obj[0];
	struct drm_gem_object *obj = &gem_ob->base;
	struct exynos_drm_gem_buf *buf;
	int ret;

	buf = exynos_drm_fb_buffer(fb, 0);
	if (!buf) {
		DRM_ERROR("buffer is null\n");
		return -ENOMEM;
	}

	drm_gem_object_reference(obj);

	ret = dma_map_sg(obj->dev->dev,
			 buf->sgt->sgl,
			 buf->sgt->orig_nents,
			 DMA_BIDIRECTIONAL);
	if (!ret) {
		DRM_ERROR("failed to map sg\n");
		return -ENOMEM;
	}

	buf->dma_addr = buf->sgt->sgl->dma_address;

	return 0;
}

static int exynos_drm_fb_unmap(struct drm_framebuffer *fb)
{
	struct exynos_drm_fb *exynos_fb = to_exynos_fb(fb);
	struct exynos_drm_gem_obj *gem_ob = exynos_fb->exynos_gem_obj[0];
	struct drm_gem_object *obj = &gem_ob->base;
	struct exynos_drm_gem_buf *buf;

	buf = exynos_drm_fb_buffer(fb, 0);
	if (!buf) {
		DRM_ERROR("buffer is null\n");
		return -ENOMEM;
	}

	/*
	 * Not critical, this is used for cleanup in the fb_create error path
	 * path so keep it silent.
	 */
	if (!buf->dma_addr)
		return -ENOMEM;

	buf->dma_addr = 0;

	/* Unmap the SGT to remove the IOMMU mapping created for this buffer */
	dma_unmap_sg(obj->dev->dev,
		     buf->sgt->sgl,
		     buf->sgt->orig_nents,
		     DMA_BIDIRECTIONAL);

	drm_gem_object_unreference_unlocked(obj);

	return 0;
}

void exynos_drm_fb_release(struct kref *kref)
{
	struct exynos_drm_fb *exynos_fb;

	exynos_fb = container_of(kref, struct exynos_drm_fb, refcount);
	schedule_work(&exynos_fb->release_work);
}

static void exynos_drm_fb_release_work_fn(struct work_struct *work)
{
	struct drm_framebuffer *fb;
	struct exynos_drm_fb *exynos_fb;
	int i, nr;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	exynos_fb = container_of(work, struct exynos_drm_fb, release_work);
	fb = &exynos_fb->fb;

#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
	if (exynos_fb->dma_buf)
		dma_buf_put(exynos_fb->dma_buf);
#endif

	drm_framebuffer_cleanup(fb);

	if (exynos_drm_fb_unmap(fb))
		DRM_ERROR("Couldn't unmap buffer\n");

	nr = exynos_drm_format_num_buffers(fb->pixel_format);

	for (i = 0; i < nr; i++) {
		struct drm_gem_object *obj;

		obj = &exynos_fb->exynos_gem_obj[i]->base;
		drm_gem_object_unreference_unlocked(obj);
	}

	kfree(exynos_fb);
}

static void exynos_drm_fb_destroy(struct drm_framebuffer *fb)
{
	exynos_drm_fb_put(to_exynos_fb(fb));
}

static int exynos_drm_fb_create_handle(struct drm_framebuffer *fb,
					struct drm_file *file_priv,
					unsigned int *handle)
{
	struct exynos_drm_fb *exynos_fb = to_exynos_fb(fb);

	DRM_DEBUG_KMS("%s\n", __FILE__);

	return drm_gem_handle_create(file_priv,
			&exynos_fb->exynos_gem_obj[0]->base, handle);
}

static int exynos_drm_fb_dirty(struct drm_framebuffer *fb,
				struct drm_file *file_priv, unsigned flags,
				unsigned color, struct drm_clip_rect *clips,
				unsigned num_clips)
{
	DRM_DEBUG_KMS("%s\n", __FILE__);

	/* TODO */

	return 0;
}

static struct drm_framebuffer_funcs exynos_drm_fb_funcs = {
	.destroy	= exynos_drm_fb_destroy,
	.create_handle	= exynos_drm_fb_create_handle,
	.dirty		= exynos_drm_fb_dirty,
};

struct drm_framebuffer *
exynos_drm_framebuffer_init(struct drm_device *dev,
			    struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct exynos_drm_fb *exynos_fb;
	int ret;

	exynos_fb = kzalloc(sizeof(*exynos_fb), GFP_KERNEL);
	if (!exynos_fb) {
		DRM_ERROR("failed to allocate exynos drm framebuffer\n");
		return ERR_PTR(-ENOMEM);
	}

	kref_init(&exynos_fb->refcount);
	INIT_WORK(&exynos_fb->release_work, exynos_drm_fb_release_work_fn);

	ret = drm_framebuffer_init(dev, &exynos_fb->fb, &exynos_drm_fb_funcs);
	if (ret) {
		DRM_ERROR("failed to initialize framebuffer\n");
		goto err_init;
	}

	drm_helper_mode_fill_fb_struct(&exynos_fb->fb, mode_cmd);

	return &exynos_fb->fb;

err_init:
	kfree(exynos_fb);
	return ERR_PTR(ret);
}

static struct drm_framebuffer *
exynos_user_fb_create(struct drm_device *dev, struct drm_file *file_priv,
		      struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_gem_object *obj;
	struct drm_framebuffer *fb;
	struct exynos_drm_fb *exynos_fb;
	int i, nr, ret = 0;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	fb = exynos_drm_framebuffer_init(dev, mode_cmd);
	if (IS_ERR(fb))
		return fb;

	exynos_fb = to_exynos_fb(fb);
	nr = exynos_drm_format_num_buffers(fb->pixel_format);

	for (i = 0; i < nr; i++) {
		obj = drm_gem_object_lookup(dev, file_priv,
				mode_cmd->handles[i]);
		if (!obj) {
			DRM_ERROR("failed to lookup gem object\n");
			ret = -ENOENT;
			goto err_lookup;
		}

		/*
		 * We keep the reference which came from drm_gem_object_lookup.
		 * It is used to ensure the bo's lifetime is >= the framebuffer.
		 */

		exynos_fb->exynos_gem_obj[i] = to_exynos_gem_obj(obj);
	}

	if (exynos_drm_fb_map(fb)) {
		DRM_ERROR("Failed to map gem object\n");
		ret = -ENOMEM;
		goto err_map;
	}

	return fb;

err_map:
err_lookup:
	while (--i >= 0) {
		obj = &exynos_fb->exynos_gem_obj[i]->base;
		drm_gem_object_unreference_unlocked(obj);
	}
	kfree(exynos_fb);
	return ERR_PTR(ret);
}

struct exynos_drm_gem_buf *exynos_drm_fb_buffer(struct drm_framebuffer *fb,
						int index)
{
	struct exynos_drm_fb *exynos_fb = to_exynos_fb(fb);
	struct exynos_drm_gem_buf *buffer;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (index >= MAX_FB_BUFFER)
		return NULL;

	buffer = exynos_fb->exynos_gem_obj[index]->buffer;
	if (!buffer)
		return NULL;

	DRM_DEBUG_KMS("vaddr = 0x%lx, dma_addr = 0x%lx\n",
			(unsigned long)buffer->kvaddr,
			(unsigned long)buffer->dma_addr);

	return buffer;
}

static void exynos_drm_output_poll_changed(struct drm_device *dev)
{
	struct exynos_drm_private *private = dev->dev_private;
	struct drm_fb_helper *fb_helper = private->fb_helper;

	if (fb_helper)
		drm_fb_helper_hotplug_event(fb_helper);
}

static struct drm_mode_config_funcs exynos_drm_mode_config_funcs = {
	.fb_create = exynos_user_fb_create,
	.output_poll_changed = exynos_drm_output_poll_changed,
};

void exynos_drm_mode_config_init(struct drm_device *dev)
{
	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;

	/*
	 * set max width and height as default value(4096x4096).
	 * this value would be used to check framebuffer size limitation
	 * at drm_mode_addfb().
	 */
	dev->mode_config.max_width = 4096;
	dev->mode_config.max_height = 4096;

	dev->mode_config.funcs = &exynos_drm_mode_config_funcs;
}
