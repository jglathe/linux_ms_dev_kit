// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 Sebastian Reichel <sre@kernel.org>
 */

#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>

struct t14s_fan_ctrl {
	struct i2c_client *i2c;
	struct device *dev;
};

enum t14s_fan_ctrl_fan {
	FAN1 = 0x01,
	FAN2 = 0x02,
};

static const char * const t14s_fan_labels[] = {
	"System",
};

static int t14s_fan_ctrl_blk_transfer(struct t14s_fan_ctrl *t14s_fan_ctrl, u8 cmd, u8 arg, void *databuf, u8 len)
{
	struct i2c_client *client = t14s_fan_ctrl->i2c;
	u8 cmdbuf[2] = {cmd, arg};
	struct i2c_msg msgs[2] = {
		{ .addr = client->addr, .len = sizeof(cmdbuf), .buf = (u8 *) &cmdbuf },
		{ .addr = client->addr, .len = len, .buf = databuf,
		  .flags = I2C_M_RD } };
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		dev_err(t14s_fan_ctrl->dev, "command 0x%02x 0x%02x failed: %d\n", cmd, arg, ret);
	return ret;
}

static int t14s_fan_ctrl_fan_status(struct t14s_fan_ctrl *t14s_fan_ctrl, enum t14s_fan_ctrl_fan fan)
{
	u8 databuf[1];
	int ret;

	// sudo i2ctransfer -y 5 w2@0x36 0x21 0x01 r1
	ret = t14s_fan_ctrl_blk_transfer(t14s_fan_ctrl, 0x21, fan, databuf, sizeof(databuf));
	if (ret < 0)
		return ret;

	return databuf[0]; // 0 = off, 1 = on
}

static int t14s_fan_ctrl_fan_speed(struct t14s_fan_ctrl *t14s_fan_ctrl, enum t14s_fan_ctrl_fan fan)
{
	u8 databuf[3];
	int ret;

	// sudo i2ctransfer -y 5 w2@0x36 0x22 0x01 r3
	ret = t14s_fan_ctrl_blk_transfer(t14s_fan_ctrl, 0x22, fan, databuf, sizeof(databuf));
	if (ret < 0)
		return ret;

	return databuf[2] << 8 | databuf[1]; // RPM
}

static int
t14s_fan_do_read_fan(struct t14s_fan_ctrl *t14s_fan_ctrl, u32 attr, int channel, long *val)
{
	int ret;

	switch (attr) {
	case hwmon_fan_enable:
		ret = t14s_fan_ctrl_fan_status(t14s_fan_ctrl, channel);
		if (ret < 0)
			return ret;
		*val = ret;
		return 0;
	case hwmon_fan_input:
		ret = t14s_fan_ctrl_fan_speed(t14s_fan_ctrl, channel);
		if (ret < 0)
			return ret;
		*val = ret;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int
t14s_fan_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
		    u32 attr, int channel, long *val)
{
	struct t14s_fan_ctrl *t14s_fan_ctrl = dev_get_drvdata(dev);
	channel++;

	switch (type) {
	case hwmon_fan:
		return t14s_fan_do_read_fan(t14s_fan_ctrl, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t
t14s_fan_hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
			  u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		if (attr == hwmon_fan_input || attr == hwmon_fan_label)
			return 0444;
		return 0;
	default:
		return 0;
	}
}

static int
t14s_fan_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_fan:
		*str = t14s_fan_labels[channel];
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops t14s_fan_hwmon_ops = {
	.is_visible = t14s_fan_hwmon_is_visible,
	.read = t14s_fan_hwmon_read,
	.read_string = t14s_fan_hwmon_read_string,
};

static const struct hwmon_channel_info *t14s_fan_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_ENABLE | HWMON_F_INPUT | HWMON_F_LABEL),
	NULL
};

static struct hwmon_chip_info t14s_fan_chip_info = {
	.ops = &t14s_fan_hwmon_ops,
	.info = t14s_fan_hwmon_info,
};

static int t14s_fan_ctrl_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct t14s_fan_ctrl *t14s_fan_ctrl;
	struct device *hwdev;

	t14s_fan_ctrl = devm_kzalloc(dev, sizeof(*t14s_fan_ctrl), GFP_KERNEL);
	if (!t14s_fan_ctrl)
		return -ENOMEM;

	t14s_fan_ctrl->i2c = client;
	t14s_fan_ctrl->dev = dev;

	hwdev = devm_hwmon_device_register_with_info(dev, "fan-controller",
						     t14s_fan_ctrl,
						     &t14s_fan_chip_info, NULL);

	return PTR_ERR_OR_ZERO(hwdev);
}

static const struct i2c_device_id t14s_fan_ctrl_id_table[] = {
	{ "tp-t14s-fan-ctrl" },
	{}
};
MODULE_DEVICE_TABLE(i2c, t14s_fan_ctrl_id_table);

static const struct of_device_id of_t14s_fan_ctrl_match_table[] = {
	{ .compatible = "lenovo,thinkpad-t14s-fan-controller", },
	{}
};
MODULE_DEVICE_TABLE(of, of_t14s_fan_ctrl_match_table);

static struct i2c_driver t14s_fan_ctrl_driver = {
	.probe		= t14s_fan_ctrl_probe,
	.id_table	= t14s_fan_ctrl_id_table,
	.driver		= {
		.name	= "t14s_fan_ctrl",
		.of_match_table = of_t14s_fan_ctrl_match_table,
	},
};
module_i2c_driver(t14s_fan_ctrl_driver);

MODULE_AUTHOR("Sebastian Reichel <sre@kernel.org>");
MODULE_DESCRIPTION("Thinkpad T14s Gen6 Snapdragon fan controller driver");
MODULE_LICENSE("GPL");
