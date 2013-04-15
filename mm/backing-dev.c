
#include <linux/wait.h>
#include <linux/backing-dev.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/writeback.h>
#include <linux/device.h>

void default_unplug_io_fn(struct backing_dev_info *bdi, struct page *page)
{
}
EXPORT_SYMBOL(default_unplug_io_fn);

struct backing_dev_info default_backing_dev_info = {
        .name           = "default",
        .ra_pages       = VM_MAX_READAHEAD * 1024 / PAGE_CACHE_SIZE,
        .state          = 0,
        .capabilities   = BDI_CAP_MAP_COPY,
        .unplug_io_fn   = default_unplug_io_fn,
};
EXPORT_SYMBOL_GPL(default_backing_dev_info);

struct backing_dev_info noop_backing_dev_info = {
        .name           = "noop",
        .capabilities   = BDI_CAP_NO_ACCT_AND_WRITEBACK,
};
EXPORT_SYMBOL_GPL(noop_backing_dev_info);
 
static struct class *bdi_class;
DEFINE_SPINLOCK(bdi_lock);
LIST_HEAD(bdi_list);
LIST_HEAD(bdi_pending_list);

static struct task_struct *sync_supers_tsk;
static struct timer_list sync_supers_timer;

static int bdi_sync_supers(void *);
static void sync_supers_timer_fn(unsigned long);
static void arm_supers_timer(void);

static void bdi_add_default_flusher_task(struct backing_dev_info *bdi);

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>

static struct dentry *bdi_debug_root;

static void bdi_debug_init(void)
{
	bdi_debug_root = debugfs_create_dir("bdi", NULL);
}

static int bdi_debug_stats_show(struct seq_file *m, void *v)
{
	struct backing_dev_info *bdi = m->private;
	unsigned long background_thresh;
	unsigned long dirty_thresh;
	unsigned long bdi_thresh;

	get_dirty_limits(&background_thresh, &dirty_thresh, &bdi_thresh, bdi);

#define K(x) ((x) << (PAGE_SHIFT - 10))
	seq_printf(m,
		   "BdiWriteback:     %8lu kB\n"
		   "BdiReclaimable:   %8lu kB\n"
		   "BdiDirtyThresh:   %8lu kB\n"
		   "DirtyThresh:      %8lu kB\n"
		   "BackgroundThresh: %8lu kB\n",
		   (unsigned long) K(bdi_stat(bdi, BDI_WRITEBACK)),
		   (unsigned long) K(bdi_stat(bdi, BDI_RECLAIMABLE)),
		   K(bdi_thresh),
		   K(dirty_thresh),
		   K(background_thresh));
#undef K

	return 0;
}

static int bdi_debug_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, bdi_debug_stats_show, inode->i_private);
}

static const struct file_operations bdi_debug_stats_fops = {
	.open		= bdi_debug_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void bdi_debug_register(struct backing_dev_info *bdi, const char *name)
{
	bdi->debug_dir = debugfs_create_dir(name, bdi_debug_root);
	bdi->debug_stats = debugfs_create_file("stats", 0444, bdi->debug_dir,
					       bdi, &bdi_debug_stats_fops);
}

static void bdi_debug_unregister(struct backing_dev_info *bdi)
{
	debugfs_remove(bdi->debug_stats);
	debugfs_remove(bdi->debug_dir);
}
#else
static inline void bdi_debug_init(void)
{
}
static inline void bdi_debug_register(struct backing_dev_info *bdi,
				      const char *name)
{
}
static inline void bdi_debug_unregister(struct backing_dev_info *bdi)
{
}
#endif

static ssize_t read_ahead_kb_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct backing_dev_info *bdi = dev_get_drvdata(dev);
	char *end;
	unsigned long read_ahead_kb;
	ssize_t ret = -EINVAL;

	read_ahead_kb = simple_strtoul(buf, &end, 10);
	if (*buf && (end[0] == '\0' || (end[0] == '\n' && end[1] == '\0'))) {
		bdi->ra_pages = read_ahead_kb >> (PAGE_SHIFT - 10);
		ret = count;
	}
	return ret;
}

#define K(pages) ((pages) << (PAGE_SHIFT - 10))

#define BDI_SHOW(name, expr)						\
static ssize_t name##_show(struct device *dev,				\
			   struct device_attribute *attr, char *page)	\
{									\
	struct backing_dev_info *bdi = dev_get_drvdata(dev);		\
									\
	return snprintf(page, PAGE_SIZE-1, "%lld\n", (long long)expr);	\
}

BDI_SHOW(read_ahead_kb, K(bdi->ra_pages))

static ssize_t min_ratio_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct backing_dev_info *bdi = dev_get_drvdata(dev);
	char *end;
	unsigned int ratio;
	ssize_t ret = -EINVAL;

	ratio = simple_strtoul(buf, &end, 10);
	if (*buf && (end[0] == '\0' || (end[0] == '\n' && end[1] == '\0'))) {
		ret = bdi_set_min_ratio(bdi, ratio);
		if (!ret)
			ret = count;
	}
	return ret;
}
BDI_SHOW(min_ratio, bdi->min_ratio)

static ssize_t max_ratio_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct backing_dev_info *bdi = dev_get_drvdata(dev);
	char *end;
	unsigned int ratio;
	ssize_t ret = -EINVAL;

	ratio = simple_strtoul(buf, &end, 10);
	if (*buf && (end[0] == '\0' || (end[0] == '\n' && end[1] == '\0'))) {
		ret = bdi_set_max_ratio(bdi, ratio);
		if (!ret)
			ret = count;
	}
	return ret;
}
BDI_SHOW(max_ratio, bdi->max_ratio)

#define __ATTR_RW(attr) __ATTR(attr, 0644, attr##_show, attr##_store)

static struct device_attribute bdi_dev_attrs[] = {
	__ATTR_RW(read_ahead_kb),
	__ATTR_RW(min_ratio),
	__ATTR_RW(max_ratio),
	__ATTR_NULL,
};

static __init int bdi_class_init(void)
{
	bdi_class = class_create(THIS_MODULE, "bdi");
	bdi_class->dev_attrs = bdi_dev_attrs;
	bdi_debug_init();
	return 0;
}
postcore_initcall(bdi_class_init);

static int __init default_bdi_init(void)
{
	int err;
	
	sync_supers_tsk = kthread_run(bdi_sync_supers, NULL, "sync_supers");
	BUG_ON(IS_ERR(sync_supers_tsk));

	init_timer(&sync_supers_timer);
	setup_timer(&sync_supers_timer, sync_supers_timer_fn, 0);
	arm_supers_timer();

	err = bdi_init(&default_backing_dev_info);
	if (!err)
		bdi_register(&default_backing_dev_info, NULL, "default");

	return err;
}
subsys_initcall(default_bdi_init);

static void bdi_wb_init(struct bdi_writeback *wb, struct backing_dev_info *bdi)
{
	memset(wb, 0, sizeof(*wb));

	wb->bdi = bdi;
	wb->last_old_flush = jiffies;
	INIT_LIST_HEAD(&wb->b_dirty);
	INIT_LIST_HEAD(&wb->b_io);
	INIT_LIST_HEAD(&wb->b_more_io);
}

static void bdi_task_init(struct backing_dev_info *bdi,
			  struct bdi_writeback *wb)
{
	struct task_struct *tsk = current;

	spin_lock(&bdi->wb_lock);
	list_add_tail_rcu(&wb->list, &bdi->wb_list);
	spin_unlock(&bdi->wb_lock);

	tsk->flags |= PF_FLUSHER | PF_SWAPWRITE;
	set_freezable();

	/*
	 * Our parent may run at a different priority, just set us to normal
	 */
	set_user_nice(tsk, 0);
}

static int bdi_start_fn(void *ptr)
{
	struct bdi_writeback *wb = ptr;
	struct backing_dev_info *bdi = wb->bdi;
	int ret;

	/*
	 * Add us to the active bdi_list
	 */
	spin_lock(&bdi_lock);
	list_add(&bdi->bdi_list, &bdi_list);
	spin_unlock(&bdi_lock);

	bdi_task_init(bdi, wb);

	/*
	 * Clear pending bit and wakeup anybody waiting to tear us down
	 */
	clear_bit(BDI_pending, &bdi->state);
	smp_mb__after_clear_bit();
	wake_up_bit(&bdi->state, BDI_pending);

	ret = bdi_writeback_task(wb);

	/*
	 * Remove us from the list
	 */
	spin_lock(&bdi->wb_lock);
	list_del_rcu(&wb->list);
	spin_unlock(&bdi->wb_lock);

	/*
	 * Flush any work that raced with us exiting. No new work
	 * will be added, since this bdi isn't discoverable anymore.
	 */
	if (!list_empty(&bdi->work_list))
		wb_do_writeback(wb, 1);

	wb->task = NULL;
	return ret;
}

int bdi_has_dirty_io(struct backing_dev_info *bdi)
{
	return wb_has_dirty_io(&bdi->wb);
}

static void bdi_flush_io(struct backing_dev_info *bdi)
{
	struct writeback_control wbc = {
		.bdi			= bdi,
		.sync_mode		= WB_SYNC_NONE,
		.older_than_this	= NULL,
		.range_cyclic		= 1,
		.nr_to_write		= 1024,
	};

	writeback_inodes_wbc(&wbc);
}

/*
 * kupdated() used to do this. We cannot do it from the bdi_forker_task()
 * or we risk deadlocking on ->s_umount. The longer term solution would be
 * to implement sync_supers_bdi() or similar and simply do it from the
 * bdi writeback tasks individually.
 */
static int bdi_sync_supers(void *unused)
{
	set_user_nice(current, 0);

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();

		/*
		 * Do this periodically, like kupdated() did before.
		 */
		sync_supers();
	}

	return 0;
}

static void arm_supers_timer(void)
{
	unsigned long next;

	next = msecs_to_jiffies(dirty_writeback_interval * 10) + jiffies;
	mod_timer(&sync_supers_timer, round_jiffies_up(next));
}

static void sync_supers_timer_fn(unsigned long unused)
{
	wake_up_process(sync_supers_tsk);
	arm_supers_timer();
}

static int bdi_forker_task(void *ptr)
{
	struct bdi_writeback *me = ptr;

	bdi_task_init(me->bdi, me);

	for (;;) {
		struct backing_dev_info *bdi, *tmp;
		struct bdi_writeback *wb;

		/*
		 * Temporary measure, we want to make sure we don't see
		 * dirty data on the default backing_dev_info
		 */
		if (wb_has_dirty_io(me) || !list_empty(&me->bdi->work_list))
			wb_do_writeback(me, 0);

		spin_lock(&bdi_lock);

		/*
		 * Check if any existing bdi's have dirty data without
		 * a thread registered. If so, set that up.
		 */
		list_for_each_entry_safe(bdi, tmp, &bdi_list, bdi_list) {
			if (bdi->wb.task)
				continue;
			if (list_empty(&bdi->work_list) &&
			    !bdi_has_dirty_io(bdi))
				continue;

			bdi_add_default_flusher_task(bdi);
		}

		set_current_state(TASK_INTERRUPTIBLE);

		if (list_empty(&bdi_pending_list)) {
			unsigned long wait;

			spin_unlock(&bdi_lock);
			wait = msecs_to_jiffies(dirty_writeback_interval * 10);
			schedule_timeout(wait);
			try_to_freeze();
			continue;
		}

		__set_current_state(TASK_RUNNING);

		/*
		 * This is our real job - check for pending entries in
		 * bdi_pending_list, and create the tasks that got added
		 */
		bdi = list_entry(bdi_pending_list.next, struct backing_dev_info,
				 bdi_list);
		list_del_init(&bdi->bdi_list);
		spin_unlock(&bdi_lock);

		wb = &bdi->wb;
		wb->task = kthread_run(bdi_start_fn, wb, "flush-%s",
					dev_name(bdi->dev));
		/*
		 * If task creation fails, then readd the bdi to
		 * the pending list and force writeout of the bdi
		 * from this forker thread. That will free some memory
		 * and we can try again.
		 */
		if (IS_ERR(wb->task)) {
			wb->task = NULL;

			/*
			 * Add this 'bdi' to the back, so we get
			 * a chance to flush other bdi's to free
			 * memory.
			 */
			spin_lock(&bdi_lock);
			list_add_tail(&bdi->bdi_list, &bdi_pending_list);
			spin_unlock(&bdi_lock);

			bdi_flush_io(bdi);
		}
	}

	return 0;
}

/*
 * Add the default flusher task that gets created for any bdi
 * that has dirty data pending writeout
 */
void static bdi_add_default_flusher_task(struct backing_dev_info *bdi)
{
	if (!bdi_cap_writeback_dirty(bdi))
		return;

	if (WARN_ON(!test_bit(BDI_registered, &bdi->state))) {
		printk(KERN_ERR "bdi %p/%s is not registered!\n",
							bdi, bdi->name);
		return;
	}

	/*
	 * Check with the helper whether to proceed adding a task. Will only
	 * abort if we two or more simultanous calls to
	 * bdi_add_default_flusher_task() occured, further additions will block
	 * waiting for previous additions to finish.
	 */
	if (!test_and_set_bit(BDI_pending, &bdi->state)) {
		list_move_tail(&bdi->bdi_list, &bdi_pending_list);

		/*
		 * We are now on the pending list, wake up bdi_forker_task()
		 * to finish the job and add us back to the active bdi_list
		 */
		wake_up_process(default_backing_dev_info.wb.task);
	}
}

int bdi_register(struct backing_dev_info *bdi, struct device *parent,
		const char *fmt, ...)
{
	va_list args;
	int ret = 0;
	struct device *dev;

	if (bdi->dev)	/* The driver needs to use separate queues per device */
		goto exit;

	va_start(args, fmt);
	dev = device_create_vargs(bdi_class, parent, MKDEV(0, 0), bdi, fmt, args);
	va_end(args);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto exit;
	}

	spin_lock(&bdi_lock);
	list_add_tail(&bdi->bdi_list, &bdi_list);
	spin_unlock(&bdi_lock);

	bdi->dev = dev;

	/*
	 * Just start the forker thread for our default backing_dev_info,
	 * and add other bdi's to the list. They will get a thread created
	 * on-demand when they need it.
	 */
	if (bdi_cap_flush_forker(bdi)) {
		struct bdi_writeback *wb = &bdi->wb;

		wb->task = kthread_run(bdi_forker_task, wb, "bdi-%s",
						dev_name(dev));
		if (IS_ERR(wb->task)) {
			wb->task = NULL;
			ret = -ENOMEM;

			spin_lock(&bdi_lock);
			list_del(&bdi->bdi_list);
			spin_unlock(&bdi_lock);
			goto exit;
		}
	}

	bdi_debug_register(bdi, dev_name(dev));
	set_bit(BDI_registered, &bdi->state);
exit:
	return ret;
}
EXPORT_SYMBOL(bdi_register);

int bdi_register_dev(struct backing_dev_info *bdi, dev_t dev)
{
	return bdi_register(bdi, NULL, "%u:%u", MAJOR(dev), MINOR(dev));
}
EXPORT_SYMBOL(bdi_register_dev);

/*
 * Remove bdi from the global list and shutdown any threads we have running
 */
static void bdi_wb_shutdown(struct backing_dev_info *bdi)
{
	struct bdi_writeback *wb;

	if (!bdi_cap_writeback_dirty(bdi))
		return;

	/*
	 * If setup is pending, wait for that to complete first
	 */
	wait_on_bit(&bdi->state, BDI_pending, bdi_sched_wait,
			TASK_UNINTERRUPTIBLE);

	/*
	 * Make sure nobody finds us on the bdi_list anymore
	 */
	spin_lock(&bdi_lock);
	list_del(&bdi->bdi_list);
	spin_unlock(&bdi_lock);

	/*
	 * Finally, kill the kernel threads. We don't need to be RCU
	 * safe anymore, since the bdi is gone from visibility.
	 */
	list_for_each_entry(wb, &bdi->wb_list, list)
		kthread_stop(wb->task);
}

void bdi_unregister(struct backing_dev_info *bdi)
{
	if (bdi->dev) {
		if (!bdi_cap_flush_forker(bdi))
			bdi_wb_shutdown(bdi);
		bdi_debug_unregister(bdi);
		device_unregister(bdi->dev);
		bdi->dev = NULL;
	}
}
EXPORT_SYMBOL(bdi_unregister);

int bdi_init(struct backing_dev_info *bdi)
{
	int i, err;

	bdi->dev = NULL;

	bdi->min_ratio = 0;
	bdi->max_ratio = 100;
	bdi->max_prop_frac = PROP_FRAC_BASE;
	spin_lock_init(&bdi->wb_lock);
	INIT_LIST_HEAD(&bdi->bdi_list);
	INIT_LIST_HEAD(&bdi->wb_list);
	INIT_LIST_HEAD(&bdi->work_list);

	bdi_wb_init(&bdi->wb, bdi);

	/*
	 * Just one thread support for now, hard code mask and count
	 */
	bdi->wb_mask = 1;
	bdi->wb_cnt = 1;

	for (i = 0; i < NR_BDI_STAT_ITEMS; i++) {
		err = percpu_counter_init(&bdi->bdi_stat[i], 0);
		if (err)
			goto err;
	}

	bdi->dirty_exceeded = 0;
	err = prop_local_init_percpu(&bdi->completions);

	if (err) {
err:
		while (i--)
			percpu_counter_destroy(&bdi->bdi_stat[i]);
	}

	return err;
}
EXPORT_SYMBOL(bdi_init);

void bdi_destroy(struct backing_dev_info *bdi)
{
	int i;

	WARN_ON(bdi_has_dirty_io(bdi));

	bdi_unregister(bdi);

	for (i = 0; i < NR_BDI_STAT_ITEMS; i++)
		percpu_counter_destroy(&bdi->bdi_stat[i]);

	prop_local_destroy_percpu(&bdi->completions);
}
EXPORT_SYMBOL(bdi_destroy);

static wait_queue_head_t congestion_wqh[2] = {
		__WAIT_QUEUE_HEAD_INITIALIZER(congestion_wqh[0]),
		__WAIT_QUEUE_HEAD_INITIALIZER(congestion_wqh[1])
	};

void clear_bdi_congested(struct backing_dev_info *bdi, int sync)
{
	enum bdi_state bit;
	wait_queue_head_t *wqh = &congestion_wqh[sync];

	bit = sync ? BDI_sync_congested : BDI_async_congested;
	clear_bit(bit, &bdi->state);
	smp_mb__after_clear_bit();
	if (waitqueue_active(wqh))
		wake_up(wqh);
}
EXPORT_SYMBOL(clear_bdi_congested);

void set_bdi_congested(struct backing_dev_info *bdi, int sync)
{
	enum bdi_state bit;

	bit = sync ? BDI_sync_congested : BDI_async_congested;
	set_bit(bit, &bdi->state);
}
EXPORT_SYMBOL(set_bdi_congested);

/**
 * congestion_wait - wait for a backing_dev to become uncongested
 * @sync: SYNC or ASYNC IO
 * @timeout: timeout in jiffies
 *
 * Waits for up to @timeout jiffies for a backing_dev (any backing_dev) to exit
 * write congestion.  If no backing_devs are congested then just wait for the
 * next write to be completed.
 */
long congestion_wait(int sync, long timeout)
{
	long ret;
	DEFINE_WAIT(wait);
	wait_queue_head_t *wqh = &congestion_wqh[sync];

	prepare_to_wait(wqh, &wait, TASK_UNINTERRUPTIBLE);
	ret = io_schedule_timeout(timeout);
	finish_wait(wqh, &wait);
	return ret;
}
EXPORT_SYMBOL(congestion_wait);

