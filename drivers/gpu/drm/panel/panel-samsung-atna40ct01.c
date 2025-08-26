/*
 * DRM driver for Samsung ATNA40CT01 eDP OLED panel
 *
 * Copyright 2025 xAI (based on panel-samsung-atna33xc20.c)
 */
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <drm/display/drm_dp_aux_bus.h>
#include <drm/display/drm_dp_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct atna40ct01 {
    struct drm_panel panel;
    struct regulator *supply;
    struct gpio_desc *enable_gpio;
    struct drm_dp_aux *aux;
    bool prepared;
    bool enabled;
};

static inline struct atna40ct01 *to_atna40ct01(struct drm_panel *panel)
{
    return container_of(panel, struct atna40ct01, panel);
}

static int atna40ct01_suspend(struct device *dev)
{
    struct atna40ct01 *ctx = dev_get_drvdata(dev);

    if (!ctx->enable_gpio || !ctx->supply) {
        dev_err(dev, "Invalid GPIO or regulator in suspend\n");
        return -EINVAL;
    }

    gpiod_set_value_cansleep(ctx->enable_gpio, 0);
    regulator_disable(ctx->supply);

    return 0;
}

static int atna40ct01_resume(struct device *dev)
{
    struct atna40ct01 *ctx = dev_get_drvdata(dev);
    int ret;

    if (!ctx->enable_gpio || !ctx->supply) {
        dev_err(dev, "Invalid GPIO or regulator in resume\n");
        return -EINVAL;
    }

    dev_info(dev, "Enabling regulator\n");
    ret = regulator_enable(ctx->supply);
    if (ret) {
        dev_err(dev, "Failed to enable regulator: %d\n", ret);
        return ret;
    }

    dev_info(dev, "Waiting 1ms after regulator enable\n");
    msleep(1);

    dev_info(dev, "Setting enable GPIO high\n");
    gpiod_set_value_cansleep(ctx->enable_gpio, 1);

    dev_info(dev, "Waiting 200ms after GPIO enable\n");
    msleep(200);

    return 0;
}

static int atna40ct01_prepare(struct drm_panel *panel)
{
    struct atna40ct01 *ctx = to_atna40ct01(panel);
    int ret;

    if (ctx->prepared)
        return 0;

    dev_info(ctx->panel.dev, "Preparing panel\n");

    ret = pm_runtime_get_sync(panel->dev);
    if (ret < 0) {
        dev_err(panel->dev, "Failed to resume in prepare: %d\n", ret);
        pm_runtime_put_autosuspend(panel->dev);
        return ret;
    }

    ctx->prepared = true;
    return 0;
}

static int atna40ct01_unprepare(struct drm_panel *panel)
{
    struct atna40ct01 *ctx = to_atna40ct01(panel);

    if (!ctx->prepared)
        return 0;

    pm_runtime_put_autosuspend(panel->dev);

    ctx->prepared = false;
    return 0;
}

static int atna40ct01_enable(struct drm_panel *panel)
{
    struct atna40ct01 *ctx = to_atna40ct01(panel);

    if (ctx->enabled)
        return 0;

    dev_info(ctx->panel.dev, "Enabling panel\n");

    if (ctx->panel.backlight) {
        backlight_enable(ctx->panel.backlight);
        backlight_update_status(ctx->panel.backlight);
    } else {
        dev_warn(ctx->panel.dev, "No backlight available\n");
    }

    ctx->enabled = true;
    return 0;
}

static int atna40ct01_disable(struct drm_panel *panel)
{
    struct atna40ct01 *ctx = to_atna40ct01(panel);

    if (!ctx->enabled)
        return 0;

    if (ctx->panel.backlight) {
        backlight_disable(ctx->panel.backlight);
    }

    ctx->enabled = false;
    return 0;
}

static int atna40ct01_get_modes(struct drm_panel *panel,
                                struct drm_connector *connector)
{
    struct atna40ct01 *ctx = to_atna40ct01(panel);
    struct drm_display_mode *mode;
    int ret;

    ret = pm_runtime_get_sync(panel->dev);
    if (ret < 0) {
        dev_err(panel->dev, "Failed to resume in get_modes: %d\n", ret);
        return ret;
    }

    mode = drm_mode_create(connector->dev);
    if (!mode) {
        ret = -ENOMEM;
        goto put;
    }

    mode->clock = 154250;
    mode->hdisplay = 1920;
    mode->hsync_start = 1920 + 48;
    mode->hsync_end = 1920 + 48 + 32;
    mode->htotal = 1920 + 48 + 32 + 80;
    mode->vdisplay = 1200;
    mode->vsync_start = 1200 + 8;
    mode->vsync_end = 1200 + 8 + 8;
    mode->vtotal = 1200 + 8 + 8 + 20;
    mode->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
    mode->width_mm = 302;
    mode->height_mm = 189;

    drm_mode_set_name(mode);
    mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
    drm_mode_probed_add(connector, mode);

    ret = 1;

put:
    pm_runtime_mark_last_busy(panel->dev);
    pm_runtime_put_autosuspend(panel->dev);
    return ret;
}

static const struct drm_panel_funcs atna40ct01_funcs = {
    .prepare = atna40ct01_prepare,
    .unprepare = atna40ct01_unprepare,
    .enable = atna40ct01_enable,
    .disable = atna40ct01_disable,
    .get_modes = atna40ct01_get_modes,
};

static int atna40ct01_probe(struct dp_aux_ep_device *aux_ep)
{
    struct device *dev = &aux_ep->dev;
    struct atna40ct01 *ctx;
    int ret;

    ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    dev_set_drvdata(dev, ctx);  // Moved here to fix NULL ctx in resume

    ctx->aux = aux_ep->aux;

    ctx->supply = devm_regulator_get(dev, "power");
    if (IS_ERR(ctx->supply)) {
        ret = PTR_ERR(ctx->supply);
        dev_err_probe(dev, ret, "Failed to get power supply\n");
        return ret;
    }

    ctx->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
    if (IS_ERR(ctx->enable_gpio)) {
        ret = PTR_ERR(ctx->enable_gpio);
        dev_err_probe(dev, ret, "Failed to get enable GPIO\n");
        return ret;
    }

    drm_panel_init(&ctx->panel, dev, &atna40ct01_funcs, DRM_MODE_CONNECTOR_eDP);

    pm_runtime_enable(dev);
    pm_runtime_set_autosuspend_delay(dev, 2000);
    pm_runtime_use_autosuspend(dev);

    ret = pm_runtime_get_sync(dev);
    if (ret < 0) {
        dev_err(dev, "Failed to resume in probe: %d\n", ret);
        pm_runtime_put_noidle(dev);
        pm_runtime_disable(dev);
        return ret;
    }

    ret = drm_panel_dp_aux_backlight(&ctx->panel, ctx->aux);
    pm_runtime_mark_last_busy(dev);
    pm_runtime_put_autosuspend(dev);

    if (ret)
        dev_warn(dev, "Failed to register dp aux backlight: %d\n", ret);

    drm_panel_add(&ctx->panel);

    dev_info(dev, "Probed panel-samsung-atna40ct01\n");
    return 0;
}

static void atna40ct01_remove(struct dp_aux_ep_device *aux_ep)
{
    struct device *dev = &aux_ep->dev;
    struct atna40ct01 *ctx = dev_get_drvdata(dev);

    drm_panel_remove(&ctx->panel);
    pm_runtime_disable(dev);
}

static const struct of_device_id atna40ct01_of_match[] = {
    { .compatible = "samsung,atna40ct01" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, atna40ct01_of_match);

static const struct dev_pm_ops atna40ct01_pm_ops = {
    SET_RUNTIME_PM_OPS(atna40ct01_suspend, atna40ct01_resume, NULL)
    SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};

static struct dp_aux_ep_driver atna40ct01_driver = {
    .driver = {
        .name = "panel-samsung-atna40ct01",
        .of_match_table = atna40ct01_of_match,
        .pm = &atna40ct01_pm_ops,
    },
    .probe = atna40ct01_probe,
    .remove = atna40ct01_remove,
};

static int __init atna40ct01_init(void)
{
    return dp_aux_dp_driver_register(&atna40ct01_driver);
}
module_init(atna40ct01_init);

static void __exit atna40ct01_exit(void)
{
    dp_aux_dp_driver_unregister(&atna40ct01_driver);
}
module_exit(atna40ct01_exit);

MODULE_AUTHOR("xAI");
MODULE_DESCRIPTION("DRM driver for Samsung ATNA40CT01 eDP OLED panel");
MODULE_LICENSE("GPL");
