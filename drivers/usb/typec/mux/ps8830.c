// SPDX-License-Identifier: GPL-2.0+
/*
 * Parade PS8830 usb retimer driver
 *
 * Copyright (C) 2024 Linaro Ltd.
 */

#include <drm/bridge/aux-bridge.h>
#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/usb/typec_retimer.h>

struct ps8830_retimer {
	struct i2c_client *client;
	struct gpio_desc *reset_gpio;
	struct regmap *regmap;
	struct typec_switch_dev *sw;
	struct typec_retimer *retimer;
	struct clk *xo_clk;
	struct regulator *vdd_supply;
	struct regulator *vdd33_supply;
	struct regulator *vdd33_cap_supply;
	struct regulator *vddat_supply;
	struct regulator *vddar_supply;
	struct regulator *vddio_supply;

	struct typec_switch *typec_switch;
	struct typec_mux *typec_mux;

	struct mutex lock; /* protect non-concurrent retimer & switch */

	enum typec_orientation orientation;
	unsigned long mode;
	int cfg[3];
};

static void ps8830_write(struct ps8830_retimer *retimer, int cfg0, int cfg1, int cfg2)
{
	regmap_write(retimer->regmap, 0x0, cfg0);
	regmap_write(retimer->regmap, 0x1, cfg1);
	regmap_write(retimer->regmap, 0x2, cfg2);
}

static void ps8830_configure(struct ps8830_retimer *retimer, int cfg0, int cfg1, int cfg2)
{
	if (cfg0 == retimer->cfg[0] &&
	    cfg1 == retimer->cfg[1] &&
	    cfg2 == retimer->cfg[2])
		return;

	retimer->cfg[0] = cfg0;
	retimer->cfg[1] = cfg1;
	retimer->cfg[2] = cfg2;

	/* Write safe-mode config before switching to new config */
	ps8830_write(retimer, 0x1, 0x0, 0x0);

	msleep(1);

	ps8830_write(retimer, cfg0, cfg1, cfg2);
}

static int ps8380_set(struct ps8830_retimer *retimer)
{
	int cfg0 = 0x00;
	int cfg1 = 0x00;
	int cfg2 = 0x00;

	if (retimer->orientation == TYPEC_ORIENTATION_NONE ||
	    retimer->mode == TYPEC_STATE_SAFE) {
		ps8830_write(retimer, 0x1, 0x0, 0x0);
		return 0;
	}

	if (retimer->orientation == TYPEC_ORIENTATION_NORMAL)
		cfg0 = 0x01;
	else
		cfg0 = 0x03;

	switch (retimer->mode) {
	/* USB3 Only */
	case TYPEC_STATE_USB:
		cfg0 |= 0x20;
		break;

	/* DP Only */
	case TYPEC_DP_STATE_C:
		cfg1 = 0x85;
		break;

	case TYPEC_DP_STATE_E:
		cfg1 = 0x81;
		break;

	/* DP + USB */
	case TYPEC_DP_STATE_D:
		cfg0 |= 0x20;
		cfg1 = 0x85;
		break;

	default:
		return -EOPNOTSUPP;
	}

	ps8830_configure(retimer, cfg0, cfg1, cfg2);

	return 0;
}

static int ps8830_sw_set(struct typec_switch_dev *sw,
			 enum typec_orientation orientation)
{
	struct ps8830_retimer *retimer = typec_switch_get_drvdata(sw);
	int ret = 0;

	ret = typec_switch_set(retimer->typec_switch, orientation);
	if (ret)
		return ret;

	mutex_lock(&retimer->lock);

	if (retimer->orientation != orientation) {
		retimer->orientation = orientation;

		ret = ps8380_set(retimer);
	}

	mutex_unlock(&retimer->lock);

	return ret;
}

static int ps8830_retimer_set(struct typec_retimer *rtmr,
			      struct typec_retimer_state *state)
{
	struct ps8830_retimer *retimer = typec_retimer_get_drvdata(rtmr);
	struct typec_mux_state mux_state;
	int ret = 0;

	mutex_lock(&retimer->lock);

	if (state->mode != retimer->mode) {
		retimer->mode = state->mode;

		ret = ps8380_set(retimer);
	}

	mutex_unlock(&retimer->lock);

	if (ret)
		return ret;

	mux_state.alt = state->alt;
	mux_state.data = state->data;
	mux_state.mode = state->mode;

	return typec_mux_set(retimer->typec_mux, &mux_state);
}

static int ps8830_enable_vregs(struct ps8830_retimer *retimer)
{
	struct device *dev = &retimer->client->dev;
	int ret;

	ret = regulator_enable(retimer->vdd33_supply);
	if (ret < 0) {
		dev_err(dev, "cannot enable VDD 3.3V regulator: %d\n", ret);
		return ret;
	}

	ret = regulator_enable(retimer->vdd33_cap_supply);
	if (ret < 0) {
		dev_err(dev, "cannot enable VDD 3.3V CAP regulator: %d\n", ret);
		goto err_vdd33_disable;
	}

	msleep(2);

	ret = regulator_enable(retimer->vdd_supply);
	if (ret < 0) {
		dev_err(dev, "cannot enable VDD regulator: %d\n", ret);
		goto err_vdd33_cap_disable;
	}

	ret = regulator_enable(retimer->vddar_supply);
	if (ret < 0) {
		dev_err(dev, "cannot enable VDDAR regulator: %d\n", ret);
		goto err_vdd_disable;
	}

	ret = regulator_enable(retimer->vddat_supply);
	if (ret < 0) {
		dev_err(dev, "cannot enable VDDAT regulator: %d\n", ret);
		goto err_vddar_disable;
	}

	ret = regulator_enable(retimer->vddio_supply);
	if (ret < 0) {
		dev_err(dev, "cannot enable VDDIO regulator: %d\n", ret);
		goto err_vddat_disable;
	}

	return 0;

err_vddat_disable:
	regulator_disable(retimer->vddat_supply);

err_vddar_disable:
	regulator_disable(retimer->vddar_supply);

err_vdd_disable:
	regulator_disable(retimer->vdd_supply);

err_vdd33_cap_disable:
	regulator_disable(retimer->vdd33_cap_supply);

err_vdd33_disable:
	regulator_disable(retimer->vdd33_supply);

	return ret;
}

static const struct regmap_config ps8830_retimer_regmap = {
	.max_register = 0x1f,
	.reg_bits = 8,
	.val_bits = 8,
};

static int ps8830_retimer_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct typec_switch_desc sw_desc = { };
	struct typec_retimer_desc rtmr_desc = { };
	struct ps8830_retimer *retimer;
	int ret;

	retimer = devm_kzalloc(dev, sizeof(*retimer), GFP_KERNEL);
	if (!retimer)
		return -ENOMEM;

	retimer->client = client;

	retimer->regmap = devm_regmap_init_i2c(client, &ps8830_retimer_regmap);
	if (IS_ERR(retimer->regmap)) {
		dev_err(dev, "failed to allocate register map\n");
		return PTR_ERR(retimer->regmap);
	}

	retimer->vdd_supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(retimer->vdd_supply))
		return PTR_ERR(retimer->vdd_supply);

	retimer->vdd33_supply = devm_regulator_get(dev, "vdd33");
	if (IS_ERR(retimer->vdd33_supply))
		return PTR_ERR(retimer->vdd33_supply);

	retimer->vdd33_cap_supply = devm_regulator_get(dev, "vdd33-cap");
	if (IS_ERR(retimer->vdd33_cap_supply))
		return PTR_ERR(retimer->vdd33_cap_supply);

	retimer->vddat_supply = devm_regulator_get(dev, "vddat");
	if (IS_ERR(retimer->vddat_supply))
		return PTR_ERR(retimer->vddat_supply);

	retimer->vddar_supply = devm_regulator_get(dev, "vddar");
	if (IS_ERR(retimer->vddar_supply))
		return PTR_ERR(retimer->vddar_supply);

	retimer->vddio_supply = devm_regulator_get(dev, "vddio");
	if (IS_ERR(retimer->vddio_supply))
		return PTR_ERR(retimer->vddio_supply);

	retimer->xo_clk = devm_clk_get(dev, "xo");
	if (IS_ERR(retimer->xo_clk))
		return PTR_ERR(retimer->xo_clk);

	retimer->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(retimer->reset_gpio))
		return PTR_ERR(retimer->reset_gpio);

	retimer->typec_switch = fwnode_typec_switch_get(dev->fwnode);
	if (IS_ERR(retimer->typec_switch))
		return dev_err_probe(dev, PTR_ERR(retimer->typec_switch),
				     "failed to acquire orientation-switch\n");

	retimer->typec_mux = fwnode_typec_mux_get(dev->fwnode);
	if (IS_ERR(retimer->typec_mux)) {
		ret = dev_err_probe(dev, PTR_ERR(retimer->typec_mux),
				    "failed to acquire mode-mux\n");
		goto err_switch_put;
	}

	mutex_init(&retimer->lock);

	sw_desc.drvdata = retimer;
	sw_desc.fwnode = dev_fwnode(dev);
	sw_desc.set = ps8830_sw_set;

	ret = drm_aux_bridge_register(dev);
	if (ret)
		goto err_mux_put;

	retimer->sw = typec_switch_register(dev, &sw_desc);
	if (IS_ERR(retimer->sw)) {
		dev_err(dev, "Error registering typec switch\n");
		goto err_aux_bridge_unregister;
	}

	rtmr_desc.drvdata = retimer;
	rtmr_desc.fwnode = dev_fwnode(dev);
	rtmr_desc.set = ps8830_retimer_set;

	retimer->retimer = typec_retimer_register(dev, &rtmr_desc);
	if (IS_ERR(retimer->retimer)) {
		dev_err(dev, "Error registering typec retimer\n");
		goto err_switch_unregister;
	}

	ret = clk_prepare_enable(retimer->xo_clk);
	if (ret) {
		dev_err(dev, "failed to enable XO: %d\n", ret);
		goto err_retimer_unregister;
	}

	ret = ps8830_enable_vregs(retimer);
	if (ret) {
		dev_err(dev, "failed to enable XO: %d\n", ret);
		goto err_clk_disable;
	}

	/* timings needed as per datasheet */
	msleep(4);

	gpiod_set_value(retimer->reset_gpio, 1);

	msleep(60);

	/* Retimer might be left on by bootloader, so read current status */
	regmap_read(retimer->regmap, 0x0, &retimer->cfg[0]);
	regmap_read(retimer->regmap, 0x1, &retimer->cfg[1]);
	regmap_read(retimer->regmap, 0x2, &retimer->cfg[2]);

	retimer->mode = TYPEC_STATE_SAFE;
	retimer->orientation = TYPEC_ORIENTATION_NONE;

	if (retimer->cfg[0] & 0x20) {
		if (retimer->cfg[1] == 0x85)
			retimer->mode = TYPEC_DP_STATE_D;
		else
			retimer->mode = TYPEC_STATE_USB;
	} else {
		if (retimer->cfg[1] == 0x85)
			retimer->mode = TYPEC_DP_STATE_C;
		else if (retimer->cfg[1] == 0x81)
			retimer->mode = TYPEC_DP_STATE_E;
	}

	if (retimer->mode != TYPEC_STATE_SAFE) {
		if (FIELD_GET(0x01, retimer->cfg[0]))
			retimer->orientation = TYPEC_ORIENTATION_NORMAL;
		else if (FIELD_GET(0x03, retimer->cfg[0]) == 0x03)
			retimer->orientation = TYPEC_ORIENTATION_REVERSE;
	}

	return 0;

err_clk_disable:
	clk_disable_unprepare(retimer->xo_clk);

err_retimer_unregister:
	typec_retimer_unregister(retimer->retimer);

err_switch_unregister:
	typec_switch_unregister(retimer->sw);

err_aux_bridge_unregister:
	gpiod_set_value(retimer->reset_gpio, 0);
	clk_disable_unprepare(retimer->xo_clk);

err_mux_put:
	typec_mux_put(retimer->typec_mux);

err_switch_put:
	typec_switch_put(retimer->typec_switch);

	return ret;
}

static void ps8830_retimer_remove(struct i2c_client *client)
{
	struct ps8830_retimer *retimer = i2c_get_clientdata(client);

	typec_retimer_unregister(retimer->retimer);
	typec_switch_unregister(retimer->sw);

	gpiod_set_value(retimer->reset_gpio, 0);

	regulator_disable(retimer->vddio_supply);
	regulator_disable(retimer->vddat_supply);
	regulator_disable(retimer->vddar_supply);
	regulator_disable(retimer->vdd_supply);
	regulator_disable(retimer->vdd33_cap_supply);
	regulator_disable(retimer->vdd33_supply);

	clk_disable_unprepare(retimer->xo_clk);

	typec_mux_put(retimer->typec_mux);
	typec_switch_put(retimer->typec_switch);
}

static const struct of_device_id ps8830_retimer_of_table[] = {
	{ .compatible = "parade,ps8830" },
	{ }
};
MODULE_DEVICE_TABLE(of, ps8830_retimer_of_table);

static struct i2c_driver ps8830_retimer_driver = {
	.driver = {
		.name = "ps8830_retimer",
		.of_match_table = ps8830_retimer_of_table,
	},
	.probe		= ps8830_retimer_probe,
	.remove		= ps8830_retimer_remove,
};

module_i2c_driver(ps8830_retimer_driver);

MODULE_DESCRIPTION("Parade PS8830 Type-C Retimer driver");
MODULE_LICENSE("GPL");
