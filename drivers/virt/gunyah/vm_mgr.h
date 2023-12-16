/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _GUNYAH_VM_MGR_PRIV_H
#define _GUNYAH_VM_MGR_PRIV_H

#include <linux/device.h>
#include <linux/gunyah_rsc_mgr.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/rwsem.h>
#include <linux/wait.h>

#include <uapi/linux/gunyah.h>

long gunyah_dev_vm_mgr_ioctl(struct gunyah_rm *rm, unsigned int cmd,
			     unsigned long arg);

struct gunyah_vm {
	u16 vmid;
	struct gunyah_rm *rm;
	struct device *parent;
	enum gunyah_rm_vm_auth_mechanism auth;

	struct notifier_block nb;
	enum gunyah_rm_vm_status vm_status;
	wait_queue_head_t vm_status_wait;
	struct rw_semaphore status_lock;

	struct kref kref;
	struct mutex fn_lock;
	struct list_head functions;
	struct mutex resources_lock;
	struct list_head resources;
	struct list_head resource_tickets;
};

#endif
