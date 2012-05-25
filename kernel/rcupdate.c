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
#include <linux/kernel_stat.h>

#ifdef CONFIG_DEBUG_LOCK_ALLOC
static struct lock_class_key rcu_lock_key;
struct lockdep_map rcu_lock_map =
       STATIC_LOCKDEP_MAP_INIT("rcu_read_lock", &rcu_lock_key);
EXPORT_SYMBOL_GPL(rcu_lock_map);
#endif

int rcu_scheduler_active __read_mostly;
EXPORT_SYMBOL_GPL(rcu_scheduler_active);

/*
 * This function is invoked towards the end of the scheduler's initialization
 * process.  Before this is called, the idle task might contain
 * RCU read-side critical sections (during which time, this idle
 * task is booting the system).  After this function is called, the
 * idle tasks are prohibited from containing RCU read-side critical
 * sections.
 */
void rcu_scheduler_starting(void)
{
       WARN_ON(num_online_cpus() != 1);
       WARN_ON(nr_context_switches() > 0);
       rcu_scheduler_active = 1;
}

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

