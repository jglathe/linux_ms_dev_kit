// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Code shared between the main and auxiliary Qualcomm PMIC voltage ADCs
 * of type ADC5 Gen3.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iio/adc/qcom-adc5-gen3-common.h>

int adc5_gen3_read(struct adc5_device_data *adc, unsigned int sdam_index,
		   u16 offset, u8 *data, int len)
{
	return regmap_bulk_read(adc->regmap,
				adc->base[sdam_index].base_addr + offset,
				data, len);
}
EXPORT_SYMBOL(adc5_gen3_read);

int adc5_gen3_write(struct adc5_device_data *adc, unsigned int sdam_index,
		    u16 offset, u8 *data, int len)
{
	return regmap_bulk_write(adc->regmap,
				 adc->base[sdam_index].base_addr + offset,
				 data, len);
}
EXPORT_SYMBOL(adc5_gen3_write);

/*
 * Worst case delay from PBS in readying handshake bit
 * can be up to 15ms, when PBS is busy running other
 * simultaneous transactions, while in the best case, it is
 * already ready at this point. Assigning polling delay and
 * retry count accordingly.
 */

#define ADC5_GEN3_HS_DELAY_US			100
#define ADC5_GEN3_HS_RETRY_COUNT		150

int adc5_gen3_poll_wait_hs(struct adc5_device_data *adc,
			   unsigned int sdam_index)
{
	u8 conv_req = ADC5_GEN3_CONV_REQ_REQ;
	int ret, count;
	u8 status = 0;

	for (count = 0; count < ADC5_GEN3_HS_RETRY_COUNT; count++) {
		ret = adc5_gen3_read(adc, sdam_index, ADC5_GEN3_HS, &status, sizeof(status));
		if (ret)
			return ret;

		if (status == ADC5_GEN3_HS_READY) {
			ret = adc5_gen3_read(adc, sdam_index, ADC5_GEN3_CONV_REQ,
					     &conv_req, sizeof(conv_req));
			if (ret)
				return ret;

			if (!conv_req)
				return 0;
		}

		fsleep(ADC5_GEN3_HS_DELAY_US);
	}

	pr_err("Setting HS ready bit timed out, sdam_index:%d, status:%#x\n",
	       sdam_index, status);
	return -ETIMEDOUT;
}
EXPORT_SYMBOL(adc5_gen3_poll_wait_hs);

void adc5_gen3_update_dig_param(struct adc5_channel_common_prop *prop, u8 *data)
{
	/* Update calibration select and decimation ratio select */
	*data &= ~(ADC5_GEN3_DIG_PARAM_CAL_SEL_MASK | ADC5_GEN3_DIG_PARAM_DEC_RATIO_SEL_MASK);
	*data |= FIELD_PREP(ADC5_GEN3_DIG_PARAM_CAL_SEL_MASK, prop->cal_method);
	*data |= FIELD_PREP(ADC5_GEN3_DIG_PARAM_DEC_RATIO_SEL_MASK, prop->decimation);
}
EXPORT_SYMBOL(adc5_gen3_update_dig_param);

int adc5_gen3_status_clear(struct adc5_device_data *adc,
			   int sdam_index, u16 offset, u8 *val, int len)
{
	u8 value;
	int ret;

	ret = adc5_gen3_write(adc, sdam_index, offset, val, len);
	if (ret)
		return ret;

	/* To indicate conversion request is only to clear a status */
	value = 0;
	ret = adc5_gen3_write(adc, sdam_index, ADC5_GEN3_PERPH_CH, &value, sizeof(value));
	if (ret)
		return ret;

	value = ADC5_GEN3_CONV_REQ_REQ;
	return adc5_gen3_write(adc, sdam_index, ADC5_GEN3_CONV_REQ, &value, sizeof(value));
}
EXPORT_SYMBOL(adc5_gen3_status_clear);

MODULE_DESCRIPTION("Qualcomm ADC5 Gen3 common functionality");
MODULE_LICENSE("GPL");
