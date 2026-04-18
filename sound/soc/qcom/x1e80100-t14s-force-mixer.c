/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal in-tree wrapper for Lenovo ThinkPad T14s Gen 6 (X1E80100)
 * Forces the three DISPLAY_PORT_RX_* virtual mixers at probe so HDMI0/1/2
 * sinks appear reliably for the full T14s-HiFi UCM.
 *
 * DEBUG VERSION 4 – only the exact DAPM widget names from your topology
 * (DISPLAY_PORT_RX_X Audio Mixer) + full defer + extra logging
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <sound/soc.h>

static int t14s_force_mixer_probe(struct platform_device *pdev)
{
	struct device_node *sound_np;
	struct platform_device *sound_pdev;
	struct snd_soc_card *card;

	dev_info(&pdev->dev, "=== probe started (DEBUG 4) ===\n");

	/* Prefer being a child of &sound; fallback to global lookup */
	if (pdev->dev.of_node && pdev->dev.of_node->parent &&
	    of_device_is_compatible(pdev->dev.of_node->parent, "qcom,x1e80100-sndcard"))
		sound_np = of_node_get(pdev->dev.of_node->parent);
	else
		sound_np = of_find_compatible_node(NULL, NULL, "qcom,x1e80100-sndcard");

	if (!sound_np) {
		dev_info(&pdev->dev, "sound card node not found → EPROBE_DEFER\n");
		return -EPROBE_DEFER;
	}

	sound_pdev = of_find_device_by_node(sound_np);
	of_node_put(sound_np);

	if (!sound_pdev) {
		dev_info(&pdev->dev, "sound_pdev not found → EPROBE_DEFER\n");
		return -EPROBE_DEFER;
	}

	card = dev_get_drvdata(&sound_pdev->dev);
	put_device(&sound_pdev->dev);

	if (!card || !card->dapm.card) {
		dev_info(&pdev->dev, "card or dapm.card not ready → EPROBE_DEFER\n");
		return -EPROBE_DEFER;
	}

	dev_info(&pdev->dev, "main sound card ready, forcing exact topology DAPM widgets now\n");

	/* === ONLY the widget names that actually exist in the topology === */
	snd_soc_dapm_force_enable_pin(&card->dapm, "DISPLAY_PORT_RX_0 Audio Mixer");
	snd_soc_dapm_force_enable_pin(&card->dapm, "DISPLAY_PORT_RX_1 Audio Mixer");
	snd_soc_dapm_force_enable_pin(&card->dapm, "DISPLAY_PORT_RX_2 Audio Mixer");

	snd_soc_dapm_sync(&card->dapm);

	dev_info(&pdev->dev, "T14s Gen 6 DP widgets forced (topology names only) + DAPM sync\n");
	return 0;
}

static const struct of_device_id t14s_force_of_match[] = {
	{ .compatible = "lenovo,t14s-force-dp-mixer" },
	{ }
};
MODULE_DEVICE_TABLE(of, t14s_force_of_match);

static struct platform_driver t14s_force_mixer_driver = {
	.driver = {
		.name = "x1e80100-t14s-force-mixer",
		.of_match_table = t14s_force_of_match,
		.pm = &snd_soc_pm_ops,
	},
	.probe = t14s_force_mixer_probe,
};

module_platform_driver(t14s_force_mixer_driver);

MODULE_DESCRIPTION("Lenovo ThinkPad T14s Gen 6 - force virtual DP mixers (DEBUG 4)");
MODULE_LICENSE("GPL");
