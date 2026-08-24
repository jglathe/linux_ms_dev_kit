// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2023, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
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
		 * PA Volume limit REMOVED 2026-08-01. The control now exposes the
		 * full hardware range, 0..31 (-9 dB .. +37.5 dB, 1.5 dB steps).
		 *
		 * Upstream capped this at 6 (0 dB) "to reduce the risk of speaker
		 * damage until we have active speaker protection in place". Both
		 * the author and reviewer of that series said the cap should go
		 * once protection exists. On this machine it does, verified live:
		 * SP 0x4027 + SP_VI 0x4024 enabled with VI feedback at 8 kHz on
		 * both amps, R0/T0 byte-identical to Windows, 106/107 calibration
		 * frames accepted, 20 protection stages accepted at graph start.
		 *
		 * WARNING: protection limits excursion and coil temperature. It
		 * does NOT make arbitrary levels safe on 4 ohm micro-speakers.
		 * Sustained operation near the top of this range will damage them.
		 * The operating point is set independently in UCM (SP11-HiFi.conf),
		 * currently 24 (+27 dB). Do not confuse that PA control with the
		 * codec's separate PA_AUX setting when comparing deployments.
		 *
		 * Digital Volume stays capped at 81 (-3 dB) as upstream, and is the
		 * runtime knob for fine adjustment in 1 dB steps.
		 */
		snd_soc_limit_volume(card, "WSA WSA_RX0 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA WSA_RX1 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA2 WSA_RX0 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA2 WSA_RX1 Digital Volume", 81);
		break;
	case WSA_CODEC_DMA_TX_0:
	case WSA_CODEC_DMA_TX_1:
		return 0;
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
		rate->min = rate->max = 8000;
		channels->min = channels->max = 2;
		snd_mask_none(format);
		snd_mask_set_format(format, SNDRV_PCM_FORMAT_S32_LE);
		break;
	case WSA_CODEC_DMA_TX_1:
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

static int x1e80100_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct x1e80100_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	unsigned int channels = substream->runtime->channels;
	unsigned int rx_slot[4];
	unsigned int tx_slot[4];
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

	ret = qcom_snd_sdw_prepare(substream,
				   &data->stream_prepared[cpu_dai->id]);
	if (cpu_dai->id == WSA_CODEC_DMA_TX_0) {
		q6apm_sp11_set_vi_ready(ret == 0);
		dev_info(rtd->dev, "SP11 VI feedback %s\n",
			 ret ? "unavailable; protection will bypass" :
			       "ready on WSA_CODEC_DMA_TX_0");
		/*
		 * VI is a protection sidechain, not the render transport.
		 * Keep playback available if its SoundWire allocation fails;
		 * the frontend will explicitly bypass SP/SPVI before start.
		 */
		if (ret)
			return 0;
	}
	if (cpu_dai->id == WSA_CODEC_DMA_TX_1) {
		q6apm_sp11_set_cps_ready(ret == 0);
		dev_info(rtd->dev, "SP11 CPS feedback %s\n",
			 ret ? "unavailable; protection will bypass" :
			       "ready on WSA_CODEC_DMA_TX_1");
		if (ret)
			return 0;
	}

	return ret;
}

static int x1e80100_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct x1e80100_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	if (cpu_dai->id == WSA_CODEC_DMA_TX_0)
		q6apm_sp11_set_vi_ready(false);
	else if (cpu_dai->id == WSA_CODEC_DMA_TX_1)
		q6apm_sp11_set_cps_ready(false);

	return qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
}

static const struct snd_soc_ops x1e80100_be_ops = {
	.startup = qcom_snd_sdw_startup,
	.shutdown = qcom_snd_sdw_shutdown,
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
