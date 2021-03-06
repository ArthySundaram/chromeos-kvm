/*
 * exynos_thermal.c - Samsung EXYNOS TMU (Thermal Management Unit)
 *
 *  Copyright (C) 2011 Samsung Electronics
 *  Donggeun Kim <dg77.kim@samsung.com>
 *  Amit Daniel Kachhap <amit.kachhap@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/module.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/workqueue.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/platform_data/exynos_thermal.h>
#include <linux/thermal.h>
#include <linux/cpufreq.h>
#include <linux/cpu_cooling.h>
#include <linux/of.h>

#include <plat/cpu.h>
#include <mach/regs-pmu.h>	/* for EXYNOS5_PS_HOLD_CONTROL */

/*Exynos generic registers*/
#define EXYNOS_TMU_REG_TRIMINFO		0x0
#define EXYNOS_TMU_REG_CONTROL		0x20
#define EXYNOS_TMU_REG_STATUS		0x28
#define EXYNOS_TMU_REG_CURRENT_TEMP	0x40
#define EXYNOS_TMU_REG_INTEN		0x70
#define EXYNOS_TMU_REG_INTSTAT		0x74
#define EXYNOS_TMU_REG_INTCLEAR		0x78

#define EXYNOS_TMU_TRIM_TEMP_MASK	0xff
#define EXYNOS_TMU_GAIN_SHIFT		8
#define EXYNOS_TMU_REF_VOLTAGE_SHIFT	24
#define EXYNOS_TMU_CORE_ON		3
#define EXYNOS_TMU_CORE_OFF		2
#define EXYNOS_TMU_DEF_CODE_TO_TEMP_OFFSET	50

/*Exynos4 specific registers*/
#define EXYNOS4_TMU_REG_THRESHOLD_TEMP	0x44
#define EXYNOS4_TMU_REG_TRIG_LEVEL0	0x50
#define EXYNOS4_TMU_REG_TRIG_LEVEL1	0x54
#define EXYNOS4_TMU_REG_TRIG_LEVEL2	0x58
#define EXYNOS4_TMU_REG_TRIG_LEVEL3	0x5C
#define EXYNOS4_TMU_REG_PAST_TEMP0	0x60
#define EXYNOS4_TMU_REG_PAST_TEMP1	0x64
#define EXYNOS4_TMU_REG_PAST_TEMP2	0x68
#define EXYNOS4_TMU_REG_PAST_TEMP3	0x6C

#define EXYNOS4_TMU_TRIG_LEVEL0_MASK	0x1
#define EXYNOS4_TMU_TRIG_LEVEL1_MASK	0x10
#define EXYNOS4_TMU_TRIG_LEVEL2_MASK	0x100
#define EXYNOS4_TMU_TRIG_LEVEL3_MASK	0x1000
#define EXYNOS4_TMU_INTCLEAR_VAL	0x1111

/*Exynos5 specific registers*/
#define EXYNOS5_TMU_TRIMINFO_CON	0x14
#define EXYNOS5_THD_TEMP_RISE		0x50
#define EXYNOS5_THD_TEMP_FALL		0x54
#define EXYNOS5_EMUL_CON		0x80

#define EXYNOS5_TRIMINFO_RELOAD		0x1
#define EXYNOS5_TMU_CLEAR_RISE_INT	0x111
#define EXYNOS5_TMU_CLEAR_FALL_INT	(0x111 << 16)
#define EXYNOS5_TMU_HWTRIG_ENABLE	(1 << 31)
#define EXYNOS5_MUX_ADDR_VALUE		6
#define EXYNOS5_MUX_ADDR_SHIFT		20
#define EXYNOS5_TMU_TRIP_MODE_SHIFT	13

#define EFUSE_MIN_VALUE 40
#define EFUSE_MAX_VALUE 100

/*In-kernel thermal framework related macros & definations*/
#define SENSOR_NAME_LEN	16
#define MAX_TRIP_COUNT	8
#define MAX_COOLING_DEVICE 4

#define ACTIVE_INTERVAL 500
#define IDLE_INTERVAL 10000
#define MCELSIUS	1000

/* CPU Zone information */
#define PANIC_ZONE      4
#define WARN_ZONE       3
#define MONITOR_ZONE    2
#define SAFE_ZONE       1

#define GET_ZONE(trip) (trip + 2)
#define GET_TRIP(zone) (zone - 2)

#define EXYNOS_ZONE_COUNT	3

struct exynos_tmu_data {
	struct exynos_tmu_platform_data *pdata;
	struct resource *mem;
	void __iomem *base;
	int irq;
	enum soc_type soc;
	struct work_struct irq_work;
	struct mutex lock;
	struct clk *clk;
	u32 thd_temp_rise;	/* exynos5 resume support */
	u8 temp_error1, temp_error2;
};

struct	thermal_trip_point_conf {
	int trip_val[MAX_TRIP_COUNT];
	int trip_count;
};

struct	thermal_cooling_conf {
	struct freq_clip_table freq_data[MAX_TRIP_COUNT];
	int freq_clip_count;
};

struct thermal_sensor_conf {
	char name[SENSOR_NAME_LEN];
	int (*read_temperature)(void *data);
	struct thermal_trip_point_conf trip_data;
	struct thermal_cooling_conf cooling_data;
	void *private_data;
};

struct exynos_thermal_zone {
	enum thermal_device_mode mode;
	struct thermal_zone_device *therm_dev;
	struct thermal_cooling_device *cool_dev[MAX_COOLING_DEVICE];
	unsigned int cool_dev_size;
	struct platform_device *exynos4_dev;
	struct thermal_sensor_conf *sensor_conf;
};

static struct exynos_thermal_zone *th_zone;
static void exynos_unregister_thermal(void);
static int exynos_register_thermal(struct thermal_sensor_conf *sensor_conf);

/* Get mode callback functions for thermal zone */
static int exynos_get_mode(struct thermal_zone_device *thermal,
			enum thermal_device_mode *mode)
{
	if (th_zone)
		*mode = th_zone->mode;
	return 0;
}

/* Set mode callback functions for thermal zone */
static int exynos_set_mode(struct thermal_zone_device *thermal,
			enum thermal_device_mode mode)
{
	if (!th_zone->therm_dev) {
		pr_notice("thermal zone not registered\n");
		return 0;
	}

	mutex_lock(&th_zone->therm_dev->lock);

	if (mode == THERMAL_DEVICE_ENABLED)
		th_zone->therm_dev->polling_delay = IDLE_INTERVAL;
	else
		th_zone->therm_dev->polling_delay = 0;

	mutex_unlock(&th_zone->therm_dev->lock);

	th_zone->mode = mode;
	thermal_zone_device_update(th_zone->therm_dev);
	pr_info("thermal polling set for duration=%d msec\n",
				th_zone->therm_dev->polling_delay);
	return 0;
}

/*
 * This function may be called from interrupt based temperature sensor
 * when threshold is changed.
 */
static void exynos_report_trigger(void)
{
	unsigned int i;
	char data[10];
	char *envp[] = { data, NULL };

	if (!th_zone || !th_zone->therm_dev)
		return;

	thermal_zone_device_update(th_zone->therm_dev);

	mutex_lock(&th_zone->therm_dev->lock);
	/* Find the level for which trip happened */
	for (i = 0; i < th_zone->sensor_conf->trip_data.trip_count; i++) {
		if (th_zone->therm_dev->last_temperature <
			th_zone->sensor_conf->trip_data.trip_val[i] * MCELSIUS)
			break;
	}

	if (th_zone->mode == THERMAL_DEVICE_ENABLED) {
		if (i > 0)
			th_zone->therm_dev->polling_delay = ACTIVE_INTERVAL;
		else
			th_zone->therm_dev->polling_delay = IDLE_INTERVAL;
	}

	snprintf(data, sizeof(data), "%u", i);
	kobject_uevent_env(&th_zone->therm_dev->device.kobj, KOBJ_CHANGE, envp);
	mutex_unlock(&th_zone->therm_dev->lock);
}

/* Get trip type callback functions for thermal zone */
static int exynos_get_trip_type(struct thermal_zone_device *thermal, int trip,
				 enum thermal_trip_type *type)
{
	switch (GET_ZONE(trip)) {
	case MONITOR_ZONE:
	case WARN_ZONE:
		*type = THERMAL_TRIP_ACTIVE;
		break;
	case PANIC_ZONE:
		*type = THERMAL_TRIP_CRITICAL;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/* Get trip temperature callback functions for thermal zone */
static int exynos_get_trip_temp(struct thermal_zone_device *thermal, int trip,
				unsigned long *temp)
{
	if (trip < GET_TRIP(MONITOR_ZONE) || trip > GET_TRIP(PANIC_ZONE))
		return -EINVAL;

	*temp = th_zone->sensor_conf->trip_data.trip_val[trip];
	/* convert the temperature into millicelsius */
	*temp = *temp * MCELSIUS;

	return 0;
}

/* Get critical temperature callback functions for thermal zone */
static int exynos_get_crit_temp(struct thermal_zone_device *thermal,
				unsigned long *temp)
{
	int ret;
	/* Panic zone */
	ret = exynos_get_trip_temp(thermal, GET_TRIP(PANIC_ZONE), temp);
	return ret;
}

/* Bind callback functions for thermal zone */
static int exynos_bind(struct thermal_zone_device *thermal,
			struct thermal_cooling_device *cdev)
{
	int ret = 0, i;

	/* find the cooling device registered*/
	for (i = 0; i < th_zone->cool_dev_size; i++)
		if (cdev == th_zone->cool_dev[i])
			break;

	/*No matching cooling device*/
	if (i == th_zone->cool_dev_size)
		return 0;

	switch (GET_ZONE(i)) {
	case MONITOR_ZONE:
	case WARN_ZONE:
		if (thermal_zone_bind_cooling_device(thermal, i, cdev)) {
			pr_err("error binding cooling dev inst 0\n");
			ret = -EINVAL;
		}
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

/* Unbind callback functions for thermal zone */
static int exynos_unbind(struct thermal_zone_device *thermal,
			struct thermal_cooling_device *cdev)
{
	int ret = 0, i;

	/* find the cooling device registered*/
	for (i = 0; i < th_zone->cool_dev_size; i++)
		if (cdev == th_zone->cool_dev[i])
			break;

	/*No matching cooling device*/
	if (i == th_zone->cool_dev_size)
		return 0;

	switch (GET_ZONE(i)) {
	case MONITOR_ZONE:
	case WARN_ZONE:
		if (thermal_zone_unbind_cooling_device(thermal, i, cdev)) {
			pr_err("error unbinding cooling dev\n");
			ret = -EINVAL;
		}
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

/* Get temperature callback functions for thermal zone */
static int exynos_get_temp(struct thermal_zone_device *thermal,
			unsigned long *temp)
{
	void *data;

	if (!th_zone->sensor_conf) {
		pr_info("Temperature sensor not initialised\n");
		return -EINVAL;
	}
	data = th_zone->sensor_conf->private_data;
	*temp = th_zone->sensor_conf->read_temperature(data);
	/* convert the temperature into millicelsius */
	*temp = *temp * MCELSIUS;
	return 0;
}

/* Operation callback functions for thermal zone */
static struct thermal_zone_device_ops const exynos_dev_ops = {
	.bind = exynos_bind,
	.unbind = exynos_unbind,
	.get_temp = exynos_get_temp,
	.get_mode = exynos_get_mode,
	.set_mode = exynos_set_mode,
	.get_trip_type = exynos_get_trip_type,
	.get_trip_temp = exynos_get_trip_temp,
	.get_crit_temp = exynos_get_crit_temp,
};

/* Register with the in-kernel thermal management */
static int exynos_register_thermal(struct thermal_sensor_conf *sensor_conf)
{
	int ret, count, tab_size;
	struct freq_clip_table *tab_ptr, *clip_data;

	if (!sensor_conf || !sensor_conf->read_temperature) {
		pr_err("Temperature sensor not initialised\n");
		return -EINVAL;
	}

	th_zone = kzalloc(sizeof(struct exynos_thermal_zone), GFP_KERNEL);
	if (!th_zone)
		return -ENOMEM;

	th_zone->sensor_conf = sensor_conf;

	tab_ptr = (struct freq_clip_table *)sensor_conf->cooling_data.freq_data;
	tab_size = sensor_conf->cooling_data.freq_clip_count;

	/* Register the cpufreq cooling device */
	for (count = 0; count < tab_size; count++) {
		clip_data = (struct freq_clip_table *)&(tab_ptr[count]);
		clip_data->mask_val = cpumask_of(0);
		th_zone->cool_dev[count] = cpufreq_cooling_register(
						clip_data, 1);
		if (IS_ERR(th_zone->cool_dev[count])) {
			pr_err("Failed to register cpufreq cooling device\n");
			ret = -EINVAL;
			th_zone->cool_dev_size = count;
			goto err_unregister;
		}
	}
	th_zone->cool_dev_size = count;

	th_zone->therm_dev = thermal_zone_device_register(sensor_conf->name,
			EXYNOS_ZONE_COUNT, NULL, &exynos_dev_ops, 0, 0, 0,
			IDLE_INTERVAL);

	if (IS_ERR(th_zone->therm_dev)) {
		pr_err("Failed to register thermal zone device\n");
		ret = -EINVAL;
		goto err_unregister;
	}
	th_zone->mode = THERMAL_DEVICE_ENABLED;

	pr_info("Exynos: Kernel Thermal management registered\n");
	return 0;

err_unregister:
	exynos_unregister_thermal();
	return ret;
}

/* Un-Register with the in-kernel thermal management */
static void exynos_unregister_thermal(void)
{
	int i;

	for (i = 0; i < th_zone->cool_dev_size; i++) {
		if (th_zone && th_zone->cool_dev[i])
			cpufreq_cooling_unregister(th_zone->cool_dev[i]);
	}

	if (th_zone && th_zone->therm_dev)
		thermal_zone_device_unregister(th_zone->therm_dev);

	kfree(th_zone);

	pr_info("Exynos: Kernel Thermal management unregistered\n");
}

/*
 * TMU treats temperature as a mapped temperature code.
 * The temperature is converted differently depending on the calibration type.
 */
static int temp_to_code(struct exynos_tmu_data *data, u8 temp)
{
	struct exynos_tmu_platform_data *pdata = data->pdata;
	int temp_code;

	if (data->soc == SOC_ARCH_EXYNOS4)
		/* temp should range between 25 and 125 */
		if (temp < 25 || temp > 125) {
			temp_code = -EINVAL;
			goto out;
		}

	switch (pdata->cal_type) {
	case TYPE_TWO_POINT_TRIMMING:
		temp_code = (temp - 25) *
		    (data->temp_error2 - data->temp_error1) /
		    (85 - 25) + data->temp_error1;
		break;
	case TYPE_ONE_POINT_TRIMMING:
		temp_code = temp + data->temp_error1 - 25;
		break;
	default:
		temp_code = temp + EXYNOS_TMU_DEF_CODE_TO_TEMP_OFFSET;
		break;
	}
out:
	return temp_code;
}

/*
 * Calculate a temperature value from a temperature code.
 * The unit of the temperature is degree Celsius.
 */
static int code_to_temp(struct exynos_tmu_data *data, u8 temp_code)
{
	struct exynos_tmu_platform_data *pdata = data->pdata;
	int temp;

	if (data->soc == SOC_ARCH_EXYNOS4)
		/* temp_code should range between 75 and 175 */
		if (temp_code < 75 || temp_code > 175) {
			temp = -ENODATA;
			goto out;
		}

	switch (pdata->cal_type) {
	case TYPE_TWO_POINT_TRIMMING:
		temp = (temp_code - data->temp_error1) * (85 - 25) /
		    (data->temp_error2 - data->temp_error1) + 25;
		break;
	case TYPE_ONE_POINT_TRIMMING:
		temp = temp_code - data->temp_error1 + 25;
		break;
	default:
		temp = temp_code - EXYNOS_TMU_DEF_CODE_TO_TEMP_OFFSET;
		break;
	}
out:
	return temp;
}

static int exynos5_setup_temp_rise(struct platform_device *pdev)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);
	struct exynos_tmu_platform_data *pdata = data->pdata;

	u8 hwtrig_code, hwtrig_temp;
	u32 rising_threshold;
	int threshold_code;

	/* Write temperature code for thresholds */
	threshold_code = temp_to_code(data, pdata->trigger_levels[0]);
	if (threshold_code < 0)
		return threshold_code;

	rising_threshold = threshold_code;

	threshold_code = temp_to_code(data, pdata->trigger_levels[1]);
	if (threshold_code < 0)
		return threshold_code;

	rising_threshold |= threshold_code << 8;

	threshold_code = temp_to_code(data, pdata->trigger_levels[2]);
	if (threshold_code < 0)
		return threshold_code;

	rising_threshold |= (threshold_code << 16);

	/* On "warm" and "cold" reset, hwtrig_enable remains set
	 * but TEMP_RISE gets cleared. :(
	 * Value ranges copied from "62.3 Temperature Code Table"
	 * of "S5PC520 User's Manual" version 0.01 Oct 2011.
	 */
	hwtrig_code = readl(data->base + EXYNOS5_THD_TEMP_RISE) >> 24;
	hwtrig_temp = code_to_temp(data, hwtrig_code);
	if (hwtrig_temp >= 25 && hwtrig_temp <= 125
			&& (hwtrig_code < threshold_code)) {
		/* Boot FW set the HW trig value.  */
		pr_warn("Thermal: HW poweroff threshold (%d) "
			"< Kernel shutdown threshold (%d). "
			"Changing Kernel shutdown to %d C.\n",
			hwtrig_code, threshold_code,
			code_to_temp(data, hwtrig_code - 2));

		/* ckear previous code */
		rising_threshold &= ~(0xff << 16);

		/* don't have to use temp. 1:1 mapping of temp:code */
		rising_threshold |= ((hwtrig_code - 2) << 16) |
			(hwtrig_code << 24);
	} else {
		/* Boot FW did NOT set HW poweroff trigger.
		 * or trigger is set to invalid value.
		 */
		threshold_code = temp_to_code(data, pdata->trigger_levels[3]);

		if (threshold_code < 0)
			return threshold_code;

		rising_threshold |= threshold_code << 24;
	}

	data->thd_temp_rise = rising_threshold;
	return 0;
}


static int exynos_tmu_initialize(struct platform_device *pdev)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);
	struct exynos_tmu_platform_data *pdata = data->pdata;
	unsigned int status, trim_info;
	int ret = 0, threshold_code;

	mutex_lock(&data->lock);
	clk_enable(data->clk);

	status = readb(data->base + EXYNOS_TMU_REG_STATUS);
	if (!status) {
		ret = -EBUSY;
		goto out;
	}

	if (data->soc == SOC_ARCH_EXYNOS5) {
		__raw_writel(EXYNOS5_TRIMINFO_RELOAD,
				data->base + EXYNOS5_TMU_TRIMINFO_CON);
	}
	/* Save trimming info in order to perform calibration */
	trim_info = readl(data->base + EXYNOS_TMU_REG_TRIMINFO);
	data->temp_error1 = trim_info & EXYNOS_TMU_TRIM_TEMP_MASK;
	data->temp_error2 = ((trim_info >> 8) & EXYNOS_TMU_TRIM_TEMP_MASK);

	if ((EFUSE_MIN_VALUE > data->temp_error1) ||
			(data->temp_error1 > EFUSE_MAX_VALUE) ||
			(data->temp_error2 != 0))
		data->temp_error1 = pdata->efuse_value;

	if (data->soc == SOC_ARCH_EXYNOS4) {
		/* Write temperature code for threshold */
		threshold_code = temp_to_code(data, pdata->threshold);
		if (threshold_code < 0) {
			ret = threshold_code;
			goto out;
		}
		writeb(threshold_code,
			data->base + EXYNOS4_TMU_REG_THRESHOLD_TEMP);

		writeb(pdata->trigger_levels[0],
			data->base + EXYNOS4_TMU_REG_TRIG_LEVEL0);
		writeb(pdata->trigger_levels[1],
			data->base + EXYNOS4_TMU_REG_TRIG_LEVEL1);
		writeb(pdata->trigger_levels[2],
			data->base + EXYNOS4_TMU_REG_TRIG_LEVEL2);
		writeb(pdata->trigger_levels[3],
			data->base + EXYNOS4_TMU_REG_TRIG_LEVEL3);

		writel(EXYNOS4_TMU_INTCLEAR_VAL,
			data->base + EXYNOS_TMU_REG_INTCLEAR);
	} else if (data->soc == SOC_ARCH_EXYNOS5) {
		/* thd_temp_rise is nonzero on resume. Don't compute again. */
		if (!data->thd_temp_rise) {
			ret = exynos5_setup_temp_rise(pdev);
			if (ret < 0)
				goto out;
		}

		writel(data->thd_temp_rise, data->base + EXYNOS5_THD_TEMP_RISE);
		writel(0, data->base + EXYNOS5_THD_TEMP_FALL);

		/* clear all interrupt status bits regardless of which ones
		 * we will enable. HW Trigger seems to depend on REISE_LEVEL3
		 * being cleared.
		 */
		writel(~0, data->base + EXYNOS_TMU_REG_INTCLEAR);
	}
out:
	clk_disable(data->clk);
	mutex_unlock(&data->lock);

	return ret;
}

static void exynos_tmu_control(struct platform_device *pdev, bool on)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);
	struct exynos_tmu_platform_data *pdata = data->pdata;
	unsigned int con, interrupt_en;

	mutex_lock(&data->lock);
	clk_enable(data->clk);

	con = pdata->reference_voltage << EXYNOS_TMU_REF_VOLTAGE_SHIFT |
		pdata->gain << EXYNOS_TMU_GAIN_SHIFT;

	if (data->soc == SOC_ARCH_EXYNOS5) {
		con |= pdata->noise_cancel_mode << EXYNOS5_TMU_TRIP_MODE_SHIFT;
		con |= EXYNOS5_MUX_ADDR_VALUE << EXYNOS5_MUX_ADDR_SHIFT;

		/* enable HW Trigger (must set multiple bits to enable) */
		con |= 1 << 12;
	}

	if (on) {
		con |= EXYNOS_TMU_CORE_ON;
		interrupt_en = pdata->trigger_level3_en << 12 |
			pdata->trigger_level2_en << 8 |
			pdata->trigger_level1_en << 4 |
			pdata->trigger_level0_en;
	} else {
		con |= EXYNOS_TMU_CORE_OFF;
		interrupt_en = 0; /* Disable all interrupts */
	}

	writel(interrupt_en, data->base + EXYNOS_TMU_REG_INTEN);
	writel(con, data->base + EXYNOS_TMU_REG_CONTROL);

	if (data->soc == SOC_ARCH_EXYNOS5) {
		/* enable overtemp HW poweroff.
		 * Enable this *after* thresholds have been set.
		 */
		con = readl(EXYNOS5_PS_HOLD_CONTROL);
		con |= EXYNOS5_TMU_HWTRIG_ENABLE;
		writel(con, EXYNOS5_PS_HOLD_CONTROL);
	}

	clk_disable(data->clk);
	mutex_unlock(&data->lock);
}

static int exynos_tmu_read(struct exynos_tmu_data *data)
{
	u8 temp_code;
	int temp;

	mutex_lock(&data->lock);
	clk_enable(data->clk);

	temp_code = readb(data->base + EXYNOS_TMU_REG_CURRENT_TEMP);
	temp = code_to_temp(data, temp_code);

	clk_disable(data->clk);
	mutex_unlock(&data->lock);

	return temp;
}

static void exynos_tmu_work(struct work_struct *work)
{
	struct exynos_tmu_data *data = container_of(work,
			struct exynos_tmu_data, irq_work);

	mutex_lock(&data->lock);
	clk_enable(data->clk);


	if (data->soc == SOC_ARCH_EXYNOS5)
		writel(EXYNOS5_TMU_CLEAR_RISE_INT,
				data->base + EXYNOS_TMU_REG_INTCLEAR);
	else
		writel(EXYNOS4_TMU_INTCLEAR_VAL,
				data->base + EXYNOS_TMU_REG_INTCLEAR);

	clk_disable(data->clk);
	mutex_unlock(&data->lock);
	exynos_report_trigger();
	enable_irq(data->irq);
}

static irqreturn_t exynos_tmu_irq(int irq, void *id)
{
	struct exynos_tmu_data *data = id;

	disable_irq_nosync(irq);
	schedule_work(&data->irq_work);

	return IRQ_HANDLED;
}
static struct thermal_sensor_conf exynos_sensor_conf = {
	.name			= "exynos-therm",
	.read_temperature	= (int (*)(void *))exynos_tmu_read,
};

#if defined(CONFIG_CPU_EXYNOS4210)
static struct exynos_tmu_platform_data const exynos4_default_tmu_data = {
	.threshold = 80,
	.trigger_levels[0] = 5,
	.trigger_levels[1] = 20,
	.trigger_levels[2] = 30,
	.trigger_level0_en = 1,
	.trigger_level1_en = 1,
	.trigger_level2_en = 1,
	.trigger_level3_en = 0,
	.gain = 15,
	.reference_voltage = 7,
	.cal_type = TYPE_ONE_POINT_TRIMMING,
	.freq_tab[0] = {
		.freq_clip_max = 800 * 1000,
		.temp_level = 85,
	},
	.freq_tab[1] = {
		.freq_clip_max = 200 * 1000,
		.temp_level = 100,
	},
	.freq_tab_count = 2,
	.type = SOC_ARCH_EXYNOS4,
};
#define EXYNOS4_TMU_DRV_DATA (&exynos4_default_tmu_data)
#else
#define EXYNOS4_TMU_DRV_DATA (NULL)
#endif

#if defined(CONFIG_SOC_EXYNOS5250)
static struct exynos_tmu_platform_data const exynos5_default_tmu_data = {
	.trigger_levels[0] = 85,	/* slow e.g. 800Mhz */
	.trigger_levels[1] = 103,	/* very slow e.g. 200Mhz */
	.trigger_levels[2] = 108,	/* kernel issues shutdown */
	.trigger_levels[3] = 110,	/* HW poweroff trigger */
	.trigger_level0_en = 1,
	.trigger_level1_en = 1,
	.trigger_level2_en = 1,
	.trigger_level3_en = 0,		/* HW Trig is enabled differently */
	.gain = 8,
	.reference_voltage = 16,
	.noise_cancel_mode = 4,
	.cal_type = TYPE_ONE_POINT_TRIMMING,
	.efuse_value = 55,
	.freq_tab[0] = {

		.freq_clip_max = 800 * 1000,
		.temp_level = 85,
	},
	.freq_tab[1] = {
		.freq_clip_max = 200 * 1000,
		.temp_level = 103,
	},
	.freq_tab_count = 2,
	.type = SOC_ARCH_EXYNOS5,
};
#define EXYNOS5_TMU_DRV_DATA (&exynos5_default_tmu_data)
#else
#define EXYNOS5_TMU_DRV_DATA (NULL)
#endif

#ifdef CONFIG_OF
static const struct of_device_id exynos_tmu_match[] = {
	{
		.compatible = "samsung,exynos4-tmu",
		.data = (void *)EXYNOS4_TMU_DRV_DATA,
	},
	{
		.compatible = "samsung,exynos5-tmu",
		.data = (void *)EXYNOS5_TMU_DRV_DATA,
	},
	{},
};
MODULE_DEVICE_TABLE(of, exynos_tmu_match);
#else
#define  exynos_tmu_match NULL
#endif

static struct platform_device_id exynos_tmu_driver_ids[] = {
	{
		.name		= "exynos4-tmu",
		.driver_data    = (kernel_ulong_t)EXYNOS4_TMU_DRV_DATA,
	},
	{
		.name		= "exynos5-tmu",
		.driver_data    = (kernel_ulong_t)EXYNOS5_TMU_DRV_DATA,
	},
	{ },
};
MODULE_DEVICE_TABLE(platform, exynos4_tmu_driver_ids);

static inline struct  exynos_tmu_platform_data *exynos_get_driver_data(
			struct platform_device *pdev)
{
#ifdef CONFIG_OF
	if (pdev->dev.of_node) {
		const struct of_device_id *match;
		match = of_match_node(exynos_tmu_match, pdev->dev.of_node);
		if (!match)
			return NULL;
		return (struct exynos_tmu_platform_data *) match->data;
	}
#endif
	return (struct exynos_tmu_platform_data *)
			platform_get_device_id(pdev)->driver_data;
}
static int __devinit exynos_tmu_probe(struct platform_device *pdev)
{
	struct exynos_tmu_data *data;
	struct exynos_tmu_platform_data *pdata = pdev->dev.platform_data;
	int ret, i;

	if (!pdata)
		pdata = exynos_get_driver_data(pdev);

	if (!pdata) {
		dev_err(&pdev->dev, "No platform init data supplied.\n");
		return -ENODEV;
	}
	data = kzalloc(sizeof(struct exynos_tmu_data), GFP_KERNEL);
	if (!data) {
		dev_err(&pdev->dev, "Failed to allocate driver structure\n");
		return -ENOMEM;
	}

	data->irq = platform_get_irq(pdev, 0);
	if (data->irq < 0) {
		ret = data->irq;
		dev_err(&pdev->dev, "Failed to get platform irq\n");
		goto err_free;
	}

	INIT_WORK(&data->irq_work, exynos_tmu_work);

	data->mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!data->mem) {
		ret = -ENOENT;
		dev_err(&pdev->dev, "Failed to get platform resource\n");
		goto err_free;
	}

	data->mem = request_mem_region(data->mem->start,
			resource_size(data->mem), pdev->name);
	if (!data->mem) {
		ret = -ENODEV;
		dev_err(&pdev->dev, "Failed to request memory region\n");
		goto err_free;
	}

	data->base = ioremap(data->mem->start, resource_size(data->mem));
	if (!data->base) {
		ret = -ENODEV;
		dev_err(&pdev->dev, "Failed to ioremap memory\n");
		goto err_mem_region;
	}

	ret = request_irq(data->irq, exynos_tmu_irq,
		IRQF_TRIGGER_RISING, "exynos-tmu", data);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request irq: %d\n", data->irq);
		goto err_io_remap;
	}

	data->clk = clk_get(NULL, "tmu_apbif");
	if (IS_ERR(data->clk)) {
		ret = PTR_ERR(data->clk);
		dev_err(&pdev->dev, "Failed to get clock\n");
		goto err_irq;
	}

	if (pdata->type == SOC_ARCH_EXYNOS5 ||
				pdata->type == SOC_ARCH_EXYNOS4)
		data->soc = pdata->type;
	else {
		ret = -EINVAL;
		dev_err(&pdev->dev, "Platform not supported\n");
		goto err_clk;
	}

	data->pdata = pdata;
	platform_set_drvdata(pdev, data);
	mutex_init(&data->lock);

	ret = exynos_tmu_initialize(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to initialize TMU\n");
		goto err_clk;
	}

	exynos_tmu_control(pdev, true);

	/*Register the sensor with thermal management interface*/
	(&exynos_sensor_conf)->private_data = data;
	exynos_sensor_conf.trip_data.trip_count = pdata->trigger_level0_en +
			pdata->trigger_level1_en + pdata->trigger_level2_en +
			pdata->trigger_level3_en;

	for (i = 0; i < exynos_sensor_conf.trip_data.trip_count; i++)
		exynos_sensor_conf.trip_data.trip_val[i] =
			pdata->threshold + pdata->trigger_levels[i];

	exynos_sensor_conf.cooling_data.freq_clip_count =
						pdata->freq_tab_count;
	for (i = 0; i < pdata->freq_tab_count; i++) {
		exynos_sensor_conf.cooling_data.freq_data[i].freq_clip_max =
					pdata->freq_tab[i].freq_clip_max;
		exynos_sensor_conf.cooling_data.freq_data[i].temp_level =
					pdata->freq_tab[i].temp_level;
	}

	ret = exynos_register_thermal(&exynos_sensor_conf);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register thermal interface\n");
		goto err_clk;
	}
	return 0;

err_clk:
	platform_set_drvdata(pdev, NULL);
	clk_put(data->clk);
err_irq:
	free_irq(data->irq, data);
err_io_remap:
	iounmap(data->base);
err_mem_region:
	release_mem_region(data->mem->start, resource_size(data->mem));
err_free:
	kfree(data);

	return ret;
}

static int __devexit exynos_tmu_remove(struct platform_device *pdev)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);

	exynos_tmu_control(pdev, false);

	exynos_unregister_thermal();

	clk_put(data->clk);

	free_irq(data->irq, data);

	iounmap(data->base);
	release_mem_region(data->mem->start, resource_size(data->mem));

	platform_set_drvdata(pdev, NULL);

	kfree(data);

	return 0;
}

#ifdef CONFIG_PM
static int exynos_tmu_suspend(struct platform_device *pdev, pm_message_t state)
{
	exynos_tmu_control(pdev, false);

	return 0;
}

static int exynos_tmu_resume(struct platform_device *pdev)
{
	exynos_tmu_initialize(pdev);
	exynos_tmu_control(pdev, true);

	return 0;
}
#else
#define exynos_tmu_suspend NULL
#define exynos_tmu_resume NULL
#endif

static struct platform_driver exynos_tmu_driver = {
	.driver = {
		.name   = "exynos-tmu",
		.owner  = THIS_MODULE,
		.of_match_table = exynos_tmu_match,
	},
	.probe = exynos_tmu_probe,
	.remove	= __devexit_p(exynos_tmu_remove),
	.suspend = exynos_tmu_suspend,
	.resume = exynos_tmu_resume,
	.id_table = exynos_tmu_driver_ids,
};

module_platform_driver(exynos_tmu_driver);

MODULE_DESCRIPTION("EXYNOS TMU Driver");
MODULE_AUTHOR("Donggeun Kim <dg77.kim@samsung.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:exynos-tmu");
