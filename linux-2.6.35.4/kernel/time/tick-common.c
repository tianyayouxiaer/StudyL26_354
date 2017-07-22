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
 //  周期处理函数  
//  函数任务：  
//      1.如果本cpu负责更新全局时间  
//          1.1 执行do_timer  
//      2.更新进程运行时间  
//          2.1 通知调度器更新其虚拟时钟  
//          2.2 更新进程cpu上执行时间  
//  调用路径：tick_handle_periodic->tick_periodic 
static void tick_periodic(int cpu)
{
	//本cpu负责更新全局时间
	if (tick_do_timer_cpu == cpu) {
		write_seqlock(&xtime_lock);

		/* Keep track of the next tick event */
		//计算下个周期
		tick_next_period = ktime_add(tick_next_period, tick_period);
		//执行do_timer

		do_timer(1);
		write_sequnlock(&xtime_lock);
	}
	//更新进程时间
	update_process_times(user_mode(get_irq_regs()));
	profile_tick(CPU_PROFILING);
}

/*
 * Event handler for periodic ticks
 */
 //  clockevent周期处理函数(非广播设备)  
//  函数任务:  
//      1.执行周期任务  
//      2.如果设备为单触发模式  
//          2.1 重编程下一次事件到期时间  
void tick_handle_periodic(struct clock_event_device *dev)
{
	int cpu = smp_processor_id();
	ktime_t next;
	//执行do_timer更新全局事件，更新进程时间 

	tick_periodic(cpu);
	//周期模式不需要手动设置下次到期时间，直接退出 

	if (dev->mode != CLOCK_EVT_MODE_ONESHOT)
		return;
	/*
	 * Setup the next period for devices, which do not have
	 * periodic mode:
	 */
	 //计算下次到期时间
	next = ktime_add(dev->next_event, tick_period);
	for (;;) {
		if (!clockevents_program_event(dev, next, ktime_get()))//重编程设备事件到期
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
		 //重新编程设备失败，说明已经经过一个tick周期，此时执行tick周期任务  
		if (timekeeping_valid_for_hres())
			tick_periodic(cpu);
		//更新下次到期时间 
		next = ktime_add(next, tick_period);
	}
}

/*
 * Setup the device for a periodic tick
 */
//  周期模式启动设备  
//  调用路径：tick_check_new_device->tick_setup_device->tick_setup_periodic  
//  函数任务：  
//      1.设置设备周期处理函数  
//      2.根据设备支持的模式，激活设备  
//          2.1 如果设备支持周期模式，并且为激活单触发模式  
//              2.1.1 设置设备当前为周期模式，退出  
//              2.1.2 由周期处理函数执行周期任务  
//          2.2 否则，通过单触发模式模拟周期运行  
//              2.2.1 获取下次tick到期的时间  
//              2.2.2 编程设备下一次到期时间  
//              2.2.3 由周期处理函数执行周期任务，更新下次到期时间
void tick_setup_periodic(struct clock_event_device *dev, int broadcast)
{
	//设置clockevent的周期处理函数

	tick_set_periodic_handler(dev, broadcast);

	/* Broadcast setup ? */
	//设备不能正常启动，则有可能broadcast代其运行
	if (!tick_device_is_functional(dev))
		return;

	if ((dev->features & CLOCK_EVT_FEAT_PERIODIC) &&
	    !tick_broadcast_oneshot_active()) {
	    //设置设备为周期模式
		clockevents_set_mode(dev, CLOCK_EVT_MODE_PERIODIC);
	} else {
		unsigned long seq;
		ktime_t next;
		//获取下次tick到期的时间

		do {
			seq = read_seqbegin(&xtime_lock);
			next = tick_next_period;
		} while (read_seqretry(&xtime_lock, seq));
		//设置设备为单触发模式

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

//  绑定tick device到clockevnet设备  
//  函数任务：  
//      1.tick device未绑定过clockevent  
//          1.1 选择cpu负责更新全局时间  
//          1.2 以周期模式启动  
//      2.tick device绑定过clockevnet  
//          2.1 暂时设置其eventhandler=noop，保留下次到期时间  
//      3.绑定tick device到新clockevent  
//      4.clockevent并不仅服务于本cpu，设置irq亲和本cpu  
//      5.如果设备没有正常工作，将设备加入广播组，然后退出  
//      6.否则更新clockevent的事件处理函数  
//          6.1 首次绑定，以周期模式启动  
//          6.2 否则按照之前的模式启动  
//  调用路径：tick_check_new_device->tick_setup_device  
  
//  注：  
//      1.由负责更新全局时间的cpu，初始化tick_next_period，tick_period  
//          tick_next_period, 周期时钟下次到期的时间  
//          tick_period，周期时钟的周期，1HZ经历的纳秒  
static void tick_setup_device(struct tick_device *td,
			      struct clock_event_device *newdev, int cpu,
			      const struct cpumask *cpumask)
{
	ktime_t next_event;
	void (*handler)(struct clock_event_device *) = NULL;

	/*
	 * First device setup ?
	 */
	 //tick device第一次创建，没有绑定clockevent 
	if (!td->evtdev) {
		/*
		 * If no cpu took the do_timer update, assign it to
		 * this cpu:
		 */
		 //如果没有cpu负责do_timer，由本cpu负责
		if (tick_do_timer_cpu == TICK_DO_TIMER_BOOT) {
			tick_do_timer_cpu = cpu;
			tick_next_period = ktime_get();
			tick_period = ktime_set(0, NSEC_PER_SEC / HZ);
		}

		/*
		 * Startup in periodic mode first.
		 */
		 //以周期模式启动
		td->mode = TICKDEV_MODE_PERIODIC;
	} else {
		//tick device非第一次创建，此次为更新clockevent  
        //更新event处理函数为noop 
		handler = td->evtdev->event_handler;
		next_event = td->evtdev->next_event;
		td->evtdev->event_handler = clockevents_handle_noop;
	}
	//更新tick device的clockevent

	td->evtdev = newdev;

	/*
	 * When the device is not per cpu, pin the interrupt to the
	 * current cpu:
	 */
	  //clockevent并不仅服务于本cpu，设置irq与本cpu亲和
	if (!cpumask_equal(newdev->cpumask, cpumask))
		irq_set_affinity(newdev->irq, cpumask);

	/*
	 * When global broadcasting is active, check if the current
	 * device is registered as a placeholder for broadcast mode.
	 * This allows us to handle this x86 misfeature in a generic
	 * way.
	 */
	 //如果设备没有正常工作，将设备加入广播组，然后退出
	if (tick_device_uses_broadcast(newdev, cpu))
		return;
	//设置clockevent的eventhandler

	if (td->mode == TICKDEV_MODE_PERIODIC)
		tick_setup_periodic(newdev, 0);//以周期模式启动 
	else
		tick_setup_oneshot(newdev, handler, next_event); //以单触发模式启动
}

/*
 * Check, if the new registered device should be used.
 */
 //  clockevent设备注册处理函数  
 //  调用路径：tick_notify->CLOCK_EVT_NOTIFY_ADD  
 //  函数任务：  
 // 	 1.确认设备服务于本cpu	
 // 	 2.如果设备非本cpu的local device  
 // 		 2.1 设置cpu的irq亲和性  
 // 		 2.2 如果本cpu已经绑定一个local device，并且二者服务的cpu相同，则忽略  
 // 	 3.如果本cpu已有local device，选择两者中更好的  
 // 		 3.1 优先选择支持单触发的  
 // 		 3.2 然后选择高rating的  
 // 	 4.如果本cpu使用的clockevent设备为广播设备，直接关闭	
 // 	 5.更新cpu的tick device为新tick device  
 // 	 6.启动tick device  
 // 	 7.异步通知周期时钟关于时钟事件设备的改变  
   
 //  注：如果不能设置为本cpu tick device的事件源，考虑其是否可以作为广播设备。 

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
		 //非local device，并且不能设置irq 亲和性，忽略
		if (!irq_can_set_affinity(newdev->irq))
			goto out_bc;

		/*
		 * If we have a cpu local device already, do not replace it
		 * by a non cpu local device
		 */
		 //如果本cpu已经绑定有一个local device，则忽略
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
	 //如果当前使用的clockevent设备为广播设备，直接关闭  
	if (tick_is_broadcast_device(curdev)) {
		clockevents_shutdown(curdev);
		curdev = NULL;
	}
	//设置cpu的tick device为新tick device 
	clockevents_exchange_device(curdev, newdev);
	/*
		重新绑定当前cpu的tick_device和新注册的clock_event_device，
		如果发现是当前cpu第一次注册tick_device，就把它设置为TICKDEV_MODE_PERIODIC模式，
		如果是替换旧的tick_device，则根据新的tick_device的特性，设置为TICKDEV_MODE_PERIODIC
		或TICKDEV_MODE_ONESHOT模式。
	*/
	tick_setup_device(td, newdev, cpu, cpumask_of(cpu));
	if (newdev->features & CLOCK_EVT_FEAT_ONESHOT)
		tick_oneshot_notify();//异步通知周期时钟关于时钟事件设备的改变 

	raw_spin_unlock_irqrestore(&tick_device_lock, flags);
	return NOTIFY_STOP;

out_bc:
	/*
	 * Can the new device be used as a broadcast device ?
	 */
	  //尝试设置clockevent设备为tick device
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
 //  选择do_timer cpu  
//  调用路径：tick_notify->tick_handover_do_timer  
static void tick_handover_do_timer(int *cpup)
{
	//如果dying cpu负责do_timer,重新选取online cpu中第一个cpu负责
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
 //  关闭cpu的clockevent设备  
//  调用路径：tick_notify->tick_shutdown
static void tick_shutdown(unsigned int *cpup)
{
	struct tick_device *td = &per_cpu(tick_cpu_device, *cpup);
	struct clock_event_device *dev = td->evtdev;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_device_lock, flags);
	td->mode = TICKDEV_MODE_PERIODIC;
	//标记给定cpu的clockevent设备为不可用
	if (dev) {
		/*
		 * Prevent that the clock events layer tries to call
		 * the set mode function!
		 */
		dev->mode = CLOCK_EVT_MODE_UNUSED;
		//将clockevent链接到clockevents_released
		clockevents_exchange_device(dev, NULL);
		td->evtdev = NULL;
	}
	raw_spin_unlock_irqrestore(&tick_device_lock, flags);
}

//  挂起cpu的clockevent  
//  调用路径：tick_notify->tick_suspend 
static void tick_suspend(void)
{
	struct tick_device *td = &__get_cpu_var(tick_cpu_device);
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_device_lock, flags);
	//关闭cpu tick device的clockevent
	clockevents_shutdown(td->evtdev);
	raw_spin_unlock_irqrestore(&tick_device_lock, flags);
}

//  恢复cpu的clockevent  
//  调用路径：tick_notify->tick_resume  
static void tick_resume(void)
{
	struct tick_device *td = &__get_cpu_var(tick_cpu_device);
	unsigned long flags;
	//恢复广播
	int broadcast = tick_resume_broadcast();

	raw_spin_lock_irqsave(&tick_device_lock, flags);
	clockevents_set_mode(td->evtdev, CLOCK_EVT_MODE_RESUME);
	//恢复clockevent的触发模式
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
 //  监听clockevent设备，cpu的状态变化 
static int tick_notify(struct notifier_block *nb, unsigned long reason,
			       void *dev)
{
	switch (reason) {
	//新eventclock设备注册
	case CLOCK_EVT_NOTIFY_ADD:
		return tick_check_new_device(dev);
	//cpu开启\关闭广播模式
	case CLOCK_EVT_NOTIFY_BROADCAST_ON:
	case CLOCK_EVT_NOTIFY_BROADCAST_OFF:
	case CLOCK_EVT_NOTIFY_BROADCAST_FORCE:
		tick_broadcast_on_off(reason, dev);
		break;
	//cpu进入\离开广播模式
	case CLOCK_EVT_NOTIFY_BROADCAST_ENTER:
	case CLOCK_EVT_NOTIFY_BROADCAST_EXIT:
		tick_broadcast_oneshot_control(reason);
		break;
	//选择do_timer的cpu 
	case CLOCK_EVT_NOTIFY_CPU_DYING:
		tick_handover_do_timer(dev);
		break;
	//停用dead cpu的clockevent 
	case CLOCK_EVT_NOTIFY_CPU_DEAD:
		tick_shutdown_broadcast_oneshot(dev);
		tick_shutdown_broadcast(dev);
		tick_shutdown(dev);
		break;
	//挂起cpu的clockevent

	case CLOCK_EVT_NOTIFY_SUSPEND:
		tick_suspend();
		tick_suspend_broadcast();
		break;
		
	//恢复cpu的clockevent
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
 //  1.每个cpu都具有一个tick_device，维护周期时钟。	
 //  2.tick_device依赖一个clockevent设备，提供周期事件。	
 //  3.cpu电源状态的改变会影响tick_device，通过tick_notifier监听电源状态。	
 //  4.全局广播设备接管进入省电模式cpu的周期时间维护。  
 //  4.broadcast_mask保存开启广播模式的cpu， broadcast_oneshot_mask保存进入省电模式的cpu。	
   
 //  监听clockevent设备状态  
 //  调用路径：start_kernel->tick_init

void __init tick_init(void)
{
	clockevents_register_notifier(&tick_notifier);
}
