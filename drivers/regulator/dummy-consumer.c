// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Aleksandrs Vinarskis
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>

struct dummy_consumer_data {
	struct regulator *regulator;
};

static int dummy_consumer_probe(struct platform_device *pdev)
{
	struct dummy_consumer_data *data;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->regulator = devm_regulator_get(&pdev->dev, "vdd");
	if (IS_ERR(data->regulator)) {
		dev_err(&pdev->dev, "Failed to get regulator\n");
		return PTR_ERR(data->regulator);
	}

	ret = regulator_enable(data->regulator);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable regulator: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, data);

	dev_dbg(&pdev->dev, "Dummy regulator consumer initialized\n");
	return 0;
}

static void dummy_consumer_remove(struct platform_device *pdev)
{
	struct dummy_consumer_data *data = platform_get_drvdata(pdev);

	regulator_disable(data->regulator);
}

static int dummy_consumer_resume(struct device *dev)
{
	struct dummy_consumer_data *data = dev_get_drvdata(dev);

	return regulator_enable(data->regulator);
}

static int dummy_consumer_suspend(struct device *dev)
{
	struct dummy_consumer_data *data = dev_get_drvdata(dev);

	return regulator_disable(data->regulator);
}

static DEFINE_SIMPLE_DEV_PM_OPS(dummy_consumer_pm, dummy_consumer_suspend, dummy_consumer_resume);

static const struct of_device_id dummy_consumer_of_match[] = {
	{ .compatible = "regulator-dummy-consumer", },
	{ }
};
MODULE_DEVICE_TABLE(of, dummy_consumer_of_match);

static struct platform_driver dummy_consumer_driver = {
	.driver = {
		.name = "regulator-dummy-consumer",
		.of_match_table = dummy_consumer_of_match,
		.pm = pm_sleep_ptr(&dummy_consumer_pm),
	},
	.probe = dummy_consumer_probe,
	.remove = dummy_consumer_remove,
};
module_platform_driver(dummy_consumer_driver);

MODULE_AUTHOR("Aleksandrs Vinarskis <alex.vinarskis@gmail.com>");
MODULE_DESCRIPTION("Dummy regulator consumer driver");
MODULE_LICENSE("GPL");
