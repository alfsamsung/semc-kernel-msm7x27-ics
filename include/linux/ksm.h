#ifndef __LINUX_KSM_H
#define __LINUX_KSM_H
/*
 * Memory merging support.
 *
 * This code enables dynamic sharing of identical pages found in different
 * memory areas, even if they are not shared by fork().
 */

#include <linux/bitops.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/sched.h>

struct stable_node;
struct mem_cgroup;

struct page *ksm_does_need_to_copy(struct page *page,
                      struct vm_area_struct *vma, unsigned long address);

#ifdef CONFIG_KSM
int ksm_madvise(struct vm_area_struct *vma, unsigned long start,
		unsigned long end, int advice, unsigned long *vm_flags);
int __ksm_enter(struct mm_struct *mm);
void __ksm_exit(struct mm_struct *mm);

static inline int ksm_fork(struct mm_struct *mm, struct mm_struct *oldmm)
{
	if (test_bit(MMF_VM_MERGEABLE, &oldmm->flags))
		return __ksm_enter(mm);
	return 0;
}

static inline void ksm_exit(struct mm_struct *mm)
{
	if (test_bit(MMF_VM_MERGEABLE, &mm->flags))
		__ksm_exit(mm);
}

/*
 * A KSM page is one of those write-protected "shared pages" or "merged pages"
 * which KSM maps into multiple mms, wherever identical anonymous page content
 * is found in VM_MERGEABLE vmas.  It's a PageAnon page, with NULL anon_vma.
 */
static inline int PageKsm(struct page *page)
{
	return ((unsigned long)page->mapping == PAGE_MAPPING_ANON);
}

static inline struct stable_node *page_stable_node(struct page *page)
{
       return PageKsm(page) ? page_rmapping(page) : NULL;
}

static inline void set_page_stable_node(struct page *page,
                                       struct stable_node *stable_node)
{
       page->mapping = (void *)stable_node +
                               (PAGE_MAPPING_ANON | PAGE_MAPPING_KSM);
}

/*
 * When do_swap_page() first faults in from swap what used to be a KSM page,
 * no problem, it will be assigned to this vma's anon_vma; but thereafter,
 * it might be faulted into a different anon_vma (or perhaps to a different
 * offset in the same anon_vma).  do_swap_page() cannot do all the locking
 * needed to reconstitute a cross-anon_vma KSM page: for now it has to make
 * a copy, and leave remerging the pages to a later pass of ksmd.
 *
 * We'd like to make this conditional on vma->vm_flags & VM_MERGEABLE,
 * but what if the vma was unmerged while the page was swapped out?
 */
static inline int ksm_might_need_to_copy(struct page *page,
                       struct vm_area_struct *vma, unsigned long address)
{
	struct anon_vma *anon_vma = page_anon_vma(page);

       return anon_vma &&
		(anon_vma->root != vma->anon_vma->root ||
		  page->index != linear_page_index(vma, address));
}
/*
 * But we have to avoid the checking which page_add_anon_rmap() performs.
 */
static inline void page_add_ksm_rmap(struct page *page)
{
        if (atomic_inc_and_test(&page->_mapcount)) {
                page->mapping = (void *) PAGE_MAPPING_ANON;
                __inc_zone_page_state(page, NR_ANON_PAGES);
        }
}
 
int page_referenced_ksm(struct page *page,
                       struct mem_cgroup *memcg, unsigned long *vm_flags);
int try_to_unmap_ksm(struct page *page, enum ttu_flags flags);

#else  /* !CONFIG_KSM */

static inline int ksm_madvise(struct vm_area_struct *vma, unsigned long start,
		unsigned long end, int advice, unsigned long *vm_flags)
{
	return 0;
}

static inline int ksm_fork(struct mm_struct *mm, struct mm_struct *oldmm)
{
	return 0;
}

static inline void ksm_exit(struct mm_struct *mm)
{
}

static inline int PageKsm(struct page *page)
{
	return 0;
}

static inline int ksm_might_need_to_copy(struct page *page,
                       struct vm_area_struct *vma, unsigned long address)
{
       return 0;
}

static inline int page_referenced_ksm(struct page *page,
                       struct mem_cgroup *memcg, unsigned long *vm_flags)
{
       return 0;
}

static inline int try_to_unmap_ksm(struct page *page, enum ttu_flags flags)
{
       return 0;
}
#endif /* !CONFIG_KSM */

#endif /* __LINUX_KSM_H */