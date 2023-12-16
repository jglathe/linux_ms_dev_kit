/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _GUNYAH_RSC_MGR_H
#define _GUNYAH_RSC_MGR_H

#include <linux/notifier.h>
#include <linux/gunyah.h>

#define GUNYAH_VMID_INVAL U16_MAX
#define GUNYAH_MEM_HANDLE_INVAL U32_MAX

struct gunyah_rm;
int gunyah_rm_notifier_register(struct gunyah_rm *rm,
				struct notifier_block *nb);
int gunyah_rm_notifier_unregister(struct gunyah_rm *rm,
				  struct notifier_block *nb);
struct device *gunyah_rm_get(struct gunyah_rm *rm);
void gunyah_rm_put(struct gunyah_rm *rm);

struct gunyah_rm_vm_exited_payload {
	__le16 vmid;
	__le16 exit_type;
	__le32 exit_reason_size;
	u8 exit_reason[];
} __packed;

enum gunyah_rm_notification_id {
	/* clang-format off */
	GUNYAH_RM_NOTIFICATION_VM_EXITED		 = 0x56100001,
	GUNYAH_RM_NOTIFICATION_VM_STATUS		 = 0x56100008,
	/* clang-format on */
};

enum gunyah_rm_vm_status {
	/* clang-format off */
	GUNYAH_RM_VM_STATUS_NO_STATE		= 0,
	GUNYAH_RM_VM_STATUS_INIT		= 1,
	GUNYAH_RM_VM_STATUS_READY		= 2,
	GUNYAH_RM_VM_STATUS_RUNNING		= 3,
	GUNYAH_RM_VM_STATUS_PAUSED		= 4,
	GUNYAH_RM_VM_STATUS_LOAD		= 5,
	GUNYAH_RM_VM_STATUS_AUTH		= 6,
	GUNYAH_RM_VM_STATUS_INIT_FAILED		= 8,
	GUNYAH_RM_VM_STATUS_EXITED		= 9,
	GUNYAH_RM_VM_STATUS_RESETTING		= 10,
	GUNYAH_RM_VM_STATUS_RESET		= 11,
	/* clang-format on */
};

struct gunyah_rm_vm_status_payload {
	__le16 vmid;
	u16 reserved;
	u8 vm_status;
	u8 os_status;
	__le16 app_status;
} __packed;

#define GUNYAH_RM_ACL_X BIT(0)
#define GUNYAH_RM_ACL_W BIT(1)
#define GUNYAH_RM_ACL_R BIT(2)

struct gunyah_rm_mem_acl_entry {
	__le16 vmid;
	u8 perms;
	u8 reserved;
} __packed;

struct gunyah_rm_mem_entry {
	__le64 phys_addr;
	__le64 size;
} __packed;

enum gunyah_rm_mem_type {
	GUNYAH_RM_MEM_TYPE_NORMAL = 0,
	GUNYAH_RM_MEM_TYPE_IO = 1,
};

/*
 * struct gunyah_rm_mem_parcel - Info about memory to be lent/shared/donated/reclaimed
 * @mem_type: The type of memory: normal (DDR) or IO
 * @label: An client-specified identifier which can be used by the other VMs to identify the purpose
 *         of the memory parcel.
 * @n_acl_entries: Count of the number of entries in the @acl_entries array.
 * @acl_entries: An array of access control entries. Each entry specifies a VM and what access
 *               is allowed for the memory parcel.
 * @n_mem_entries: Count of the number of entries in the @mem_entries array.
 * @mem_entries: An array of regions to be associated with the memory parcel. Addresses should be
 *               (intermediate) physical addresses from Linux's perspective.
 * @mem_handle: On success, filled with memory handle that RM allocates for this memory parcel
 */
struct gunyah_rm_mem_parcel {
	enum gunyah_rm_mem_type mem_type;
	u32 label;
	size_t n_acl_entries;
	struct gunyah_rm_mem_acl_entry *acl_entries;
	size_t n_mem_entries;
	struct gunyah_rm_mem_entry *mem_entries;
	u32 mem_handle;
};

/* RPC Calls */
int gunyah_rm_mem_share(struct gunyah_rm *rm,
			struct gunyah_rm_mem_parcel *parcel);
int gunyah_rm_mem_reclaim(struct gunyah_rm *rm,
			  struct gunyah_rm_mem_parcel *parcel);

int gunyah_rm_alloc_vmid(struct gunyah_rm *rm, u16 vmid);
int gunyah_rm_dealloc_vmid(struct gunyah_rm *rm, u16 vmid);
int gunyah_rm_vm_reset(struct gunyah_rm *rm, u16 vmid);
int gunyah_rm_vm_start(struct gunyah_rm *rm, u16 vmid);
int gunyah_rm_vm_stop(struct gunyah_rm *rm, u16 vmid);

enum gunyah_rm_vm_auth_mechanism {
	/* clang-format off */
	GUNYAH_RM_VM_AUTH_NONE			= 0,
	GUNYAH_RM_VM_AUTH_QCOM_PIL_ELF		= 1,
	GUNYAH_RM_VM_AUTH_QCOM_ANDROID_PVM	= 2,
	/* clang-format on */
};

int gunyah_rm_vm_configure(struct gunyah_rm *rm, u16 vmid,
			   enum gunyah_rm_vm_auth_mechanism auth_mechanism,
			   u32 mem_handle, u64 image_offset, u64 image_size,
			   u64 dtb_offset, u64 dtb_size);
int gunyah_rm_vm_init(struct gunyah_rm *rm, u16 vmid);
int gunyah_rm_vm_set_boot_context(struct gunyah_rm *rm, u16 vmid, u8 reg_set,
				  u8 reg_index, u64 value);

struct gunyah_rm_hyp_resource {
	u8 type;
	u8 reserved;
	__le16 partner_vmid;
	__le32 resource_handle;
	__le32 resource_label;
	__le64 cap_id;
	__le32 virq_handle;
	__le32 virq;
	__le64 base;
	__le64 size;
} __packed;

struct gunyah_rm_hyp_resources {
	__le32 n_entries;
	struct gunyah_rm_hyp_resource entries[];
} __packed;

int gunyah_rm_get_hyp_resources(struct gunyah_rm *rm, u16 vmid,
				struct gunyah_rm_hyp_resources **resources);
int gunyah_rm_get_vmid(struct gunyah_rm *rm, u16 *vmid);

int gunyah_rm_vm_set_demand_paging(struct gunyah_rm *rm, u16 vmid, u32 count,
				   struct gunyah_rm_mem_entry *mem_entries);

enum gunyah_rm_range_id {
	GUNYAH_RM_RANGE_ID_IMAGE = 0,
	GUNYAH_RM_RANGE_ID_FIRMWARE = 1,
};

int gunyah_rm_vm_set_address_layout(struct gunyah_rm *rm, u16 vmid,
				    enum gunyah_rm_range_id range_id,
				    u64 base_address, u64 size);

struct gunyah_resource *
gunyah_rm_alloc_resource(struct gunyah_rm *rm,
			 struct gunyah_rm_hyp_resource *hyp_resource);
void gunyah_rm_free_resource(struct gunyah_resource *ghrsc);

#endif
