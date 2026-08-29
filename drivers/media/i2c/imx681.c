// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony IMX681 sensor driver (Surface Pro 11 front camera).
 *
 * The SP11 mode is an independently written representation of register
 * transactions observed while operating the author's own camera hardware.
 * Single supported mode:
 * 3840x2640 RAW10 at ~30 fps over ONE C-PHY TRIO at 2406 Msym/s
 * (CSI_SIGNALING_MODE 0x0111=3). This tree carries experimental
 * three-phase qcom camss support.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "imx681-tables.h"

#define IMX681_REG_MODEL_ID	CCI_REG16(0x0016)
#define IMX681_MODEL_ID		0x0681
#define IMX681_REG_MODE_SELECT	CCI_REG8(0x0100)
#define IMX681_MODE_STREAMING	1
#define IMX681_MODE_STANDBY	0
#define IMX681_REG_GROUP_HOLD	CCI_REG8(0x0104)
#define IMX681_REG_EXPOSURE	CCI_REG24(0x0229)
#define IMX681_REG_ANALOG_GAIN	CCI_REG16(0x0204)
#define IMX681_REG_DIGITAL_GAIN	CCI_REG16(0x020e)
#define IMX681_REG_FRAME_LENGTH	CCI_REG16(0x0340)

#define IMX681_WIDTH		3840
#define IMX681_HEIGHT		2640
#define IMX681_LINE_LENGTH	6752
#define IMX681_FRAME_LENGTH_DEF	2708
#define IMX681_FRAME_LENGTH_MAX	0xffff
#define IMX681_EXPOSURE_MARGIN	48
#define IMX681_EXPOSURE_DEF	1600
#define IMX681_AGAIN_MAX	960	/* gain = 1024 / (1024 - code) */
#define IMX681_DGAIN_DEF	0x0100
#define IMX681_PIXEL_RATE	548571428
/* V4L2 C-PHY link frequency is half the 2406-Msym/s symbol rate. */
#define IMX681_LINK_FREQ	1203000000
#define IMX681_MBUS_CODE	MEDIA_BUS_FMT_SRGGB10_1X10

#define IMX681_XCLR_MIN_DELAY_US	6200
#define IMX681_XCLR_DELAY_RANGE_US	1000

static const char * const imx681_supply_name[] = {
	"dovdd",	/* IO 1.8 V (PM8010 LDO3_M) */
	"avdd",		/* analog 2.8 V (PM8550 LDO7_B) */
};

static const s64 imx681_link_freq_menu[] = {
	IMX681_LINK_FREQ,
};

struct imx681 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *xclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx681_supply_name)];

	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *vblank_ctrl;
	struct v4l2_ctrl *expo_ctrl;
	struct v4l2_ctrl *again_ctrl;
	struct v4l2_ctrl *dgain_ctrl;
};

static inline struct imx681 *to_imx681(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx681, sd);
}

static int imx681_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx681 *sensor = to_imx681(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(imx681_supply_name),
				    sensor->supplies);
	if (ret) {
		dev_err(dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(sensor->xclk);
	if (ret) {
		dev_err(dev, "failed to enable xclk: %d\n", ret);
		goto disable_reg;
	}

	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(IMX681_XCLR_MIN_DELAY_US,
		     IMX681_XCLR_MIN_DELAY_US + IMX681_XCLR_DELAY_RANGE_US);
	return 0;

disable_reg:
	regulator_bulk_disable(ARRAY_SIZE(imx681_supply_name),
			       sensor->supplies);
	return ret;
}

static int imx681_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx681 *sensor = to_imx681(sd);

	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	clk_disable_unprepare(sensor->xclk);
	regulator_bulk_disable(ARRAY_SIZE(imx681_supply_name),
			       sensor->supplies);
	return 0;
}

static int imx681_identify(struct imx681 *sensor)
{
	u64 id = 0;
	int ret;

	ret = cci_read(sensor->regmap, IMX681_REG_MODEL_ID, &id, NULL);
	if (ret) {
		dev_err(sensor->dev, "model ID read failed: %d\n", ret);
		return ret;
	}

	dev_info(sensor->dev, "detected model id 0x%04llx%s\n", id,
		 id == IMX681_MODEL_ID ? " (IMX681)" : " (UNEXPECTED)");

	return id == IMX681_MODEL_ID ? 0 : -ENODEV;
}

/* ----------------------------------------------------------- controls */

static int imx681_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx681 *sensor =
		container_of(ctrl->handler, struct imx681, ctrl_handler);
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		unsigned int expo_max = IMX681_HEIGHT + ctrl->val -
					IMX681_EXPOSURE_MARGIN;

		ret = __v4l2_ctrl_modify_range(sensor->expo_ctrl,
					       sensor->expo_ctrl->minimum,
					       expo_max, 1,
					       min_t(int, IMX681_EXPOSURE_DEF,
						     expo_max));
		if (ret)
			return ret;
	}

	if (!pm_runtime_get_if_in_use(sensor->dev))
		return 0;

	cci_write(sensor->regmap, IMX681_REG_GROUP_HOLD, 1, &ret);
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		cci_write(sensor->regmap, IMX681_REG_FRAME_LENGTH,
			  IMX681_HEIGHT + ctrl->val, &ret);
		break;
	case V4L2_CID_EXPOSURE:
		cci_write(sensor->regmap, IMX681_REG_EXPOSURE, ctrl->val,
			  &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(sensor->regmap, IMX681_REG_ANALOG_GAIN, ctrl->val,
			  &ret);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		cci_write(sensor->regmap, IMX681_REG_DIGITAL_GAIN, ctrl->val,
			  &ret);
		break;
	default:
		break;
	}
	cci_write(sensor->regmap, IMX681_REG_GROUP_HOLD, 0, &ret);

	pm_runtime_put_autosuspend(sensor->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx681_ctrl_ops = {
	.s_ctrl = imx681_s_ctrl,
};

static int imx681_init_controls(struct imx681 *sensor)
{
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;
	const struct v4l2_ctrl_ops *ops = &imx681_ctrl_ops;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	ret = v4l2_fwnode_device_parse(sensor->dev, &props);
	if (ret)
		return ret;

	/* v4l2_fwnode_device_properties can add two more controls. */
	ret = v4l2_ctrl_handler_init(hdl, 10);
	if (ret)
		return ret;

	ctrl = v4l2_ctrl_new_int_menu(hdl, ops, V4L2_CID_LINK_FREQ,
				      ARRAY_SIZE(imx681_link_freq_menu) - 1, 0,
				      imx681_link_freq_menu);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_PIXEL_RATE,
				 IMX681_PIXEL_RATE, IMX681_PIXEL_RATE, 1,
				 IMX681_PIXEL_RATE);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_HBLANK,
				 IMX681_LINE_LENGTH - IMX681_WIDTH,
				 IMX681_LINE_LENGTH - IMX681_WIDTH, 1,
				 IMX681_LINE_LENGTH - IMX681_WIDTH);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->vblank_ctrl =
		v4l2_ctrl_new_std(hdl, ops, V4L2_CID_VBLANK,
				  IMX681_EXPOSURE_MARGIN,
				  IMX681_FRAME_LENGTH_MAX - IMX681_HEIGHT, 1,
				  IMX681_FRAME_LENGTH_DEF - IMX681_HEIGHT);
	sensor->expo_ctrl =
		v4l2_ctrl_new_std(hdl, ops, V4L2_CID_EXPOSURE, 8,
				  IMX681_FRAME_LENGTH_DEF -
				  IMX681_EXPOSURE_MARGIN, 1,
				  IMX681_EXPOSURE_DEF);
	sensor->again_ctrl =
		v4l2_ctrl_new_std(hdl, ops, V4L2_CID_ANALOGUE_GAIN, 0,
				  IMX681_AGAIN_MAX, 1, 0);
	sensor->dgain_ctrl =
		v4l2_ctrl_new_std(hdl, ops, V4L2_CID_DIGITAL_GAIN, 0x0100,
				  0x0fff, 1, IMX681_DGAIN_DEF);

	v4l2_ctrl_new_fwnode_properties(hdl, ops, &props);

	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	sensor->sd.ctrl_handler = hdl;
	return 0;
}

/* ------------------------------------------------------------- format */

static void imx681_fill_fmt(struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = IMX681_MBUS_CODE;
	fmt->width = IMX681_WIDTH;
	fmt->height = IMX681_HEIGHT;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int imx681_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = IMX681_MBUS_CODE;
	return 0;
}

static int imx681_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != IMX681_MBUS_CODE)
		return -EINVAL;
	fse->min_width = IMX681_WIDTH;
	fse->max_width = IMX681_WIDTH;
	fse->min_height = IMX681_HEIGHT;
	fse->max_height = IMX681_HEIGHT;
	return 0;
}

static int imx681_set_pad_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *fmt)
{
	imx681_fill_fmt(&fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	return 0;
}

static int imx681_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, sel->pad);
		break;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = IMX681_WIDTH;
		sel->r.height = IMX681_HEIGHT;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int imx681_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	imx681_fill_fmt(v4l2_subdev_state_get_format(state, 0));
	*v4l2_subdev_state_get_crop(state, 0) = (struct v4l2_rect) {
		.width = IMX681_WIDTH,
		.height = IMX681_HEIGHT,
	};

	return 0;
}

/* ------------------------------------------------------------ streaming */

static int imx681_start_streaming(struct imx681 *sensor)
{
	int ret;

	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret < 0)
		return ret;

	ret = cci_multi_reg_write(sensor->regmap, imx681_global_init_table,
				  ARRAY_SIZE(imx681_global_init_table), NULL);
	if (!ret)
		ret = cci_multi_reg_write(sensor->regmap,
					  imx681_mode_3840x2640,
					  ARRAY_SIZE(imx681_mode_3840x2640),
					  NULL);
	if (!ret)
		ret = cci_write(sensor->regmap, IMX681_REG_FRAME_LENGTH,
				IMX681_HEIGHT + sensor->vblank_ctrl->val,
				NULL);
	if (ret)
		goto err_rpm_put;

	ret = __v4l2_ctrl_handler_setup(&sensor->ctrl_handler);
	if (ret)
		goto err_rpm_put;

	ret = cci_write(sensor->regmap, IMX681_REG_MODE_SELECT,
			IMX681_MODE_STREAMING, NULL);
	if (ret)
		goto err_rpm_put;

	dev_dbg(sensor->dev, "streaming started\n");
	return 0;

err_rpm_put:
	dev_err(sensor->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put(sensor->dev);
	return ret;
}

static int imx681_stop_streaming(struct imx681 *sensor)
{
	int ret;

	ret = cci_write(sensor->regmap, IMX681_REG_MODE_SELECT,
			IMX681_MODE_STANDBY, NULL);
	pm_runtime_put_autosuspend(sensor->dev);
	return ret;
}

static int imx681_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx681 *sensor = to_imx681(sd);

	return enable ? imx681_start_streaming(sensor) :
			imx681_stop_streaming(sensor);
}

static const struct v4l2_subdev_video_ops imx681_video_ops = {
	.s_stream = imx681_s_stream,
};

static const struct v4l2_subdev_pad_ops imx681_pad_ops = {
	.enum_mbus_code = imx681_enum_mbus_code,
	.enum_frame_size = imx681_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = imx681_set_pad_fmt,
	.get_selection = imx681_get_selection,
};

static const struct v4l2_subdev_ops imx681_subdev_ops = {
	.video = &imx681_video_ops,
	.pad = &imx681_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx681_internal_ops = {
	.init_state = imx681_init_state,
};

static int imx681_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct imx681 *sensor;
	unsigned int i;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	sensor->dev = dev;

	v4l2_i2c_subdev_init(&sensor->sd, client, &imx681_subdev_ops);
	sensor->sd.internal_ops = &imx681_internal_ops;

	for (i = 0; i < ARRAY_SIZE(imx681_supply_name); i++)
		sensor->supplies[i].supply = imx681_supply_name[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(imx681_supply_name),
				      sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	sensor->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk),
				     "failed to get xclk\n");

	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
				     "failed to get reset gpio\n");

	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(dev, PTR_ERR(sensor->regmap),
				     "failed to init regmap\n");

	ret = imx681_power_on(dev);
	if (ret)
		return ret;

	ret = imx681_identify(sensor);
	if (ret)
		goto err_power_off;

	ret = imx681_init_controls(sensor);
	if (ret)
		goto err_power_off;

	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto err_ctrls;

	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto err_entity;

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 2000);
	pm_runtime_use_autosuspend(dev);

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret) {
		dev_err(dev, "async subdev register failed: %d\n", ret);
		goto err_pm;
	}

	pm_runtime_put_autosuspend(dev);
	return 0;

err_pm:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	v4l2_subdev_cleanup(&sensor->sd);
err_entity:
	media_entity_cleanup(&sensor->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
err_power_off:
	imx681_power_off(dev);
	return ret;
}

static void imx681_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx681 *sensor = to_imx681(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx681_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static const struct dev_pm_ops imx681_pm_ops = {
	SET_RUNTIME_PM_OPS(imx681_power_off, imx681_power_on, NULL)
};

static const struct of_device_id imx681_dt_ids[] = {
	{ .compatible = "sony,imx681" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx681_dt_ids);

static struct i2c_driver imx681_i2c_driver = {
	.driver = {
		.name = "imx681",
		.of_match_table = imx681_dt_ids,
		.pm = &imx681_pm_ops,
	},
	.probe = imx681_probe,
	.remove = imx681_remove,
};

module_i2c_driver(imx681_i2c_driver);

MODULE_DESCRIPTION("Sony IMX681 sensor driver (Surface Pro 11)");
MODULE_LICENSE("GPL");
