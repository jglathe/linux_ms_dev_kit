// SPDX-License-Identifier: GPL-2.0
/*
 * Onsemi AR1337 CMOS Image Sensor Driver
 *
 * Copyright (C) 2019 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define AR1337_REG_CHIP_ID 0x0000
#define AR1337_REG_MODE	0x0100
#define AR1337_REG_SOFT_RESET 0x0103
#define AR1337_REG_HOLD	0x0104

#define AR1337_CHIP_ID 0x0253

#define AR1337_DEFAULT_FORMAT MEDIA_BUS_FMT_SGRBG10_1X10

static const char * const ar1337_supply_name[] = {
	"vdda",
	"vddd",
	"vdddo",
};

#define AR1337_NUM_SUPPLIES ARRAY_SIZE(ar1337_supply_name)

struct ar1337_regval {
	u16 reg;
	u16 val;
};

struct ar1337_mode {
	u32 width;
	u32 height;
	u32 pixel_rate;

	const struct ar1337_regval *data;
	u32 data_size;
};

struct ar1337 {
	struct i2c_client *i2c_client;
	struct device *dev;
	struct clk *xclk;

	struct v4l2_subdev sd;
	struct v4l2_fwnode_endpoint *ep;
	struct media_pad pad;
	struct v4l2_mbus_framefmt current_format;
	const struct ar1337_mode *current_mode;

	struct regulator_bulk_data supplies[AR1337_NUM_SUPPLIES];
	struct gpio_desc *pwdn_gpio;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *pixel_rate;

	struct mutex lock;
};

static const struct ar1337_regval ar1337_global_init_settings[] = {
/* mipi_timing_recommended */
	{ 0x31B0, 0x005C },
	{ 0x31B2, 0x002D },
	{ 0x31B4, 0x3392 },
	{ 0x31B6, 0x242B },
	{ 0x31B8, 0x2413 },
	{ 0x31BA, 0x2070 },
	{ 0x31BC, 0x868B },
	/* PLL SETUP MAX */
	{ 0x0304, 0x0101 },
	{ 0x0306, 0x2E2E },
	{ 0x0302, 0x0001 },
	{ 0x030A, 0x0001 },
	{ 0x0300, 0x0005 },
	{ 0x0308, 0x000A },
	{ 0x0112, 0x0A0A },
	{ 0x3016, 0x0101 },
};

static const struct ar1337_regval ar1337_2104_1560_settings[] = {
	{ 0x0344, 0x0008 }, //X_ADDR_START 8
	{ 0x0348, 0x1077 }, //X_ADDR_END 4215
	{ 0x0346, 0x0008 }, //Y_ADDR_START 8
	{ 0x034A, 0x0C35 }, //Y_ADDR_END 3125
	{ 0x034C, 0x0838 }, //X_OUTPUT_SIZE 2104
	{ 0x034E, 0x0618 }, //Y_OUTPUT_SIZE 1560
	{ 0x3040, 0x0043 }, //X_BIN, X_ODD_INC, Y_ODD_INC
	{ 0x317A, 0x516E }, //SF_BIN_ENABLE
	{ 0x3F3C, 0x0003 }, //SF_BIN_ENABLE
	{ 0x0400, 0x0001 }, //Scaling Enabling: 0= disable, 1= x-dir
	{ 0x0404, 0x0020 }, //Scale_M = 32
	{ 0x3040, 0x0041 }, //read_mode
	{ 0x0112, 0x0A0A }, //data_format=10
	{ 0x3172, 0x0206 }, //digbin_enable
	{ 0x317A, 0x416E }, //sfbin_enable
	{ 0x3F3C, 0x0003 }, //bin4
	{ 0x3222, 0x00C0 }, //pdaf_row_offset=192
	{ 0x32c8, 0x0848 }, //pdaf_seq_start=832
	{ 0x32ca, 0x0486 }, //pdaf_odp_line_length=1158
	{ 0x31AE, 0x0204 }, //MIPI DATA LANES = 4
	{ 0x0342, 0x1230 }, //LINE_LENGTH_PCK Default
	{ 0x0340, 0x062C }, //FRAME_LENGTH_LINES 1580

	/* Our settings */
	{ 0x3220, 0x0000 },// PDAF DISABLE
	{ 0x3044, 0x2580 },
	{ 0x301A, 0x4018 },
};

/* Mode configs */
static const struct ar1337_mode ar1337_modes[] = {
	{
		.width = 2104,
		.height = 1560,
		.data = ar1337_2104_1560_settings,
		.data_size = ARRAY_SIZE(ar1337_2104_1560_settings),
		.pixel_rate = 110400000,
	},
};

static inline struct ar1337 *to_ar1337(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct ar1337, sd);
}

static int ar1337_write_reg8(struct ar1337 *ar1337, u16 reg, u8 val)
{
	u8 regbuf[3];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;
	regbuf[2] = val;

	ret = i2c_master_send(ar1337->i2c_client, regbuf, 3);
	if (ret < 0) {
		dev_err(ar1337->dev, "%s: write reg error %d: reg=%x, val=%x\n",
			__func__, ret, reg, val);
		return ret;
	}

	return 0;
}

static int ar1337_read_reg8(struct ar1337 *ar1337, u16 reg, u8 *val)
{
	int ret;
	u8 regbuf[2];

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;

	ret = i2c_master_send(ar1337->i2c_client, regbuf, 2);
	if (ret < 0) {
		dev_err(ar1337->dev, "%s: write reg error %d: reg=%x\n",
			__func__, ret, reg);
		return ret;
	}

	ret = i2c_master_recv(ar1337->i2c_client, val, 1);
	if (ret < 0) {
		dev_err(ar1337->dev, "%s: read reg error %d: reg=%x\n",
			__func__, ret, reg);
		return ret;
	}

	return 0;
}

static int ar1337_read_reg16(struct ar1337 *ar1337, u16 reg, u16 *val)
{
	int ret;
	u8 regbuf[2], valbuf[2];

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;

	ret = i2c_master_send(ar1337->i2c_client, regbuf, 2);
	if (ret < 0) {
		dev_err(ar1337->dev, "%s: write reg error %d: reg=%x\n",
			__func__, ret, reg);
		return ret;
	}

	ret = i2c_master_recv(ar1337->i2c_client, valbuf, 2);
	if (ret < 0) {
		dev_err(ar1337->dev, "%s: read reg error %d: reg=%x\n",
			__func__, ret, reg);
		return ret;
	}

	*val = be16_to_cpu(*((__be16 *)valbuf));

	return 0;
}

static int ar1337_write_reg16(struct ar1337 *ar1337, u16 reg, u16 val)
{
	u8 regbuf[4];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;
	regbuf[2] = val >> 8;
	regbuf[3] = val & 0xff;

	ret = i2c_master_send(ar1337->i2c_client, regbuf, 4);
	if (ret < 0) {
		dev_err(ar1337->dev, "%s: write reg error %d: reg=%x, val=%x\n",
			__func__, ret, reg, val);
		return ret;
	}

	return 0;
}

static int ar1337_set_register_array(struct ar1337 *ar1337,
				     const struct ar1337_regval *settings,
				     unsigned int num_settings)
{
	unsigned int i;
	int ret;

	for (i = 0; i < num_settings; ++i, ++settings) {
		ret = ar1337_write_reg16(ar1337, settings->reg, settings->val);
		if (ret < 0)
			return ret;

		/* Settle time is 10ms for all registers */
		msleep(10);
	}

	return 0;
}
/*
static int ar1337_write_buffered_reg(struct ar1337 *ar1337, u16 address_low,
				     u8 nr_regs, u32 value)
{
	unsigned int i;
	int ret;

	ret = ar1337_write_reg8(ar1337, AR1337_REG_HOLD, 0x01);
	if (ret) {
		dev_err(ar1337->dev, "Error setting hold register\n");
		return ret;
	}

	for (i = 0; i < nr_regs; i++) {
		ret = ar1337_write_reg(ar1337, address_low + i,
				       (u8)(value >> (i * 8)));
		if (ret) {
			dev_err(ar1337->dev, "Error writing buffered registers\n");
			return ret;
		}
	}

	ret = ar1337_write_reg8(ar1337, AR1337_REG_HOLD, 0x00);
	if (ret) {
		dev_err(ar1337->dev, "Error setting hold register\n");
		return ret;
	}

	return ret;
}
*/
static int ar1337_set_gain(struct ar1337 *ar1337, u32 value)
{
	int ret;
/*
	ret = ar1337_write_buffered_reg(ar1337, AR1337_GAIN_HIGH, 1,
					(u8)((value >> 8) & 0xFF));
	if (ret < 0)
		return ret;

	ret = ar1337_write_buffered_reg(ar1337, AR1337_GAIN_HIGH, 1,
					(u8)((value) & 0xFF));
	if (ret < 0)
		return ret;
*/
	return 0;
}

/* Stop streaming */
static int ar1337_stop_streaming(struct ar1337 *ar1337)
{
	return ar1337_write_reg8(ar1337, AR1337_REG_MODE, 0x00);
}

static int ar1337_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ar1337 *ar1337 = container_of(ctrl->handler,
					     struct ar1337, ctrls);
	int ret = 0;

	/* V4L2 controls values will be applied only when power is already up */
	if (!pm_runtime_get_if_in_use(ar1337->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_GAIN:
		ret = ar1337_set_gain(ar1337, ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(ar1337->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ar1337_ctrl_ops = {
	.s_ctrl = ar1337_set_ctrl,
};

static int ar1337_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	code->code = AR1337_DEFAULT_FORMAT;

	return 0;
}

static int ar1337_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct ar1337 *ar1337 = to_ar1337(sd);
	struct v4l2_mbus_framefmt *framefmt;

	mutex_lock(&ar1337->lock);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		framefmt = v4l2_subdev_get_try_format(&ar1337->sd, cfg,
						      fmt->pad);
	else
		framefmt = &ar1337->current_format;

	fmt->format = *framefmt;

	mutex_unlock(&ar1337->lock);

	return 0;
}

static const struct ar1337_mode *
ar1337_find_nearest_mode(unsigned int width, unsigned int height)
{
	unsigned int max_dist_match = (unsigned int) - 1;
	int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(ar1337_modes); i++) {
		unsigned int dist = min(width, ar1337_modes[i].width)
				* min(height, ar1337_modes[i].height);
		dist = ar1337_modes[i].width * ar1337_modes[i].height +
			width * height - 2 * dist;
		if (dist < max_dist_match) {
			n = i;
			max_dist_match = dist;
		}
	}

	return &ar1337_modes[n];
}

static int ar1337_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct ar1337 *ar1337 = to_ar1337(sd);
	const struct ar1337_mode *mode;
	struct v4l2_mbus_framefmt *format;
	int ret = 0;

	mutex_lock(&ar1337->lock);

	mode = ar1337_find_nearest_mode(fmt->format.width, fmt->format.height);

	fmt->format.width = mode->width;
	fmt->format.height = mode->height;

	fmt->format.code = AR1337_DEFAULT_FORMAT;
	fmt->format.field = V4L2_FIELD_NONE;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		format = v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
	} else {
		format = &ar1337->current_format;
		__v4l2_ctrl_s_ctrl_int64(ar1337->pixel_rate, mode->pixel_rate);

		ar1337->current_mode = mode;
	}

	*format = fmt->format;

	mutex_unlock(&ar1337->lock);

	return ret;
}

static int ar1337_entity_init_cfg(struct v4l2_subdev *subdev,
				  struct v4l2_subdev_pad_config *cfg)
{
	struct v4l2_subdev_format fmt = { 0 };

	fmt.which = cfg ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt.format.width = 2104;
	fmt.format.height = 1560;

	ar1337_set_fmt(subdev, cfg, &fmt);

	return 0;
}

/* Start streaming */
static int ar1337_start_streaming(struct ar1337 *ar1337)
{
	int ret;

	/* Set init register settings */
	ret = ar1337_set_register_array(ar1337, ar1337_global_init_settings,
				ARRAY_SIZE(ar1337_global_init_settings));
	if (ret < 0) {
		dev_err(ar1337->dev, "Could not set init registers\n");
		return ret;
	}

	/* Apply default values of current mode */
	ret = ar1337_set_register_array(ar1337, ar1337->current_mode->data,
					ar1337->current_mode->data_size);
	if (ret < 0) {
		dev_err(ar1337->dev, "Could not set current mode\n");
		return ret;
	}

	/* Apply customized values from user */
	ret = v4l2_ctrl_handler_setup(ar1337->sd.ctrl_handler);
	if (ret) {
		dev_err(ar1337->dev, "Could not sync v4l2 controls\n");
		return ret;
	}

	/* Start streaming */
	return ar1337_write_reg8(ar1337, AR1337_REG_MODE, 0x01);
}

static int ar1337_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct ar1337 *ar1337 = to_ar1337(sd);
	int ret;

	if (enable) {
		ret = pm_runtime_get_sync(ar1337->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(ar1337->dev);
			return ret;
		}

		ret = ar1337_start_streaming(ar1337);
		if (ret) {
			dev_err(ar1337->dev, "Start stream failed\n");
			pm_runtime_put(ar1337->dev);
			return ret;
		}

		u8 val;
		ar1337_read_reg8(ar1337, AR1337_REG_MODE, &val);
		pr_info("AR1337_REG_MODE: 0x%x\n", val);
	} else {
		ar1337_stop_streaming(ar1337);
		pm_runtime_put(ar1337->dev);
	}

	return 0;
}

static int ar1337_get_regulators(struct device *dev, struct ar1337 *ar1337)
{
	unsigned int i;

	for (i = 0; i < AR1337_NUM_SUPPLIES; i++)
		ar1337->supplies[i].supply = ar1337_supply_name[i];

	return devm_regulator_bulk_get(dev, AR1337_NUM_SUPPLIES,
				       ar1337->supplies);
}

static int ar1337_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ar1337 *ar1337 = to_ar1337(sd);
	int ret;

	ret = regulator_bulk_enable(AR1337_NUM_SUPPLIES, ar1337->supplies);
	if (ret) {
		dev_err(ar1337->dev, "Failed to enable regulators\n");
		return ret;
	}

	ret = clk_prepare_enable(ar1337->xclk);
	if (ret) {
		dev_err(ar1337->dev, "Failed to enable clock\n");
		return ret;
	}

	usleep_range(10000, 20000);

	gpiod_set_value_cansleep(ar1337->pwdn_gpio, 0);

	usleep_range(10000, 20000);

	/* Reset the sensor */
	ret = ar1337_write_reg8(ar1337, AR1337_REG_SOFT_RESET, 0x01);
	if (ret < 0)
		return ret;

	usleep_range(100000, 150000);

	return 0;
}

static int ar1337_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ar1337 *ar1337 = to_ar1337(sd);

	clk_disable_unprepare(ar1337->xclk);
	gpiod_set_value_cansleep(ar1337->pwdn_gpio, 0);
	regulator_bulk_disable(AR1337_NUM_SUPPLIES, ar1337->supplies);

	return 0;
}

static const struct dev_pm_ops ar1337_pm_ops = {
	SET_RUNTIME_PM_OPS(ar1337_power_on, ar1337_power_off, NULL)
};

static const struct v4l2_subdev_video_ops ar1337_video_ops = {
	.s_stream = ar1337_set_stream,
};

static const struct v4l2_subdev_pad_ops ar1337_pad_ops = {
	.init_cfg = ar1337_entity_init_cfg,
	.enum_mbus_code = ar1337_enum_mbus_code,
	.get_fmt = ar1337_get_fmt,
	.set_fmt = ar1337_set_fmt,
};

static const struct v4l2_subdev_ops ar1337_subdev_ops = {
	.video = &ar1337_video_ops,
	.pad = &ar1337_pad_ops,
};

static const struct media_entity_operations ar1337_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int ar1337_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	struct ar1337 *ar1337;
	u32 xclk_freq;
	int ret;
	u16 val = 0;

	ar1337 = devm_kzalloc(dev, sizeof(*ar1337), GFP_KERNEL);
	if (!ar1337)
		return -ENOMEM;

	ar1337->i2c_client = client;
	ar1337->dev = dev;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "Endpoint node not found\n");
		return -EINVAL;
	}

	ar1337->ep = v4l2_fwnode_endpoint_alloc_parse(endpoint);
	fwnode_handle_put(endpoint);
	if (IS_ERR(ar1337->ep)) {
		dev_err(dev, "Parsing endpoint node failed\n");
		ret = PTR_ERR(ar1337->ep);
		goto free_err;
	}

	/* Only CSI2 is supported for now */
	if (ar1337->ep->bus_type != V4L2_MBUS_CSI2) {
		dev_err(dev, "Unsupported bus type, should be CSI2\n");
		ret = -EINVAL;
		goto free_err;
	}

	/* Set default mode to max resolution */
	ar1337->current_mode = &ar1337_modes[0];

	/* get system clock (xclk) */
	ar1337->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(ar1337->xclk)) {
		dev_err(dev, "Could not get xclk");
		ret = PTR_ERR(ar1337->xclk);
		goto free_err;
	}

	ret = fwnode_property_read_u32(dev_fwnode(dev), "clock-frequency",
				       &xclk_freq);
	if (ret) {
		dev_err(dev, "Could not get xclk frequency\n");
		goto free_err;
	}

	/* external clock must be 24 MHz */
	if (xclk_freq != 24000000) {
		dev_err(dev, "External clock frequency %u is not supported\n",
			xclk_freq);
		ret = -EINVAL;
		goto free_err;
	}

	ret = clk_set_rate(ar1337->xclk, xclk_freq);
	if (ret) {
		dev_err(dev, "Could not set xclk frequency\n");
		goto free_err;
	}

	ret = ar1337_get_regulators(dev, ar1337);
	if (ret < 0) {
		dev_err(dev, "Cannot get regulators\n");
		goto free_err;
	}

	ar1337->pwdn_gpio = devm_gpiod_get_optional(dev, "powerdown",
						    GPIOD_OUT_HIGH);
	if (IS_ERR(ar1337->pwdn_gpio)) {
		dev_err(dev, "Cannot get powerdown gpio\n");
		ret = PTR_ERR(ar1337->pwdn_gpio);
		goto free_err;
	}

	mutex_init(&ar1337->lock);

	v4l2_ctrl_handler_init(&ar1337->ctrls, 2);

	v4l2_ctrl_new_std(&ar1337->ctrls, &ar1337_ctrl_ops,
			  V4L2_CID_GAIN, 0, 72, 1, 0);

	ar1337->pixel_rate = v4l2_ctrl_new_std(&ar1337->ctrls, &ar1337_ctrl_ops,
					       V4L2_CID_PIXEL_RATE, 1,
					       INT_MAX, 1,
					       ar1337_modes[0].pixel_rate);

	ar1337->sd.ctrl_handler = &ar1337->ctrls;

	if (ar1337->ctrls.error) {
		dev_err(dev, "Control initialization error %d\n",
			ar1337->ctrls.error);
		ret = ar1337->ctrls.error;
		goto free_ctrl;
	}

	v4l2_i2c_subdev_init(&ar1337->sd, client, &ar1337_subdev_ops);
	ar1337->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ar1337->sd.dev = &client->dev;
	ar1337->sd.entity.ops = &ar1337_subdev_entity_ops;
	ar1337->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ar1337->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&ar1337->sd.entity, 1, &ar1337->pad);
	if (ret < 0) {
		dev_err(dev, "Could not register media entity\n");
		goto free_ctrl;
	}

	ret = v4l2_async_register_subdev(&ar1337->sd);
	if (ret < 0) {
		dev_err(dev, "Could not register v4l2 device\n");
		goto free_entity;
	}

	ret = ar1337_power_on(dev);
	if (ret < 0) {
		dev_err(dev, "Could not power on the device\n");
		goto free_entity;
	}

	/* Read CHIP-ID */
	ret = ar1337_read_reg16(ar1337, AR1337_REG_CHIP_ID, &val);
	if (ret < 0) {
		dev_err(dev, "Could not read chip id\n");
		goto free_entity;
	}

	if (val != AR1337_CHIP_ID) {
		dev_err(dev, "Chip ID not valid: 0x%x\n", val);
		goto free_entity;
	}

	dev_info(dev, "AR1337 detected at address 0x%02x\n", client->addr);

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	v4l2_fwnode_endpoint_free(ar1337->ep);

	return 0;

free_entity:
	media_entity_cleanup(&ar1337->sd.entity);
free_ctrl:
	v4l2_ctrl_handler_free(&ar1337->ctrls);
	mutex_destroy(&ar1337->lock);
free_err:
	v4l2_fwnode_endpoint_free(ar1337->ep);

	return ret;
}

static int ar1337_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ar1337 *ar1337 = to_ar1337(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	mutex_destroy(&ar1337->lock);

	pm_runtime_disable(ar1337->dev);
	if (!pm_runtime_status_suspended(ar1337->dev))
		ar1337_power_off(ar1337->dev);
	pm_runtime_set_suspended(ar1337->dev);

	return 0;
}

static const struct of_device_id ar1337_of_match[] = {
	{ .compatible = "onsemi,ar1337" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ar1337_of_match);

static struct i2c_driver ar1337_i2c_driver = {
	.probe_new  = ar1337_probe,
	.remove = ar1337_remove,
	.driver = {
		.name  = "ar1337",
		.pm = &ar1337_pm_ops,
		.of_match_table = ar1337_of_match,
	},
};

module_i2c_driver(ar1337_i2c_driver);

MODULE_DESCRIPTION("Onsemi AR1337 CMOS Image Sensor Driver");
MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>");
MODULE_LICENSE("GPL v2");
