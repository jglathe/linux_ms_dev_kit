// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "gunyah_guest_mem: " fmt

#include <linux/anon_inodes.h>
#include <linux/types.h>
#include <linux/falloc.h>
#include <linux/file.h>
#include <linux/maple_tree.h>
#include <linux/migrate.h>
#include <linux/pagemap.h>

#include <uapi/linux/gunyah.h>

#include "vm_mgr.h"

/**
 * struct gunyah_gmem_binding - Represents a binding of guestmem to a Gunyah VM
 * @gfn: Guest address to place acquired folios
 * @ghvm: Pointer to Gunyah VM in this binding
 * @mt: Maple tree to track folios which have been provided to the VM
 * @i_off: offset into the guestmem to grab folios from
 * @inode: Pointer to guest mem inode
 * @i_entry: list entry for inode->i_private_list
 * @flags: Access flags for the binding
 * @nr: Number of pages covered by this binding
 */
struct gunyah_gmem_binding {
	u64 gfn;
	struct gunyah_vm *ghvm;
	struct maple_tree mt;

	pgoff_t i_off;
	struct inode *inode;
	struct list_head i_entry;

	u32 flags;
	unsigned long nr;
};

static inline pgoff_t gunyah_gfn_to_off(struct gunyah_gmem_binding *b, u64 gfn)
{
	return gfn - b->gfn + b->i_off;
}

static inline u64 gunyah_off_to_gfn(struct gunyah_gmem_binding *b, pgoff_t off)
{
	return off - b->i_off + b->gfn;
}

static inline bool gunyah_guest_mem_is_lend(struct gunyah_vm *ghvm, u32 flags)
{
	u8 access = flags & GUNYAH_MEM_ACCESS_MASK;

	if (access == GUNYAH_MEM_FORCE_LEND)
		return true;
	else if (access == GUNYAH_MEM_FORCE_SHARE)
		return false;

	/* RM requires all VMs to be protected (isolated) */
	return true;
}

static struct folio *gunyah_gmem_get_huge_folio(struct inode *inode,
						pgoff_t index)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	unsigned long huge_index = round_down(index, HPAGE_PMD_NR);
	unsigned long flags = (unsigned long)inode->i_private;
	struct address_space *mapping = inode->i_mapping;
	gfp_t gfp = mapping_gfp_mask(mapping);
	struct folio *folio;

	if (!(flags & GHMF_ALLOW_HUGEPAGE))
		return NULL;

	if (filemap_range_has_page(mapping, huge_index << PAGE_SHIFT,
				   (huge_index + HPAGE_PMD_NR - 1)
					   << PAGE_SHIFT))
		return NULL;

	folio = filemap_alloc_folio(gfp, HPAGE_PMD_ORDER);
	if (!folio)
		return NULL;

	if (filemap_add_folio(mapping, folio, huge_index, gfp)) {
		folio_put(folio);
		return NULL;
	}

	return folio;
#else
	return NULL;
#endif
}

static struct folio *gunyah_gmem_get_folio(struct inode *inode, pgoff_t index)
{
	struct folio *folio;

	folio = gunyah_gmem_get_huge_folio(inode, index);
	if (!folio) {
		folio = filemap_grab_folio(inode->i_mapping, index);
		if (IS_ERR_OR_NULL(folio))
			return NULL;
	}

	/*
	 * Use the up-to-date flag to track whether or not the memory has been
	 * zeroed before being handed off to the guest.  There is no backing
	 * storage for the memory, so the folio will remain up-to-date until
	 * it's removed.
	 */
	if (!folio_test_uptodate(folio)) {
		unsigned long nr_pages = folio_nr_pages(folio);
		unsigned long i;

		for (i = 0; i < nr_pages; i++)
			clear_highpage(folio_page(folio, i));

		folio_mark_uptodate(folio);
	}

	/*
	 * Ignore accessed, referenced, and dirty flags.  The memory is
	 * unevictable and there is no storage to write back to.
	 */
	return folio;
}

static vm_fault_t gunyah_gmem_host_fault(struct vm_fault *vmf)
{
	struct folio *folio;

	folio = gunyah_gmem_get_folio(file_inode(vmf->vma->vm_file),
				      vmf->pgoff);
	if (!folio || folio_test_private(folio)) {
		folio_unlock(folio);
		folio_put(folio);
		return VM_FAULT_SIGBUS;
	}

	vmf->page = folio_file_page(folio, vmf->pgoff);

	return VM_FAULT_LOCKED;
}

static const struct vm_operations_struct gunyah_gmem_vm_ops = {
	.fault = gunyah_gmem_host_fault,
};

static int gunyah_gmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	file_accessed(file);
	vma->vm_ops = &gunyah_gmem_vm_ops;
	return 0;
}

static long gunyah_gmem_punch_hole(struct inode *inode, loff_t offset,
				   loff_t len)
{
	truncate_inode_pages_range(inode->i_mapping, offset, offset + len - 1);

	return 0;
}

static long gunyah_gmem_allocate(struct inode *inode, loff_t offset, loff_t len)
{
	struct address_space *mapping = inode->i_mapping;
	pgoff_t start, index, end;
	int r;

	/* Dedicated guest is immutable by default. */
	if (offset + len > i_size_read(inode))
		return -EINVAL;

	filemap_invalidate_lock_shared(mapping);

	start = offset >> PAGE_SHIFT;
	end = (offset + len) >> PAGE_SHIFT;

	r = 0;
	for (index = start; index < end;) {
		struct folio *folio;

		if (signal_pending(current)) {
			r = -EINTR;
			break;
		}

		folio = gunyah_gmem_get_folio(inode, index);
		if (!folio) {
			r = -ENOMEM;
			break;
		}

		index = folio_next_index(folio);

		folio_unlock(folio);
		folio_put(folio);

		/* 64-bit only, wrapping the index should be impossible. */
		if (WARN_ON_ONCE(!index))
			break;

		cond_resched();
	}

	filemap_invalidate_unlock_shared(mapping);

	return r;
}

static long gunyah_gmem_fallocate(struct file *file, int mode, loff_t offset,
				  loff_t len)
{
	long ret;

	if (!(mode & FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE |
		     FALLOC_FL_ZERO_RANGE))
		return -EOPNOTSUPP;

	if (!PAGE_ALIGNED(offset) || !PAGE_ALIGNED(len))
		return -EINVAL;

	if (mode & FALLOC_FL_PUNCH_HOLE)
		ret = gunyah_gmem_punch_hole(file_inode(file), offset, len);
	else
		ret = gunyah_gmem_allocate(file_inode(file), offset, len);

	if (!ret)
		file_modified(file);
	return ret;
}

static int gunyah_gmem_release(struct inode *inode, struct file *file)
{
	struct gunyah_gmem_binding *b, *n;

	gunyah_gmem_punch_hole(inode, 0, U64_MAX);

	list_for_each_entry_safe(b, n, &inode->i_mapping->i_private_list,
				 i_entry) {
		gunyah_gmem_remove_binding(b);
	}

	return 0;
}

static const struct file_operations gunyah_gmem_fops = {
	.owner = THIS_MODULE,
	.llseek = generic_file_llseek,
	.mmap = gunyah_gmem_mmap,
	.open = generic_file_open,
	.fallocate = gunyah_gmem_fallocate,
	.release = gunyah_gmem_release,
};

static const struct address_space_operations gunyah_gmem_aops = {
	.dirty_folio = noop_dirty_folio,
	.migrate_folio = migrate_folio,
	.error_remove_folio = generic_error_remove_folio,
};

int gunyah_guest_mem_create(struct gunyah_create_mem_args *args)
{
	const char *anon_name = "[gh-gmem]";
	unsigned long fd_flags = 0;
	struct inode *inode;
	struct file *file;
	int fd, err;

	if (!PAGE_ALIGNED(args->size))
		return -EINVAL;

	if (args->flags & ~(GHMF_CLOEXEC | GHMF_ALLOW_HUGEPAGE))
		return -EINVAL;

	if (args->flags & GHMF_CLOEXEC)
		fd_flags |= O_CLOEXEC;

	fd = get_unused_fd_flags(fd_flags);
	if (fd < 0)
		return fd;

	/*
	 * Use the so called "secure" variant, which creates a unique inode
	 * instead of reusing a single inode.  Each guest_memfd instance needs
	 * its own inode to track the size, flags, etc.
	 */
	file = anon_inode_create_getfile(anon_name, &gunyah_gmem_fops, NULL,
					 O_RDWR, NULL);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_fd;
	}

	file->f_flags |= O_LARGEFILE;

	inode = file->f_inode;
	WARN_ON(file->f_mapping != inode->i_mapping);

	inode->i_private = (void *)(unsigned long)args->flags;
	inode->i_mapping->a_ops = &gunyah_gmem_aops;
	inode->i_mode |= S_IFREG;
	inode->i_size = args->size;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_large_folios(inode->i_mapping);
	mapping_set_unmovable(inode->i_mapping);
	// mapping_set_release_always(inode->i_mapping);
	/* Unmovable mappings are supposed to be marked unevictable as well. */
	WARN_ON_ONCE(!mapping_unevictable(inode->i_mapping));

	fd_install(fd, file);
	return fd;

err_fd:
	put_unused_fd(fd);
	return err;
}

void gunyah_gmem_remove_binding(struct gunyah_gmem_binding *b)
{
	mtree_erase(&b->ghvm->mem_layout, b->gfn);
	list_del(&b->i_entry);
	kfree(b);
}

static inline unsigned long gunyah_gmem_page_mask(struct inode *inode)
{
	unsigned long gmem_flags = (unsigned long)inode->i_private;

	if (gmem_flags & GHMF_ALLOW_HUGEPAGE) {
#if IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)
		return HPAGE_PMD_MASK;
#else
		return ULONG_MAX;
#endif
	}

	return PAGE_MASK;
}

static int gunyah_gmem_init_binding(struct gunyah_vm *ghvm, struct inode *inode,
				    struct gunyah_map_mem_args *args,
				    struct gunyah_gmem_binding *binding)
{
	const unsigned long page_mask = ~gunyah_gmem_page_mask(inode);

	if (args->flags & ~(GUNYAH_MEM_ALLOW_RWX | GUNYAH_MEM_ACCESS_MASK))
		return -EINVAL;

	if (args->guest_addr & page_mask)
		return -EINVAL;

	if (args->offset & page_mask)
		return -EINVAL;

	if (args->size & page_mask)
		return -EINVAL;

	binding->gfn = gunyah_gpa_to_gfn(args->guest_addr);
	binding->ghvm = ghvm;
	binding->i_off = args->offset >> PAGE_SHIFT;
	binding->inode = inode;
	binding->flags = args->flags;
	binding->nr = args->size >> PAGE_SHIFT;

	return 0;
}

static int gunyah_gmem_remove_mapping(struct gunyah_vm *ghvm,
				      struct inode *inode,
				      struct gunyah_map_mem_args *args)
{
	struct gunyah_gmem_binding argb;
	struct gunyah_gmem_binding *b = NULL;
	unsigned long start_delta, end_delta;
	int ret;

	ret = gunyah_gmem_init_binding(ghvm, inode, args, &argb);
	if (ret)
		return ret;

	filemap_invalidate_lock(inode->i_mapping);
	list_for_each_entry(b, &inode->i_mapping->i_private_list, i_entry) {
		if (b->ghvm != argb.ghvm || b->flags != argb.flags ||
		    WARN_ON(b->inode != argb.inode))
			continue;
		/* Check if argb guest addresses is within b */
		if (b->gfn > argb.gfn)
			continue;
		if (b->gfn + b->nr < argb.gfn + argb.nr)
			continue;
		start_delta = argb.gfn - b->gfn;
		if (argb.i_off - b->i_off != start_delta)
			continue;
		end_delta = argb.gfn + argb.nr - b->gfn - b->nr;
		if (!start_delta && !end_delta) {
			/* wipe the mapping entirely */
			gunyah_gmem_remove_binding(b);
			goto out;
		} else if (start_delta && !end_delta) {
			/* shrink the end */
			down_write(&ghvm->mem_lock);
			mtree_erase(&b->ghvm->mem_layout, b->gfn);
			b->nr = start_delta;
			ret = mtree_insert_range(&ghvm->mem_layout, b->gfn,
						 b->gfn + b->nr - 1, b,
						 GFP_KERNEL);
			up_write(&ghvm->mem_lock);
			goto out;
		} else if (!start_delta && end_delta) {
			/* Shrink the beginning */
			down_write(&ghvm->mem_lock);
			mtree_erase(&b->ghvm->mem_layout, b->gfn);
			b->gfn += argb.nr;
			b->i_off += argb.nr;
			b->nr -= argb.nr;
			ret = mtree_insert_range(&ghvm->mem_layout, b->gfn,
						 b->gfn + b->nr - 1, b,
						 GFP_KERNEL);
			up_write(&ghvm->mem_lock);
			goto out;
		} else {
			/* TODO: split the mapping into 2 */
			ret = -EINVAL;
			goto out;
		}
	}
	ret = -ENOENT;
out:
	filemap_invalidate_unlock(inode->i_mapping);
	return ret;
}

static bool gunyah_gmem_binding_allowed_overlap(struct gunyah_gmem_binding *a,
						struct gunyah_gmem_binding *b)
{
	/* Bindings can't overlap within a VM. Only one guest mem can
	 * provide for a given guest address
	 */
	if (a->ghvm == b->ghvm && a->gfn + a->nr <= b->gfn &&
	    a->gfn >= b->gfn + b->nr)
		return false;

	/* Gunyah only guarantees we can share a page with one VM and
	 * doesn't (currently) allow us to share same page with multiple VMs,
	 * regardless whether host can also access.
	 */
	if (a->inode == b->inode) {
		if (a->ghvm == b->ghvm) {
			if (gunyah_guest_mem_is_lend(a->ghvm, a->flags) ||
			    gunyah_guest_mem_is_lend(b->ghvm, b->flags))
				return false;
		} else {
			if (a->i_off + a->nr < b->i_off)
				return false;
			if (a->i_off > b->i_off + b->nr)
				return false;
		}
	}

	return true;
}

static int gunyah_gmem_add_mapping(struct gunyah_vm *ghvm, struct inode *inode,
				   struct gunyah_map_mem_args *args)
{
	struct gunyah_gmem_binding *b, *tmp = NULL;
	int ret;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	ret = gunyah_gmem_init_binding(ghvm, inode, args, b);
	if (ret)
		return ret;

	filemap_invalidate_lock(inode->i_mapping);
	list_for_each_entry(tmp, &inode->i_mapping->i_private_list, i_entry) {
		if (!gunyah_gmem_binding_allowed_overlap(b, tmp)) {
			ret = -EEXIST;
			goto unlock;
		}
	}

	ret = mtree_insert_range(&ghvm->mem_layout, b->gfn, b->gfn + b->nr - 1,
				 b, GFP_KERNEL);
	if (ret)
		goto unlock;

	list_add(&b->i_entry, &inode->i_mapping->i_private_list);

unlock:
	filemap_invalidate_unlock(inode->i_mapping);
	return ret;
}

int gunyah_gmem_modify_binding(struct gunyah_vm *ghvm,
			       struct gunyah_map_mem_args *args)
{
	u8 access = args->flags & GUNYAH_MEM_ACCESS_MASK;
	struct file *file;
	int ret = -EINVAL;

	file = fget(args->guest_mem_fd);
	if (!file)
		return -EINVAL;

	if (file->f_op != &gunyah_gmem_fops)
		goto err_file;

	if (args->flags & ~(GUNYAH_MEM_ALLOW_RWX | GUNYAH_MEM_UNMAP | GUNYAH_MEM_ACCESS_MASK))
		goto err_file;

	/* VM needs to have some permissions to the memory */
	if (!(args->flags & GUNYAH_MEM_ALLOW_RWX))
		goto err_file;

	if (access != GUNYAH_MEM_DEFAULT_ACCESS &&
	    access != GUNYAH_MEM_FORCE_LEND && access != GUNYAH_MEM_FORCE_SHARE)
		goto err_file;

	if (!PAGE_ALIGNED(args->guest_addr) || !PAGE_ALIGNED(args->offset) ||
	    !PAGE_ALIGNED(args->size))
		goto err_file;

	if (args->flags & GUNYAH_MEM_UNMAP) {
		args->flags &= ~GUNYAH_MEM_UNMAP;
		ret = gunyah_gmem_remove_mapping(ghvm, file_inode(file), args);
	} else {
		ret = gunyah_gmem_add_mapping(ghvm, file_inode(file), args);
	}

err_file:
	fput(file);
	return ret;
}

int gunyah_gmem_share_parcel(struct gunyah_vm *ghvm, struct gunyah_rm_mem_parcel *parcel,
			     u64 *gfn, u64 *nr)
{
	struct folio *folio, *prev_folio;
	unsigned long nr_entries, i, j, start, end;
	struct gunyah_gmem_binding *b;
	bool lend;
	u64 size;
	int ret;

	if (!*nr)
		return -EINVAL;

	down_read(&ghvm->mem_lock);
	b = mtree_load(&ghvm->mem_layout, *gfn);
	if (!b) {
		ret = -ENOENT;
		goto unlock;
	}

	start = *gfn - b->gfn;
	end = min(*gfn + *nr, b->gfn + b->nr) - b->gfn;

	nr_entries = 0;
	prev_folio = NULL;
	for (i = start + b->i_off; i < end + b->i_off;) {
		folio = gunyah_gmem_get_folio(b->inode, i); /* A */
		if (!folio) {
			ret = -ENOMEM;
			goto out;
		}

		if (!prev_folio || folio_next(prev_folio) != folio)
			nr_entries++;
		i = folio_index(folio) + folio_nr_pages(folio);
		prev_folio = folio;
	}

	parcel->mem_entries =
		kcalloc(nr_entries, sizeof(*parcel->mem_entries), GFP_KERNEL);
	if (!parcel->mem_entries) {
		ret = -ENOMEM;
		goto out;
	}

	j = 0;
	prev_folio = NULL;
	size = 0;
	for (i = start + b->i_off; i < end + b->i_off;) {
		folio = filemap_get_folio(b->inode->i_mapping, i); /* B */
		if (WARN_ON(IS_ERR(folio))) {
			ret = PTR_ERR(folio);
			i = end + b->i_off;
			goto out;
		}

		if (!prev_folio || folio_next(prev_folio) != folio) {
			if (likely(prev_folio)) {
				parcel->mem_entries[j].size = cpu_to_le64(size);
				size = 0;
				j++;
				BUG_ON(j == nr_entries);
			}
			parcel->mem_entries[j].phys_addr = cpu_to_le64(PFN_PHYS(folio_pfn(folio)));
		}
		size += folio_size(folio);
		if (likely(prev_folio))
			folio_put(prev_folio); /* B */
		i = folio_index(folio) + folio_nr_pages(folio);
		prev_folio = folio;
	}
	folio_put(prev_folio); /* B */
	BUG_ON(j + 1 != nr_entries);
	parcel->mem_entries[j].size = cpu_to_le64(size);
	parcel->n_mem_entries = nr_entries;

	lend = parcel->n_acl_entries == 1 || gunyah_guest_mem_is_lend(ghvm, b->flags);
	if (lend)
		parcel->n_acl_entries = 1;

	parcel->acl_entries = kcalloc(parcel->n_acl_entries,
				      sizeof(*parcel->acl_entries), GFP_KERNEL);
	if (!parcel->n_acl_entries) {
		ret = -ENOMEM;
		goto free_entries;
	}

	parcel->acl_entries[0].vmid = cpu_to_le16(ghvm->vmid);
	if (b->flags & GUNYAH_MEM_ALLOW_READ)
		parcel->acl_entries[0].perms |= GUNYAH_RM_ACL_R;
	if (b->flags & GUNYAH_MEM_ALLOW_WRITE)
		parcel->acl_entries[0].perms |= GUNYAH_RM_ACL_W;
	if (b->flags & GUNYAH_MEM_ALLOW_EXEC)
		parcel->acl_entries[0].perms |= GUNYAH_RM_ACL_X;

	if (!lend) {
		u16 host_vmid;

		ret = gunyah_rm_get_vmid(ghvm->rm, &host_vmid);
		if (ret)
			goto free_acl;

		parcel->acl_entries[1].vmid = cpu_to_le16(host_vmid);
		parcel->acl_entries[1].perms = GUNYAH_RM_ACL_R | GUNYAH_RM_ACL_W | GUNYAH_RM_ACL_X;
	}

	parcel->mem_handle = GUNYAH_MEM_HANDLE_INVAL;
	*nr = end;
	folio = filemap_get_folio(b->inode->i_mapping, start); /* C */
	*gfn = folio_index(folio) - b->i_off + b->gfn;
	folio_put(folio); /* C */

	ret = gunyah_rm_mem_share(ghvm->rm, parcel);
	if (ret)
		dev_warn(ghvm->parent, "Failed to share parcel for DTB: %d\n",
			 ret);

	goto out;
free_acl:
	kfree(parcel->acl_entries);
	parcel->acl_entries = NULL;
free_entries:
	kfree(parcel->mem_entries);
	parcel->mem_entries = NULL;
out:
	for (j = start + b->i_off; j < i;) {
		folio = filemap_get_folio(b->inode->i_mapping, j); /* D */
		if (IS_ERR(folio))
			continue;
		j = folio_index(folio) + folio_nr_pages(folio);
		if (!ret) {
			folio_set_private(folio);
			WARN_ON(mtree_insert_range(&b->mt, folio_index(folio),
						   j - 1, folio, GFP_KERNEL));
		}
		folio_unlock(folio); /* A */
		folio_put(folio); /* D */
		/* matching folio_put for A is done at
		 * (1) gunyah_gmem_reclaim_parcel or
		 * (2) after gunyah_gmem_parcel_to_paged, gunyah_vm_reclaim_folio
		 */
	}
unlock:
	up_read(&ghvm->mem_lock);
	return ret;
}

int gunyah_gmem_reclaim_parcel(struct gunyah_vm *ghvm,
			       struct gunyah_rm_mem_parcel *parcel, u64 gfn,
			       u64 nr)
{
	struct gunyah_gmem_binding *b;
	struct folio *folio;
	unsigned long i;
	int ret;

	if (parcel->mem_handle != GUNYAH_MEM_HANDLE_INVAL) {
		ret = gunyah_rm_mem_reclaim(ghvm->rm, parcel);
		if (ret) {
			dev_err(ghvm->parent, "Failed to reclaim parcel: %d\n",
				ret);
			/* We can't reclaim the pages -- hold onto the pages
			 * forever because we don't know what state the memory
			 * is in
			 */
			return ret;
		}
		parcel->mem_handle = 0;
		kfree(parcel->mem_entries);
		kfree(parcel->acl_entries);
	}

	down_read(&ghvm->mem_lock);
	b = mtree_load(&ghvm->mem_layout, gfn);
	if (WARN_ON(!b)) {
		up_read(&ghvm->mem_lock);
		return 0;
	}

	i = gfn - b->gfn + b->i_off;
	mt_for_each(&b->mt, folio, i, nr + b->i_off) {
		mtree_erase(&b->mt, i);
		folio_put(folio); /* A */
	}
	up_read(&ghvm->mem_lock);
	return 0;
}

int gunyah_gmem_setup_demand_paging(struct gunyah_vm *ghvm)
{
	struct gunyah_rm_mem_entry *entries;
	struct gunyah_gmem_binding *b;
	unsigned long index = 0;
	u32 count = 0, i;
	int ret = 0;

	down_read(&ghvm->mem_lock);
	mt_for_each(&ghvm->mem_layout, b, index, ULONG_MAX)
		if (gunyah_guest_mem_is_lend(ghvm, b->flags))
			count++;

	if (!count)
		goto out;

	entries = kcalloc(count, sizeof(*entries), GFP_KERNEL);
	if (!entries) {
		ret = -ENOMEM;
		goto out;
	}

	index = i = 0;
	mt_for_each(&ghvm->mem_layout, b, index, ULONG_MAX) {
		if (!gunyah_guest_mem_is_lend(ghvm, b->flags))
			continue;
		entries[i].phys_addr = cpu_to_le64(gunyah_gfn_to_gpa(b->gfn));
		entries[i].size = cpu_to_le64(b->nr << PAGE_SHIFT);
		if (++i == count)
			break;
	}

	ret = gunyah_rm_vm_set_demand_paging(ghvm->rm, ghvm->vmid, i, entries);
	kfree(entries);
out:
	up_read(&ghvm->mem_lock);
	return ret;
}

int gunyah_gmem_demand_page(struct gunyah_vm *ghvm, u64 gpa, bool write)
{
	unsigned long gfn = gunyah_gpa_to_gfn(gpa);
	struct gunyah_gmem_binding *b;
	struct folio *folio;
	int ret;

	down_read(&ghvm->mem_lock);
	b = mtree_load(&ghvm->mem_layout, gfn);
	if (!b) {
		up_read(&ghvm->mem_lock);
		return -ENOENT;
	}

	if (write && !(b->flags & GUNYAH_MEM_ALLOW_WRITE))
		return -EPERM;

	folio = gunyah_gmem_get_folio(b->inode, gunyah_gfn_to_off(b, gfn));
	if (IS_ERR(folio)) {
		up_read(&ghvm->mem_lock);
		pr_err_ratelimited(
			"Failed to obtain memory for guest addr %016llx: %ld\n",
			gpa, PTR_ERR(folio));
		return PTR_ERR(folio);
	}

	if (gunyah_guest_mem_is_lend(ghvm, b->flags) && folio_mapped(folio)) {
		ret = -EPERM;
		goto out;
	}

	gfn = gunyah_off_to_gfn(b, folio_index(folio));

	ret = gunyah_vm_provide_folio(ghvm, folio, gfn,
				      !gunyah_guest_mem_is_lend(ghvm, b->flags),
				      !!(b->flags & GUNYAH_MEM_ALLOW_WRITE));
	if (ret) {
		pr_err_ratelimited(
			"Failed to provide folio for guest addr: %016llx: %d\n",
			gpa, ret);
		goto out;
	}

	if (gunyah_guest_mem_is_lend(ghvm, b->flags))
		folio_set_private(folio);
out:
	folio_unlock(folio);
	folio_put(folio);
	up_read(&ghvm->mem_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_gmem_demand_page);
