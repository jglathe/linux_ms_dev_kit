// SPDX-License-Identifier: GPL-2.0
#include <linux/bitmap.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

struct gpio_irq_mux {
	struct irq_domain *domain;
	unsigned long *enabled;
	unsigned int num_irqs;
	int parent_irq;
	struct irq_chip chip;
};

static void gpio_irq_mux_mask(struct irq_data *d)
{
	struct gpio_irq_mux *mux = irq_data_get_irq_chip_data(d);

	clear_bit(d->hwirq, mux->enabled);
}

static void gpio_irq_mux_unmask(struct irq_data *d)
{
	struct gpio_irq_mux *mux = irq_data_get_irq_chip_data(d);

	set_bit(d->hwirq, mux->enabled);
}

static int gpio_irq_mux_set_type(struct irq_data *d, unsigned int type)
{
	return 0;
}

static int gpio_irq_mux_set_wake(struct irq_data *d, unsigned int on)
{
	struct gpio_irq_mux *mux = irq_data_get_irq_chip_data(d);
	return irq_set_irq_wake(mux->parent_irq, on);
}

static int gpio_irq_mux_domain_map(struct irq_domain *d, unsigned int virq,
				   irq_hw_number_t hwirq)
{
	struct gpio_irq_mux *mux = d->host_data;

	if (hwirq >= mux->num_irqs)
		return -EINVAL;

	irq_set_chip_data(virq, mux);
	irq_set_chip_and_handler(virq, &mux->chip, handle_level_irq);
	irq_set_noprobe(virq);

	return 0;
}

static const struct irq_domain_ops gpio_irq_mux_domain_ops = {
	.map = gpio_irq_mux_domain_map,
	.xlate = irq_domain_xlate_onecell,
};

static void gpio_irq_mux_handler(struct irq_desc *desc)
{
	struct gpio_irq_mux *mux = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned int i;

	chained_irq_enter(chip, desc);
	for (i = 0; i < mux->num_irqs; i++) {
		if (test_bit(i, mux->enabled))
			generic_handle_domain_irq(mux->domain, i);
	}
	chained_irq_exit(chip, desc);
}

static int gpio_irq_mux_probe(struct platform_device *pdev)
{
	struct gpio_irq_mux *mux;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	u32 num_irqs;
	int ret;

	ret = of_property_read_u32(np, "num-interrupts", &num_irqs);
	if (ret) {
		dev_err(dev, "missing num-interrupts property\n");
		return ret;
	}

	if (!num_irqs)
		return -EINVAL;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	mux->enabled = devm_bitmap_zalloc(dev, num_irqs, GFP_KERNEL);
	if (!mux->enabled)
		return -ENOMEM;

	mux->parent_irq = platform_get_irq(pdev, 0);
	if (mux->parent_irq < 0)
		return mux->parent_irq;

	mux->num_irqs = num_irqs;
	mux->chip.name = dev_name(dev);
	mux->chip.irq_mask = gpio_irq_mux_mask;
	mux->chip.irq_unmask = gpio_irq_mux_unmask;
	mux->chip.irq_set_type = gpio_irq_mux_set_type;
	mux->chip.irq_set_wake = gpio_irq_mux_set_wake;

	mux->domain = irq_domain_add_linear(np, num_irqs,
					    &gpio_irq_mux_domain_ops, mux);
	if (!mux->domain)
		return -ENOMEM;

	irq_set_chained_handler_and_data(mux->parent_irq,
					 gpio_irq_mux_handler, mux);

	platform_set_drvdata(pdev, mux);

	return 0;
}

static void gpio_irq_mux_remove(struct platform_device *pdev)
{
	struct gpio_irq_mux *mux = platform_get_drvdata(pdev);

	irq_set_chained_handler_and_data(mux->parent_irq, NULL, NULL);
	irq_domain_remove(mux->domain);
}

static const struct of_device_id gpio_irq_mux_of_match[] = {
	{ .compatible = "gpio-irq-mux" },
	{ }
};
MODULE_DEVICE_TABLE(of, gpio_irq_mux_of_match);

static struct platform_driver gpio_irq_mux_driver = {
	.probe = gpio_irq_mux_probe,
	.remove = gpio_irq_mux_remove,
	.driver = {
		.name = "gpio-irq-mux",
		.of_match_table = gpio_irq_mux_of_match,
	},
};
module_platform_driver(gpio_irq_mux_driver);

MODULE_AUTHOR("Codex");
MODULE_DESCRIPTION("GPIO IRQ multiplexer");
MODULE_LICENSE("GPL");
