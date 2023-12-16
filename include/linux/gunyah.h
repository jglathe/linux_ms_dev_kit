/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _LINUX_GUNYAH_H
#define _LINUX_GUNYAH_H

#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/limits.h>
#include <linux/types.h>

/* Matches resource manager's resource types for VM_GET_HYP_RESOURCES RPC */
enum gunyah_resource_type {
	/* clang-format off */
	GUNYAH_RESOURCE_TYPE_BELL_TX	= 0,
	GUNYAH_RESOURCE_TYPE_BELL_RX	= 1,
	GUNYAH_RESOURCE_TYPE_MSGQ_TX	= 2,
	GUNYAH_RESOURCE_TYPE_MSGQ_RX	= 3,
	GUNYAH_RESOURCE_TYPE_VCPU	= 4,
	GUNYAH_RESOURCE_TYPE_MEM_EXTENT	= 9,
	GUNYAH_RESOURCE_TYPE_ADDR_SPACE	= 10,
	/* clang-format on */
};

struct gunyah_resource {
	enum gunyah_resource_type type;
	u64 capid;
	unsigned int irq;

	struct list_head list;
	u32 rm_label;
};

/******************************************************************************/
/* Common arch-independent definitions for Gunyah hypercalls                  */
#define GUNYAH_CAPID_INVAL U64_MAX
#define GUNYAH_VMID_ROOT_VM 0xff

enum gunyah_error {
	/* clang-format off */
	GUNYAH_ERROR_OK				= 0,
	GUNYAH_ERROR_UNIMPLEMENTED		= -1,
	GUNYAH_ERROR_RETRY			= -2,

	GUNYAH_ERROR_ARG_INVAL			= 1,
	GUNYAH_ERROR_ARG_SIZE			= 2,
	GUNYAH_ERROR_ARG_ALIGN			= 3,

	GUNYAH_ERROR_NOMEM			= 10,

	GUNYAH_ERROR_ADDR_OVFL			= 20,
	GUNYAH_ERROR_ADDR_UNFL			= 21,
	GUNYAH_ERROR_ADDR_INVAL			= 22,

	GUNYAH_ERROR_DENIED			= 30,
	GUNYAH_ERROR_BUSY			= 31,
	GUNYAH_ERROR_IDLE			= 32,

	GUNYAH_ERROR_IRQ_BOUND			= 40,
	GUNYAH_ERROR_IRQ_UNBOUND		= 41,

	GUNYAH_ERROR_CSPACE_CAP_NULL		= 50,
	GUNYAH_ERROR_CSPACE_CAP_REVOKED		= 51,
	GUNYAH_ERROR_CSPACE_WRONG_OBJ_TYPE	= 52,
	GUNYAH_ERROR_CSPACE_INSUF_RIGHTS	= 53,
	GUNYAH_ERROR_CSPACE_FULL		= 54,

	GUNYAH_ERROR_MSGQUEUE_EMPTY		= 60,
	GUNYAH_ERROR_MSGQUEUE_FULL		= 61,
	/* clang-format on */
};

/**
 * gunyah_error_remap() - Remap Gunyah hypervisor errors into a Linux error code
 * @gunyah_error: Gunyah hypercall return value
 */
static inline int gunyah_error_remap(enum gunyah_error gunyah_error)
{
	switch (gunyah_error) {
	case GUNYAH_ERROR_OK:
		return 0;
	case GUNYAH_ERROR_NOMEM:
		return -ENOMEM;
	case GUNYAH_ERROR_DENIED:
	case GUNYAH_ERROR_CSPACE_CAP_NULL:
	case GUNYAH_ERROR_CSPACE_CAP_REVOKED:
	case GUNYAH_ERROR_CSPACE_WRONG_OBJ_TYPE:
	case GUNYAH_ERROR_CSPACE_INSUF_RIGHTS:
		return -EACCES;
	case GUNYAH_ERROR_CSPACE_FULL:
	case GUNYAH_ERROR_BUSY:
	case GUNYAH_ERROR_IDLE:
		return -EBUSY;
	case GUNYAH_ERROR_IRQ_BOUND:
	case GUNYAH_ERROR_IRQ_UNBOUND:
	case GUNYAH_ERROR_MSGQUEUE_FULL:
	case GUNYAH_ERROR_MSGQUEUE_EMPTY:
		return -EIO;
	case GUNYAH_ERROR_UNIMPLEMENTED:
		return -EOPNOTSUPP;
	case GUNYAH_ERROR_RETRY:
		return -EAGAIN;
	default:
		return -EINVAL;
	}
}

enum gunyah_api_feature {
	/* clang-format off */
	GUNYAH_FEATURE_DOORBELL		= 1,
	GUNYAH_FEATURE_MSGQUEUE		= 2,
	GUNYAH_FEATURE_VCPU		= 5,
	GUNYAH_FEATURE_MEMEXTENT	= 6,
	/* clang-format on */
};

bool arch_is_gunyah_guest(void);

#define GUNYAH_API_V1 1

/* Other bits reserved for future use and will be zero */
/* clang-format off */
#define GUNYAH_API_INFO_API_VERSION_MASK	GENMASK_ULL(13, 0)
#define GUNYAH_API_INFO_BIG_ENDIAN		BIT_ULL(14)
#define GUNYAH_API_INFO_IS_64BIT		BIT_ULL(15)
#define GUNYAH_API_INFO_VARIANT_MASK 		GENMASK_ULL(63, 56)
/* clang-format on */

struct gunyah_hypercall_hyp_identify_resp {
	u64 api_info;
	u64 flags[3];
};

static inline u16
gunyah_api_version(const struct gunyah_hypercall_hyp_identify_resp *gunyah_api)
{
	return FIELD_GET(GUNYAH_API_INFO_API_VERSION_MASK,
			 gunyah_api->api_info);
}

void gunyah_hypercall_hyp_identify(
	struct gunyah_hypercall_hyp_identify_resp *hyp_identity);

enum gunyah_error gunyah_hypercall_bell_send(u64 capid, u64 new_flags,
					     u64 *old_flags);
enum gunyah_error gunyah_hypercall_bell_set_mask(u64 capid, u64 enable_mask,
						 u64 ack_mask);

#define GUNYAH_HYPERCALL_MSGQ_TX_FLAGS_PUSH BIT(0)

enum gunyah_error gunyah_hypercall_msgq_send(u64 capid, size_t size, void *buff,
					     u64 tx_flags, bool *ready);
enum gunyah_error gunyah_hypercall_msgq_recv(u64 capid, void *buff, size_t size,
					     size_t *recv_size, bool *ready);

#define GUNYAH_ADDRSPACE_SELF_CAP 0

enum gunyah_pagetable_access {
	/* clang-format off */
	GUNYAH_PAGETABLE_ACCESS_NONE		= 0,
	GUNYAH_PAGETABLE_ACCESS_X		= 1,
	GUNYAH_PAGETABLE_ACCESS_W		= 2,
	GUNYAH_PAGETABLE_ACCESS_R		= 4,
	GUNYAH_PAGETABLE_ACCESS_RX		= 5,
	GUNYAH_PAGETABLE_ACCESS_RW		= 6,
	GUNYAH_PAGETABLE_ACCESS_RWX		= 7,
	/* clang-format on */
};

/* clang-format off */
#define GUNYAH_MEMEXTENT_MAPPING_USER_ACCESS		GENMASK_ULL(2, 0)
#define GUNYAH_MEMEXTENT_MAPPING_KERNEL_ACCESS		GENMASK_ULL(6, 4)
#define GUNYAH_MEMEXTENT_MAPPING_TYPE			GENMASK_ULL(23, 16)
/* clang-format on */

enum gunyah_memextent_donate_type {
	/* clang-format off */
	GUNYAH_MEMEXTENT_DONATE_TO_CHILD		= 0,
	GUNYAH_MEMEXTENT_DONATE_TO_PARENT		= 1,
	GUNYAH_MEMEXTENT_DONATE_TO_SIBLING		= 2,
	GUNYAH_MEMEXTENT_DONATE_TO_PROTECTED		= 3,
	GUNYAH_MEMEXTENT_DONATE_FROM_PROTECTED		= 4,
	/* clang-format on */
};

enum gunyah_addrspace_map_flag_bits {
	/* clang-format off */
	GUNYAH_ADDRSPACE_MAP_FLAG_PARTIAL	= 0,
	GUNYAH_ADDRSPACE_MAP_FLAG_PRIVATE	= 1,
	GUNYAH_ADDRSPACE_MAP_FLAG_VMMIO		= 2,
	GUNYAH_ADDRSPACE_MAP_FLAG_NOSYNC	= 31,
	/* clang-format on */
};

enum gunyah_error gunyah_hypercall_addrspace_map(u64 capid, u64 extent_capid,
						 u64 vbase, u32 extent_attrs,
						 u32 flags, u64 offset,
						 u64 size);
enum gunyah_error gunyah_hypercall_addrspace_unmap(u64 capid, u64 extent_capid,
						   u64 vbase, u32 flags,
						   u64 offset, u64 size);

/* clang-format off */
#define GUNYAH_MEMEXTENT_OPTION_TYPE_MASK	GENMASK_ULL(7, 0)
#define GUNYAH_MEMEXTENT_OPTION_NOSYNC		BIT(31)
/* clang-format on */

enum gunyah_error gunyah_hypercall_memextent_donate(u32 options, u64 from_capid,
						    u64 to_capid, u64 offset,
						    u64 size);

struct gunyah_hypercall_vcpu_run_resp {
	union {
		enum {
			/* clang-format off */
			/* VCPU is ready to run */
			GUNYAH_VCPU_STATE_READY			= 0,
			/* VCPU is sleeping until an interrupt arrives */
			GUNYAH_VCPU_STATE_EXPECTS_WAKEUP	= 1,
			/* VCPU is powered off */
			GUNYAH_VCPU_STATE_POWERED_OFF		= 2,
			/* VCPU is blocked in EL2 for unspecified reason */
			GUNYAH_VCPU_STATE_BLOCKED		= 3,
			/* VCPU has returned for MMIO READ */
			GUNYAH_VCPU_ADDRSPACE_VMMIO_READ	= 4,
			/* VCPU has returned for MMIO WRITE */
			GUNYAH_VCPU_ADDRSPACE_VMMIO_WRITE	= 5,
			/* Host needs to satisfy a page fault */
			GUNYAH_VCPU_ADDRSPACE_PAGE_FAULT	= 7,
			/* clang-format on */
		} state;
		u64 sized_state;
	};
	u64 state_data[3];
};

enum {
	GUNYAH_ADDRSPACE_VMMIO_ACTION_EMULATE = 0,
	GUNYAH_ADDRSPACE_VMMIO_ACTION_RETRY = 1,
	GUNYAH_ADDRSPACE_VMMIO_ACTION_FAULT = 2,
};

enum gunyah_error
gunyah_hypercall_vcpu_run(u64 capid, u64 *resume_data,
			  struct gunyah_hypercall_vcpu_run_resp *resp);

#endif
