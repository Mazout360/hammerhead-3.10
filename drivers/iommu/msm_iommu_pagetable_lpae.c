/* Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/errno.h>
#include <linux/iommu.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include <asm/cacheflush.h>

#include "msm_iommu_priv.h"
#include <trace/events/kmem.h>
#include "msm_iommu_pagetable.h"

#define NUM_FL_PTE      4   /* First level */
#define NUM_SL_PTE      512 /* Second level */
#define NUM_TL_PTE      512 /* Third level */

#define PTE_SIZE	8

#define FL_ALIGN	0x20

/* First-level/second-level page table bits */
#define FL_OFFSET(va)           (((va) & 0xC0000000) >> 30)

#define FLSL_BASE_MASK            (0xFFFFFFF000ULL)
#define FLSL_1G_BLOCK_MASK        (0xFFC0000000ULL)
#define FLSL_BLOCK_MASK           (0xFFFFE00000ULL)
#define FLSL_TYPE_BLOCK           (1 << 0)
#define FLSL_TYPE_TABLE           (3 << 0)
#define FLSL_PTE_TYPE_MASK        (3 << 0)
#define FLSL_APTABLE_RO           (2 << 61)
#define FLSL_APTABLE_RW           (0 << 61)

#define FL_TYPE_SECT              (2 << 0)
#define FL_SUPERSECTION           (1 << 18)
#define FL_AP0                    (1 << 10)
#define FL_AP1                    (1 << 11)
#define FL_AP2                    (1 << 15)
#define FL_SHARED                 (1 << 16)
#define FL_BUFFERABLE             (1 << 2)
#define FL_CACHEABLE              (1 << 3)
#define FL_TEX0                   (1 << 12)
#define FL_NG                     (1 << 17)

/* Second-level page table bits */
#define SL_OFFSET(va)             (((va) & 0x3FE00000) >> 21)

/* Third-level page table bits */
#define TL_OFFSET(va)             (((va) & 0x1FF000) >> 12)

#define TL_TYPE_PAGE              (3 << 0)
#define TL_PAGE_MASK              (0xFFFFFFF000ULL)
#define TL_ATTR_INDEX_MASK        (0x7)
#define TL_ATTR_INDEX_SHIFT       (0x2)
#define TL_NS                     (0x1 << 5)
#define TL_AP_RO                  (0x3 << 6) /* Access Permission: R */
#define TL_AP_RW                  (0x1 << 6) /* Access Permission: RW */
#define TL_SH_ISH                 (0x3 << 8) /* Inner shareable */
#define TL_SH_OSH                 (0x2 << 8) /* Outer shareable */
#define TL_SH_NSH                 (0x0 << 8) /* Non-shareable */
#define TL_AF                     (0x1 << 10)  /* Access Flag */
#define TL_NG                     (0x1 << 11) /* Non-Global */
#define TL_CH                     (0x1ULL << 52) /* Contiguous hint */
#define TL_PXN                    (0x1ULL << 53) /* Privilege Execute Never */
#define TL_XN                     (0x1ULL << 54) /* Execute Never */

/* normal non-cacheable */
#define PTE_MT_BUFFERABLE         (1 << 2)
/* normal inner write-alloc */
#define PTE_MT_WRITEALLOC         (7 << 2)

#define PTE_MT_MASK               (7 << 2)

#define FOLLOW_TO_NEXT_TABLE(pte) ((u64 *) __va(((*pte) & FLSL_BASE_MASK)))

static void __msm_iommu_pagetable_unmap_range(struct msm_iommu_pt *pt, u32 va,
					      u32 len, u32 silent);

static inline void clean_pte(u64 *start, u64 *end,
				s32 redirect)
{
	if (!redirect)
		dmac_flush_range(start, end);
}

s32 msm_iommu_pagetable_alloc(struct msm_iommu_pt *pt)
{
	u32 size = PTE_SIZE * NUM_FL_PTE + FL_ALIGN;
	phys_addr_t fl_table_phys;

	pt->unaligned_fl_table = kzalloc(size, GFP_KERNEL);
	if (!pt->unaligned_fl_table)
		return -ENOMEM;


	fl_table_phys = virt_to_phys(pt->unaligned_fl_table);
	fl_table_phys = ALIGN(fl_table_phys, FL_ALIGN);
	pt->fl_table = phys_to_virt(fl_table_phys);

	clean_pte(pt->fl_table, pt->fl_table + NUM_FL_PTE, pt->redirect);
	return 0;
}

void msm_iommu_pagetable_free(struct msm_iommu_pt *pt)
{
	s32 i;
	u64 *fl_table = pt->fl_table;

	for (i = 0; i < NUM_FL_PTE; ++i) {
		if ((fl_table[i] & FLSL_TYPE_TABLE) == FLSL_TYPE_TABLE) {
			u64 p = fl_table[i] & FLSL_BASE_MASK;
			free_page((u32)phys_to_virt(p));
		}
	}
	kfree(pt->unaligned_fl_table);
	pt->unaligned_fl_table = 0;
	pt->fl_table = 0;
}

#ifdef CONFIG_ARM_LPAE
/*
 * If LPAE is enabled in the ARM processor then just use the same
 * cache policy as the kernel for the SMMU cached mappings.
 */
static inline u32 __get_cache_attr(void)
{
	return pgprot_kernel & PTE_MT_MASK;
}
#else
/*
 * If LPAE is NOT enabled in the ARM processor then hard code the policy.
 * This is mostly for debugging so that we can enable SMMU LPAE without
 * ARM CPU LPAE.
 */
static inline u32 __get_cache_attr(void)
{
	return PTE_MT_WRITEALLOC;
}

#endif

/*
 * Get the IOMMU attributes for the ARM LPAE long descriptor format page
 * table entry bits. The only upper attribute bits we currently use is the
 * contiguous bit which is set when we actually have a contiguous mapping.
 * Lower attribute bits specify memory attributes and the protection
 * (Read/Write/Execute).
 */
static inline void __get_attr(s32 prot, u64 *upper_attr, u64 *lower_attr)
{
	u32 attr_idx = PTE_MT_BUFFERABLE;

	*upper_attr = 0;
	*lower_attr = 0;

	if (!(prot & (IOMMU_READ | IOMMU_WRITE))) {
		prot |= IOMMU_READ | IOMMU_WRITE;
		WARN_ONCE(1, "No attributes in iommu mapping; assuming RW\n");
	}

	if ((prot & IOMMU_WRITE) && !(prot & IOMMU_READ)) {
		prot |= IOMMU_READ;
		WARN_ONCE(1, "Write-only unsupported; falling back to RW\n");
	}

	if (prot & IOMMU_CACHE)
		attr_idx = __get_cache_attr();

	*lower_attr |= attr_idx;
	*lower_attr |= TL_NG | TL_AF;
	*lower_attr |= (prot & IOMMU_CACHE) ? TL_SH_ISH : TL_SH_NSH;
	*lower_attr |= (prot & IOMMU_WRITE) ? TL_AP_RW : TL_AP_RO;
}

static inline u64 *make_second_level_tbl(s32 redirect, u64 *fl_pte)
{
	u64 *sl = (u64 *) __get_free_page(GFP_KERNEL);

	if (!sl) {
		pr_err("Could not allocate second level table\n");
		goto fail;
	}
	memset(sl, 0, SZ_4K);
	clean_pte(sl, sl + NUM_SL_PTE, redirect);

	/* Leave APTable bits 0 to let next level decide access permissinons */
	*fl_pte = (((phys_addr_t)__pa(sl)) & FLSL_BASE_MASK) | FLSL_TYPE_TABLE;
	clean_pte(fl_pte, fl_pte + 1, redirect);
fail:
	return sl;
}

static inline u64 *make_third_level_tbl(s32 redirect, u64 *sl_pte)
{
	u64 *tl = (u64 *) __get_free_page(GFP_KERNEL);

	if (!tl) {
		pr_err("Could not allocate third level table\n");
		goto fail;
	}
	memset(tl, 0, SZ_4K);
	clean_pte(tl, tl + NUM_TL_PTE, redirect);

	/* Leave APTable bits 0 to let next level decide access permissions */
	*sl_pte = (((phys_addr_t)__pa(tl)) & FLSL_BASE_MASK) | FLSL_TYPE_TABLE;

	clean_pte(sl_pte, sl_pte + 1, redirect);
fail:
	return tl;
}

static inline s32 tl_4k_map(u64 *tl_pte, phys_addr_t pa,
			    u64 upper_attr, u64 lower_attr, s32 redirect)
{
	s32 ret = 0;

	if (*tl_pte) {
		ret = -EBUSY;
		goto fail;
	}

	*tl_pte = upper_attr | (pa & TL_PAGE_MASK) | lower_attr | TL_TYPE_PAGE;
	clean_pte(tl_pte, tl_pte + 1, redirect);
fail:
	return ret;
}

static inline s32 tl_64k_map(u64 *tl_pte, phys_addr_t pa,
			     u64 upper_attr, u64 lower_attr, s32 redirect)
{
	s32 ret = 0;
	s32 i;

	for (i = 0; i < 16; ++i)
		if (*(tl_pte+i)) {
			ret = -EBUSY;
			goto fail;
		}

	/* Add Contiguous hint TL_CH */
	upper_attr |= TL_CH;

	for (i = 0; i < 16; ++i)
		*(tl_pte+i) = upper_attr | (pa & TL_PAGE_MASK) |
			      lower_attr | TL_TYPE_PAGE;
	clean_pte(tl_pte, tl_pte + 16, redirect);
fail:
	return ret;
}

static inline s32 sl_2m_map(u64 *sl_pte, phys_addr_t pa,
			    u64 upper_attr, u64 lower_attr, s32 redirect)
{
	s32 ret = 0;

	if (*sl_pte) {
		ret = -EBUSY;
		goto fail;
	}

	*sl_pte = upper_attr | (pa & FLSL_BLOCK_MASK) |
		  lower_attr | FLSL_TYPE_BLOCK;
	clean_pte(sl_pte, sl_pte + 1, redirect);
fail:
	return ret;
}

static inline s32 sl_32m_map(u64 *sl_pte, phys_addr_t pa,
			     u64 upper_attr, u64 lower_attr, s32 redirect)
{
	s32 i;
	s32 ret = 0;

	for (i = 0; i < 16; ++i) {
		if (*(sl_pte+i)) {
			ret = -EBUSY;
			goto fail;
		}
	}

	/* Add Contiguous hint TL_CH */
	upper_attr |= TL_CH;

	for (i = 0; i < 16; ++i)
		*(sl_pte+i) = upper_attr | (pa & FLSL_BLOCK_MASK) |
			      lower_attr | FLSL_TYPE_BLOCK;
	clean_pte(sl_pte, sl_pte + 16, redirect);
fail:
	return ret;
}

static inline s32 fl_1G_map(u64 *fl_pte, phys_addr_t pa,
			    u64 upper_attr, u64 lower_attr, s32 redirect)
{
	s32 ret = 0;

	if (*fl_pte) {
		ret = -EBUSY;
		goto fail;
	}

	*fl_pte = upper_attr | (pa & FLSL_1G_BLOCK_MASK) |
		  lower_attr | FLSL_TYPE_BLOCK;

	clean_pte(fl_pte, fl_pte + 1, redirect);
fail:
	return ret;
}

static inline s32 common_error_check(size_t len, u64 const *fl_table)
{
	s32 ret = 0;

	if (len != SZ_1G && len != SZ_32M && len != SZ_2M &&
	    len != SZ_64K && len != SZ_4K) {
		pr_err("Bad length: %d\n", len);
		ret = -EINVAL;
	} else if (!fl_table) {
		pr_err("Null page table\n");
		ret = -EINVAL;
	}
	return ret;
}

static inline s32 handle_1st_lvl(u64 *fl_pte, phys_addr_t pa, u64 upper_attr,
				     u64 lower_attr, size_t len, s32 redirect)
{
	s32 ret = 0;

	if (len == SZ_1G) {
		ret = fl_1G_map(fl_pte, pa, upper_attr, lower_attr, redirect);
	} else {
		/* Need second level page table */
		if (*fl_pte == 0) {
			if (make_second_level_tbl(redirect, fl_pte) == NULL)
				ret = -ENOMEM;
		}
		if (!ret) {
			if ((*fl_pte & FLSL_TYPE_TABLE) != FLSL_TYPE_TABLE)
				ret = -EBUSY;
		}
	}
	return ret;
}

static inline s32 handle_3rd_lvl(u64 *sl_pte, u32 va, phys_addr_t pa,
				 u64 upper_attr, u64 lower_attr, size_t len,
				 s32 redirect)
{
	u64 *tl_table;
	u64 *tl_pte;
	u32 tl_offset;
	s32 ret = 0;

	/* Need a 3rd level table */
	if (*sl_pte == 0) {
		if (make_third_level_tbl(redirect, sl_pte) == NULL) {
			ret = -ENOMEM;
			goto fail;
		}
	}

	if ((*sl_pte & FLSL_TYPE_TABLE) != FLSL_TYPE_TABLE) {
		ret = -EBUSY;
		goto fail;
	}

	tl_table = FOLLOW_TO_NEXT_TABLE(sl_pte);
	tl_offset = TL_OFFSET(va);
	tl_pte = tl_table + tl_offset;

	if (len == SZ_64K)
		ret = tl_64k_map(tl_pte, pa, upper_attr, lower_attr, redirect);
	else
		ret = tl_4k_map(tl_pte, pa, upper_attr, lower_attr, redirect);

fail:
	return ret;
}

int msm_iommu_pagetable_map(struct msm_iommu_pt *pt, unsigned long va,
			    phys_addr_t pa, size_t len, int prot)
{
	u64 *fl_pte;
	u32 fl_offset;
	u32 sl_offset;
	u64 *sl_table;
	u64 *sl_pte;
	u64 upper_attr;
	u64 lower_attr;
	s32 ret;
	u32 redirect = pt->redirect;

	ret = common_error_check(len, pt->fl_table);
	if (ret)
		goto fail;

	if (!pt->fl_table) {
		pr_err("Null page table\n");
		ret = -EINVAL;
		goto fail;
	}

	__get_attr(prot, &upper_attr, &lower_attr);

	fl_offset = FL_OFFSET(va);
	fl_pte = pt->fl_table + fl_offset;

	ret = handle_1st_lvl(fl_pte, pa, upper_attr, lower_attr, len, redirect);
	if (ret)
		goto fail;

	sl_table = FOLLOW_TO_NEXT_TABLE(fl_pte);
	sl_offset = SL_OFFSET(va);
	sl_pte = sl_table + sl_offset;

	if (len == SZ_32M)
		ret = sl_32m_map(sl_pte, pa, upper_attr, lower_attr, redirect);
	else if (len == SZ_2M)
		ret = sl_2m_map(sl_pte, pa, upper_attr, lower_attr, redirect);
	else if (len == SZ_64K || len == SZ_4K)
		ret = handle_3rd_lvl(sl_pte, va, pa, upper_attr, lower_attr,
				     len, redirect);

fail:
	return ret;
}

static u32 free_table(u64 *prev_level_pte, u64 *table, u32 table_len,
		       s32 redirect, u32 check)
{
	u32 i;
	u32 used = 0;

	if (check) {
		for (i = 0; i < table_len; ++i)
			if (table[i]) {
				used = 1;
				break;
			}
	}
	if (!used) {
		free_page((u32)table);
		*prev_level_pte = 0;
		clean_pte(prev_level_pte, prev_level_pte + 1, redirect);
	}
	return !used;
}

static void fl_1G_unmap(u64 *fl_pte, s32 redirect)
{
	*fl_pte = 0;
	clean_pte(fl_pte, fl_pte + 1, redirect);
}

size_t msm_iommu_pagetable_unmap(struct msm_iommu_pt *pt, unsigned long va,
				size_t len)
{
	msm_iommu_pagetable_unmap_range(pt, va, len);
	return len;
}

static phys_addr_t get_phys_addr(struct scatterlist *sg)
{
	/*
	 * Try sg_dma_address first so that we can
	 * map carveout regions that do not have a
	 * struct page associated with them.
	 */
	phys_addr_t pa = sg_dma_address(sg);
	if (pa == 0)
		pa = sg_phys(sg);
	return pa;
}

static inline s32 is_fully_aligned(u32 va, phys_addr_t pa, size_t len,
				   s32 align)
{
	return  IS_ALIGNED(va | pa, align) && (len >= align);
}

s32 msm_iommu_pagetable_map_range(struct msm_iommu_pt *pt, u32 va,
		       struct scatterlist *sg, u32 len, s32 prot)
{
	phys_addr_t pa;
	u32 offset = 0;
	u64 *fl_pte;
	u64 *sl_pte;
	u32 fl_offset;
	u32 sl_offset;
	u64 *sl_table = NULL;
	u32 chunk_size, chunk_offset = 0;
	s32 ret = 0;
	u64 up_at;
	u64 lo_at;
	u32 redirect = pt->redirect;
	unsigned int start_va = va;

	BUG_ON(len & (SZ_4K - 1));

	if (!pt->fl_table) {
		pr_err("Null page table\n");
		ret = -EINVAL;
		goto fail;
	}

	__get_attr(prot, &up_at, &lo_at);

	pa = get_phys_addr(sg);

	while (offset < len) {
		u32 chunk_left = sg->length - chunk_offset;

		fl_offset = FL_OFFSET(va);
		fl_pte = pt->fl_table + fl_offset;

		chunk_size = SZ_4K;
		if (is_fully_aligned(va, pa, chunk_left, SZ_1G))
			chunk_size = SZ_1G;
		else if (is_fully_aligned(va, pa, chunk_left, SZ_32M))
			chunk_size = SZ_32M;
		else if (is_fully_aligned(va, pa, chunk_left, SZ_2M))
			chunk_size = SZ_2M;
		else if (is_fully_aligned(va, pa, chunk_left, SZ_64K))
			chunk_size = SZ_64K;

		trace_iommu_map_range(va, pa, sg->length, chunk_size);

		ret = handle_1st_lvl(fl_pte, pa, up_at, lo_at,
				     chunk_size, redirect);
		if (ret)
			goto fail;

		sl_table = FOLLOW_TO_NEXT_TABLE(fl_pte);
		sl_offset = SL_OFFSET(va);
		sl_pte = sl_table + sl_offset;

		if (chunk_size == SZ_32M)
			ret = sl_32m_map(sl_pte, pa, up_at, lo_at, redirect);
		else if (chunk_size == SZ_2M)
			ret = sl_2m_map(sl_pte, pa, up_at, lo_at, redirect);
		else if (chunk_size == SZ_64K || chunk_size == SZ_4K)
			ret = handle_3rd_lvl(sl_pte, va, pa, up_at, lo_at,
					     chunk_size, redirect);
		if (ret)
			goto fail;

		offset += chunk_size;
		chunk_offset += chunk_size;
		va += chunk_size;
		pa += chunk_size;

		if (chunk_offset >= sg->length && offset < len) {
			chunk_offset = 0;
			sg = sg_next(sg);
			pa = get_phys_addr(sg);
		}
	}
fail:
	if (ret && offset > 0)
		__msm_iommu_pagetable_unmap_range(pt, start_va, offset, 1);
	return ret;
}

void msm_iommu_pagetable_unmap_range(struct msm_iommu_pt *pt, u32 va, u32 len)
{
	__msm_iommu_pagetable_unmap_range(pt, va, len, 0);
}

static void __msm_iommu_pagetable_unmap_range(struct msm_iommu_pt *pt, u32 va,
					      u32 len, u32 silent)
{
	u32 offset = 0;
	u64 *fl_pte;
	u64 *sl_pte;
	u64 *tl_pte;
	u32 fl_offset;
	u32 sl_offset;
	u64 *sl_table;
	u64 *tl_table;
	u32 sl_start, sl_end;
	u32 tl_start, tl_end;
	u32 redirect = pt->redirect;

	BUG_ON(len & (SZ_4K - 1));

	while (offset < len) {
		u32 entries;
		u32 check;
		u32 left_to_unmap = len - offset;
		u32 type;

		fl_offset = FL_OFFSET(va);
		fl_pte = pt->fl_table + fl_offset;

		if (*fl_pte == 0) {
			if (!silent)
				pr_err("First level PTE is 0 at index 0x%x (offset: 0x%x)\n",
					fl_offset, offset);
			return;
		}
		type = *fl_pte & FLSL_PTE_TYPE_MASK;

		if (type == FLSL_TYPE_BLOCK) {
			fl_1G_unmap(fl_pte, redirect);
			va += SZ_1G;
			offset += SZ_1G;
		} else if (type == FLSL_TYPE_TABLE) {
			sl_table = FOLLOW_TO_NEXT_TABLE(fl_pte);
			sl_offset = SL_OFFSET(va);
			sl_pte = sl_table + sl_offset;
			type = *sl_pte & FLSL_PTE_TYPE_MASK;

			if (type == FLSL_TYPE_BLOCK) {
				sl_start = sl_offset;
				sl_end = (left_to_unmap / SZ_2M) + sl_start;

				if (sl_end > NUM_TL_PTE)
					sl_end = NUM_TL_PTE;

				entries = sl_end - sl_start;

				memset(sl_table + sl_start, 0,
				       entries * sizeof(*sl_pte));

				clean_pte(sl_table + sl_start,
					  sl_table + sl_end, redirect);

				/* If we just unmapped the whole table, don't
				 * bother seeing if there are still used
				 * entries left.
				 */
				check = ((sl_end - sl_start) != NUM_SL_PTE);

				free_table(fl_pte, sl_table, NUM_SL_PTE,
					   redirect, check);

				offset += entries * SZ_2M;
				va += entries * SZ_2M;
			} else if (type == FLSL_TYPE_TABLE) {
				u32 tbl_freed;

				tl_start = TL_OFFSET(va);
				tl_table =  FOLLOW_TO_NEXT_TABLE(sl_pte);
				tl_end = (left_to_unmap / SZ_4K) + tl_start;

				if (tl_end > NUM_TL_PTE)
					tl_end = NUM_TL_PTE;

				entries = tl_end - tl_start;

				memset(tl_table + tl_start, 0,
				       entries * sizeof(*tl_pte));

				clean_pte(tl_table + tl_start,
					  tl_table + tl_end, redirect);

				/* If we just unmapped the whole table, don't
				 * bother seeing if there are still used
				 * entries left.
				 */
				check = entries != NUM_TL_PTE;

				tbl_freed = free_table(sl_pte, tl_table,
						NUM_TL_PTE, redirect, check);
				if (tbl_freed)
					free_table(fl_pte, sl_table, NUM_SL_PTE,
						   redirect, 1);

				offset += entries * SZ_4K;
				va += entries * SZ_4K;
			} else {
				if (!silent)
					pr_err("Second level PTE (0x%llx) is invalid at index 0x%x (offset: 0x%x)\n",
						*sl_pte, sl_offset, offset);
			}
		} else {
			if (!silent)
				pr_err("First level PTE (0x%llx) is invalid at index 0x%x (offset: 0x%x)\n",
					*fl_pte, fl_offset, offset);
		}
	}
}

phys_addr_t msm_iommu_iova_to_phys_soft(struct iommu_domain *domain,
							phys_addr_t va)
{
	pr_err("iova_to_phys is not implemented for LPAE\n");
	return 0;
}

void __init msm_iommu_pagetable_init(void)
{
}
