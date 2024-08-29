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
	struct regulator_bulk_data supplies[4];
	struct gpio_desc *reset_gpio;
	struct regmap *regmap;
	struct typec_switch_dev *sw;
	struct typec_retimer *retimer;
	struct clk *xo_clk;

	bool needs_update;
	struct typec_switch *typec_switch;
	struct typec_mux *typec_mux;

	struct mutex lock; /* protect non-concurrent retimer & switch */

	enum typec_orientation orientation;
	unsigned long mode;
	int cfg[3];

};

static int ps8830_configure(struct ps8830_retimer *retimer, int cfg0, int cfg1, int cfg2)
{
	if (cfg0 == retimer->cfg[0] &&
	    cfg1 == retimer->cfg[1] &&
	    cfg2 == retimer->cfg[2])
		return 0;

	retimer->cfg[0] = cfg0;
	retimer->cfg[1] = cfg1;
	retimer->cfg[2] = cfg2;

	regmap_write(retimer->regmap, 0x0, cfg0);
	regmap_write(retimer->regmap, 0x1, cfg1);
	regmap_write(retimer->regmap, 0x2, cfg2);

	return 0;
}

static int ps8380_set(struct ps8830_retimer *retimer)
{
	int cfg0 = 0x00, cfg1 = 0x00, cfg2 = 0x00;
	int ret;

	retimer->needs_update = false;

	switch (retimer->orientation) {
	/* Safe mode */
	case TYPEC_ORIENTATION_NONE:
		cfg0 = 0x01;
		cfg1 = 0x00;
		cfg2 = 0x00;
		break;
	case TYPEC_ORIENTATION_NORMAL:
		cfg0 = 0x01;
		break;
	case TYPEC_ORIENTATION_REVERSE:
		cfg0 = 0x03;
		break;
	}

	switch (retimer->mode) {
	/* Safe mode */
	case TYPEC_STATE_SAFE:
		cfg0 = 0x01;
		cfg1 = 0x00;
		cfg2 = 0x00;
		break;

	/* USB3 Only */
	case TYPEC_STATE_USB:
		cfg0 |= 0x20;
		break;

	/* DP Only */
	case TYPEC_DP_STATE_C:
	case TYPEC_DP_STATE_E:
		cfg0 &= 0x0f;
		cfg1 = 0x85;
		break;

	/* DP + USB */
	case TYPEC_DP_STATE_D:
	case TYPEC_DP_STATE_F:
		cfg0 |= 0x20;
		cfg1 = 0x85;
		break;

	default:
		return -EOPNOTSUPP;
	}

	gpiod_set_value(retimer->reset_gpio, 0);
	msleep(20);
	gpiod_set_value(retimer->reset_gpio, 1);

	msleep(60);

	ret = ps8830_configure(retimer, 0x01, 0x00, 0x00);

	msleep(30);

	return ps8830_configure(retimer, cfg0, cfg1, cfg2);
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
		retimer->needs_update = true;
	}

	if (retimer->needs_update)
		ret = ps8380_set(retimer);

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
		retimer->needs_update = true;
	}

	if (retimer->needs_update)
		ret = ps8380_set(retimer);

	mutex_unlock(&retimer->lock);

	if (ret)
		return ret;

	mux_state.alt = state->alt;
	mux_state.data = state->data;
	mux_state.mode = state->mode;

	return typec_mux_set(retimer->typec_mux, &mux_state);
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
		dev_err(dev, "Failed to allocate register map\n");
		return PTR_ERR(retimer->regmap);
	}

	retimer->supplies[0].supply = "vdd33";
	retimer->supplies[1].supply = "vdd18";
	retimer->supplies[2].supply = "vdd15";
	retimer->supplies[3].supply = "vcc";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(retimer->supplies),
				      retimer->supplies);
	if (ret)
		return ret;

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

	ret = regulator_bulk_enable(ARRAY_SIZE(retimer->supplies),
				    retimer->supplies);
	if (ret < 0) {
		dev_err(dev, "cannot enable regulators %d\n", ret);
		goto err_mux_put;
	}

	ret = clk_prepare_enable(retimer->xo_clk);
	if (ret) {
		dev_err(dev, "Failed to enable XO: %d\n", ret);
		goto err_disable_vreg;
	}

	gpiod_set_value(retimer->reset_gpio, 0);
	msleep(20);
	gpiod_set_value(retimer->reset_gpio, 1);

	msleep(60);
	mutex_init(&retimer->lock);

	sw_desc.drvdata = retimer;
	sw_desc.fwnode = dev_fwnode(dev);
	sw_desc.set = ps8830_sw_set;

	ret = drm_aux_bridge_register(dev);
	if (ret)
		goto err_disable_gpio;

	retimer->sw = typec_switch_register(dev, &sw_desc);
	if (IS_ERR(retimer->sw)) {
		ret = dev_err_probe(dev, PTR_ERR(retimer->sw),
				    "Error registering typec switch\n");
		goto err_disable_gpio;
	}

	rtmr_desc.drvdata = retimer;
	rtmr_desc.fwnode = dev_fwnode(dev);
	rtmr_desc.set = ps8830_retimer_set;

	retimer->retimer = typec_retimer_register(dev, &rtmr_desc);
	if (IS_ERR(retimer->retimer)) {
		ret = dev_err_probe(dev, PTR_ERR(retimer->retimer),
				    "Error registering typec retimer\n");
		goto err_switch_unregister;
	}

	dev_info(dev, "Registered Parade PS8830 retimer\n");
	return 0;

err_switch_unregister:
	typec_switch_unregister(retimer->sw);

err_disable_gpio:
	gpiod_set_value(retimer->reset_gpio, 0);
	clk_disable_unprepare(retimer->xo_clk);

err_disable_vreg:
	regulator_bulk_disable(ARRAY_SIZE(retimer->supplies),
			       retimer->supplies);
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

	clk_disable_unprepare(retimer->xo_clk);

	regulator_bulk_disable(ARRAY_SIZE(retimer->supplies),
			       retimer->supplies);

	typec_mux_put(retimer->typec_mux);
	typec_switch_put(retimer->typec_switch);
}

static const struct i2c_device_id ps8830_retimer_table[] = {
	{ "parade,ps8830" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ps8830_retimer_table);

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
	.id_table	= ps8830_retimer_table,
};

module_i2c_driver(ps8830_retimer_driver);

MODULE_DESCRIPTION("Parade PS8830 Type-C Retimer driver");
MODULE_LICENSE("GPL");
