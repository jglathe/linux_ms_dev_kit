// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "gunyah_vm_mgr: " fmt

#include <asm/gunyah.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

#include "vm_mgr.h"

#define WRITE_TAG (1 << 0)
#define SHARE_TAG (1 << 1)
#define MEM_PARCEL_TAG (1 << 2)

static inline struct gunyah_resource *
__first_resource(struct gunyah_vm_resource_ticket *ticket)
{
	return list_first_entry_or_null(&ticket->resources,
					struct gunyah_resource, list);
}

int gunyah_vm_share_parcel(struct gunyah_vm *ghvm,
			   struct gunyah_rm_mem_parcel *parcel, u64 gfn, u64 nr)
{
	int ret;

	ret = mtree_insert_range(&ghvm->gm, gfn, gfn + nr - 1,
				 xa_tag_pointer(parcel, MEM_PARCEL_TAG),
				 GFP_KERNEL);
	if (ret)
		return ret;

	ret = gunyah_rm_mem_share(ghvm->rm, parcel);
	if (ret)
		mtree_erase(&ghvm->gm, gfn);

	return ret;
}

int gunyah_vm_parcel_to_paged(struct gunyah_vm *ghvm,
			      struct gunyah_rm_mem_parcel *parcel, u64 gfn,
			      u64 nr)
{
	struct gunyah_rm_mem_entry *entry;
	struct folio *folio;
	unsigned long g, e, tag = 0;
	pgoff_t off;
	int ret;

	if (parcel->n_acl_entries > 1)
		tag |= SHARE_TAG;
	if (parcel->acl_entries[0].perms & GUNYAH_RM_ACL_W)
		tag |= WRITE_TAG;

	for (e = 0, g = gfn; g < gfn + nr && e < parcel->n_mem_entries; e++) {
		entry = &parcel->mem_entries[e];
		folio = pfn_folio(PHYS_PFN(entry->phys_addr));

		for (off = 0; off < PHYS_PFN(entry->size);
		     off += folio_nr_pages(folio)) {
			ret = mtree_store_range(&ghvm->gm, g + off, g + off - 1,
						xa_tag_pointer(folio, tag),
						GFP_KERNEL);
			if (ret == -ENOMEM)
				return ret;
			BUG_ON(ret);

			folio = folio_next(folio);
		}
	}

	return 0;
}

int gunyah_vm_reclaim_parcel(struct gunyah_vm *ghvm,
			     struct gunyah_rm_mem_parcel *parcel, u64 gfn)
{
	int ret;

	ret = gunyah_rm_mem_reclaim(ghvm->rm, parcel);
	if (ret)
		return ret;

	mtree_erase(&ghvm->gm, gfn);

	return ret;
}

static inline u32 donate_flags(bool share)
{
	if (share)
		return FIELD_PREP_CONST(GUNYAH_MEMEXTENT_OPTION_TYPE_MASK,
					GUNYAH_MEMEXTENT_DONATE_TO_SIBLING);
	else
		return FIELD_PREP_CONST(GUNYAH_MEMEXTENT_OPTION_TYPE_MASK,
					GUNYAH_MEMEXTENT_DONATE_TO_PROTECTED);
}

static inline u32 reclaim_flags(bool share)
{
	if (share)
		return FIELD_PREP_CONST(GUNYAH_MEMEXTENT_OPTION_TYPE_MASK,
					GUNYAH_MEMEXTENT_DONATE_TO_SIBLING);
	else
		return FIELD_PREP_CONST(GUNYAH_MEMEXTENT_OPTION_TYPE_MASK,
					GUNYAH_MEMEXTENT_DONATE_FROM_PROTECTED);
}

int gunyah_vm_provide_folio(struct gunyah_vm *ghvm, struct folio *folio,
			    u64 gfn, bool share, bool write)
{
	struct gunyah_resource *guest_extent, *host_extent, *addrspace;
	u32 map_flags = BIT(GUNYAH_ADDRSPACE_MAP_FLAG_PARTIAL);
	u64 extent_attrs, gpa = gunyah_gfn_to_gpa(gfn);
	phys_addr_t pa = PFN_PHYS(folio_pfn(folio));
	enum gunyah_pagetable_access access;
	size_t size = folio_size(folio);
	enum gunyah_error gunyah_error;
	unsigned long tag = 0;
	int ret;

	if (share) {
		guest_extent = __first_resource(&ghvm->guest_shared_extent_ticket);
		host_extent = __first_resource(&ghvm->host_shared_extent_ticket);
	} else {
		guest_extent = __first_resource(&ghvm->guest_private_extent_ticket);
		host_extent = __first_resource(&ghvm->host_private_extent_ticket);
	}
	addrspace = __first_resource(&ghvm->addrspace_ticket);

	if (!addrspace || !guest_extent || !host_extent)
		return -ENODEV;

	if (share) {
		map_flags |= BIT(GUNYAH_ADDRSPACE_MAP_FLAG_VMMIO);
		tag |= SHARE_TAG;
	} else {
		map_flags |= BIT(GUNYAH_ADDRSPACE_MAP_FLAG_PRIVATE);
	}

	if (write)
		tag |= WRITE_TAG;

	ret = mtree_insert_range(&ghvm->gm, gfn,
				 gfn + folio_nr_pages(folio) - 1,
				 xa_tag_pointer(folio, tag), GFP_KERNEL);
	if (ret)
		return ret;

	if (share && write)
		access = GUNYAH_PAGETABLE_ACCESS_RW;
	else if (share && !write)
		access = GUNYAH_PAGETABLE_ACCESS_R;
	else if (!share && write)
		access = GUNYAH_PAGETABLE_ACCESS_RWX;
	else /* !share && !write */
		access = GUNYAH_PAGETABLE_ACCESS_RX;

	gunyah_error = gunyah_hypercall_memextent_donate(donate_flags(share),
							 host_extent->capid,
							 guest_extent->capid,
							 pa, size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		pr_err("Failed to donate memory for guest address 0x%016llx: %d\n",
		       gpa, gunyah_error);
		ret = gunyah_error_remap(gunyah_error);
		goto remove;
	}

	extent_attrs =
		FIELD_PREP_CONST(GUNYAH_MEMEXTENT_MAPPING_TYPE,
				 ARCH_GUNYAH_DEFAULT_MEMTYPE) |
		FIELD_PREP(GUNYAH_MEMEXTENT_MAPPING_USER_ACCESS, access) |
		FIELD_PREP(GUNYAH_MEMEXTENT_MAPPING_KERNEL_ACCESS, access);
	gunyah_error = gunyah_hypercall_addrspace_map(addrspace->capid,
						      guest_extent->capid, gpa,
						      extent_attrs, map_flags,
						      pa, size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		pr_err("Failed to map guest address 0x%016llx: %d\n", gpa,
		       gunyah_error);
		ret = gunyah_error_remap(gunyah_error);
		goto memextent_reclaim;
	}

	folio_get(folio);
	return 0;
memextent_reclaim:
	gunyah_error = gunyah_hypercall_memextent_donate(reclaim_flags(share),
							 guest_extent->capid,
							 host_extent->capid, pa,
							 size);
	if (gunyah_error != GUNYAH_ERROR_OK)
		pr_err("Failed to reclaim memory donation for guest address 0x%016llx: %d\n",
		       gpa, gunyah_error);
remove:
	mtree_erase(&ghvm->gm, gfn);
	return ret;
}

int gunyah_vm_reclaim_folio(struct gunyah_vm *ghvm, u64 gfn)
{
	const u32 map_flags = BIT(GUNYAH_ADDRSPACE_MAP_FLAG_PARTIAL);
	struct gunyah_resource *guest_extent, *host_extent, *addrspace;
	enum gunyah_pagetable_access access;
	enum gunyah_error gunyah_error;
	struct folio *folio;
	bool write, share;
	phys_addr_t pa;
	size_t size;
	void *entry;
	int ret;

	addrspace = __first_resource(&ghvm->addrspace_ticket);
	if (!addrspace)
		return -ENODEV;

	entry = mtree_load(&ghvm->gm, gfn);
	if (!entry)
		return 0;

	share = !!(xa_pointer_tag(entry) & SHARE_TAG);
	write = !!(xa_pointer_tag(entry) & WRITE_TAG);
	folio = xa_untag_pointer(entry);
	folio_lock(folio);
	if (mtree_load(&ghvm->gm, gfn) != entry) {
		ret = -EAGAIN;
		goto err;
	}

	if (share) {
		guest_extent = __first_resource(&ghvm->guest_shared_extent_ticket);
		host_extent = __first_resource(&ghvm->host_shared_extent_ticket);
	} else {
		guest_extent = __first_resource(&ghvm->guest_private_extent_ticket);
		host_extent = __first_resource(&ghvm->host_private_extent_ticket);
	}

	pa = PFN_PHYS(folio_pfn(folio));
	size = folio_size(folio);

	gunyah_error = gunyah_hypercall_addrspace_unmap(addrspace->capid,
							guest_extent->capid,
							gfn << PAGE_SHIFT,
							map_flags, pa, size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		ret = gunyah_error_remap(gunyah_error);
		goto err;
	}

	gunyah_error = gunyah_hypercall_memextent_donate(reclaim_flags(share),
							 guest_extent->capid,
							 host_extent->capid, pa,
							 size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		pr_err_ratelimited(
			"Failed to reclaim memory donation for guest address 0x%016llx: %d\n",
			gunyah_gfn_to_gpa(gfn), gunyah_error);
		ret = gunyah_error_remap(gunyah_error);
		goto err;
	}

	if (share && write)
		access = GUNYAH_PAGETABLE_ACCESS_RW;
	else if (share && !write)
		access = GUNYAH_PAGETABLE_ACCESS_R;
	else if (!share && write)
		access = GUNYAH_PAGETABLE_ACCESS_RWX;
	else /* !share && !write */
		access = GUNYAH_PAGETABLE_ACCESS_RX;

	gunyah_error = gunyah_hypercall_memextent_donate(donate_flags(share),
							 guest_extent->capid,
							 host_extent->capid, pa,
							 size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		pr_err("Failed to reclaim memory donation for guest address 0x%016llx: %d\n",
		       gfn << PAGE_SHIFT, gunyah_error);
		ret = gunyah_error_remap(gunyah_error);
		goto err;
	}

	BUG_ON(mtree_erase(&ghvm->gm, gfn) != entry);

	folio_unlock(folio);
	folio_put(folio);
	return 0;
err:
	folio_unlock(folio);
	return ret;
}

void gunyah_vm_reclaim_memory(struct gunyah_vm *ghvm)
{
	unsigned long gfn = 0;
	void *entry;
	int ret = 0;

	mt_for_each(&ghvm->gm, entry, gfn, ULONG_MAX) {
		if (xa_pointer_tag(entry) == MEM_PARCEL_TAG) {
			ret = gunyah_vm_reclaim_parcel(
				ghvm, xa_untag_pointer(entry), gfn);
			if (ret)
				dev_err(ghvm->parent,
					"Failed to reclaim guest parcel at %lx: %d\n",
					gfn << PAGE_SHIFT, ret);
		} else {
			gunyah_vm_reclaim_folio(ghvm, gfn);
		}
	}
}
