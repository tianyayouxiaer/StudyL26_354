/*  linux/include/linux/tick.h
 *
 *  This file contains the structure definitions for tick related functions
 *
 */
#ifndef _LINUX_TICK_H
#define _LINUX_TICK_H

#include <linux/clockchips.h>

/*
tick_device是基于clock_event_device的进一步封装，用于代替原有的时钟滴答中断，
给内核提供tick事件，以完成进程的调度和进程信息统计，负载平衡和时间更新等操作。
*/


#ifdef CONFIG_GENERIC_CLOCKEVENTS

enum tick_device_mode {
	TICKDEV_MODE_PERIODIC,
	TICKDEV_MODE_ONESHOT,
};

// 当内核没有配置成支持高精度定时器时，系统的tick由tick_device产生，
// tick_device其实是clock_event_device的简单封装，它内嵌了一个clock_event_device
// 指针和它的工作模式：
struct tick_device {
	struct clock_event_device *evtdev;
	enum tick_device_mode mode;
};

//低分辨率下的动态时钟:不管tick_device的工作模式（周期触发或者是单次触发），
//tick_device所关联的clock_event_device的事件回调处理函数都是：tick_handle_periodic，
//不管当前是否处于idle状态，他都会精确地按HZ数来提供周期性的tick事件，这不符合动态时钟的要求，
//所以，要使动态时钟发挥作用，系统首先要切换至支持动态时钟的工作模式：NOHZ_MODE_LOWRES  。
enum tick_nohz_mode {
	NOHZ_MODE_INACTIVE,//系统动态时钟尚未激活
	NOHZ_MODE_LOWRES,//系统工作于低分辨率模式下的动态时钟
	NOHZ_MODE_HIGHRES,//系统工作于高精度模式下的动态时钟
};

/**
 * struct tick_sched - sched tick emulation and no idle tick control/stats
 * @sched_timer:	hrtimer to schedule the periodic tick in high
 *			resolution mode
 * @idle_tick:		Store the last idle tick expiry time when the tick
 *			timer is modified for idle sleeps. This is necessary
 *			to resume the tick timer operation in the timeline
 *			when the CPU returns from idle
 * @tick_stopped:	Indicator that the idle tick has been stopped
 * @idle_jiffies:	jiffies at the entry to idle for idle time accounting
 * @idle_calls:		Total number of idle calls
 * @idle_sleeps:	Number of idle calls, where the sched tick was stopped
 * @idle_entrytime:	Time when the idle call was entered
 * @idle_waketime:	Time when the idle was interrupted
 * @idle_exittime:	Time when the idle state was left
 * @idle_sleeptime:	Sum of the time slept in idle with sched tick stopped
 * @iowait_sleeptime:	Sum of the time slept in idle with sched tick stopped, with IO outstanding
 * @sleep_length:	Duration of the current idle sleep
 * @do_timer_lst:	CPU was the last one doing do_timer before going idle
 */
 //  动态时钟数据结构
struct tick_sched {
	struct hrtimer			sched_timer;//用于实现动态时钟的定时器, 该字段用于在高精度模式下，模拟周期时钟的一个hrtimer
	unsigned long			check_clocks;//实现clock_event_device和clocksource的异步通知机制，帮助系统切换至高精度模式或者是动态时钟模式。
	enum tick_nohz_mode		nohz_mode;//当前动态时钟所处的模式
	ktime_t				idle_tick;//保存停止周期时钟是时内核时间，当退出idle时要恢复周期时钟，需要使用该时间，以保持系统中时间线（jiffies）的正确性。
	int				inidle;//处于idle进程中
	int				tick_stopped;//1，即当前没有基于周期时钟信号的工作要做，否则为0 ，表明idle状态的周期时钟已经停止
	unsigned long			idle_jiffies;//系统进入idle时的jiffies值，用于信息统计
	unsigned long			idle_calls;//系统进入idle的统计次数 
	unsigned long			idle_sleeps;//系统进入idle且成功停掉周期时钟的次数
	int				idle_active;//表明目前系统是否处于idle状态中
	ktime_t				idle_entrytime; //系统进入idle的时刻
	ktime_t				idle_waketime;//系统退出idle的时刻
	ktime_t				idle_exittime;//离开idle状态的时间
	ktime_t				idle_sleeptime;//累计各次idle中停止周期时钟的总时间
	ktime_t				iowait_sleeptime;
	ktime_t				sleep_length;//本次idle中停止周期时钟的时间，存储周期时钟将禁用的长度，即从时钟禁用起，到预计将发生下一个时钟信号为止
	unsigned long			last_jiffies;//系统中最后一次周期时钟的jiffies值
	unsigned long			next_jiffies;//预计下一次周期时钟的jiffies
	ktime_t				idle_expires;//进入idle后，下一个最先到期的定时器时刻
	int				do_timer_last;//记录此cpu在停用时钟之前是否为执行do_timer的cpu
};

extern void __init tick_init(void);
extern int tick_is_oneshot_available(void);
extern struct tick_device *tick_get_device(int cpu);

# ifdef CONFIG_HIGH_RES_TIMERS
extern int tick_init_highres(void);
extern int tick_program_event(ktime_t expires, int force);
extern void tick_setup_sched_timer(void);
# endif

# if defined CONFIG_NO_HZ || defined CONFIG_HIGH_RES_TIMERS
extern void tick_cancel_sched_timer(int cpu);
# else
static inline void tick_cancel_sched_timer(int cpu) { }
# endif

# ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
extern struct tick_device *tick_get_broadcast_device(void);
extern struct cpumask *tick_get_broadcast_mask(void);

#  ifdef CONFIG_TICK_ONESHOT
extern struct cpumask *tick_get_broadcast_oneshot_mask(void);
#  endif

# endif /* BROADCAST */

# ifdef CONFIG_TICK_ONESHOT
extern void tick_clock_notify(void);
extern int tick_check_oneshot_change(int allow_nohz);
extern struct tick_sched *tick_get_tick_sched(int cpu);
extern void tick_check_idle(int cpu);
extern int tick_oneshot_mode_active(void);
#  ifndef arch_needs_cpu
#   define arch_needs_cpu(cpu) (0)
#  endif
# else
static inline void tick_clock_notify(void) { }
static inline int tick_check_oneshot_change(int allow_nohz) { return 0; }
static inline void tick_check_idle(int cpu) { }
static inline int tick_oneshot_mode_active(void) { return 0; }
# endif

#else /* CONFIG_GENERIC_CLOCKEVENTS */
static inline void tick_init(void) { }
static inline void tick_cancel_sched_timer(int cpu) { }
static inline void tick_clock_notify(void) { }
static inline int tick_check_oneshot_change(int allow_nohz) { return 0; }
static inline void tick_check_idle(int cpu) { }
static inline int tick_oneshot_mode_active(void) { return 0; }
#endif /* !CONFIG_GENERIC_CLOCKEVENTS */

# ifdef CONFIG_NO_HZ
extern void tick_nohz_stop_sched_tick(int inidle);
extern void tick_nohz_restart_sched_tick(void);
extern ktime_t tick_nohz_get_sleep_length(void);
extern u64 get_cpu_idle_time_us(int cpu, u64 *last_update_time);
extern u64 get_cpu_iowait_time_us(int cpu, u64 *last_update_time);
# else
static inline void tick_nohz_stop_sched_tick(int inidle) { }
static inline void tick_nohz_restart_sched_tick(void) { }
static inline ktime_t tick_nohz_get_sleep_length(void)
{
	ktime_t len = { .tv64 = NSEC_PER_SEC/HZ };

	return len;
}
static inline u64 get_cpu_idle_time_us(int cpu, u64 *unused) { return -1; }
static inline u64 get_cpu_iowait_time_us(int cpu, u64 *unused) { return -1; }
# endif /* !NO_HZ */

#endif
