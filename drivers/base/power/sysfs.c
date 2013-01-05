/*
 * drivers/base/power/sysfs.c - sysfs entries for device PM
 */

#include <linux/device.h>
#include <linux/string.h>
#include <linux/pm_runtime.h>
#include "power.h"

/*
 * 	control - Report/change current runtime PM setting of the device
 *
 *     Runtime power management of a device can be blocked with the help of
 *     this attribute.  All devices have one of the following two values for
 *     the power/control file:
 *
 *      + "auto\n" to allow the device to be power managed at run time;
 *      + "on\n" to prevent the device from being power managed at run time;
 *
 *     The default for all devices is "auto", which means that devices may be
 *     subject to automatic power management, depending on their drivers.
 *     Changing this attribute to "on" prevents the driver from power managing
 *     the device at run time.  Doing that while the device is suspended causes
 *     it to be woken up.
 * 
 *	wakeup - Report/change current wakeup option for device
 *
 *	Some devices support "wakeup" events, which are hardware signals
 *	used to activate devices from suspended or low power states.  Such
 *	devices have one of three values for the sysfs power/wakeup file:
 *
 *	 + "enabled\n" to issue the events;
 *	 + "disabled\n" not to do so; or
 *	 + "\n" for temporary or permanent inability to issue wakeup.
 *
 *	(For example, unconfigured USB devices can't issue wakeups.)
 *
 *	Familiar examples of devices that can issue wakeup events include
 *	keyboards and mice (both PS2 and USB styles), power buttons, modems,
 *	"Wake-On-LAN" Ethernet links, GPIO lines, and more.  Some events
 *	will wake the entire system from a suspend state; others may just
 *	wake up the device (if the system as a whole is already active).
 *	Some wakeup events use normal IRQ lines; other use special out
 *	of band signaling.
 *
 *	It is the responsibility of device drivers to enable (or disable)
 *	wakeup signaling as part of changing device power states, respecting
 *	the policy choices provided through the driver model.
 *
 *	Devices may not be able to generate wakeup events from all power
 *	states.  Also, the events may be ignored in some configurations;
 *	for example, they might need help from other devices that aren't
 *	active, or which may have wakeup disabled.  Some drivers rely on
 *	wakeup events internally (unless they are disabled), keeping
 *	their hardware in low power modes whenever they're unused.  This
 *	saves runtime power, without requiring system-wide sleep states.
 * 
 * 	wakeup_count - Report the number of wakeup events related to the device
 */

static const char enabled[] = "enabled";
static const char disabled[] = "disabled";

const char power_group_name[] = "power";
EXPORT_SYMBOL_GPL(power_group_name);

#ifdef CONFIG_PM_RUNTIME
static const char ctrl_auto[] = "auto";
static const char ctrl_on[] = "on";

static ssize_t control_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sprintf(buf, "%s\n",
				dev->power.runtime_auto ? ctrl_auto : ctrl_on);
}

static ssize_t control_store(struct device * dev, struct device_attribute *attr,
			      const char * buf, size_t n)
{
	char *cp;
	int len = n;

	cp = memchr(buf, '\n', n);
	if (cp)
		len = cp - buf;
	if (len == sizeof ctrl_auto - 1 && strncmp(buf, ctrl_auto, len) == 0)
		pm_runtime_allow(dev);
	else if (len == sizeof ctrl_on - 1 && strncmp(buf, ctrl_on, len) == 0)
		pm_runtime_forbid(dev);
	else
		return -EINVAL;
	return n;
}

static DEVICE_ATTR(control, 0644, control_show, control_store);
#endif

static ssize_t
wake_show(struct device * dev, struct device_attribute *attr, char * buf)
{
	return sprintf(buf, "%s\n", device_can_wakeup(dev)
		? (device_may_wakeup(dev) ? enabled : disabled)
		: "");
}

static ssize_t
wake_store(struct device * dev, struct device_attribute *attr,
	const char * buf, size_t n)
{
	char *cp;
	int len = n;

	if (!device_can_wakeup(dev))
		return -EINVAL;

	cp = memchr(buf, '\n', n);
	if (cp)
		len = cp - buf;
	if (len == sizeof enabled - 1
			&& strncmp(buf, enabled, sizeof enabled - 1) == 0)
		device_set_wakeup_enable(dev, 1);
	else if (len == sizeof disabled - 1
			&& strncmp(buf, disabled, sizeof disabled - 1) == 0)
		device_set_wakeup_enable(dev, 0);
	else
		return -EINVAL;
	return n;
}

static DEVICE_ATTR(wakeup, 0644, wake_show, wake_store);

#ifdef CONFIG_PM_SLEEP
static ssize_t wakeup_count_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	unsigned long count = 0;
	bool enabled = false;

	spin_lock_irq(&dev->power.lock);
	if (dev->power.wakeup) {
		count = dev->power.wakeup->event_count;
		enabled = true;
	}
	spin_unlock_irq(&dev->power.lock);
	return enabled ? sprintf(buf, "%lu\n", count) : sprintf(buf, "\n");
}

static DEVICE_ATTR(wakeup_count, 0444, wakeup_count_show, NULL);

static ssize_t wakeup_active_count_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
       unsigned long count = 0;
       bool enabled = false;

       spin_lock_irq(&dev->power.lock);
       if (dev->power.wakeup) {
               count = dev->power.wakeup->active_count;
               enabled = true;
       }
       spin_unlock_irq(&dev->power.lock);
       return enabled ? sprintf(buf, "%lu\n", count) : sprintf(buf, "\n");
}

static DEVICE_ATTR(wakeup_active_count, 0444, wakeup_active_count_show, NULL);

static ssize_t wakeup_hit_count_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
       unsigned long count = 0;
       bool enabled = false;

       spin_lock_irq(&dev->power.lock);
       if (dev->power.wakeup) {
               count = dev->power.wakeup->hit_count;
               enabled = true;
       }
       spin_unlock_irq(&dev->power.lock);
       return enabled ? sprintf(buf, "%lu\n", count) : sprintf(buf, "\n");
}

static DEVICE_ATTR(wakeup_hit_count, 0444, wakeup_hit_count_show, NULL);

static ssize_t wakeup_active_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
       unsigned int active = 0;
       bool enabled = false;

       spin_lock_irq(&dev->power.lock);
       if (dev->power.wakeup) {
               active = dev->power.wakeup->active;
               enabled = true;
       }
       spin_unlock_irq(&dev->power.lock);
       return enabled ? sprintf(buf, "%u\n", active) : sprintf(buf, "\n");
}

static DEVICE_ATTR(wakeup_active, 0444, wakeup_active_show, NULL);

static ssize_t wakeup_total_time_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
       s64 msec = 0;
       bool enabled = false;

       spin_lock_irq(&dev->power.lock);
       if (dev->power.wakeup) {
               msec = ktime_to_ms(dev->power.wakeup->total_time);
               enabled = true;
       }
       spin_unlock_irq(&dev->power.lock);
       return enabled ? sprintf(buf, "%lld\n", msec) : sprintf(buf, "\n");
}

static DEVICE_ATTR(wakeup_total_time_ms, 0444, wakeup_total_time_show, NULL);
static ssize_t wakeup_max_time_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
       s64 msec = 0;
       bool enabled = false;

       spin_lock_irq(&dev->power.lock);
       if (dev->power.wakeup) {
               msec = ktime_to_ms(dev->power.wakeup->max_time);
               enabled = true;
       }
       spin_unlock_irq(&dev->power.lock);
       return enabled ? sprintf(buf, "%lld\n", msec) : sprintf(buf, "\n");
}

static DEVICE_ATTR(wakeup_max_time_ms, 0444, wakeup_max_time_show, NULL);

static ssize_t wakeup_last_time_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
       s64 msec = 0;
       bool enabled = false;

       spin_lock_irq(&dev->power.lock);
       if (dev->power.wakeup) {
               msec = ktime_to_ms(dev->power.wakeup->last_time);
               enabled = true;
       }
       spin_unlock_irq(&dev->power.lock);
       return enabled ? sprintf(buf, "%lld\n", msec) : sprintf(buf, "\n");
}

static DEVICE_ATTR(wakeup_last_time_ms, 0444, wakeup_last_time_show, NULL);
#endif /* CONFIG_PM_SLEEP */

static struct attribute *power_attrs[] = {
	NULL,
};
static struct attribute_group pm_attr_group = {
	.name	= "power",
	.name   = power_group_name,
	.attrs	= power_attrs,
};

static struct attribute *wakeup_attrs[] = {
#ifdef CONFIG_PM_SLEEP
       &dev_attr_wakeup.attr,
       &dev_attr_wakeup_count.attr,
       &dev_attr_wakeup_active_count.attr,
       &dev_attr_wakeup_hit_count.attr,
       &dev_attr_wakeup_active.attr,
       &dev_attr_wakeup_total_time_ms.attr,
       &dev_attr_wakeup_max_time_ms.attr,
       &dev_attr_wakeup_last_time_ms.attr,
#endif
       NULL,
};
static struct attribute_group pm_wakeup_attr_group = {
       .name   = power_group_name,
       .attrs  = wakeup_attrs,
};

static struct attribute *runtime_attrs[] = {
#ifdef CONFIG_PM_RUNTIME
#ifndef CONFIG_PM_ADVANCED_DEBUG
       &dev_attr_runtime_status.attr,
#endif
       &dev_attr_control.attr,
       &dev_attr_runtime_suspended_time.attr,
       &dev_attr_runtime_active_time.attr,
#endif /* CONFIG_PM_RUNTIME */
       NULL,
};
static struct attribute_group pm_runtime_attr_group = {
       .name   = power_group_name,
       .attrs  = runtime_attrs,
};

int dpm_sysfs_add(struct device *dev)
{
       int rc;

       rc = sysfs_create_group(&dev->kobj, &pm_attr_group);
       if (rc)
               return rc;

       if (pm_runtime_callbacks_present(dev)) {
               rc = sysfs_merge_group(&dev->kobj, &pm_runtime_attr_group);
               if (rc)
                       goto err_out;
       }

       if (device_can_wakeup(dev)) {
               rc = sysfs_merge_group(&dev->kobj, &pm_wakeup_attr_group);
               if (rc) {
                       if (pm_runtime_callbacks_present(dev))
                               sysfs_unmerge_group(&dev->kobj,
                                                   &pm_runtime_attr_group);
                       goto err_out;
               }
       }
       return 0;

 err_out:
       sysfs_remove_group(&dev->kobj, &pm_attr_group);
       return rc;
}

int wakeup_sysfs_add(struct device *dev)
{
       return sysfs_merge_group(&dev->kobj, &pm_wakeup_attr_group);
}

void wakeup_sysfs_remove(struct device *dev)
{
       sysfs_unmerge_group(&dev->kobj, &pm_wakeup_attr_group);
}

void rpm_sysfs_remove(struct device *dev)
{
	sysfs_unmerge_group(&dev->kobj, &pm_runtime_attr_group);
}

void dpm_sysfs_remove(struct device *dev)
{
	rpm_sysfs_remove(dev);
	sysfs_unmerge_group(&dev->kobj, &pm_wakeup_attr_group);
}

