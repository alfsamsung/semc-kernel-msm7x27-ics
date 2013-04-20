#ifndef __LINUX_PERCPU_H
#define __LINUX_PERCPU_H

#include <linux/preempt.h>
#include <linux/slab.h> /* For kmalloc() */
#include <linux/smp.h>
#include <linux/cpumask.h>

#include <asm/percpu.h>

#ifdef CONFIG_SMP
#define DEFINE_PER_CPU(type, name)					\
	__attribute__((__section__(".data.percpu")))			\
	PER_CPU_ATTRIBUTES __typeof__(type) per_cpu__##name

#ifdef MODULE
#define SHARED_ALIGNED_SECTION ".data.percpu"
#else
#define SHARED_ALIGNED_SECTION ".data.percpu.shared_aligned"
#endif

#define DEFINE_PER_CPU_SHARED_ALIGNED(type, name)			\
	__attribute__((__section__(SHARED_ALIGNED_SECTION)))		\
	PER_CPU_ATTRIBUTES __typeof__(type) per_cpu__##name		\
	____cacheline_aligned_in_smp

#define DEFINE_PER_CPU_PAGE_ALIGNED(type, name)			\
	__attribute__((__section__(".data.percpu.page_aligned")))	\
	PER_CPU_ATTRIBUTES __typeof__(type) per_cpu__##name
#else
#define DEFINE_PER_CPU(type, name)					\
	PER_CPU_ATTRIBUTES __typeof__(type) per_cpu__##name

#define DEFINE_PER_CPU_SHARED_ALIGNED(type, name)		      \
	DEFINE_PER_CPU(type, name)

#define DEFINE_PER_CPU_PAGE_ALIGNED(type, name)		      \
	DEFINE_PER_CPU(type, name)
#endif

#define EXPORT_PER_CPU_SYMBOL(var) EXPORT_SYMBOL(per_cpu__##var)
#define EXPORT_PER_CPU_SYMBOL_GPL(var) EXPORT_SYMBOL_GPL(per_cpu__##var)

/* Enough to cover all DEFINE_PER_CPUs in kernel, including modules. */
#ifndef PERCPU_ENOUGH_ROOM
#ifdef CONFIG_MODULES
#define PERCPU_MODULE_RESERVE	8192
#else
#define PERCPU_MODULE_RESERVE	0
#endif

#define PERCPU_ENOUGH_ROOM						\
	(__per_cpu_end - __per_cpu_start + PERCPU_MODULE_RESERVE)
#endif	/* PERCPU_ENOUGH_ROOM */

/*
 * Must be an lvalue. Since @var must be a simple identifier,
 * we force a syntax error here if it isn't.
 */
#define get_cpu_var(var) (*({				\
	extern int simple_identifier_##var(void);	\
	preempt_disable();				\
	&__get_cpu_var(var); }))
#define put_cpu_var(var) preempt_enable()

#ifdef CONFIG_SMP

struct percpu_data {
	void *ptrs[1];
};

#define __percpu_disguise(pdata) (struct percpu_data *)~(unsigned long)(pdata)

/*
 * Use this to get to a cpu's version of the per-cpu object
 * dynamically allocated. Non-atomic access to the current CPU's
 * version should probably be combined with get_cpu()/put_cpu().
 */
#define per_cpu_ptr(ptr, cpu)                                          \
({                                                                     \
        struct percpu_data *__p = __percpu_disguise(ptr);              \
        (__typeof__(ptr))__p->ptrs[(cpu)];                             \
})

extern void *__alloc_percpu(size_t size, size_t align);
extern void free_percpu(void *__pdata);

#else /* CONFIG_SMP */

#define per_cpu_ptr(ptr, cpu) ({ (void)(cpu); (ptr); })

static inline void *__alloc_percpu(size_t size, size_t align)
 {
	/*
	 * Can't easily make larger alignment work with kmalloc.  WARN
	 * on it.  Larger alignment should only be used for module
	 * percpu sections on SMP for which this path isn't used.
	 */
	WARN_ON_ONCE(align > __alignof__(unsigned long long));
	return kzalloc(size, GFP_KERNEL);
}

static inline void free_percpu(void *p)
{
	kfree(p);
}

#endif /* CONFIG_SMP */

#define alloc_percpu(type)     (type *)__alloc_percpu(sizeof(type), \
							__alignof__(type))
#endif /* __LINUX_PERCPU_H */
