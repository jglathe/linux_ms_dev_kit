// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm SPEL (SoC Power and Electrical Limits) Driver
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/powercap.h>
#include <linux/slab.h>
#include <linux/types.h>

/* SPEL register bitmasks */
#define ENERGY_STATUS_MASK		GENMASK(31, 0)

#define POWER_LIMIT_MASK		GENMASK(14, 0)
#define POWER_LIMIT_ENABLE		BIT(31)

#define TIME_WINDOW_MASK_L		GENMASK(14, 0)
#define TIME_WINDOW_MASK_H		GENMASK(22, 16)
#define TIME_WINDOW_MAX			((FIELD_MAX(TIME_WINDOW_MASK_H) << 15) | \
					 FIELD_MAX(TIME_WINDOW_MASK_L))

#define ENERGY_UNIT_MASK		GENMASK(19, 16)
#define TIME_UNIT_MASK			GENMASK(11, 8)
#define POWER_UNIT_MASK			GENMASK(2, 0)

#define LIMITS_CAPABILITY_OFFSET	0x20
#define ENERGY_RPT_UNIT_OFFSET		0x04

#define ENERGY_UNIT_SCALE		1000

#define SPEL_DOMAIN_NAME_LENGTH		16

/* Domain types */
enum spel_domain_type {
	SPEL_DOMAIN_SYS,
	SPEL_DOMAIN_SOC,
	SPEL_DOMAIN_CL0,
	SPEL_DOMAIN_CL1,
	SPEL_DOMAIN_CL2,
	SPEL_DOMAIN_IGPU,
	SPEL_DOMAIN_DGPU,
	SPEL_DOMAIN_NSP,
	SPEL_DOMAIN_MMCX,
	SPEL_DOMAIN_INFRA,
	SPEL_DOMAIN_DRAM,
	SPEL_DOMAIN_MDM,
	SPEL_DOMAIN_WLAN,
	SPEL_DOMAIN_USB1,
	SPEL_DOMAIN_USB2,
	SPEL_DOMAIN_USB3,
	SPEL_DOMAIN_MAX,
};

/* Power limit IDs */
enum spel_power_limit_id {
	POWER_LIMIT1,
	POWER_LIMIT2,
	POWER_LIMIT3,
	POWER_LIMIT4,
	POWER_LIMITS_MAX,
};

/* Unit types for conversion */
enum unit_type {
	POWER_UNIT,
	ENERGY_UNIT,
	TIME_UNIT,
};

/* Power limit operation types */
enum pl_ops_type {
	PL_LIMIT,
	PL_TIME_WINDOW,
};

static const char * const pl_names[] = {
	[POWER_LIMIT1] = "pl1",
	[POWER_LIMIT2] = "pl2",
	[POWER_LIMIT3] = "pl3",
	[POWER_LIMIT4] = "pl4",
};

/**
 * struct spel_domain_info - Domain configuration
 * @name:	Domain name
 * @offset:	Register offset in node base
 */
struct spel_domain_info {
	const char *name;
	u32 offset;
};

/* Domain configuration */
static const struct spel_domain_info domain_info[SPEL_DOMAIN_MAX] = {
	[SPEL_DOMAIN_SYS]	= { "sys", 0x40 },
	[SPEL_DOMAIN_SOC]	= { "soc", 0x00 },
	[SPEL_DOMAIN_CL0]	= { "cl0", 0x5c },
	[SPEL_DOMAIN_CL1]	= { "cl1", 0x60 },
	[SPEL_DOMAIN_CL2]	= { "cl2", 0x64 },
	[SPEL_DOMAIN_IGPU]	= { "igpu", 0x08 },
	[SPEL_DOMAIN_DGPU]	= { "dgpu", 0x44 },
	[SPEL_DOMAIN_NSP]	= { "nsp", 0x0c },
	[SPEL_DOMAIN_MMCX]	= { "mmcx", 0x10 },
	[SPEL_DOMAIN_INFRA]	= { "infra", 0x18 },
	[SPEL_DOMAIN_DRAM]	= { "dram", 0x1c },
	[SPEL_DOMAIN_MDM]	= { "mdm", 0x48 },
	[SPEL_DOMAIN_WLAN]	= { "wlan", 0x4c },
	[SPEL_DOMAIN_USB1]	= { "usb1", 0x50 },
	[SPEL_DOMAIN_USB2]	= { "usb2", 0x54 },
	[SPEL_DOMAIN_USB3]	= { "usb3", 0x58 },
};

/**
 * struct spel_constraint_info - Power limit constraint information
 * @limit_offset:	Register offset for power limit value
 * @time_window_offset:	Register offset for time window
 * @supported_mask:	Bit mask in capability register
 * @domain_id:		Domain this constraint applies to
 * @pl_id:		Power limit ID (PL1, PL2, etc.)
 */
struct spel_constraint_info {
	u32 limit_offset;
	u32 time_window_offset;
	u32 supported_mask;
	enum spel_domain_type domain_id;
	int pl_id;
};

/* Constraint configuration */
static const struct spel_constraint_info constraints[] = {
	/* SYS domain constraints */
	{ 0x10, 0x70, BIT(0), SPEL_DOMAIN_SYS, POWER_LIMIT1 },
	{ 0x14, 0x74, BIT(1), SPEL_DOMAIN_SYS, POWER_LIMIT2 },
	{ 0x18, 0x78, BIT(2), SPEL_DOMAIN_SYS, POWER_LIMIT3 },
	{ 0x1c, 0x7c, BIT(3), SPEL_DOMAIN_SYS, POWER_LIMIT4 },
	/* SoC domain constraints */
	{ 0x00, 0x60, BIT(4), SPEL_DOMAIN_SOC, POWER_LIMIT1 },
	{ 0x04, 0x64, BIT(5), SPEL_DOMAIN_SOC, POWER_LIMIT2 },
	{ 0x08, 0x68, BIT(6), SPEL_DOMAIN_SOC, POWER_LIMIT3 },
	{ 0x0c, 0x6c, BIT(7), SPEL_DOMAIN_SOC, POWER_LIMIT4 },
};

/**
 * struct spel_domain - SPEL power domain
 * @power_zone:		Powercap zone
 * @lock:		Mutex protecting register access
 * @sp:			Parent sys domain
 * @status_reg:		Energy counter register
 * @name:		Domain name
 * @id:			Domain type ID
 */
struct spel_domain {
	struct powercap_zone power_zone;
	struct mutex lock; /* Protects register read/write operations */
	void *sp;
	void __iomem *status_reg;
	char name[SPEL_DOMAIN_NAME_LENGTH];
	enum spel_domain_type id;
};

/**
 * struct spel_system -	SPEL system
 * @domains:		Array of domains
 * @power_zone:		Parent powercap zone
 * @node_base:		Base address for node registers
 * @constraint_base:	Base address for constraint registers
 * @config_base:	Base address for config registers
 * @control_type:	Powercap control type
 * @dev:		Device pointer for logging
 * @limits:		Supported power limits per domain
 * @power_unit:		Power unit in microWatts (common for all domains)
 * @energy_unit:	Energy unit in nanoJoules (common for all domains)
 * @time_unit:		Time unit in microseconds (common for all domains)
 */
struct spel_system {
	struct spel_domain *domains;
	struct powercap_zone *power_zone;
	void __iomem *node_base;
	void __iomem *constraint_base;
	void __iomem *config_base;
	struct powercap_control_type *control_type;
	struct device *dev;
	int limits[SPEL_DOMAIN_MAX];
	unsigned int power_unit;
	unsigned int energy_unit;
	unsigned int time_unit;
};

#define power_zone_to_spel_domain(_zone) \
	container_of(_zone, struct spel_domain, power_zone)

static bool is_pl_valid(struct spel_domain *sd, int pl)
{
	struct spel_system *sp = sd->sp;

	return !!(sp->limits[sd->id] & BIT(pl));
}

static int get_pl_ops_offset(struct spel_domain *sd, int pl, enum pl_ops_type pl_op)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(constraints); i++) {
		const struct spel_constraint_info *ci = &constraints[i];

		if (ci->domain_id == sd->id && ci->pl_id == pl) {
			switch (pl_op) {
			case PL_LIMIT:
				return ci->limit_offset;
			case PL_TIME_WINDOW:
				return ci->time_window_offset;
			default:
				return -EOPNOTSUPP;
			}
		}
	}

	return -EOPNOTSUPP;
}

static u64 spel_unit_xlate(struct spel_domain *sd, enum unit_type type,
			   u64 value, int to_raw)
{
	struct spel_system *sp = sd->sp;
	u64 units, scale;

	switch (type) {
	case POWER_UNIT:
		units = sp->power_unit;
		scale = 1;
		break;
	case ENERGY_UNIT:
		units = sp->energy_unit;
		scale = ENERGY_UNIT_SCALE;
		break;
	case TIME_UNIT:
		units = sp->time_unit;
		scale = 1;
		break;
	default:
		return value;
	}

	if (to_raw)
		return DIV_ROUND_CLOSEST_ULL(value * scale, units);

	value *= units;
	return div64_u64(value, scale);
}

static int spel_read_pl_data(struct spel_domain *sd, int pl,
			     enum pl_ops_type pl_op, bool xlate, u64 *data)
{
	struct spel_system *sp = sd->sp;
	void __iomem *reg_addr;
	u64 value;
	int offset;

	if (!is_pl_valid(sd, pl))
		return -EINVAL;

	offset = get_pl_ops_offset(sd, pl, pl_op);
	if (offset < 0)
		return offset;

	guard(mutex)(&sd->lock);

	reg_addr = sp->constraint_base + offset;
	value = readl(reg_addr);

	switch (pl_op) {
	case PL_LIMIT:
		value = FIELD_GET(POWER_LIMIT_MASK, value);
		if (xlate)
			*data = spel_unit_xlate(sd, POWER_UNIT, value, 0);
		else
			*data = value;
		break;
	case PL_TIME_WINDOW:
		/* Decode time window: bits [22:16] are upper 7 bits, [14:0] are lower 15 bits */
		value = (FIELD_GET(TIME_WINDOW_MASK_H, value) << 15) |
			FIELD_GET(TIME_WINDOW_MASK_L, value);
		if (xlate)
			*data = spel_unit_xlate(sd, TIME_UNIT, value, 0);
		else
			*data = value;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int spel_write_pl_data(struct spel_domain *sd, int pl,
			      enum pl_ops_type pl_op, unsigned long long value)
{
	struct spel_system *sp = sd->sp;
	void __iomem *reg_addr;
	u64 reg_val, new_val;
	int offset;

	if (!is_pl_valid(sd, pl))
		return -EINVAL;

	offset = get_pl_ops_offset(sd, pl, pl_op);
	if (offset < 0)
		return offset;

	guard(mutex)(&sd->lock);

	reg_addr = sp->constraint_base + offset;
	reg_val = readl(reg_addr);

	switch (pl_op) {
	case PL_LIMIT:
		new_val = spel_unit_xlate(sd, POWER_UNIT, value, 1);
		if (new_val > FIELD_MAX(POWER_LIMIT_MASK))
			return -EINVAL;
		reg_val = (reg_val & ~POWER_LIMIT_MASK) | FIELD_PREP(POWER_LIMIT_MASK, new_val);

		/*
		 * Enable/Disable PL based on the value:
		 * - If value is 0, disable the PL (clear enable bit)
		 * - If value is non-zero, enable the PL (set enable bit)
		 */
		if (new_val == 0)
			reg_val &= ~POWER_LIMIT_ENABLE;
		else
			reg_val |= POWER_LIMIT_ENABLE;

		writel(reg_val, reg_addr);
		return 0;

	case PL_TIME_WINDOW:
		/*
		 * Encode time window: upper 7 bits to [22:16], lower 15 bits to [14:0]
		 */
		new_val = spel_unit_xlate(sd, TIME_UNIT, value, 1);
		if (new_val > TIME_WINDOW_MAX)
			return -EINVAL;
		/* Read-modify-write to preserve other bits */
		reg_val = (reg_val & ~(TIME_WINDOW_MASK_H | TIME_WINDOW_MASK_L)) |
			  FIELD_PREP(TIME_WINDOW_MASK_H, new_val >> 15) |
			  FIELD_PREP(TIME_WINDOW_MASK_L, new_val);
		writel(reg_val, reg_addr);

		/*
		 * Time window register update doesn't trigger firmware interrupt.
		 * Write to the PL register with current value to trigger the interrupt.
		 */
		offset = get_pl_ops_offset(sd, pl, PL_LIMIT);
		if (offset >= 0) {
			reg_addr = sp->constraint_base + offset;
			reg_val = readl(reg_addr);
			writel(reg_val, reg_addr);
		}
		return 0;

	default:
		return -EINVAL;
	}
}

static int spel_get_energy_counter(struct powercap_zone *power_zone, u64 *energy_raw)
{
	struct spel_domain *sd = power_zone_to_spel_domain(power_zone);
	u64 value;

	value = readl(sd->status_reg);

	*energy_raw = spel_unit_xlate(sd, ENERGY_UNIT, value, 0);

	return 0;
}

static int spel_get_max_energy_counter(struct powercap_zone *pcd_dev, u64 *energy)
{
	struct spel_domain *sd = power_zone_to_spel_domain(pcd_dev);

	*energy = spel_unit_xlate(sd, ENERGY_UNIT, ENERGY_STATUS_MASK, 0);

	return 0;
}

static int spel_release_zone(struct powercap_zone *power_zone)
{
	struct spel_domain *sd = power_zone_to_spel_domain(power_zone);
	struct spel_system *sp = sd->sp;

	/* Free the domains array when the last zone (SYS domain) is released */
	if (sd->id == SPEL_DOMAIN_SYS)
		kfree(sp->domains);

	return 0;
}

static int spel_find_nr_power_limit(struct spel_domain *sd)
{
	int i, nr_pl = 0;

	for (i = 0; i < ARRAY_SIZE(pl_names); i++) {
		if (is_pl_valid(sd, i))
			nr_pl++;
	}

	return nr_pl;
}

static const struct powercap_zone_ops zone_ops = {
	.get_energy_uj = spel_get_energy_counter,
	.get_max_energy_range_uj = spel_get_max_energy_counter,
	.release = spel_release_zone,
};

static int spel_constraint_to_pl(struct spel_domain *sd, int cid)
{
	int i, id;

	for (i = 0, id = 0; i < ARRAY_SIZE(pl_names); i++) {
		if (is_pl_valid(sd, i) && id++ == cid)
			return i;
	}

	return -EINVAL;
}

static int spel_set_power_limit(struct powercap_zone *power_zone, int cid,
				u64 power_limit)
{
	struct spel_domain *sd = power_zone_to_spel_domain(power_zone);
	int id;

	id = spel_constraint_to_pl(sd, cid);
	if (id < 0)
		return id;

	return spel_write_pl_data(sd, id, PL_LIMIT, power_limit);
}

static int spel_get_power_limit(struct powercap_zone *power_zone, int cid,
				u64 *data)
{
	struct spel_domain *sd = power_zone_to_spel_domain(power_zone);
	u64 val;
	int ret, id;

	id = spel_constraint_to_pl(sd, cid);
	if (id < 0)
		return id;

	ret = spel_read_pl_data(sd, id, PL_LIMIT, true, &val);
	if (!ret)
		*data = val;

	return ret;
}

static int spel_set_time_window(struct powercap_zone *power_zone, int cid,
				u64 window)
{
	struct spel_domain *sd = power_zone_to_spel_domain(power_zone);
	int id;

	id = spel_constraint_to_pl(sd, cid);
	if (id < 0)
		return id;

	return spel_write_pl_data(sd, id, PL_TIME_WINDOW, window);
}

static int spel_get_time_window(struct powercap_zone *power_zone, int cid,
				u64 *data)
{
	struct spel_domain *sd = power_zone_to_spel_domain(power_zone);
	u64 val;
	int ret, id;

	id = spel_constraint_to_pl(sd, cid);
	if (id < 0)
		return id;

	ret = spel_read_pl_data(sd, id, PL_TIME_WINDOW, true, &val);
	if (!ret)
		*data = val;

	return ret;
}

static const char *spel_get_constraint_name(struct powercap_zone *power_zone,
					    int cid)
{
	struct spel_domain *sd = power_zone_to_spel_domain(power_zone);
	int id;

	id = spel_constraint_to_pl(sd, cid);
	if (id >= 0 && id < ARRAY_SIZE(pl_names))
		return pl_names[id];

	return NULL;
}

static const struct powercap_zone_constraint_ops constraint_ops = {
	.set_power_limit_uw = spel_set_power_limit,
	.get_power_limit_uw = spel_get_power_limit,
	.set_time_window_us = spel_set_time_window,
	.get_time_window_us = spel_get_time_window,
	.get_name = spel_get_constraint_name,
};

static void spel_init_domains(struct spel_system *sp)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(domain_info); i++) {
		struct spel_domain *sd = &sp->domains[i];

		sd->sp = sp;
		snprintf(sd->name, SPEL_DOMAIN_NAME_LENGTH, "%s",
			 domain_info[i].name);
		sd->id = i;
		sd->status_reg = sp->node_base + domain_info[i].offset;

		/* PL1 is always supported (required for powercap registration) */
		sp->limits[i] = BIT(POWER_LIMIT1);
	}
}

static void spel_update_unit(struct spel_system *sp)
{
	u32 value, shift;

	/* Read power_unit and time_unit from offset 0x0 */
	value = readl(sp->config_base);

	/*
	 * Unit calculation: 1 / (2^shift)
	 * Masks limit: TIME_UNIT (4 bits, max 15), POWER_UNIT (3 bits, max 7).
	 */
	shift = FIELD_GET(POWER_UNIT_MASK, value);
	sp->power_unit = 1000000 / (1 << shift);

	shift = FIELD_GET(TIME_UNIT_MASK, value);
	/*
	 * Time window in register is in milliseconds.
	 */
	sp->time_unit = 1000 * (1 << shift);

	/* Read energy_unit from ENERGY_RPT_UNIT_OFFSET */
	value = readl(sp->config_base + ENERGY_RPT_UNIT_OFFSET);

	/*
	 * Unit calculation: 1 / (2^shift)
	 * Masks limit: ENERGY_UNIT (4 bits, max 15).
	 */
	shift = FIELD_GET(ENERGY_UNIT_MASK, value);
	sp->energy_unit = ENERGY_UNIT_SCALE * 1000000 / (1 << shift);

	dev_dbg(sp->dev, "Units: energy=%dnJ, time=%dus, power=%duW\n",
		sp->energy_unit, sp->time_unit, sp->power_unit);
}

static void spel_detect_powerlimit(struct spel_domain *sd)
{
	struct spel_system *sp = sd->sp;
	u32 capabilities;
	int i, j;

	capabilities = readl(sp->config_base + LIMITS_CAPABILITY_OFFSET);

	/*
	 * Detect power limits from hardware capabilities.
	 * Start from index 1 (POWER_LIMIT2) since PL1 is always enabled in spel_init_domains().
	 */
	for (i = 1; i < ARRAY_SIZE(pl_names); i++) {
		for (j = 0; j < ARRAY_SIZE(constraints); j++) {
			const struct spel_constraint_info *ci = &constraints[j];

			if (ci->domain_id == sd->id && ci->pl_id == i) {
				if (capabilities & ci->supported_mask)
					sp->limits[sd->id] |= BIT(i);
				break;
			}
		}
	}
}

static int spel_init_system(struct spel_system *sp, struct device *dev)
{
	int i, ret;

	/* Read unit configuration (common for all domains) */
	spel_update_unit(sp);

	sp->domains = kcalloc(ARRAY_SIZE(domain_info),
			      sizeof(struct spel_domain), GFP_KERNEL);
	if (!sp->domains)
		return -ENOMEM;

	spel_init_domains(sp);

	for (i = 0; i < ARRAY_SIZE(domain_info); i++) {
		struct spel_domain *sd = &sp->domains[i];

		ret = devm_mutex_init(dev, &sd->lock);
		if (ret) {
			dev_err(dev, "Failed to initialize mutex for domain %s\n", sd->name);
			kfree(sp->domains);
			return ret;
		}

		spel_detect_powerlimit(sd);
	}

	return 0;
}

static int spel_register_powercap(struct spel_system *sp)
{
	struct spel_domain *sd;
	struct powercap_zone *power_zone;
	int nr_pl, ret, i;

	/* Register SYS domain as parent zone */
	sd = &sp->domains[SPEL_DOMAIN_SYS];
	nr_pl = spel_find_nr_power_limit(sd);

	power_zone = powercap_register_zone(&sd->power_zone,
					    sp->control_type, sd->name,
					    NULL, &zone_ops, nr_pl,
					    &constraint_ops);
	if (IS_ERR(power_zone)) {
		dev_err(sp->dev, "Failed to register power zone %s\n",
			sd->name);
		return PTR_ERR(power_zone);
	}
	sp->power_zone = power_zone;

	/* Register other domains as children */
	for (i = 0; i < ARRAY_SIZE(domain_info); i++) {
		struct powercap_zone *parent;

		if (i == SPEL_DOMAIN_SYS)
			continue;

		sd = &sp->domains[i];

		/* SOC is child of SYS, others are children of SOC */
		if (i == SPEL_DOMAIN_SOC)
			parent = sp->power_zone;
		else
			parent = &sp->domains[SPEL_DOMAIN_SOC].power_zone;

		nr_pl = spel_find_nr_power_limit(sd);
		power_zone = powercap_register_zone(&sd->power_zone,
						    sp->control_type,
						    sd->name, parent,
						    &zone_ops, nr_pl,
						    &constraint_ops);

		if (IS_ERR(power_zone)) {
			dev_err(sp->dev, "Failed to register power_zone %s\n",
				sd->name);
			ret = PTR_ERR(power_zone);
			goto err_cleanup;
		}
	}

	return 0;

err_cleanup:
	/* Unregister in reverse order: children first, then SOC, then SYS */
	for (i = i - 1; i >= 0; i--)
		powercap_unregister_zone(sp->control_type, &sp->domains[i].power_zone);

	return ret;
}

static int spel_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spel_system *sp;
	int ret;

	sp = devm_kzalloc(dev, sizeof(*sp), GFP_KERNEL);
	if (!sp)
		return -ENOMEM;

	sp->dev = dev;

	/* Map config registers (units, capabilities) */
	sp->config_base = devm_platform_ioremap_resource_byname(pdev, "config");
	if (IS_ERR(sp->config_base))
		return PTR_ERR(sp->config_base);

	/* Map constraint registers (power limits) */
	sp->constraint_base = devm_platform_ioremap_resource_byname(pdev, "constraints");
	if (IS_ERR(sp->constraint_base))
		return PTR_ERR(sp->constraint_base);

	/* Map spel domain registers (energy counters) */
	sp->node_base = devm_platform_ioremap_resource_byname(pdev, "nodes");
	if (IS_ERR(sp->node_base))
		return PTR_ERR(sp->node_base);

	sp->control_type = powercap_register_control_type(NULL, "qcom-spel",
							  NULL);
	if (IS_ERR(sp->control_type)) {
		dev_err(dev, "Failed to register control type\n");
		return PTR_ERR(sp->control_type);
	}

	/* Initialize system and domains */
	ret = spel_init_system(sp, dev);
	if (ret) {
		dev_err(dev, "Failed to initialize system\n");
		goto err_unregister_control;
	}

	ret = spel_register_powercap(sp);
	if (ret) {
		dev_err(dev, "Failed to register powercap zones\n");
		if (!sp->power_zone)
			kfree(sp->domains);
		goto err_unregister_control;
	}

	platform_set_drvdata(pdev, sp);

	return 0;

err_unregister_control:
	powercap_unregister_control_type(sp->control_type);
	return ret;
}

static void spel_remove(struct platform_device *pdev)
{
	struct spel_system *sp = platform_get_drvdata(pdev);
	int i;

	/* Unregister in reverse order: children first, then SOC, then SYS */
	for (i = ARRAY_SIZE(domain_info) - 1; i >= 0; i--)
		powercap_unregister_zone(sp->control_type, &sp->domains[i].power_zone);

	powercap_unregister_control_type(sp->control_type);
}

static const struct of_device_id spel_of_match[] = {
	{ .compatible = "qcom,glymur-spel" },
	{ }
};
MODULE_DEVICE_TABLE(of, spel_of_match);

static struct platform_driver spel_driver = {
	.probe = spel_probe,
	.remove = spel_remove,
	.driver = {
		.name = "qcom_spel",
		.of_match_table = spel_of_match,
	},
};

module_platform_driver(spel_driver);

MODULE_DESCRIPTION("Qualcomm SPEL Powercap Driver");
MODULE_LICENSE("GPL");
