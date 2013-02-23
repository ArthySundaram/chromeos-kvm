/*
 *  Copyright (C) 2012 Google, Inc
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 * The ChromeOS EC multi function device is used to mux all the requests
 * to the EC device for its multiple features : keyboard controller,
 * battery charging and regulator control, firmware update.
 */

#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/mfd/core.h>
#include <linux/mfd/chromeos_ec.h>
#include <linux/mfd/chromeos_ec_commands.h>
#include <linux/of_platform.h>

int cros_ec_prepare_tx(struct chromeos_ec_device *ec_dev,
		       struct chromeos_ec_msg *msg)
{
	uint8_t *out;
	int csum, i;

	BUG_ON(msg->out_len > EC_HOST_PARAM_SIZE);
	out = ec_dev->dout;
	out[0] = EC_CMD_VERSION0 + msg->version;
	out[1] = msg->cmd;
	out[2] = msg->out_len;
	csum = out[0] + out[1] + out[2];
	for (i = 0; i < msg->out_len; i++)
		csum += out[EC_MSG_TX_HEADER_BYTES + i] = msg->out_buf[i];
	out[EC_MSG_TX_HEADER_BYTES + msg->out_len] = (uint8_t)(csum & 0xff);

	return EC_MSG_TX_PROTO_BYTES + msg->out_len;
}

static int cros_ec_command_sendrecv(struct chromeos_ec_device *ec_dev,
		uint16_t cmd, void *out_buf, int out_len,
		void *in_buf, int in_len)
{
	struct chromeos_ec_msg msg;

	msg.version = cmd >> 8;
	msg.cmd = cmd & 0xff;
	msg.out_buf = out_buf;
	msg.out_len = out_len;
	msg.in_buf = in_buf;
	msg.in_len = in_len;

	return ec_dev->command_xfer(ec_dev, &msg);
}

static int cros_ec_command_recv(struct chromeos_ec_device *ec_dev,
		uint16_t cmd, void *buf, int buf_len)
{
	return cros_ec_command_sendrecv(ec_dev, cmd, NULL, 0, buf, buf_len);
}

static int cros_ec_command_send(struct chromeos_ec_device *ec_dev,
		uint16_t cmd, void *buf, int buf_len)
{
	return cros_ec_command_sendrecv(ec_dev, cmd, buf, buf_len, NULL, 0);
}

struct chromeos_ec_device *__devinit cros_ec_alloc(const char *name)
{
	struct chromeos_ec_device *ec_dev;

	ec_dev = kzalloc(sizeof(*ec_dev), GFP_KERNEL);
	if (ec_dev == NULL) {
		dev_err(ec_dev->dev, "cannot allocate\n");
		return NULL;
	}
	ec_dev->name = name;

	return ec_dev;
}

void cros_ec_free(struct chromeos_ec_device *ec)
{
	kfree(ec);
}

static irqreturn_t ec_irq_thread(int irq, void *data)
{
	struct chromeos_ec_device *ec_dev = data;

	if (device_may_wakeup(ec_dev->dev))
		pm_wakeup_event(ec_dev->dev, 0);

	blocking_notifier_call_chain(&ec_dev->event_notifier, 1, ec_dev);

	return IRQ_HANDLED;
}

static int __devinit check_protocol_version(struct chromeos_ec_device *ec)
{
	int ret;
	struct ec_response_proto_version data;

	ret = ec->command_recv(ec, EC_CMD_PROTO_VERSION, &data, sizeof(data));
	if (ret < 0)
		return ret;
	dev_info(ec->dev, "protocol version: %d\n", data.version);
	if (data.version != EC_PROTO_VERSION)
		return -EPROTONOSUPPORT;

	return 0;
}

static struct mfd_cell cros_devs[] = {
	{
		.name = "mkbp",
		.id = 1,
	},
	{
		.name = "cros_ec-fw",
		.id = 2,
	},
};

int __devinit cros_ec_register(struct chromeos_ec_device *ec_dev)
{
	struct device *dev = ec_dev->dev;
	int err = 0;
	struct device_node *node;
	int id = 0;

	BLOCKING_INIT_NOTIFIER_HEAD(&ec_dev->event_notifier);
	BLOCKING_INIT_NOTIFIER_HEAD(&ec_dev->wake_notifier);

	ec_dev->command_send = cros_ec_command_send;
	ec_dev->command_recv = cros_ec_command_recv;
	ec_dev->command_sendrecv = cros_ec_command_sendrecv;

	if (ec_dev->din_size) {
		ec_dev->din = kmalloc(ec_dev->din_size, GFP_KERNEL);
		if (!ec_dev->din) {
			err = -ENOMEM;
			dev_err(dev, "cannot allocate din\n");
			goto fail_din;
		}
	}
	if (ec_dev->dout_size) {
		ec_dev->dout = kmalloc(ec_dev->dout_size, GFP_KERNEL);
		if (!ec_dev->dout) {
			err = -ENOMEM;
			dev_err(dev, "cannot allocate dout\n");
			goto fail_dout;
		}
	}

	if (!ec_dev->irq) {
		dev_dbg(dev, "no valid IRQ: %d\n", ec_dev->irq);
		goto fail_irq;
	}

	err = request_threaded_irq(ec_dev->irq, NULL, ec_irq_thread,
				   IRQF_TRIGGER_LOW | IRQF_ONESHOT,
				   "chromeos-ec", ec_dev);
	if (err) {
		dev_err(dev, "request irq %d: error %d\n", ec_dev->irq, err);
		goto fail_irq;
	}

	err = check_protocol_version(ec_dev);
	if (err < 0) {
		dev_err(dev, "protocol version check failed: %d\n", err);
		goto fail_proto;
	}

	err = mfd_add_devices(dev, 0, cros_devs,
			      ARRAY_SIZE(cros_devs),
			      NULL, ec_dev->irq);
	if (err)
		goto fail_mfd;

	/* adding sub-devices declared in the device tree */
	for_each_child_of_node(dev->of_node, node) {
		char name[128];
		struct mfd_cell cell = {
			.id = id++,
			.name = name,
		};

		dev_dbg(dev, "adding MFD sub-device %s\n", node->name);
		if (of_modalias_node(node, name, sizeof(name)) < 0) {
			dev_err(dev, "modalias failure on %s\n",
				node->full_name);
			continue;
		}

		err = mfd_add_devices(dev, 0x10, &cell, 1, NULL, 0);
		if (err)
			dev_err(dev, "fail to add %s\n", node->full_name);
	}

	dev_info(dev, "Chrome EC (%s)\n", ec_dev->name);

	return 0;

fail_mfd:
fail_proto:
	free_irq(ec_dev->irq, ec_dev);
fail_irq:
	kfree(ec_dev->dout);
fail_dout:
	kfree(ec_dev->din);
fail_din:
	return err;
}

int __devinit cros_ec_remove(struct chromeos_ec_device *ec_dev)
{
	mfd_remove_devices(ec_dev->dev);
	free_irq(ec_dev->irq, ec_dev);
	kfree(ec_dev->dout);
	kfree(ec_dev->din);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
int cros_ec_suspend(struct chromeos_ec_device *ec_dev)
{
	struct device *dev = ec_dev->dev;

	if (device_may_wakeup(dev))
		ec_dev->wake_enabled = !enable_irq_wake(ec_dev->irq);

	disable_irq(ec_dev->irq);

	return 0;
}

int cros_ec_resume(struct chromeos_ec_device *ec_dev)
{
	/*
	 * When the EC is not a wake source, then it could not have caused the
	 * resume, so we should do the resume processing. This may clear the
	 * EC's key scan buffer, for example. If the EC is a wake source (e.g.
	 * the lid is open and the user might press a key to wake) then we
	 * don't want to do resume processing (key scan buffer should be
	 * preserved).
	 */
	if (!ec_dev->wake_enabled)
		blocking_notifier_call_chain(&ec_dev->wake_notifier, 1, ec_dev);
	enable_irq(ec_dev->irq);

	if (ec_dev->wake_enabled) {
		disable_irq_wake(ec_dev->irq);
		ec_dev->wake_enabled = 0;
	}

	return 0;
}
#endif
