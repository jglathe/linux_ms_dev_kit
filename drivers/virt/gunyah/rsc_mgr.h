/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef __GUNYAH_RSC_MGR_PRIV_H
#define __GUNYAH_RSC_MGR_PRIV_H

#include <linux/gunyah.h>
#include <linux/gunyah_rsc_mgr.h>
#include <linux/types.h>

struct gunyah_rm;

int gunyah_rm_call(struct gunyah_rm *rsc_mgr, u32 message_id,
		   const void *req_buf, size_t req_buf_size, void **resp_buf,
		   size_t *resp_buf_size);

int gunyah_rm_platform_pre_mem_share(struct gunyah_rm *rm,
				     struct gunyah_rm_mem_parcel *mem_parcel);
int gunyah_rm_platform_post_mem_reclaim(
	struct gunyah_rm *rm, struct gunyah_rm_mem_parcel *mem_parcel);

int gunyah_rm_platform_pre_demand_page(struct gunyah_rm *rm, u16 vmid,
				       u32 flags, struct folio *folio);
int gunyah_rm_platform_reclaim_demand_page(struct gunyah_rm *rm, u16 vmid,
					   u32 flags, struct folio *folio);

#endif
