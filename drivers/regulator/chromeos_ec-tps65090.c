/*
 *  Copyright (C) 2013 Google, Inc
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
 * Expose the ChromeOS EC regulator information.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/mfd/chromeos_ec.h>
#include <linux/mfd/chromeos_ec_commands.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/of.h>

#define MAX_REGULATORS		10

struct ec_tps65090_regulator {
	u32			   control_reg;
	struct chromeos_ec_device *ec;
	struct regulator_desc	   desc;
	struct regulator_dev	  *rdev;
};

struct ec_tps65090_data {
	struct ec_tps65090_regulator *regulators[MAX_REGULATORS];
};

/* Control register for FETs(7) range from 15 -> 21 and correspond to
   FET number (1-7) on the EC side */
#define reg_to_ec_fet_index(reg) (reg->control_reg - 14)

static int ec_tps65090_fet_is_enabled(struct regulator_dev *rdev)
{
	struct ec_tps65090_regulator *reg = rdev_get_drvdata(rdev);
	struct chromeos_ec_device *ec = reg->ec;
	struct ec_params_ldo_get ec_data;
	struct ec_response_ldo_get ec_info;
	int ret;

	ec_data.index = reg_to_ec_fet_index(reg);
	ret = ec->command_sendrecv(ec, EC_CMD_LDO_GET, &ec_data,
				   sizeof(struct ec_params_ldo_get), &ec_info,
				   sizeof(struct ec_response_ldo_get));
	if (ret < 0)
		return ret;

	return ec_info.state;
}

static int ec_tps65090_fet_enable(struct regulator_dev *rdev)
{
	struct ec_params_ldo_set ec_data;
	struct ec_tps65090_regulator *reg = rdev_get_drvdata(rdev);
	struct chromeos_ec_device *ec = reg->ec;

	ec_data.index = reg_to_ec_fet_index(reg);
	ec_data.state = 1;
	return ec->command_send(ec, EC_CMD_LDO_SET, &ec_data,
				sizeof(struct ec_params_ldo_set));
}

static int ec_tps65090_fet_disable(struct regulator_dev *rdev)
{
	struct ec_tps65090_regulator *reg = rdev_get_drvdata(rdev);
	struct chromeos_ec_device *ec = reg->ec;
	struct ec_params_ldo_set ec_data;

	ec_data.index = reg_to_ec_fet_index(reg);
	ec_data.state = 0;
	return ec->command_send(ec, EC_CMD_LDO_SET, &ec_data,
				sizeof(struct ec_params_ldo_set));
}

static int ec_tps65090_set_voltage(struct regulator_dev *rdev, int min,
				   int max, unsigned *sel)
{
	/*
	 * Only needed for the core code to set constraints; the voltage
	 * isn't actually adjustable on tps65090.
	 */
	return 0;
}

static struct regulator_ops ec_tps65090_fet_ops = {
	.is_enabled	= ec_tps65090_fet_is_enabled,
	.enable		= ec_tps65090_fet_enable,
	.disable	= ec_tps65090_fet_disable,
	.set_voltage	= ec_tps65090_set_voltage,
};

static void ec_tps65090_unregister_regs(struct ec_tps65090_regulator *regs[])
{
	int i;

	for (i = 0; i < MAX_REGULATORS; i++)
		if (regs[i]) {
			regulator_unregister(regs[i]->rdev);
			kfree(regs[i]->rdev->desc->name);
			kfree(regs[i]->rdev);
		}
}

static int __devinit ec_tps65090_probe(struct platform_device *pdev)
{
	struct chromeos_ec_device *ec = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct ec_tps65090_data *data;
	struct ec_tps65090_regulator *reg;
	struct device_node *mfd_np, *reg_np, *np;
	struct regulator_init_data *ri;
	u32 id;

	if (!ec) {
		dev_err(dev, "no EC device found\n");
		return -EINVAL;
	}

	mfd_np = dev->parent->of_node;
	if (!mfd_np) {
		dev_err(dev, "no device tree data available\n");
		return -EINVAL;
	}

	reg_np = of_find_node_by_name(mfd_np, "voltage-regulators");
	if (!reg_np) {
		dev_err(dev, "no OF regulator data found at %s\n",
			mfd_np->full_name);
		return -EINVAL;
	}

	data = devm_kzalloc(dev, sizeof(struct ec_tps65090_data), GFP_KERNEL);
	if (!data) {
		of_node_put(reg_np);
		return -ENOMEM;
	}

	id = 0;
	for_each_child_of_node(reg_np, np) {
		ri = of_get_regulator_init_data(dev, np);
		if (!ri) {
			dev_err(dev, "regulator_init_data failed for %s\n",
				np->full_name);
			goto err;
		}

		reg = devm_kzalloc(dev, sizeof(struct ec_tps65090_regulator),
				   GFP_KERNEL);
		reg->desc.name = kstrdup(of_get_property(np, "regulator-name",
							 NULL), GFP_KERNEL);
		reg->ec = ec;
		if (!reg->desc.name) {
			dev_err(dev, "no regulator-name specified at %s\n",
				np->full_name);
			goto err;
		}

		if (of_property_read_u32(np, "tps65090-control-reg",
					 &reg->control_reg)) {
			dev_err(dev, "no control-reg property at %s\n",
				np->full_name);
			goto err;
		}

		reg->desc.id = id;
		reg->desc.ops = &ec_tps65090_fet_ops;
		reg->desc.type = REGULATOR_VOLTAGE;
		reg->desc.owner = THIS_MODULE;
		reg->rdev = regulator_register(&reg->desc, dev, ri, reg, np);
		dev_dbg(dev, "%s supply registered (FET%d)\n", reg->desc.name,
			reg_to_ec_fet_index(reg));
		data->regulators[id++] = reg;
	}

	platform_set_drvdata(pdev, data);
	of_node_put(reg_np);

	return 0;

err:
	dev_err(dev, "bad OF regulator data in %s\n", reg_np->full_name);
	ec_tps65090_unregister_regs(data->regulators);
	of_node_put(reg_np);
	return -EINVAL;
}

static int __devexit ec_tps65090_remove(struct platform_device *pdev)
{
	struct ec_tps65090_data *data = platform_get_drvdata(pdev);
	ec_tps65090_unregister_regs(data->regulators);
	return 0;
}

static struct platform_driver ec_tps65090_driver = {
	.driver = {
		.name = "cros_ec-tps65090",
		.owner = THIS_MODULE,
	},
	.probe = ec_tps65090_probe,
	.remove = __devexit_p(ec_tps65090_remove),
};

module_platform_driver(ec_tps65090_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ChromeOS EC TPS65090");
MODULE_ALIAS("regulator:cros_ec-tps65090");
