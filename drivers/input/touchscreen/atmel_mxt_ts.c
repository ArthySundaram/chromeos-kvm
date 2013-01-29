/*
 * Atmel maXTouch Touchscreen driver
 *
 * Copyright (C) 2010 Samsung Electronics Co.Ltd
 * Author: Joonyoung Shim <jy0922.shim@samsung.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include <linux/async.h>
#include <linux/debugfs.h>
#include <linux/completion.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/i2c/atmel_mxt_ts.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#if defined(CONFIG_ACPI_BUTTON)
#include <acpi/button.h>
#endif


/* Version */
#define MXT_VER_20		20
#define MXT_VER_21		21
#define MXT_VER_22		22

/* Slave addresses */
#define MXT_APP_LOW		0x4a
#define MXT_APP_HIGH		0x4b
/*
 * MXT_BOOT_LOW disagrees with Atmel documentation, but has been
 * updated to support new touch hardware that pairs 0x26 boot with 0x4a app.
 */
#define MXT_BOOT_LOW		0x26
#define MXT_BOOT_HIGH		0x25

/* Firmware */
#define MXT_FW_NAME		"maxtouch.fw"

/* Config file */
#define MXT_CONFIG_NAME		"maxtouch.cfg"

/* Configuration Data */
#define MXT_CONFIG_VERSION	"OBP_RAW V1"

/* Registers */
#define MXT_INFO		0x00
#define MXT_FAMILY_ID		0x00
#define MXT_VARIANT_ID		0x01
#define MXT_VERSION		0x02
#define MXT_BUILD		0x03
#define MXT_MATRIX_X_SIZE	0x04
#define MXT_MATRIX_Y_SIZE	0x05
#define MXT_OBJECT_NUM		0x06
#define MXT_OBJECT_START	0x07

#define MXT_OBJECT_SIZE		6

/* Object types */
#define MXT_DEBUG_DIAGNOSTIC_T37	37
#define MXT_GEN_MESSAGE_T5		5
#define MXT_GEN_COMMAND_T6		6
#define MXT_GEN_POWER_T7		7
#define MXT_GEN_ACQUIRE_T8		8
#define MXT_GEN_DATASOURCE_T53		53
#define MXT_TOUCH_MULTI_T9		9
#define MXT_TOUCH_KEYARRAY_T15		15
#define MXT_TOUCH_PROXIMITY_T23		23
#define MXT_TOUCH_PROXKEY_T52		52
#define MXT_PROCI_GRIPFACE_T20		20
#define MXT_PROCG_NOISE_T22		22
#define MXT_PROCI_ONETOUCH_T24		24
#define MXT_PROCI_TWOTOUCH_T27		27
#define MXT_PROCI_GRIP_T40		40
#define MXT_PROCI_PALM_T41		41
#define MXT_PROCI_TOUCHSUPPRESSION_T42	42
#define MXT_PROCI_STYLUS_T47		47
#define MXT_PROCG_NOISESUPPRESSION_T48	48
#define MXT_PROCI_ADAPTIVETHRESHOLD_T55 55
#define MXT_PROCI_SHIELDLESS_T56	56
#define MXT_PROCI_EXTRATOUCHSCREENDATA_T57	57
#define MXT_PROCG_NOISESUPPRESSION_T62	62
#define MXT_PROCI_LENSBENDING_T65	65
#define MXT_SPT_COMMSCONFIG_T18		18
#define MXT_SPT_GPIOPWM_T19		19
#define MXT_SPT_SELFTEST_T25		25
#define MXT_SPT_CTECONFIG_T28		28
#define MXT_SPT_USERDATA_T38		38
#define MXT_SPT_DIGITIZER_T43		43
#define MXT_SPT_MESSAGECOUNT_T44	44
#define MXT_SPT_CTECONFIG_T46		46
#define MXT_SPT_TIMER_T61		61

/* MXT_GEN_COMMAND_T6 field */
#define MXT_COMMAND_RESET	0
#define MXT_COMMAND_BACKUPNV	1
#define MXT_COMMAND_CALIBRATE	2
#define MXT_COMMAND_REPORTALL	3
#define MXT_COMMAND_DIAGNOSTIC	5

#define MXT_T6_CMD_PAGE_UP		0x01
#define MXT_T6_CMD_PAGE_DOWN		0x02
#define MXT_T6_CMD_DELTAS		0x10
#define MXT_T6_CMD_REFS			0x11
#define MXT_T6_CMD_DEVICE_ID		0x80
#define MXT_T6_CMD_TOUCH_THRESH		0xF4

/* MXT_GEN_POWER_T7 field */
#define MXT_POWER_IDLEACQINT	0
#define MXT_POWER_ACTVACQINT	1
#define MXT_POWER_ACTV2IDLETO	2

/* MXT_GEN_ACQUIRE_T8 field */
#define MXT_ACQUIRE_CHRGTIME	0
#define MXT_ACQUIRE_TCHDRIFT	2
#define MXT_ACQUIRE_DRIFTST	3
#define MXT_ACQUIRE_TCHAUTOCAL	4
#define MXT_ACQUIRE_SYNC	5
#define MXT_ACQUIRE_ATCHCALST	6
#define MXT_ACQUIRE_ATCHCALSTHR	7

/* MXT_TOUCH_MULTI_T9 field */
#define MXT_TOUCH_CTRL		0
#define MXT_TOUCH_XORIGIN	1
#define MXT_TOUCH_YORIGIN	2
#define MXT_TOUCH_XSIZE		3
#define MXT_TOUCH_YSIZE		4
#define MXT_TOUCH_BLEN		6
#define MXT_TOUCH_TCHTHR	7
#define MXT_TOUCH_TCHDI		8
#define MXT_TOUCH_ORIENT	9
#define MXT_TOUCH_MOVHYSTI	11
#define MXT_TOUCH_MOVHYSTN	12
#define MXT_TOUCH_NUMTOUCH	14
#define MXT_TOUCH_MRGHYST	15
#define MXT_TOUCH_MRGTHR	16
#define MXT_TOUCH_AMPHYST	17
#define MXT_TOUCH_XRANGE_LSB	18
#define MXT_TOUCH_XRANGE_MSB	19
#define MXT_TOUCH_YRANGE_LSB	20
#define MXT_TOUCH_YRANGE_MSB	21
#define MXT_TOUCH_XLOCLIP	22
#define MXT_TOUCH_XHICLIP	23
#define MXT_TOUCH_YLOCLIP	24
#define MXT_TOUCH_YHICLIP	25
#define MXT_TOUCH_XEDGECTRL	26
#define MXT_TOUCH_XEDGEDIST	27
#define MXT_TOUCH_YEDGECTRL	28
#define MXT_TOUCH_YEDGEDIST	29
#define MXT_TOUCH_JUMPLIMIT	30

/* MXT_TOUCH_CTRL bits */
#define MXT_TOUCH_CTRL_ENABLE	(1 << 0)
#define MXT_TOUCH_CTRL_RPTEN	(1 << 1)
#define MXT_TOUCH_CTRL_DISAMP	(1 << 2)
#define MXT_TOUCH_CTRL_DISVECT	(1 << 3)
#define MXT_TOUCH_CTRL_DISMOVE	(1 << 4)
#define MXT_TOUCH_CTRL_DISREL	(1 << 5)
#define MXT_TOUCH_CTRL_DISPRESS	(1 << 6)
#define MXT_TOUCH_CTRL_SCANEN	(1 << 7)
#define MXT_TOUCH_CTRL_OPERATIONAL	(MXT_TOUCH_CTRL_ENABLE | \
					 MXT_TOUCH_CTRL_SCANEN | \
					 MXT_TOUCH_CTRL_RPTEN)
#define MXT_TOUCH_CTRL_SCANNING		(MXT_TOUCH_CTRL_ENABLE | \
					 MXT_TOUCH_CTRL_SCANEN)

/* MXT_PROCI_GRIPFACE_T20 field */
#define MXT_GRIPFACE_CTRL	0
#define MXT_GRIPFACE_XLOGRIP	1
#define MXT_GRIPFACE_XHIGRIP	2
#define MXT_GRIPFACE_YLOGRIP	3
#define MXT_GRIPFACE_YHIGRIP	4
#define MXT_GRIPFACE_MAXTCHS	5
#define MXT_GRIPFACE_SZTHR1	7
#define MXT_GRIPFACE_SZTHR2	8
#define MXT_GRIPFACE_SHPTHR1	9
#define MXT_GRIPFACE_SHPTHR2	10
#define MXT_GRIPFACE_SUPEXTTO	11

/* MXT_PROCI_NOISE field */
#define MXT_NOISE_CTRL		0
#define MXT_NOISE_OUTFLEN	1
#define MXT_NOISE_GCAFUL_LSB	3
#define MXT_NOISE_GCAFUL_MSB	4
#define MXT_NOISE_GCAFLL_LSB	5
#define MXT_NOISE_GCAFLL_MSB	6
#define MXT_NOISE_ACTVGCAFVALID	7
#define MXT_NOISE_NOISETHR	8
#define MXT_NOISE_FREQHOPSCALE	10
#define MXT_NOISE_FREQ0		11
#define MXT_NOISE_FREQ1		12
#define MXT_NOISE_FREQ2		13
#define MXT_NOISE_FREQ3		14
#define MXT_NOISE_FREQ4		15
#define MXT_NOISE_IDLEGCAFVALID	16

/* MXT_SPT_COMMSCONFIG_T18 */
#define MXT_COMMS_CTRL		0
#define MXT_COMMS_CMD		1

/* MXT_SPT_CTECONFIG_T28 field */
#define MXT_CTE_CTRL		0
#define MXT_CTE_CMD		1
#define MXT_CTE_MODE		2
#define MXT_CTE_IDLEGCAFDEPTH	3
#define MXT_CTE_ACTVGCAFDEPTH	4
#define MXT_CTE_VOLTAGE		5

#define MXT_VOLTAGE_DEFAULT	2700000
#define MXT_VOLTAGE_STEP	10000

/* Define for MXT_GEN_COMMAND_T6 */
#define MXT_BOOT_VALUE		0xa5
#define MXT_BACKUP_VALUE	0x55
#define MXT_BACKUP_TIME		270	/* msec */
#define MXT_RESET_TIME		350	/* msec */
#define MXT_CAL_TIME		25	/* msec */

#define MXT_FWRESET_TIME	500	/* msec */

/* Default value for acquisition interval when in suspend mode*/
#define MXT_SUSPEND_ACQINT_VALUE 32      /* msec */

/* MXT_SPT_GPIOPWM_T19 field */
#define MXT_GPIO0_MASK		0x04
#define MXT_GPIO1_MASK		0x08
#define MXT_GPIO2_MASK		0x10
#define MXT_GPIO3_MASK		0x20

/* Command to unlock bootloader */
#define MXT_UNLOCK_CMD_MSB	0xaa
#define MXT_UNLOCK_CMD_LSB	0xdc

/* Bootloader mode status */
#define MXT_WAITING_BOOTLOAD_CMD	0xc0	/* valid 7 6 bit only */
#define MXT_WAITING_FRAME_DATA	0x80	/* valid 7 6 bit only */
#define MXT_FRAME_CRC_CHECK	0x02
#define MXT_FRAME_CRC_FAIL	0x03
#define MXT_FRAME_CRC_PASS	0x04
#define MXT_APP_CRC_FAIL	0x40	/* valid 7 8 bit only */
#define MXT_BOOT_STATUS_MASK	0x3f

/* Touch status */
#define MXT_UNGRIP		(1 << 0)
#define MXT_SUPPRESS		(1 << 1)
#define MXT_AMP			(1 << 2)
#define MXT_VECTOR		(1 << 3)
#define MXT_MOVE		(1 << 4)
#define MXT_RELEASE		(1 << 5)
#define MXT_PRESS		(1 << 6)
#define MXT_DETECT		(1 << 7)

/* Touch orient bits */
#define MXT_XY_SWITCH		(1 << 0)
#define MXT_X_INVERT		(1 << 1)
#define MXT_Y_INVERT		(1 << 2)

#define MXT_MAX_FINGER		10

/* For CMT (must match XRANGE/YRANGE as defined in board config */
#define MXT_PIXELS_PER_MM	20

struct mxt_cfg_file_hdr {
	bool valid;
	u32 info_crc;
	u32 cfg_crc;
};

struct mxt_cfg_file_line {
	struct list_head list;
	u16 addr;
	u8 size;
	u8 *content;
};

struct mxt_info {
	u8 family_id;
	u8 variant_id;
	u8 version;
	u8 build;
	u8 matrix_xsize;
	u8 matrix_ysize;
	u8 object_num;
};

struct mxt_object {
	u8 type;
	u16 start_address;
	u16 size;
	u16 instances;
	u8 num_report_ids;
};

struct mxt_message {
	u8 reportid;
	u8 message[7];
};

/* Each client has this additional data */
struct mxt_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	const struct mxt_platform_data *pdata;
	struct mxt_object *object_table;
	struct mxt_info info;
	bool is_tp;

	bool irq_wake;  /* irq wake is enabled */

	/* for fw update in bootloader */
	struct completion bl_completion;

	/* for auto-calibration in suspend */
	struct completion auto_cal_completion;

	unsigned int irq;
	unsigned int max_x;
	unsigned int max_y;

	/* max touchscreen area in terms of pixels and channels */
	unsigned int max_area_pixels;
	unsigned int max_area_channels;

	u32 info_csum;
	u32 config_csum;

	/* Cached parameters from object table */
	u16 T5_address;
	u8 T6_reportid;
	u8 T9_reportid_min;
	u8 T9_reportid_max;
	u8 T19_reportid;
	u16 T44_address;

	/* Saved T7 configuration
	 * [0] = IDLEACQINT
	 * [1] = ACTVACQINT
	 * [2] = ACTV2IDLETO
	 */
	u8 T7_config[3];
	bool T7_config_valid;

	/* T7 IDLEACQINT & ACTVACQINT setting when in suspend mode*/
	u8 suspend_acq_interval;

	/* Saved T9 Ctrl field */
	u8 T9_ctrl;
	bool T9_ctrl_valid;


	/* Saved T42 Touch Suppression field */
	u8 T42_ctrl;
	bool T42_ctrl_valid;

	/* Saved T19 GPIO config */
	u8 T19_ctrl;
	bool T19_ctrl_valid;

	/* per-instance debugfs root */
	struct dentry *dentry_dev;
	struct dentry *dentry_deltas;
	struct dentry *dentry_refs;
	struct dentry *dentry_object;

	/* Protect access to the T37 object buffer, used by debugfs */
	struct mutex T37_buf_mutex;
	u8 *T37_buf;
	size_t T37_buf_size;

	/* Protect access to the object register buffer */
	struct mutex object_str_mutex;
	char *object_str;
	size_t object_str_size;

	/* firmware file name */
	char *fw_file;

	/* config file name */
	char *config_file;

	/* map for the tracking id currently being used */
	bool current_id[MXT_MAX_FINGER];

#if defined(CONFIG_ACPI_BUTTON)
	/* notifier block for acpi_lid_notifier */
	struct notifier_block lid_notifier;
#endif
};

/* global root node of the atmel_mxt_ts debugfs directory. */
static struct dentry *mxt_debugfs_root;

static int mxt_initialize(struct mxt_data *data);
static int mxt_input_dev_create(struct mxt_data *data);
static int get_touch_major_pixels(struct mxt_data *data, int touch_channels);

static bool mxt_object_readable(unsigned int type)
{
	switch (type) {
	case MXT_GEN_COMMAND_T6:
	case MXT_GEN_POWER_T7:
	case MXT_GEN_ACQUIRE_T8:
	case MXT_GEN_DATASOURCE_T53:
	case MXT_TOUCH_MULTI_T9:
	case MXT_TOUCH_KEYARRAY_T15:
	case MXT_TOUCH_PROXIMITY_T23:
	case MXT_TOUCH_PROXKEY_T52:
	case MXT_PROCI_GRIPFACE_T20:
	case MXT_PROCG_NOISE_T22:
	case MXT_PROCI_ONETOUCH_T24:
	case MXT_PROCI_TWOTOUCH_T27:
	case MXT_PROCI_GRIP_T40:
	case MXT_PROCI_PALM_T41:
	case MXT_PROCI_TOUCHSUPPRESSION_T42:
	case MXT_PROCI_STYLUS_T47:
	case MXT_PROCG_NOISESUPPRESSION_T48:
	case MXT_PROCI_ADAPTIVETHRESHOLD_T55:
	case MXT_PROCI_SHIELDLESS_T56:
	case MXT_PROCI_EXTRATOUCHSCREENDATA_T57:
	case MXT_PROCG_NOISESUPPRESSION_T62:
	case MXT_PROCI_LENSBENDING_T65:
	case MXT_SPT_COMMSCONFIG_T18:
	case MXT_SPT_GPIOPWM_T19:
	case MXT_SPT_SELFTEST_T25:
	case MXT_SPT_CTECONFIG_T28:
	case MXT_DEBUG_DIAGNOSTIC_T37:
	case MXT_SPT_DIGITIZER_T43:
	case MXT_SPT_CTECONFIG_T46:
	case MXT_SPT_TIMER_T61:
	case MXT_SPT_USERDATA_T38:
		return true;
	default:
		return false;
	}
}

static bool mxt_object_writable(unsigned int type)
{
	switch (type) {
	case MXT_GEN_COMMAND_T6:
	case MXT_GEN_POWER_T7:
	case MXT_GEN_ACQUIRE_T8:
	case MXT_TOUCH_MULTI_T9:
	case MXT_TOUCH_KEYARRAY_T15:
	case MXT_TOUCH_PROXIMITY_T23:
	case MXT_TOUCH_PROXKEY_T52:
	case MXT_PROCI_GRIPFACE_T20:
	case MXT_PROCG_NOISE_T22:
	case MXT_PROCI_ONETOUCH_T24:
	case MXT_PROCI_TWOTOUCH_T27:
	case MXT_PROCI_GRIP_T40:
	case MXT_PROCI_PALM_T41:
	case MXT_PROCI_TOUCHSUPPRESSION_T42:
	case MXT_PROCI_STYLUS_T47:
	case MXT_PROCG_NOISESUPPRESSION_T48:
	case MXT_PROCI_ADAPTIVETHRESHOLD_T55:
	case MXT_PROCI_SHIELDLESS_T56:
	case MXT_PROCI_EXTRATOUCHSCREENDATA_T57:
	case MXT_PROCG_NOISESUPPRESSION_T62:
	case MXT_PROCI_LENSBENDING_T65:
	case MXT_SPT_COMMSCONFIG_T18:
	case MXT_SPT_GPIOPWM_T19:
	case MXT_SPT_SELFTEST_T25:
	case MXT_SPT_CTECONFIG_T28:
	case MXT_SPT_DIGITIZER_T43:
	case MXT_SPT_CTECONFIG_T46:
	case MXT_SPT_TIMER_T61:
		return true;
	default:
		return false;
	}
}

static void mxt_dump_message(struct device *dev,
			     struct mxt_message *message)
{
	dev_dbg(dev, "reportid: %u\tmessage: %02x %02x %02x %02x %02x %02x %02x\n",
		message->reportid, message->message[0], message->message[1],
		message->message[2], message->message[3], message->message[4],
		message->message[5], message->message[6]);
}

/*
 * Release all the fingers that are being tracked. To avoid unwanted gestures,
 * move all the fingers to (0,0) with largest PRESSURE and TOUCH_MAJOR.
 * Userspace apps can use these info to filter out these events and/or cancel
 * existing gestures.
 */
static void mxt_release_all_fingers(struct mxt_data *data)
{
	struct device *dev = &data->client->dev;
	struct input_dev *input_dev = data->input_dev;
	int id;
	int max_area_channels = min(255U, data->max_area_channels);
	int max_touch_major = get_touch_major_pixels(data, max_area_channels);
	bool need_update = false;
	for (id = 0; id < MXT_MAX_FINGER; id++) {
		if (data->current_id[id]) {
			dev_warn(dev, "Move touch %d to (0,0)\n", id);
			input_mt_slot(input_dev, id);
			input_mt_report_slot_state(input_dev, MT_TOOL_FINGER,
						   true);
			input_report_abs(input_dev, ABS_MT_POSITION_X, 0);
			input_report_abs(input_dev, ABS_MT_POSITION_Y, 0);
			input_report_abs(input_dev, ABS_MT_PRESSURE, 255);
			input_report_abs(input_dev, ABS_MT_TOUCH_MAJOR,
					 max_touch_major);
			need_update = true;
		}
	}
	if (need_update)
		input_sync(data->input_dev);

	for (id = 0; id < MXT_MAX_FINGER; id++) {
		if (data->current_id[id]) {
			dev_warn(dev, "Release touch contact %d\n", id);
			input_mt_slot(input_dev, id);
			input_mt_report_slot_state(input_dev, MT_TOOL_FINGER,
						   false);
			data->current_id[id] = false;
		}
	}
	if (need_update)
		input_sync(data->input_dev);
}

static int mxt_wait_for_chg(struct mxt_data *data, unsigned int timeout_ms)
{
	struct device *dev = &data->client->dev;
	struct completion *comp = &data->bl_completion;
	unsigned long timeout = msecs_to_jiffies(timeout_ms);
	long ret;

	ret = wait_for_completion_interruptible_timeout(comp, timeout);
	if (ret < 0) {
		dev_err(dev, "Wait for completion interrupted.\n");
		/*
		 * TODO: handle -EINTR better by terminating fw update process
		 * before returning to userspace by writing length 0x000 to
		 * device (iff we are in WAITING_FRAME_DATA state).
		 */
		return -EINTR;
	} else if (ret == 0) {
		dev_err(dev, "Wait for completion timed out.\n");
		return -ETIMEDOUT;
	}
	return 0;
}

static int mxt_check_bootloader(struct mxt_data *data, unsigned int state)
{
	struct i2c_client *client = data->client;
	int count;
	u8 val;

recheck:
	if (state != MXT_WAITING_BOOTLOAD_CMD) {
		/*
		 * In application update mode, the interrupt
		 * line signals state transitions. We must wait for the
		 * CHG assertion before reading the status byte.
		 * Once the status byte has been read, the line is deasserted.
		 */
		int ret = mxt_wait_for_chg(data, 300);
		if (ret) {
			dev_err(&client->dev, "Update wait error %d\n", ret);
			return ret;
		}
	}

	count = i2c_master_recv(client, &val, 1);
	if (count != 1) {
		dev_err(&client->dev, "%s: i2c recv failed\n", __func__);
		return count < 0 ? count : -EIO;
	}

	switch (state) {
	case MXT_WAITING_BOOTLOAD_CMD:
		dev_info(&client->dev, "bootloader version: %d\n",
			 val & MXT_BOOT_STATUS_MASK);
	case MXT_WAITING_FRAME_DATA:
		val &= ~MXT_BOOT_STATUS_MASK;
		break;
	case MXT_FRAME_CRC_PASS:
		if (val == MXT_FRAME_CRC_CHECK)
			goto recheck;
		break;
	default:
		return -EINVAL;
	}

	if (val != state) {
		dev_err(&client->dev, "Invalid bootloader mode state %d, %d\n",
			val, state);
		return -EINVAL;
	}

	return 0;
}

static int mxt_unlock_bootloader(struct i2c_client *client)
{
	int count;
	u8 buf[2];

	buf[0] = MXT_UNLOCK_CMD_LSB;
	buf[1] = MXT_UNLOCK_CMD_MSB;

	count = i2c_master_send(client, buf, 2);
	if (count != 2) {
		dev_err(&client->dev, "%s: i2c send failed\n", __func__);
		return count < 0 ? count : -EIO;
	}

	return 0;
}

static int mxt_fw_write(struct i2c_client *client,
			     const u8 *data, unsigned int frame_size)
{
	int count;
	count = i2c_master_send(client, data, frame_size);
	if (count != frame_size) {
		dev_err(&client->dev, "%s: i2c send failed\n", __func__);
		return count < 0 ? count : -EIO;
	}

	return 0;
}

#ifdef DEBUG
#define DUMP_LEN	16
static void mxt_dump_xfer(struct device *dev, const char *func, u16 reg,
			  u16 len, const u8 *val)
{
	/* Rough guess for string size */
	char str[DUMP_LEN * 3 + 2];
	int i;
	size_t n;

	for (i = 0, n = 0; i < len; i++) {
		n += snprintf(&str[n], sizeof(str) - n, "%02x ", val[i]);
		if ((i + 1) % DUMP_LEN == 0 || (i + 1) == len) {
			dev_dbg(dev,
				"%s(reg: %d len: %d offset: 0x%02x): %s\n",
				func, reg, len, (i / DUMP_LEN) * DUMP_LEN,
				str);
			n = 0;
		}
	}
}
#undef DUMP_LEN
#else
static void mxt_dump_xfer(struct device *dev, const char *func, u16 reg,
			  u16 len, const u8 *val) { }
#endif

static int mxt_read_reg(struct i2c_client *client, u16 reg, u16 len, void *val)
{
	struct i2c_msg xfer[2];
	int ret;
	u8 buf[2];

	buf[0] = reg & 0xff;
	buf[1] = (reg >> 8) & 0xff;

	/* Write register */
	xfer[0].addr = client->addr;
	xfer[0].flags = 0;
	xfer[0].len = 2;
	xfer[0].buf = buf;

	/* Read data */
	xfer[1].addr = client->addr;
	xfer[1].flags = I2C_M_RD;
	xfer[1].len = len;
	xfer[1].buf = val;

	ret = i2c_transfer(client->adapter, xfer, 2);
	if (ret != 2) {
		dev_err(&client->dev, "%s: i2c read failed\n", __func__);
		return ret < 0 ? ret : -EIO;
	}

	mxt_dump_xfer(&client->dev, __func__, reg, len, val);

	return 0;
}

static int mxt_write_reg(struct i2c_client *client, u16 reg, u16 len,
			 const void *val)
{
	size_t count = 2 + len;		/* + 2-byte offset */
	int ret;
	u8 buf[count];

	buf[0] = reg & 0xff;
	buf[1] = (reg >> 8) & 0xff;
	memcpy(&buf[2], val, len);

	mxt_dump_xfer(&client->dev, __func__, reg, len, val);

	ret = i2c_master_send(client, buf, count);
	if (ret != count) {
		dev_err(&client->dev, "%s: i2c write failed\n", __func__);
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

static struct mxt_object *mxt_get_object(struct mxt_data *data, u8 type)
{
	struct mxt_object *object;
	int i;

	for (i = 0; i < data->info.object_num; i++) {
		object = data->object_table + i;
		if (object->type == type)
			return object;
	}

	dev_err(&data->client->dev, "Invalid object type\n");
	return NULL;
}

static int mxt_read_object(struct mxt_data *data, struct mxt_object *object,
			   u8 instance, void *val)
{
	u16 addr;

	BUG_ON(instance >= object->instances);
	addr = object->start_address + instance * object->size;
	return mxt_read_reg(data->client, addr, object->size, val);
}

static int mxt_write_object(struct mxt_data *data, u8 type, u8 instance,
			    u8 offset, u8 val)
{
	struct mxt_object *object;
	u16 reg;

	object = mxt_get_object(data, type);
	if (!object || instance >= object->instances || offset >= object->size)
		return -EINVAL;

	reg = object->start_address + instance * object->size + offset;
	return mxt_write_reg(data->client, reg, 1, &val);
}

static int mxt_read_num_messages(struct mxt_data *data, u8 *count)
{
	/* TODO: Optimization: read first message along with message count */
	return mxt_read_reg(data->client, data->T44_address, 1, count);
}

static int mxt_read_messages(struct mxt_data *data, u8 count,
			     struct mxt_message *messages)
{
	return mxt_read_reg(data->client, data->T5_address,
			    sizeof(struct mxt_message) * count, messages);
}

static void mxt_input_button(struct mxt_data *data, struct mxt_message *message)
{
	struct device *dev = &data->client->dev;
	struct input_dev *input = data->input_dev;
	bool button;

	/* Active-low switch */
	button = !(message->message[0] & MXT_GPIO3_MASK);
	input_report_key(input, BTN_LEFT, button);
	dev_dbg(dev, "Button state: %d\n", button);
}

/*
 * Assume a circle touch contact and use the diameter as the touch major.
 * touch_pixels = touch_channels * (max_area_pixels / max_area_channels)
 * touch_pixels = pi * (touch_major / 2) ^ 2;
 */
static int get_touch_major_pixels(struct mxt_data *data, int touch_channels)
{
	int touch_pixels;

	if (data->max_area_channels == 0)
		return 0;

	touch_pixels = DIV_ROUND_CLOSEST(touch_channels * data->max_area_pixels,
					 data->max_area_channels);
	return int_sqrt(DIV_ROUND_CLOSEST(touch_pixels * 100, 314)) * 2;
}

static void mxt_input_touch(struct mxt_data *data, struct mxt_message *message)
{
	struct device *dev = &data->client->dev;
	struct input_dev *input_dev = data->input_dev;
	u8 status;
	int x;
	int y;
	int area;
	int amplitude;
	int vector1, vector2;
	int id;
	int touch_major;

	id = message->reportid - data->T9_reportid_min;

	status = message->message[0];
	x = (message->message[1] << 4) | ((message->message[3] >> 4) & 0xf);
	y = (message->message[2] << 4) | ((message->message[3] & 0xf));
	if (data->max_x < 1024)
		x >>= 2;
	if (data->max_y < 1024)
		y >>= 2;

	area = message->message[4];
	touch_major = get_touch_major_pixels(data, area);
	amplitude = message->message[5];

	/* The two vector components are 4-bit signed ints (2s complement) */
	vector1 = (signed)((signed char)message->message[6]) >> 4;
	vector2 = (signed)((signed char)(message->message[6] << 4)) >> 4;

	dev_dbg(dev,
		"[%d] %c%c%c%c%c%c%c%c x: %d y: %d area: %d amp: %d vector: [%d,%d]\n",
		id,
		(status & MXT_DETECT) ? 'D' : '.',
		(status & MXT_PRESS) ? 'P' : '.',
		(status & MXT_RELEASE) ? 'R' : '.',
		(status & MXT_MOVE) ? 'M' : '.',
		(status & MXT_VECTOR) ? 'V' : '.',
		(status & MXT_AMP) ? 'A' : '.',
		(status & MXT_SUPPRESS) ? 'S' : '.',
		(status & MXT_UNGRIP) ? 'U' : '.',
		x, y, area, amplitude, vector1, vector2);

	input_mt_slot(input_dev, id);
	input_mt_report_slot_state(input_dev, MT_TOOL_FINGER,
				   status & MXT_DETECT);
	data->current_id[id] = status & MXT_DETECT;

	if (status & MXT_DETECT) {
		input_report_abs(input_dev, ABS_MT_POSITION_X, x);
		input_report_abs(input_dev, ABS_MT_POSITION_Y, y);
		input_report_abs(input_dev, ABS_MT_PRESSURE, amplitude);
		input_report_abs(input_dev, ABS_MT_TOUCH_MAJOR, touch_major);
		/* TODO: Use vector to report ORIENTATION & TOUCH_MINOR */
	}
}

static int mxt_proc_messages(struct mxt_data *data, u8 count, bool report)
{
	struct device *dev = &data->client->dev;
	struct mxt_message messages[count], *msg;
	int ret;
	bool update_input;

	ret = mxt_read_messages(data, count, messages);
	if (ret) {
		dev_err(dev, "Failed to read %u messages (%d).\n", count, ret);
		return ret;
	}
	if (!report)
		return 0;

	update_input = false;
	for (msg = messages; msg < &messages[count]; msg++) {
		mxt_dump_message(dev, msg);

		if (msg->reportid >= data->T9_reportid_min &&
		    msg->reportid <= data->T9_reportid_max) {
			mxt_input_touch(data, msg);
			update_input = true;
		} else if (msg->reportid == data->T19_reportid) {
			mxt_input_button(data, msg);
			update_input = true;
		} else if (msg->reportid == data->T6_reportid) {
			data->config_csum = msg->message[1] |
					    (msg->message[2] << 8) |
					    (msg->message[3] << 16);
			dev_info(dev, "Status: %02x Config Checksum: %06x\n",
				 msg->message[0], data->config_csum);
			if (msg->message[0] == 0x00)
				complete(&data->auto_cal_completion);
		}
	}

	if (update_input) {
		input_mt_report_pointer_emulation(data->input_dev,
						  data->is_tp);
		input_sync(data->input_dev);
	}

	return 0;
}

static int mxt_handle_messages(struct mxt_data *data, bool report)
{
	struct device *dev = &data->client->dev;
	int ret;
	u8 count;

	ret = mxt_read_num_messages(data, &count);
	if (ret) {
		dev_err(dev, "Failed to read message count (%d).\n", ret);
		return ret;
	}

	if (count > 0)
		ret = mxt_proc_messages(data, count, report);

	return ret;
}

static bool mxt_in_bootloader(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	return (client->addr == MXT_BOOT_LOW || client->addr == MXT_BOOT_HIGH);
}

static int mxt_enter_bl(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	struct device *dev = &client->dev;
	int ret;

	if (mxt_in_bootloader(data))
		return 0;

	disable_irq(data->irq);

	if (data->input_dev) {
		input_unregister_device(data->input_dev);
		data->input_dev = NULL;
	}

	/* Change to the bootloader mode */
	ret = mxt_write_object(data, MXT_GEN_COMMAND_T6, 0,
			       MXT_COMMAND_RESET, MXT_BOOT_VALUE);
	if (ret) {
		enable_irq(data->irq);
		return ret;
	}

	/* Change to slave address of bootloader */
	if (client->addr == MXT_APP_LOW)
		client->addr = MXT_BOOT_LOW;
	else
		client->addr = MXT_BOOT_HIGH;

	INIT_COMPLETION(data->bl_completion);
	enable_irq(data->irq);

	/* Wait for CHG assert to indicate successful reset into bootloader */
	ret = mxt_wait_for_chg(data, MXT_RESET_TIME);
	if (ret) {
		dev_err(dev, "Failed waiting for reset to bootloader.\n");
		if (client->addr == MXT_BOOT_LOW)
			client->addr = MXT_APP_LOW;
		else
			client->addr = MXT_APP_HIGH;
		return ret;
	}
	return 0;
}

static void mxt_exit_bl(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	struct device *dev = &client->dev;
	int error;

	if (!mxt_in_bootloader(data))
		return;

	/* Wait for reset */
	mxt_wait_for_chg(data, MXT_FWRESET_TIME);

	disable_irq(data->irq);
	if (client->addr == MXT_BOOT_LOW)
		client->addr = MXT_APP_LOW;
	else
		client->addr = MXT_APP_HIGH;

	kfree(data->object_table);
	data->object_table = NULL;

	error = mxt_initialize(data);
	if (error) {
		dev_err(dev, "Failed to initialize on exit bl. error = %d\n",
			error);
		return;
	}

	error = mxt_input_dev_create(data);
	if (error) {
		dev_err(dev, "Create input dev failed after init. error = %d\n",
			error);
		return;
	}

	error = mxt_handle_messages(data, false);
	if (error)
		dev_err(dev, "Handle messages failed after init. error = %d\n",
			error);
	enable_irq(data->irq);
}

static irqreturn_t mxt_interrupt(int irq, void *dev_id)
{
	struct mxt_data *data = (struct mxt_data *)dev_id;

	if (mxt_in_bootloader(data)) {
		/* bootloader state transition completion */
		complete(&data->bl_completion);
	} else {
		mxt_handle_messages(data, true);
	}
	return IRQ_HANDLED;
}

static int mxt_apply_pdata_config(struct mxt_data *data)
{
	const struct mxt_platform_data *pdata = data->pdata;
	struct device *dev = &data->client->dev;
	int i, offset;
	int ret;

	if (!pdata->config) {
		dev_info(dev, "No cfg data defined, skipping reg init\n");
		return 0;
	}

	for (offset = 0, i = 0; i < data->info.object_num; i++) {
		struct mxt_object *object = &data->object_table[i];
		size_t config_size;

		if (!mxt_object_writable(object->type))
			continue;

		config_size = object->size * object->instances;
		if (offset + config_size > pdata->config_length) {
			dev_err(dev, "Not enough config data!\n");
			return -EINVAL;
		}

		ret = mxt_write_reg(data->client, object->start_address,
				    config_size, &pdata->config[offset]);
		if (ret)
			return ret;
		offset += config_size;
	}

	return 0;
}

static int mxt_handle_pdata(struct mxt_data *data)
{
	const struct mxt_platform_data *pdata = data->pdata;
	struct device *dev = &data->client->dev;
	u8 voltage;
	int ret;

	if (!pdata) {
		dev_info(dev, "No platform data provided\n");
		return 0;
	}

	ret = mxt_apply_pdata_config(data);
	if (ret)
		return ret;

	/* Set touchscreen lines */
	mxt_write_object(data, MXT_TOUCH_MULTI_T9, 0,
			 MXT_TOUCH_XSIZE, pdata->x_line);
	mxt_write_object(data, MXT_TOUCH_MULTI_T9, 0,
			 MXT_TOUCH_YSIZE, pdata->y_line);

	/* Set touchscreen orient */
	mxt_write_object(data, MXT_TOUCH_MULTI_T9, 0,
			 MXT_TOUCH_ORIENT, pdata->orient);

	/* Set touchscreen burst length */
	mxt_write_object(data, MXT_TOUCH_MULTI_T9, 0,
			 MXT_TOUCH_BLEN, pdata->blen);

	/* Set touchscreen threshold */
	mxt_write_object(data, MXT_TOUCH_MULTI_T9, 0,
			 MXT_TOUCH_TCHTHR, pdata->threshold);

	/* Set touchscreen resolution */
	mxt_write_object(data, MXT_TOUCH_MULTI_T9, 0,
			 MXT_TOUCH_XRANGE_LSB, (pdata->x_size - 1) & 0xff);
	mxt_write_object(data, MXT_TOUCH_MULTI_T9, 0,
			 MXT_TOUCH_XRANGE_MSB, (pdata->x_size - 1) >> 8);
	mxt_write_object(data, MXT_TOUCH_MULTI_T9, 0,
			 MXT_TOUCH_YRANGE_LSB, (pdata->y_size - 1) & 0xff);
	mxt_write_object(data, MXT_TOUCH_MULTI_T9, 0,
			 MXT_TOUCH_YRANGE_MSB, (pdata->y_size - 1) >> 8);

	/* Set touchscreen voltage */
	if (pdata->voltage) {
		if (pdata->voltage < MXT_VOLTAGE_DEFAULT) {
			voltage = (MXT_VOLTAGE_DEFAULT - pdata->voltage) /
				MXT_VOLTAGE_STEP;
			voltage = 0xff - voltage + 1;
		} else
			voltage = (pdata->voltage - MXT_VOLTAGE_DEFAULT) /
				MXT_VOLTAGE_STEP;

		mxt_write_object(data, MXT_SPT_CTECONFIG_T28, 0,
				 MXT_CTE_VOLTAGE, voltage);
	}

	/* Backup to memory */
	ret = mxt_write_object(data, MXT_GEN_COMMAND_T6, 0,
			       MXT_COMMAND_BACKUPNV, MXT_BACKUP_VALUE);
	if (ret)
		return ret;
	msleep(MXT_BACKUP_TIME);

	return 0;
}

/* Update 24-bit CRC with two new bytes of data */
static u32 crc24_step(u32 crc, u8 byte1, u8 byte2)
{
	const u32 crcpoly = 0x80001b;
	u16 data = byte1 | (byte2 << 8);
	u32 result = data ^ (crc << 1);

	/* XOR result with crcpoly if bit 25 is set (overflow occurred) */
	if (result & 0x01000000)
		result ^= crcpoly;

	return result & 0x00ffffff;
}

static u32 crc24(u32 crc, const u8 *data, size_t len)
{
	size_t i;

	for (i = 0; i < len - 1; i += 2)
		crc = crc24_step(crc, data[i], data[i + 1]);

	/* If there were an odd number of bytes pad with 0 */
	if (i < len)
		crc = crc24_step(crc, data[i], 0);

	return crc;
}

static int mxt_verify_info_block_csum(struct mxt_data *data, const void *buf)
{
	struct i2c_client *client = data->client;
	struct device *dev = &client->dev;
	size_t object_table_size, info_block_size;
	u32 crc = 0;
	u8 *info_block;
	int ret = 0;

	object_table_size = data->info.object_num * MXT_OBJECT_SIZE;
	info_block_size = sizeof(data->info) + object_table_size;
	info_block = kmalloc(info_block_size, GFP_KERNEL);
	if (!info_block)
		return -ENOMEM;

	/*
	 * Information Block CRC is computed over both ID info and Object Table
	 * So concat them in a temporary buffer, before computing CRC.
	 * TODO: refactor how the info block is read from the device such
	 * that it ends up in a single buffer and this copy is not needed.
	 */
	memcpy(info_block, &data->info, sizeof(data->info));
	memcpy(&info_block[sizeof(data->info)], buf, object_table_size);

	crc = crc24(crc, info_block, info_block_size);

	if (crc != data->info_csum) {
		dev_err(dev, "Information Block CRC mismatch: %06x != %06x\n",
			data->info_csum, crc);
		ret = -EINVAL;
	}

	kfree(info_block);
	return ret;
}

static int mxt_get_object_table(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	struct device *dev = &client->dev;
	int error;
	int i;
	u8 reportid;
	u8 buf[data->info.object_num][MXT_OBJECT_SIZE];
	u8 csum[3];

	data->object_table = kcalloc(data->info.object_num,
				     sizeof(struct mxt_object), GFP_KERNEL);
	if (!data->object_table) {
		dev_err(dev, "Failed to allocate object table\n");
		return -ENOMEM;
	}

	error = mxt_read_reg(client, MXT_OBJECT_START, sizeof(buf), buf);
	if (error)
		return error;

	/*
	 * Read Information Block checksum from 3 bytes immediately following
	 * info block
	 */
	error = mxt_read_reg(client, MXT_OBJECT_START + sizeof(buf),
			     sizeof(csum), csum);
	if (error)
		return error;

	data->info_csum = csum[0] | (csum[1] << 8) | (csum[2] << 16);
	dev_info(dev, "Information Block Checksum = %06x\n", data->info_csum);

	error = mxt_verify_info_block_csum(data, buf);
	if (error)
		return error;

	/* Valid Report IDs start counting from 1 */
	reportid = 1;
	for (i = 0; i < data->info.object_num; i++) {
		struct mxt_object *object = &data->object_table[i];
		u8 num_ids, min_id, max_id;

		object->type = buf[i][0];
		object->start_address = (buf[i][2] << 8) | buf[i][1];
		object->size = buf[i][3] + 1;
		object->instances = buf[i][4] + 1;
		object->num_report_ids = buf[i][5];

		num_ids = object->num_report_ids * object->instances;
		min_id = num_ids ? reportid : 0;
		max_id = num_ids ? reportid + num_ids - 1 : 0;
		reportid += num_ids;

		dev_info(dev,
			 "Type %2d Start %3d Size %3d Instances %2d ReportIDs %3u : %3u\n",
			 object->type, object->start_address, object->size,
			 object->instances, min_id, max_id);

		/* Save data for objects used when processing interrupts */
		switch (object->type) {
		case MXT_GEN_MESSAGE_T5:
			data->T5_address = object->start_address;
			break;
		case MXT_GEN_COMMAND_T6:
			data->T6_reportid = min_id;
			break;
		case MXT_TOUCH_MULTI_T9:
			data->T9_reportid_min = min_id;
			data->T9_reportid_max = max_id;
			break;
		case MXT_SPT_GPIOPWM_T19:
			data->T19_reportid = min_id;
			break;
		case MXT_SPT_MESSAGECOUNT_T44:
			data->T44_address = object->start_address;
			break;
		}
	}

	return 0;
}

static int mxt_calc_resolution(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	u8 orient;
	__le16 xyrange[2];
	unsigned int max_x, max_y;
	u8 xylines[2];
	int ret;

	struct mxt_object *T9 = mxt_get_object(data, MXT_TOUCH_MULTI_T9);
	if (T9 == NULL)
		return -EINVAL;

	/* Get touchscreen resolution */
	ret = mxt_read_reg(client, T9->start_address + MXT_TOUCH_XRANGE_LSB,
			   4, xyrange);
	if (ret)
		return ret;

	ret = mxt_read_reg(client, T9->start_address + MXT_TOUCH_ORIENT,
			   1, &orient);
	if (ret)
		return ret;

	ret = mxt_read_reg(client, T9->start_address + MXT_TOUCH_XSIZE,
			   2, xylines);
	if (ret)
		return ret;

	max_x = le16_to_cpu(xyrange[0]);
	max_y = le16_to_cpu(xyrange[1]);

	if (orient & MXT_XY_SWITCH) {
		data->max_x = max_y;
		data->max_y = max_x;
	} else {
		data->max_x = max_x;
		data->max_y = max_y;
	}

	data->max_area_pixels = max_x * max_y;
	data->max_area_channels = xylines[0] * xylines[1];

	return 0;
}

/*
 * Atmel Raw Config File Format
 *
 * The first four lines of the raw config file contain:
 *  1) Version
 *  2) Chip ID Information (first 7 bytes of device memory)
 *  3) Chip Information Block 24-bit CRC Checksum
 *  4) Chip Configuration 24-bit CRC Checksum
 *
 * The rest of the file consists of one line per object instance:
 *   <TYPE> <INSTANCE> <SIZE> <CONTENTS>
 *
 *  <TYPE> - 2-byte object type as hex
 *  <INSTANCE> - 2-byte object instance number as hex
 *  <SIZE> - 2-byte object size as hex
 *  <CONTENTS> - array of <SIZE> 1-byte hex values
 */
static int mxt_cfg_verify_hdr(struct mxt_data *data, char **config)
{
	struct i2c_client *client = data->client;
	struct device *dev = &client->dev;
	struct mxt_info info;
	char *token;
	int ret = 0;
	u32 crc;

	/* Process the first four lines of the file*/
	/* 1) Version */
	token = strsep(config, "\n");
	dev_info(dev, "Config File: Version = %s\n", token ?: "<null>");
	if (!token ||
	    strncmp(token, MXT_CONFIG_VERSION, strlen(MXT_CONFIG_VERSION))) {
		dev_err(dev, "Invalid config file: Bad Version\n");
		return -EINVAL;
	}

	/* 2) Chip ID */
	token = strsep(config, "\n");
	if (!token) {
		dev_err(dev, "Invalid config file: No Chip ID\n");
		return -EINVAL;
	}
	ret = sscanf(token, "%hhx %hhx %hhx %hhx %hhx %hhx %hhx",
		     &info.family_id, &info.variant_id,
		     &info.version, &info.build, &info.matrix_xsize,
		     &info.matrix_ysize, &info.object_num);
	dev_info(dev, "Config File: Chip ID = %02x %02x %02x %02x %02x %02x %02x\n",
		info.family_id, info.variant_id, info.version, info.build,
		info.matrix_xsize, info.matrix_ysize, info.object_num);
	if (ret != 7 ||
	    info.family_id != data->info.family_id ||
	    info.variant_id != data->info.variant_id ||
	    info.version != data->info.version ||
	    info.build != data->info.build ||
	    info.matrix_xsize != data->info.matrix_xsize ||
	    info.matrix_ysize != data->info.matrix_ysize ||
	    info.object_num != data->info.object_num) {
		dev_err(dev, "Invalid config file: Chip ID info mismatch\n");
		dev_err(dev, "Chip Info: %02x %02x %02x %02x %02x %02x %02x\n",
			data->info.family_id, data->info.variant_id,
			data->info.version, data->info.build,
			data->info.matrix_xsize, data->info.matrix_ysize,
			data->info.object_num);
		return -EINVAL;
	}

	/* 3) Info Block CRC */
	token = strsep(config, "\n");
	if (!token) {
		dev_err(dev, "Invalid config file: No Info Block CRC\n");
		return -EINVAL;
	}
	ret = sscanf(token, "%x", &crc);
	dev_info(dev, "Config File: Info Block CRC = %06x\n", crc);
	if (ret != 1 || crc != data->info_csum) {
		dev_err(dev, "Invalid config file: Bad Info Block CRC\n");
		return -EINVAL;
	}

	/* 4) Config CRC */
	/*
	 * Parse but don't verify against current config;
	 * TODO: Verify against CRC of rest of file?
	 */
	token = strsep(config, "\n");
	if (!token) {
		dev_err(dev, "Invalid config file: No Config CRC\n");
		return -EINVAL;
	}
	ret = sscanf(token, "%x", &crc);
	dev_info(dev, "Config File: Config CRC = %06x\n", crc);
	if (ret != 1) {
		dev_err(dev, "Invalid config file: Bad Config CRC\n");
		return -EINVAL;
	}

	return 0;
}

static int mxt_cfg_proc_line(struct mxt_data *data, const char *line,
			     struct list_head *cfg_list)
{
	int ret;
	u16 type, instance, size;
	int len;
	struct mxt_cfg_file_line *cfg_line;
	struct mxt_object *object;
	u8 *content;
	size_t i;

	ret = sscanf(line, "%hx %hx %hx%n", &type, &instance, &size, &len);
	/* Skip unparseable lines */
	if (ret < 3)
		return 0;
	/* Only support 1-byte types */
	if (type > 0xff)
		return -EINVAL;

	/* Supplied object MUST be a valid instance and match object size */
	object = mxt_get_object(data, type);
	if (!object || instance > object->instances || size != object->size)
		return -EINVAL;

	content = kmalloc(size, GFP_KERNEL);
	if (!content)
		return -ENOMEM;

	for (i = 0; i < size; i++) {
		line += len;
		ret = sscanf(line, "%hhx%n", &content[i], &len);
		if (ret < 1) {
			ret = -EINVAL;
			goto free_content;
		}
	}

	cfg_line = kzalloc(sizeof(*cfg_line), GFP_KERNEL);
	if (!cfg_line) {
		ret = -ENOMEM;
		goto free_content;
	}
	INIT_LIST_HEAD(&cfg_line->list);
	cfg_line->addr = object->start_address + instance * object->size;
	cfg_line->size = object->size;
	cfg_line->content = content;
	list_add_tail(&cfg_line->list, cfg_list);

	return 0;

free_content:
	kfree(content);
	return ret;
}

static int mxt_cfg_proc_data(struct mxt_data *data, char **config)
{
	struct i2c_client *client = data->client;
	struct device *dev = &client->dev;
	char *line;
	int ret = 0;
	struct list_head cfg_lines;
	struct mxt_cfg_file_line *cfg_line, *cfg_line_tmp;

	INIT_LIST_HEAD(&cfg_lines);

	while ((line = strsep(config, "\n"))) {
		ret = mxt_cfg_proc_line(data, line, &cfg_lines);
		if (ret < 0)
			goto free_objects;
	}

	list_for_each_entry(cfg_line, &cfg_lines, list) {
		dev_dbg(dev, "Addr = %u Size = %u\n",
			cfg_line->addr, cfg_line->size);
		print_hex_dump(KERN_DEBUG, "atmel_mxt_ts: ", DUMP_PREFIX_OFFSET,
			       16, 1, cfg_line->content, cfg_line->size, false);

		ret = mxt_write_reg(client, cfg_line->addr, cfg_line->size,
				    cfg_line->content);
		if (ret)
			break;
	}

free_objects:
	list_for_each_entry_safe(cfg_line, cfg_line_tmp, &cfg_lines, list) {
		list_del(&cfg_line->list);
		kfree(cfg_line->content);
		kfree(cfg_line);
	}
	return ret;
}

static int mxt_load_config(struct mxt_data *data, const char *fn)
{
	struct i2c_client *client = data->client;
	struct device *dev = &client->dev;
	const struct firmware *fw = NULL;
	int ret, ret2;
	char *cfg_copy = NULL;
	char *running;

	ret = request_firmware(&fw, fn, dev);
	if (ret) {
		dev_err(dev, "Unable to open config file %s\n", fn);
		return ret;
	}

	dev_info(dev, "Using config file %s (size = %zu)\n", fn, fw->size);

	/* Make a mutable, '\0'-terminated copy of the config file */
	cfg_copy = kmalloc(fw->size + 1, GFP_KERNEL);
	if (!cfg_copy) {
		ret = -ENOMEM;
		goto err_alloc_copy;
	}
	memcpy(cfg_copy, fw->data, fw->size);
	cfg_copy[fw->size] = '\0';

	/* Verify config file header (after which running points to data) */
	running = cfg_copy;
	ret = mxt_cfg_verify_hdr(data, &running);
	if (ret) {
		dev_err(dev, "Error verifying config header (%d)\n", ret);
		goto free_cfg_copy;
	}

	disable_irq(data->irq);
	if (data->input_dev) {
		input_unregister_device(data->input_dev);
		data->input_dev = NULL;
	}

	/* Write configuration */
	ret = mxt_cfg_proc_data(data, &running);
	if (ret) {
		dev_err(dev, "Error writing config file (%d)\n", ret);
		goto register_input_dev;
	}
	/* Backup nvram */
	ret = mxt_write_object(data, MXT_GEN_COMMAND_T6, 0,
			       MXT_COMMAND_BACKUPNV,
			       MXT_BACKUP_VALUE);
	if (ret) {
		dev_err(dev, "Error backup to nvram (%d)\n", ret);
		goto register_input_dev;
	}
	msleep(MXT_BACKUP_TIME);

	/* Reset device */
	ret = mxt_write_object(data, MXT_GEN_COMMAND_T6, 0,
			       MXT_COMMAND_RESET, 1);
	if (ret) {
		dev_err(dev, "Error resetting device (%d)\n", ret);
		goto register_input_dev;
	}
	msleep(MXT_RESET_TIME);

register_input_dev:
	ret2 = mxt_input_dev_create(data);
	if (ret2) {
		dev_err(dev, "Error creating input_dev (%d)\n", ret2);
		ret = ret2;
	}
	enable_irq(data->irq);

	/* Clear message buffer */
	ret2 = mxt_handle_messages(data, true);
	if (ret2) {
		dev_err(dev, "Error clearing msg buffer (%d)\n", ret2);
		ret = ret2;
	}
free_cfg_copy:
	kfree(cfg_copy);
err_alloc_copy:
	release_firmware(fw);
	return ret;
}

static int mxt_load_fw(struct device *dev, const char *fn)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	const struct firmware *fw = NULL;
	unsigned int frame_size;
	unsigned int pos = 0;
	int ret;

	ret = request_firmware(&fw, fn, dev);
	if (ret) {
		dev_err(dev, "Unable to open firmware %s\n", fn);
		return ret;
	}

	if (!mxt_in_bootloader(data)) {
		ret = mxt_enter_bl(data);
		if (ret) {
			dev_err(dev, "Failed to reset to bootloader.\n");
			goto out;
		}
	}

	ret = mxt_check_bootloader(data, MXT_WAITING_BOOTLOAD_CMD);
	if (ret)
		goto out;

	/* Unlock bootloader */
	ret = mxt_unlock_bootloader(client);
	if (ret)
		goto out;

	while (pos < fw->size) {
		ret = mxt_check_bootloader(data, MXT_WAITING_FRAME_DATA);
		if (ret)
			goto out;

		frame_size = (fw->data[pos] << 8) + fw->data[pos + 1];

		/* We should add 2 at frame size as the the firmware data is not
		 * included the CRC bytes.
		 */
		frame_size += 2;

		/* Write one frame to device */
		ret = mxt_fw_write(client, fw->data + pos, frame_size);
		if (ret)
			goto out;

		ret = mxt_check_bootloader(data, MXT_FRAME_CRC_PASS);
		if (ret)
			goto out;

		pos += frame_size;
		dev_dbg(dev, "Updated %d bytes / %zd bytes\n", pos, fw->size);
	}

	/* Device exits bl mode to app mode only if successful */
	mxt_exit_bl(data);
out:
	release_firmware(fw);
	return ret;
}

/*
 * Helper function for performing a T6 diagnostic command
 */
static int mxt_T6_diag_cmd(struct mxt_data *data, struct mxt_object *T6,
			   u8 cmd)
{
	int ret;
	u16 addr = T6->start_address + MXT_COMMAND_DIAGNOSTIC;

	ret = mxt_write_reg(data->client, addr, 1, &cmd);
	if (ret)
		return ret;

	/*
	 * Poll T6.diag until it returns 0x00, which indicates command has
	 * completed.
	 */
	while (cmd != 0) {
		ret = mxt_read_reg(data->client, addr, 1, &cmd);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * SysFS Helper function for reading DELTAS and REFERENCE values for T37 object
 *
 * For both modes, a T37_buf is allocated to stores matrix_xsize * matrix_ysize
 * 2-byte (little-endian) values, which are returned to userspace unmodified.
 *
 * It is left to userspace to parse the 2-byte values.
 * - deltas are signed 2's complement 2-byte little-endian values.
 *     s32 delta = (b[0] + (b[1] << 8));
 * - refs are signed 'offset binary' 2-byte little-endian values, with offset
 *   value 0x4000:
 *     s32 ref = (b[0] + (b[1] << 8)) - 0x4000;
 */
static ssize_t mxt_T37_fetch(struct mxt_data *data, u8 mode)
{
	struct mxt_object *T6, *T37;
	u8 *obuf;
	ssize_t ret = 0;
	size_t i;
	size_t T37_buf_size, num_pages;
	size_t pos;

	if (!data || !data->object_table)
		return -ENODEV;

	T6 = mxt_get_object(data, MXT_GEN_COMMAND_T6);
	T37 = mxt_get_object(data, MXT_DEBUG_DIAGNOSTIC_T37);
	if (!T6 || T6->size < 6 || !T37 || T37->size < 3) {
		dev_err(&data->client->dev, "Invalid T6 or T37 object\n");
		return -ENODEV;
	}

	/* Something has gone wrong if T37_buf is already allocated */
	if (data->T37_buf)
		return -EINVAL;

	T37_buf_size = data->info.matrix_xsize * data->info.matrix_ysize *
		       sizeof(__le16);
	data->T37_buf_size = T37_buf_size;
	data->T37_buf = kmalloc(data->T37_buf_size, GFP_KERNEL);
	if (!data->T37_buf)
		return -ENOMEM;

	/* Temporary buffer used to fetch one T37 page */
	obuf = kmalloc(T37->size, GFP_KERNEL);
	if (!obuf)
		return -ENOMEM;

	disable_irq(data->irq);
	num_pages = DIV_ROUND_UP(T37_buf_size, T37->size - 2);
	pos = 0;
	for (i = 0; i < num_pages; i++) {
		u8 cmd;
		size_t chunk_len;

		/* For first page, send mode as cmd, otherwise PageUp */
		cmd = (i == 0) ? mode : MXT_T6_CMD_PAGE_UP;
		ret = mxt_T6_diag_cmd(data, T6, cmd);
		if (ret)
			goto err_free_T37_buf;

		ret = mxt_read_object(data, T37, 0, obuf);
		if (ret)
			goto err_free_T37_buf;

		/* Verify first two bytes are current mode and page # */
		if (obuf[0] != mode) {
			dev_err(&data->client->dev,
				"Unexpected mode (%u != %u)\n", obuf[0], mode);
			ret = -EIO;
			goto err_free_T37_buf;
		}

		if (obuf[1] != i) {
			dev_err(&data->client->dev,
				"Unexpected page (%u != %zu)\n", obuf[1], i);
			ret = -EIO;
			goto err_free_T37_buf;
		}

		/*
		 * Copy the data portion of the page, or however many bytes are
		 * left, whichever is less.
		 */
		chunk_len = min((size_t)T37->size - 2, T37_buf_size - pos);
		memcpy(&data->T37_buf[pos], &obuf[2], chunk_len);
		pos += chunk_len;
	}

	goto out;

err_free_T37_buf:
	kfree(data->T37_buf);
	data->T37_buf = NULL;
	data->T37_buf_size = 0;
out:
	kfree(obuf);
	enable_irq(data->irq);
	return ret ?: 0;
}

/*
 **************************************************************
 * sysfs interface
 **************************************************************
*/
static ssize_t mxt_backupnv_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	int ret;

	/* Backup non-volatile memory */
	ret = mxt_write_object(data, MXT_GEN_COMMAND_T6, 0,
			       MXT_COMMAND_BACKUPNV, MXT_BACKUP_VALUE);
	if (ret)
		return ret;
	msleep(MXT_BACKUP_TIME);

	return count;
}

static ssize_t mxt_calibrate_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	int ret;

	disable_irq(data->irq);

	/* Perform touch surface recalibration */
	ret = mxt_write_object(data, MXT_GEN_COMMAND_T6, 0,
			       MXT_COMMAND_CALIBRATE, 1);
	if (ret)
		return ret;
	msleep(MXT_CAL_TIME);

	enable_irq(data->irq);

	return count;
}

static ssize_t mxt_config_csum_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	return scnprintf(buf, PAGE_SIZE, "%06x\n", data->config_csum);
}

static ssize_t mxt_fw_file_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	return scnprintf(buf, PAGE_SIZE, "%s\n", data->fw_file);
}

static int mxt_update_file_name(struct device *dev, char** file_name,
				const char *buf, size_t count)
{
	char *file_name_tmp;

	/* Simple sanity check */
	if (count > 64) {
		dev_warn(dev, "File name too long\n");
		return -EINVAL;
	}

	file_name_tmp = krealloc(*file_name, count + 1, GFP_KERNEL);
	if (!file_name_tmp) {
		dev_warn(dev, "no memory\n");
		return -ENOMEM;
	}

	*file_name = file_name_tmp;
	memcpy(*file_name, buf, count);

	/* Echo into the sysfs entry may append newline at the end of buf */
	if (buf[count - 1] == '\n')
		(*file_name)[count - 1] = '\0';
	else
		(*file_name)[count] = '\0';

	return 0;
}

static ssize_t mxt_fw_file_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	int ret;

	ret = mxt_update_file_name(dev, &data->fw_file, buf, count);
	if (ret)
		return ret;

	return count;
}

static ssize_t mxt_config_file_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	return scnprintf(buf, PAGE_SIZE, "%s\n", data->config_file);
}

static ssize_t mxt_config_file_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	int ret;

	ret = mxt_update_file_name(dev, &data->config_file, buf, count);
	return ret ? ret : count;
}

static ssize_t mxt_fw_version_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	struct mxt_info *info = &data->info;
	return scnprintf(buf, PAGE_SIZE, "%d.%d.%d\n",
			 info->version >> 4, info->version & 0xf, info->build);
}

/* Hardware Version is <FamilyID>.<VariantID> */
static ssize_t mxt_hw_version_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	struct mxt_info *info = &data->info;
	return scnprintf(buf, PAGE_SIZE, "%d.%d\n",
			 info->family_id, info->variant_id);
}

static ssize_t mxt_info_csum_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	return scnprintf(buf, PAGE_SIZE, "%06x\n", data->info_csum);
}

/* Matrix Size is <MatrixSizeX> <MatrixSizeY> */
static ssize_t mxt_matrix_size_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	struct mxt_info *info = &data->info;
	return scnprintf(buf, PAGE_SIZE, "%u %u\n",
			 info->matrix_xsize, info->matrix_ysize);
}

static ssize_t mxt_object_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	int ret;
	u32 param;
	u8 type, instance, offset, val;

	ret = kstrtou32(buf, 16, &param);
	if (ret < 0)
		return -EINVAL;

	/*
	 * Byte Write Command is encoded in 32-bit word: TTIIOOVV:
	 * <Type> <Instance> <Offset> <Value>
	 */
	type = (param & 0xff000000) >> 24;
	instance = (param & 0x00ff0000) >> 16;
	offset = (param & 0x0000ff00) >> 8;
	val = param & 0x000000ff;

	ret = mxt_write_object(data, type, instance, offset, val);
	if (ret)
		return ret;

	return count;
}

static ssize_t mxt_update_config_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	int error;

	error = mxt_load_config(data, data->config_file);
	if (error)
		dev_err(dev, "The config update failed (%d)\n", error);
	else
		dev_dbg(dev, "The config update succeeded\n");

	return error ?: count;
}

static ssize_t mxt_update_fw_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	int error;

	error = mxt_load_fw(dev, data->fw_file);
	if (error) {
		dev_err(dev, "The firmware update failed(%d)\n", error);
		count = error;
	} else {
		dev_dbg(dev, "The firmware update succeeded\n");
	}
	return count;
}

static ssize_t mxt_suspend_acq_interval_ms_show(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	u8 interval_reg = data->suspend_acq_interval;
	u8 interval_ms = (interval_reg == 255) ? 0 : interval_reg;
	return scnprintf(buf, PAGE_SIZE, "%u\n", interval_ms);
}

static ssize_t mxt_suspend_acq_interval_ms_store(struct device *dev,
						 struct device_attribute *attr,
						 const char *buf, size_t count)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	int ret;
	u32 param;

	ret = kstrtou32(buf, 10, &param);
	if (ret < 0)
		return -EINVAL;

	/* 0 ms inteval means "free run" */
	if (param == 0)
		param = 255;
	/* 254 ms is the largest interval */
	else if (param > 254)
		param = 254;

	data->suspend_acq_interval = param;
	return count;
}

static DEVICE_ATTR(backupnv, S_IWUSR, NULL, mxt_backupnv_store);
static DEVICE_ATTR(calibrate, S_IWUSR, NULL, mxt_calibrate_store);
static DEVICE_ATTR(config_csum, S_IRUGO, mxt_config_csum_show, NULL);
static DEVICE_ATTR(fw_file, S_IRUGO | S_IWUSR, mxt_fw_file_show,
		   mxt_fw_file_store);
static DEVICE_ATTR(config_file, S_IRUGO | S_IWUSR, mxt_config_file_show,
		   mxt_config_file_store);
static DEVICE_ATTR(fw_version, S_IRUGO, mxt_fw_version_show, NULL);
static DEVICE_ATTR(hw_version, S_IRUGO, mxt_hw_version_show, NULL);
static DEVICE_ATTR(info_csum, S_IRUGO, mxt_info_csum_show, NULL);
static DEVICE_ATTR(matrix_size, S_IRUGO, mxt_matrix_size_show, NULL);
static DEVICE_ATTR(object, S_IWUSR, NULL, mxt_object_store);
static DEVICE_ATTR(update_config, S_IWUSR, NULL, mxt_update_config_store);
static DEVICE_ATTR(update_fw, S_IWUSR, NULL, mxt_update_fw_store);
static DEVICE_ATTR(suspend_acq_interval_ms, S_IRUGO | S_IWUSR,
		   mxt_suspend_acq_interval_ms_show,
		   mxt_suspend_acq_interval_ms_store);

static struct attribute *mxt_attrs[] = {
	&dev_attr_backupnv.attr,
	&dev_attr_calibrate.attr,
	&dev_attr_config_csum.attr,
	&dev_attr_fw_file.attr,
	&dev_attr_config_file.attr,
	&dev_attr_fw_version.attr,
	&dev_attr_hw_version.attr,
	&dev_attr_info_csum.attr,
	&dev_attr_matrix_size.attr,
	&dev_attr_object.attr,
	&dev_attr_update_config.attr,
	&dev_attr_update_fw.attr,
	NULL
};

static const struct attribute_group mxt_attr_group = {
	.attrs = mxt_attrs,
};

static struct attribute *mxt_power_attrs[] = {
	&dev_attr_suspend_acq_interval_ms.attr,
	NULL
};

static const struct attribute_group mxt_power_attr_group = {
	.name = power_group_name,
	.attrs = mxt_power_attrs,
};

/*
 **************************************************************
 * debugfs helper functions
 **************************************************************
*/

/*
 * Print the formatted string into the end of string |*str| which has size
 * |*str_size|. Extra space will be allocated to hold the formatted string
 * and |*str_size| will be updated accordingly.
 */
static int mxt_asprintf(char **str, size_t *str_size, const char *fmt, ...)
{
	unsigned int len;
	va_list ap, aq;
	int ret;
	char *str_tmp;

	va_start(ap, fmt);
	va_copy(aq, ap);
	len = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);

	str_tmp = krealloc(*str, *str_size + len + 1, GFP_KERNEL);
	if (str_tmp == NULL)
		return -ENOMEM;

	*str = str_tmp;

	ret = vsnprintf(*str + *str_size, len + 1, fmt, ap);
	va_end(ap);

	if (ret != len)
		return -EINVAL;

	*str_size += len;

	return 0;
}

static int mxt_object_fetch(struct mxt_data *data)
{
	size_t count = 0;
	size_t i, j, k;
	int ret = 0;
	char *str = NULL;
	u8 *obuf = NULL;
	u8 *obuf_tmp = NULL;

	if (data->object_str)
		return -EINVAL;

	for (i = 0; i < data->info.object_num; i++) {
		struct mxt_object *object = &data->object_table[i];

		if (!mxt_object_readable(object->type))
			continue;

		ret = mxt_asprintf(&str, &count, "\nType: %u\n",
				   object->type);
		if (ret)
			goto err;

		obuf_tmp = krealloc(obuf, object->size, GFP_KERNEL);
		if (!obuf_tmp) {
			ret = -ENOMEM;
			goto err;
		}

		obuf = obuf_tmp;

		for (j = 0; j < object->instances; j++) {
			if (object->instances > 1) {
				ret = mxt_asprintf(&str, &count,
						   "Instance: %zu\n", j);
				if (ret)
					goto err;
			}

			ret = mxt_read_object(data, object, j, obuf);
			if (ret)
				goto err;

			for (k = 0; k < object->size; k++) {
				ret = mxt_asprintf(&str, &count,
						   "\t[%2zu]: %02x (%d)\n",
						   k, obuf[k], obuf[k]);
				if (ret)
					goto err;
			}
		}
	}

	goto done;

err:
	kfree(str);
	str = NULL;
	count = 0;
done:
	data->object_str = str;
	data->object_str_size = count;
	kfree(obuf);
	return ret;
}

/*
 **************************************************************
 * debugfs interface
 **************************************************************
*/
static int mxt_debugfs_T37_open(struct inode *inode, struct file *file)
{
	struct mxt_data *mxt = inode->i_private;
	int ret;
	u8 cmd;

	if (file->f_dentry == mxt->dentry_deltas)
		cmd = MXT_T6_CMD_DELTAS;
	else if (file->f_dentry == mxt->dentry_refs)
		cmd = MXT_T6_CMD_REFS;
	else
		return -EINVAL;

	/* Only allow one T37 debugfs file to be opened at a time */
	ret = mutex_lock_interruptible(&mxt->T37_buf_mutex);
	if (ret)
		return ret;

	if (!i2c_use_client(mxt->client)) {
		ret = -ENODEV;
		goto err_unlock;
	}

	/* Fetch all T37 pages into mxt->T37_buf */
	ret = mxt_T37_fetch(mxt, cmd);
	if (ret)
		goto err_release;

	file->private_data = mxt;

	return 0;

err_release:
	i2c_release_client(mxt->client);
err_unlock:
	mutex_unlock(&mxt->T37_buf_mutex);
	return ret;
}

static int mxt_debugfs_T37_release(struct inode *inode, struct file *file)
{
	struct mxt_data *mxt = file->private_data;

	file->private_data = NULL;

	kfree(mxt->T37_buf);
	mxt->T37_buf = NULL;
	mxt->T37_buf_size = 0;

	i2c_release_client(mxt->client);
	mutex_unlock(&mxt->T37_buf_mutex);

	return 0;
}


/* Return some bytes from the buffered T37 object, starting from *ppos */
static ssize_t mxt_debugfs_T37_read(struct file *file, char __user *buffer,
				    size_t count, loff_t *ppos)
{
	struct mxt_data *mxt = file->private_data;

	if (!mxt->T37_buf)
		return -ENODEV;

	if (*ppos >= mxt->T37_buf_size)
		return 0;

	if (count + *ppos > mxt->T37_buf_size)
		count = mxt->T37_buf_size - *ppos;

	if (copy_to_user(buffer, &mxt->T37_buf[*ppos], count))
		return -EFAULT;

	*ppos += count;

	return count;
}

static const struct file_operations mxt_debugfs_T37_fops = {
	.owner = THIS_MODULE,
	.open = mxt_debugfs_T37_open,
	.release = mxt_debugfs_T37_release,
	.read = mxt_debugfs_T37_read
};

static int mxt_debugfs_object_open(struct inode *inode, struct file *file)
{
	struct mxt_data *mxt = inode->i_private;
	int ret;

	/* Only allow one object debugfs file to be opened at a time */
	ret = mutex_lock_interruptible(&mxt->object_str_mutex);
	if (ret)
		return ret;

	if (!i2c_use_client(mxt->client)) {
		ret = -ENODEV;
		goto err_object_unlock;
	}

	ret = mxt_object_fetch(mxt);
	if (ret)
		goto err_object_i2c_release;
	file->private_data = mxt;

	return 0;

err_object_i2c_release:
	i2c_release_client(mxt->client);
err_object_unlock:
	mutex_unlock(&mxt->object_str_mutex);
	return ret;
}

static int mxt_debugfs_object_release(struct inode *inode, struct file *file)
{
	struct mxt_data *mxt = file->private_data;
	file->private_data = NULL;

	kfree(mxt->object_str);
	mxt->object_str = NULL;
	mxt->object_str_size = 0;

	i2c_release_client(mxt->client);
	mutex_unlock(&mxt->object_str_mutex);

	return 0;
}

static ssize_t mxt_debugfs_object_read(struct file *file, char __user* buffer,
				   size_t count, loff_t *ppos)
{
	struct mxt_data *mxt = file->private_data;
	if (!mxt->object_str)
		return -ENODEV;

	if (*ppos >= mxt->object_str_size)
		return 0;

	if (count + *ppos > mxt->object_str_size)
		count = mxt->object_str_size - *ppos;

	if (copy_to_user(buffer, &mxt->object_str[*ppos], count))
		return -EFAULT;

	*ppos += count;

	return count;
}

static const struct file_operations mxt_debugfs_object_fops = {
	.owner = THIS_MODULE,
	.open = mxt_debugfs_object_open,
	.release = mxt_debugfs_object_release,
	.read = mxt_debugfs_object_read,
};

static int mxt_debugfs_init(struct mxt_data *mxt)
{
	struct device *dev = &mxt->client->dev;

	if (!mxt_debugfs_root)
		return -ENODEV;

	mxt->dentry_dev = debugfs_create_dir(kobject_name(&dev->kobj),
					     mxt_debugfs_root);

	if (!mxt->dentry_dev)
		return -ENODEV;

	mutex_init(&mxt->T37_buf_mutex);

	mxt->dentry_deltas = debugfs_create_file("deltas", S_IRUSR,
						 mxt->dentry_dev, mxt,
						 &mxt_debugfs_T37_fops);
	mxt->dentry_refs = debugfs_create_file("refs", S_IRUSR,
					       mxt->dentry_dev, mxt,
					       &mxt_debugfs_T37_fops);
	mutex_init(&mxt->object_str_mutex);

	mxt->dentry_object = debugfs_create_file("object", S_IRUGO,
						 mxt->dentry_dev, mxt,
						 &mxt_debugfs_object_fops);
	return 0;
}

static void mxt_debugfs_remove(struct mxt_data *mxt)
{
	if (mxt->dentry_dev) {
		debugfs_remove_recursive(mxt->dentry_dev);
		mutex_destroy(&mxt->object_str_mutex);
		kfree(mxt->object_str);
		mutex_destroy(&mxt->T37_buf_mutex);
		kfree(mxt->T37_buf);
	}
}

static int mxt_save_regs(struct mxt_data *data, u8 type, u8 instance,
			 u8 offset, u8 *val, u16 size)
{
	struct mxt_object *object;
	u16 addr;
	int ret;

	object = mxt_get_object(data, type);
	if (!object)
		return -EINVAL;

	addr = object->start_address + instance * object->size + offset;
	ret = mxt_read_reg(data->client, addr, size, val);
	if (ret)
		return -EINVAL;

	return 0;
}

static int mxt_set_regs(struct mxt_data *data, u8 type, u8 instance,
			u8 offset, const u8 *val, u16 size)
{
	struct mxt_object *object;
	u16 addr;
	int ret;

	object = mxt_get_object(data, type);
	if (!object)
		return -EINVAL;

	addr = object->start_address + instance * object->size + offset;
	ret = mxt_write_reg(data->client, addr, size, val);
	if (ret)
		return -EINVAL;

	return 0;
}

static void mxt_start(struct mxt_data *data)
{
	/* Enable touch reporting */
	mxt_write_object(data, MXT_TOUCH_MULTI_T9, 0, MXT_TOUCH_CTRL,
			 MXT_TOUCH_CTRL_OPERATIONAL);
}

static void mxt_stop(struct mxt_data *data)
{
	/* Disable touch reporting */
	mxt_write_object(data, MXT_TOUCH_MULTI_T9, 0, MXT_TOUCH_CTRL,
			 MXT_TOUCH_CTRL_SCANNING);
}

static int mxt_input_open(struct input_dev *dev)
{
	struct mxt_data *data = input_get_drvdata(dev);

	mxt_start(data);

	return 0;
}

static void mxt_input_close(struct input_dev *dev)
{
	struct mxt_data *data = input_get_drvdata(dev);

	mxt_stop(data);
}

#if defined(CONFIG_ACPI_BUTTON)
static int mxt_lid_notify(struct notifier_block *nb, unsigned long val,
			   void *unused)
{
	struct mxt_data *data = container_of(nb, struct mxt_data, lid_notifier);

	if (mxt_in_bootloader(data))
		return NOTIFY_OK;

	if (val == 0)
		mxt_stop(data);
	else
		mxt_start(data);

	return NOTIFY_OK;
}
#endif

static int mxt_input_dev_create(struct mxt_data *data)
{
	struct input_dev *input_dev;
	int error;
	int max_area_channels;
	int max_touch_major;

	/* Don't need to register input_dev in bl mode */
	if (mxt_in_bootloader(data))
		return 0;

	error = mxt_calc_resolution(data);
	if (error)
		return error;

	/* Clear the existing one if it exists */
	if (data->input_dev) {
		input_unregister_device(data->input_dev);
		data->input_dev = NULL;
	}

	data->input_dev = input_dev = input_allocate_device();
	if (!input_dev)
		return -ENOMEM;

	input_dev->name = (data->is_tp) ? "Atmel maXTouch Touchpad" :
					  "Atmel maXTouch Touchscreen";
	input_dev->phys = data->client->adapter->name;
	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = &data->client->dev;
	input_dev->open = mxt_input_open;
	input_dev->close = mxt_input_close;

	__set_bit(EV_ABS, input_dev->evbit);
	__set_bit(EV_KEY, input_dev->evbit);
	__set_bit(BTN_TOUCH, input_dev->keybit);

	if (data->is_tp) {
		__set_bit(INPUT_PROP_POINTER, input_dev->propbit);
		__set_bit(INPUT_PROP_BUTTONPAD, input_dev->propbit);

		__set_bit(BTN_LEFT, input_dev->keybit);
		__set_bit(BTN_TOOL_FINGER, input_dev->keybit);
		__set_bit(BTN_TOOL_DOUBLETAP, input_dev->keybit);
		__set_bit(BTN_TOOL_TRIPLETAP, input_dev->keybit);
		__set_bit(BTN_TOOL_QUADTAP, input_dev->keybit);
		__set_bit(BTN_TOOL_QUINTTAP, input_dev->keybit);
	}

	/* For single touch */
	input_set_abs_params(input_dev, ABS_X,
			     0, data->max_x, 0, 0);
	input_set_abs_params(input_dev, ABS_Y,
			     0, data->max_y, 0, 0);
	input_set_abs_params(input_dev, ABS_PRESSURE,
			     0, 255, 0, 0);
	input_abs_set_res(input_dev, ABS_X, MXT_PIXELS_PER_MM);
	input_abs_set_res(input_dev, ABS_Y, MXT_PIXELS_PER_MM);

	/* For multi touch */
	error = input_mt_init_slots(input_dev, MXT_MAX_FINGER);
	if (error)
		goto err_free_device;

	max_area_channels = min(255U, data->max_area_channels);
	max_touch_major = get_touch_major_pixels(data, max_area_channels);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR,
			     0, max_touch_major, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_X,
			     0, data->max_x, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y,
			     0, data->max_y, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE,
			     0, 255, 0, 0);
	input_abs_set_res(input_dev, ABS_MT_POSITION_X, MXT_PIXELS_PER_MM);
	input_abs_set_res(input_dev, ABS_MT_POSITION_Y, MXT_PIXELS_PER_MM);

	input_set_drvdata(input_dev, data);

	error = input_register_device(input_dev);
	if (error)
		goto err_free_device;

	return 0;

err_free_device:
	input_free_device(data->input_dev);
	data->input_dev = NULL;
	return error;
}

static int mxt_initialize(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	struct device *dev = &client->dev;
	struct mxt_info *info = &data->info;
	int error;

	/* Read 7-byte info block starting at address 0 */
	error = mxt_read_reg(client, MXT_INFO, sizeof(*info), info);
	if (error)
		return error;

	/* Get object table information */
	error = mxt_get_object_table(data);
	if (error)
		return error;

	/* Apply config from platform data */
	error = mxt_handle_pdata(data);
	if (error)
		return error;

	/* Soft reset */
	error = mxt_write_object(data, MXT_GEN_COMMAND_T6, 0,
				 MXT_COMMAND_RESET, 1);
	if (error)
		return error;
	msleep(MXT_RESET_TIME);

	dev_info(dev, "Family ID: %d Variant ID: %d Major.Minor.Build: %d.%d.%d\n",
		 info->family_id, info->variant_id, info->version >> 4,
		 info->version & 0xf, info->build);

	dev_info(dev, "Matrix X Size: %d Matrix Y Size: %d Object Num: %d\n",
		 info->matrix_xsize, info->matrix_ysize, info->object_num);

	return 0;
}


static void mxt_initialize_async(void *closure, async_cookie_t cookie)
{
	struct mxt_data *data = closure;
	struct i2c_client *client = data->client;
	unsigned long irqflags;
	int error;

	if (!mxt_in_bootloader(data)) {
		error = mxt_initialize(data);
		if (error)
			goto error_free_object;
	} else {
		dev_info(&client->dev, "device came up in bootloader mode.\n");
	}

	error = mxt_input_dev_create(data);
	if (error)
		goto error_free_object;

	/* Default to falling edge if no platform data provided */
	irqflags = data->pdata ? data->pdata->irqflags : IRQF_TRIGGER_FALLING;
	error = request_threaded_irq(client->irq,
				     NULL,
				     mxt_interrupt,
				     irqflags,
				     client->name,
				     data);
	if (error) {
		dev_err(&client->dev, "Failed to register interrupt\n");
		goto error_unregister_device;
	}

	if (!mxt_in_bootloader(data)) {
		error = mxt_handle_messages(data, true);
		if (error)
			goto error_free_irq;
	}

	/* Force the device to report back status so we can cache the device
	 * config checksum
	 */
	error = mxt_write_object(data, MXT_GEN_COMMAND_T6, 0,
				 MXT_COMMAND_REPORTALL, 1);
	if (error)
		dev_warn(&client->dev, "error making device report status.\n");

	error = sysfs_create_group(&client->dev.kobj, &mxt_attr_group);
	if (error)
		dev_warn(&client->dev, "error creating sysfs entries.\n");

	error = sysfs_merge_group(&client->dev.kobj, &mxt_power_attr_group);
	if (error)
		dev_warn(&client->dev, "error merging power sysfs entries.\n");

	error = mxt_debugfs_init(data);
	if (error)
		dev_warn(&client->dev, "error creating debugfs entries.\n");

	return;

error_free_irq:
	free_irq(client->irq, data);
error_unregister_device:
	input_unregister_device(data->input_dev);
error_free_object:
	kfree(data->object_table);
	kfree(data->fw_file);
	kfree(data->config_file);
	kfree(data);
}

static int __devinit mxt_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	const struct mxt_platform_data *pdata = client->dev.platform_data;
	struct mxt_data *data;
	int error;

	data = kzalloc(sizeof(struct mxt_data), GFP_KERNEL);
	if (!data) {
		dev_err(&client->dev, "Failed to allocate memory\n");
		return -ENOMEM;
	}

	data->is_tp = !strcmp(id->name, "atmel_mxt_tp");

	data->client = client;
	i2c_set_clientdata(client, data);

	data->pdata = pdata;
	data->irq = client->irq;

	data->suspend_acq_interval = MXT_SUSPEND_ACQINT_VALUE;

	error = mxt_update_file_name(&client->dev, &data->fw_file, MXT_FW_NAME,
				     strlen(MXT_FW_NAME));
	if (error)
		goto err_free_object;

	error = mxt_update_file_name(&client->dev, &data->config_file,
				     MXT_CONFIG_NAME, strlen(MXT_CONFIG_NAME));
	if (error)
		goto err_free_object;

	init_completion(&data->bl_completion);
	init_completion(&data->auto_cal_completion);

	device_set_wakeup_enable(&client->dev, false);

	async_schedule(mxt_initialize_async, data);

#if defined(CONFIG_ACPI_BUTTON)
	data->lid_notifier.notifier_call = mxt_lid_notify;
	if (acpi_lid_notifier_register(&data->lid_notifier)) {
		pr_info("lid notifier registration failed\n");
		data->lid_notifier.notifier_call = NULL;
	}
#endif

	return 0;

err_free_object:
	kfree(data->object_table);
	kfree(data->fw_file);
	kfree(data->config_file);
	kfree(data);
	return error;
}

static int __devexit mxt_remove(struct i2c_client *client)
{
	struct mxt_data *data = i2c_get_clientdata(client);

	mxt_debugfs_remove(data);
	sysfs_unmerge_group(&client->dev.kobj, &mxt_power_attr_group);
	sysfs_remove_group(&client->dev.kobj, &mxt_attr_group);
	free_irq(data->irq, data);
	if (data->input_dev)
		input_unregister_device(data->input_dev);
#if defined(CONFIG_ACPI_BUTTON)
	if (data->lid_notifier.notifier_call)
		acpi_lid_notifier_unregister(&data->lid_notifier);
#endif
	kfree(data->object_table);
	kfree(data->fw_file);
	kfree(data->config_file);
	kfree(data);

	return 0;
}

#ifdef CONFIG_PM_SLEEP

static void mxt_suspend_enable_T9(struct mxt_data *data)
{
	struct device *dev = &data->client->dev;
	u8 T9_ctrl = MXT_TOUCH_CTRL_ENABLE | MXT_TOUCH_CTRL_RPTEN;
	int ret;
	unsigned long timeout = msecs_to_jiffies(350);
	bool need_enable = false;
	bool need_report = false;

	dev_dbg(dev, "Current T9_Ctrl is %x\n", data->T9_ctrl);

	need_enable = !(data->T9_ctrl & MXT_TOUCH_CTRL_ENABLE);
	need_report = !(data->T9_ctrl & MXT_TOUCH_CTRL_RPTEN);

	/* If already enabled and reporting, do nothing */
	if (!need_enable && !need_report)
		return;

	/* If the ENABLE bit is toggled, there will be auto-calibration msg.
	 * We will have to clear this msg before going into suspend otherwise
	 * it will wake up the device immediately
	 */
	if (need_enable)
		INIT_COMPLETION(data->auto_cal_completion);

	/* Enable T9 object (ENABLE and REPORT) */
	ret = mxt_set_regs(data, MXT_TOUCH_MULTI_T9, 0, 0,
			   &T9_ctrl, 1);
	if (ret) {
		dev_err(dev, "Set T9 ctrl config failed, %d\n", ret);
		return;
	}

	if (need_enable) {
		ret = wait_for_completion_interruptible_timeout(
			&data->auto_cal_completion, timeout);
		if (ret <= 0)
			dev_err(dev, "Wait for auto cal completion failed.\n");
	}
}

static int mxt_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct mxt_data *data = i2c_get_clientdata(client);
	struct input_dev *input_dev = data->input_dev;
	u8 T7_config_idle[3] = {data->suspend_acq_interval,
				data->suspend_acq_interval,
				0};
	u8 T7_config_deepsleep[3] = {0x00, 0x00, 0};
	u8 *power_config;
	int ret;

	if (mxt_in_bootloader(data))
		return 0;

	mutex_lock(&input_dev->mutex);

	/* Save 3 bytes T7 Power config */
	ret = mxt_save_regs(data, MXT_GEN_POWER_T7, 0, 0,
			    data->T7_config, 3);
	if (ret)
		dev_err(dev, "Save T7 Power config failed, %d\n", ret);
	data->T7_config_valid = (ret == 0);

	/* Set T7 to idle mode if we allow wakeup from touch, otherwise
	 * put it into deepsleep mode.
	 */
	power_config = device_may_wakeup(dev) ? T7_config_idle
					      : T7_config_deepsleep;

	ret = mxt_set_regs(data, MXT_GEN_POWER_T7, 0, 0,
			   power_config, 3);
	if (ret)
		dev_err(dev, "Set T7 Power config failed, %d\n", ret);

	/* Save 1 byte T9 Ctrl config */
	ret = mxt_save_regs(data, MXT_TOUCH_MULTI_T9, 0, 0,
			    &data->T9_ctrl, 1);
	if (ret)
		dev_err(dev, "Save T9 ctrl config failed, %d\n", ret);
	data->T9_ctrl_valid = (ret == 0);

#if defined(CONFIG_ACPI_BUTTON)
	ret = acpi_lid_open();
	if (ret == 0) {
		/* lid is closed. set T9_ctrl to operational on resume */
		data->T9_ctrl = MXT_TOUCH_CTRL_OPERATIONAL;
		data->T9_ctrl_valid = true;
	}
#endif

	/*
	 *  For tpads, save T42 and T19 ctrl registers if may wakeup,
	 *  enable large object suppression, and disable button wake.
	 *  This will prevent a lid close from acting as a wake source.
	 */
	if (data->is_tp && device_may_wakeup(dev)) {
		u8 T42_sleep = 0x01;
		u8 T19_sleep = 0x00;

		ret = mxt_save_regs(data, MXT_PROCI_TOUCHSUPPRESSION_T42, 0, 0,
				    &data->T42_ctrl, 1);
		if (ret)
			dev_err(dev, "Save T42 ctrl config failed, %d\n", ret);
		data->T42_ctrl_valid = (ret == 0);

		ret = mxt_save_regs(data, MXT_SPT_GPIOPWM_T19, 0, 0,
				    &data->T19_ctrl, 1);
		if (ret)
			dev_err(dev, "Save T19 ctrl config failed, %d\n", ret);
		data->T19_ctrl_valid = (ret == 0);


		/* Enable Large Object Suppression */
		ret = mxt_set_regs(data, MXT_PROCI_TOUCHSUPPRESSION_T42, 0, 0,
				   &T42_sleep, 1);
		if (ret)
			dev_err(dev, "Set T42 ctrl failed, %d\n", ret);

		/* Disable Touchpad Button via GPIO */
		ret = mxt_set_regs(data, MXT_SPT_GPIOPWM_T19, 0, 0,
				   &T19_sleep, 1);
		if (ret)
			dev_err(dev, "Set T19 ctrl failed, %d\n", ret);

	} else {
		data->T42_ctrl_valid = data->T19_ctrl_valid = false;
	}

	if (device_may_wakeup(dev)) {
		/* If we allow wakeup from touch, we have to enable T9 so
		 * that IRQ can be generated from touch
		 */

		/* Set proper T9 ENABLE & REPTN bits */
		if (data->T9_ctrl_valid)
			mxt_suspend_enable_T9(data);

		/* Enable wake from IRQ */
		data->irq_wake = (enable_irq_wake(data->irq) == 0);
	} else if (input_dev->users) {
		mxt_stop(data);
	}

	disable_irq(data->irq);

	mutex_unlock(&input_dev->mutex);

	return 0;
}

static int mxt_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct mxt_data *data = i2c_get_clientdata(client);
	struct input_dev *input_dev = data->input_dev;
	int ret;

	if (mxt_in_bootloader(data))
		return 0;

	/* Process any pending message so that CHG line can be de-asserted */
	ret = mxt_handle_messages(data, false);
	if (ret)
		dev_err(dev, "Handling message fails upon resume, %d\n", ret);

	mxt_release_all_fingers(data);

	mutex_lock(&input_dev->mutex);

	/* Restore the T9 Ctrl config to before-suspend value */
	if (data->T9_ctrl_valid) {
		ret = mxt_set_regs(data, MXT_TOUCH_MULTI_T9, 0, 0,
				   &data->T9_ctrl, 1);
		if (ret)
			dev_err(dev, "Set T9 ctrl config failed, %d\n", ret);
	}

	/* Restore the T7 Power config to before-suspend value */
	if (data->T7_config_valid) {
		ret = mxt_set_regs(data, MXT_GEN_POWER_T7, 0, 0,
				   data->T7_config, 3);
		if (ret)
			dev_err(dev, "Set T7 power config failed, %d\n", ret);
	}

	/* Restore the T42 ctrl to before-suspend value */
	if (data->T42_ctrl_valid) {
		ret = mxt_set_regs(data, MXT_PROCI_TOUCHSUPPRESSION_T42, 0, 0,
				   &data->T42_ctrl, 1);
		if (ret)
			dev_err(dev, "Set T42 ctrl failed, %d\n", ret);
	}

	/* Restore the T19 ctrl to before-suspend value */
	if (data->T19_ctrl_valid) {
		ret = mxt_set_regs(data, MXT_SPT_GPIOPWM_T19, 0, 0,
				   &data->T19_ctrl, 1);
		if (ret)
			dev_err(dev, "Set T19 ctrl failed, %d\n", ret);
	}

	if (!device_may_wakeup(dev)) {
		/* Recalibration in case of environment change */
		ret = mxt_write_object(data, MXT_GEN_COMMAND_T6, 0,
				       MXT_COMMAND_CALIBRATE, 1);
		if (ret)
			dev_err(dev, "Resume recalibration failed %d\n", ret);
		msleep(MXT_CAL_TIME);
	}

	mutex_unlock(&input_dev->mutex);

	enable_irq(data->irq);

	if (device_may_wakeup(dev) && data->irq_wake)
		disable_irq_wake(data->irq);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(mxt_pm_ops, mxt_suspend, mxt_resume);

static const struct i2c_device_id mxt_id[] = {
	{ "qt602240_ts", 0 },
	{ "atmel_mxt_ts", 0 },
	{ "atmel_mxt_tp", 0 },
	{ "mXT224", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mxt_id);

static struct i2c_driver mxt_driver = {
	.driver = {
		.name	= "atmel_mxt_ts",
		.owner	= THIS_MODULE,
		.pm	= &mxt_pm_ops,
	},
	.probe		= mxt_probe,
	.remove		= __devexit_p(mxt_remove),
	.id_table	= mxt_id,
};

static int __init mxt_init(void)
{
	/* Create a global debugfs root for all atmel_mxt_ts devices */
	mxt_debugfs_root = debugfs_create_dir(mxt_driver.driver.name, NULL);
	if (mxt_debugfs_root == ERR_PTR(-ENODEV))
		mxt_debugfs_root = NULL;

	return i2c_add_driver(&mxt_driver);
}

static void __exit mxt_exit(void)
{
	if (mxt_debugfs_root)
		debugfs_remove_recursive(mxt_debugfs_root);

	i2c_del_driver(&mxt_driver);
}

module_init(mxt_init);
module_exit(mxt_exit);

/* Module information */
MODULE_AUTHOR("Joonyoung Shim <jy0922.shim@samsung.com>");
MODULE_DESCRIPTION("Atmel maXTouch Touchscreen driver");
MODULE_LICENSE("GPL");
