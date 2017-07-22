/*
 * linux/kernel/time/tick-broadcast.c
 *
 * This file contains functions which emulate a local clock-event
 * device via a broadcast event source.
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

#include "tick-internal.h"

/*
 * Broadcast support for broken x86 hardware, where the local apic
 * timer stops in C3 state.
 */

static struct tick_device tick_broadcast_device;
/* FIXME: Use cpumask_var_t. */
static DECLARE_BITMAP(tick_broadcast_mask, NR_CPUS);
static DECLARE_BITMAP(tmpmask, NR_CPUS);
static DEFINE_RAW_SPINLOCK(tick_broadcast_lock);
static int tick_broadcast_force;

#ifdef CONFIG_TICK_ONESHOT
static void tick_broadcast_clear_oneshot(int cpu);
#else
static inline void tick_broadcast_clear_oneshot(int cpu) { }
#endif

/*
 * Debugging: see timer_list.c
 */
struct tick_device *tick_get_broadcast_device(void)
{
	return &tick_broadcast_device;
}

struct cpumask *tick_get_broadcast_mask(void)
{
	return to_cpumask(tick_broadcast_mask);
}

/*
 * Start the device in periodic mode
 */
 //  开启广播设备  
//  函数任务：  
//      1.设置广播设备的事件处理函数  
//      2.编程启动广播设备  
//  调用路径：tick_check_new_device->tick_broadcast_start_periodic  
static void tick_broadcast_start_periodic(struct clock_event_device *bc)
{
	if (bc)
		tick_setup_periodic(bc, 1);
}

/*
 * Check, if the device can be utilized as broadcast device:
 */
//  设置设备为广播设备  
//  函数任务：  
//      1.成为广播设备的条件  
//          1.1 设备不受电源状态的影响  
//          1.2 设备的rating高于当前使用的广播设备  
//      2.绑定此设备到全局广播设备  
//      3.如果有设备被代理  
//          3.1 开启广播设备 
int tick_check_broadcast_device(struct clock_event_device *dev)
{
	//检查是否可以作为广播设备

	if ((tick_broadcast_device.evtdev &&
	     tick_broadcast_device.evtdev->rating >= dev->rating) ||
	     (dev->features & CLOCK_EVT_FEAT_C3STOP))
		return 0;

	clockevents_exchange_device(NULL, dev);
	tick_broadcast_device.evtdev = dev;
	//有设备需要被广播设备代理
	if (!cpumask_empty(tick_get_broadcast_mask()))
		tick_broadcast_start_periodic(dev);//启动广播设备
	return 1;
}

/*
 * Check, if the device is the broadcast device
 */
int tick_is_broadcast_device(struct clock_event_device *dev)
{
	return (dev && tick_broadcast_device.evtdev == dev);
}

/*
 * Check, if the device is disfunctional and a place holder, which
 * needs to be handled by the broadcast device.
 */
 //  将cpu加入广播组  
//  函数任务：  
//      1.如果cpu的tick device没有正常工作  
//          1.1 设置tick device的事件处理函数为周期事件处理函数  
//          1.2 将设备加入广播组  
//          1.3 启动广播设备  
//      2.否则，如果设备不受省电模式C3的影响  
//          2.1 将cpu从广播组中删除  
int tick_device_uses_broadcast(struct clock_event_device *dev, int cpu)
{
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	/*
	 * Devices might be registered with both periodic and oneshot
	 * mode disabled. This signals, that the device needs to be
	 * operated from the broadcast device and is a placeholder for
	 * the cpu local device.
	 */
	if (!tick_device_is_functional(dev)) {
		dev->event_handler = tick_handle_periodic;
		cpumask_set_cpu(cpu, tick_get_broadcast_mask());
		tick_broadcast_start_periodic(tick_broadcast_device.evtdev);
		ret = 1;
	} else {
		/*
		 * When the new device is not affected by the stop
		 * feature and the cpu is marked in the broadcast mask
		 * then clear the broadcast bit.
		 */
		 //如果本设备不受省电模式C3的影响
		if (!(dev->features & CLOCK_EVT_FEAT_C3STOP)) {
			int cpu = smp_processor_id();
			//将cpu从广播组中删除

			cpumask_clear_cpu(cpu, tick_get_broadcast_mask());
			tick_broadcast_clear_oneshot(cpu);
		}
	}
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
	return ret;
}

/*
 * Broadcast the event to the cpus, which are set in the mask (mangled).
 */
//  执行事件处理函数  
//  函数任务：  
//      1.执行本cpu的事件处理函数  
//      2.通过ipi通知代理的cpu，执行事件处理函数  
//  调用路径：tick_handle_periodic_broadcast->...->tick_do_broadcast 
static void tick_do_broadcast(struct cpumask *mask)
{
	int cpu = smp_processor_id();
	struct tick_device *td;

	/*
	 * Check, if the current cpu is in the mask
	 */
	 //检查当前cpu是否在掩码
	if (cpumask_test_cpu(cpu, mask)) {
	 	//从掩码中清除本cpu
		cpumask_clear_cpu(cpu, mask);
		td = &per_cpu(tick_cpu_device, cpu);
		//执行事件处理函数
		td->evtdev->event_handler(td->evtdev);
	}
	//检查是否有其他cpu需要被代理
	if (!cpumask_empty(mask)) {
		/*
		 * It might be necessary to actually check whether the devices
		 * have different broadcast functions. For now, just use the
		 * one of the first device. This works as long as we have this
		 * misfeature only on x86 (lapic)
		 */
		 //通过ipi通知其他cpu执行时钟事件处理函数
		td = &per_cpu(tick_cpu_device, cpumask_first(mask));
		td->evtdev->broadcast(mask);
	}
}

/*
 * Periodic broadcast:
 * - invoke the broadcast handlers
 */
//  执行事件处理函数  
//  函数任务：  
//      1.通过tick_broadcast_mask掩码获取代理的cpu  
//      2.执行事件处理函数  
//  调用路径：tick_handle_periodic_broadcast->tick_do_periodic_broadcast
static void tick_do_periodic_broadcast(void)
{
	raw_spin_lock(&tick_broadcast_lock);
	//通过tick_broadcast_mask掩码获取代理的cpu
	cpumask_and(to_cpumask(tmpmask),
		    cpu_online_mask, tick_get_broadcast_mask());
	//对代理的cpu执行事件处理函数
	tick_do_broadcast(to_cpumask(tmpmask));

	raw_spin_unlock(&tick_broadcast_lock);
}

/*
 * Event handler for periodic broadcast ticks
 */
//  clockevent周期处理函数(广播设备)  
//  函数任务：  
//      1.执行本cpu的事件处理函数  
//      2.通过ipi通知代理的cpu，执行时间处理函数  
//      3.重新编程下次事件的到期时间 
static void tick_handle_periodic_broadcast(struct clock_event_device *dev)
{
	ktime_t next;
	//执行事件处理函数

	tick_do_periodic_broadcast();

	/*
	 * The device is in periodic mode. No reprogramming necessary:
	 */
	 //重编程下次事件的到期时间
	if (dev->mode == CLOCK_EVT_MODE_PERIODIC)
		return;

	/*
	 * Setup the next period for devices, which do not have
	 * periodic mode. We read dev->next_event first and add to it
	 * when the event alrady expired. clockevents_program_event()
	 * sets dev->next_event only when the event is really
	 * programmed to the device.
	 */
	for (next = dev->next_event; ;) {
		next = ktime_add(next, tick_period);

		if (!clockevents_program_event(dev, next, ktime_get()))
			return;
		tick_do_periodic_broadcast();
	}
}

/*
 * Powerstate information: The system enters/leaves a state, where
 * affected devices might stop
 */
 //  cpu开启\关闭广播模式  
//  函数主要任务：  
//      1.开启广播模式  
//          1.1 将cpu加入广播组bitmap中  
//          1.2 如果广播设备为周期触发模式，关闭此clockevent  
//      2.关闭广播模式  
//          2.1 从广播组bitmap中删除cpu  
//          2.2 如果广播设备为周期触发模式，恢复其clockevent  
//      3.如果广播组为空  
//          3.1 停止全局广播设备  
//      4.否则  
//          4.1 设置全局广播设备为单触发模式或周期模式  
//  注：  
//      1.全局广播设备tick_broadcast_device  
//      2.每个cpu有各自的tick_device，选择其中一个充当全局广播设备  
//      3.加入广播组的clockevent需要关闭其周期触发模式  
static void tick_do_broadcast_on_off(unsigned long *reason)
{
	struct clock_event_device *bc, *dev;
	struct tick_device *td;
	unsigned long flags;
	int cpu, bc_stopped;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	cpu = smp_processor_id();
	//per cpu的tick device
	td = &per_cpu(tick_cpu_device, cpu);
	dev = td->evtdev;
	//全局广播设备
	bc = tick_broadcast_device.evtdev;

	/*
	 * Is the device not affected by the powerstate ?
	 */
	 //设备不受电源状态的影响
	if (!dev || !(dev->features & CLOCK_EVT_FEAT_C3STOP))
		goto out;

	if (!tick_device_is_functional(dev))
		goto out;
		
	//广播组是否为空
	bc_stopped = cpumask_empty(tick_get_broadcast_mask());

	switch (*reason) {
	//开启cpu的广播模式 
	case CLOCK_EVT_NOTIFY_BROADCAST_ON:
	case CLOCK_EVT_NOTIFY_BROADCAST_FORCE:
	//当前cpu不在广播组中
		if (!cpumask_test_cpu(cpu, tick_get_broadcast_mask())) {
		 //将cpu加入广播组
			cpumask_set_cpu(cpu, tick_get_broadcast_mask());
			//关闭周期触发模式的clockevent设备
			if (tick_broadcast_device.mode ==
			    TICKDEV_MODE_PERIODIC)
				clockevents_shutdown(dev);
		}
		if (*reason == CLOCK_EVT_NOTIFY_BROADCAST_FORCE)
			tick_broadcast_force = 1;
		break;
	//关闭cpu的广播模式
	case CLOCK_EVT_NOTIFY_BROADCAST_OFF:
		if (!tick_broadcast_force &&
			//从广播组中清除该设备
		    cpumask_test_cpu(cpu, tick_get_broadcast_mask())) {
			cpumask_clear_cpu(cpu, tick_get_broadcast_mask());
			//恢复该设备的周期触发模式
			if (tick_broadcast_device.mode ==
			    TICKDEV_MODE_PERIODIC)
				tick_setup_periodic(dev, 0);
		}
		break;
	}
	//删除了广播组中最后一个设备 
	if (cpumask_empty(tick_get_broadcast_mask())) {
		if (!bc_stopped)
			//删除了广播组中最后一个设备 
			clockevents_shutdown(bc);
	} else if (bc_stopped) {
		if (tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC)
			tick_broadcast_start_periodic(bc);
		else
			tick_broadcast_setup_oneshot(bc);
	}
out:
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

/*
 * Powerstate information: The system enters/leaves a state, where
 * affected devices might stop.
 */
 //  cpu开启\关闭广播模式  
//  调用路径：tick_notify->tick_broadcast_on_off  
//  注：  
//      1.加入、退出广播组指针对online的cpu  
void tick_broadcast_on_off(unsigned long reason, int *oncpu)
{
	//忽略offline的cpu
	if (!cpumask_test_cpu(*oncpu, cpu_online_mask))
		printk(KERN_ERR "tick-broadcast: ignoring broadcast for "
		       "offline CPU #%d\n", *oncpu);
	else
		tick_do_broadcast_on_off(&reason);
}

/*
 * Set the periodic handler depending on broadcast on/off
 */
//  设置clockevent周期处理函数  
//  函数参数：  
//      broadcast，指示此设备是否为全局广播设备  
//  调用路径：tick_setup_periodic->tick_set_periodic_handler  
void tick_set_periodic_handler(struct clock_event_device *dev, int broadcast)
{
	if (!broadcast)
		dev->event_handler = tick_handle_periodic;
	else
		dev->event_handler = tick_handle_periodic_broadcast;
}

/*
 * Remove a CPU from broadcasting
 */
 //  从broadcast掩码中删除cpu  
//  调用路径：tick_notify->tick_shutdown_broadcast 
void tick_shutdown_broadcast(unsigned int *cpup)
{
	struct clock_event_device *bc;
	unsigned long flags;
	unsigned int cpu = *cpup;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);
	//从broadcast掩码中删除dead cpu

	bc = tick_broadcast_device.evtdev;
	cpumask_clear_cpu(cpu, tick_get_broadcast_mask());
	//广播组为空，停止全局广播广播设备
	if (tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC) {
		if (bc && cpumask_empty(tick_get_broadcast_mask()))
			clockevents_shutdown(bc);
	}

	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

void tick_suspend_broadcast(void)
{
	struct clock_event_device *bc;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	bc = tick_broadcast_device.evtdev;
	if (bc)
		clockevents_shutdown(bc);

	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

int tick_resume_broadcast(void)
{
	struct clock_event_device *bc;
	unsigned long flags;
	int broadcast = 0;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	bc = tick_broadcast_device.evtdev;

	if (bc) {
		clockevents_set_mode(bc, CLOCK_EVT_MODE_RESUME);

		switch (tick_broadcast_device.mode) {
		case TICKDEV_MODE_PERIODIC:
			if (!cpumask_empty(tick_get_broadcast_mask()))
				tick_broadcast_start_periodic(bc);
			broadcast = cpumask_test_cpu(smp_processor_id(),
						     tick_get_broadcast_mask());
			break;
		case TICKDEV_MODE_ONESHOT:
			broadcast = tick_resume_broadcast_oneshot(bc);
			break;
		}
	}
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);

	return broadcast;
}


#ifdef CONFIG_TICK_ONESHOT

/* FIXME: use cpumask_var_t. */
static DECLARE_BITMAP(tick_broadcast_oneshot_mask, NR_CPUS);

/*
 * Exposed for debugging: see timer_list.c
 */
struct cpumask *tick_get_broadcast_oneshot_mask(void)
{
	return to_cpumask(tick_broadcast_oneshot_mask);
}

static int tick_broadcast_set_event(ktime_t expires, int force)
{
	struct clock_event_device *bc = tick_broadcast_device.evtdev;

	return tick_dev_program_event(bc, expires, force);
}

int tick_resume_broadcast_oneshot(struct clock_event_device *bc)
{
	clockevents_set_mode(bc, CLOCK_EVT_MODE_ONESHOT);
	return 0;
}

/*
 * Called from irq_enter() when idle was interrupted to reenable the
 * per cpu device.
 */
void tick_check_oneshot_broadcast(int cpu)
{
	if (cpumask_test_cpu(cpu, to_cpumask(tick_broadcast_oneshot_mask))) {
		struct tick_device *td = &per_cpu(tick_cpu_device, cpu);

		clockevents_set_mode(td->evtdev, CLOCK_EVT_MODE_ONESHOT);
	}
}

/*
 * Handle oneshot mode broadcasting
 */
static void tick_handle_oneshot_broadcast(struct clock_event_device *dev)
{
	struct tick_device *td;
	ktime_t now, next_event;
	int cpu;

	raw_spin_lock(&tick_broadcast_lock);
again:
	dev->next_event.tv64 = KTIME_MAX;
	next_event.tv64 = KTIME_MAX;
	cpumask_clear(to_cpumask(tmpmask));
	now = ktime_get();
	/* Find all expired events */
	for_each_cpu(cpu, tick_get_broadcast_oneshot_mask()) {
		td = &per_cpu(tick_cpu_device, cpu);
		if (td->evtdev->next_event.tv64 <= now.tv64)
			cpumask_set_cpu(cpu, to_cpumask(tmpmask));
		else if (td->evtdev->next_event.tv64 < next_event.tv64)
			next_event.tv64 = td->evtdev->next_event.tv64;
	}

	/*
	 * Wakeup the cpus which have an expired event.
	 */
	tick_do_broadcast(to_cpumask(tmpmask));

	/*
	 * Two reasons for reprogram:
	 *
	 * - The global event did not expire any CPU local
	 * events. This happens in dyntick mode, as the maximum PIT
	 * delta is quite small.
	 *
	 * - There are pending events on sleeping CPUs which were not
	 * in the event mask
	 */
	if (next_event.tv64 != KTIME_MAX) {
		/*
		 * Rearm the broadcast device. If event expired,
		 * repeat the above
		 */
		if (tick_broadcast_set_event(next_event, 0))
			goto again;
	}
	raw_spin_unlock(&tick_broadcast_lock);
}

/*
 * Powerstate information: The system enters/leaves a state, where
 * affected devices might stop
 */

 //  cpu进入\离开广播模式  
//      当cpu进入省电模式时，由全局广播设备接管cpu的时间维护  
//  调用路径：tick_notify->tick_broadcast_oneshot_control  
//  函数主要任务：  
//      1.cpu进入省电模式，由全局广播设备接管其时间维护  
//          1.1 设置cpu关闭状态,更新全局广播设备的到期时间  
//      2.cpu离开省电模式，全局广播设备不再接管其时间维护  
//          2.1 更新全局广播设备的到期时间  
//  注：  
//      cpu在进入省电模式时，进入广播模式  
//      cpu在离开省电模式时，退出广播模式  
void tick_broadcast_oneshot_control(unsigned long reason)
{
	struct clock_event_device *bc, *dev;
	struct tick_device *td;
	unsigned long flags;
	int cpu;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	/*
	 * Periodic mode does not care about the enter/exit of power
	 * states
	 */
	 //电源状态变化不影响周期模式的全局广播设备
	if (tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC)
		goto out;

	bc = tick_broadcast_device.evtdev;
	cpu = smp_processor_id();
	td = &per_cpu(tick_cpu_device, cpu);
	dev = td->evtdev;
	
	//cpu的clockevent设备不受电源状态的影响
	if (!(dev->features & CLOCK_EVT_FEAT_C3STOP))
		goto out;

	//cpu进入省电模式，由全局广播设备接管其tick device的任务
	if (reason == CLOCK_EVT_NOTIFY_BROADCAST_ENTER) {
		//设置cpu关闭状态 
		if (!cpumask_test_cpu(cpu, tick_get_broadcast_oneshot_mask())) {
			cpumask_set_cpu(cpu, tick_get_broadcast_oneshot_mask());
			clockevents_set_mode(dev, CLOCK_EVT_MODE_SHUTDOWN);
			//cpu下一个事件的到期时间小于全局广播设备下一个事件的到期时间，设置cpu在下一个周期到期
			if (dev->next_event.tv64 < bc->next_event.tv64)
				tick_broadcast_set_event(dev->next_event, 1);
		}
		//cpu离开省电模式，全局广播设备不再接管其tick device的任务 
	} else {
		//清除cpu关闭状态
		if (cpumask_test_cpu(cpu, tick_get_broadcast_oneshot_mask())) {
			cpumask_clear_cpu(cpu,
					  tick_get_broadcast_oneshot_mask());
			clockevents_set_mode(dev, CLOCK_EVT_MODE_ONESHOT);
			//重新编程cpu的tick device在下一个周期到期
			if (dev->next_event.tv64 != KTIME_MAX)
				tick_program_event(dev->next_event, 1);
		}
	}

out:
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

/*
 * Reset the one shot broadcast for a cpu
 *
 * Called with tick_broadcast_lock held
 */
static void tick_broadcast_clear_oneshot(int cpu)
{
	cpumask_clear_cpu(cpu, tick_get_broadcast_oneshot_mask());
}

static void tick_broadcast_init_next_event(struct cpumask *mask,
					   ktime_t expires)
{
	struct tick_device *td;
	int cpu;

	for_each_cpu(cpu, mask) {
		td = &per_cpu(tick_cpu_device, cpu);
		if (td->evtdev)
			td->evtdev->next_event = expires;
	}
}

/**
 * tick_broadcast_setup_oneshot - setup the broadcast device
 */
void tick_broadcast_setup_oneshot(struct clock_event_device *bc)
{
	/* Set it up only once ! */
	if (bc->event_handler != tick_handle_oneshot_broadcast) {
		int was_periodic = bc->mode == CLOCK_EVT_MODE_PERIODIC;
		int cpu = smp_processor_id();

		bc->event_handler = tick_handle_oneshot_broadcast;
		clockevents_set_mode(bc, CLOCK_EVT_MODE_ONESHOT);

		/* Take the do_timer update */
		tick_do_timer_cpu = cpu;

		/*
		 * We must be careful here. There might be other CPUs
		 * waiting for periodic broadcast. We need to set the
		 * oneshot_mask bits for those and program the
		 * broadcast device to fire.
		 */
		cpumask_copy(to_cpumask(tmpmask), tick_get_broadcast_mask());
		cpumask_clear_cpu(cpu, to_cpumask(tmpmask));
		cpumask_or(tick_get_broadcast_oneshot_mask(),
			   tick_get_broadcast_oneshot_mask(),
			   to_cpumask(tmpmask));

		if (was_periodic && !cpumask_empty(to_cpumask(tmpmask))) {
			tick_broadcast_init_next_event(to_cpumask(tmpmask),
						       tick_next_period);
			tick_broadcast_set_event(tick_next_period, 1);
		} else
			bc->next_event.tv64 = KTIME_MAX;
	}
}

/*
 * Select oneshot operating mode for the broadcast device
 */
void tick_broadcast_switch_to_oneshot(void)
{
	struct clock_event_device *bc;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	tick_broadcast_device.mode = TICKDEV_MODE_ONESHOT;
	bc = tick_broadcast_device.evtdev;
	if (bc)
		tick_broadcast_setup_oneshot(bc);
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}


/*
 * Remove a dead CPU from broadcasting
 */
//  dead cpu的clockevent的处理：  
//      1.从oneshot掩码中删除cpu  
//      2.从broadcast掩码中删除cpu  
//      3.关闭clockevent设备  
//          3.1 设置该cpu tick device的clockevent为null  
//          3.2 将clockevent设备链入clockevents_released  
  
//  从oneshot掩码中删除cpu  
//  调用路径：tick_notify->tick_shutdown_broadcast_oneshot 

void tick_shutdown_broadcast_oneshot(unsigned int *cpup)
{
	unsigned long flags;
	unsigned int cpu = *cpup;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	/*
	 * Clear the broadcast mask flag for the dead cpu, but do not
	 * stop the broadcast device!
	 */
	 //从oneshot掩码中删除dead cpu
	cpumask_clear_cpu(cpu, tick_get_broadcast_oneshot_mask());

	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

/*
 * Check, whether the broadcast device is in one shot mode
 */
int tick_broadcast_oneshot_active(void)
{
	return tick_broadcast_device.mode == TICKDEV_MODE_ONESHOT;
}

#endif
