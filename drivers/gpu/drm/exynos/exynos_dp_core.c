/*
 * Samsung SoC DP (Display Port) interface driver.
 *
 * Copyright (C) 2012 Samsung Electronics Co., Ltd.
 * Author: Jingoo Han <jg1.han@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "drmP.h"
#include "drm_crtc_helper.h"

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/workqueue.h>

#include <video/exynos_dp.h>
#include "exynos_drm_drv.h"
#include "exynos_drm_display.h"

#ifdef CONFIG_DRM_PTN3460
#include "i2c/ptn3460.h"
#endif

#include <plat/cpu.h>
#include <plat/gpio-cfg.h>

#include "exynos_dp_core.h"

#define PLL_MAX_TRIES 100

static int exynos_dp_init_dp(struct exynos_dp_device *dp)
{
	exynos_dp_reset(dp);

	/* SW defined function Normal operation */
	exynos_dp_enable_sw_function(dp);

	exynos_dp_init_analog_func(dp);

	exynos_dp_init_hpd(dp);
	exynos_dp_init_aux(dp);

	return 0;
}

static int exynos_dp_detect_hpd(struct exynos_dp_device *dp)
{
	int timeout_loop = 0;

	if (gpio_is_valid(dp->hpd_gpio))
		return !gpio_get_value(dp->hpd_gpio);

	while (exynos_dp_get_plug_in_status(dp) != 0) {
		timeout_loop++;
		if (DP_TIMEOUT_LOOP_COUNT < timeout_loop) {
			dev_err(dp->dev, "failed to get hpd plug status\n");
			return -ETIMEDOUT;
		}
		udelay(10);
	}

	return 0;
}

static unsigned char exynos_dp_calc_edid_check_sum(unsigned char *edid_data)
{
	int i;
	unsigned char sum = 0;

	for (i = 0; i < EDID_BLOCK_LENGTH; i++)
		sum = sum + edid_data[i];

	return sum;
}

static int exynos_dp_read_edid(struct exynos_dp_device *dp)
{
	unsigned char edid[EDID_BLOCK_LENGTH * 2];
	unsigned int extend_block = 0;
	unsigned char sum;
	unsigned char test_vector;
	int retval;

	/*
	 * EDID device address is 0x50.
	 * However, if necessary, you must have set upper address
	 * into E-EDID in I2C device, 0x30.
	 */

	/* Read Extension Flag, Number of 128-byte EDID extension blocks */
	retval = exynos_dp_read_byte_from_i2c(dp, I2C_EDID_DEVICE_ADDR,
				EDID_EXTENSION_FLAG,
				&extend_block);
	if (retval)
		return retval;

	if (extend_block > 0) {
		dev_dbg(dp->dev, "EDID data includes a single extension!\n");

		/* Read EDID data */
		retval = exynos_dp_read_bytes_from_i2c(dp, I2C_EDID_DEVICE_ADDR,
						EDID_HEADER_PATTERN,
						EDID_BLOCK_LENGTH,
						&edid[EDID_HEADER_PATTERN]);
		if (retval != 0) {
			dev_err(dp->dev, "EDID Read failed!\n");
			return -EIO;
		}
		sum = exynos_dp_calc_edid_check_sum(edid);
		if (sum != 0) {
			dev_err(dp->dev, "EDID bad checksum!\n");
			return -EIO;
		}

		/* Read additional EDID data */
		retval = exynos_dp_read_bytes_from_i2c(dp,
				I2C_EDID_DEVICE_ADDR,
				EDID_BLOCK_LENGTH,
				EDID_BLOCK_LENGTH,
				&edid[EDID_BLOCK_LENGTH]);
		if (retval != 0) {
			dev_err(dp->dev, "EDID Read failed!\n");
			return -EIO;
		}
		sum = exynos_dp_calc_edid_check_sum(&edid[EDID_BLOCK_LENGTH]);
		if (sum != 0) {
			dev_err(dp->dev, "EDID bad checksum!\n");
			return -EIO;
		}

		exynos_dp_read_byte_from_dpcd(dp, DPCD_ADDR_TEST_REQUEST,
					&test_vector);
		if (test_vector & DPCD_TEST_EDID_READ) {
			exynos_dp_write_byte_to_dpcd(dp,
				DPCD_ADDR_TEST_EDID_CHECKSUM,
				edid[EDID_BLOCK_LENGTH + EDID_CHECKSUM]);
			exynos_dp_write_byte_to_dpcd(dp,
				DPCD_ADDR_TEST_RESPONSE,
				DPCD_TEST_EDID_CHECKSUM_WRITE);
		}
	} else {
		dev_info(dp->dev, "EDID data does not include any extensions.\n");

		/* Read EDID data */
		retval = exynos_dp_read_bytes_from_i2c(dp,
				I2C_EDID_DEVICE_ADDR,
				EDID_HEADER_PATTERN,
				EDID_BLOCK_LENGTH,
				&edid[EDID_HEADER_PATTERN]);
		if (retval != 0) {
			dev_err(dp->dev, "EDID Read failed!\n");
			return -EIO;
		}
		sum = exynos_dp_calc_edid_check_sum(edid);
		if (sum != 0) {
			dev_err(dp->dev, "EDID bad checksum!\n");
			return -EIO;
		}

		exynos_dp_read_byte_from_dpcd(dp,
			DPCD_ADDR_TEST_REQUEST,
			&test_vector);
		if (test_vector & DPCD_TEST_EDID_READ) {
			exynos_dp_write_byte_to_dpcd(dp,
				DPCD_ADDR_TEST_EDID_CHECKSUM,
				edid[EDID_CHECKSUM]);
			exynos_dp_write_byte_to_dpcd(dp,
				DPCD_ADDR_TEST_RESPONSE,
				DPCD_TEST_EDID_CHECKSUM_WRITE);
		}
	}

	dev_err(dp->dev, "EDID Read success!\n");
	return 0;
}

static int exynos_dp_handle_edid(struct exynos_dp_device *dp)
{
	u8 buf[12];
	int i;
	int ret;

	/* Read DPCD DPCD_ADDR_DPCD_REV~RECEIVE_PORT1_CAP_1 */
	ret = exynos_dp_read_bytes_from_dpcd(dp, DPCD_ADDR_DPCD_REV, 12, buf);
	if (ret)
		return ret;

	/* Read EDID */
	for (i = 0; i < 3; i++) {
		ret = exynos_dp_read_edid(dp);
		if (!ret)
			break;
	}

	return ret;
}

static void exynos_dp_enable_rx_to_enhanced_mode(struct exynos_dp_device *dp,
						bool enable)
{
	u8 data;

	exynos_dp_read_byte_from_dpcd(dp, DPCD_ADDR_LANE_COUNT_SET, &data);

	if (enable)
		exynos_dp_write_byte_to_dpcd(dp, DPCD_ADDR_LANE_COUNT_SET,
			DPCD_ENHANCED_FRAME_EN |
			DPCD_LANE_COUNT_SET(data));
	else
		exynos_dp_write_byte_to_dpcd(dp, DPCD_ADDR_LANE_COUNT_SET,
			DPCD_LANE_COUNT_SET(data));
}

static int exynos_dp_is_enhanced_mode_available(struct exynos_dp_device *dp)
{
	u8 data;
	int retval;

	exynos_dp_read_byte_from_dpcd(dp, DPCD_ADDR_MAX_LANE_COUNT, &data);
	retval = DPCD_ENHANCED_FRAME_CAP(data);

	return retval;
}

static void exynos_dp_set_enhanced_mode(struct exynos_dp_device *dp)
{
	u8 data;

	data = exynos_dp_is_enhanced_mode_available(dp);
	exynos_dp_enable_rx_to_enhanced_mode(dp, data);
	exynos_dp_enable_enhanced_mode(dp, data);
}

static void exynos_dp_training_pattern_dis(struct exynos_dp_device *dp)
{
	exynos_dp_set_training_pattern(dp, DP_NONE);

	exynos_dp_write_byte_to_dpcd(dp,
		DPCD_ADDR_TRAINING_PATTERN_SET,
		DPCD_TRAINING_PATTERN_DISABLED);
}

static void exynos_dp_set_lane_lane_pre_emphasis(struct exynos_dp_device *dp,
					int pre_emphasis, int lane)
{
	switch (lane) {
	case 0:
		exynos_dp_set_lane0_pre_emphasis(dp, pre_emphasis);
		break;
	case 1:
		exynos_dp_set_lane1_pre_emphasis(dp, pre_emphasis);
		break;

	case 2:
		exynos_dp_set_lane2_pre_emphasis(dp, pre_emphasis);
		break;

	case 3:
		exynos_dp_set_lane3_pre_emphasis(dp, pre_emphasis);
		break;
	}
}

static int exynos_dp_link_start(struct exynos_dp_device *dp)
{
	int ret, lane, lane_count, pll_tries;
	u8 buf[4];

	lane_count = dp->link_train.lane_count;

	dp->link_train.lt_state = CLOCK_RECOVERY;
	dp->link_train.eq_loop = 0;

	for (lane = 0; lane < lane_count; lane++)
		dp->link_train.cr_loop[lane] = 0;

	/* Set link rate and count as you want to establish*/
	exynos_dp_set_link_bandwidth(dp, dp->link_train.link_rate);
	exynos_dp_set_lane_count(dp, dp->link_train.lane_count);

	/* Setup RX configuration */
	buf[0] = dp->link_train.link_rate;
	buf[1] = dp->link_train.lane_count;
	ret = exynos_dp_write_bytes_to_dpcd(dp, DPCD_ADDR_LINK_BW_SET, 2, buf);
	if (ret)
		return ret;

	/* Set TX pre-emphasis to minimum */
	for (lane = 0; lane < lane_count; lane++)
		exynos_dp_set_lane_lane_pre_emphasis(dp,
			PRE_EMPHASIS_LEVEL_0, lane);

	/* Wait for PLL lock */
	pll_tries = 0;
	while (exynos_dp_get_pll_lock_status(dp) == PLL_UNLOCKED) {
		if (pll_tries == PLL_MAX_TRIES)
			return -ETIMEDOUT;

		pll_tries++;
		udelay(100);
	}

	/* Set training pattern 1 */
	exynos_dp_set_training_pattern(dp, TRAINING_PTN1);

	/* Set RX training pattern */
	ret = exynos_dp_write_byte_to_dpcd(dp, DPCD_ADDR_TRAINING_PATTERN_SET,
		DPCD_SCRAMBLING_DISABLED | DPCD_TRAINING_PATTERN_1);
	if (ret)
		return ret;

	for (lane = 0; lane < lane_count; lane++)
		buf[lane] = DPCD_PRE_EMPHASIS_PATTERN2_LEVEL0 |
			    DPCD_VOLTAGE_SWING_PATTERN1_LEVEL0;
	ret = exynos_dp_write_bytes_to_dpcd(dp, DPCD_ADDR_TRAINING_LANE0_SET,
		lane_count, buf);
	if (ret)
		return ret;

	return ret;
}

static unsigned char exynos_dp_get_lane_status(u8 link_status[6], int lane)
{
	int shift = (lane & 1) * 4;
	u8 link_value = link_status[lane>>1];

	return (link_value >> shift) & 0xf;
}

static int exynos_dp_clock_recovery_ok(u8 link_status[6], int lane_count)
{
	int lane;
	u8 lane_status;

	for (lane = 0; lane < lane_count; lane++) {
		lane_status = exynos_dp_get_lane_status(link_status, lane);
		if ((lane_status & DPCD_LANE_CR_DONE) == 0)
			return -EINVAL;
	}
	return 0;
}

static int exynos_dp_channel_eq_ok(u8 link_status[6], int lane_count)
{
	int lane;
	u8 lane_align;
	u8 lane_status;

	lane_align = link_status[2];
	if ((lane_align & DPCD_INTERLANE_ALIGN_DONE) == 0)
		return -EINVAL;

	for (lane = 0; lane < lane_count; lane++) {
		lane_status = exynos_dp_get_lane_status(link_status, lane);
		lane_status &= DPCD_CHANNEL_EQ_BITS;
		if (lane_status != DPCD_CHANNEL_EQ_BITS)
			return -EINVAL;
	}
	return 0;
}

static unsigned char exynos_dp_get_adjust_request_voltage(u8 adjust_request[2],
							int lane)
{
	int shift = (lane & 1) * 4;
	u8 link_value = adjust_request[lane>>1];

	return (link_value >> shift) & 0x3;
}

static unsigned char exynos_dp_get_adjust_request_pre_emphasis(
					u8 adjust_request[2],
					int lane)
{
	int shift = (lane & 1) * 4;
	u8 link_value = adjust_request[lane>>1];

	return ((link_value >> shift) & 0xc) >> 2;
}

static void exynos_dp_set_lane_link_training(struct exynos_dp_device *dp,
					u8 training_lane_set, int lane)
{
	switch (lane) {
	case 0:
		exynos_dp_set_lane0_link_training(dp, training_lane_set);
		break;
	case 1:
		exynos_dp_set_lane1_link_training(dp, training_lane_set);
		break;

	case 2:
		exynos_dp_set_lane2_link_training(dp, training_lane_set);
		break;

	case 3:
		exynos_dp_set_lane3_link_training(dp, training_lane_set);
		break;
	}
}

static unsigned int exynos_dp_get_lane_link_training(
				struct exynos_dp_device *dp,
				int lane)
{
	u32 reg = 0;

	switch (lane) {
	case 0:
		reg = exynos_dp_get_lane0_link_training(dp);
		break;
	case 1:
		reg = exynos_dp_get_lane1_link_training(dp);
		break;
	case 2:
		reg = exynos_dp_get_lane2_link_training(dp);
		break;
	case 3:
		reg = exynos_dp_get_lane3_link_training(dp);
		break;
	}

	return reg;
}

static void exynos_dp_reduce_link_rate(struct exynos_dp_device *dp)
{
	if (dp->link_train.link_rate == LINK_RATE_2_70GBPS) {
		/* set to reduced bit rate */
		dp->link_train.link_rate = LINK_RATE_1_62GBPS;
		dev_err(dp->dev, "set to bandwidth %.2x\n",
			dp->link_train.link_rate);
		dp->link_train.lt_state = START;
	} else {
		exynos_dp_training_pattern_dis(dp);
		/* set enhanced mode if available */
		exynos_dp_set_enhanced_mode(dp);
		dp->link_train.lt_state = FAILED;
	}
}

static void exynos_dp_get_adjust_train(struct exynos_dp_device *dp,
				u8 adjust_request[2])
{
	int lane;
	int lane_count;
	u8 voltage_swing;
	u8 pre_emphasis;
	u8 training_lane;

	lane_count = dp->link_train.lane_count;
	for (lane = 0; lane < lane_count; lane++) {
		voltage_swing = exynos_dp_get_adjust_request_voltage(
						adjust_request, lane);
		pre_emphasis = exynos_dp_get_adjust_request_pre_emphasis(
						adjust_request, lane);
		training_lane = DPCD_VOLTAGE_SWING_SET(voltage_swing) |
				DPCD_PRE_EMPHASIS_SET(pre_emphasis);

		if (voltage_swing == VOLTAGE_LEVEL_3 ||
		   pre_emphasis == PRE_EMPHASIS_LEVEL_3) {
			training_lane |= DPCD_MAX_SWING_REACHED;
			training_lane |= DPCD_MAX_PRE_EMPHASIS_REACHED;
		}
		dp->link_train.training_lane[lane] = training_lane;
	}
}

static int exynos_dp_check_max_cr_loop(struct exynos_dp_device *dp,
					u8 voltage_swing)
{
	int lane;
	int lane_count;

	lane_count = dp->link_train.lane_count;
	for (lane = 0; lane < lane_count; lane++) {
		if (voltage_swing == VOLTAGE_LEVEL_3 ||
			dp->link_train.cr_loop[lane] == MAX_CR_LOOP)
			return -EINVAL;
	}
	return 0;
}

static int exynos_dp_process_clock_recovery(struct exynos_dp_device *dp)
{
	int ret, lane, lane_count;
	u8 voltage_swing, pre_emphasis, training_lane, link_status[6];
	u8 *adjust_request;

	udelay(100);

	ret = exynos_dp_read_bytes_from_dpcd(dp, DPCD_ADDR_LANE0_1_STATUS, 6,
			link_status);
	if (ret)
		return ret;

	lane_count = dp->link_train.lane_count;
	if (lane_count == 0)
		return 0;

	adjust_request = link_status + 4;

	if (exynos_dp_clock_recovery_ok(link_status, lane_count) == 0) {
		/* set training pattern 2 for EQ */
		exynos_dp_set_training_pattern(dp, TRAINING_PTN2);

		ret = exynos_dp_write_byte_to_dpcd(dp,
			DPCD_ADDR_TRAINING_PATTERN_SET,
			DPCD_SCRAMBLING_DISABLED | DPCD_TRAINING_PATTERN_2);
		if (ret)
			return ret;

		dp->link_train.lt_state = EQUALIZER_TRAINING;
	} else {
		for (lane = 0; lane < lane_count; lane++) {
			training_lane = exynos_dp_get_lane_link_training(
							dp, lane);
			voltage_swing = exynos_dp_get_adjust_request_voltage(
							adjust_request, lane);
			pre_emphasis = exynos_dp_get_adjust_request_pre_emphasis(
							adjust_request, lane);
			if ((DPCD_VOLTAGE_SWING_GET(training_lane) == voltage_swing) &&
			    (DPCD_PRE_EMPHASIS_GET(training_lane) == pre_emphasis))
				dp->link_train.cr_loop[lane]++;
			dp->link_train.training_lane[lane] = training_lane;
		}

		if (exynos_dp_check_max_cr_loop(dp, voltage_swing) != 0) {
			exynos_dp_reduce_link_rate(dp);
			return ret;
		}
	}

	exynos_dp_get_adjust_train(dp, adjust_request);

	for (lane = 0; lane < lane_count; lane++) {
		exynos_dp_set_lane_link_training(dp,
			dp->link_train.training_lane[lane], lane);
		ret = exynos_dp_write_byte_to_dpcd(dp,
			DPCD_ADDR_TRAINING_LANE0_SET + lane,
			dp->link_train.training_lane[lane]);
		if (ret)
			return ret;
	}

	return ret;
}

static int exynos_dp_process_equalizer_training(struct exynos_dp_device *dp)
{
	int ret, lane, lane_count;
	u8 link_status[6];
	u32 reg;
	u8 *adjust_request;

	udelay(400);

	ret = exynos_dp_read_bytes_from_dpcd(dp, DPCD_ADDR_LANE0_1_STATUS,
				6, link_status);
	if (ret)
		return ret;

	adjust_request = link_status + 4;
	lane_count = dp->link_train.lane_count;

	if (exynos_dp_clock_recovery_ok(link_status, lane_count)) {
		exynos_dp_reduce_link_rate(dp);
		return ret;
	}
	if (exynos_dp_channel_eq_ok(link_status, lane_count) == 0) {
		/* traing pattern Set to Normal */
		exynos_dp_training_pattern_dis(dp);

		dev_info(dp->dev, "Link Training success!\n");

		exynos_dp_get_link_bandwidth(dp, &reg);
		dp->link_train.link_rate = reg;
		dev_dbg(dp->dev, "final bandwidth = %.2x\n",
			dp->link_train.link_rate);

		exynos_dp_get_lane_count(dp, &reg);
		dp->link_train.lane_count = reg;
		dev_dbg(dp->dev, "final lane count = %.2x\n",
			dp->link_train.lane_count);
		/* set enhanced mode if available */
		exynos_dp_set_enhanced_mode(dp);

		dp->link_train.lt_state = FINISHED;
	} else {
		/* not all locked */
		dp->link_train.eq_loop++;

		if (dp->link_train.eq_loop > MAX_EQ_LOOP) {
			exynos_dp_reduce_link_rate(dp);
		} else {
			exynos_dp_get_adjust_train(dp, adjust_request);

			for (lane = 0; lane < lane_count; lane++) {
				exynos_dp_set_lane_link_training(dp,
					dp->link_train.training_lane[lane],
					lane);
				ret = exynos_dp_write_byte_to_dpcd(dp,
					DPCD_ADDR_TRAINING_LANE0_SET + lane,
					dp->link_train.training_lane[lane]);
				if (ret)
					return ret;
			}
		}
	}

	return ret;
}

static void exynos_dp_get_max_rx_bandwidth(struct exynos_dp_device *dp,
			u8 *bandwidth)
{
	u8 data;

	/*
	 * For DP rev.1.1, Maximum link rate of Main Link lanes
	 * 0x06 = 1.62 Gbps, 0x0a = 2.7 Gbps
	 */
	exynos_dp_read_byte_from_dpcd(dp, DPCD_ADDR_MAX_LINK_RATE, &data);
	*bandwidth = data;
}

static void exynos_dp_get_max_rx_lane_count(struct exynos_dp_device *dp,
			u8 *lane_count)
{
	u8 data;

	/*
	 * For DP rev.1.1, Maximum number of Main Link lanes
	 * 0x01 = 1 lane, 0x02 = 2 lanes, 0x04 = 4 lanes
	 */
	exynos_dp_read_byte_from_dpcd(dp, DPCD_ADDR_MAX_LANE_COUNT, &data);
	*lane_count = DPCD_MAX_LANE_COUNT(data);
}

static void exynos_dp_init_training(struct exynos_dp_device *dp,
			enum link_lane_count_type max_lane,
			enum link_rate_type max_rate)
{
	/*
	 * MACRO_RST must be applied after the PLL_LOCK to avoid
	 * the DP inter pair skew issue for at least 10 us
	 */
	exynos_dp_reset_macro(dp);

	/* Initialize by reading RX's DPCD */
	exynos_dp_get_max_rx_bandwidth(dp, &dp->link_train.link_rate);
	exynos_dp_get_max_rx_lane_count(dp, &dp->link_train.lane_count);

	if ((dp->link_train.link_rate != LINK_RATE_1_62GBPS) &&
	   (dp->link_train.link_rate != LINK_RATE_2_70GBPS)) {
		dev_err(dp->dev, "Rx Max Link Rate is abnormal :%x !\n",
			dp->link_train.link_rate);
		dp->link_train.link_rate = LINK_RATE_1_62GBPS;
	}

	if (dp->link_train.lane_count == 0) {
		dev_err(dp->dev, "Rx Max Lane count is abnormal :%x !\n",
			dp->link_train.lane_count);
		dp->link_train.lane_count = (u8)LANE_COUNT1;
	}

	/* Setup TX lane count & rate */
	if (dp->link_train.lane_count > max_lane)
		dp->link_train.lane_count = max_lane;
	if (dp->link_train.link_rate > max_rate)
		dp->link_train.link_rate = max_rate;

	/* All DP analog module power up */
	exynos_dp_set_analog_power_down(dp, POWER_ALL, 0);
}

static int exynos_dp_sw_link_training(struct exynos_dp_device *dp)
{
	int ret = 0, training_finished = 0;

	/* Turn off unnecessary lanes */
	switch (dp->link_train.lane_count) {
	case LANE_COUNT1:
		exynos_dp_set_analog_power_down(dp, CH1_BLOCK, 1);
	case LANE_COUNT2:
		exynos_dp_set_analog_power_down(dp, CH2_BLOCK, 1);
		exynos_dp_set_analog_power_down(dp, CH3_BLOCK, 1);
		break;
	default:
		break;
	}

	dp->link_train.lt_state = START;

	/* Process here */
	while (!ret && !training_finished) {
		switch (dp->link_train.lt_state) {
		case START:
			ret = exynos_dp_link_start(dp);
			break;
		case CLOCK_RECOVERY:
			ret = exynos_dp_process_clock_recovery(dp);
			break;
		case EQUALIZER_TRAINING:
			ret = exynos_dp_process_equalizer_training(dp);
			break;
		case FINISHED:
			training_finished = 1;
			break;
		case FAILED:
			return -EREMOTEIO;
		}
	}
	if (ret)
		dev_err(dp->dev, "eDP link training failed (%d)\n", ret);

	return ret;
}

static int exynos_dp_set_hw_link_train(struct exynos_dp_device *dp,
				u32 max_lane,
				u32 max_rate)
{
	u32 status;
	int ret, lane;

	exynos_dp_stop_video(dp);

	if (exynos_dp_get_pll_lock_status(dp) == PLL_UNLOCKED) {
		dev_err(dp->dev, "PLL is not locked yet.\n");
		return -EAGAIN;
	}

	exynos_dp_reset_macro(dp);

	/* Set TX pre-emphasis to minimum */
	for (lane = 0; lane < max_lane; lane++)
		exynos_dp_set_lane_lane_pre_emphasis(dp,
				PRE_EMPHASIS_LEVEL_0, lane);

	/* All DP analog module power up */
	exynos_dp_set_analog_power_down(dp, POWER_ALL, 0);

	/* Initialize by reading RX's DPCD */
	exynos_dp_get_max_rx_bandwidth(dp, &dp->link_train.link_rate);
	exynos_dp_get_max_rx_lane_count(dp, &dp->link_train.lane_count);

	if ((dp->link_train.link_rate != LINK_RATE_1_62GBPS) &&
		(dp->link_train.link_rate != LINK_RATE_2_70GBPS)) {
		dev_err(dp->dev, "Rx Max Link Rate is abnormal :%x !\n",
			dp->link_train.link_rate);
		dp->link_train.link_rate = LINK_RATE_1_62GBPS;
	}

	if (dp->link_train.lane_count == 0) {
		dev_err(dp->dev, "Rx Max Lane count is abnormal :%x !\n",
			dp->link_train.lane_count);
		dp->link_train.lane_count = (u8)LANE_COUNT1;
	}

	/* Setup TX lane count & rate */
	if (dp->link_train.lane_count > max_lane)
		dp->link_train.lane_count = max_lane;
	if (dp->link_train.link_rate > max_rate)
		dp->link_train.link_rate = max_rate;

	/* Set link rate and count as you want to establish*/
	exynos_dp_set_lane_count(dp, dp->video_info->lane_count);
	exynos_dp_set_link_bandwidth(dp, dp->video_info->link_rate);

	/* Set sink to D0 (Sink Not Ready) mode. */
	exynos_dp_write_byte_to_dpcd(dp, DPCD_ADDR_SINK_POWER_STATE,
						DPCD_SET_POWER_STATE_D0);

	/* Enable H/W Link Training */
	ret = exynos_dp_enable_hw_link_training(dp);

	if (ret) {
		dev_err(dp->dev, " H/W link training failure: %d\n", ret);
		return ret;
	}

	exynos_dp_get_link_bandwidth(dp, &status);
	dp->link_train.link_rate = status;
	dev_dbg(dp->dev, "final bandwidth = %.2x\n",
				dp->link_train.link_rate);

	exynos_dp_get_lane_count(dp, &status);
	dp->link_train.lane_count = status;
	dev_dbg(dp->dev, "final lane count = %.2x\n",
				dp->link_train.lane_count);

	return 0;
}

static int exynos_dp_set_link_train(struct exynos_dp_device *dp,
				u32 count,
				u32 bwtype)
{
	int i;
	int retval;

	for (i = 0; i < DP_TIMEOUT_LOOP_COUNT; i++) {
		exynos_dp_init_training(dp, count, bwtype);
		retval = exynos_dp_sw_link_training(dp);
		if (retval == 0)
			break;

		udelay(100);
	}

	return retval;
}

static int exynos_dp_config_video(struct exynos_dp_device *dp)
{
	int retval = 0;
	int timeout_loop = 0;

	exynos_dp_config_video_slave_mode(dp);

	exynos_dp_set_video_color_format(dp);

	if (exynos_dp_get_pll_lock_status(dp) == PLL_UNLOCKED) {
		dev_err(dp->dev, "PLL is not locked yet.\n");
		return -EINVAL;
	}

	for (;;) {
		timeout_loop++;
		if (!exynos_dp_is_slave_video_stream_clock_on(dp))
			break;
		if (DP_TIMEOUT_LOOP_COUNT < timeout_loop) {
			dev_err(dp->dev, "Wait for stream clock timed out\n");
			return -ETIMEDOUT;
		}

		usleep_range(1000, 5000);
	}

	/* Set to use the register calculated M/N video */
	exynos_dp_set_video_cr_mn(dp, CALCULATED_M, 0, 0);

	/* For video bist, Video timing must be generated by register */
	exynos_dp_set_video_timing_mode(dp, VIDEO_TIMING_FROM_CAPTURE);

	/* Disable video mute */
	exynos_dp_enable_video_mute(dp, 0);

	/* Configure video slave mode */
	exynos_dp_enable_video_master(dp, 0);

	/* Enable video */
	exynos_dp_start_video(dp);

	timeout_loop = 0;

	for (;;) {
		timeout_loop++;
		if (!exynos_dp_is_video_stream_on(dp))
			break;

		if (DP_TIMEOUT_LOOP_COUNT < timeout_loop) {
			dev_err(dp->dev, "Wait for video stream timed out\n");
			return -ETIMEDOUT;
		}

		usleep_range(1000, 5000);
	}

	if (retval != 0)
		dev_err(dp->dev, "Video stream is not detected!\n");

	return retval;
}

static void exynos_dp_enable_scramble(struct exynos_dp_device *dp, bool enable)
{
	u8 data;

	if (enable) {
		exynos_dp_enable_scrambling(dp);

		exynos_dp_read_byte_from_dpcd(dp,
			DPCD_ADDR_TRAINING_PATTERN_SET,
			&data);
		exynos_dp_write_byte_to_dpcd(dp,
			DPCD_ADDR_TRAINING_PATTERN_SET,
			(u8)(data & ~DPCD_SCRAMBLING_DISABLED));
	} else {
		exynos_dp_disable_scrambling(dp);

		exynos_dp_read_byte_from_dpcd(dp,
			DPCD_ADDR_TRAINING_PATTERN_SET,
			&data);
		exynos_dp_write_byte_to_dpcd(dp,
			DPCD_ADDR_TRAINING_PATTERN_SET,
			(u8)(data | DPCD_SCRAMBLING_DISABLED));
	}
}

static irqreturn_t exynos_dp_irq_handler(int irq, void *arg)
{
	struct exynos_dp_device *dp = arg;
	enum dp_irq_type irq_type;

	irq_type = exynos_dp_get_irq_type(dp);
	switch (irq_type) {
	case DP_IRQ_TYPE_HP_CABLE_IN:
	case DP_IRQ_TYPE_HP_CABLE_OUT:
		dev_dbg(dp->dev, "Received irq - type=%d\n", irq_type);
		schedule_work(&dp->hotplug_work);
		exynos_dp_clear_hotplug_interrupts(dp);
		break;
	case DP_IRQ_TYPE_HP_CHANGE:
		/*
		 * We get these change notifications once in a while, but there
		 * is nothing we can do with them. Just ignore it for now and
		 * only handle cable changes.
		 */
		dev_dbg(dp->dev, "Received irq - hotplug change; ignoring.\n");
		exynos_dp_clear_hotplug_interrupts(dp);
		break;
	default:
		dev_err(dp->dev, "Received irq - unknown type!\n");
		break;
	}
	return IRQ_HANDLED;
}

static void exynos_dp_hotplug(struct work_struct *work)
{
	struct exynos_dp_device *dp;
	int ret;

	dp = container_of(work, struct exynos_dp_device, hotplug_work);

	/* Cable is disconnected, skip dp initialization */
	if (exynos_dp_detect_hpd(dp))
		goto out;

#ifdef CONFIG_DRM_PTN3460
	ret = ptn3460_wait_until_ready(30 * 1000);
	if (ret) {
		DRM_ERROR("PTN3460 is not ready, don't plug\n");
		return;
	}
#endif

	ret = exynos_dp_handle_edid(dp);
	if (ret) {
		dev_err(dp->dev, "unable to handle edid\n");
		return;
	}

	if (dp->training_type == SW_LINK_TRAINING)
		ret = exynos_dp_set_link_train(dp, dp->video_info->lane_count,
			dp->video_info->link_rate);
	else
		ret = exynos_dp_set_hw_link_train(dp,
			dp->video_info->lane_count, dp->video_info->link_rate);
	if (ret) {
		dev_err(dp->dev, "unable to do link train\n");
		return;
	}

	exynos_dp_enable_scramble(dp, 1);
	exynos_dp_enable_rx_to_enhanced_mode(dp, 1);
	exynos_dp_enable_enhanced_mode(dp, 1);

	exynos_dp_set_lane_count(dp, dp->video_info->lane_count);
	exynos_dp_set_link_bandwidth(dp, dp->video_info->link_rate);

	exynos_dp_init_video(dp);
	exynos_dp_config_video(dp);

out:
	drm_helper_hpd_irq_event(dp->drm_dev);
}

static int exynos_dp_power_off(struct exynos_dp_device *dp)
{
	exynos_dp_disable_hpd(dp);

	if (work_pending(&dp->hotplug_work))
		flush_work_sync(&dp->hotplug_work);

	if (dp->phy_ops.phy_exit)
		dp->phy_ops.phy_exit();

	clk_disable(dp->clock);
	return 0;
}

static int exynos_dp_power_on(struct exynos_dp_device *dp)
{
	if (dp->phy_ops.phy_init)
		dp->phy_ops.phy_init();

	clk_enable(dp->clock);

	exynos_dp_init_dp(dp);

	/*
	 * DP controller is reset and needs HPD interrupt to trigger
	 * re-configuration. If we don't have valid IRQ, this is never
	 * going to happen. Let's reconfigure it here in this case.
	 */
	if (dp->irq < 0 && !exynos_dp_detect_hpd(dp))
		schedule_work(&dp->hotplug_work);

	return 0;
}

static int exynos_dp_dpms(void *ctx, int mode)
{
	struct exynos_dp_device *dp = ctx;

	switch (mode) {
	case DRM_MODE_DPMS_ON:
		return exynos_dp_power_on(dp);

	case DRM_MODE_DPMS_STANDBY:
	case DRM_MODE_DPMS_SUSPEND:
	case DRM_MODE_DPMS_OFF:
		return exynos_dp_power_off(dp);

	default:
		DRM_ERROR("Unknown dpms mode %d\n", mode);
	}
	return -EINVAL;
}

static int exynos_dp_check_timing(void *ctx, void *timing)
{
	/*
	 * TODO(seanpaul): The datasheet isn't terribly descriptive about the
	 * limitations we have here. It's not vitally important to implement
	 * this right now, but should be implemented once we use EDID to mode
	 * set.
	 */
	return 0;
}

static bool exynos_dp_is_connected(void *ctx)
{
	struct exynos_dp_device *dp = ctx;

	if (dp->force_connected)
		return true;
	else
		return !exynos_dp_detect_hpd(dp);
}

static int exynos_dp_subdrv_probe(void *ctx, struct drm_device *drm_dev)
{
	struct exynos_dp_device *dp = ctx;

	dp->drm_dev = drm_dev;

	exynos_dp_dpms(dp, DRM_MODE_DPMS_ON);

	return 0;
}

static struct exynos_panel_ops dp_panel_ops = {
	.subdrv_probe = exynos_dp_subdrv_probe,
	.is_connected = exynos_dp_is_connected,
	.check_timing = exynos_dp_check_timing,
	.dpms = exynos_dp_dpms,
};

static int __devinit exynos_dp_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct exynos_dp_device *dp;
	struct exynos_dp_platdata *pdata;

	int ret = 0;
	int irqflags;

	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "no platform data\n");
		return -EINVAL;
	}

	dp = kzalloc(sizeof(struct exynos_dp_device), GFP_KERNEL);
	if (!dp) {
		dev_err(&pdev->dev, "no memory for device data\n");
		return -ENOMEM;
	}

	dp->dev = &pdev->dev;

	dp->clock = clk_get(&pdev->dev, "dp");
	if (IS_ERR(dp->clock)) {
		dev_err(&pdev->dev, "failed to get clock\n");
		ret = PTR_ERR(dp->clock);
		goto err_dp;
	}

	clk_enable(dp->clock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "failed to get registers\n");
		ret = -EINVAL;
		goto err_clock;
	}

	res = request_mem_region(res->start, resource_size(res),
				dev_name(&pdev->dev));
	if (!res) {
		dev_err(&pdev->dev, "failed to request registers region\n");
		ret = -EINVAL;
		goto err_clock;
	}

	dp->res = res;

	dp->reg_base = ioremap(res->start, resource_size(res));
	if (!dp->reg_base) {
		dev_err(&pdev->dev, "failed to ioremap\n");
		ret = -ENOMEM;
		goto err_req_region;
	}

	if (gpio_is_valid(pdata->hpd_gpio)) {
		dp->hpd_gpio = pdata->hpd_gpio;
		ret = gpio_request_one(dp->hpd_gpio, GPIOF_IN, "dp_hpd");
		if (ret)
			goto err_ioremap;
#ifdef CONFIG_S5P_GPIO_INT
		ret = s5p_register_gpio_interrupt(dp->hpd_gpio);
		if (ret < 0) {
			dev_err(&pdev->dev, "cannot register/get GPIO irq\n");
			goto err_gpio;
		}
		s3c_gpio_cfgpin(dp->hpd_gpio, S3C_GPIO_SFN(0xf));
#endif
		dp->irq = gpio_to_irq(dp->hpd_gpio);
		irqflags = IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;
	} else {
		dp->hpd_gpio = -ENODEV;
		dp->irq = platform_get_irq(pdev, 0);
		irqflags = 0;
	}

	dp->training_type = pdata->training_type;
	dp->video_info = pdata->video_info;
	dp->force_connected = pdata->force_connected;
	if (pdata->phy_init) {
		dp->phy_ops.phy_init = pdata->phy_init;
		dp->phy_ops.phy_init();
	}
	if (pdata->phy_exit)
		dp->phy_ops.phy_exit = pdata->phy_exit;

	INIT_WORK(&dp->hotplug_work, exynos_dp_hotplug);

	if (dp->irq >= 0) {
		ret = request_irq(dp->irq, exynos_dp_irq_handler, irqflags,
				"exynos-dp", dp);
		if (ret) {
			dev_err(&pdev->dev, "failed to request irq\n");
			goto err_gpio;
		}
	}

	platform_set_drvdata(pdev, dp);

	exynos_display_attach_panel(EXYNOS_DRM_DISPLAY_TYPE_FIMD, &dp_panel_ops,
			dp);

	return 0;

err_gpio:
	if (gpio_is_valid(dp->hpd_gpio))
		gpio_free(dp->hpd_gpio);
err_ioremap:
	iounmap(dp->reg_base);
err_req_region:
	release_mem_region(res->start, resource_size(res));
err_clock:
	clk_put(dp->clock);
err_dp:
	kfree(dp);

	return ret;
}

static int __devexit exynos_dp_remove(struct platform_device *pdev)
{
	struct exynos_dp_device *dp = platform_get_drvdata(pdev);

	/* power_off will take care of flushing the hotplug_work */
	exynos_dp_dpms(dp, DRM_MODE_DPMS_OFF);

	if (gpio_is_valid(dp->hpd_gpio))
		gpio_free(dp->hpd_gpio);

	free_irq(dp->irq, dp);
	iounmap(dp->reg_base);

	clk_disable(dp->clock);
	clk_put(dp->clock);

	release_mem_region(dp->res->start, resource_size(dp->res));

	kfree(dp);

	return 0;
}

struct platform_driver dp_driver = {
	.probe		= exynos_dp_probe,
	.remove		= __devexit_p(exynos_dp_remove),
	.driver		= {
		.name	= "s5p-dp",
		.owner	= THIS_MODULE,
	},
};
