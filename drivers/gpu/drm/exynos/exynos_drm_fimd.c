/* exynos_drm_fimd.c
 *
 * Copyright (C) 2011 Samsung Electronics Co.Ltd
 * Authors:
 *	Joonyoung Shim <jy0922.shim@samsung.com>
 *	Inki Dae <inki.dae@samsung.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */
#include "drmP.h"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>

#include <drm/exynos_drm.h>
#include <plat/regs-fb-v4.h>

#include "exynos_dp_core.h"
#include "exynos_drm_drv.h"
#include "exynos_drm_crtc.h"
#include "exynos_drm_fbdev.h"
#include "exynos_drm_display.h"

/*
 * FIMD is stand for Fully Interactive Mobile Display and
 * as a display controller, it transfers contents drawn on memory
 * to a LCD Panel through Display Interfaces such as RGB or
 * CPU Interface.
 */

/* position control register for hardware window 0, 2 ~ 4.*/
#define VIDOSD_A(win)		(VIDOSD_BASE + 0x00 + (win) * 16)
#define VIDOSD_B(win)		(VIDOSD_BASE + 0x04 + (win) * 16)
/* size control register for hardware window 0. */
#define VIDOSD_C_SIZE_W0	(VIDOSD_BASE + 0x08)
/* alpha control register for hardware window 1 ~ 4. */
#define VIDOSD_C(win)		(VIDOSD_BASE + 0x18 + (win) * 16)
/* size control register for hardware window 1 ~ 4. */
#define VIDOSD_D(win)		(VIDOSD_BASE + 0x0C + (win) * 16)

#define VIDWx_BUF_START(win, buf)	(VIDW_BUF_START(buf) + (win) * 8)
#define VIDWx_BUF_END(win, buf)		(VIDW_BUF_END(buf) + (win) * 8)
#define VIDWx_BUF_SIZE(win, buf)	(VIDW_BUF_SIZE(buf) + (win) * 4)

/* color key control register for hardware window 1 ~ 4. */
#define WKEYCON0_BASE(x)		((WKEYCON0 + 0x140) + (x * 8))
/* color key value register for hardware window 1 ~ 4. */
#define WKEYCON1_BASE(x)		((WKEYCON1 + 0x140) + (x * 8))

/* FIMD has totally five hardware windows. */
#define WINDOWS_NR	5

#define get_fimd_context(dev)	platform_get_drvdata(to_platform_device(dev))

struct fimd_win_data {
	unsigned int		offset_x;
	unsigned int		offset_y;
	unsigned int		ovl_width;
	unsigned int		ovl_height;
	unsigned int		fb_width;
	unsigned int		fb_height;
	unsigned int		fb_pitch;
	unsigned int		bpp;

	/*
	 * TODO(seanpaul): These fields only really make sense for the 'default
	 * window', but we go through the same path for updating a plane as we
	 * do for setting the crtc mode. If/when/once these are decoupled, this
	 * code should be refactored to seperate plane/overlay/window settings
	 * with crtc settings.
	 */
	unsigned int		hsync_len;
	unsigned int		hmargin;
	unsigned int		vsync_len;
	unsigned int		vmargin;

	dma_addr_t		dma_addr;
	void __iomem		*vaddr;
	unsigned int		buf_offsize;
	unsigned int		line_size;	/* bytes */
	bool			enabled;
	bool			win_suspended;
};

struct fimd_context {
	struct drm_device		*drm_dev;
	enum disp_panel_type		panel_type;
	int				pipe;
	int				irq;
	struct drm_crtc			*crtc;
	struct clk			*bus_clk;
	struct clk			*lcd_clk;
	struct resource			*regs_res;
	void __iomem			*regs;
	void __iomem			*regs_mie;
	struct fimd_win_data		win_data[WINDOWS_NR];
	unsigned int			default_win;
	unsigned long			irq_flags;
	u32				vidcon0;
	u32				vidcon1;
	bool				suspended;
	struct mutex			lock;
	u32				clkdiv;

	struct exynos_drm_panel_info *panel;
};

static struct exynos_drm_panel_info *fimd_get_panel(void *ctx)
{
	struct fimd_context *fimd_ctx = ctx;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	return fimd_ctx->panel;
}

static int fimd_power_on(struct fimd_context *ctx, bool enable);

static int fimd_power(void *ctx, int mode)
{
	struct fimd_context *fimd_ctx = ctx;
	bool enable;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	switch (mode) {
	case DRM_MODE_DPMS_ON:
		enable = true;
		break;
	case DRM_MODE_DPMS_STANDBY:
	case DRM_MODE_DPMS_SUSPEND:
	case DRM_MODE_DPMS_OFF:
		enable = false;
		break;
	default:
		DRM_DEBUG_KMS("unspecified mode %d\n", mode);
		return -EINVAL;
	}

	fimd_power_on(fimd_ctx, enable);

	return 0;
}

static void fimd_commit(void *ctx)
{
	struct fimd_context *fimd_ctx = ctx;
	struct fimd_win_data *win_data;
	u32 val;

	if (fimd_ctx->suspended)
		return;
	win_data = &fimd_ctx->win_data[fimd_ctx->default_win];

	if (!win_data->ovl_width || !win_data->ovl_height)
		return;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	/* setup polarity values from machine code. */
	writel(fimd_ctx->vidcon1, fimd_ctx->regs + VIDCON1);

	/* setup vertical timing values. */
	val = VIDTCON0_VBPD(win_data->vmargin - 1) |
		VIDTCON0_VFPD(win_data->vmargin - 1) |
		VIDTCON0_VSPW(win_data->vsync_len - 1);
	writel(val, fimd_ctx->regs + VIDTCON0);

	/* setup horizontal timing values.  */
	val = VIDTCON1_HBPD(win_data->hmargin - 1) |
		VIDTCON1_HFPD(win_data->hmargin - 1) |
		VIDTCON1_HSPW(win_data->hsync_len - 1);
	writel(val, fimd_ctx->regs + VIDTCON1);

	/* setup horizontal and vertical display size. */
	val = VIDTCON2_LINEVAL(win_data->ovl_height - 1) |
	       VIDTCON2_HOZVAL(win_data->ovl_width - 1);
	writel(val, fimd_ctx->regs + VIDTCON2);

	/* setup clock source, clock divider, enable dma. */
	val = fimd_ctx->vidcon0;
	val &= ~(VIDCON0_CLKVAL_F_MASK | VIDCON0_CLKDIR);

	if (fimd_ctx->clkdiv)
		val |= VIDCON0_CLKVAL_F(fimd_ctx->clkdiv - 1) | VIDCON0_CLKDIR;
	else
		val &= ~VIDCON0_CLKDIR;	/* 1:1 clock */

	/*
	 * fields of register with prefix '_F' would be updated
	 * at vsync(same as dma start)
	 */
	val |= VIDCON0_ENVID | VIDCON0_ENVID_F;
	writel(val, fimd_ctx->regs + VIDCON0);
}

static int fimd_enable_vblank(void *ctx, int pipe)
{
	struct fimd_context *fimd_ctx = ctx;
	u32 val;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	fimd_ctx->pipe = pipe;

	if (fimd_ctx->suspended)
		return -EPERM;

	if (!test_and_set_bit(0, &fimd_ctx->irq_flags)) {
		val = readl(fimd_ctx->regs + VIDINTCON0);

		val |= VIDINTCON0_INT_ENABLE;
		val |= VIDINTCON0_INT_FRAME;

		val &= ~VIDINTCON0_FRAMESEL0_MASK;
		val |= VIDINTCON0_FRAMESEL0_VSYNC;
		val &= ~VIDINTCON0_FRAMESEL1_MASK;
		val |= VIDINTCON0_FRAMESEL1_NONE;

		writel(val, fimd_ctx->regs + VIDINTCON0);
	}

	return 0;
}

static void fimd_disable_vblank(void *ctx)
{
	struct fimd_context *fimd_ctx = ctx;
	u32 val;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (fimd_ctx->suspended)
		return;

	if (test_and_clear_bit(0, &fimd_ctx->irq_flags)) {
		val = readl(fimd_ctx->regs + VIDINTCON0);

		val &= ~VIDINTCON0_INT_FRAME;
		val &= ~VIDINTCON0_INT_ENABLE;

		writel(val, fimd_ctx->regs + VIDINTCON0);
	}
}

static int fimd_calc_clkdiv(struct fimd_context *fimd_ctx,
			    struct fimd_win_data *win_data,
			    int refresh)
{
	unsigned long clk = clk_get_rate(fimd_ctx->lcd_clk);
	u32 retrace;
	u32 clkdiv;
	u32 best_framerate = 0;
	u32 framerate;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	retrace = win_data->hmargin * 2 + win_data->hsync_len +
				win_data->ovl_width;
	retrace *= win_data->vmargin * 2 + win_data->vsync_len +
				win_data->ovl_height;

	/* default framerate is 60Hz */
	if (!refresh)
		refresh = 60;

	clk /= retrace;

	for (clkdiv = 1; clkdiv < 0x100; clkdiv++) {
		int tmp;

		/* get best framerate */
		framerate = clk / clkdiv;
		tmp = refresh - framerate;
		if (tmp < 0) {
			best_framerate = framerate;
			continue;
		} else {
			if (!best_framerate)
				best_framerate = framerate;
			else if (tmp < (best_framerate - framerate))
				best_framerate = framerate;
			break;
		}
	}
	return clkdiv;
}

static void fimd_win_mode_set(void *ctx, struct exynos_drm_overlay *overlay)
{
	struct fimd_context *fimd_ctx = ctx;
	struct fimd_win_data *win_data;
	int win;
	unsigned long offset;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (!overlay) {
		DRM_ERROR("overlay is NULL\n");
		return;
	}

	win = overlay->zpos;
	if (win == DEFAULT_ZPOS)
		win = fimd_ctx->default_win;

	if (win < 0 || win > WINDOWS_NR)
		return;

	offset = overlay->fb_x * (overlay->bpp >> 3);
	offset += overlay->fb_y * overlay->fb_pitch;

	DRM_DEBUG_KMS("offset = 0x%lx, pitch = %x\n",
		offset, overlay->fb_pitch);

	win_data = &fimd_ctx->win_data[win];

	win_data->offset_x = overlay->crtc_x;
	win_data->offset_y = overlay->crtc_y;
	win_data->ovl_width = overlay->crtc_width;
	win_data->ovl_height = overlay->crtc_height;
	win_data->fb_width = overlay->fb_width;
	win_data->fb_height = overlay->fb_height;
	win_data->fb_pitch = overlay->fb_pitch;
	win_data->hsync_len = overlay->crtc_hsync_len;
	win_data->hmargin = (overlay->crtc_htotal - overlay->crtc_width -
				overlay->crtc_hsync_len) / 2;
	win_data->vsync_len = overlay->crtc_vsync_len;
	win_data->vmargin = (overlay->crtc_vtotal - overlay->crtc_height -
				overlay->crtc_vsync_len) / 2;
	win_data->dma_addr = overlay->dma_addr[0] + offset;
	win_data->vaddr = overlay->vaddr[0] + offset;
	win_data->bpp = overlay->bpp;
	win_data->buf_offsize = overlay->fb_pitch -
		(overlay->fb_width * (overlay->bpp >> 3));
	win_data->line_size = overlay->fb_width * (overlay->bpp >> 3);

	if (win == fimd_ctx->default_win)
		fimd_ctx->clkdiv = fimd_calc_clkdiv(fimd_ctx, win_data,
					overlay->refresh);

	DRM_DEBUG_KMS("offset_x = %d, offset_y = %d\n",
			win_data->offset_x, win_data->offset_y);
	DRM_DEBUG_KMS("ovl_width = %d, ovl_height = %d\n",
			win_data->ovl_width, win_data->ovl_height);
	DRM_DEBUG_KMS("paddr = 0x%lx, vaddr = 0x%lx\n",
			(unsigned long)win_data->dma_addr,
			(unsigned long)win_data->vaddr);
	DRM_DEBUG_KMS("fb_width = %d, crtc_width = %d\n",
			overlay->fb_width, overlay->crtc_width);
}

static void fimd_win_set_pixfmt(struct fimd_context *fimd_ctx, unsigned int win)
{
	struct fimd_win_data *win_data = &fimd_ctx->win_data[win];
	unsigned long val;
	unsigned long bytes;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	val = WINCONx_ENWIN;

	switch (win_data->bpp) {
	case 1:
		val |= WINCON0_BPPMODE_1BPP;
		val |= WINCONx_BITSWP;
		bytes = win_data->fb_width >> 3;
		break;
	case 2:
		val |= WINCON0_BPPMODE_2BPP;
		val |= WINCONx_BITSWP;
		bytes = win_data->fb_width >> 2;
		break;
	case 4:
		val |= WINCON0_BPPMODE_4BPP;
		val |= WINCONx_BITSWP;
		bytes = win_data->fb_width >> 1;
		break;
	case 8:
		val |= WINCON0_BPPMODE_8BPP_PALETTE;
		val |= WINCONx_BYTSWP;
		bytes = win_data->fb_width;
		break;
	case 16:
		val |= WINCON0_BPPMODE_16BPP_565;
		val |= WINCONx_HAWSWP;
		bytes = win_data->fb_width << 1;
		break;
	case 24:
		val |= WINCON0_BPPMODE_24BPP_888;
		val |= WINCONx_WSWP;
		bytes = win_data->fb_width * 3;
		break;
	case 32:
		val |= WINCON1_BPPMODE_28BPP_A4888
			| WINCON1_BLD_PIX | WINCON1_ALPHA_SEL;
		val |= WINCONx_WSWP;
		bytes = win_data->fb_width << 2;
		break;
	default:
		DRM_DEBUG_KMS("invalid pixel size so using unpacked 24bpp.\n");
		bytes = win_data->fb_width * 3;
		val |= WINCON0_BPPMODE_24BPP_888;
		val |= WINCONx_WSWP;
		break;
	}

	/*
	 * Adjust the burst size based on the number of bytes to be read.
	 * Each WORD of the BURST is 8 bytes long. There are 3 BURST sizes
	 * supported by fimd.
	 * WINCONx_BURSTLEN_4WORD = 32 bytes
	 * WINCONx_BURSTLEN_8WORD = 64 bytes
	 * WINCONx_BURSTLEN_16WORD = 128 bytes
	 */
	if (win_data->fb_width <= 64)
		val |= WINCONx_BURSTLEN_4WORD;
	else
		val |= WINCONx_BURSTLEN_16WORD;

	DRM_DEBUG_KMS("bpp = %d\n", win_data->bpp);

	writel(val, fimd_ctx->regs + WINCON(win));
}

static void fimd_win_set_colkey(struct fimd_context *fimd_ctx, unsigned int win)
{
	unsigned int keycon0 = 0, keycon1 = 0;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	keycon0 = ~(WxKEYCON0_KEYBL_EN | WxKEYCON0_KEYEN_F |
			WxKEYCON0_DIRCON) | WxKEYCON0_COMPKEY(0);

	keycon1 = WxKEYCON1_COLVAL(0xffffffff);

	writel(keycon0, fimd_ctx->regs + WKEYCON0_BASE(win));
	writel(keycon1, fimd_ctx->regs + WKEYCON1_BASE(win));
}

static void mie_set_6bit_dithering(struct fimd_context *ctx, int win)
{
	struct fimd_win_data *win_data = &ctx->win_data[win];
	unsigned long val;
	unsigned int width, height;
	int i;

	width = win_data->ovl_width;
	height = win_data->ovl_height;

	writel(MIE_HRESOL(width) | MIE_VRESOL(height) | MIE_MODE_UI,
			ctx->regs_mie + MIE_CTRL1);

	writel(MIE_WINHADDR0(0) | MIE_WINHADDR1(width),
			ctx->regs_mie + MIE_WINHADDR);
	writel(MIE_WINVADDR0(0) | MIE_WINVADDR1(height),
			ctx->regs_mie + MIE_WINVADDR);

	val = (width + win_data->hmargin * 2 + win_data->hsync_len) *
			(height + win_data->vmargin * 2 + win_data->vsync_len) /
			(MIE_PWMCLKVAL + 1);

	writel(PWMCLKCNT(val), ctx->regs_mie + MIE_PWMCLKCNT);

	writel((MIE_VBPD(win_data->vmargin)) | MIE_VFPD(win_data->vmargin) |
		MIE_VSPW(win_data->vsync_len), ctx->regs_mie + MIE_PWMVIDTCON1);

	writel(MIE_HBPD(win_data->hmargin) | MIE_HFPD(win_data->hmargin) |
		MIE_HSPW(win_data->hsync_len), ctx->regs_mie + MIE_PWMVIDTCON2);

	writel(MIE_DITHCON_EN | MIE_RGB6MODE, ctx->regs_mie + MIE_AUXCON);

	/* Bypass MIE image brightness enhancement */
	for (i = 0; i <= 0x30; i += 4) {
		writel(0, ctx->regs_mie + 0x100 + i);
		writel(0, ctx->regs_mie + 0x200 + i);
	}
}

static void fimd_win_commit(void *ctx, int zpos)
{
	struct fimd_context *fimd_ctx = ctx;
	struct fimd_win_data *win_data;
	int win = zpos;
	unsigned long val, alpha, size;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (fimd_ctx->suspended)
		return;

	if (win == DEFAULT_ZPOS)
		win = fimd_ctx->default_win;

	if (win < 0 || win > WINDOWS_NR)
		return;

	win_data = &fimd_ctx->win_data[win];

	/*
	 * SHADOWCON register is used for enabling timing.
	 *
	 * for example, once only width value of a register is set,
	 * if the dma is started then fimd hardware could malfunction so
	 * with protect window setting, the register fields with prefix '_F'
	 * wouldn't be updated at vsync also but updated once unprotect window
	 * is set.
	 */

	/* protect windows */
	val = readl(fimd_ctx->regs + SHADOWCON);
	val |= SHADOWCON_WINx_PROTECT(win);
	writel(val, fimd_ctx->regs + SHADOWCON);

	/* buffer start address */
	val = (unsigned long)win_data->dma_addr;
	writel(val, fimd_ctx->regs + VIDWx_BUF_START(win, 0));

	/* buffer end address */
	size = win_data->fb_height * win_data->fb_pitch;
	val = (unsigned long)(win_data->dma_addr + size);
	writel(val, fimd_ctx->regs + VIDWx_BUF_END(win, 0));

	DRM_DEBUG_KMS("start addr = 0x%lx, end addr = 0x%lx, size = 0x%lx\n",
			(unsigned long)win_data->dma_addr, val, size);
	DRM_DEBUG_KMS("ovl_width = %d, ovl_height = %d\n",
			win_data->ovl_width, win_data->ovl_height);

	/* buffer size */
	val = VIDW_BUF_SIZE_OFFSET(win_data->buf_offsize) |
		VIDW_BUF_SIZE_PAGEWIDTH(win_data->line_size);
	writel(val, fimd_ctx->regs + VIDWx_BUF_SIZE(win, 0));

	/* OSD position */
	val = VIDOSDxA_TOPLEFT_X(win_data->offset_x) |
		VIDOSDxA_TOPLEFT_Y(win_data->offset_y);
	writel(val, fimd_ctx->regs + VIDOSD_A(win));

	val = VIDOSDxB_BOTRIGHT_X(win_data->offset_x +
					win_data->ovl_width - 1) |
		VIDOSDxB_BOTRIGHT_Y(win_data->offset_y +
					win_data->ovl_height - 1);
	writel(val, fimd_ctx->regs + VIDOSD_B(win));

	DRM_DEBUG_KMS("osd pos: tx = %d, ty = %d, bx = %d, by = %d\n",
			win_data->offset_x, win_data->offset_y,
			win_data->offset_x + win_data->ovl_width - 1,
			win_data->offset_y + win_data->ovl_height - 1);

	/* hardware window 0 doesn't support alpha channel. */
	if (win != 0) {
		/* OSD alpha */
		alpha = VIDISD14C_ALPHA1_R(0xf) |
			VIDISD14C_ALPHA1_G(0xf) |
			VIDISD14C_ALPHA1_B(0xf);

		writel(alpha, fimd_ctx->regs + VIDOSD_C(win));
	}

	/* OSD size */
	if (win != 3 && win != 4) {
		u32 offset = VIDOSD_D(win);
		if (win == 0)
			offset = VIDOSD_C_SIZE_W0;
		val = win_data->ovl_width * win_data->ovl_height;
		writel(val, fimd_ctx->regs + offset);

		DRM_DEBUG_KMS("osd size = 0x%x\n", (unsigned int)val);
	}

	fimd_win_set_pixfmt(fimd_ctx, win);

	/* hardware window 0 doesn't support color key. */
	if (win != 0)
		fimd_win_set_colkey(fimd_ctx, win);

	/* wincon */
	val = readl(fimd_ctx->regs + WINCON(win));
	val |= WINCONx_ENWIN;
	writel(val, fimd_ctx->regs + WINCON(win));

	mie_set_6bit_dithering(fimd_ctx, win);

	/* Enable DMA channel and unprotect windows */
	val = readl(fimd_ctx->regs + SHADOWCON);
	val |= SHADOWCON_CHx_ENABLE(win);
	val &= ~SHADOWCON_WINx_PROTECT(win);
	writel(val, fimd_ctx->regs + SHADOWCON);

	win_data->enabled = true;
}

static void fimd_win_disable(void *ctx, int zpos)
{
	struct fimd_context *fimd_ctx = ctx;
	struct fimd_win_data *win_data;
	int win = zpos;
	u32 val;

	DRM_DEBUG_KMS("%s\n", __FILE__);
	if (fimd_ctx->suspended)
		return;

	if (win == DEFAULT_ZPOS)
		win = fimd_ctx->default_win;

	if (win < 0 || win > WINDOWS_NR)
		return;

	win_data = &fimd_ctx->win_data[win];

	/* protect windows */
	val = readl(fimd_ctx->regs + SHADOWCON);
	val |= SHADOWCON_WINx_PROTECT(win);
	writel(val, fimd_ctx->regs + SHADOWCON);

	/* wincon */
	val = readl(fimd_ctx->regs + WINCON(win));
	val &= ~WINCONx_ENWIN;
	writel(val, fimd_ctx->regs + WINCON(win));

	/* unprotect windows */
	val = readl(fimd_ctx->regs + SHADOWCON);
	val &= ~SHADOWCON_CHx_ENABLE(win);
	val &= ~SHADOWCON_WINx_PROTECT(win);
	writel(val, fimd_ctx->regs + SHADOWCON);

	win_data->enabled = false;
}

static void fimd_apply(void *ctx)
{
	struct fimd_context *fimd_ctx = ctx;
	struct fimd_win_data *win_data;
	int i;

	for (i = 0; i < WINDOWS_NR; i++) {
		win_data = &fimd_ctx->win_data[i];
		if (win_data->enabled)
			fimd_win_commit(ctx, i);
	}
}

static irqreturn_t fimd_irq_handler(int irq, void *arg)
{
	struct fimd_context *fimd_ctx = (struct fimd_context *)arg;
	u32 val;

	val = readl(fimd_ctx->regs + VIDINTCON1);

	if (val & VIDINTCON1_INT_FRAME)
		/* VSYNC interrupt */
		writel(VIDINTCON1_INT_FRAME, fimd_ctx->regs + VIDINTCON1);

	/* check the crtc is detached already from encoder */
	if (fimd_ctx->pipe < 0)
		goto out;

	drm_handle_vblank(fimd_ctx->drm_dev, fimd_ctx->pipe);
	exynos_drm_crtc_finish_pageflip(fimd_ctx->drm_dev, fimd_ctx->pipe);

out:
	return IRQ_HANDLED;
}

static int fimd_subdrv_probe(void *ctx, struct drm_device *drm_dev)
{
	struct fimd_context *fimd_ctx = ctx;
	DRM_DEBUG_KMS("%s\n", __FILE__);

	/*
	 * enable drm irq mode.
	 * - with irq_enabled = 1, we can use the vblank feature.
	 *
	 * P.S. note that we wouldn't use drm irq handler but
	 *	just specific driver own one instead because
	 *	drm framework supports only one irq handler.
	 */
	drm_dev->irq_enabled = 1;

	/*
	 * with vblank_disable_allowed = 1, vblank interrupt will be disabled
	 * by drm timer once a current process gives up ownership of
	 * vblank event.(after drm_vblank_put function is called)
	 */
	drm_dev->vblank_disable_allowed = 1;

	fimd_ctx->drm_dev = drm_dev;

	return 0;
}

static struct exynos_controller_ops fimd_controller_ops = {
	.subdrv_probe = fimd_subdrv_probe,
	.get_panel = fimd_get_panel,
	.enable_vblank = fimd_enable_vblank,
	.disable_vblank = fimd_disable_vblank,
	.power = fimd_power,
	.mode_set = fimd_win_mode_set,
	.commit = fimd_commit,
	.apply = fimd_apply,
	.win_commit = fimd_win_commit,
	.win_disable = fimd_win_disable,
};

static void fimd_clear_win(struct fimd_context *ctx, int win)
{
	u32 val;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	writel(0, ctx->regs + WINCON(win));
	writel(0, ctx->regs + VIDOSD_A(win));
	writel(0, ctx->regs + VIDOSD_B(win));
	writel(0, ctx->regs + VIDOSD_C(win));

	if (win == 1 || win == 2)
		writel(0, ctx->regs + VIDOSD_D(win));

	val = readl(ctx->regs + SHADOWCON);
	val &= ~SHADOWCON_WINx_PROTECT(win);
	writel(val, ctx->regs + SHADOWCON);
}

/*
 * Disables all windows for suspend, keeps track of which ones were enabled.
 */
static void fimd_window_suspend(struct fimd_context *fimd_ctx)
{
	struct fimd_win_data *win_data;
	int i;

	for(i = 0; i < WINDOWS_NR; i++)
	{
		win_data = &fimd_ctx->win_data[i];
		win_data->win_suspended = win_data->enabled;
		fimd_win_disable(fimd_ctx, i);
	}
}

/*
 * Resumes the suspended windows.
 */
static void fimd_window_resume(struct fimd_context *fimd_ctx)
{
	struct fimd_win_data *win_data;
	int i;

	for(i = 0; i < WINDOWS_NR; i++)
	{
		win_data = &fimd_ctx->win_data[i];
		if (win_data->win_suspended) {
			fimd_win_commit(fimd_ctx, i);
			win_data->win_suspended = false;
		}
	}
}

static int fimd_power_on(struct fimd_context *fimd_ctx, bool enable)
{
	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (enable) {
		int ret;

		ret = clk_enable(fimd_ctx->bus_clk);
		if (ret < 0)
			return ret;

		ret = clk_enable(fimd_ctx->lcd_clk);
		if  (ret < 0) {
			clk_disable(fimd_ctx->bus_clk);
			return ret;
		}

		fimd_ctx->suspended = false;

		/* if vblank was enabled status, enable it again. */
		if (test_and_clear_bit(0, &fimd_ctx->irq_flags))
			fimd_enable_vblank(fimd_ctx, fimd_ctx->pipe);

		fimd_apply(fimd_ctx);
		fimd_commit(fimd_ctx);

		if (fimd_ctx->panel_type == DP_LCD)
			writel(MIE_CLK_ENABLE, fimd_ctx->regs + DPCLKCON);

		fimd_window_resume(fimd_ctx);
	} else {
		/*
		 * We need to make sure that all windows are disabled before we
		 * suspend that connector. Otherwise we might try to scan from
		 * a destroyed buffer later.
		 */
		fimd_window_suspend(fimd_ctx);

		if (fimd_ctx->panel_type == DP_LCD)
			writel(0, fimd_ctx->regs + DPCLKCON);

		clk_disable(fimd_ctx->lcd_clk);
		clk_disable(fimd_ctx->bus_clk);

		fimd_ctx->suspended = true;
	}

	return 0;
}

#ifdef CONFIG_EXYNOS_IOMMU
static int iommu_init(struct platform_device *pdev)
{
	struct platform_device *pds;

	pds = find_sysmmu_dt(pdev, "sysmmu");
	if (pds==NULL) {
		printk(KERN_ERR "No sysmmu found\n");
		return -1;
	}

	platform_set_sysmmu(&pds->dev, &pdev->dev);
	/*
	 * Due to the ordering in Makefile, this should be called first
	 * (before exynos_drm_drv.c and exynos_mixer.c and actually create
	 * the common mapping instead of reusing it. Ensure this assumption
	 * holds.
	 */
	WARN_ON(exynos_drm_common_mapping);
	exynos_drm_common_mapping = s5p_create_iommu_mapping(&pdev->dev,
					0x20000000, SZ_256M, 4,
					exynos_drm_common_mapping);

	if (!exynos_drm_common_mapping) {
		printk(KERN_ERR "IOMMU mapping not created\n");
		return -1;
	}

	return 0;
}

static void iommu_deinit(struct platform_device *pdev)
{
	s5p_destroy_iommu_mapping(&pdev->dev);
	DRM_DEBUG("released the IOMMU mapping\n");
}
#endif

static int __devinit fimd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fimd_context *fimd_ctx;
	struct exynos_drm_fimd_pdata *pdata;
	struct resource *res;
	struct clk *clk_parent;
	int win;
	int ret = -EINVAL;

#ifdef CONFIG_EXYNOS_IOMMU
	ret = iommu_init(pdev);
	if (ret < 0) {
		dev_err(dev, "failed to initialize IOMMU\n");
		return -ENODEV;
	}
#endif
	DRM_DEBUG_KMS("%s\n", __FILE__);

	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(dev, "no platform data specified\n");
		return -EINVAL;
	}

	fimd_ctx = kzalloc(sizeof(*fimd_ctx), GFP_KERNEL);
	if (!fimd_ctx)
		return -ENOMEM;

	fimd_ctx->panel_type = pdata->panel_type;
	fimd_ctx->pipe = -1;

	fimd_ctx->bus_clk = clk_get(dev, "fimd");
	if (IS_ERR(fimd_ctx->bus_clk)) {
		dev_err(dev, "failed to get bus clock\n");
		ret = PTR_ERR(fimd_ctx->bus_clk);
		goto err_clk_get;
	}

	fimd_ctx->lcd_clk = clk_get(dev, "sclk_fimd");
	if (IS_ERR(fimd_ctx->lcd_clk)) {
		dev_err(dev, "failed to get lcd clock\n");
		ret = PTR_ERR(fimd_ctx->lcd_clk);
		goto err_bus_clk;
	}

	clk_parent = clk_get(NULL, "sclk_vpll");
	if (IS_ERR(clk_parent)) {
		ret = PTR_ERR(clk_parent);
		goto err_clk;
	}

	if (clk_set_parent(fimd_ctx->lcd_clk, clk_parent)) {
		ret = PTR_ERR(fimd_ctx->lcd_clk);
		goto err_clk;
	}

	if (clk_set_rate(fimd_ctx->lcd_clk, pdata->clock_rate)) {
		ret = PTR_ERR(fimd_ctx->lcd_clk);
		goto err_clk;
	}

	clk_put(clk_parent);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "failed to find registers\n");
		ret = -ENOENT;
		goto err_clk;
	}

	fimd_ctx->regs_res = request_mem_region(res->start, resource_size(res),
					   dev_name(dev));
	if (!fimd_ctx->regs_res) {
		dev_err(dev, "failed to claim register region\n");
		ret = -ENOENT;
		goto err_clk;
	}

	fimd_ctx->regs = ioremap(res->start, resource_size(res));
	if (!fimd_ctx->regs) {
		dev_err(dev, "failed to map registers\n");
		ret = -ENXIO;
		goto err_req_region_io;
	}

	fimd_ctx->regs_mie = ioremap(MIE_BASE_ADDRESS, 0x400);
	if (!fimd_ctx->regs_mie) {
		dev_err(dev, "failed to map registers\n");
		ret = -ENXIO;
		goto err_req_region_io_mie;
	}

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 1);
	if (!res) {
		dev_err(dev, "irq request failed.\n");
		goto err_req_region_irq;
	}

	fimd_ctx->irq = res->start;

	ret = request_irq(fimd_ctx->irq, fimd_irq_handler, 0, "drm_fimd",
			fimd_ctx);
	if (ret < 0) {
		dev_err(dev, "irq request failed.\n");
		goto err_req_irq;
	}

	fimd_ctx->vidcon0 = pdata->vidcon0;
	fimd_ctx->vidcon1 = pdata->vidcon1;
	fimd_ctx->default_win = pdata->default_win;
	fimd_ctx->panel = &pdata->panel;

	mutex_init(&fimd_ctx->lock);

	platform_set_drvdata(pdev, fimd_ctx);

	fimd_power(fimd_ctx, DRM_MODE_DPMS_ON);

	for (win = 0; win < WINDOWS_NR; win++)
		fimd_clear_win(fimd_ctx, win);

	if (fimd_ctx->panel_type == DP_LCD)
		writel(MIE_CLK_ENABLE, fimd_ctx->regs + DPCLKCON);

	exynos_display_attach_controller(EXYNOS_DRM_DISPLAY_TYPE_FIMD,
			&fimd_controller_ops, fimd_ctx);

	return 0;

err_req_irq:
err_req_region_irq:
	iounmap(fimd_ctx->regs_mie);

err_req_region_io_mie:
	iounmap(fimd_ctx->regs);

err_req_region_io:
	release_resource(fimd_ctx->regs_res);
	kfree(fimd_ctx->regs_res);

err_clk:
	clk_disable(fimd_ctx->lcd_clk);
	clk_put(fimd_ctx->lcd_clk);

err_bus_clk:
	clk_disable(fimd_ctx->bus_clk);
	clk_put(fimd_ctx->bus_clk);

err_clk_get:
#ifdef CONFIG_EXYNOS_IOMMU
	iommu_deinit(pdev);
#endif
	kfree(fimd_ctx);
	return ret;
}

static int __devexit fimd_remove(struct platform_device *pdev)
{
	struct fimd_context *ctx = platform_get_drvdata(pdev);

	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (ctx->suspended)
		goto out;

	clk_disable(ctx->lcd_clk);
	clk_disable(ctx->bus_clk);

	fimd_power(ctx, DRM_MODE_DPMS_OFF);

out:
	clk_put(ctx->lcd_clk);
	clk_put(ctx->bus_clk);

	iounmap(ctx->regs_mie);
	iounmap(ctx->regs);
	release_resource(ctx->regs_res);
	kfree(ctx->regs_res);
	free_irq(ctx->irq, ctx);
#ifdef CONFIG_EXYNOS_IOMMU
	iommu_deinit(pdev);
#endif
	kfree(ctx);

	return 0;
}

static struct platform_device_id exynos_drm_driver_ids[] = {
	{
		.name		= "exynos4-fb",
	}, {
		.name		= "exynos5-fb",
	},
	{},
};
MODULE_DEVICE_TABLE(platform, exynos_drm_driver_ids);

struct platform_driver fimd_driver = {
	.probe		= fimd_probe,
	.remove		= __devexit_p(fimd_remove),
	.id_table       = exynos_drm_driver_ids,
	.driver		= {
		.name	= "exynos-drm-fimd",
		.owner	= THIS_MODULE,
	},
};
