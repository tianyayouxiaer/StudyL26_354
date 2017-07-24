v/*  linux/include/linux/clockchips.h
 *
 *  This file contains the structure definitions for clockchips.
 *
 *  If you are not a clockchip, or the time of day code, you should
 *  not be including this file!
 */
#ifndef _LINUX_CLOCKCHIPS_H
#define _LINUX_CLOCKCHIPS_H

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BUILD

#include <linux/clocksource.h>
#include <linux/cpumask.h>
#include <linux/ktime.h>
#include <linux/notifier.h>

/*
参见： http://blog.csdn.net/droidphone/article/details/8017604
*/

struct clock_event_device;

/* Clock event mode commands */
enum clock_event_mode {
	CLOCK_EVT_MODE_UNUSED = 0,
	CLOCK_EVT_MODE_SHUTDOWN,
	CLOCK_EVT_MODE_PERIODIC,
	CLOCK_EVT_MODE_ONESHOT,
	CLOCK_EVT_MODE_RESUME,
};

/* Clock event notification values */
enum clock_event_nofitiers {
	CLOCK_EVT_NOTIFY_ADD,
	CLOCK_EVT_NOTIFY_BROADCAST_ON,
	CLOCK_EVT_NOTIFY_BROADCAST_OFF,
	CLOCK_EVT_NOTIFY_BROADCAST_FORCE,
	CLOCK_EVT_NOTIFY_BROADCAST_ENTER,
	CLOCK_EVT_NOTIFY_BROADCAST_EXIT,
	CLOCK_EVT_NOTIFY_SUSPEND,
	CLOCK_EVT_NOTIFY_RESUME,
	CLOCK_EVT_NOTIFY_CPU_DYING,
	CLOCK_EVT_NOTIFY_CPU_DEAD,
};

/*
 * Clock event features
 */
#define CLOCK_EVT_FEAT_PERIODIC		0x000001
#define CLOCK_EVT_FEAT_ONESHOT		0x000002
/*
 * x86(64) specific misfeatures:
 *
 * - Clockevent source stops in C3 State and needs broadcast support.
 * - Local APIC timer is used as a dummy device.
 */
#define CLOCK_EVT_FEAT_C3STOP		0x000004
#define CLOCK_EVT_FEAT_DUMMY		0x000008

/**
 * struct clock_event_device - clock event device descriptor
 * @name:		ptr to clock event name
 * @features:		features
 * @max_delta_ns:	maximum delta value in ns
 * @min_delta_ns:	minimum delta value in ns
 * @mult:		nanosecond to cycles multiplier
 * @shift:		nanoseconds to cycles divisor (power of two)
 * @rating:		variable to rate clock event devices
 * @irq:		IRQ number (only for non CPU local devices)
 * @cpumask:		cpumask to indicate for which CPUs this device works
 * @set_next_event:	set next event function
 * @set_mode:		set mode function
 * @event_handler:	Assigned by the framework to be called by the low
 *			level handler of the event source
 * @broadcast:		function to broadcast events
 * @list:		list head for the management code
 * @mode:		operating mode assigned by the management code
 * @next_event:		local storage for the next event in oneshot mode
 * @retries:		number of forced programming retries
 */

 /*
 时钟事件设备
 clock_event_device主要用于实现普通定时器和高精度定时器，同时也用于产生tick事件，供给进程调度子系统使用。
 时钟事件设备的核心数据结构是clock_event_device结构，它代表着一个时钟硬件设备，该设备就好像是一个具有事件
 触发能力（通常就是指中断）的clocksource，它不停地计数，当计数值达到预先编程设定的数值那一刻，会引发一个
 时钟事件中断，继而触发该设备的事件处理回调函数，以完成对时钟事件的处理。
 */
 /*
 钟事件设备的核心数据结构是clock_event_device结构，它代表着一个时钟硬件设备，该设备就好像是一个
 具有事件触发能力（通常就是指中断）的clocksource，它不停地计数，当计数值达到预先编程设定的数值
 那一刻，会引发一个时钟事件中断，继而触发该设备的事件处理回调函数，以完成对时钟事件的处理
 */
struct clock_event_device {
	const char		*name;
	unsigned int		features;
	u64			max_delta_ns;//可设置的最大时间差，单位是纳秒。
	u64			min_delta_ns;//可设置的最小时间差，单位是纳秒。
	u32			mult;
	u32			shift;//用于把纳秒转换为cycle。
	int			rating;//设备的精度等级。
	int			irq;
	const struct cpumask	*cpumask;
	//设置下一次时间触发的时间，使用类似于clocksource的cycle计数值（离现在的cycle差值）作为参数。
	int			(*set_next_event)(unsigned long evt,
						  struct clock_event_device *);
	//设置时钟事件设备的工作模式。
	void			(*set_mode)(enum clock_event_mode mode,
					    struct clock_event_device *);
	void			(*broadcast)(const struct cpumask *mask);
	//时间中断到来时，machine底层的的中断服务程序会调用该回调，框架层利用该回调实现对时钟事件的处理。
	void			(*event_handler)(struct clock_event_device *);
	void			(*broadcast)(const struct cpumask *mask);
	struct list_head	list;//系统中注册的时钟事件设备用该字段挂在全局链表变量clockevent_devices上。
	//该时钟事件设备的工作模式，两种主要的工作模式分别是：
	//CLOCK_EVT_MODE_PERIODIC  周期触发模式，设置后按给定的周期不停地触发事件；
	//CLOCK_EVT_MODE_ONESHOT  单次触发模式，只在设置好的触发时刻触发一次；
	enum clock_event_mode	mode;
	ktime_t			next_event;
	unsigned long		retries;
};

/*
 * Calculate a multiplication factor for scaled math, which is used to convert
 * nanoseconds based values to clock ticks:
 *
 * clock_ticks = (nanoseconds * factor) >> shift.
 *
 * div_sc is the rearranged equation to calculate a factor from a given clock
 * ticks / nanoseconds ratio:
 *
 * factor = (clock_ticks << shift) / nanoseconds
 */
static inline unsigned long div_sc(unsigned long ticks, unsigned long nsec,
				   int shift)
{
	uint64_t tmp = ((uint64_t)ticks) << shift;

	do_div(tmp, nsec);
	return (unsigned long) tmp;
}

/* Clock event layer functions */
extern u64 clockevent_delta2ns(unsigned long latch,
			       struct clock_event_device *evt);
extern void clockevents_register_device(struct clock_event_device *dev);

extern void clockevents_exchange_device(struct clock_event_device *old,
					struct clock_event_device *new);
extern void clockevents_set_mode(struct clock_event_device *dev,
				 enum clock_event_mode mode);
extern int clockevents_register_notifier(struct notifier_block *nb);
extern int clockevents_program_event(struct clock_event_device *dev,
				     ktime_t expires, ktime_t now);

extern void clockevents_handle_noop(struct clock_event_device *dev);

static inline void
clockevents_calc_mult_shift(struct clock_event_device *ce, u32 freq, u32 minsec)
{
	return clocks_calc_mult_shift(&ce->mult, &ce->shift, NSEC_PER_SEC,
				      freq, minsec);
}

#ifdef CONFIG_GENERIC_CLOCKEVENTS
extern void clockevents_notify(unsigned long reason, void *arg);
#else
# define clockevents_notify(reason, arg) do { } while (0)
#endif

#else /* CONFIG_GENERIC_CLOCKEVENTS_BUILD */

#define clockevents_notify(reason, arg) do { } while (0)

#endif

#endif
