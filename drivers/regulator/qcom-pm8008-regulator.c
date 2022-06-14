// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mfd/qcom_pm8008.h>
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
	/* name  parent       headroom_uv voltage_range */
	{ "ldo1", "vdd_l1_l2", 225000, nldo_ranges, },
	{ "ldo2", "vdd_l1_l2", 225000, nldo_ranges, },
	{ "ldo3", "vdd_l3_l4", 300000, pldo_ranges, },
	{ "ldo4", "vdd_l3_l4", 300000, pldo_ranges, },
	{ "ldo5", "vdd_l5",    200000, pldo_ranges, },
	{ "ldo6", "vdd_l6",    200000, pldo_ranges, },
	{ "ldo7", "vdd_l7",    200000, pldo_ranges, },
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
	int rc, i;
	u32 base;
	unsigned int reg;
	const char *name;
	struct device *dev = &pdev->dev;
	struct regulator_config reg_config = {};
	struct regulator_dev    *rdev;
	const struct pm8008_data *chip = dev_get_drvdata(pdev->dev.parent);
	struct pm8008_regulator *pm8008_reg;

	pm8008_reg = devm_kzalloc(dev, sizeof(*pm8008_reg), GFP_KERNEL);
	if (!pm8008_reg)
		return -ENOMEM;

	pm8008_reg->regmap = pm8008_get_regmap(chip);
	if (!pm8008_reg->regmap) {
		dev_err(dev, "parent regmap is missing\n");
		return -EINVAL;
	}

	rc = of_property_read_string(dev->of_node, "regulator-name", &name);
	if (rc)
		return rc;

	/* get the required regulator data */
	for (i = 0; i < ARRAY_SIZE(reg_data); i++)
		if (strstr(name, reg_data[i].name))
			break;

	if (i == ARRAY_SIZE(reg_data)) {
		dev_err(dev, "Invalid regulator name %s\n", name);
		return -EINVAL;
	}

	rc = of_property_read_u32_index(dev->of_node, "reg", 1, &base);
	if (rc < 0) {
		dev_err(dev, "%s: failed to get regulator base rc=%d\n", name, rc);
		return rc;
	}
	pm8008_reg->base = base;

	/* get slew rate */
	rc = regmap_bulk_read(pm8008_reg->regmap,
			LDO_STEPPER_CTL_REG(pm8008_reg->base), &reg, 1);
	if (rc < 0) {
		dev_err(dev, "failed to read step rate configuration rc=%d\n", rc);
		return rc;
	}
	reg &= STEP_RATE_MASK;
	pm8008_reg->step_rate = DEFAULT_VOLTAGE_STEPPER_RATE >> reg;

	pm8008_reg->rdesc.type = REGULATOR_VOLTAGE;
	pm8008_reg->rdesc.ops = &pm8008_regulator_ops;
	pm8008_reg->rdesc.name = reg_data[i].name;
	pm8008_reg->rdesc.supply_name = reg_data[i].supply_name;
	pm8008_reg->rdesc.of_match = reg_data[i].name;
	pm8008_reg->rdesc.uV_step = VSET_STEP_UV;
	pm8008_reg->rdesc.linear_ranges = reg_data[i].voltage_range;
	pm8008_reg->rdesc.n_linear_ranges = 1;
	BUILD_BUG_ON((ARRAY_SIZE(pldo_ranges) != 1) ||
				(ARRAY_SIZE(nldo_ranges) != 1));

	if (reg_data[i].voltage_range == nldo_ranges) {
		pm8008_reg->rdesc.min_uV = NLDO_MIN_UV;
		pm8008_reg->rdesc.n_voltages
				= ((NLDO_MAX_UV - NLDO_MIN_UV)
					/ pm8008_reg->rdesc.uV_step) + 1;
	} else {
		pm8008_reg->rdesc.min_uV = PLDO_MIN_UV;
		pm8008_reg->rdesc.n_voltages
				= ((PLDO_MAX_UV - PLDO_MIN_UV)
					/ pm8008_reg->rdesc.uV_step) + 1;
	}

	pm8008_reg->rdesc.enable_reg = LDO_ENABLE_REG(pm8008_reg->base);
	pm8008_reg->rdesc.enable_mask = ENABLE_BIT;
	pm8008_reg->rdesc.min_dropout_uV = reg_data[i].min_dropout_uv;

	reg_config.dev = dev->parent;
	reg_config.driver_data = pm8008_reg;
	reg_config.regmap = pm8008_reg->regmap;

	rdev = devm_regulator_register(dev, &pm8008_reg->rdesc, &reg_config);
	if (IS_ERR(rdev)) {
		rc = PTR_ERR(rdev);
		dev_err(dev, "%s: failed to register regulator rc=%d\n",
				reg_data[i].name, rc);
		return rc;
	}

	return 0;
}

static const struct of_device_id pm8008_regulator_match_table[] = {
	{ .compatible = "qcom,pm8008-regulator", },
	{ }
};
MODULE_DEVICE_TABLE(of, pm8008_regulator_match_table);

static struct platform_driver pm8008_regulator_driver = {
	.driver	= {
		.name		= "qcom-pm8008-regulator",
		.of_match_table	= pm8008_regulator_match_table,
	},
	.probe	= pm8008_regulator_probe,
};

module_platform_driver(pm8008_regulator_driver);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. PM8008 PMIC Regulator Driver");
MODULE_LICENSE("GPL");
