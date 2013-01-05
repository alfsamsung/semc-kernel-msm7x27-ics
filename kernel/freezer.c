/*
 * kernel/freezer.c - Function to freeze a process
 *
 * Originally from kernel/power/process.c
 */

#include <linux/interrupt.h>
#include <linux/suspend.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/freezer.h>
#include <linux/kthread.h>

/* total number of freezing conditions in effect */
atomic_t system_freezing_cnt = ATOMIC_INIT(0);
EXPORT_SYMBOL(system_freezing_cnt);

/* indicate whether PM freezing is in effect, protected by pm_mutex */
bool pm_freezing;
bool pm_nosig_freezing;

/* protects freezing and frozen transitions */
static DEFINE_SPINLOCK(freezer_lock);

/**
+ * freezing_slow_path - slow path for testing whether a task needs to be frozen
+ * @p: task to be tested
+ *
+ * This function is called by freezing() if system_freezing_cnt isn't zero
+ * and tests whether @p needs to enter and stay in frozen state.  Can be
+ * called under any context.  The freezers are responsible for ensuring the
+ * target tasks see the updated state.
+ */
bool freezing_slow_path(struct task_struct *p)
{
	if (p->flags & PF_NOFREEZE)
		return false;

	if (pm_nosig_freezing || cgroup_freezing_or_frozen(p))
		return true;

	if (pm_freezing && !(p->flags & PF_FREEZER_NOSIG))
		return true;

	return false;
}
EXPORT_SYMBOL(freezing_slow_path);

/* Refrigerator is place where frozen processes are stored :-). */
bool __refrigerator(bool check_kthr_stop)
{
	/* Hmm, should we be allowed to suspend when there are realtime
	   processes around? */
	long save;
	bool was_frozen = false;

	/*
	 * No point in checking freezing() again - the caller already did.
	 * Proceed to enter FROZEN.
	 */
	spin_lock_irq(&freezer_lock);
repeat:
	current->flags |= PF_FROZEN;
	spin_unlock_irq(&freezer_lock);

	save = current->state;
	pr_debug("%s entered refrigerator\n", current->comm);

	spin_lock_irq(&current->sighand->siglock);
	recalc_sigpending(); /* We sent fake signal, clean it up */
	spin_unlock_irq(&current->sighand->siglock);
	
	for (;;) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (!freezing(current) ||
		    (check_kthr_stop && kthread_should_stop()))
			break;
		was_frozen = true;
		schedule();
	}
	
	/* leave FROZEN */
	spin_lock_irq(&freezer_lock);
	if (freezing(current))
		goto repeat;
	current->flags &= ~PF_FROZEN;
	spin_unlock_irq(&freezer_lock);

	pr_debug("%s left refrigerator\n", current->comm);
	/*
        * Restore saved task state before returning.  The mb'd version
        * needs to be used; otherwise, it might silently break
        * synchronization which depends on ordered task state change.
        */
	set_current_state(save);
	
	return was_frozen;
}
EXPORT_SYMBOL(__refrigerator);

static void fake_signal_wake_up(struct task_struct *p)
{
	unsigned long flags;

	spin_lock_irqsave(&p->sighand->siglock, flags);
	signal_wake_up(p, 0);
	spin_unlock_irqrestore(&p->sighand->siglock, flags);
}

/**
 * freeze_task - send a freeze request to given task
 * @p: task to send the request to
 * 
 * If @p is freezing, the freeze request is sent either by sending a fake
 * signal (if it's not a kernel thread) or waking it up (if it's a kernel
 * thread).
 *
 * RETURNS:
 * %false, if @p is not freezing or already frozen; %true, otherwise
 */
bool freeze_task(struct task_struct *p)
{
	unsigned long flags;

	spin_lock_irqsave(&freezer_lock, flags);

	if (!freezing(p) || frozen(p)) {
		      spin_unlock_irqrestore(&freezer_lock, flags);
		      return false;
	      }

	if (should_send_signal(p)) {
		fake_signal_wake_up(p);
		/*
		 * fake_signal_wake_up() goes through p's scheduler
		 * lock and guarantees that TASK_STOPPED/TRACED ->
		 * TASK_RUNNING transition can't race with task state
		 * testing in try_to_freeze_tasks().
		 */
	} else {
		wake_up_state(p, TASK_INTERRUPTIBLE);
	}

	spin_unlock_irqrestore(&freezer_lock, flags);
	return true;
}

void __thaw_task(struct task_struct *p)
{
	unsigned long flags;
	
	/*
        * Clear freezing and kick @p if FROZEN.  Clearing is guaranteed to
        * be visible to @p as waking up implies wmb.  Waking up inside
        * freezer_lock also prevents wakeups from leaking outside
        * refrigerator.
	* 
	* If !FROZEN, @p hasn't reached refrigerator, recalc sigpending to
	* avoid leaving dangling TIF_SIGPENDING behind.
        */
	
	spin_lock_irqsave(&freezer_lock, flags);
	if (frozen(p)) {
		wake_up_process(p);
	} else {
		spin_lock(&p->sighand->siglock);
		recalc_sigpending_and_wake(p);
		spin_unlock(&p->sighand->siglock);
	}
	spin_unlock_irqrestore(&freezer_lock, flags);
}

