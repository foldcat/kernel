// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * This file contains the routines for initializing the MMU
 * on the 8xx series of chips.
 *  -- christophe
 *
 *  Derived from arch/powerpc/mm/40x_mmu.c:
 */

#include <linux/memblock.h>
#include <linux/hugetlb.h>

#include <asm/fixmap.h>

#include <mm/mmu_decl.h>

#define IMMR_SIZE (FIX_IMMR_SIZE << PAGE_SHIFT)

static unsigned long block_mapped_ram;

/*
 * Return PA for this VA if it is in an area mapped with LTLBs or fixmap.
 * Otherwise, returns 0
 */
phys_addr_t v_block_mapped(unsigned long va)
{
	unsigned long p = PHYS_IMMR_BASE;

	if (va >= VIRT_IMMR_BASE && va < VIRT_IMMR_BASE + IMMR_SIZE)
		return p + va - VIRT_IMMR_BASE;
	if (va >= PAGE_OFFSET && va < PAGE_OFFSET + block_mapped_ram)
		return __pa(va);
	return 0;
}

/*
 * Return VA for a given PA mapped with LTLBs or fixmap
 * Return 0 if not mapped
 */
unsigned long p_block_mapped(phys_addr_t pa)
{
	unsigned long p = PHYS_IMMR_BASE;

	if (pa >= p && pa < p + IMMR_SIZE)
		return VIRT_IMMR_BASE + pa - p;
	if (pa < block_mapped_ram)
		return (unsigned long)__va(pa);
	return 0;
}

static pte_t __init *early_hugepd_alloc_kernel(hugepd_t *pmdp, unsigned long va)
{
	if (hpd_val(*pmdp) == 0) {
		pte_t *ptep = memblock_alloc(sizeof(pte_basic_t), SZ_4K);

		if (!ptep)
			return NULL;

		hugepd_populate_kernel((hugepd_t *)pmdp, ptep, PAGE_SHIFT_8M);
		hugepd_populate_kernel((hugepd_t *)pmdp + 1, ptep, PAGE_SHIFT_8M);
	}
	return hugepte_offset(*(hugepd_t *)pmdp, va, PGDIR_SHIFT);
}

static int __ref __early_map_kernel_hugepage(unsigned long va, phys_addr_t pa,
					     pgprot_t prot, int psize, bool new)
{
	pmd_t *pmdp = pmd_off_k(va);
	pte_t *ptep;

	if (WARN_ON(psize != MMU_PAGE_512K && psize != MMU_PAGE_8M))
		return -EINVAL;

	if (new) {
		if (WARN_ON(slab_is_available()))
			return -EINVAL;

		if (psize == MMU_PAGE_512K)
			ptep = early_pte_alloc_kernel(pmdp, va);
		else
			ptep = early_hugepd_alloc_kernel((hugepd_t *)pmdp, va);
	} else {
		if (psize == MMU_PAGE_512K)
			ptep = pte_offset_kernel(pmdp, va);
		else
			ptep = hugepte_offset(*(hugepd_t *)pmdp, va, PGDIR_SHIFT);
	}

	if (WARN_ON(!ptep))
		return -ENOMEM;

	/* The PTE should never be already present */
	if (new && WARN_ON(pte_present(*ptep) && pgprot_val(prot)))
		return -EINVAL;

	set_huge_pte_at(&init_mm, va, ptep,
			pte_mkhuge(pfn_pte(pa >> PAGE_SHIFT, prot)),
			1UL << mmu_psize_to_shift(psize));

	return 0;
}

/*
 * MMU_init_hw does the chip-specific initialization of the MMU hardware.
 */
void __init MMU_init_hw(void)
{
}

static bool immr_is_mapped __initdata;

void __init mmu_mapin_immr(void)
{
	if (immr_is_mapped)
		return;

	immr_is_mapped = true;

	__early_map_kernel_hugepage(VIRT_IMMR_BASE, PHYS_IMMR_BASE,
				    PAGE_KERNEL_NCG, MMU_PAGE_512K, true);
}

static int mmu_mapin_ram_chunk(unsigned long offset, unsigned long top,
			       pgprot_t prot, bool new)
{
	unsigned long v = PAGE_OFFSET + offset;
	unsigned long p = offset;
	int err = 0;

	WARN_ON(!IS_ALIGNED(offset, SZ_512K) || !IS_ALIGNED(top, SZ_512K));

	for (; p < ALIGN(p, SZ_8M) && p < top && !err; p += SZ_512K, v += SZ_512K)
		err = __early_map_kernel_hugepage(v, p, prot, MMU_PAGE_512K, new);
	for (; p < ALIGN_DOWN(top, SZ_8M) && p < top && !err; p += SZ_8M, v += SZ_8M)
		err = __early_map_kernel_hugepage(v, p, prot, MMU_PAGE_8M, new);
	for (; p < ALIGN_DOWN(top, SZ_512K) && p < top && !err; p += SZ_512K, v += SZ_512K)
		err = __early_map_kernel_hugepage(v, p, prot, MMU_PAGE_512K, new);

	if (!new)
		flush_tlb_kernel_range(PAGE_OFFSET + v, PAGE_OFFSET + top);

	return err;
}

unsigned long __init mmu_mapin_ram(unsigned long base, unsigned long top)
{
	unsigned long etext8 = ALIGN(__pa(_etext), SZ_8M);
	unsigned long sinittext = __pa(_sinittext);
	bool strict_boundary = strict_kernel_rwx_enabled() || debug_pagealloc_enabled_or_kfence();
	unsigned long boundary = strict_boundary ? sinittext : etext8;
	unsigned long einittext8 = ALIGN(__pa(_einittext), SZ_8M);

	WARN_ON(top < einittext8);

	mmu_mapin_immr();

	mmu_mapin_ram_chunk(0, boundary, PAGE_KERNEL_TEXT, true);
	if (debug_pagealloc_enabled_or_kfence()) {
		top = boundary;
	} else {
		mmu_mapin_ram_chunk(boundary, einittext8, PAGE_KERNEL_TEXT, true);
		mmu_mapin_ram_chunk(einittext8, top, PAGE_KERNEL, true);
	}

	if (top > SZ_32M)
		memblock_set_current_limit(top);

	block_mapped_ram = top;

	return top;
}

int mmu_mark_initmem_nx(void)
{
	unsigned long etext8 = ALIGN(__pa(_etext), SZ_8M);
	unsigned long sinittext = __pa(_sinittext);
	unsigned long boundary = strict_kernel_rwx_enabled() ? sinittext : etext8;
	unsigned long einittext8 = ALIGN(__pa(_einittext), SZ_8M);
	int err = 0;

	if (!debug_pagealloc_enabled_or_kfence())
		err = mmu_mapin_ram_chunk(boundary, einittext8, PAGE_KERNEL, false);

	mmu_pin_tlb(block_mapped_ram, false);

	return err;
}

#ifdef CONFIG_STRICT_KERNEL_RWX
int mmu_mark_rodata_ro(void)
{
	unsigned long sinittext = __pa(_sinittext);
	int err;

	err = mmu_mapin_ram_chunk(0, sinittext, PAGE_KERNEL_ROX, false);
	if (IS_ENABLED(CONFIG_PIN_TLB_DATA))
		mmu_pin_tlb(block_mapped_ram, true);

	return err;
}
#endif

void __init setup_initial_memory_limit(phys_addr_t first_memblock_base,
				       phys_addr_t first_memblock_size)
{
	/* We don't currently support the first MEMBLOCK not mapping 0
	 * physical on those processors
	 */
	BUG_ON(first_memblock_base != 0);

	/* 8xx can only access 32MB at the moment */
	memblock_set_current_limit(min_t(u64, first_memblock_size, SZ_32M));
}

int pud_clear_huge(pud_t *pud)
{
	 return 0;
}

int pmd_clear_huge(pmd_t *pmd)
{
	 return 0;
}