/*
 *
 * (C) COPYRIGHT 2011-2012 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained from Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */



/**
 * @file mali_kbase_pm_metrics.c
 * Metrics for power management
 */

#include <osk/mali_osk.h>

#include <kbase/src/common/mali_kbase.h>
#include <kbase/src/common/mali_kbase_pm.h>
#if defined(CONFIG_MALI_T6XX_DVFS) && defined(CONFIG_MACH_MANTA)
#include <kbase/src/platform/mali_kbase_dvfs.h>
#endif

/* When VSync is being hit aim for utilisation between 70-90% */
#define KBASE_PM_VSYNC_MIN_UTILISATION          70
#define KBASE_PM_VSYNC_MAX_UTILISATION          90
/* Otherwise aim for 10-40% */
#define KBASE_PM_NO_VSYNC_MIN_UTILISATION       10
#define KBASE_PM_NO_VSYNC_MAX_UTILISATION       40

static enum hrtimer_restart dvfs_callback(struct hrtimer *timer)
{
	unsigned long flags;
	kbasep_pm_metrics_data * metrics;

	OSK_ASSERT(timer != NULL);

	metrics = container_of(timer, kbasep_pm_metrics_data, timer );

	kbase_platform_dvfs_event(metrics->kbdev);

	spin_lock_irqsave(&metrics->lock, flags);
	if (metrics->timer_active)
	{
		hrtimer_start(timer,
                      HR_TIMER_DELAY_MSEC(KBASE_PM_DVFS_FREQUENCY),
                      HRTIMER_MODE_REL);
	}
	spin_unlock_irqrestore(&metrics->lock, flags);

	return HRTIMER_NORESTART;
}

mali_error kbasep_pm_metrics_init(kbase_device *kbdev)
{
	OSK_ASSERT(kbdev != NULL);

	kbdev->pm.metrics.kbdev = kbdev;
	kbdev->pm.metrics.vsync_hit = 0;
	kbdev->pm.metrics.utilisation = 0;

	kbdev->pm.metrics.time_period_start = ktime_get();
	kbdev->pm.metrics.time_busy = 0;
	kbdev->pm.metrics.time_idle = 0;
	kbdev->pm.metrics.gpu_active = MALI_TRUE;
	kbdev->pm.metrics.timer_active = MALI_TRUE;

	spin_lock_init(&kbdev->pm.metrics.lock);

	hrtimer_init(&kbdev->pm.metrics.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	kbdev->pm.metrics.timer.function = dvfs_callback;

	hrtimer_start(&kbdev->pm.metrics.timer,
                  HR_TIMER_DELAY_MSEC(KBASE_PM_DVFS_FREQUENCY),
                  HRTIMER_MODE_REL);

	kbase_pm_register_vsync_callback(kbdev);

	return MALI_ERROR_NONE;
}
KBASE_EXPORT_TEST_API(kbasep_pm_metrics_init)

mali_bool kbasep_pm_metrics_isactive(kbase_device *kbdev)
{
	mali_bool isactive;
	unsigned long flags;

	OSK_ASSERT(kbdev != NULL);

	spin_lock_irqsave(&kbdev->pm.metrics.lock, flags);
	isactive = (kbdev->pm.metrics.timer_active == MALI_TRUE);
	spin_unlock_irqrestore(&kbdev->pm.metrics.lock, flags);

	return isactive;
}


void kbasep_pm_metrics_term(kbase_device *kbdev)
{
	unsigned long flags;
	OSK_ASSERT(kbdev != NULL);

	spin_lock_irqsave(&kbdev->pm.metrics.lock, flags);
	kbdev->pm.metrics.timer_active = MALI_FALSE;
	spin_unlock_irqrestore(&kbdev->pm.metrics.lock, flags);

	hrtimer_cancel(&kbdev->pm.metrics.timer);

	kbase_pm_unregister_vsync_callback(kbdev);
}
KBASE_EXPORT_TEST_API(kbasep_pm_metrics_term)

void kbasep_pm_record_gpu_idle(kbase_device *kbdev)
{
	unsigned long flags;
	ktime_t now = ktime_get();
	ktime_t diff;

	OSK_ASSERT(kbdev != NULL);

	spin_lock_irqsave(&kbdev->pm.metrics.lock, flags);

	OSK_ASSERT(kbdev->pm.metrics.gpu_active == MALI_TRUE);

	kbdev->pm.metrics.gpu_active = MALI_FALSE;

	diff = ktime_sub( now, kbdev->pm.metrics.time_period_start );

	kbdev->pm.metrics.time_busy += (u32)(ktime_to_ns( diff ) >> KBASE_PM_TIME_SHIFT);
	kbdev->pm.metrics.time_period_start = now;

	spin_unlock_irqrestore(&kbdev->pm.metrics.lock, flags);
}
KBASE_EXPORT_TEST_API(kbasep_pm_record_gpu_idle)

void kbasep_pm_record_gpu_active(kbase_device *kbdev)
{
	unsigned long flags;
	ktime_t now = ktime_get();
	ktime_t diff;

	OSK_ASSERT(kbdev != NULL);

	spin_lock_irqsave(&kbdev->pm.metrics.lock, flags);

	OSK_ASSERT(kbdev->pm.metrics.gpu_active == MALI_FALSE);

	kbdev->pm.metrics.gpu_active = MALI_TRUE;

	diff = ktime_sub( now, kbdev->pm.metrics.time_period_start );

	kbdev->pm.metrics.time_idle += (u32)(ktime_to_ns( diff ) >> KBASE_PM_TIME_SHIFT);
	kbdev->pm.metrics.time_period_start = now;

	spin_unlock_irqrestore(&kbdev->pm.metrics.lock, flags);
}
KBASE_EXPORT_TEST_API(kbasep_pm_record_gpu_active)

void kbase_pm_report_vsync(kbase_device *kbdev, int buffer_updated)
{
	unsigned long flags;
	OSK_ASSERT(kbdev != NULL);

	spin_lock_irqsave(&kbdev->pm.metrics.lock, flags);
	kbdev->pm.metrics.vsync_hit = buffer_updated;
	spin_unlock_irqrestore(&kbdev->pm.metrics.lock, flags);
}
KBASE_EXPORT_TEST_API(kbase_pm_report_vsync)
