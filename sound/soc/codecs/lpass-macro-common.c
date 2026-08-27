// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2022, The Linux Foundation. All rights reserved.

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#include "lpass-macro-common.h"

static DEFINE_MUTEX(lpass_codec_mutex);
static enum lpass_codec_version lpass_codec_version;

static DEFINE_MUTEX(lpass_dmic_clk_mutex);
static void *lpass_dmic_clk_priv;
static const struct lpass_macro_dmic_clk_ops *lpass_dmic_clk_ops;

struct lpass_macro *lpass_macro_pds_init(struct device *dev)
{
	struct lpass_macro *l_pds;
	int ret;

	if (!of_property_present(dev->of_node, "power-domains"))
		return NULL;

	l_pds = devm_kzalloc(dev, sizeof(*l_pds), GFP_KERNEL);
	if (!l_pds)
		return ERR_PTR(-ENOMEM);

	l_pds->macro_pd = dev_pm_domain_attach_by_name(dev, "macro");
	if (IS_ERR_OR_NULL(l_pds->macro_pd)) {
		ret = l_pds->macro_pd ? PTR_ERR(l_pds->macro_pd) : -ENODATA;
		goto macro_err;
	}

	ret = pm_runtime_resume_and_get(l_pds->macro_pd);
	if (ret < 0)
		goto macro_sync_err;

	l_pds->dcodec_pd = dev_pm_domain_attach_by_name(dev, "dcodec");
	if (IS_ERR_OR_NULL(l_pds->dcodec_pd)) {
		ret = l_pds->dcodec_pd ? PTR_ERR(l_pds->dcodec_pd) : -ENODATA;
		goto dcodec_err;
	}

	ret = pm_runtime_resume_and_get(l_pds->dcodec_pd);
	if (ret < 0)
		goto dcodec_sync_err;
	return l_pds;

dcodec_sync_err:
	dev_pm_domain_detach(l_pds->dcodec_pd, false);
dcodec_err:
	pm_runtime_put(l_pds->macro_pd);
macro_sync_err:
	dev_pm_domain_detach(l_pds->macro_pd, false);
macro_err:
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(lpass_macro_pds_init);

void lpass_macro_pds_exit(struct lpass_macro *pds)
{
	if (pds) {
		pm_runtime_put(pds->macro_pd);
		dev_pm_domain_detach(pds->macro_pd, false);
		pm_runtime_put(pds->dcodec_pd);
		dev_pm_domain_detach(pds->dcodec_pd, false);
	}
}
EXPORT_SYMBOL_GPL(lpass_macro_pds_exit);

void lpass_macro_set_codec_version(enum lpass_codec_version version)
{
	mutex_lock(&lpass_codec_mutex);
	lpass_codec_version = version;
	mutex_unlock(&lpass_codec_mutex);
}
EXPORT_SYMBOL_GPL(lpass_macro_set_codec_version);

enum lpass_codec_version lpass_macro_get_codec_version(void)
{
	enum lpass_codec_version ver;

	mutex_lock(&lpass_codec_mutex);
	ver = lpass_codec_version;
	mutex_unlock(&lpass_codec_mutex);

	return ver;
}
EXPORT_SYMBOL_GPL(lpass_macro_get_codec_version);

int lpass_macro_register_dmic_clk_provider(void *priv, const struct lpass_macro_dmic_clk_ops *ops)
{
	int ret = 0;

	if (!priv || !ops || !ops->request)
		return -EINVAL;

	mutex_lock(&lpass_dmic_clk_mutex);
	if (lpass_dmic_clk_priv && lpass_dmic_clk_priv != priv) {
		ret = -EBUSY;
	} else {
		lpass_dmic_clk_priv = priv;
		lpass_dmic_clk_ops = ops;
	}
	mutex_unlock(&lpass_dmic_clk_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(lpass_macro_register_dmic_clk_provider);

void lpass_macro_unregister_dmic_clk_provider(void *priv)
{
	mutex_lock(&lpass_dmic_clk_mutex);
	if (lpass_dmic_clk_priv == priv) {
		lpass_dmic_clk_ops = NULL;
		lpass_dmic_clk_priv = NULL;
	}
	mutex_unlock(&lpass_dmic_clk_mutex);
}
EXPORT_SYMBOL_GPL(lpass_macro_unregister_dmic_clk_provider);

int lpass_macro_dmic_clk_request(unsigned int dmic, bool enable)
{
	int ret;

	mutex_lock(&lpass_dmic_clk_mutex);
	if (!lpass_dmic_clk_ops)
		ret = -ENODEV;
	else
		ret = lpass_dmic_clk_ops->request(lpass_dmic_clk_priv, dmic, enable);
	mutex_unlock(&lpass_dmic_clk_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(lpass_macro_dmic_clk_request);

MODULE_DESCRIPTION("Common macro driver");
MODULE_LICENSE("GPL");
