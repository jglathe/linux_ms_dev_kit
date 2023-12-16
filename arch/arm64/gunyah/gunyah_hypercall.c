// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/arm-smccc.h>
#include <linux/module.h>
#include <linux/gunyah.h>
#include <linux/uuid.h>

/* {c1d58fcd-a453-5fdb-9265-ce36673d5f14} */
static const uuid_t GUNYAH_UUID = UUID_INIT(0xc1d58fcd, 0xa453, 0x5fdb, 0x92,
					    0x65, 0xce, 0x36, 0x67, 0x3d, 0x5f,
					    0x14);

bool arch_is_gunyah_guest(void)
{
	struct arm_smccc_res res;
	uuid_t uuid;
	u32 *up;

	arm_smccc_1_1_hvc(ARM_SMCCC_VENDOR_HYP_CALL_UID_FUNC_ID, &res);

	up = (u32 *)&uuid.b[0];
	up[0] = lower_32_bits(res.a0);
	up[1] = lower_32_bits(res.a1);
	up[2] = lower_32_bits(res.a2);
	up[3] = lower_32_bits(res.a3);

	return uuid_equal(&uuid, &GUNYAH_UUID);
}
EXPORT_SYMBOL_GPL(arch_is_gunyah_guest);

#define GUNYAH_HYPERCALL(fn)                                      \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64, \
			   ARM_SMCCC_OWNER_VENDOR_HYP, fn)

/* clang-format off */
#define GUNYAH_HYPERCALL_HYP_IDENTIFY		GUNYAH_HYPERCALL(0x8000)
#define GUNYAH_HYPERCALL_MSGQ_SEND		GUNYAH_HYPERCALL(0x801B)
#define GUNYAH_HYPERCALL_MSGQ_RECV		GUNYAH_HYPERCALL(0x801C)
#define GUNYAH_HYPERCALL_ADDRSPACE_MAP		GUNYAH_HYPERCALL(0x802B)
#define GUNYAH_HYPERCALL_ADDRSPACE_UNMAP		GUNYAH_HYPERCALL(0x802C)
#define GUNYAH_HYPERCALL_MEMEXTENT_DONATE		GUNYAH_HYPERCALL(0x8061)
/* clang-format on */

/**
 * gunyah_hypercall_hyp_identify() - Returns build information and feature flags
 *                               supported by Gunyah.
 * @hyp_identity: filled by the hypercall with the API info and feature flags.
 */
void gunyah_hypercall_hyp_identify(
	struct gunyah_hypercall_hyp_identify_resp *hyp_identity)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GUNYAH_HYPERCALL_HYP_IDENTIFY, &res);

	hyp_identity->api_info = res.a0;
	hyp_identity->flags[0] = res.a1;
	hyp_identity->flags[1] = res.a2;
	hyp_identity->flags[2] = res.a3;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_hyp_identify);

enum gunyah_error gunyah_hypercall_msgq_send(u64 capid, size_t size, void *buff,
					     u64 tx_flags, bool *ready)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GUNYAH_HYPERCALL_MSGQ_SEND, capid, size,
			  (uintptr_t)buff, tx_flags, 0, &res);

	if (res.a0 == GUNYAH_ERROR_OK)
		*ready = !!res.a1;

	return res.a0;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_msgq_send);

enum gunyah_error gunyah_hypercall_msgq_recv(u64 capid, void *buff, size_t size,
					     size_t *recv_size, bool *ready)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GUNYAH_HYPERCALL_MSGQ_RECV, capid, (uintptr_t)buff,
			  size, 0, &res);

	if (res.a0 == GUNYAH_ERROR_OK) {
		*recv_size = res.a1;
		*ready = !!res.a2;
	}

	return res.a0;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_msgq_recv);

enum gunyah_error gunyah_hypercall_addrspace_map(u64 capid, u64 extent_capid, u64 vbase,
					u32 extent_attrs, u32 flags, u64 offset, u64 size)
{
	struct arm_smccc_1_2_regs args = {
		.a0 = GUNYAH_HYPERCALL_ADDRSPACE_MAP,
		.a1 = capid,
		.a2 = extent_capid,
		.a3 = vbase,
		.a4 = extent_attrs,
		.a5 = flags,
		.a6 = offset,
		.a7 = size,
		/* C language says this will be implictly zero. Gunyah requires 0, so be explicit */
		.a8 = 0,
	};
	struct arm_smccc_1_2_regs res;

	arm_smccc_1_2_hvc(&args, &res);

	return res.a0;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_addrspace_map);

enum gunyah_error gunyah_hypercall_addrspace_unmap(u64 capid, u64 extent_capid, u64 vbase,
					u32 flags, u64 offset, u64 size)
{
	struct arm_smccc_1_2_regs args = {
		.a0 = GUNYAH_HYPERCALL_ADDRSPACE_UNMAP,
		.a1 = capid,
		.a2 = extent_capid,
		.a3 = vbase,
		.a4 = flags,
		.a5 = offset,
		.a6 = size,
		/* C language says this will be implictly zero. Gunyah requires 0, so be explicit */
		.a7 = 0,
	};
	struct arm_smccc_1_2_regs res;

	arm_smccc_1_2_hvc(&args, &res);

	return res.a0;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_addrspace_unmap);

enum gunyah_error gunyah_hypercall_memextent_donate(u32 options, u64 from_capid, u64 to_capid,
					    u64 offset, u64 size)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GUNYAH_HYPERCALL_MEMEXTENT_DONATE, options, from_capid, to_capid,
				offset, size, 0, &res);

	return res.a0;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_memextent_donate);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gunyah Hypervisor Hypercalls");
