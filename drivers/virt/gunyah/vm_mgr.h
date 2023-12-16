/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _GUNYAH_VM_MGR_PRIV_H
#define _GUNYAH_VM_MGR_PRIV_H

#include <linux/device.h>
#include <linux/gunyah_rsc_mgr.h>
#include <linux/gunyah_vm_mgr.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/maple_tree.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/wait.h>

#include <uapi/linux/gunyah.h>

static inline u64 gunyah_gpa_to_gfn(u64 gpa)
{
	return gpa >> PAGE_SHIFT;
}

static inline u64 gunyah_gfn_to_gpa(u64 gfn)
{
	return gfn << PAGE_SHIFT;
}

long gunyah_dev_vm_mgr_ioctl(struct gunyah_rm *rm, unsigned int cmd,
			     unsigned long arg);

struct gunyah_vm {
	u16 vmid;
	struct maple_tree gm;
	struct maple_tree mem_layout;
	struct rw_semaphore mem_lock;
	struct gunyah_vm_resource_ticket addrspace_ticket,
		host_private_extent_ticket, host_shared_extent_ticket,
		guest_private_extent_ticket, guest_shared_extent_ticket;

	struct gunyah_rm *rm;
	struct device *parent;
	enum gunyah_rm_vm_auth_mechanism auth;

	struct notifier_block nb;
	enum gunyah_rm_vm_status vm_status;
	wait_queue_head_t vm_status_wait;
	struct rw_semaphore status_lock;
	struct gunyah_vm_exit_info exit_info;

	struct kref kref;
	struct mutex fn_lock;
	struct list_head functions;
	struct mutex resources_lock;
	struct list_head resources;
	struct list_head resource_tickets;
	struct rb_root mmio_handler_root;
	struct rw_semaphore mmio_handler_lock;
	struct xarray boot_context;
};

int gunyah_vm_mmio_write(struct gunyah_vm *ghvm, u64 addr, u32 len, u64 data);

int gunyah_vm_share_parcel(struct gunyah_vm *ghvm,
			   struct gunyah_rm_mem_parcel *parcel, u64 gfn,
			   u64 nr);
int gunyah_vm_parcel_to_paged(struct gunyah_vm *ghvm,
			      struct gunyah_rm_mem_parcel *parcel, u64 gfn,
			      u64 nr);
int gunyah_vm_reclaim_parcel(struct gunyah_vm *ghvm,
			     struct gunyah_rm_mem_parcel *parcel, u64 gfn);
int gunyah_vm_provide_folio(struct gunyah_vm *ghvm, struct folio *folio,
			    u64 gfn, bool share, bool write);
int gunyah_vm_reclaim_folio(struct gunyah_vm *ghvm, u64 gfn);
void gunyah_vm_reclaim_memory(struct gunyah_vm *ghvm);

int gunyah_vm_mmio_write(struct gunyah_vm *ghvm, u64 addr, u32 len, u64 data);

int gunyah_guest_mem_create(struct gunyah_create_mem_args *args);
int gunyah_gmem_modify_binding(struct gunyah_vm *ghvm,
			       struct gunyah_map_mem_args *args);
struct gunyah_gmem_binding;
void gunyah_gmem_remove_binding(struct gunyah_gmem_binding *binding);
int gunyah_gmem_share_parcel(struct gunyah_vm *ghvm,
			     struct gunyah_rm_mem_parcel *parcel, u64 *gfn,
			     u64 *nr);
int gunyah_gmem_reclaim_parcel(struct gunyah_vm *ghvm,
			       struct gunyah_rm_mem_parcel *parcel, u64 gfn,
			       u64 nr);

#endif
