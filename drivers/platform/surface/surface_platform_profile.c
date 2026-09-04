// SPDX-License-Identifier: GPL-2.0+
/*
 * Surface Platform Profile / Performance Mode driver for Surface System
 * Aggregator Module (thermal and fan subsystem).
 *
 * Copyright (C) 2021-2022 Maximilian Luz <luzmaximilian@gmail.com>
 */

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpuhotplug.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_profile.h>
#include <linux/pm_qos.h>
#include <linux/property.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include <linux/surface_aggregator/device.h>

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
	struct mutex profile_lock; /* Protects TMP, fan, and frequency transitions. */
	bool has_freq_cap;
#if IS_ENABLED(CONFIG_CPU_FREQ)
	struct cpufreq_policy **cpufreq_policies;
	struct freq_qos_request *freq_qos_reqs;
	unsigned int *freq_cap_khz;
	unsigned int low_power_max_freq_khz;
	struct mutex freq_qos_lock; /* Protects frequency QoS state. */
	struct hlist_node cpuhp_node;
	enum cpuhp_state cpuhp_state;
	struct notifier_block cpufreq_nb;
	int freq_qos_setup_status;
	bool accept_policies;
	bool cpuhp_instance_registered;
	bool cpuhp_replaying;
	bool cpuhp_state_registered;
	bool cpufreq_notifier_registered;
	bool freq_capped;
	bool freq_qos_enabled;
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

#if IS_ENABLED(CONFIG_CPU_FREQ)
static unsigned int ssam_find_cap_freq(const struct cpufreq_policy *policy,
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
ssam_platform_profile_cap_freq(struct ssam_platform_profile_device *tpd,
			       struct cpufreq_policy *policy)
{
	unsigned int max_freq;

	down_read(&policy->rwsem);
	max_freq = ssam_find_cap_freq(policy, tpd->low_power_max_freq_khz);
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

	if (!tpd->freq_qos_enabled)
		return 0;

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
#else
static int
ssam_platform_profile_apply_freq_cap(struct ssam_platform_profile_device *tpd,
				     enum platform_profile_option profile)
{
	return 0;
}
#endif

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

static int
ssam_platform_profile_get_internal(struct ssam_platform_profile_device *tpd,
				   enum platform_profile_option *profile)
{
	enum ssam_tmp_profile tp;
	int status;

	status = ssam_tmp_profile_get(tpd->sdev, &tp);
	if (status)
		return status;

	status = convert_ssam_tmp_to_profile(tpd->sdev, tp);
	if (status < 0)
		return status;

	*profile = status;
	return 0;
}

static int ssam_platform_profile_get(struct device *dev,
				     enum platform_profile_option *profile)
{
	struct ssam_platform_profile_device *tpd = dev_get_drvdata(dev);
	int status;

	mutex_lock(&tpd->profile_lock);
	status = ssam_platform_profile_get_internal(tpd, profile);
	mutex_unlock(&tpd->profile_lock);

	return status;
}

static void
ssam_platform_profile_reconcile(struct ssam_platform_profile_device *tpd,
				enum platform_profile_option old_profile,
				enum platform_profile_option target_profile,
				int transition_status)
{
	enum platform_profile_option current_profile;
	bool fan_uncertain = false;
	int fan_profile;
	int status;

	status = ssam_platform_profile_get_internal(tpd, &current_profile);
	if (status) {
		dev_err(&tpd->sdev->dev,
			"profile transition failed (%d) and TMP readback failed: %d\n",
			transition_status, status);

		if (old_profile == PLATFORM_PROFILE_LOW_POWER ||
		    target_profile == PLATFORM_PROFILE_LOW_POWER) {
			status = ssam_platform_profile_apply_freq_cap(tpd,
								      PLATFORM_PROFILE_LOW_POWER);
			if (status)
				dev_err(&tpd->sdev->dev,
					"failed to retain the conservative CPU cap: %d\n",
					status);
		}
		return;
	}

	if (tpd->has_fan) {
		fan_profile = convert_profile_to_ssam_fan(tpd->sdev, current_profile);
		if (fan_profile >= 0) {
			status = ssam_fan_profile_set(tpd->sdev, fan_profile);
			if (status) {
				dev_warn(&tpd->sdev->dev,
					 "failed to reconcile fan profile: %d\n",
					 status);
				fan_uncertain = true;
			}
		}
	}

	if (fan_uncertain &&
	    (old_profile == PLATFORM_PROFILE_LOW_POWER ||
	     target_profile == PLATFORM_PROFILE_LOW_POWER))
		current_profile = PLATFORM_PROFILE_LOW_POWER;

	status = ssam_platform_profile_apply_freq_cap(tpd, current_profile);
	if (status)
		dev_err(&tpd->sdev->dev,
			"failed to reconcile the CPU frequency cap: %d\n",
			status);
}

static int
ssam_platform_profile_set_legacy(struct ssam_platform_profile_device *tpd,
				 enum platform_profile_option profile)
{
	int fan_profile;
	int tmp_profile;
	int status;

	tmp_profile = convert_profile_to_ssam_tmp(tpd->sdev, profile);
	if (tmp_profile < 0)
		return tmp_profile;

	status = ssam_tmp_profile_set(tpd->sdev, tmp_profile);
	if (status)
		return status;

	if (!tpd->has_fan)
		return 0;

	fan_profile = convert_profile_to_ssam_fan(tpd->sdev, profile);
	if (fan_profile < 0)
		return fan_profile;

	return ssam_fan_profile_set(tpd->sdev, fan_profile);
}

static int
ssam_platform_profile_set_internal(struct ssam_platform_profile_device *tpd,
				   enum platform_profile_option profile)
{
	enum platform_profile_option old_profile;
	int fan_profile;
	int tmp_profile;
	int status;

	if (!tpd->has_freq_cap)
		return ssam_platform_profile_set_legacy(tpd, profile);

	status = ssam_platform_profile_get_internal(tpd, &old_profile);
	if (status)
		return status;

	tmp_profile = convert_profile_to_ssam_tmp(tpd->sdev, profile);
	if (tmp_profile < 0)
		return tmp_profile;

	fan_profile = convert_profile_to_ssam_fan(tpd->sdev, profile);
	if (fan_profile < 0)
		return fan_profile;

	/* Enter low power only after every tracked CPU policy is capped. */
	if (profile == PLATFORM_PROFILE_LOW_POWER) {
		status = ssam_platform_profile_apply_freq_cap(tpd, profile);
		if (status)
			return status;
	}

	if (tpd->has_fan) {
		status = ssam_fan_profile_set(tpd->sdev, fan_profile);
		if (status)
			goto reconcile;
	}

	/* TMP is authoritative and is therefore the firmware commit point. */
	status = ssam_tmp_profile_set(tpd->sdev, tmp_profile);
	if (status)
		goto reconcile;

	/* Keep the cap until TMP has committed a non-low-power profile. */
	if (profile != PLATFORM_PROFILE_LOW_POWER) {
		status = ssam_platform_profile_apply_freq_cap(tpd, profile);
		if (status)
			goto reconcile;
	}

	return 0;

reconcile:
	ssam_platform_profile_reconcile(tpd, old_profile, profile, status);
	return status;
}

static int ssam_platform_profile_set(struct device *dev,
				     enum platform_profile_option profile)
{
	struct ssam_platform_profile_device *tpd = dev_get_drvdata(dev);
	int status;

	mutex_lock(&tpd->profile_lock);
	status = ssam_platform_profile_set_internal(tpd, profile);
	mutex_unlock(&tpd->profile_lock);

	return status;
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

#if IS_ENABLED(CONFIG_CPU_FREQ)
static int
ssam_platform_profile_add_policy_locked(struct ssam_platform_profile_device *tpd,
					struct cpufreq_policy *policy,
					unsigned int cap_freq)
{
	unsigned int max_freq;
	int free_slot = -1;
	int i;
	int status;

	lockdep_assert_held(&tpd->freq_qos_lock);

	if (!tpd->accept_policies)
		return -ESHUTDOWN;

	for (i = 0; i < num_possible_cpus(); i++) {
		if (tpd->cpufreq_policies[i] == policy)
			return 0;

		if (!tpd->cpufreq_policies[i] && free_slot < 0)
			free_slot = i;
	}

	if (free_slot < 0)
		return -ENOSPC;

	max_freq = tpd->freq_capped ? cap_freq :
		FREQ_QOS_MAX_DEFAULT_VALUE;
	status = freq_qos_add_request(&policy->constraints,
				      &tpd->freq_qos_reqs[free_slot],
				      FREQ_QOS_MAX, max_freq);
	if (status < 0)
		return status;

	/* CPUFREQ_REMOVE_POLICY synchronously clears this raw pointer. */
	tpd->cpufreq_policies[free_slot] = policy;
	tpd->freq_cap_khz[free_slot] = cap_freq;

	return 0;
}

static void
ssam_platform_profile_remove_policy_locked(struct ssam_platform_profile_device *tpd,
					   struct cpufreq_policy *policy)
{
	int i;

	lockdep_assert_held(&tpd->freq_qos_lock);

	for (i = 0; i < num_possible_cpus(); i++) {
		if (tpd->cpufreq_policies[i] != policy)
			continue;

		if (freq_qos_request_active(&tpd->freq_qos_reqs[i]))
			freq_qos_remove_request(&tpd->freq_qos_reqs[i]);
		tpd->cpufreq_policies[i] = NULL;
		tpd->freq_cap_khz[i] = 0;
		break;
	}
}

static void
ssam_platform_profile_remove_all_policies_locked(struct ssam_platform_profile_device *tpd)
{
	int i;

	lockdep_assert_held(&tpd->freq_qos_lock);

	for (i = 0; i < num_possible_cpus(); i++) {
		if (!tpd->cpufreq_policies[i])
			continue;

		if (freq_qos_request_active(&tpd->freq_qos_reqs[i]))
			freq_qos_remove_request(&tpd->freq_qos_reqs[i]);
		tpd->cpufreq_policies[i] = NULL;
		tpd->freq_cap_khz[i] = 0;
	}

	tpd->freq_capped = false;
}

static int
ssam_platform_profile_add_active_cpu(struct ssam_platform_profile_device *tpd,
				     unsigned int cpu)
{
	struct cpufreq_policy *validation;
	struct cpufreq_policy *policy;
	unsigned int cap_freq;
	int status;

	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return -ENODEV;

	cap_freq = ssam_platform_profile_cap_freq(tpd, policy);

	mutex_lock(&tpd->freq_qos_lock);
	validation = cpufreq_cpu_get(cpu);
	if (validation != policy) {
		status = -ENODEV;
	} else {
		status = ssam_platform_profile_add_policy_locked(tpd, policy,
								 cap_freq);
	}
	mutex_unlock(&tpd->freq_qos_lock);

	if (validation)
		cpufreq_cpu_put(validation);
	cpufreq_cpu_put(policy);

	return status;
}

static int
ssam_platform_profile_add_hotplug_cpu(struct ssam_platform_profile_device *tpd,
				      unsigned int cpu)
{
	struct cpufreq_policy *policy;
	unsigned int cap_freq;
	int status;

	/* CPU hotplug serialization keeps this inactive policy alive. */
	policy = cpufreq_cpu_policy(cpu);
	if (!policy)
		return -ENODEV;

	cap_freq = ssam_platform_profile_cap_freq(tpd, policy);

	mutex_lock(&tpd->freq_qos_lock);
	status = ssam_platform_profile_add_policy_locked(tpd, policy,
							 cap_freq);
	mutex_unlock(&tpd->freq_qos_lock);

	return status;
}

static int
ssam_platform_profile_cpuhp_online(unsigned int cpu,
				   struct hlist_node *node)
{
	struct ssam_platform_profile_device *tpd =
		hlist_entry(node, struct ssam_platform_profile_device, cpuhp_node);
	int status;

	status = ssam_platform_profile_add_active_cpu(tpd, cpu);
	if (status == -ENODEV && !READ_ONCE(tpd->cpuhp_replaying))
		status = ssam_platform_profile_add_hotplug_cpu(tpd, cpu);

	if (status == -ENODEV || status == -ESHUTDOWN)
		return 0;

	if (status) {
		dev_err(&tpd->sdev->dev,
			"failed to add CPU frequency QoS request for CPU%u: %d\n",
			cpu, status);
		if (READ_ONCE(tpd->cpuhp_replaying) &&
		    !tpd->freq_qos_setup_status)
			tpd->freq_qos_setup_status = status;
	}

	/* A CPUHP startup callback must not leave a partially replayed setup. */
	return 0;
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
					      tpd->low_power_max_freq_khz);
		cpu = policy->cpu;
		mutex_lock(&tpd->freq_qos_lock);
		status = ssam_platform_profile_add_policy_locked(tpd, policy,
								 cap_freq);
		mutex_unlock(&tpd->freq_qos_lock);
		if (status < 0 && status != -ESHUTDOWN)
			dev_err(&tpd->sdev->dev,
				"failed to add CPU frequency QoS request for policy %u: %d\n",
				cpu, status);
		break;

	case CPUFREQ_REMOVE_POLICY:
		mutex_lock(&tpd->freq_qos_lock);
		ssam_platform_profile_remove_policy_locked(tpd, policy);
		mutex_unlock(&tpd->freq_qos_lock);
		break;
	}

	return NOTIFY_OK;
}

static void ssam_platform_profile_remove_qos(void *data)
{
	struct ssam_platform_profile_device *tpd = data;

	mutex_lock(&tpd->freq_qos_lock);
	tpd->accept_policies = false;
	mutex_unlock(&tpd->freq_qos_lock);

	if (tpd->cpuhp_instance_registered) {
		cpuhp_state_remove_instance_nocalls(tpd->cpuhp_state,
						    &tpd->cpuhp_node);
		tpd->cpuhp_instance_registered = false;
	}
	if (tpd->cpuhp_state_registered) {
		cpuhp_remove_multi_state(tpd->cpuhp_state);
		tpd->cpuhp_state_registered = false;
	}

	/* Keep REMOVE notifications live until no raw policy pointer remains. */
	mutex_lock(&tpd->freq_qos_lock);
	ssam_platform_profile_remove_all_policies_locked(tpd);
	tpd->freq_qos_enabled = false;
	mutex_unlock(&tpd->freq_qos_lock);

	if (tpd->cpufreq_notifier_registered) {
		cpufreq_unregister_notifier(&tpd->cpufreq_nb,
					    CPUFREQ_POLICY_NOTIFIER);
		tpd->cpufreq_notifier_registered = false;
	}
}

static int
ssam_platform_profile_add_freq_qos(struct ssam_platform_profile_device *tpd,
				   unsigned int freq_cap)
{
	struct device *dev = &tpd->sdev->dev;
	size_t policy_count = num_possible_cpus();
	unsigned int cpu;
	int status;

	tpd->cpufreq_policies = devm_kcalloc(dev, policy_count,
					     sizeof(*tpd->cpufreq_policies),
					     GFP_KERNEL);
	if (!tpd->cpufreq_policies)
		return -ENOMEM;

	tpd->freq_qos_reqs = devm_kcalloc(dev, policy_count,
					  sizeof(*tpd->freq_qos_reqs),
					  GFP_KERNEL);
	if (!tpd->freq_qos_reqs)
		return -ENOMEM;

	tpd->freq_cap_khz = devm_kcalloc(dev, policy_count,
					 sizeof(*tpd->freq_cap_khz),
					 GFP_KERNEL);
	if (!tpd->freq_cap_khz)
		return -ENOMEM;

	mutex_init(&tpd->freq_qos_lock);
	tpd->low_power_max_freq_khz = freq_cap;
	tpd->cpufreq_nb.notifier_call = ssam_platform_profile_cpufreq_event;

	status = cpufreq_register_notifier(&tpd->cpufreq_nb,
					   CPUFREQ_POLICY_NOTIFIER);
	if (status) {
		dev_warn(dev,
			 "CPU frequency control unavailable; low-power profile will not cap CPUs: %d\n",
			 status);
		return 0;
	}
	tpd->cpufreq_notifier_registered = true;

	mutex_lock(&tpd->freq_qos_lock);
	tpd->accept_policies = true;
	mutex_unlock(&tpd->freq_qos_lock);

	status = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN,
					 "surface/platform-profile:online",
					 ssam_platform_profile_cpuhp_online,
					 NULL);
	if (status < 0)
		goto disable_cap;
	tpd->cpuhp_state = status;
	tpd->cpuhp_state_registered = true;

	WRITE_ONCE(tpd->cpuhp_replaying, true);
	status = cpuhp_state_add_instance(tpd->cpuhp_state, &tpd->cpuhp_node);
	WRITE_ONCE(tpd->cpuhp_replaying, false);
	if (status)
		goto disable_cap;
	tpd->cpuhp_instance_registered = true;

	/*
	 * Close the interval between add_instance() dropping its CPU read lock
	 * and cpuhp_replaying becoming false. A CPU that crossed our callback
	 * in that interval either appears in this scan or is caught on its next
	 * online transition.
	 */
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		status = ssam_platform_profile_add_active_cpu(tpd, cpu);
		if (status == -ENODEV || status == -ESHUTDOWN)
			continue;
		if (status) {
			dev_err(dev,
				"failed to add CPU frequency QoS request for CPU%u: %d\n",
				cpu, status);
			if (!tpd->freq_qos_setup_status)
				tpd->freq_qos_setup_status = status;
		}
	}
	cpus_read_unlock();

	if (tpd->freq_qos_setup_status) {
		status = tpd->freq_qos_setup_status;
		goto disable_cap;
	}

	mutex_lock(&tpd->freq_qos_lock);
	tpd->freq_qos_enabled = true;
	mutex_unlock(&tpd->freq_qos_lock);

	return 0;

disable_cap:
	dev_warn(dev,
		 "CPU frequency cap setup failed; platform profiles remain available: %d\n",
		 status);
	ssam_platform_profile_remove_qos(tpd);
	return 0;
}
#else
static bool
ssam_platform_profile_freq_qos_enabled(struct ssam_platform_profile_device *tpd)
{
	return false;
}

static int
ssam_platform_profile_add_freq_qos(struct ssam_platform_profile_device *tpd,
				   unsigned int freq_cap)
{
	dev_warn(&tpd->sdev->dev,
		 "CPU frequency control is disabled; low-power profile will not cap CPUs\n");
	return 0;
}

static void ssam_platform_profile_remove_qos(void *data)
{
}
#endif

#if IS_ENABLED(CONFIG_CPU_FREQ)
static bool
ssam_platform_profile_freq_qos_enabled(struct ssam_platform_profile_device *tpd)
{
	return tpd->freq_qos_enabled;
}
#endif

static int surface_platform_profile_probe(struct ssam_device *sdev)
{
	const char *freq_property = "low-power-max-frequency-khz";
	struct ssam_platform_profile_device *tpd;
	unsigned int freq_cap;
	int status;

	tpd = devm_kzalloc(&sdev->dev, sizeof(*tpd), GFP_KERNEL);
	if (!tpd)
		return -ENOMEM;

	tpd->sdev = sdev;
	ssam_device_set_drvdata(sdev, tpd);
	mutex_init(&tpd->profile_lock);

	tpd->has_fan = device_property_read_bool(&sdev->dev, "has_fan");

	if (device_property_present(&sdev->dev, freq_property)) {
		status = device_property_read_u32(&sdev->dev, freq_property, &freq_cap);
		if (status)
			return status;
		if (!freq_cap)
			return -EINVAL;

		tpd->has_freq_cap = true;
		status = ssam_platform_profile_add_freq_qos(tpd, freq_cap);
		if (status)
			return status;

		if (ssam_platform_profile_freq_qos_enabled(tpd)) {
			status = devm_add_action_or_reset(&sdev->dev,
							  ssam_platform_profile_remove_qos,
							  tpd);
			if (status)
				return status;
		}
	}

	tpd->ppdev = devm_platform_profile_register(&sdev->dev,
						    "Surface Platform Profile",
						    tpd,
						    &ssam_platform_profile_ops);
	if (IS_ERR(tpd->ppdev)) {
		status = PTR_ERR(tpd->ppdev);
		return status;
	}

	if (device_property_read_bool(&sdev->dev, "default-low-power")) {
		mutex_lock(&tpd->profile_lock);
		status = ssam_platform_profile_set_internal(tpd,
							    PLATFORM_PROFILE_LOW_POWER);
		mutex_unlock(&tpd->profile_lock);
		if (status)
			dev_warn(&sdev->dev,
				 "failed to select the default low-power profile: %d\n",
				 status);
	}

	return 0;
}

static const struct ssam_device_id ssam_platform_profile_match[] = {
	{ SSAM_SDEV(TMP, SAM, 0x00, 0x01) },
	{ },
};
MODULE_DEVICE_TABLE(ssam, ssam_platform_profile_match);

static struct ssam_device_driver surface_platform_profile = {
	.probe = surface_platform_profile_probe,
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
