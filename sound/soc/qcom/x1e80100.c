// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2023, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/soundwire/sdw.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

#include "common.h"
#include "qdsp6/q6afe.h"
#include "qdsp6/q6apm.h"
#include "qdsp6/q6dsp-common.h"
#include "sdw.h"

struct x1e80100_snd_cfg {
	const char *driver_name;
	const unsigned int *channels_map;
	int channels_num;
	bool protected_speaker_feedback;
};

struct x1e80100_snd_data {
	bool stream_prepared[AFE_PORT_MAX];
	struct snd_soc_card *card;
	const struct x1e80100_snd_cfg *cfg;
	struct snd_soc_jack jack;
	struct snd_soc_jack dp_jack[8];
	bool jack_setup;
};

static int x1e80100_snd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct x1e80100_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_jack *dp_jack = NULL;
	int dp_pcm_id = 0;

	switch (cpu_dai->id) {
	case WSA_CODEC_DMA_RX_0:
	case WSA_CODEC_DMA_RX_1:
		/*
		 * Set limit of -3 dB on Digital Volume and 0 dB on PA Volume
		 * to reduce the risk of speaker damage until we have active
		 * speaker protection in place.
		 */
		snd_soc_limit_volume(card, "WSA WSA_RX0 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA WSA_RX1 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA2 WSA_RX0 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA2 WSA_RX1 Digital Volume", 81);
		snd_soc_limit_volume(card, "SpkrLeft PA Volume", 6);
		snd_soc_limit_volume(card, "SpkrRight PA Volume", 6);
		snd_soc_limit_volume(card, "WooferLeft PA Volume", 6);
		snd_soc_limit_volume(card, "TweeterLeft PA Volume", 6);
		snd_soc_limit_volume(card, "WooferRight PA Volume", 6);
		snd_soc_limit_volume(card, "TweeterRight PA Volume", 6);
		break;
	case WSA_CODEC_DMA_TX_0:
	case WSA_CODEC_DMA_TX_1:
		if (data->cfg->protected_speaker_feedback)
			return 0;
		break;
	case DISPLAY_PORT_RX_0:
		dp_pcm_id = 0;
		dp_jack = &data->dp_jack[dp_pcm_id];
		break;
	case DISPLAY_PORT_RX_1 ... DISPLAY_PORT_RX_7:
		dp_pcm_id = cpu_dai->id - DISPLAY_PORT_RX_1 + 1;
		dp_jack = &data->dp_jack[dp_pcm_id];
		break;
	default:
		break;
	}

	if (dp_jack)
		return qcom_snd_dp_jack_setup(rtd, dp_jack, dp_pcm_id);

	return qcom_snd_wcd_jack_setup(rtd, &data->jack, &data->jack_setup);
}

static int x1e80100_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct x1e80100_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
							  SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *format = hw_param_mask(params,
					       SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	switch (cpu_dai->id) {
	case TX_CODEC_DMA_TX_0:
	case TX_CODEC_DMA_TX_1:
	case TX_CODEC_DMA_TX_2:
	case TX_CODEC_DMA_TX_3:
		channels->min = 1;
		break;
	case WSA_CODEC_DMA_TX_0:
		if (!data->cfg->protected_speaker_feedback)
			break;
		rate->min = 8000;
		rate->max = 8000;
		channels->min = 2;
		channels->max = 2;
		snd_mask_none(format);
		snd_mask_set_format(format, SNDRV_PCM_FORMAT_S32_LE);
		break;
	case WSA_CODEC_DMA_TX_1:
		if (!data->cfg->protected_speaker_feedback)
			break;
		rate->min = 24000;
		rate->max = 24000;
		channels->min = 2;
		channels->max = 2;
		snd_mask_none(format);
		snd_mask_set_format(format, SNDRV_PCM_FORMAT_S32_LE);
		break;
	default:
		break;
	}

	return 0;
}

static int x1e80100_snd_hw_map_channels(struct x1e80100_snd_data *data,
					unsigned int *ch_map, int num)
{
	if (data->cfg->channels_map) {
		for (int i = 0; i < data->cfg->channels_num; i++)
			ch_map[i] = data->cfg->channels_map[i];

		return 0;
	}

	switch (num) {
	case 1:
		ch_map[0] = PCM_CHANNEL_FC;
		break;
	case 2:
		ch_map[0] = PCM_CHANNEL_FL;
		ch_map[1] = PCM_CHANNEL_FR;
		break;
	case 3:
		ch_map[0] = PCM_CHANNEL_FL;
		ch_map[1] = PCM_CHANNEL_FR;
		ch_map[2] = PCM_CHANNEL_FC;
		break;
	case 4:
		ch_map[0] = PCM_CHANNEL_FL;
		ch_map[1] = PCM_CHANNEL_LB;
		ch_map[2] = PCM_CHANNEL_FR;
		ch_map[3] = PCM_CHANNEL_RB;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int x1e80100_protection_backend(unsigned int dai_id)
{
	switch (dai_id) {
	case WSA_CODEC_DMA_TX_0:
		return Q6APM_PROTECTION_BACKEND_VI;
	case WSA_CODEC_DMA_TX_1:
		return Q6APM_PROTECTION_BACKEND_CPS;
	default:
		return -EINVAL;
	}
}

static int x1e80100_set_protection_ready(struct snd_soc_pcm_runtime *rtd,
					 struct snd_soc_dai *cpu_dai,
					 int backend, bool ready)
{
	int ret;

	ret = q6apm_set_protection_backend_ready(cpu_dai->dev, cpu_dai->id,
						 backend, ready);
	if (ret && ret != -ENODEV)
		dev_warn(rtd->dev,
			 "failed to update protected backend readiness: %d\n", ret);

	return ret;
}

static int x1e80100_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct x1e80100_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct sdw_stream_runtime *sruntime;
	unsigned int channels = substream->runtime->channels;
	unsigned int rx_slot[4];
	unsigned int tx_slot[4];
	bool ready;
	int backend;
	int protection_ret;
	int ret;

	switch (cpu_dai->id) {
	case WSA_CODEC_DMA_RX_0:
	case WSA_CODEC_DMA_RX_1:
		ret = x1e80100_snd_hw_map_channels(data, rx_slot, channels);
		if (ret)
			return ret;

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
						  channels, rx_slot);
		if (ret)
			return ret;
		break;
	case WSA_CODEC_DMA_TX_0:
	case WSA_CODEC_DMA_TX_1:
		if (!data->cfg->protected_speaker_feedback)
			break;

		ret = x1e80100_snd_hw_map_channels(data, tx_slot, channels);
		if (ret)
			return ret;

		ret = snd_soc_dai_set_channel_map(cpu_dai, channels, tx_slot,
						  0, NULL);
		if (ret)
			return ret;
		break;
	default:
		break;
	}
	if (data->cfg->protected_speaker_feedback) {
		backend = x1e80100_protection_backend(cpu_dai->id);
		if (backend >= 0) {
			sruntime = qcom_snd_sdw_get_stream(substream);
			if (IS_ERR_OR_NULL(sruntime)) {
				data->stream_prepared[cpu_dai->id] = false;
				ret = x1e80100_set_protection_ready(rtd, cpu_dai,
								    backend, false);
				if (ret && ret != -ENODEV)
					return ret;
				dev_warn(rtd->dev,
					 "speaker-feedback backend %d has no SoundWire runtime; using bypass\n",
					 cpu_dai->id);
				return 0;
			}
		}
	}

	ret = qcom_snd_sdw_prepare(substream,
				   &data->stream_prepared[cpu_dai->id]);
	if (!data->cfg->protected_speaker_feedback)
		return ret;

	backend = x1e80100_protection_backend(cpu_dai->id);
	if (backend < 0)
		return ret;

	ready = !ret && data->stream_prepared[cpu_dai->id];
	protection_ret = x1e80100_set_protection_ready(rtd, cpu_dai, backend,
						       ready);
	if (!ready) {
		if (protection_ret)
			return ret ? ret : protection_ret;
		dev_warn(rtd->dev,
			 "speaker-feedback backend %d unavailable; using bypass\n",
			 cpu_dai->id);
		return 0;
	}
	if (protection_ret && protection_ret != -ENODEV) {
		qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
		return protection_ret;
	}

	return 0;
}

static int x1e80100_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct x1e80100_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int backend;
	int ret;

	if (data->cfg->protected_speaker_feedback) {
		backend = x1e80100_protection_backend(cpu_dai->id);
		if (backend >= 0) {
			ret = x1e80100_set_protection_ready(rtd, cpu_dai,
							    backend, false);
			if (ret && ret != -ENODEV)
				return ret;
		}
	}

	return qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
}

static void x1e80100_snd_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct x1e80100_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int backend;
	int ret;

	if (data->cfg->protected_speaker_feedback) {
		backend = x1e80100_protection_backend(cpu_dai->id);
		if (backend >= 0) {
			ret = x1e80100_set_protection_ready(rtd, cpu_dai,
							    backend, false);
			if (ret && ret != -ENODEV)
				return;
		}
	}

	qcom_snd_sdw_shutdown(substream);
}

static const struct snd_soc_ops x1e80100_be_ops = {
	.startup = qcom_snd_sdw_startup,
	.shutdown = x1e80100_snd_shutdown,
	.hw_free = x1e80100_snd_hw_free,
	.prepare = x1e80100_snd_prepare,
};

static void x1e80100_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->init = x1e80100_snd_init;
			link->be_hw_params_fixup = x1e80100_be_hw_params_fixup;
			link->ops = &x1e80100_be_ops;
		}
	}
}

static int x1e80100_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct x1e80100_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;
	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	card->owner = THIS_MODULE;
	card->dev = dev;
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);

	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	data->cfg = of_device_get_match_data(dev);

	card->driver_name = data->cfg->driver_name;
	x1e80100_add_be_ops(card);

	return devm_snd_soc_register_card(dev, card);
}

static const struct x1e80100_snd_cfg x1e80100_cfg = {
	.driver_name = "x1e80100",
};

static const struct x1e80100_snd_cfg denali_cfg = {
	.driver_name = "x1e80100",
	.protected_speaker_feedback = true,
};

static const struct x1e80100_snd_cfg glymur_cfg = {
	.driver_name = "glymur",
};

static const unsigned int right_left_4_channels_map[] = {
	PCM_CHANNEL_FR,
	PCM_CHANNEL_RB,
	PCM_CHANNEL_FL,
	PCM_CHANNEL_LB,
};

static const struct x1e80100_snd_cfg dell_xps13_9345_cfg = {
	.driver_name = "x1e80100",
	.channels_map = right_left_4_channels_map,
	.channels_num = ARRAY_SIZE(right_left_4_channels_map),
};

static const struct of_device_id snd_x1e80100_dt_match[] = {
	{ .compatible = "microsoft,denali-sndcard", .data = &denali_cfg, },
	{ .compatible = "qcom,x1e80100-sndcard", .data = &x1e80100_cfg, },
	{ .compatible = "dell,xps13-9345-sndcard", .data = &dell_xps13_9345_cfg, },
	{ .compatible = "qcom,glymur-sndcard", .data = &glymur_cfg, },
	{}
};
MODULE_DEVICE_TABLE(of, snd_x1e80100_dt_match);

static struct platform_driver snd_x1e80100_driver = {
	.probe  = x1e80100_platform_probe,
	.driver = {
		.name = "snd-x1e80100",
		.of_match_table = snd_x1e80100_dt_match,
	},
};
module_platform_driver(snd_x1e80100_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_AUTHOR("Krzysztof Kozlowski <krzysztof.kozlowski@linaro.org>");
MODULE_DESCRIPTION("Qualcomm X1E80100 ASoC Machine Driver");
MODULE_LICENSE("GPL");
