// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

#define VSET_STEP_MV			8
#define VSET_STEP_UV			(VSET_STEP_MV * 1000)

#define LDO_ENABLE_REG(base)		((base) + 0x46)
#define ENABLE_BIT			BIT(7)

#define LDO_VSET_LB_REG(base)		((base) + 0x40)

#define LDO_STEPPER_CTL_REG(base)	((base) + 0x3b)
#define DEFAULT_VOLTAGE_STEPPER_RATE	38400
#define STEP_RATE_MASK			GENMASK(1, 0)

#define NLDO_MIN_UV			528000
#define NLDO_MAX_UV			1504000

#define PLDO_MIN_UV			1504000
#define PLDO_MAX_UV			3400000

struct pm8008_regulator_data {
	const char			*name;
	const char			*supply_name;
	u16				base;
	int				min_dropout_uv;
	const struct linear_range	*voltage_range;
};

struct pm8008_regulator {
	struct regmap		*regmap;
	struct regulator_desc	rdesc;
	u16			base;
	int			step_rate;
};

static const struct linear_range nldo_ranges[] = {
	REGULATOR_LINEAR_RANGE(528000, 0, 122, 8000),
};

static const struct linear_range pldo_ranges[] = {
	REGULATOR_LINEAR_RANGE(1504000, 0, 237, 8000),
};

static const struct pm8008_regulator_data reg_data[] = {
	/* name	  parent       base    headroom_uv voltage_range */
	{ "ldo1", "vdd_l1_l2", 0x4000, 225000, nldo_ranges, },
	{ "ldo2", "vdd_l1_l2", 0x4100, 225000, nldo_ranges, },
	{ "ldo3", "vdd_l3_l4", 0x4200, 300000, pldo_ranges, },
	{ "ldo4", "vdd_l3_l4", 0x4300, 300000, pldo_ranges, },
	{ "ldo5", "vdd_l5",    0x4400, 200000, pldo_ranges, },
	{ "ldo6", "vdd_l6",    0x4500, 200000, pldo_ranges, },
	{ "ldo7", "vdd_l7",    0x4600, 200000, pldo_ranges, },
};

static int pm8008_regulator_get_voltage(struct regulator_dev *rdev)
{
	struct pm8008_regulator *pm8008_reg = rdev_get_drvdata(rdev);
	__le16 mV;
	int uV;

	regmap_bulk_read(pm8008_reg->regmap,
			LDO_VSET_LB_REG(pm8008_reg->base), (void *)&mV, 2);

	uV = le16_to_cpu(mV) * 1000;
	return (uV - pm8008_reg->rdesc.min_uV) / pm8008_reg->rdesc.uV_step;
}

static inline int pm8008_write_voltage(struct pm8008_regulator *pm8008_reg,
							int mV)
{
	__le16 vset_raw;

	vset_raw = cpu_to_le16(mV);

	return regmap_bulk_write(pm8008_reg->regmap,
			LDO_VSET_LB_REG(pm8008_reg->base),
			(const void *)&vset_raw, sizeof(vset_raw));
}

static int pm8008_regulator_set_voltage_time(struct regulator_dev *rdev,
				int old_uV, int new_uv)
{
	struct pm8008_regulator *pm8008_reg = rdev_get_drvdata(rdev);

	return DIV_ROUND_UP(abs(new_uv - old_uV), pm8008_reg->step_rate);
}

static int pm8008_regulator_set_voltage(struct regulator_dev *rdev,
					unsigned int selector)
{
	struct pm8008_regulator *pm8008_reg = rdev_get_drvdata(rdev);
	int rc, mV;

	rc = regulator_list_voltage_linear_range(rdev, selector);
	if (rc < 0)
		return rc;

	/* voltage control register is set with voltage in millivolts */
	mV = DIV_ROUND_UP(rc, 1000);

	rc = pm8008_write_voltage(pm8008_reg, mV);
	if (rc < 0)
		return rc;

	return 0;
}

static const struct regulator_ops pm8008_regulator_ops = {
	.enable			= regulator_enable_regmap,
	.disable		= regulator_disable_regmap,
	.is_enabled		= regulator_is_enabled_regmap,
	.set_voltage_sel	= pm8008_regulator_set_voltage,
	.get_voltage_sel	= pm8008_regulator_get_voltage,
	.list_voltage		= regulator_list_voltage_linear,
	.set_voltage_time	= pm8008_regulator_set_voltage_time,
};

static int pm8008_regulator_probe(struct platform_device *pdev)
{
	struct regulator_config reg_config = {};
	struct pm8008_regulator *pm8008_reg;
	struct device *dev = &pdev->dev;
	struct regulator_desc *rdesc;
	struct regulator_dev *rdev;
	struct regmap *regmap;
	unsigned int val;
	int rc, i;

	regmap = dev_get_regmap(dev->parent, "secondary");
	if (!regmap)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(reg_data); i++) {
		pm8008_reg = devm_kzalloc(dev, sizeof(*pm8008_reg), GFP_KERNEL);
		if (!pm8008_reg)
			return -ENOMEM;

		pm8008_reg->regmap = regmap;
		pm8008_reg->base = reg_data[i].base;

		/* get slew rate */
		rc = regmap_bulk_read(pm8008_reg->regmap,
				LDO_STEPPER_CTL_REG(pm8008_reg->base), &val, 1);
		if (rc < 0) {
			dev_err(dev, "failed to read step rate: %d\n", rc);
			return rc;
		}
		val &= STEP_RATE_MASK;
		pm8008_reg->step_rate = DEFAULT_VOLTAGE_STEPPER_RATE >> val;

		rdesc = &pm8008_reg->rdesc;
		rdesc->type = REGULATOR_VOLTAGE;
		rdesc->ops = &pm8008_regulator_ops;
		rdesc->name = reg_data[i].name;
		rdesc->supply_name = reg_data[i].supply_name;
		rdesc->of_match = reg_data[i].name;
		rdesc->uV_step = VSET_STEP_UV;
		rdesc->linear_ranges = reg_data[i].voltage_range;
		rdesc->n_linear_ranges = 1;
		BUILD_BUG_ON((ARRAY_SIZE(pldo_ranges) != 1) ||
				(ARRAY_SIZE(nldo_ranges) != 1));

		if (reg_data[i].voltage_range == nldo_ranges) {
			rdesc->min_uV = NLDO_MIN_UV;
			rdesc->n_voltages = ((NLDO_MAX_UV - NLDO_MIN_UV) / rdesc->uV_step) + 1;
		} else {
			rdesc->min_uV = PLDO_MIN_UV;
			rdesc->n_voltages = ((PLDO_MAX_UV - PLDO_MIN_UV) / rdesc->uV_step) + 1;
		}

		rdesc->enable_reg = LDO_ENABLE_REG(pm8008_reg->base);
		rdesc->enable_mask = ENABLE_BIT;
		rdesc->min_dropout_uV = reg_data[i].min_dropout_uv;
		rdesc->regulators_node = of_match_ptr("regulators");

		reg_config.dev = dev->parent;
		reg_config.driver_data = pm8008_reg;
		reg_config.regmap = pm8008_reg->regmap;

		rdev = devm_regulator_register(dev, rdesc, &reg_config);
		if (IS_ERR(rdev)) {
			rc = PTR_ERR(rdev);
			dev_err(dev, "failed to register regulator %s: %d\n",
					reg_data[i].name, rc);
			return rc;
		}
	}

	return 0;
}

static struct platform_driver pm8008_regulator_driver = {
	.driver	= {
		.name = "qcom-pm8008-regulator",
	},
	.probe = pm8008_regulator_probe,
};

module_platform_driver(pm8008_regulator_driver);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. PM8008 PMIC Regulator Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:qcom-pm8008-regulator");
