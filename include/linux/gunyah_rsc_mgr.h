/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _GUNYAH_RSC_MGR_H
#define _GUNYAH_RSC_MGR_H

#include <linux/notifier.h>
#include <linux/gunyah.h>

#define GUNYAH_VMID_INVAL U16_MAX

struct gunyah_rm;
int gunyah_rm_notifier_register(struct gunyah_rm *rm,
				struct notifier_block *nb);
int gunyah_rm_notifier_unregister(struct gunyah_rm *rm,
				  struct notifier_block *nb);
struct device *gunyah_rm_get(struct gunyah_rm *rm);
void gunyah_rm_put(struct gunyah_rm *rm);

#endif
