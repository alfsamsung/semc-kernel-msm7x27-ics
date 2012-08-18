/*
 * Read-Copy Update mechanism for mutual exclusion
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright IBM Corporation, 2001
 *
 * Authors: Dipankar Sarma <dipankar@in.ibm.com>
 *	    Manfred Spraul <manfred@colorfullife.com>
 * 
 * Based on the original work by Paul McKenney <paulmck@us.ibm.com>
 * and inputs from Rusty Russell, Andrea Arcangeli and Andi Kleen.
 * Papers:
 * http://www.rdrop.com/users/paulmck/paper/rclockpdcsproof.pdf
 * http://lse.sourceforge.net/locking/rclock_OLS.2001.05.01c.sc.pdf (OLS2001)
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 * 		http://lse.sourceforge.net/locking/rcupdate.html
 *
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <asm/atomic.h>
#include <linux/bitops.h>
#include <linux/percpu.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/mutex.h>
#include <linux/module.h>

#ifdef CONFIG_DEBUG_LOCK_ALLOC
static struct lock_class_key rcu_lock_key;
struct lockdep_map rcu_lock_map =
       STATIC_LOCKDEP_MAP_INIT("rcu_read_lock", &rcu_lock_key);
EXPORT_SYMBOL_GPL(rcu_lock_map);
#endif

/*
 * Awaken the corresponding synchronize_rcu() instance now that a
 * grace period has elapsed.
 */
void wakeme_after_rcu(struct rcu_head  *head)
{
	struct rcu_synchronize *rcu;

	rcu = container_of(head, struct rcu_synchronize, head);
	complete(&rcu->completion);
}


/*TODO alf remove classic_rcu*/
#if defined(CONFIG_CLASSIC_RCU)
#include <linux/kernel_stat.h>

enum rcu_barrier {
        RCU_BARRIER_STD,
        RCU_BARRIER_BH,
        RCU_BARRIER_SCHED,
};

static DEFINE_PER_CPU(struct rcu_head, rcu_barrier_head) = {NULL};
static atomic_t rcu_barrier_cpu_count;
static DEFINE_MUTEX(rcu_barrier_mutex);
static struct completion rcu_barrier_completion;
int rcu_scheduler_active __read_mostly;

static void rcu_barrier_callback(struct rcu_head *notused)
{
        if (atomic_dec_and_test(&rcu_barrier_cpu_count))
                complete(&rcu_barrier_completion);
}

static void rcu_barrier_func(void *type)
{
        int cpu = smp_processor_id();
        struct rcu_head *head = &per_cpu(rcu_barrier_head, cpu);

        atomic_inc(&rcu_barrier_cpu_count);
        switch ((enum rcu_barrier)type) {
        case RCU_BARRIER_STD:
                call_rcu(head, rcu_barrier_callback);
                break;
        case RCU_BARRIER_BH:
                call_rcu_bh(head, rcu_barrier_callback);
                break;
        case RCU_BARRIER_SCHED:
                call_rcu_sched(head, rcu_barrier_callback);
                break;
        }
}

static void _rcu_barrier(enum rcu_barrier type)
{
        BUG_ON(in_interrupt());
        /* Take cpucontrol mutex to protect against CPU hotplug */
        mutex_lock(&rcu_barrier_mutex);
        init_completion(&rcu_barrier_completion);

atomic_set(&rcu_barrier_cpu_count, 1);
        on_each_cpu(rcu_barrier_func, (void *)type, 1);
        if (atomic_dec_and_test(&rcu_barrier_cpu_count))
                complete(&rcu_barrier_completion);
        wait_for_completion(&rcu_barrier_completion);
        mutex_unlock(&rcu_barrier_mutex);
}

void rcu_barrier(void)
{
        _rcu_barrier(RCU_BARRIER_STD);
}
EXPORT_SYMBOL_GPL(rcu_barrier);

/**
 * rcu_barrier_bh - Wait until all in-flight call_rcu_bh() callbacks complete.
 */
void rcu_barrier_bh(void)
{
        _rcu_barrier(RCU_BARRIER_BH);
}
EXPORT_SYMBOL_GPL(rcu_barrier_bh);

/**
 * rcu_barrier_sched - Wait for in-flight call_rcu_sched() callbacks.
 */
void rcu_barrier_sched(void)
{
        _rcu_barrier(RCU_BARRIER_SCHED);
}
EXPORT_SYMBOL_GPL(rcu_barrier_sched);

void synchronize_rcu(void)
{
        struct rcu_synchronize rcu;

        if (rcu_blocking_is_gp())
                return;

        init_completion(&rcu.completion);
        /* Will wake me after RCU finished. */
        call_rcu(&rcu.head, wakeme_after_rcu);
        /* Wait for it. */
        wait_for_completion(&rcu.completion);
}
EXPORT_SYMBOL_GPL(synchronize_rcu);

void __init rcu_init(void)
{
        __rcu_init();
}

#endif /*TODO alf remove classic_rcu*/ /*--(CONFIG_CLASSIC_RCU)*/ 

#ifdef CONFIG_DEBUG_OBJECTS_RCU_HEAD
static inline void debug_init_rcu_head(struct rcu_head *head)
{
       debug_object_init(head, &rcuhead_debug_descr);
}

static inline void debug_rcu_head_free(struct rcu_head *head)
{
       debug_object_free(head, &rcuhead_debug_descr);
}

/*
 * fixup_init is called when:
 * - an active object is initialized
 */
static int rcuhead_fixup_init(void *addr, enum debug_obj_state state)
{
       struct rcu_head *head = addr;

       switch (state) {
       case ODEBUG_STATE_ACTIVE:
               /*
                * Ensure that queued callbacks are all executed.
                * If we detect that we are nested in a RCU read-side critical
                * section, we should simply fail, otherwise we would deadlock.
                */
               if (rcu_preempt_depth() != 0 || preempt_count() != 0 ||
                   irqs_disabled()) {
                       WARN_ON(1);
                       return 0;
               }
               rcu_barrier();
               rcu_barrier_sched();
               rcu_barrier_bh();
               debug_object_init(head, &rcuhead_debug_descr);
               return 1;
       default:
               return 0;
       }
}

/*
 * fixup_activate is called when:
 * - an active object is activated
 * - an unknown object is activated (might be a statically initialized object)
 * Activation is performed internally by call_rcu().
 */
static int rcuhead_fixup_activate(void *addr, enum debug_obj_state state)
{
       struct rcu_head *head = addr;

       switch (state) {

       case ODEBUG_STATE_NOTAVAILABLE:
               /*
                * This is not really a fixup. We just make sure that it is
                * tracked in the object tracker.
                */
               debug_object_init(head, &rcuhead_debug_descr);
               debug_object_activate(head, &rcuhead_debug_descr);
               return 0;

       case ODEBUG_STATE_ACTIVE:
               /*
                * Ensure that queued callbacks are all executed.
                * If we detect that we are nested in a RCU read-side critical
                * section, we should simply fail, otherwise we would deadlock.
                */
               if (rcu_preempt_depth() != 0 || preempt_count() != 0 ||
                   irqs_disabled()) {
                       WARN_ON(1);
                       return 0;
               }
               rcu_barrier();
               rcu_barrier_sched();
               rcu_barrier_bh();
               debug_object_activate(head, &rcuhead_debug_descr);
               return 1;
       default:
               return 0;
       }
}

/*
 * fixup_free is called when:
 * - an active object is freed
 */
static int rcuhead_fixup_free(void *addr, enum debug_obj_state state)
{
       struct rcu_head *head = addr;

       switch (state) {
       case ODEBUG_STATE_ACTIVE:
               /*
                * Ensure that queued callbacks are all executed.
                * If we detect that we are nested in a RCU read-side critical
                * section, we should simply fail, otherwise we would deadlock.
                */
#ifndef CONFIG_PREEMPT
               WARN_ON(1);
               return 0;
#else
               if (rcu_preempt_depth() != 0 || preempt_count() != 0 ||
                   irqs_disabled()) {
                       WARN_ON(1);
                       return 0;
               }
               rcu_barrier();
               rcu_barrier_sched();
               rcu_barrier_bh();
               debug_object_free(head, &rcuhead_debug_descr);
               return 1;
#endif
       default:
               return 0;
       }
}

/**
 * init_rcu_head_on_stack() - initialize on-stack rcu_head for debugobjects
 * @head: pointer to rcu_head structure to be initialized
 *
 * This function informs debugobjects of a new rcu_head structure that
 * has been allocated as an auto variable on the stack.  This function
 * is not required for rcu_head structures that are statically defined or
 * that are dynamically allocated on the heap.  This function has no
 * effect for !CONFIG_DEBUG_OBJECTS_RCU_HEAD kernel builds.
 */
void init_rcu_head_on_stack(struct rcu_head *head)
{
       debug_object_init_on_stack(head, &rcuhead_debug_descr);
}
EXPORT_SYMBOL_GPL(init_rcu_head_on_stack);

/**
 * destroy_rcu_head_on_stack() - destroy on-stack rcu_head for debugobjects
 * @head: pointer to rcu_head structure to be initialized
 *
 * This function informs debugobjects that an on-stack rcu_head structure
 * is about to go out of scope.  As with init_rcu_head_on_stack(), this
 * function is not required for rcu_head structures that are statically
 * defined or that are dynamically allocated on the heap.  Also as with
 * init_rcu_head_on_stack(), this function has no effect for
 * !CONFIG_DEBUG_OBJECTS_RCU_HEAD kernel builds.
 */
void destroy_rcu_head_on_stack(struct rcu_head *head)
{
       debug_object_free(head, &rcuhead_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_rcu_head_on_stack);

struct debug_obj_descr rcuhead_debug_descr = {
       .name = "rcu_head",
       .fixup_init = rcuhead_fixup_init,
       .fixup_activate = rcuhead_fixup_activate,
       .fixup_free = rcuhead_fixup_free,
};
EXPORT_SYMBOL_GPL(rcuhead_debug_descr);
#endif /* #ifdef CONFIG_DEBUG_OBJECTS_RCU_HEAD */

