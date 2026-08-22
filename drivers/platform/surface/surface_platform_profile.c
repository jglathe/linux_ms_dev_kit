// SPDX-License-Identifier: GPL-2.0+
/*
 * Surface Platform Profile / Performance Mode driver for Surface System
 * Aggregator Module (thermal and fan subsystem).
 *
 * Copyright (C) 2021-2022 Maximilian Luz <luzmaximilian@gmail.com>
 */

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_profile.h>
#include <linux/pm_qos.h>
#include <linux/power_supply.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include <linux/surface_aggregator/device.h>

#define SSAM_PROFILE_POWER_SAVER_MAX_FREQ_KHZ 2515000

// Enum for the platform performance profile sent to the TMP module.
enum ssam_tmp_profile {
	SSAM_TMP_PROFILE_NORMAL             = 1,
	SSAM_TMP_PROFILE_BATTERY_SAVER      = 2,
	SSAM_TMP_PROFILE_BETTER_PERFORMANCE = 3,
	SSAM_TMP_PROFILE_BEST_PERFORMANCE   = 4,
};

// Enum for the fan profile sent to the FAN module. This fan profile is
// only sent to the EC if the 'has_fan' property is set. The integers are
// not a typo, they differ from the performance profile indices.
enum ssam_fan_profile {
	SSAM_FAN_PROFILE_NORMAL             = 2,
	SSAM_FAN_PROFILE_BATTERY_SAVER      = 1,
	SSAM_FAN_PROFILE_BETTER_PERFORMANCE = 3,
	SSAM_FAN_PROFILE_BEST_PERFORMANCE   = 4,
};

struct ssam_tmp_profile_info {
	__le32 profile;
	__le16 unknown1;
	__le16 unknown2;
} __packed;

struct ssam_platform_profile_device {
	struct ssam_device *sdev;
	struct device *ppdev;
	struct cpufreq_policy **cpufreq_policies;
	struct freq_qos_request *freq_qos_reqs;
	unsigned int *freq_cap_khz;
	struct mutex freq_qos_lock;
	int num_policies;
	bool freq_capped;
#if IS_ENABLED(CONFIG_CPU_FREQ)
	struct notifier_block cpufreq_nb;
#endif
#if IS_ENABLED(CONFIG_POWER_SUPPLY)
	struct notifier_block power_supply_nb;
#endif
	bool has_fan;
};

SSAM_DEFINE_SYNC_REQUEST_CL_R(__ssam_tmp_profile_get, struct ssam_tmp_profile_info, {
	.target_category = SSAM_SSH_TC_TMP,
	.command_id      = 0x02,
});

SSAM_DEFINE_SYNC_REQUEST_CL_W(__ssam_tmp_profile_set, __le32, {
	.target_category = SSAM_SSH_TC_TMP,
	.command_id      = 0x03,
});

SSAM_DEFINE_SYNC_REQUEST_W(__ssam_fan_profile_set, u8, {
	.target_category = SSAM_SSH_TC_FAN,
	.target_id = SSAM_SSH_TID_SAM,
	.command_id = 0x0e,
	.instance_id = 0x01,
});

static int ssam_tmp_profile_get(struct ssam_device *sdev, enum ssam_tmp_profile *p)
{
	struct ssam_tmp_profile_info info;
	int status;

	status = ssam_retry(__ssam_tmp_profile_get, sdev, &info);
	if (status < 0)
		return status;

	*p = le32_to_cpu(info.profile);
	return 0;
}

static int ssam_tmp_profile_set(struct ssam_device *sdev, enum ssam_tmp_profile p)
{
	const __le32 profile_le = cpu_to_le32(p);

	return ssam_retry(__ssam_tmp_profile_set, sdev, &profile_le);
}

static int ssam_fan_profile_set(struct ssam_device *sdev, enum ssam_fan_profile p)
{
	const u8 profile = p;

	return ssam_retry(__ssam_fan_profile_set, sdev->ctrl, &profile);
}

static unsigned int ssam_find_cap_freq(struct cpufreq_policy *policy,
				       unsigned int ceiling_khz)
{
	struct cpufreq_frequency_table *entry;
	unsigned int cap_freq = 0;

	if (!policy->freq_table)
		return ceiling_khz;

	cpufreq_for_each_valid_entry(entry, policy->freq_table) {
		if (entry->frequency <= ceiling_khz &&
		    entry->frequency > cap_freq)
			cap_freq = entry->frequency;
	}

	return cap_freq ?: ceiling_khz;
}

static unsigned int
ssam_platform_profile_cap_freq(struct cpufreq_policy *policy)
{
	unsigned int max_freq;

	down_read(&policy->rwsem);
	max_freq = ssam_find_cap_freq(policy,
				      SSAM_PROFILE_POWER_SAVER_MAX_FREQ_KHZ);
	up_read(&policy->rwsem);

	return max_freq;
}

static int
ssam_platform_profile_apply_freq_cap(struct ssam_platform_profile_device *tpd,
				     enum platform_profile_option profile)
{
	struct cpufreq_policy *policy;
	bool capped;
	int i, status;

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		capped = true;
		break;
	case PLATFORM_PROFILE_BALANCED:
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
	case PLATFORM_PROFILE_PERFORMANCE:
		capped = false;
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&tpd->freq_qos_lock);

	if (tpd->freq_capped == capped) {
		mutex_unlock(&tpd->freq_qos_lock);
		return 0;
	}

	for (i = 0; i < num_possible_cpus(); i++) {
		unsigned int max_freq;

		policy = tpd->cpufreq_policies[i];
		if (!policy)
			continue;

		max_freq = capped ? tpd->freq_cap_khz[i] :
			FREQ_QOS_MAX_DEFAULT_VALUE;
		status = freq_qos_update_request(&tpd->freq_qos_reqs[i],
						 max_freq);
		if (status < 0)
			goto rollback;
	}

	tpd->freq_capped = capped;
	mutex_unlock(&tpd->freq_qos_lock);
	return 0;

rollback:
	dev_err(&tpd->sdev->dev,
		"failed to update CPU frequency cap for policy %u: %d\n",
		policy->cpu, status);

	while (--i >= 0) {
		unsigned int max_freq;

		policy = tpd->cpufreq_policies[i];
		if (!policy)
			continue;

		max_freq = tpd->freq_capped ? tpd->freq_cap_khz[i] :
			FREQ_QOS_MAX_DEFAULT_VALUE;
		if (freq_qos_update_request(&tpd->freq_qos_reqs[i],
					    max_freq) < 0)
			dev_warn(&tpd->sdev->dev,
				 "failed to roll back CPU frequency cap for policy %u\n",
				 policy->cpu);
	}

	mutex_unlock(&tpd->freq_qos_lock);
	return status;
}

static int convert_ssam_tmp_to_profile(struct ssam_device *sdev, enum ssam_tmp_profile p)
{
	switch (p) {
	case SSAM_TMP_PROFILE_NORMAL:
		return PLATFORM_PROFILE_BALANCED;

	case SSAM_TMP_PROFILE_BATTERY_SAVER:
		return PLATFORM_PROFILE_LOW_POWER;

	case SSAM_TMP_PROFILE_BETTER_PERFORMANCE:
		return PLATFORM_PROFILE_BALANCED_PERFORMANCE;

	case SSAM_TMP_PROFILE_BEST_PERFORMANCE:
		return PLATFORM_PROFILE_PERFORMANCE;

	default:
		dev_err(&sdev->dev, "invalid performance profile: %d", p);
		return -EINVAL;
	}
}

static int convert_profile_to_ssam_tmp(struct ssam_device *sdev, enum platform_profile_option p)
{
	switch (p) {
	case PLATFORM_PROFILE_LOW_POWER:
		return SSAM_TMP_PROFILE_BATTERY_SAVER;

	case PLATFORM_PROFILE_BALANCED:
		return SSAM_TMP_PROFILE_NORMAL;

	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		return SSAM_TMP_PROFILE_BETTER_PERFORMANCE;

	case PLATFORM_PROFILE_PERFORMANCE:
		return SSAM_TMP_PROFILE_BEST_PERFORMANCE;

	default:
		/* This should have already been caught by platform_profile_store(). */
		WARN(true, "unsupported platform profile");
		return -EOPNOTSUPP;
	}
}

static int convert_profile_to_ssam_fan(struct ssam_device *sdev, enum platform_profile_option p)
{
	switch (p) {
	case PLATFORM_PROFILE_LOW_POWER:
		return SSAM_FAN_PROFILE_BATTERY_SAVER;

	case PLATFORM_PROFILE_BALANCED:
		return SSAM_FAN_PROFILE_NORMAL;

	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		return SSAM_FAN_PROFILE_BETTER_PERFORMANCE;

	case PLATFORM_PROFILE_PERFORMANCE:
		return SSAM_FAN_PROFILE_BEST_PERFORMANCE;

	default:
		/* This should have already been caught by platform_profile_store(). */
		WARN(true, "unsupported platform profile");
		return -EOPNOTSUPP;
	}
}

static int ssam_platform_profile_get(struct device *dev,
				     enum platform_profile_option *profile)
{
	struct ssam_platform_profile_device *tpd;
	enum ssam_tmp_profile tp;
	int status;

	tpd = dev_get_drvdata(dev);

	status = ssam_tmp_profile_get(tpd->sdev, &tp);
	if (status)
		return status;

	status = convert_ssam_tmp_to_profile(tpd->sdev, tp);
	if (status < 0)
		return status;

	*profile = status;
	return 0;
}

static int
ssam_platform_profile_set_internal(struct ssam_platform_profile_device *tpd,
				   enum platform_profile_option profile)
{
	int tp;

	tp = convert_profile_to_ssam_tmp(tpd->sdev, profile);
	if (tp < 0)
		return tp;

	tp = ssam_tmp_profile_set(tpd->sdev, tp);
	if (tp < 0)
		return tp;

	if (tpd->has_fan) {
		tp = convert_profile_to_ssam_fan(tpd->sdev, profile);
		if (tp < 0)
			return tp;
		tp = ssam_fan_profile_set(tpd->sdev, tp);
		if (tp < 0)
			return tp;
	}

	return ssam_platform_profile_apply_freq_cap(tpd, profile);
}

static int ssam_platform_profile_set(struct device *dev,
				     enum platform_profile_option profile)
{
	return ssam_platform_profile_set_internal(dev_get_drvdata(dev), profile);
}

static int ssam_platform_profile_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);

	return 0;
}

static const struct platform_profile_ops ssam_platform_profile_ops = {
	.probe = ssam_platform_profile_probe,
	.profile_get = ssam_platform_profile_get,
	.profile_set = ssam_platform_profile_set,
};

static void
ssam_platform_profile_remove_freq_qos(struct ssam_platform_profile_device *tpd)
{
	int i;

	mutex_lock(&tpd->freq_qos_lock);
	for (i = 0; i < num_possible_cpus(); i++) {
		if (!tpd->cpufreq_policies[i])
			continue;

		if (freq_qos_request_active(&tpd->freq_qos_reqs[i]))
			freq_qos_remove_request(&tpd->freq_qos_reqs[i]);
		cpufreq_cpu_put(tpd->cpufreq_policies[i]);
		tpd->cpufreq_policies[i] = NULL;
		tpd->freq_cap_khz[i] = 0;
	}

	tpd->num_policies = 0;
	tpd->freq_capped = false;
	mutex_unlock(&tpd->freq_qos_lock);
}

static int
ssam_platform_profile_add_policy(struct ssam_platform_profile_device *tpd,
				 struct cpufreq_policy *policy,
				 unsigned int cap_freq)
{
	unsigned int max_freq;
	int free_slot = -1;
	int i, status;

	mutex_lock(&tpd->freq_qos_lock);

	for (i = 0; i < num_possible_cpus(); i++) {
		if (tpd->cpufreq_policies[i] == policy) {
			mutex_unlock(&tpd->freq_qos_lock);
			cpufreq_cpu_put(policy);
			return 0;
		}

		if (!tpd->cpufreq_policies[i] && free_slot < 0)
			free_slot = i;
	}

	if (free_slot < 0) {
		status = -ENOSPC;
		goto err_unlock;
	}

	max_freq = tpd->freq_capped ? cap_freq : FREQ_QOS_MAX_DEFAULT_VALUE;
	status = freq_qos_add_request(&policy->constraints,
				      &tpd->freq_qos_reqs[free_slot],
				      FREQ_QOS_MAX, max_freq);
	if (status < 0)
		goto err_unlock;

	tpd->cpufreq_policies[free_slot] = policy;
	tpd->freq_cap_khz[free_slot] = cap_freq;
	tpd->num_policies++;
	mutex_unlock(&tpd->freq_qos_lock);

	return 0;

err_unlock:
	mutex_unlock(&tpd->freq_qos_lock);
	cpufreq_cpu_put(policy);
	return status;
}

#if IS_ENABLED(CONFIG_CPU_FREQ)
static void
ssam_platform_profile_remove_policy(struct ssam_platform_profile_device *tpd,
				    struct cpufreq_policy *policy)
{
	int i;

	mutex_lock(&tpd->freq_qos_lock);
	for (i = 0; i < num_possible_cpus(); i++) {
		if (tpd->cpufreq_policies[i] != policy)
			continue;

		if (freq_qos_request_active(&tpd->freq_qos_reqs[i]))
			freq_qos_remove_request(&tpd->freq_qos_reqs[i]);
		cpufreq_cpu_put(tpd->cpufreq_policies[i]);
		tpd->cpufreq_policies[i] = NULL;
		tpd->freq_cap_khz[i] = 0;
		tpd->num_policies--;
		break;
	}
	mutex_unlock(&tpd->freq_qos_lock);
}

static int
ssam_platform_profile_cpufreq_event(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	struct ssam_platform_profile_device *tpd =
		container_of(nb, struct ssam_platform_profile_device, cpufreq_nb);
	struct cpufreq_policy *policy = data;
	unsigned int cap_freq;
	unsigned int cpu;
	int status;

	switch (event) {
	case CPUFREQ_CREATE_POLICY:
		/* The cpufreq core holds policy->rwsem for write here. */
		cap_freq = ssam_find_cap_freq(policy,
					      SSAM_PROFILE_POWER_SAVER_MAX_FREQ_KHZ);
		cpu = policy->cpu;
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			return NOTIFY_DONE;

		status = ssam_platform_profile_add_policy(tpd, policy, cap_freq);
		if (status < 0)
			dev_err(&tpd->sdev->dev,
				"failed to add CPU frequency QoS request for policy %u: %d\n",
				cpu, status);
		break;

	case CPUFREQ_REMOVE_POLICY:
		ssam_platform_profile_remove_policy(tpd, policy);
		break;
	}

	return NOTIFY_OK;
}

static int
ssam_platform_profile_register_cpufreq_notifier(struct ssam_platform_profile_device *tpd)
{
	tpd->cpufreq_nb.notifier_call = ssam_platform_profile_cpufreq_event;

	return cpufreq_register_notifier(&tpd->cpufreq_nb,
					 CPUFREQ_POLICY_NOTIFIER);
}

static void
ssam_platform_profile_unregister_cpufreq_notifier(struct ssam_platform_profile_device *tpd)
{
	cpufreq_unregister_notifier(&tpd->cpufreq_nb, CPUFREQ_POLICY_NOTIFIER);
}
#else
static int
ssam_platform_profile_register_cpufreq_notifier(struct ssam_platform_profile_device *tpd)
{
	return 0;
}

static void
ssam_platform_profile_unregister_cpufreq_notifier(struct ssam_platform_profile_device *tpd)
{
}
#endif

static int
ssam_platform_profile_add_freq_qos(struct ssam_platform_profile_device *tpd)
{
	struct device *dev = &tpd->sdev->dev;
	struct cpufreq_policy *policy;
	unsigned int policy_cpu = 0;
	int cpu, status;

	tpd->cpufreq_policies = devm_kcalloc(dev, num_possible_cpus(),
					    sizeof(*tpd->cpufreq_policies),
					    GFP_KERNEL);
	if (!tpd->cpufreq_policies)
		return -ENOMEM;

	tpd->freq_qos_reqs = devm_kcalloc(dev, num_possible_cpus(),
					  sizeof(*tpd->freq_qos_reqs),
					  GFP_KERNEL);
	if (!tpd->freq_qos_reqs)
		return -ENOMEM;

	tpd->freq_cap_khz = devm_kcalloc(dev, num_possible_cpus(),
					 sizeof(*tpd->freq_cap_khz),
					 GFP_KERNEL);
	if (!tpd->freq_cap_khz)
		return -ENOMEM;

	status = ssam_platform_profile_register_cpufreq_notifier(tpd);
	if (status)
		return status;

	cpus_read_lock();
	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		policy_cpu = policy->cpu;
		status = ssam_platform_profile_add_policy(tpd, policy,
				ssam_platform_profile_cap_freq(policy));
		if (status < 0)
			break;
	}
	cpus_read_unlock();

	if (status >= 0)
		return 0;

	dev_err(dev,
		"failed to add CPU frequency QoS request for policy %u: %d\n",
		policy_cpu, status);
	ssam_platform_profile_unregister_cpufreq_notifier(tpd);
	ssam_platform_profile_remove_freq_qos(tpd);
	return status;
}

#if IS_ENABLED(CONFIG_POWER_SUPPLY)
static int
ssam_platform_profile_power_supply_event(struct notifier_block *nb,
					 unsigned long event, void *data)
{
	if (event != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_DONE;

	/*
	 * Keep the selected profile and frequency cap identical on AC and DC.
	 * AC/DC energy-performance preference differentiation is deferred until
	 * the SCMI cpufreq interface provides EPP support.
	 */
	return NOTIFY_OK;
}

static int
ssam_platform_profile_register_power_notifier(struct ssam_platform_profile_device *tpd)
{
	tpd->power_supply_nb.notifier_call =
		ssam_platform_profile_power_supply_event;

	return power_supply_reg_notifier(&tpd->power_supply_nb);
}

static void
ssam_platform_profile_unregister_power_notifier(struct ssam_platform_profile_device *tpd)
{
	power_supply_unreg_notifier(&tpd->power_supply_nb);
}
#else
static int
ssam_platform_profile_register_power_notifier(struct ssam_platform_profile_device *tpd)
{
	return 0;
}

static void
ssam_platform_profile_unregister_power_notifier(struct ssam_platform_profile_device *tpd)
{
}
#endif

static int surface_platform_profile_probe(struct ssam_device *sdev)
{
	struct ssam_platform_profile_device *tpd;
	int status;

	tpd = devm_kzalloc(&sdev->dev, sizeof(*tpd), GFP_KERNEL);
	if (!tpd)
		return -ENOMEM;

	tpd->sdev = sdev;
	mutex_init(&tpd->freq_qos_lock);
	ssam_device_set_drvdata(sdev, tpd);

	tpd->has_fan = device_property_read_bool(&sdev->dev, "has_fan");

	status = ssam_platform_profile_add_freq_qos(tpd);
	if (status)
		return status;

	status = ssam_platform_profile_register_power_notifier(tpd);
	if (status)
		goto err_unregister_cpufreq_notifier;

	if (device_property_read_bool(&sdev->dev, "default-low-power")) {
		status = ssam_platform_profile_set_internal(tpd,
							    PLATFORM_PROFILE_LOW_POWER);
		if (status)
			goto err_unregister_power_notifier;
	}

	tpd->ppdev = devm_platform_profile_register(&sdev->dev,
						    "Surface Platform Profile",
						    tpd,
						    &ssam_platform_profile_ops);
	if (IS_ERR(tpd->ppdev)) {
		status = PTR_ERR(tpd->ppdev);
		goto err_unregister_power_notifier;
	}

	return 0;

err_unregister_power_notifier:
	ssam_platform_profile_unregister_power_notifier(tpd);
err_unregister_cpufreq_notifier:
	ssam_platform_profile_unregister_cpufreq_notifier(tpd);
	ssam_platform_profile_remove_freq_qos(tpd);
	return status;
}

static void surface_platform_profile_remove(struct ssam_device *sdev)
{
	struct ssam_platform_profile_device *tpd =
		ssam_device_get_drvdata(sdev);

	ssam_platform_profile_unregister_power_notifier(tpd);
	ssam_platform_profile_unregister_cpufreq_notifier(tpd);
	ssam_platform_profile_remove_freq_qos(tpd);
}

static const struct ssam_device_id ssam_platform_profile_match[] = {
	{ SSAM_SDEV(TMP, SAM, 0x00, 0x01) },
	{ },
};
MODULE_DEVICE_TABLE(ssam, ssam_platform_profile_match);

static struct ssam_device_driver surface_platform_profile = {
	.probe = surface_platform_profile_probe,
	.remove = surface_platform_profile_remove,
	.match_table = ssam_platform_profile_match,
	.driver = {
		.name = "surface_platform_profile",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_ssam_device_driver(surface_platform_profile);

MODULE_AUTHOR("Maximilian Luz <luzmaximilian@gmail.com>");
MODULE_DESCRIPTION("Platform Profile Support for Surface System Aggregator Module");
MODULE_LICENSE("GPL");
