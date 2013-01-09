/*
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Clock support for EXYNOS5 SoCs
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/syscore_ops.h>

#include <plat/cpu-freq.h>
#include <plat/clock.h>
#include <plat/cpu.h>
#include <plat/pll.h>
#include <plat/s5p-clock.h>
#include <plat/clock-clksrc.h>
#include <plat/devs.h>
#include <plat/pm.h>
#include <plat/cpu.h>

#include <mach/map.h>
#include <mach/regs-clock.h>
#include <mach/regs-audss.h>
#include <mach/sysmmu.h>

#include "common.h"

#ifdef CONFIG_PM_SLEEP
static struct sleep_save exynos5_clock_save[] = {
	SAVE_ITEM(EXYNOS5_CLKSRC_MASK_TOP),
	SAVE_ITEM(EXYNOS5_CLKSRC_MASK_GSCL),
	SAVE_ITEM(EXYNOS5_CLKSRC_MASK_DISP1_0),
	SAVE_ITEM(EXYNOS5_CLKSRC_MASK_FSYS),
	SAVE_ITEM(EXYNOS5_CLKSRC_MASK_MAUDIO),
	SAVE_ITEM(EXYNOS5_CLKSRC_MASK_PERIC0),
	SAVE_ITEM(EXYNOS5_CLKSRC_MASK_PERIC1),
	SAVE_ITEM(EXYNOS5_CLKGATE_IP_GSCL),
	SAVE_ITEM(EXYNOS5_CLKGATE_IP_DISP1),
	SAVE_ITEM(EXYNOS5_CLKGATE_IP_MFC),
	SAVE_ITEM(EXYNOS5_CLKGATE_IP_G3D),
	SAVE_ITEM(EXYNOS5_CLKGATE_IP_GEN),
	SAVE_ITEM(EXYNOS5_CLKGATE_IP_FSYS),
	SAVE_ITEM(EXYNOS5_CLKGATE_IP_GPS),
	SAVE_ITEM(EXYNOS5_CLKGATE_IP_PERIC),
	SAVE_ITEM(EXYNOS5_CLKGATE_IP_PERIS),
	SAVE_ITEM(EXYNOS5_CLKGATE_BLOCK),
	SAVE_ITEM(EXYNOS5_CLKDIV_TOP0),
	SAVE_ITEM(EXYNOS5_CLKDIV_TOP1),
	SAVE_ITEM(EXYNOS5_CLKDIV_GSCL),
	SAVE_ITEM(EXYNOS5_CLKDIV_DISP1_0),
	SAVE_ITEM(EXYNOS5_CLKDIV_GEN),
	SAVE_ITEM(EXYNOS5_CLKDIV_MAUDIO),
	SAVE_ITEM(EXYNOS5_CLKDIV_FSYS0),
	SAVE_ITEM(EXYNOS5_CLKDIV_FSYS1),
	SAVE_ITEM(EXYNOS5_CLKDIV_FSYS2),
	SAVE_ITEM(EXYNOS5_CLKDIV_FSYS3),
	SAVE_ITEM(EXYNOS5_CLKDIV_PERIC0),
	SAVE_ITEM(EXYNOS5_CLKDIV_PERIC1),
	SAVE_ITEM(EXYNOS5_CLKDIV_PERIC2),
	SAVE_ITEM(EXYNOS5_CLKDIV_PERIC3),
	SAVE_ITEM(EXYNOS5_CLKDIV_PERIC4),
	SAVE_ITEM(EXYNOS5_CLKDIV_PERIC5),
	SAVE_ITEM(EXYNOS5_SCLK_DIV_ISP),
	SAVE_ITEM(EXYNOS5_CLKSRC_TOP0),
	SAVE_ITEM(EXYNOS5_CLKSRC_TOP1),
	SAVE_ITEM(EXYNOS5_CLKSRC_TOP2),
	SAVE_ITEM(EXYNOS5_CLKSRC_TOP3),
	SAVE_ITEM(EXYNOS5_CLKSRC_GSCL),
	SAVE_ITEM(EXYNOS5_CLKSRC_DISP1_0),
	SAVE_ITEM(EXYNOS5_CLKSRC_MAUDIO),
	SAVE_ITEM(EXYNOS_CLKSRC_AUDSS),
	SAVE_ITEM(EXYNOS_CLKDIV_AUDSS),
	SAVE_ITEM(EXYNOS_CLKGATE_AUDSS),
	SAVE_ITEM(EXYNOS5_CLKSRC_FSYS),
	SAVE_ITEM(EXYNOS5_CLKSRC_PERIC0),
	SAVE_ITEM(EXYNOS5_CLKSRC_PERIC1),
	SAVE_ITEM(EXYNOS5_SCLK_SRC_ISP),
	SAVE_ITEM(EXYNOS5_EPLL_CON0),
	SAVE_ITEM(EXYNOS5_EPLL_CON1),
	SAVE_ITEM(EXYNOS5_EPLL_CON2),
	SAVE_ITEM(EXYNOS5_VPLL_CON0),
	SAVE_ITEM(EXYNOS5_VPLL_CON1),
	SAVE_ITEM(EXYNOS5_VPLL_CON2),
	SAVE_ITEM(EXYNOS5_PWR_CTRL1),
	SAVE_ITEM(EXYNOS5_PWR_CTRL2),
};
#endif

static struct clk exynos5_clk_sclk_dptxphy = {
	.name		= "sclk_dptx",
};

static struct clk exynos5_clk_sclk_hdmi24m = {
	.name		= "sclk_hdmi24m",
	.rate		= 24000000,
};

static struct clk exynos5_clk_sclk_hdmi27m = {
	.name		= "sclk_hdmi27m",
	.rate		= 27000000,
};

static struct clk exynos5_clk_sclk_hdmiphy = {
	.name		= "sclk_hdmiphy",
};

static struct clk exynos5_clk_sclk_usbphy = {
	.name		= "sclk_usbphy",
	.rate		= 48000000,
};

struct clksrc_clk exynos5_clk_audiocdclk0 = {
	.clk	= {
		.name		= "audiocdclk",
		.rate		= 16934400,
	},
};

static int exynos5_clksrc_mask_top_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKSRC_MASK_TOP, clk, enable);
}

static int exynos5_clksrc_mask_disp1_0_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKSRC_MASK_DISP1_0, clk, enable);
}

static int exynos5_clksrc_mask_fsys_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKSRC_MASK_FSYS, clk, enable);
}

static int exynos5_clk_ip_gscl_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKGATE_IP_GSCL, clk, enable);
}

static int exynos5_clksrc_mask_gscl_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKSRC_MASK_GSCL, clk, enable);
}

static int exynos5_clksrc_mask_peric0_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKSRC_MASK_PERIC0, clk, enable);
}

static int exynos5_clk_ip_core_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKGATE_IP_CORE, clk, enable);
}

static int exynos5_clk_ip_disp1_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKGATE_IP_DISP1, clk, enable);
}

static int exynos5_clk_ip_fsys_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKGATE_IP_FSYS, clk, enable);
}

static int exynos5_clk_block_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKGATE_BLOCK, clk, enable);
}

static int exynos5_clk_ip_gen_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKGATE_IP_GEN, clk, enable);
}

static int exynos5_clk_ip_gps_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKGATE_IP_GPS, clk, enable);
}

static int exynos5_clksrc_mask_maudio_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKSRC_MASK_MAUDIO, clk, enable);
}

static int exynos5_clk_audss_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS_CLKGATE_AUDSS, clk, enable);
}

static int exynos5_clk_ip_mfc_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKGATE_IP_MFC, clk, enable);
}

static int exynos5_clk_ip_g3d_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKGATE_IP_G3D, clk, enable);
}

static int exynos5_clk_ip_peric_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKGATE_IP_PERIC, clk, enable);
}

static int exynos5_clk_ip_peris_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKGATE_IP_PERIS, clk, enable);
}

static int exynos5_clksrc_mask_peric1_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKSRC_MASK_PERIC1, clk, enable);
}

static int exynos5_clk_ip_acp_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKGATE_IP_ACP, clk, enable);
}

static int exynos5_clk_ip_isp0_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKGATE_IP_ISP0, clk, enable);
}

static int exynos5_clk_ip_isp1_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(EXYNOS5_CLKGATE_IP_ISP1, clk, enable);
}

static int exynos5_clk_hdmiphy_ctrl(struct clk *clk, int enable)
{
	return s5p_gatectrl(S5P_HDMI_PHY_CONTROL, clk, enable);
}

/* Core list of CMU_CPU side */

/* GPLL clock output */
static struct clk clk_fout_gpll = {
	.name		= "fout_gpll",
	.id		= -1,
};

/* Possible clock sources for GPLL Mux */
static struct clk *clk_src_gpll_list[] = {
	[0] = &clk_fin_gpll,
	[1] = &clk_fout_gpll,
};

static struct clksrc_sources clk_src_gpll = {
	.sources	= clk_src_gpll_list,
	.nr_sources	= ARRAY_SIZE(clk_src_gpll_list),
};

static struct clksrc_clk exynos5_clk_mout_apll = {
	.clk	= {
		.name		= "mout_apll",
	},
	.sources = &clk_src_apll,
	.reg_src = { .reg = EXYNOS5_CLKSRC_CPU, .shift = 0, .size = 1 },
};

static struct clksrc_clk exynos5_clk_sclk_apll = {
	.clk	= {
		.name		= "sclk_apll",
		.parent		= &exynos5_clk_mout_apll.clk,
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_CPU0, .shift = 24, .size = 3 },
};

static struct clk clk_fout_bpll_div2 = {
	.name	= "fout_bpll_div2",
};

static struct clk *exynos5_clkset_mout_bpll_fout_list[] = {
	[0] = &clk_fout_bpll_div2,
	[1] = &clk_fout_bpll,
};

static struct clksrc_sources exynos5_clkset_mout_bpll_fout = {
	.sources	= exynos5_clkset_mout_bpll_fout_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clkset_mout_bpll_fout_list),
};

static struct clksrc_clk exynos5_clk_mout_bpll_fout = {
	.clk	= {
		.name		= "mout_bpll_fout",
	},
	.sources = &exynos5_clkset_mout_bpll_fout,
	.reg_src = { .reg = EXYNOS5_PLL_DIV2_SEL, .shift = 0, .size = 1 },
};

/* Possible clock sources for BPLL Mux */
static struct clk *clk_src_bpll_list[] = {
	[0] = &clk_fin_bpll,
	[1] = &exynos5_clk_mout_bpll_fout.clk,
};

struct clksrc_sources clk_src_bpll = {
	.sources	= clk_src_bpll_list,
	.nr_sources	= ARRAY_SIZE(clk_src_bpll_list),
};

static struct clksrc_clk exynos5_clk_mout_bpll = {
	.clk	= {
		.name		= "mout_bpll",
	},
	.sources = &clk_src_bpll,
	.reg_src = { .reg = EXYNOS5_CLKSRC_CDREX, .shift = 0, .size = 1 },
};

static struct clk *exynos5_clk_src_bpll_user_list[] = {
	[0] = &clk_fin_mpll,
	[1] = &exynos5_clk_mout_bpll.clk,
};

static struct clksrc_sources exynos5_clk_src_bpll_user = {
	.sources	= exynos5_clk_src_bpll_user_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clk_src_bpll_user_list),
};

static struct clksrc_clk exynos5_clk_mout_bpll_user = {
	.clk	= {
		.name		= "mout_bpll_user",
	},
	.sources = &exynos5_clk_src_bpll_user,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP2, .shift = 24, .size = 1 },
};

static struct clksrc_clk exynos5_clk_mout_cpll = {
	.clk	= {
		.name		= "mout_cpll",
	},
	.sources = &clk_src_cpll,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP2, .shift = 8, .size = 1 },
};

static struct clksrc_clk exynos5_clk_mout_epll = {
	.clk	= {
		.name		= "mout_epll",
	},
	.sources = &clk_src_epll,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP2, .shift = 12, .size = 1 },
};

static struct clk clk_fout_mpll_div2 = {
	.name	= "fout_mpll_div2",
};

static struct clk *exynos5_clkset_mout_mpll_fout_list[] = {
	[0] = &clk_fout_mpll_div2,
	[1] = &clk_fout_mpll,
};

static struct clksrc_sources exynos5_clkset_mout_mpll_fout = {
	.sources	= exynos5_clkset_mout_mpll_fout_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clkset_mout_mpll_fout_list),
};

static struct clksrc_clk exynos5_clk_mout_mpll_fout = {
	.clk	= {
		.name		= "mout_mpll_fout",
	},
	.sources = &exynos5_clkset_mout_mpll_fout,
	.reg_src = { .reg = EXYNOS5_PLL_DIV2_SEL, .shift = 4, .size = 1 },
};

static struct clk *exynos5_clk_src_mpll_list[] = {
	[0] = &clk_fin_mpll,
	[1] = &exynos5_clk_mout_mpll_fout.clk,
};

struct clksrc_sources exynos5_clk_src_mpll = {
	.sources	= exynos5_clk_src_mpll_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clk_src_mpll_list),
};

struct clksrc_clk exynos5_clk_mout_mpll = {
	.clk = {
		.name		= "mout_mpll",
	},
	.sources = &exynos5_clk_src_mpll,
	.reg_src = { .reg = EXYNOS5_CLKSRC_CORE1, .shift = 8, .size = 1 },
};

static struct clk *exynos_clkset_vpllsrc_list[] = {
	[0] = &clk_fin_vpll,
	[1] = &exynos5_clk_sclk_hdmi27m,
};

static struct clksrc_sources exynos5_clkset_vpllsrc = {
	.sources	= exynos_clkset_vpllsrc_list,
	.nr_sources	= ARRAY_SIZE(exynos_clkset_vpllsrc_list),
};

static struct clksrc_clk exynos5_clk_vpllsrc = {
	.clk	= {
		.name		= "vpll_src",
		.enable		= exynos5_clksrc_mask_top_ctrl,
		.ctrlbit	= (1 << 0),
	},
	.sources = &exynos5_clkset_vpllsrc,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP2, .shift = 0, .size = 1 },
};

static struct clk *exynos5_clkset_sclk_vpll_list[] = {
	[0] = &exynos5_clk_vpllsrc.clk,
	[1] = &clk_fout_vpll,
};

static struct clksrc_sources exynos5_clkset_sclk_vpll = {
	.sources	= exynos5_clkset_sclk_vpll_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clkset_sclk_vpll_list),
};

static struct clksrc_clk exynos5_clk_sclk_vpll = {
	.clk	= {
		.name		= "sclk_vpll",
	},
	.sources = &exynos5_clkset_sclk_vpll,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP2, .shift = 16, .size = 1 },
};

static struct clksrc_clk exynos5_clk_sclk_pixel = {
	.clk	= {
		.name		= "sclk_pixel",
		.parent		= &exynos5_clk_sclk_vpll.clk,
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_DISP1_0, .shift = 28, .size = 4 },
};

static struct clk *exynos5_clkset_sclk_hdmi_list[] = {
	[0] = &exynos5_clk_sclk_pixel.clk,
	[1] = &exynos5_clk_sclk_hdmiphy,
};

static struct clksrc_sources exynos5_clkset_sclk_hdmi = {
	.sources	= exynos5_clkset_sclk_hdmi_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clkset_sclk_hdmi_list),
};

static struct clksrc_clk exynos5_clk_sclk_hdmi = {
	.clk	= {
		.name		= "sclk_hdmi",
		.enable		= exynos5_clksrc_mask_disp1_0_ctrl,
		.ctrlbit	= (1 << 20),
	},
	.sources = &exynos5_clkset_sclk_hdmi,
	.reg_src = { .reg = EXYNOS5_CLKSRC_DISP1_0, .shift = 20, .size = 1 },
};

static struct clksrc_clk *exynos5_sclk_tv[] = {
	&exynos5_clk_sclk_pixel,
	&exynos5_clk_sclk_hdmi,
};

static struct clk *exynos5_clk_src_mpll_user_list[] = {
	[0] = &clk_fin_mpll,
	[1] = &exynos5_clk_mout_mpll.clk,
};

static struct clksrc_sources exynos5_clk_src_mpll_user = {
	.sources	= exynos5_clk_src_mpll_user_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clk_src_mpll_user_list),
};

static struct clksrc_clk exynos5_clk_mout_mpll_user = {
	.clk	= {
		.name		= "mout_mpll_user",
	},
	.sources = &exynos5_clk_src_mpll_user,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP2, .shift = 20, .size = 1 },
};

static struct clk *exynos5_clkset_mout_cpu_list[] = {
	[0] = &exynos5_clk_mout_apll.clk,
	[1] = &exynos5_clk_mout_mpll.clk,
};

static struct clksrc_sources exynos5_clkset_mout_cpu = {
	.sources	= exynos5_clkset_mout_cpu_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clkset_mout_cpu_list),
};

static struct clksrc_clk exynos5_clk_mout_cpu = {
	.clk	= {
		.name		= "mout_cpu",
	},
	.sources = &exynos5_clkset_mout_cpu,
	.reg_src = { .reg = EXYNOS5_CLKSRC_CPU, .shift = 16, .size = 1 },
};

static struct clksrc_clk exynos5_clk_dout_armclk = {
	.clk	= {
		.name		= "dout_armclk",
		.parent		= &exynos5_clk_mout_cpu.clk,
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_CPU0, .shift = 0, .size = 3 },
};

static struct clksrc_clk exynos5_clk_dout_arm2clk = {
	.clk	= {
		.name		= "dout_arm2clk",
		.parent		= &exynos5_clk_dout_armclk.clk,
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_CPU0, .shift = 28, .size = 3 },
};

static struct clk exynos5_clk_armclk = {
	.name		= "armclk",
	.parent		= &exynos5_clk_dout_arm2clk.clk,
};

/* Core list of CMU_CDREX side */

static struct clk *exynos5_clkset_cdrex_list[] = {
	[0] = &exynos5_clk_mout_mpll.clk,
	[1] = &exynos5_clk_mout_bpll.clk,
};

static struct clksrc_sources exynos5_clkset_cdrex = {
	.sources	= exynos5_clkset_cdrex_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clkset_cdrex_list),
};

static struct clksrc_clk exynos5_clk_cdrex = {
	.clk	= {
		.name		= "clk_cdrex",
	},
	.sources = &exynos5_clkset_cdrex,
	.reg_src = { .reg = EXYNOS5_CLKSRC_CDREX, .shift = 4, .size = 1 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_CDREX, .shift = 16, .size = 3 },
};

static struct clksrc_clk exynos5_clk_aclk_acp = {
	.clk	= {
		.name		= "aclk_acp",
		.parent		= &exynos5_clk_mout_mpll.clk,
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_ACP, .shift = 0, .size = 3 },
};

static struct clksrc_clk exynos5_clk_pclk_acp = {
	.clk	= {
		.name		= "pclk_acp",
		.parent		= &exynos5_clk_aclk_acp.clk,
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_ACP, .shift = 4, .size = 3 },
};

/* Core list of CMU_TOP side */

struct clk *exynos5_clkset_aclk_top_list[] = {
	[0] = &exynos5_clk_mout_mpll_user.clk,
	[1] = &exynos5_clk_mout_bpll_user.clk,
};

struct clksrc_sources exynos5_clkset_aclk = {
	.sources	= exynos5_clkset_aclk_top_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clkset_aclk_top_list),
};

static struct clksrc_clk exynos5_clk_mout_gpll = {
	.clk	= {
		.name		= "mout_gpll",
	},
	.sources = &clk_src_gpll,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP2, .shift = 28, .size = 1 },
};

/* For ACLK_400_G3D_MID */
static struct clksrc_clk exynos5_clk_aclk_400_g3d_mid = {
	.clk	= {
		.name		= "aclk_400_g3d_mid",
	},
	.sources = &exynos5_clkset_aclk,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP0, .shift = 20, .size = 1 },
};

/* For ACLK_400_G3D */
struct clk *exynos5_clkset_aclk_g3d_list[] = {
	[0] = &exynos5_clk_aclk_400_g3d_mid.clk,
	[1] = &exynos5_clk_mout_gpll.clk,
};

struct clksrc_sources exynos5_clkset_aclk_g3d = {
	.sources	= exynos5_clkset_aclk_g3d_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clkset_aclk_g3d_list),
};

static struct clksrc_clk exynos5_clk_aclk_400_g3d = {
	.clk	= {
		.name		= "aclk_400_g3d",
	},
	.sources = &exynos5_clkset_aclk_g3d,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP1, .shift = 28, .size = 1 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_TOP0, .shift = 24, .size = 3 },
};

struct clk *exynos5_clkset_aclk_333_166_list[] = {
	[0] = &exynos5_clk_mout_cpll.clk,
	[1] = &exynos5_clk_mout_mpll_user.clk,
};

struct clksrc_sources exynos5_clkset_aclk_333_166 = {
	.sources	= exynos5_clkset_aclk_333_166_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clkset_aclk_333_166_list),
};

static struct clksrc_clk exynos5_clk_aclk_333 = {
	.clk	= {
		.name		= "aclk_333",
	},
	.sources = &exynos5_clkset_aclk_333_166,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP0, .shift = 16, .size = 1 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_TOP0, .shift = 20, .size = 3 },
};

static struct clksrc_clk exynos5_clk_aclk_166 = {
	.clk	= {
		.name		= "aclk_166",
	},
	.sources = &exynos5_clkset_aclk_333_166,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP0, .shift = 8, .size = 1 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_TOP0, .shift = 8, .size = 3 },
};

static struct clksrc_clk exynos5_clk_aclk_266 = {
	.clk	= {
		.name		= "aclk_266",
		.parent		= &exynos5_clk_mout_mpll_user.clk,
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_TOP0, .shift = 16, .size = 3 },
};

static struct clksrc_clk exynos5_clk_aclk_200 = {
	.clk	= {
		.name		= "aclk_200",
	},
	.sources = &exynos5_clkset_aclk,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP0, .shift = 12, .size = 1 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_TOP0, .shift = 12, .size = 3 },
};

static struct clksrc_clk exynos5_clk_aclk_66_pre = {
	.clk	= {
		.name		= "aclk_66_pre",
		.parent		= &exynos5_clk_mout_mpll_user.clk,
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_TOP1, .shift = 24, .size = 3 },
};

static struct clksrc_clk exynos5_clk_aclk_66 = {
	.clk	= {
		.name		= "aclk_66",
		.parent		= &exynos5_clk_aclk_66_pre.clk,
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_TOP0, .shift = 0, .size = 3 },
};

static struct clk exynos5_init_clocks_off[] = {
	{
		.name		= "timers",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 24),
	}, {
		.name		= "rtc",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peris_ctrl,
		.ctrlbit	= (1 << 20),
	}, {
		.name		= "watchdog",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peris_ctrl,
		.ctrlbit	= (1 << 19),
	}, {
		.name		= "biu",
		.devname	= "dw_mmc.0",
		.parent		= &exynos5_clk_aclk_200.clk,
		.enable		= exynos5_clk_ip_fsys_ctrl,
		.ctrlbit	= (1 << 12),
	}, {
		.name		= "biu",
		.devname	= "dw_mmc.1",
		.parent		= &exynos5_clk_aclk_200.clk,
		.enable		= exynos5_clk_ip_fsys_ctrl,
		.ctrlbit	= (1 << 13),
	}, {
		.name		= "biu",
		.devname	= "dw_mmc.2",
		.parent		= &exynos5_clk_aclk_200.clk,
		.enable		= exynos5_clk_ip_fsys_ctrl,
		.ctrlbit	= (1 << 14),
	}, {
		.name		= "biu",
		.devname	= "dw_mmc.3",
		.parent		= &exynos5_clk_aclk_200.clk,
		.enable		= exynos5_clk_ip_fsys_ctrl,
		.ctrlbit	= (1 << 15),
	}, {
		.name		= "sata",
		.devname	= "ahci",
		.enable		= exynos5_clk_ip_fsys_ctrl,
		.ctrlbit	= (1 << 6),
	}, {
		.name		= "sata_phy",
		.enable		= exynos5_clk_ip_fsys_ctrl,
		.ctrlbit	= (1 << 24),
	}, {
		.name		= "sata_phy_i2c",
		.enable		= exynos5_clk_ip_fsys_ctrl,
		.ctrlbit	= (1 << 25),
	}, {
		.name		= "mfc",
		.devname	= "s5p-mfc-v6",
		.enable		= exynos5_clk_ip_mfc_ctrl,
		.ctrlbit	= (1 << 0),
	}, {
		.name		= "hdmi",
		.devname	= "exynos5-hdmi",
		.enable		= exynos5_clk_ip_disp1_ctrl,
		.ctrlbit	= (1 << 6),
	}, {
		.name		= "hdmiphy",
		.devname	= "exynos5-hdmi",
		.enable		= exynos5_clk_hdmiphy_ctrl,
		.ctrlbit	= (1 << 0),
	}, {
		.name		= "mixer",
		.devname	= "s5p-mixer",
		.enable		= exynos5_clk_ip_disp1_ctrl,
		.ctrlbit	= (1 << 5),
	}, {
		.name		= "jpeg",
		.enable		= exynos5_clk_ip_gen_ctrl,
		.ctrlbit	= (1 << 2),
	}, {
		.name		= "dsim0",
		.enable		= exynos5_clk_ip_disp1_ctrl,
		.ctrlbit	= (1 << 3),
	}, {
		.name           = "fimd",
		.devname        = "exynos5-fb",
		.enable         = exynos5_clk_ip_disp1_ctrl,
		.ctrlbit        = (1 << 0),
	}, {
		.name		= "iis",
		.devname	= "samsung-i2s.1",
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 20),
	}, {
		.name		= "iis",
		.devname	= "samsung-i2s.2",
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 21),
	}, {
		.name		= "pcm",
		.devname	= "samsung-pcm.1",
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 22),
	}, {
		.name		= "pcm",
		.devname	= "samsung-pcm.2",
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 23),
	}, {
		.name		= "spdif",
		.devname	= "samsung-spdif",
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 26),
	}, {
		.name		= "ac97",
		.devname	= "samsung-ac97",
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 27),
	}, {
		.name		= "usbhost",
		.enable		= exynos5_clk_ip_fsys_ctrl ,
		.ctrlbit	= (1 << 18),
	}, {
		.name		= "usbdrd30",
		.parent		= &exynos5_clk_aclk_200.clk,
		.enable		= exynos5_clk_ip_fsys_ctrl,
		.ctrlbit	= (1 << 19),
	}, {
		.name		= "usbotg",
		.enable		= exynos5_clk_ip_fsys_ctrl,
		.ctrlbit	= (1 << 7),
	}, {
		.name		= "gps",
		.enable		= exynos5_clk_ip_gps_ctrl,
		.ctrlbit	= ((1 << 3) | (1 << 2) | (1 << 0)),
	}, {
		.name		= "nfcon",
		.enable		= exynos5_clk_ip_fsys_ctrl,
		.ctrlbit	= (1 << 22),
	}, {
		.name		= "iop",
		.enable		= exynos5_clk_ip_fsys_ctrl,
		.ctrlbit	= ((1 << 30) | (1 << 26) | (1 << 23)),
	}, {
		.name		= "core_iop",
		.enable		= exynos5_clk_ip_core_ctrl,
		.ctrlbit	= ((1 << 21) | (1 << 3)),
	}, {
		.name		= "mcu_iop",
		.enable		= exynos5_clk_ip_fsys_ctrl,
		.ctrlbit	= (1 << 0),
	}, {
		.name		= "i2c",
		.devname	= "s3c2440-i2c.0",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 6),
	}, {
		.name		= "i2c",
		.devname	= "s3c2440-i2c.1",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 7),
	}, {
		.name		= "i2c",
		.devname	= "s3c2440-i2c.2",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 8),
	}, {
		.name		= "i2c",
		.devname	= "s3c2440-i2c.3",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 9),
	}, {
		.name		= "i2c",
		.devname	= "s3c2440-i2c.4",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 10),
	}, {
		.name		= "i2c",
		.devname	= "s3c2440-i2c.5",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 11),
	}, {
		.name		= "i2c",
		.devname	= "s3c2440-i2c.6",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 12),
	}, {
		.name		= "i2c",
		.devname	= "s3c2440-i2c.7",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 13),
	}, {
		.name		= "i2c",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 14),
	}, {
		.name           = "gscl",
		.devname        = "exynos-gsc.0",
		.enable         = exynos5_clk_ip_gscl_ctrl,
		.ctrlbit        = (1 << 0),
		.parent		= &exynos5_clk_aclk_266.clk,
	}, {
		.name           = "gscl",
		.devname        = "exynos-gsc.1",
		.enable         = exynos5_clk_ip_gscl_ctrl,
		.ctrlbit        = (1 << 1),
		.parent		= &exynos5_clk_aclk_266.clk,
	}, {
		.name           = "gscl",
		.devname        = "exynos-gsc.2",
		.enable         = exynos5_clk_ip_gscl_ctrl,
		.ctrlbit        = (1 << 2),
		.parent		= &exynos5_clk_aclk_266.clk,
	}, {
		.name           = "gscl",
		.devname        = "exynos-gsc.3",
		.enable         = exynos5_clk_ip_gscl_ctrl,
		.ctrlbit        = (1 << 3),
		.parent		= &exynos5_clk_aclk_266.clk,
	}, {
		.name		= "fimg2d",
		.enable		= exynos5_clk_ip_acp_ctrl,
		.ctrlbit	= (1 << 3),
	}, {
		.name		= "dp",
		.devname	= "s5p-dp",
		.enable		= exynos5_clk_ip_disp1_ctrl,
		.ctrlbit	= (1 << 4),
	}, {
		.name		= SYSMMU_CLOCK_NAME,
		.devname	= SYSMMU_CLOCK_DEVNAME(mfc_l, 3),
		.enable		= &exynos5_clk_ip_mfc_ctrl,
		/* There is change in the MFC_L & MFC_R in v6.5 */
		.ctrlbit	= (1 << 2),
		.parent         = &exynos5_init_clocks_off[10],
	}, {
		.name		= SYSMMU_CLOCK_NAME,
		.devname	= SYSMMU_CLOCK_DEVNAME(mfc_r, 4),
		.enable		= &exynos5_clk_ip_mfc_ctrl,
		.ctrlbit	= (1 << 1),
		.parent         = &exynos5_init_clocks_off[10],
	}, {
		.name		= SYSMMU_CLOCK_NAME,
		.devname	= SYSMMU_CLOCK_DEVNAME(tv, 28),
		.enable		= &exynos5_clk_ip_disp1_ctrl,
		.ctrlbit	= (1 << 9),
		.parent         = &exynos5_init_clocks_off[13],
	}, {
		.name		= SYSMMU_CLOCK_NAME,
		.devname	= SYSMMU_CLOCK_DEVNAME(jpeg, 7),
		.enable		= &exynos5_clk_ip_gen_ctrl,
		.ctrlbit	= (1 << 7),
	}, {
		.name		= SYSMMU_CLOCK_NAME,
		.devname	= SYSMMU_CLOCK_DEVNAME(rot, 5),
		.enable		= &exynos5_clk_ip_gen_ctrl,
		.ctrlbit	= (1 << 6)
	}, {
		.name		= SYSMMU_CLOCK_NAME,
		.devname	= SYSMMU_CLOCK_DEVNAME(gsc0, 23),
		.enable		= &exynos5_clk_ip_gscl_ctrl,
		.ctrlbit	= (1 << 7),
		.parent		= &exynos5_init_clocks_off[40],
	}, {
		.name		= SYSMMU_CLOCK_NAME,
		.devname	= SYSMMU_CLOCK_DEVNAME(gsc1, 24),
		.enable		= &exynos5_clk_ip_gscl_ctrl,
		.ctrlbit	= (1 << 8),
		.parent		= &exynos5_init_clocks_off[41],
	}, {
		.name		= SYSMMU_CLOCK_NAME,
		.devname	= SYSMMU_CLOCK_DEVNAME(gsc2, 25),
		.enable		= &exynos5_clk_ip_gscl_ctrl,
		.ctrlbit	= (1 << 9),
		.parent		= &exynos5_init_clocks_off[42],
	}, {
		.name		= SYSMMU_CLOCK_NAME,
		.devname	= SYSMMU_CLOCK_DEVNAME(gsc3, 26),
		.enable		= &exynos5_clk_ip_gscl_ctrl,
		.ctrlbit	= (1 << 10),
		.parent		= &exynos5_init_clocks_off[43],
	}, {
		.name		= SYSMMU_CLOCK_NAME,
		.devname	= SYSMMU_CLOCK_DEVNAME(isp, 11),
		.enable		= &exynos5_clk_ip_isp0_ctrl,
		.ctrlbit	= (0x3F << 8),
	}, {
		.name		= SYSMMU_CLOCK_NAME2,
		.devname	= SYSMMU_CLOCK_DEVNAME(isp, 11),
		.enable		= &exynos5_clk_ip_isp1_ctrl,
		.ctrlbit	= (0xF << 4),
	}, {
		.name		= SYSMMU_CLOCK_NAME,
		.devname	= SYSMMU_CLOCK_DEVNAME(2d, 2),
		.enable		= &exynos5_clk_ip_acp_ctrl,
		.ctrlbit	= (1 << 7),
	}, {
		.name		= "adc",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 15),
	}, {
		.name		= "spi",
		.devname	= "exynos4210-spi.0",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 16),
	}, {
		.name		= "spi",
		.devname	= "exynos4210-spi.1",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 17),
	}, {
		.name		= "spi",
		.devname	= "exynos4210-spi.2",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 18),
	}, {
		.name		= "tmu_apbif",
		.parent		= &exynos5_clk_aclk_66.clk,
		.enable		= exynos5_clk_ip_peris_ctrl,
		.ctrlbit	= (1 << 21),
	}, {
		.name		= "g3d",
		.enable		= exynos5_clk_ip_g3d_ctrl,
		.ctrlbit	= ((1 << 1) | (1 << 0)),
	}, {
		.name		= "chipid_apbif",
		.enable		= exynos5_clk_ip_peris_ctrl,
		.ctrlbit	= (1 << 0),
	}
};

static struct clk exynos5_init_clocks_on[] = {
	{
		.name		= "uart",
		.devname	= "s5pv210-uart.0",
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 0),
	}, {
		.name		= "uart",
		.devname	= "s5pv210-uart.1",
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 1),
	}, {
		.name		= "uart",
		.devname	= "s5pv210-uart.2",
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 2),
	}, {
		.name		= "uart",
		.devname	= "s5pv210-uart.3",
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 3),
	}, {
		.name		= "uart",
		.devname	= "s5pv210-uart.4",
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 4),
	}, {
		.name		= "uart",
		.devname	= "s5pv210-uart.5",
		.enable		= exynos5_clk_ip_peric_ctrl,
		.ctrlbit	= (1 << 5),
	}
};

static struct clk *clkset_sclk_audio0_list[] = {
	[0] = &exynos5_clk_audiocdclk0.clk,
	[1] = &clk_ext_xtal_mux,
	[2] = &exynos5_clk_sclk_hdmi27m,
	[3] = &exynos5_clk_sclk_dptxphy,
	[4] = &exynos5_clk_sclk_usbphy,
	[5] = &exynos5_clk_sclk_hdmiphy,
	[6] = &exynos5_clk_mout_mpll.clk,
	[7] = &exynos5_clk_mout_epll.clk,
	[8] = &exynos5_clk_sclk_vpll.clk,
	[9] = &exynos5_clk_mout_cpll.clk,
};

static struct clksrc_sources exynos5_clkset_sclk_audio0 = {
	.sources	= clkset_sclk_audio0_list,
	.nr_sources	= ARRAY_SIZE(clkset_sclk_audio0_list),
};

static struct clksrc_clk exynos5_clk_sclk_audio0 = {
	.clk	= {
		.name		= "audio-bus",
		.enable		= exynos5_clksrc_mask_maudio_ctrl,
		.ctrlbit	= (1 << 0),
	},
	.sources = &exynos5_clkset_sclk_audio0,
	.reg_src = { .reg = EXYNOS5_CLKSRC_MAUDIO, .shift = 0, .size = 4 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_MAUDIO, .shift = 0, .size = 4 },
};

static struct clk *exynos5_clkset_mout_audss_list[] = {
	&clk_ext_xtal_mux,
	&clk_fout_epll,
};

static struct clksrc_sources clkset_mout_audss = {
	.sources	= exynos5_clkset_mout_audss_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clkset_mout_audss_list),
};

static struct clksrc_clk exynos5_clk_mout_audss = {
	.clk	= {
		.name		= "mout_audss",
	},
	.sources = &clkset_mout_audss,
	.reg_src = { .reg = EXYNOS_CLKSRC_AUDSS, .shift = 0, .size = 1 },
};

static struct clk *exynos5_clkset_sclk_audss_list[] = {
	&exynos5_clk_mout_audss.clk,
	&exynos5_clk_audiocdclk0.clk,
	&exynos5_clk_sclk_audio0.clk,
};

static struct clksrc_sources exynos5_clkset_sclk_audss = {
	.sources	= exynos5_clkset_sclk_audss_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clkset_sclk_audss_list),
};

static struct clksrc_clk exynos5_clk_sclk_audss_i2s = {
	.clk		= {
		.name		= "i2sclk",
		.enable		= exynos5_clk_audss_ctrl,
		.ctrlbit	= EXYNOS_AUDSS_CLKGATE_I2SSPECIAL,
	},
	.sources = &exynos5_clkset_sclk_audss,
	.reg_src = { .reg = EXYNOS_CLKSRC_AUDSS, .shift = 2, .size = 2 },
	.reg_div = { .reg = EXYNOS_CLKDIV_AUDSS, .shift = 8, .size = 4 },
};

static struct clksrc_clk exynos5_clk_dout_audss_srp = {
	.clk	= {
		.name		= "dout_srp",
		.parent		= &exynos5_clk_mout_audss.clk,
	},
	.reg_div = { .reg = EXYNOS_CLKDIV_AUDSS, .shift = 0, .size = 4 },
};

static struct clksrc_clk exynos5_clk_sclk_audss_bus = {
	.clk	= {
		.name		= "busclk",
		.parent		= &exynos5_clk_dout_audss_srp.clk,
		.enable		= exynos5_clk_audss_ctrl,
		.ctrlbit	= EXYNOS_AUDSS_CLKGATE_I2SBUS,
	},
	.reg_div = { .reg = EXYNOS_CLKDIV_AUDSS, .shift = 4, .size = 4 },
};

static struct clk exynos5_clk_pdma0 = {
	.name		= "dma",
	.devname	= "dma-pl330.0",
	.enable		= exynos5_clk_ip_fsys_ctrl,
	.ctrlbit	= (1 << 1),
};

static struct clk exynos5_clk_pdma1 = {
	.name		= "dma",
	.devname	= "dma-pl330.1",
	.enable		= exynos5_clk_ip_fsys_ctrl,
	.ctrlbit	= (1 << 2),
};

static struct clk exynos5_clk_mdma1 = {
	.name		= "dma",
	.devname	= "dma-pl330.2",
	.enable		= exynos5_clk_ip_gen_ctrl,
	.ctrlbit	= (1 << 4),
};

struct clk *exynos5_clkset_group_list[] = {
	[0] = &clk_ext_xtal_mux,
	[1] = NULL,
	[2] = &exynos5_clk_sclk_hdmi24m,
	[3] = &exynos5_clk_sclk_dptxphy,
	[4] = &exynos5_clk_sclk_usbphy,
	[5] = &exynos5_clk_sclk_hdmiphy,
	[6] = &exynos5_clk_mout_mpll_user.clk,
	[7] = &exynos5_clk_mout_epll.clk,
	[8] = &exynos5_clk_sclk_vpll.clk,
	[9] = &exynos5_clk_mout_cpll.clk,
};

struct clksrc_sources exynos5_clkset_group = {
	.sources	= exynos5_clkset_group_list,
	.nr_sources	= ARRAY_SIZE(exynos5_clkset_group_list),
};

struct clk *exynos5_clkset_usbdrd30_list[] = {
	[0] = &exynos5_clk_mout_mpll.clk,
	[1] = &exynos5_clk_mout_cpll.clk,
};

struct clksrc_sources exynos5_clkset_usbdrd30 = {
	.sources        = exynos5_clkset_usbdrd30_list,
	.nr_sources     = ARRAY_SIZE(exynos5_clkset_usbdrd30_list),
};

/* Possible clock sources for aclk_266_gscl_sub Mux */
static struct clk *clk_src_gscl_266_list[] = {
	[0] = &clk_ext_xtal_mux,
	[1] = &exynos5_clk_aclk_266.clk,
};

static struct clksrc_sources clk_src_gscl_266 = {
	.sources	= clk_src_gscl_266_list,
	.nr_sources	= ARRAY_SIZE(clk_src_gscl_266_list),
};

static struct clksrc_clk exynos5_clk_dout_mmc0 = {
	.clk		= {
		.name		= "dout_mmc0",
	},
	.sources = &exynos5_clkset_group,
	.reg_src = { .reg = EXYNOS5_CLKSRC_FSYS, .shift = 0, .size = 4 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_FSYS1, .shift = 0, .size = 4 },
};

static struct clksrc_clk exynos5_clk_dout_mmc1 = {
	.clk		= {
		.name		= "dout_mmc1",
	},
	.sources = &exynos5_clkset_group,
	.reg_src = { .reg = EXYNOS5_CLKSRC_FSYS, .shift = 4, .size = 4 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_FSYS1, .shift = 16, .size = 4 },
};

static struct clksrc_clk exynos5_clk_dout_mmc2 = {
	.clk		= {
		.name		= "dout_mmc2",
	},
	.sources = &exynos5_clkset_group,
	.reg_src = { .reg = EXYNOS5_CLKSRC_FSYS, .shift = 8, .size = 4 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_FSYS2, .shift = 0, .size = 4 },
};

static struct clksrc_clk exynos5_clk_dout_mmc3 = {
	.clk		= {
		.name		= "dout_mmc3",
	},
	.sources = &exynos5_clkset_group,
	.reg_src = { .reg = EXYNOS5_CLKSRC_FSYS, .shift = 12, .size = 4 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_FSYS2, .shift = 16, .size = 4 },
};

static struct clksrc_clk exynos5_clk_dout_mmc4 = {
	.clk		= {
		.name		= "dout_mmc4",
	},
	.sources = &exynos5_clkset_group,
	.reg_src = { .reg = EXYNOS5_CLKSRC_FSYS, .shift = 16, .size = 4 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_FSYS3, .shift = 0, .size = 4 },
};

static struct clksrc_clk exynos5_clk_sclk_uart0 = {
	.clk	= {
		.name		= "uclk1",
		.devname	= "exynos4210-uart.0",
		.enable		= exynos5_clksrc_mask_peric0_ctrl,
		.ctrlbit	= (1 << 0),
	},
	.sources = &exynos5_clkset_group,
	.reg_src = { .reg = EXYNOS5_CLKSRC_PERIC0, .shift = 0, .size = 4 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_PERIC0, .shift = 0, .size = 4 },
};

static struct clksrc_clk exynos5_clk_sclk_uart1 = {
	.clk	= {
		.name		= "uclk1",
		.devname	= "exynos4210-uart.1",
		.enable		= exynos5_clksrc_mask_peric0_ctrl,
		.ctrlbit	= (1 << 4),
	},
	.sources = &exynos5_clkset_group,
	.reg_src = { .reg = EXYNOS5_CLKSRC_PERIC0, .shift = 4, .size = 4 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_PERIC0, .shift = 4, .size = 4 },
};

static struct clksrc_clk exynos5_clk_sclk_uart2 = {
	.clk	= {
		.name		= "uclk1",
		.devname	= "exynos4210-uart.2",
		.enable		= exynos5_clksrc_mask_peric0_ctrl,
		.ctrlbit	= (1 << 8),
	},
	.sources = &exynos5_clkset_group,
	.reg_src = { .reg = EXYNOS5_CLKSRC_PERIC0, .shift = 8, .size = 4 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_PERIC0, .shift = 8, .size = 4 },
};

static struct clksrc_clk exynos5_clk_sclk_uart3 = {
	.clk	= {
		.name		= "uclk1",
		.devname	= "exynos4210-uart.3",
		.enable		= exynos5_clksrc_mask_peric0_ctrl,
		.ctrlbit	= (1 << 12),
	},
	.sources = &exynos5_clkset_group,
	.reg_src = { .reg = EXYNOS5_CLKSRC_PERIC0, .shift = 12, .size = 4 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_PERIC0, .shift = 12, .size = 4 },
};

static struct clksrc_clk exynos5_clk_sclk_mmc0 = {
	.clk	= {
		.name		= "ciu",
		.devname	= "dw_mmc.0",
		.parent		= &exynos5_clk_dout_mmc0.clk,
		.enable		= exynos5_clksrc_mask_fsys_ctrl,
		.ctrlbit	= (1 << 0),
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_FSYS1, .shift = 8, .size = 8 },
};

static struct clksrc_clk exynos5_clk_sclk_mmc1 = {
	.clk	= {
		.name		= "ciu",
		.devname	= "dw_mmc.1",
		.parent		= &exynos5_clk_dout_mmc1.clk,
		.enable		= exynos5_clksrc_mask_fsys_ctrl,
		.ctrlbit	= (1 << 4),
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_FSYS1, .shift = 24, .size = 8 },
};

static struct clksrc_clk exynos5_clk_sclk_mmc2 = {
	.clk	= {
		.name		= "ciu",
		.devname	= "dw_mmc.2",
		.parent		= &exynos5_clk_dout_mmc2.clk,
		.enable		= exynos5_clksrc_mask_fsys_ctrl,
		.ctrlbit	= (1 << 8),
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_FSYS2, .shift = 8, .size = 8 },
};

static struct clksrc_clk exynos5_clk_sclk_mmc3 = {
	.clk	= {
		.name		= "ciu",
		.devname	= "dw_mmc.3",
		.parent		= &exynos5_clk_dout_mmc3.clk,
		.enable		= exynos5_clksrc_mask_fsys_ctrl,
		.ctrlbit	= (1 << 12),
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_FSYS2, .shift = 24, .size = 8 },
};

static struct clksrc_clk exynos5_clk_mdout_spi0 = {
	.clk	= {
		.name		= "sclk_spi_mdout",
		.devname	= "exynos4210-spi.0",
	},
	.sources = &exynos5_clkset_group,
	.reg_src = { .reg = EXYNOS5_CLKSRC_PERIC1, .shift = 16, .size = 4 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_PERIC1, .shift = 0, .size = 4 },

};

static struct clksrc_clk exynos5_clk_sclk_spi0 = {
	.clk	= {
		.name		= "sclk_spi",
		.devname	= "exynos4210-spi.0",
		.parent		= &exynos5_clk_mdout_spi0.clk,
		.enable		= exynos5_clksrc_mask_peric1_ctrl,
		.ctrlbit	= (1 << 16),
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_PERIC1, .shift = 8, .size = 8 },
};

static struct clksrc_clk exynos5_clk_mdout_spi1 = {
	.clk	= {
		.name		= "sclk_spi_mdout",
		.devname	= "exynos4210-spi.1",
	},
	.sources = &exynos5_clkset_group,
	.reg_src = { .reg = EXYNOS5_CLKSRC_PERIC1, .shift = 20, .size = 4 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_PERIC1, .shift = 16, .size = 4 },

};

static struct clksrc_clk exynos5_clk_sclk_spi1 = {
	.clk	= {
		.name		= "sclk_spi",
		.devname	= "exynos4210-spi.1",
		.parent		= &exynos5_clk_mdout_spi1.clk,
		.enable		= exynos5_clksrc_mask_peric1_ctrl,
		.ctrlbit	= (1 << 20),
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_PERIC1, .shift = 24, .size = 8 },
};

static struct clksrc_clk exynos5_clk_mdout_spi2 = {
	.clk	= {
		.name		= "sclk_spi_mdout",
		.devname	= "exynos4210-spi.2",
	},
	.sources = &exynos5_clkset_group,
	.reg_src = { .reg = EXYNOS5_CLKSRC_PERIC1, .shift = 24, .size = 4 },
	.reg_div = { .reg = EXYNOS5_CLKDIV_PERIC2, .shift = 0, .size = 4 },

};

static struct clksrc_clk exynos5_clk_sclk_spi2 = {
	.clk	= {
		.name		= "sclk_spi",
		.devname	= "exynos4210-spi.2",
		.parent		= &exynos5_clk_mdout_spi2.clk,
		.enable		= exynos5_clksrc_mask_peric1_ctrl,
		.ctrlbit	= (1 << 24),
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_PERIC2, .shift = 8, .size = 8 },
};

static struct clksrc_clk exynos5_clksrcs[] = {
	{
		.clk	= {
			.name		= "sclk_fimd",
			.devname	= "exynos5-fb",
			.enable		= exynos5_clksrc_mask_disp1_0_ctrl,
			.ctrlbit	= (1 << 0),
		},
		.sources = &exynos5_clkset_group,
		.reg_src = { .reg = EXYNOS5_CLKSRC_DISP1_0, .shift = 0, .size = 4 },
		.reg_div = { .reg = EXYNOS5_CLKDIV_DISP1_0, .shift = 0, .size = 4 },
	}, {
		.clk	= {
			.name		= "aclk_266_gscl",
		},
		.sources = &clk_src_gscl_266,
		.reg_src = { .reg = EXYNOS5_CLKSRC_TOP3, .shift = 8, .size = 1 },
	}, {
		.clk	= {
			.name		= "sclk_g3d",
			.devname	= "mali-t604.0",
			.enable		= exynos5_clk_block_ctrl,
			.ctrlbit	= (1 << 1),
		},
		.sources = &exynos5_clkset_aclk,
		.reg_src = { .reg = EXYNOS5_CLKSRC_TOP0, .shift = 20, .size = 1 },
		.reg_div = { .reg = EXYNOS5_CLKDIV_TOP0, .shift = 24, .size = 3 },
	}, {
		.clk	= {
			.name		= "sclk_sata",
			.devname	= "ahci",
			.enable		= exynos5_clk_ip_fsys_ctrl,
			.ctrlbit	= (1 << 24),
		},
		.sources = &exynos5_clkset_aclk,
		.reg_src = { .reg = EXYNOS5_CLKSRC_FSYS, .shift = 24, .size = 1 },
		.reg_div = { .reg = EXYNOS5_CLKDIV_FSYS0, .shift = 20, .size = 4 },
	}, {
		.clk	= {
			.name		= "sclk_gscl_wrap",
			.devname	= "s5p-mipi-csis.0",
			.enable		= exynos5_clksrc_mask_gscl_ctrl,
			.ctrlbit	= (1 << 24),
		},
		.sources = &exynos5_clkset_group,
		.reg_src = { .reg = EXYNOS5_CLKSRC_GSCL, .shift = 24, .size = 4 },
		.reg_div = { .reg = EXYNOS5_CLKDIV_GSCL, .shift = 24, .size = 4 },
	}, {
		.clk	= {
			.name		= "sclk_gscl_wrap",
			.devname	= "s5p-mipi-csis.1",
			.enable		= exynos5_clksrc_mask_gscl_ctrl,
			.ctrlbit	= (1 << 28),
		},
		.sources = &exynos5_clkset_group,
		.reg_src = { .reg = EXYNOS5_CLKSRC_GSCL, .shift = 28, .size = 4 },
		.reg_div = { .reg = EXYNOS5_CLKDIV_GSCL, .shift = 28, .size = 4 },
	}, {
		.clk	= {
			.name		= "sclk_cam0",
			.enable		= exynos5_clksrc_mask_gscl_ctrl,
			.ctrlbit	= (1 << 16),
		},
		.sources = &exynos5_clkset_group,
		.reg_src = { .reg = EXYNOS5_CLKSRC_GSCL, .shift = 16, .size = 4 },
		.reg_div = { .reg = EXYNOS5_CLKDIV_GSCL, .shift = 16, .size = 4 },
	}, {
		.clk	= {
			.name		= "sclk_cam1",
			.enable		= exynos5_clksrc_mask_gscl_ctrl,
			.ctrlbit	= (1 << 20),
		},
		.sources = &exynos5_clkset_group,
		.reg_src = { .reg = EXYNOS5_CLKSRC_GSCL, .shift = 20, .size = 4 },
		.reg_div = { .reg = EXYNOS5_CLKDIV_GSCL, .shift = 20, .size = 4 },
	}, {
		.clk	= {
			.name		= "sclk_jpeg",
			.parent		= &exynos5_clk_mout_cpll.clk,
		},
		.reg_div = { .reg = EXYNOS5_CLKDIV_GEN, .shift = 4, .size = 3 },
	}, {
		.clk    = {
			.name           = "sclk_usbdrd30",
			.enable         = exynos5_clksrc_mask_fsys_ctrl,
			.ctrlbit        = (1 << 28),
		},
		.sources = &exynos5_clkset_usbdrd30,
		.reg_src = { .reg = EXYNOS5_CLKSRC_FSYS, .shift = 28, .size = 1 },
		.reg_div = { .reg = EXYNOS5_CLKDIV_FSYS0, .shift = 24, .size = 4 },
	},
};

/* For ACLK_300_gscl_mid */
static struct clksrc_clk exynos5_clk_mout_aclk_300_gscl_mid = {
	.clk    = {
		.name   = "mout_aclk_300_gscl_mid",
	},
	.sources = &exynos5_clkset_aclk,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP0, .shift = 24, .size = 1 },
};

/* For ACLK_300_gscl */
struct clk *exynos5_clkset_aclk_300_gscl_list[] = {
	[0] = &exynos5_clk_mout_aclk_300_gscl_mid.clk,
	[1] = &exynos5_clk_sclk_vpll.clk,
};

struct clksrc_sources exynos5_clkset_aclk_300_gscl = {
	.sources        = exynos5_clkset_aclk_300_gscl_list,
	.nr_sources     = ARRAY_SIZE(exynos5_clkset_aclk_300_gscl_list),
};

static struct clksrc_clk exynos5_clk_mout_aclk_300_gscl = {
	.clk    = {
		.name           = "mout_aclk_300_gscl",
	},
	.sources = &exynos5_clkset_aclk_300_gscl,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP0, .shift = 25, .size = 1 },
};

static struct clksrc_clk exynos5_clk_dout_aclk_300_gscl = {
	.clk    = {
		.name           = "dout_aclk_300_gscl",
		.parent         = &exynos5_clk_mout_aclk_300_gscl.clk,
	},
	.reg_div = { .reg = EXYNOS5_CLKDIV_TOP1, .shift = 12, .size = 3 },
};

/* Possible clock sources for aclk_300_gscl_sub Mux */
static struct clk *clk_src_gscl_300_list[] = {
	[0] = &clk_ext_xtal_mux,
	[1] = &exynos5_clk_dout_aclk_300_gscl.clk,
};

static struct clksrc_sources clk_src_gscl_300 = {
	.sources        = clk_src_gscl_300_list,
	.nr_sources     = ARRAY_SIZE(clk_src_gscl_300_list),
};

static struct clksrc_clk exynos5_clk_aclk_300_gscl = {
	.clk    = {
		.name           = "aclk_300_gscl",
	},
	.sources = &clk_src_gscl_300,
	.reg_src = { .reg = EXYNOS5_CLKSRC_TOP3, .shift = 10, .size = 1 },
};

/* Clock initialization code */
static struct clksrc_clk *exynos5_sysclks[] = {
	&exynos5_clk_mout_apll,
	&exynos5_clk_sclk_apll,
	&exynos5_clk_mout_bpll,
	&exynos5_clk_mout_bpll_fout,
	&exynos5_clk_mout_bpll_user,
	&exynos5_clk_mout_cpll,
	&exynos5_clk_mout_epll,
	&exynos5_clk_mout_mpll,
	&exynos5_clk_mout_gpll,
	&exynos5_clk_mout_mpll_fout,
	&exynos5_clk_mout_mpll_user,
	&exynos5_clk_vpllsrc,
	&exynos5_clk_sclk_vpll,
	&exynos5_clk_mout_cpu,
	&exynos5_clk_dout_armclk,
	&exynos5_clk_dout_arm2clk,
	&exynos5_clk_cdrex,
	&exynos5_clk_aclk_400_g3d,
	&exynos5_clk_aclk_400_g3d_mid,
	&exynos5_clk_aclk_333,
	&exynos5_clk_aclk_266,
	&exynos5_clk_aclk_200,
	&exynos5_clk_aclk_166,
	&exynos5_clk_mout_aclk_300_gscl_mid,
	&exynos5_clk_mout_aclk_300_gscl,
	&exynos5_clk_dout_aclk_300_gscl,
	&exynos5_clk_aclk_300_gscl,
	&exynos5_clk_aclk_66_pre,
	&exynos5_clk_aclk_66,
	&exynos5_clk_dout_mmc0,
	&exynos5_clk_dout_mmc1,
	&exynos5_clk_dout_mmc2,
	&exynos5_clk_dout_mmc3,
	&exynos5_clk_dout_mmc4,
	&exynos5_clk_aclk_acp,
	&exynos5_clk_pclk_acp,
	&exynos5_clk_sclk_spi0,
	&exynos5_clk_sclk_spi1,
	&exynos5_clk_sclk_spi2,
	&exynos5_clk_mdout_spi0,
	&exynos5_clk_mdout_spi1,
	&exynos5_clk_mdout_spi2,
	&exynos5_clk_mout_audss,
	&exynos5_clk_sclk_audss_bus,
	&exynos5_clk_sclk_audss_i2s,
	&exynos5_clk_dout_audss_srp,
	&exynos5_clk_sclk_audio0,
};

static struct clk *exynos5_clk_cdev[] = {
	&exynos5_clk_pdma0,
	&exynos5_clk_pdma1,
	&exynos5_clk_mdma1,
};

static struct clksrc_clk *exynos5_clksrc_cdev[] = {
	&exynos5_clk_sclk_uart0,
	&exynos5_clk_sclk_uart1,
	&exynos5_clk_sclk_uart2,
	&exynos5_clk_sclk_uart3,
	&exynos5_clk_sclk_mmc0,
	&exynos5_clk_sclk_mmc1,
	&exynos5_clk_sclk_mmc2,
	&exynos5_clk_sclk_mmc3,
};

static struct clk_lookup exynos5_clk_lookup[] = {
	CLKDEV_INIT("exynos4210-uart.0", "clk_uart_baud0", &exynos5_clk_sclk_uart0.clk),
	CLKDEV_INIT("exynos4210-uart.1", "clk_uart_baud0", &exynos5_clk_sclk_uart1.clk),
	CLKDEV_INIT("exynos4210-uart.2", "clk_uart_baud0", &exynos5_clk_sclk_uart2.clk),
	CLKDEV_INIT("exynos4210-uart.3", "clk_uart_baud0", &exynos5_clk_sclk_uart3.clk),
	CLKDEV_INIT("exynos4-sdhci.0", "mmc_busclk.2", &exynos5_clk_sclk_mmc0.clk),
	CLKDEV_INIT("exynos4-sdhci.1", "mmc_busclk.2", &exynos5_clk_sclk_mmc1.clk),
	CLKDEV_INIT("exynos4-sdhci.2", "mmc_busclk.2", &exynos5_clk_sclk_mmc2.clk),
	CLKDEV_INIT("exynos4-sdhci.3", "mmc_busclk.2", &exynos5_clk_sclk_mmc3.clk),
	CLKDEV_INIT("exynos4210-spi.0", "spi_busclk0", &exynos5_clk_sclk_spi0.clk),
	CLKDEV_INIT("exynos4210-spi.1", "spi_busclk0", &exynos5_clk_sclk_spi1.clk),
	CLKDEV_INIT("exynos4210-spi.2", "spi_busclk0", &exynos5_clk_sclk_spi2.clk),
	CLKDEV_INIT("dma-pl330.0", "apb_pclk", &exynos5_clk_pdma0),
	CLKDEV_INIT("dma-pl330.1", "apb_pclk", &exynos5_clk_pdma1),
	CLKDEV_INIT("dma-pl330.2", "apb_pclk", &exynos5_clk_mdma1),
};

static unsigned long exynos5_epll_get_rate(struct clk *clk)
{
	return clk->rate;
}

static struct clk *exynos5_clks[] __initdata = {
	&exynos5_clk_sclk_hdmi27m,
	&exynos5_clk_sclk_hdmiphy,
	&clk_fout_bpll,
	&clk_fout_bpll_div2,
	&clk_fout_mpll_div2,
	&clk_fout_cpll,
	&clk_fout_gpll,
	&exynos5_clk_armclk,
};

static u32 exynos5_gpll_div[][6] = {
	/*rate, P, M, S, AFC_DNB, AFC*/
	{1400000000, 3, 175, 0, 0, 0}, /* for 466MHz */
	{800000000, 3, 100, 0, 0, 0},  /* for 400MHz, 200MHz */
	{667000000, 7, 389, 1, 0, 0},  /* for 333MHz, 222MHz, 166MHz */
	{600000000, 4, 200, 1, 0, 0},  /* for 300MHz, 200MHz, 150MHz */
	{533000000, 12, 533, 1, 0, 0}, /* for 533MHz, 266MHz, 133MHz */
	{450000000, 12, 450, 1, 0, 0}, /* for 450 Hz */
	{400000000, 3, 100, 1, 0, 0},
	{333000000, 4, 222, 2, 0, 0},
	{200000000, 3, 100, 2, 0, 0},
};

static unsigned long exynos5_gpll_get_rate(struct clk *clk)
{
	return clk->rate;
}


static u32 epll_div[][6] = {
	{ 192000000, 0, 48, 3, 1, 0 },
	{ 180000000, 0, 45, 3, 1, 0 },
	{  73728000, 1, 73, 3, 3, 47710 },
	{  67737600, 1, 90, 4, 3, 20762 },
	{  49152000, 0, 49, 3, 3, 9961 },
	{  45158400, 0, 45, 3, 3, 10381 },
	{ 180633600, 0, 45, 3, 1, 10381 },
	{ 32768000, 0, 131, 3, 5, 4719 },
};

static int exynos5_gpll_set_rate(struct clk *clk, unsigned long rate)
{
	unsigned int gpll_con0;
	unsigned int locktime;
	unsigned int tmp;
	unsigned int i;

	/* Return if no rate changed */
	if (clk->rate == rate)
		return 0;

	gpll_con0 = __raw_readl(EXYNOS5_GPLL_CON0);
	gpll_con0 &= ~(PLL35XX_MDIV_MASK << PLL35XX_MDIV_SHIFT |	\
			 PLL35XX_PDIV_MASK << PLL35XX_PDIV_SHIFT |	\
			 PLL35XX_SDIV_MASK << PLL35XX_SDIV_SHIFT);

	for (i = 0; i < ARRAY_SIZE(exynos5_gpll_div); i++) {
		if (exynos5_gpll_div[i][0] == rate) {
			gpll_con0 |=
				exynos5_gpll_div[i][1] << PLL35XX_PDIV_SHIFT;
			gpll_con0 |=
				exynos5_gpll_div[i][2] << PLL35XX_MDIV_SHIFT;
			gpll_con0 |=
				exynos5_gpll_div[i][3] << PLL35XX_SDIV_SHIFT;
			break;
		}
	}

	if (i == ARRAY_SIZE(exynos5_gpll_div)) {
		printk(KERN_ERR "%s: Invalid GPLL clock frequency\n", __func__);
		return -EINVAL;
	}

	locktime = 270 * exynos5_gpll_div[i][1] + 1;
	__raw_writel(locktime, EXYNOS5_GPLL_LOCK);

	__raw_writel(gpll_con0, EXYNOS5_GPLL_CON0);

	do {
		tmp = __raw_readl(EXYNOS5_GPLL_CON0);
	} while (!(tmp & (0x1 << EXYNOS5_GPLLCON0_LOCKED_SHIFT)));

	clk->rate = rate;

	return 0;
}

static struct clk_ops exynos5_gpll_ops = {
	.get_rate = exynos5_gpll_get_rate,
	.set_rate = exynos5_gpll_set_rate,
};

static int exynos5_epll_set_rate(struct clk *clk, unsigned long rate)
{
	unsigned int epll_con, epll_con_k;
	unsigned int i;
	unsigned int tmp;
	unsigned int epll_rate;
	unsigned int locktime;
	unsigned int lockcnt;

	/* Return if nothing changed */
	if (clk->rate == rate)
		return 0;

	if (clk->parent)
		epll_rate = clk_get_rate(clk->parent);
	else
		epll_rate = clk_ext_xtal_mux.rate;

	if (epll_rate != 24000000) {
		pr_err("Invalid Clock : recommended clock is 24MHz.\n");
		return -EINVAL;
	}

	epll_con = __raw_readl(EXYNOS5_EPLL_CON0);
	epll_con &= ~(0x1 << 27 | \
			PLL46XX_MDIV_MASK << PLL46XX_MDIV_SHIFT |   \
			PLL46XX_PDIV_MASK << PLL46XX_PDIV_SHIFT | \
			PLL46XX_SDIV_MASK << PLL46XX_SDIV_SHIFT);

	for (i = 0; i < ARRAY_SIZE(epll_div); i++) {
		if (epll_div[i][0] == rate) {
			epll_con_k = epll_div[i][5] << 0;
			epll_con |= epll_div[i][1] << 27;
			epll_con |= epll_div[i][2] << PLL46XX_MDIV_SHIFT;
			epll_con |= epll_div[i][3] << PLL46XX_PDIV_SHIFT;
			epll_con |= epll_div[i][4] << PLL46XX_SDIV_SHIFT;
			break;
		}
	}

	if (i == ARRAY_SIZE(epll_div)) {
		printk(KERN_ERR "%s: Invalid Clock EPLL Frequency\n",
				__func__);
		return -EINVAL;
	}

	epll_rate /= 1000000;

	/* 3000 max_cycls : specification data */
	locktime = 3000 / epll_rate * epll_div[i][3];
	lockcnt = locktime * 10000 / (10000 / epll_rate);

	__raw_writel(lockcnt, EXYNOS5_EPLL_LOCK);

	__raw_writel(epll_con, EXYNOS5_EPLL_CON0);
	__raw_writel(epll_con_k, EXYNOS5_EPLL_CON1);

	do {
		tmp = __raw_readl(EXYNOS5_EPLL_CON0);
	} while (!(tmp & 0x1 << EXYNOS5_EPLLCON0_LOCKED_SHIFT));

	clk->rate = rate;

	return 0;
}

static struct clk_ops exynos5_epll_ops = {
	.get_rate = exynos5_epll_get_rate,
	.set_rate = exynos5_epll_set_rate,
};

static int xtal_rate;

static unsigned long exynos5_fout_apll_get_rate(struct clk *clk)
{
	return s5p_get_pll35xx(xtal_rate, __raw_readl(EXYNOS5_APLL_CON0));
}

static struct clk_ops exynos5_fout_apll_ops = {
	.get_rate = exynos5_fout_apll_get_rate,
};

static u32 exynos5_vpll_div[][5] = {
	{70500000, 2, 94, 4, 0 },
};

static unsigned long exynos5_vpll_get_rate(struct clk *clk)
{
	return clk->rate;
}

static int exynos5_vpll_set_rate(struct clk *clk, unsigned long rate)
{
	unsigned int vpll_con0, vpll_con1;
	unsigned int locktime;
	unsigned int tmp;
	unsigned int i;

	/* Return if nothing changed */
	if (clk->rate == rate)
		return 0;

	vpll_con0 = __raw_readl(EXYNOS5_VPLL_CON0);
	vpll_con0 &= ~(PLL36XX_MDIV_MASK << PLL36XX_MDIV_SHIFT |
		       PLL36XX_PDIV_MASK << PLL36XX_PDIV_SHIFT |
		       PLL36XX_SDIV_MASK << PLL36XX_SDIV_SHIFT);

	vpll_con1 = __raw_readl(EXYNOS5_VPLL_CON1);
	vpll_con1 &= ~(0xffff << 0);

	for (i = 0; i < ARRAY_SIZE(exynos5_vpll_div); i++) {
		if (exynos5_vpll_div[i][0] == rate) {
			vpll_con0 |= exynos5_vpll_div[i][1] <<
							PLL36XX_PDIV_SHIFT;
			vpll_con0 |= exynos5_vpll_div[i][2] <<
							PLL36XX_MDIV_SHIFT;
			vpll_con0 |= exynos5_vpll_div[i][3] <<
							PLL36XX_SDIV_SHIFT;
			vpll_con1 |= exynos5_vpll_div[i][4] << 0;
			break;
		}
	}

	if (i == ARRAY_SIZE(exynos5_vpll_div)) {
		printk(KERN_ERR "%s: Invalid Clock VPLL Frequency\n",
				__func__);
		return -EINVAL;
	}

	/* 3000 max_cycls : specification data */
	locktime = 3000 * exynos5_vpll_div[i][1] + 1;

	__raw_writel(locktime, EXYNOS5_VPLL_LOCK);

	__raw_writel(vpll_con0, EXYNOS5_VPLL_CON0);
	__raw_writel(vpll_con1, EXYNOS5_VPLL_CON1);

	do {
		tmp = __raw_readl(EXYNOS5_VPLL_CON0);
	} while (!(tmp & (0x1 << EXYNOS5_VPLLCON0_LOCKED_SHIFT)));

	clk->rate = rate;

	return 0;
}

static struct clk_ops exynos5_vpll_ops = {
	.get_rate = exynos5_vpll_get_rate,
	.set_rate = exynos5_vpll_set_rate,
};


#ifdef CONFIG_PM
static int exynos5_clock_suspend(void)
{
	s3c_pm_do_save(exynos5_clock_save, ARRAY_SIZE(exynos5_clock_save));

	return 0;
}

static void exynos5_clock_resume(void)
{
	s3c_pm_do_restore_core(exynos5_clock_save, ARRAY_SIZE(exynos5_clock_save));
}
#else
#define exynos5_clock_suspend NULL
#define exynos5_clock_resume NULL
#endif

struct syscore_ops exynos5_clock_syscore_ops = {
	.suspend	= exynos5_clock_suspend,
	.resume		= exynos5_clock_resume,
};

void __init_or_cpufreq exynos5_setup_clocks(void)
{
	struct clk *xtal_clk;
	unsigned long apll;
	unsigned long bpll;
	unsigned long cpll;
	unsigned long mpll;
	unsigned long epll;
	unsigned long vpll;
	unsigned long gpll;
	unsigned long vpllsrc;
	unsigned long xtal;
	unsigned long armclk;
	unsigned long mout_cdrex;
	unsigned long aclk_400_g3d;
	unsigned long aclk_333;
	unsigned long aclk_266;
	unsigned long aclk_200;
	unsigned long aclk_166;
	unsigned long aclk_66;
	unsigned int ptr;

	printk(KERN_DEBUG "%s: registering clocks\n", __func__);

	xtal_clk = clk_get(NULL, "xtal");
	BUG_ON(IS_ERR(xtal_clk));

	xtal = clk_get_rate(xtal_clk);

	xtal_rate = xtal;

	clk_put(xtal_clk);

	printk(KERN_DEBUG "%s: xtal is %ld\n", __func__, xtal);

	apll = s5p_get_pll35xx(xtal, __raw_readl(EXYNOS5_APLL_CON0));
	bpll = s5p_get_pll35xx(xtal, __raw_readl(EXYNOS5_BPLL_CON0));
	cpll = s5p_get_pll35xx(xtal, __raw_readl(EXYNOS5_CPLL_CON0));
	mpll = s5p_get_pll35xx(xtal, __raw_readl(EXYNOS5_MPLL_CON0));
	epll = s5p_get_pll36xx(xtal, __raw_readl(EXYNOS5_EPLL_CON0),
			__raw_readl(EXYNOS5_EPLL_CON1));

	vpllsrc = clk_get_rate(&exynos5_clk_vpllsrc.clk);
	vpll = s5p_get_pll36xx(vpllsrc, __raw_readl(EXYNOS5_VPLL_CON0),
			__raw_readl(EXYNOS5_VPLL_CON1));

	clk_fout_apll.ops = &exynos5_fout_apll_ops;
	clk_fout_bpll.rate = bpll;
	clk_fout_bpll_div2.rate = bpll >> 1;
	clk_fout_cpll.rate = cpll;
	clk_fout_mpll.rate = mpll;
	clk_fout_mpll_div2.rate = mpll >> 1;
	clk_fout_epll.rate = epll;
	clk_fout_vpll.rate = vpll;

	gpll = s5p_get_pll35xx(xtal, __raw_readl(EXYNOS5_GPLL_CON0));
	clk_fout_gpll.rate = gpll;

	printk(KERN_INFO "EXYNOS5: PLL settings, A=%ld, B=%ld, C=%ld\n"
			"M=%ld, E=%ld V=%ld G=%ld",
			apll, bpll, cpll, mpll, epll, vpll, gpll);

	armclk = clk_get_rate(&exynos5_clk_armclk);
	mout_cdrex = clk_get_rate(&exynos5_clk_cdrex.clk);

	aclk_400_g3d = clk_get_rate(&exynos5_clk_aclk_400_g3d.clk);
	aclk_333 = clk_get_rate(&exynos5_clk_aclk_333.clk);
	aclk_266 = clk_get_rate(&exynos5_clk_aclk_266.clk);
	aclk_200 = clk_get_rate(&exynos5_clk_aclk_200.clk);
	aclk_166 = clk_get_rate(&exynos5_clk_aclk_166.clk);
	aclk_66 = clk_get_rate(&exynos5_clk_aclk_66.clk);

	printk(KERN_INFO "EXYNOS5: ARMCLK=%ld, CDREX=%ld, ACLK400G3D=%ld\n"
			"ACLK333=%ld, ACLK266=%ld, ACLK200=%ld\n"
			"ACLK166=%ld, ACLK66=%ld\n",
			armclk, mout_cdrex, aclk_400_g3d,
			aclk_333, aclk_266, aclk_200,
			aclk_166, aclk_66);


	clk_fout_epll.ops = &exynos5_epll_ops;
	clk_fout_vpll.ops = &exynos5_vpll_ops;
	clk_fout_gpll.ops = &exynos5_gpll_ops;

	if (clk_set_parent(&exynos5_clk_mout_epll.clk, &clk_fout_epll))
		printk(KERN_ERR "Unable to set parent %s of clock %s.\n",
				clk_fout_epll.name, exynos5_clk_mout_epll.clk.name);

	clk_set_rate(&exynos5_clk_sclk_apll.clk, 100000000);
	clk_set_rate(&exynos5_clk_aclk_266.clk, 300000000);

	clk_set_rate(&exynos5_clk_aclk_acp.clk, 267000000);
	clk_set_rate(&exynos5_clk_pclk_acp.clk, 134000000);

	clk_set_rate(&clk_fout_vpll, 70500000);

	for (ptr = 0; ptr < ARRAY_SIZE(exynos5_clksrcs); ptr++)
		s3c_set_clksrc(&exynos5_clksrcs[ptr], true);
}

void __init exynos5_register_clocks(void)
{
	int ptr;

	s3c24xx_register_clocks(exynos5_clks, ARRAY_SIZE(exynos5_clks));

	for (ptr = 0; ptr < ARRAY_SIZE(exynos5_sysclks); ptr++)
		s3c_register_clksrc(exynos5_sysclks[ptr], 1);

	for (ptr = 0; ptr < ARRAY_SIZE(exynos5_sclk_tv); ptr++)
		s3c_register_clksrc(exynos5_sclk_tv[ptr], 1);

	for (ptr = 0; ptr < ARRAY_SIZE(exynos5_clksrc_cdev); ptr++)
		s3c_register_clksrc(exynos5_clksrc_cdev[ptr], 1);

	s3c_register_clksrc(exynos5_clksrcs, ARRAY_SIZE(exynos5_clksrcs));
	s3c_register_clocks(exynos5_init_clocks_on, ARRAY_SIZE(exynos5_init_clocks_on));

	s3c24xx_register_clocks(exynos5_clk_cdev, ARRAY_SIZE(exynos5_clk_cdev));
	for (ptr = 0; ptr < ARRAY_SIZE(exynos5_clk_cdev); ptr++)
		s3c_disable_clocks(exynos5_clk_cdev[ptr], 1);

	s3c_register_clocks(exynos5_init_clocks_off, ARRAY_SIZE(exynos5_init_clocks_off));
	s3c_disable_clocks(exynos5_init_clocks_off, ARRAY_SIZE(exynos5_init_clocks_off));
	clkdev_add_table(exynos5_clk_lookup, ARRAY_SIZE(exynos5_clk_lookup));

	register_syscore_ops(&exynos5_clock_syscore_ops);
	s3c_pwmclk_init();
}
