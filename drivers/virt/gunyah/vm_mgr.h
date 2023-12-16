/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _GUNYAH_VM_MGR_PRIV_H
#define _GUNYAH_VM_MGR_PRIV_H

#include <linux/device.h>
#include <linux/gunyah_rsc_mgr.h>

#include <uapi/linux/gunyah.h>

long gunyah_dev_vm_mgr_ioctl(struct gunyah_rm *rm, unsigned int cmd,
			     unsigned long arg);

struct gunyah_vm {
	struct gunyah_rm *rm;
	struct device *parent;
};

#endif
