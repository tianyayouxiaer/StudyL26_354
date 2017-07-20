/*
 * linux/kernel/time/tick-common.c
 *
 * This file contains the base functions to manage periodic tick
 * related events.
 *
 * Copyright(C) 2005-2006, Thomas Gleixner <tglx@linutronix.de>
 * Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 * Copyright(C) 2006-2007, Timesys Corp., Thomas Gleixner
 *
 * This code is licenced under the GPL version 2. For details see
 * kernel-base/COPYING.
 */
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/profile.h>
#include <linux/sched.h>
#include <linux/tick.h>

#include <asm/irq_regs.h>

#include "tick-internal.h"

/*
 * Tick devices
 */
DEFINE_PER_CPU(struct tick_device, tick_cpu_device);
/*
 * Tick next event: keeps track of the tick time
 */
ktime_t tick_next_period;
ktime_t tick_period;
int tick_do_timer_cpu __read_mostly = TICK_DO_TIMER_BOOT;
static DEFINE_RAW_SPINLOCK(tick_device_lock);

/*
 * Debugging: see timer_list.c
 */
struct tick_device *tick_get_device(int cpu)
{
	return &per_cpu(tick_cpu_device, cpu);
}

/**
 * tick_is_oneshot_available - check for a oneshot capable event device
 */
int tick_is_oneshot_available(void)
{
	struct clock_event_device *dev = __get_cpu_var(tick_cpu_device).evtdev;

	return dev && (dev->features & CLOCK_EVT_FEAT_ONESHOT);
}

/*
 * Periodic tick
 */
static void tick_periodic(int cpu)
{
	if (tick_do_timer_cpu == cpu) {
		write_seqlock(&xtime_lock);

		/* Keep track of the next tick event */
		tick_next_period = ktime_add(tick_next_period, tick_period);

		do_timer(1);
		write_sequnlock(&xtime_lock);
	}

	update_process_times(user_mode(get_irq_regs()));
	profile_tick(CPU_PROFILING);
}

/*
 * Event handler for periodic ticks
 */
void tick_handle_periodic(struct clock_event_device *dev)
{
	int cpu = smp_processor_id();
	ktime_t next;

	tick_periodic(cpu);

	if (dev->mode != CLOCK_EVT_MODE_ONESHOT)
		return;
	/*
	 * Setup the next period for devices, which do not have
	 * periodic mode:
	 */
	next = ktime_add(dev->next_event, tick_period);
	for (;;) {
		if (!clockevents_program_event(dev, next, ktime_get()))
			return;
		/*
		 * Have to be careful here. If we're in oneshot mode,
		 * before we call tick_periodic() in a loop, we need
		 * to be sure we're using a real hardware clocksource.
		 * Otherwise we could get trapped in an infinite
		 * loop, as the tick_periodic() increments jiffies,
		 * when then will increment time, posibly causing
		 * the loop to trigger again and again.
		 */
		if (timekeeping_valid_for_hres())
			tick_periodic(cpu);
		next = ktime_add(next, tick_period);
	}
}

/*
 * Setup the device for a periodic tick
 */
void tick_setup_periodic(struct clock_event_device *dev, int broadcast)
{
	tick_set_periodic_handler(dev, broadcast);

	/* Broadcast setup ? */
	if (!tick_device_is_functional(dev))
		return;

	if ((dev->features & CLOCK_EVT_FEAT_PERIODIC) &&
	    !tick_broadcast_oneshot_active()) {
		clockevents_set_mode(dev, CLOCK_EVT_MODE_PERIODIC);
	} else {
		unsigned long seq;
		ktime_t next;

		do {
			seq = read_seqbegin(&xtime_lock);
			next = tick_next_period;
		} while (read_seqretry(&xtime_lock, seq));

		clockevents_set_mode(dev, CLOCK_EVT_MODE_ONESHOT);

		for (;;) {
			if (!clockevents_program_event(dev, next, ktime_get()))
				return;
			next = ktime_add(next, tick_period);
		}
	}
}

/*
 * Setup the tick device
 */
 /*
 因为启动期间，第一个注册的tick_device必然工作在TICKDEV_MODE_PERIODIC模式，
 所以tick_setup_periodic会设置clock_event_device的事件回调字段event_handler
 为tick_handle_periodic，工作一段时间后，就算有新的支持TICKDEV_MODE_ONESHOT
 模式的clock_event_device需要替换，再次进入tick_setup_device函数，
 tick_setup_oneshot的handler参数也是之前设置的tick_handle_periodic函数，
 所以我们考察tick_handle_periodic即可：
 */
static void tick_setup_device(struct tick_device *td,
			      struct clock_event_device *newdev, int cpu,
			      const struct cpumask *cpumask)
{
	ktime_t next_event;
	void (*handler)(struct clock_event_device *) = NULL;

	/*
	 * First device setup ?
	 */
	if (!td->evtdev) {
		/*
		 * If no cpu took the do_timer update, assign it to
		 * this cpu:
		 */
		if (tick_do_timer_cpu == TICK_DO_TIMER_BOOT) {
			tick_do_timer_cpu = cpu;
			tick_next_period = ktime_get();
			tick_period = ktime_set(0, NSEC_PER_SEC / HZ);
		}

		/*
		 * Startup in periodic mode first.
		 */
		td->mode = TICKDEV_MODE_PERIODIC;
	} else {
		handler = td->evtdev->event_handler;
		next_event = td->evtdev->next_event;
		td->evtdev->event_handler = clockevents_handle_noop;
	}

	td->evtdev = newdev;

	/*
	 * When the device is not per cpu, pin the interrupt to the
	 * current cpu:
	 */
	if (!cpumask_equal(newdev->cpumask, cpumask))
		irq_set_affinity(newdev->irq, cpumask);

	/*
	 * When global broadcasting is active, check if the current
	 * device is registered as a placeholder for broadcast mode.
	 * This allows us to handle this x86 misfeature in a generic
	 * way.
	 */
	if (tick_device_uses_broadcast(newdev, cpu))
		return;

	if (td->mode == TICKDEV_MODE_PERIODIC)
		tick_setup_periodic(newdev, 0);
	else
		tick_setup_oneshot(newdev, handler, next_event);
}

/*
 * Check, if the new registered device should be used.
 */
 // 比对当前cpu所使用的与新注册的clock_event_device之间的特性，如果认为新
 // 的clock_event_device更好，则会进行切换工作。
static int tick_check_new_device(struct clock_event_device *newdev)
{
	struct clock_event_device *curdev;
	struct tick_device *td;
	int cpu, ret = NOTIFY_OK;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_device_lock, flags);

	// 首先，该函数先判断注册的clock_event_device是否可用于本cpu
	cpu = smp_processor_id();
	if (!cpumask_test_cpu(cpu, newdev->cpumask))
		goto out_bc;

	// 从per-cpu变量中取出本cpu的tick_device
	td = &per_cpu(tick_cpu_device, cpu);
	curdev = td->evtdev;

	/* cpu local device ? */
	// 如果不是本地clock_event_device，会做进一步的判断：如果不能把irq绑定到本cpu，
	// 则放弃处理，如果本cpu已经有了一个本地clock_event_device，也放弃处理：
	if (!cpumask_equal(newdev->cpumask, cpumask_of(cpu))) {

		/*
		 * If the cpu affinity of the device interrupt can not
		 * be set, ignore it.
		 */
		if (!irq_can_set_affinity(newdev->irq))
			goto out_bc;

		/*
		 * If we have a cpu local device already, do not replace it
		 * by a non cpu local device
		 */
		if (curdev && cpumask_equal(curdev->cpumask, cpumask_of(cpu)))
			goto out_bc;
	}

	/*
	 * If we have an active device, then check the rating and the oneshot
	 * feature.
	 */
	 // 如果本cpu已经有了一个clock_event_device，则根据是否支持单触发模式和它的rating值，
	 // 决定是否替换原来旧的clock_event_device：
	if (curdev) {
		/*
		 * Prefer one shot capable devices !
		 */
		if ((curdev->features & CLOCK_EVT_FEAT_ONESHOT) &&
		    !(newdev->features & CLOCK_EVT_FEAT_ONESHOT))
			goto out_bc;// // 新的不支持单触发，但旧的支持，所以不能替换 
		/*
		 * Check the rating
		 */
		if (curdev->rating >= newdev->rating)
			goto out_bc;// // 旧的比新的精度高，不能替换
	}

	/*
	 * Replace the eventually existing device by the new
	 * device. If the current device is the broadcast device, do
	 * not give it back to the clockevents layer !
	 */
	if (tick_is_broadcast_device(curdev)) {
		clockevents_shutdown(curdev);
		curdev = NULL;
	}
	clockevents_exchange_device(curdev, newdev);
	/*
		重新绑定当前cpu的tick_device和新注册的clock_event_device，
		如果发现是当前cpu第一次注册tick_device，就把它设置为TICKDEV_MODE_PERIODIC模式，
		如果是替换旧的tick_device，则根据新的tick_device的特性，设置为TICKDEV_MODE_PERIODIC
		或TICKDEV_MODE_ONESHOT模式。
	*/
	tick_setup_device(td, newdev, cpu, cpumask_of(cpu));
	if (newdev->features & CLOCK_EVT_FEAT_ONESHOT)
		tick_oneshot_notify();

	raw_spin_unlock_irqrestore(&tick_device_lock, flags);
	return NOTIFY_STOP;

out_bc:
	/*
	 * Can the new device be used as a broadcast device ?
	 */
	if (tick_check_broadcast_device(newdev))
		ret = NOTIFY_STOP;

	raw_spin_unlock_irqrestore(&tick_device_lock, flags);

	return ret;
}

/*
 * Transfer the do_timer job away from a dying cpu.
 *
 * Called with interrupts disabled.
 */
static void tick_handover_do_timer(int *cpup)
{
	if (*cpup == tick_do_timer_cpu) {
		int cpu = cpumask_first(cpu_online_mask);

		tick_do_timer_cpu = (cpu < nr_cpu_ids) ? cpu :
			TICK_DO_TIMER_NONE;
	}
}

/*
 * Shutdown an event device on a given cpu:
 *
 * This is called on a life CPU, when a CPU is dead. So we cannot
 * access the hardware device itself.
 * We just set the mode and remove it from the lists.
 */
static void tick_shutdown(unsigned int *cpup)
{
	struct tick_device *td = &per_cpu(tick_cpu_device, *cpup);
	struct clock_event_device *dev = td->evtdev;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_device_lock, flags);
	td->mode = TICKDEV_MODE_PERIODIC;
	if (dev) {
		/*
		 * Prevent that the clock events layer tries to call
		 * the set mode function!
		 */
		dev->mode = CLOCK_EVT_MODE_UNUSED;
		clockevents_exchange_device(dev, NULL);
		td->evtdev = NULL;
	}
	raw_spin_unlock_irqrestore(&tick_device_lock, flags);
}

static void tick_suspend(void)
{
	struct tick_device *td = &__get_cpu_var(tick_cpu_device);
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_device_lock, flags);
	clockevents_shutdown(td->evtdev);
	raw_spin_unlock_irqrestore(&tick_device_lock, flags);
}

static void tick_resume(void)
{
	struct tick_device *td = &__get_cpu_var(tick_cpu_device);
	unsigned long flags;
	int broadcast = tick_resume_broadcast();

	raw_spin_lock_irqsave(&tick_device_lock, flags);
	clockevents_set_mode(td->evtdev, CLOCK_EVT_MODE_RESUME);

	if (!broadcast) {
		if (td->mode == TICKDEV_MODE_PERIODIC)
			tick_setup_periodic(td->evtdev, 0);
		else
			tick_resume_oneshot();
	}
	raw_spin_unlock_irqrestore(&tick_device_lock, flags);
}

/*
 * Notification about clock event devices
 */
static int tick_notify(struct notifier_block *nb, unsigned long reason,
			       void *dev)
{
	switch (reason) {

	case CLOCK_EVT_NOTIFY_ADD:
		return tick_check_new_device(dev);

	case CLOCK_EVT_NOTIFY_BROADCAST_ON:
	case CLOCK_EVT_NOTIFY_BROADCAST_OFF:
	case CLOCK_EVT_NOTIFY_BROADCAST_FORCE:
		tick_broadcast_on_off(reason, dev);
		break;

	case CLOCK_EVT_NOTIFY_BROADCAST_ENTER:
	case CLOCK_EVT_NOTIFY_BROADCAST_EXIT:
		tick_broadcast_oneshot_control(reason);
		break;

	case CLOCK_EVT_NOTIFY_CPU_DYING:
		tick_handover_do_timer(dev);
		break;

	case CLOCK_EVT_NOTIFY_CPU_DEAD:
		tick_shutdown_broadcast_oneshot(dev);
		tick_shutdown_broadcast(dev);
		tick_shutdown(dev);
		break;

	case CLOCK_EVT_NOTIFY_SUSPEND:
		tick_suspend();
		tick_suspend_broadcast();
		break;

	case CLOCK_EVT_NOTIFY_RESUME:
		tick_resume();
		break;

	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block tick_notifier = {
	.notifier_call = tick_notify,
};

/**
 * tick_init - initialize the tick control
 *
 * Register the notifier with the clockevents framework
 */
 // 简单地调用clockevents_register_notifier，同时把类型为notifier_block的tick_notifier
 // 作为参数传入
void __init tick_init(void)
{
	clockevents_register_notifier(&tick_notifier);
}
