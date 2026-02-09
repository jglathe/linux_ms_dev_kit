// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/device/devres.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/iio/adc/qcom-adc5-gen3-common.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/thermal.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/unaligned.h>

#include "../thermal_hwmon.h"

struct device;
struct adc_tm5_gen3_chip;

/**
 * struct adc_tm5_gen3_channel_props - ADC_TM channel structure
 * @timer: time period of recurring TM measurement.
 * @tm_chan_index: TM channel number used (ranging from 1-7).
 * @sdam_index: SDAM on which this TM channel lies.
 * @common_props: structure with common  ADC channel properties.
 * @high_thr_en: TM high threshold crossing detection enabled.
 * @low_thr_en: TM low threshold crossing detection enabled.
 * @chip: ADC TM device.
 * @tzd: pointer to thermal device corresponding to TM channel.
 * @last_temp: last temperature that caused threshold violation,
 *	or a thermal TM channel.
 * @last_temp_set: indicates if last_temp is stored.
 */
struct adc_tm5_gen3_channel_props {
	unsigned int timer;
	unsigned int tm_chan_index;
	unsigned int sdam_index;
	struct adc5_channel_common_prop common_props;
	bool high_thr_en;
	bool low_thr_en;
	struct adc_tm5_gen3_chip *chip;
	struct thermal_zone_device *tzd;
	int last_temp;
	bool last_temp_set;
};

/**
 * struct adc_tm5_gen3_chip - ADC Thermal Monitoring device structure
 * @dev_data: Top-level ADC device data.
 * @chan_props: Array of ADC_TM channel structures.
 * @nchannels: number of TM channels allocated
 * @dev: SPMI ADC5 Gen3 device.
 * @tm_handler_work: handler for TM interrupt for threshold violation.
 */
struct adc_tm5_gen3_chip {
	struct adc5_device_data *dev_data;
	struct adc_tm5_gen3_channel_props *chan_props;
	unsigned int nchannels;
	struct device *dev;
	struct work_struct tm_handler_work;
};

DEFINE_GUARD(adc5_gen3, struct adc_tm5_gen3_chip *, adc5_gen3_mutex_lock(_T->dev),
	     adc5_gen3_mutex_unlock(_T->dev))

static int get_sdam_from_irq(struct adc_tm5_gen3_chip *adc_tm5, int irq)
{
	for (int i = 0; i < adc_tm5->dev_data->num_sdams; i++) {
		if (adc_tm5->dev_data->base[i].irq == irq)
			return i;
	}
	return -ENOENT;
}

static irqreturn_t adctm5_gen3_isr(int irq, void *dev_id)
{
	struct adc_tm5_gen3_chip *adc_tm5 = dev_id;
	int ret, sdam_num;
	u8 tm_status[2];
	u8 status, val;

	sdam_num = get_sdam_from_irq(adc_tm5, irq);
	if (sdam_num < 0) {
		dev_err(adc_tm5->dev, "adc irq %d not associated with an sdam\n",
			irq);
		return IRQ_HANDLED;
	}

	ret = adc5_gen3_read(adc_tm5->dev_data, sdam_num, ADC5_GEN3_STATUS1,
			     &status, sizeof(status));
	if (ret) {
		dev_err(adc_tm5->dev, "adc read status1 failed with %d\n", ret);
		return IRQ_HANDLED;
	}

	if (status & ADC5_GEN3_STATUS1_CONV_FAULT) {
		dev_err_ratelimited(adc_tm5->dev,
				    "Unexpected conversion fault, status:%#x\n",
				    status);
		val = ADC5_GEN3_CONV_ERR_CLR_REQ;
		adc5_gen3_status_clear(adc_tm5->dev_data, sdam_num,
				       ADC5_GEN3_CONV_ERR_CLR, &val, 1);
		return IRQ_HANDLED;
	}

	ret = adc5_gen3_read(adc_tm5->dev_data, sdam_num, ADC5_GEN3_TM_HIGH_STS,
			     tm_status, sizeof(tm_status));
	if (ret) {
		dev_err(adc_tm5->dev, "adc read TM status failed with %d\n", ret);
		return IRQ_HANDLED;
	}

	if (tm_status[0] || tm_status[1])
		schedule_work(&adc_tm5->tm_handler_work);

	dev_dbg(adc_tm5->dev, "Interrupt status:%#x, high:%#x, low:%#x\n",
		status, tm_status[0], tm_status[1]);

	return IRQ_HANDLED;
}

static int adc5_gen3_tm_status_check(struct adc_tm5_gen3_chip *adc_tm5,
				     int sdam_index, u8 *tm_status, u8 *buf)
{
	int ret;

	ret = adc5_gen3_read(adc_tm5->dev_data, sdam_index, ADC5_GEN3_TM_HIGH_STS,
			     tm_status, 2);
	if (ret) {
		dev_err(adc_tm5->dev, "adc read TM status failed with %d\n", ret);
		return ret;
	}

	ret = adc5_gen3_status_clear(adc_tm5->dev_data, sdam_index, ADC5_GEN3_TM_HIGH_STS_CLR,
				     tm_status, 2);
	if (ret) {
		dev_err(adc_tm5->dev, "adc status clear conv_req failed with %d\n",
			ret);
		return ret;
	}

	ret = adc5_gen3_read(adc_tm5->dev_data, sdam_index, ADC5_GEN3_CH_DATA0(0),
			     buf, 16);
	if (ret)
		dev_err(adc_tm5->dev, "adc read data failed with %d\n", ret);

	return ret;
}

static void tm_handler_work(struct work_struct *work)
{
	struct adc_tm5_gen3_chip *adc_tm5 = container_of(work, struct adc_tm5_gen3_chip,
							 tm_handler_work);
	int sdam_index = -1;
	u8 tm_status[2] = { };
	u8 buf[16] = { };

	for (int i = 0; i < adc_tm5->nchannels; i++) {
		struct adc_tm5_gen3_channel_props *chan_prop = &adc_tm5->chan_props[i];
		int offset = chan_prop->tm_chan_index;
		bool upper_set, lower_set;
		int ret, temp;
		u16 code;

		scoped_guard(adc5_gen3, adc_tm5) {
			if (chan_prop->sdam_index != sdam_index) {
				sdam_index = chan_prop->sdam_index;
				ret = adc5_gen3_tm_status_check(adc_tm5, sdam_index,
								tm_status, buf);
				if (ret)
					return;
			}

			upper_set = ((tm_status[0] & BIT(offset)) && chan_prop->high_thr_en);
			lower_set = ((tm_status[1] & BIT(offset)) && chan_prop->low_thr_en);
		}

		if (!(upper_set || lower_set))
			continue;

		code = get_unaligned_le16(&buf[2 * offset]);
		dev_dbg(adc_tm5->dev, "ADC_TM threshold code:%#x\n", code);

		ret = adc5_gen3_therm_code_to_temp(adc_tm5->dev,
						   &chan_prop->common_props,
						   code, &temp);
		if (ret) {
			dev_err(adc_tm5->dev,
				"Invalid temperature reading, ret = %d, code=%#x\n",
				ret, code);
			continue;
		}

		chan_prop->last_temp = temp;
		chan_prop->last_temp_set = true;
		thermal_zone_device_update(chan_prop->tzd, THERMAL_TRIP_VIOLATED);
	}
}

static int adc_tm5_gen3_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct adc_tm5_gen3_channel_props *prop = thermal_zone_device_priv(tz);
	struct adc_tm5_gen3_chip *adc_tm5;

	if (!prop || !prop->chip)
		return -EINVAL;

	adc_tm5 = prop->chip;

	if (prop->last_temp_set) {
		pr_debug("last_temp: %d\n", prop->last_temp);
		prop->last_temp_set = false;
		*temp = prop->last_temp;
		return 0;
	}

	return adc5_gen3_get_scaled_reading(adc_tm5->dev, &prop->common_props,
					    temp);
}

static int adc_tm5_gen3_disable_channel(struct adc_tm5_gen3_channel_props *prop)
{
	struct adc_tm5_gen3_chip *adc_tm5 = prop->chip;
	int ret;
	u8 val;

	prop->high_thr_en = false;
	prop->low_thr_en = false;

	ret = adc5_gen3_poll_wait_hs(adc_tm5->dev_data, prop->sdam_index);
	if (ret)
		return ret;

	val = BIT(prop->tm_chan_index);
	ret = adc5_gen3_write(adc_tm5->dev_data, prop->sdam_index,
			      ADC5_GEN3_TM_HIGH_STS_CLR, &val, sizeof(val));
	if (ret)
		return ret;

	val = MEAS_INT_DISABLE;
	ret = adc5_gen3_write(adc_tm5->dev_data, prop->sdam_index,
			      ADC5_GEN3_TIMER_SEL, &val, sizeof(val));
	if (ret)
		return ret;

	/* To indicate there is an actual conversion request */
	val = ADC5_GEN3_CHAN_CONV_REQ | prop->tm_chan_index;
	ret = adc5_gen3_write(adc_tm5->dev_data, prop->sdam_index,
			      ADC5_GEN3_PERPH_CH, &val, sizeof(val));
	if (ret)
		return ret;

	val = ADC5_GEN3_CONV_REQ_REQ;
	return adc5_gen3_write(adc_tm5->dev_data, prop->sdam_index,
			       ADC5_GEN3_CONV_REQ, &val, sizeof(val));
}

#define ADC_TM5_GEN3_CONFIG_REGS 12

static int adc_tm5_gen3_configure(struct adc_tm5_gen3_channel_props *prop,
				  int low_temp, int high_temp)
{
	struct adc_tm5_gen3_chip *adc_tm5 = prop->chip;
	u8 buf[ADC_TM5_GEN3_CONFIG_REGS];
	u8 conv_req;
	u16 adc_code;
	int ret;

	ret = adc5_gen3_poll_wait_hs(adc_tm5->dev_data, prop->sdam_index);
	if (ret < 0)
		return ret;

	ret = adc5_gen3_read(adc_tm5->dev_data, prop->sdam_index,
			     ADC5_GEN3_SID, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	/* Write SID */
	buf[0] = FIELD_PREP(ADC5_GEN3_SID_MASK, prop->common_props.sid);

	/* Select TM channel and indicate there is an actual conversion request */
	buf[1] = ADC5_GEN3_CHAN_CONV_REQ | prop->tm_chan_index;

	buf[2] = prop->timer;

	/* Digital param selection */
	adc5_gen3_update_dig_param(&prop->common_props, &buf[3]);

	/* Update fast average sample value */
	buf[4] &= ~ADC5_GEN3_FAST_AVG_CTL_SAMPLES_MASK;
	buf[4] |= prop->common_props.avg_samples | ADC5_GEN3_FAST_AVG_CTL_EN;

	/* Select ADC channel */
	buf[5] = prop->common_props.channel;

	/* Select HW settle delay for channel */
	buf[6] = FIELD_PREP(ADC5_GEN3_HW_SETTLE_DELAY_MASK,
			    prop->common_props.hw_settle_time_us);

	/* High temperature corresponds to low voltage threshold */
	prop->low_thr_en = (high_temp != INT_MAX);
	if (prop->low_thr_en) {
		adc_code = qcom_adc_tm5_gen2_temp_res_scale(high_temp);
		put_unaligned_le16(adc_code, &buf[8]);
	}

	/* Low temperature corresponds to high voltage threshold */
	prop->high_thr_en = (low_temp != -INT_MAX);
	if (prop->high_thr_en) {
		adc_code = qcom_adc_tm5_gen2_temp_res_scale(low_temp);
		put_unaligned_le16(adc_code, &buf[10]);
	}

	buf[7] = 0;
	if (prop->high_thr_en)
		buf[7] |= ADC5_GEN3_HIGH_THR_INT_EN;
	if (prop->low_thr_en)
		buf[7] |= ADC5_GEN3_LOW_THR_INT_EN;

	ret = adc5_gen3_write(adc_tm5->dev_data, prop->sdam_index, ADC5_GEN3_SID,
			      buf, sizeof(buf));
	if (ret < 0)
		return ret;

	conv_req = ADC5_GEN3_CONV_REQ_REQ;
	return adc5_gen3_write(adc_tm5->dev_data, prop->sdam_index,
			       ADC5_GEN3_CONV_REQ, &conv_req, sizeof(conv_req));
}

static int adc_tm5_gen3_set_trip_temp(struct thermal_zone_device *tz,
				      int low_temp, int high_temp)
{
	struct adc_tm5_gen3_channel_props *prop = thermal_zone_device_priv(tz);
	struct adc_tm5_gen3_chip *adc_tm5;

	if (!prop || !prop->chip)
		return -EINVAL;

	adc_tm5 = prop->chip;

	dev_dbg(adc_tm5->dev, "channel:%s, low_temp(mdegC):%d, high_temp(mdegC):%d\n",
		prop->common_props.label, low_temp, high_temp);

	guard(adc5_gen3)(adc_tm5);
	if (high_temp == INT_MAX && low_temp == -INT_MAX)
		return adc_tm5_gen3_disable_channel(prop);

	return adc_tm5_gen3_configure(prop, low_temp, high_temp);
}

static const struct thermal_zone_device_ops adc_tm_ops = {
	.get_temp = adc_tm5_gen3_get_temp,
	.set_trips = adc_tm5_gen3_set_trip_temp,
};

static int adc_tm5_register_tzd(struct adc_tm5_gen3_chip *adc_tm5)
{
	struct thermal_zone_device *tzd;
	unsigned int channel;
	int ret;

	for (int i = 0; i < adc_tm5->nchannels; i++) {
		channel = ADC5_GEN3_V_CHAN(adc_tm5->chan_props[i].common_props);
		tzd = devm_thermal_of_zone_register(adc_tm5->dev, channel,
						    &adc_tm5->chan_props[i],
						    &adc_tm_ops);
		if (IS_ERR(tzd)) {
			if (PTR_ERR(tzd) == -ENODEV) {
				dev_info(adc_tm5->dev,
					 "thermal sensor on channel %d is not used\n",
					 channel);
				continue;
			}
			return dev_err_probe(adc_tm5->dev, PTR_ERR(tzd),
					     "Error registering TZ zone:%ld for channel:%d\n",
					     PTR_ERR(tzd), channel);
		}
		adc_tm5->chan_props[i].tzd = tzd;
		ret = devm_thermal_add_hwmon_sysfs(adc_tm5->dev, tzd);
		if (ret)
			return ret;
	}
	return 0;
}

static void adc5_gen3_clear_work(void *data)
{
	struct adc_tm5_gen3_chip *adc_tm5 = data;

	cancel_work_sync(&adc_tm5->tm_handler_work);
}

static void adc5_gen3_disable(void *data)
{
	struct adc_tm5_gen3_chip *adc_tm5 = data;

	guard(adc5_gen3)(adc_tm5);
	/* Disable all available TM channels */
	for (int i = 0; i < adc_tm5->nchannels; i++)
		adc_tm5_gen3_disable_channel(&adc_tm5->chan_props[i]);
}

static void adctm_event_handler(struct auxiliary_device *adev)
{
	struct adc_tm5_gen3_chip *adc_tm5 = auxiliary_get_drvdata(adev);

	schedule_work(&adc_tm5->tm_handler_work);
}

static int adc_tm5_probe(struct auxiliary_device *aux_dev,
			 const struct auxiliary_device_id *id)
{
	struct adc_tm5_gen3_chip *adc_tm5;
	struct tm5_aux_dev_wrapper *aux_dev_wrapper;
	struct device *dev = &aux_dev->dev;
	int ret;

	adc_tm5 = devm_kzalloc(dev, sizeof(*adc_tm5), GFP_KERNEL);
	if (!adc_tm5)
		return -ENOMEM;

	aux_dev_wrapper = container_of(aux_dev, struct tm5_aux_dev_wrapper,
				       aux_dev);

	adc_tm5->dev = dev;
	adc_tm5->dev_data = aux_dev_wrapper->dev_data;
	adc_tm5->nchannels = aux_dev_wrapper->n_tm_channels;
	adc_tm5->chan_props = devm_kcalloc(dev, aux_dev_wrapper->n_tm_channels,
					   sizeof(*adc_tm5->chan_props), GFP_KERNEL);
	if (!adc_tm5->chan_props)
		return -ENOMEM;

	for (int i = 0; i < adc_tm5->nchannels; i++) {
		adc_tm5->chan_props[i].common_props = aux_dev_wrapper->tm_props[i];
		adc_tm5->chan_props[i].timer = MEAS_INT_1S;
		adc_tm5->chan_props[i].sdam_index = (i + 1) / 8;
		adc_tm5->chan_props[i].tm_chan_index = (i + 1) % 8;
		adc_tm5->chan_props[i].chip = adc_tm5;
	}

	INIT_WORK(&adc_tm5->tm_handler_work, tm_handler_work);

	/*
	 * Skipping first SDAM IRQ as it is requested in parent driver.
	 * If there is a TM violation on that IRQ, the parent driver calls
	 * the notifier (adctm_event_handler) exposed from this driver to handle it.
	 */
	for (int i = 1; i < adc_tm5->dev_data->num_sdams; i++) {
		ret = devm_request_threaded_irq(dev,
						adc_tm5->dev_data->base[i].irq,
						NULL, adctm5_gen3_isr, IRQF_ONESHOT,
						adc_tm5->dev_data->base[i].irq_name,
						adc_tm5);
		if (ret < 0)
			return ret;
	}

	/*
	 * This drvdata is only used in the function (adctm_event_handler)
	 * called by parent ADC driver in case of TM violation on the first SDAM.
	 */
	auxiliary_set_drvdata(aux_dev, adc_tm5);

	adc5_gen3_register_tm_event_notifier(dev, adctm_event_handler);

	/*
	 * This is to cancel any instances of tm_handler_work scheduled by
	 * TM interrupt, at the time of module removal.
	 */
	ret = devm_add_action(dev, adc5_gen3_clear_work, adc_tm5);
	if (ret)
		return ret;

	ret = adc_tm5_register_tzd(adc_tm5);
	if (ret)
		return ret;

	/* This is to disable all ADC_TM channels in case of probe failure. */

	return devm_add_action(dev, adc5_gen3_disable, adc_tm5);
}

static const struct auxiliary_device_id adctm5_auxiliary_id_table[] = {
	{ .name = "qcom_spmi_adc5_gen3.adc5_tm_gen3", },
	{ }
};

MODULE_DEVICE_TABLE(auxiliary, adctm5_auxiliary_id_table);

static struct auxiliary_driver adctm5gen3_auxiliary_driver = {
	.id_table = adctm5_auxiliary_id_table,
	.probe = adc_tm5_probe,
};

module_auxiliary_driver(adctm5gen3_auxiliary_driver);

MODULE_DESCRIPTION("SPMI PMIC Thermal Monitor ADC driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("QCOM_SPMI_ADC5_GEN3");
