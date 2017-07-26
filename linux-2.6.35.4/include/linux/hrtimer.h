/*
 *  include/linux/hrtimer.h
 *
 *  hrtimers - High-resolution kernel timers
 *
 *   Copyright(C) 2005, Thomas Gleixner <tglx@linutronix.de>
 *   Copyright(C) 2005, Red Hat, Inc., Ingo Molnar
 *
 *  data type definitions, declarations, prototypes
 *
 *  Started by: Thomas Gleixner and Ingo Molnar
 *
 *  For licencing details see kernel-base/COPYING
 */
#ifndef _LINUX_HRTIMER_H
#define _LINUX_HRTIMER_H

#include <linux/rbtree.h>
#include <linux/ktime.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/percpu.h>
#include <linux/timer.h>


struct hrtimer_clock_base;
struct hrtimer_cpu_base;

/*
 * Mode arguments of xxx_hrtimer functions:
 */
enum hrtimer_mode {
	HRTIMER_MODE_ABS = 0x0,		/* Time value is absolute */
	HRTIMER_MODE_REL = 0x1,		/* Time value is relative to now */
	HRTIMER_MODE_PINNED = 0x02,	/* Timer is bound to CPU */
	HRTIMER_MODE_ABS_PINNED = 0x02,
	HRTIMER_MODE_REL_PINNED = 0x03,
};

/*
 * Return values for the callback function
 */
 // 超时回调函数返回值，决定是否需要再被重新激活
enum hrtimer_restart {
	HRTIMER_NORESTART,	/* Timer is not restarted */
	HRTIMER_RESTART,	/* Timer must be restarted */
};

/*
 * Values to track state of the timer
 *
 * Possible states:
 *
 * 0x00		inactive
 * 0x01		enqueued into rbtree
 * 0x02		callback function running
 *
 * Special cases:
 * 0x03		callback function running and enqueued
 *		(was requeued on another CPU)
 * 0x09		timer was migrated on CPU hotunplug
 * The "callback function running and enqueued" status is only possible on
 * SMP. It happens for example when a posix timer expired and the callback
 * queued a signal. Between dropping the lock which protects the posix timer
 * and reacquiring the base lock of the hrtimer, another CPU can deliver the
 * signal and rearm the timer. We have to preserve the callback running state,
 * as otherwise the timer could be removed before the softirq code finishes the
 * the handling of the timer.
 *
 * The HRTIMER_STATE_ENQUEUED bit is always or'ed to the current state to
 * preserve the HRTIMER_STATE_CALLBACK bit in the above scenario.
 *
 * All state transitions are protected by cpu_base->lock.
 */
#define HRTIMER_STATE_INACTIVE	0x00
#define HRTIMER_STATE_ENQUEUED	0x01
#define HRTIMER_STATE_CALLBACK	0x02
#define HRTIMER_STATE_MIGRATE	0x04

/**
 * struct hrtimer - the basic hrtimer structure
 * @node:	red black tree node for time ordered insertion
 * @_expires:	the absolute expiry time in the hrtimers internal
 *		representation. The time is related to the clock on
 *		which the timer is based. Is setup by adding
 *		slack to the _softexpires value. For non range timers
 *		identical to _softexpires.
 * @_softexpires: the absolute earliest expiry time of the hrtimer.
 *		The time which was given as expiry time when the timer
 *		was armed.
 * @function:	timer expiry callback function
 * @base:	pointer to the timer base (per cpu and per clock)
 * @state:	state information (See bit values above)
 * @start_site:	timer statistics field to store the site where the timer
 *		was started
 * @start_comm: timer statistics field to store the name of the process which
 *		started the timer
 * @start_pid: timer statistics field to store the pid of the task which
 *		started the timer
 *
 * The hrtimer structure must be initialized by hrtimer_init()
 */
struct hrtimer {
	struct rb_node			node;
	/*
	设定了hrtimer的到期时间的一个范围，hrtimer可以在hrtimer._softexpires至timerqueue_node.expires之间的任何时刻到期，
	我们也称timerqueue_node.expires为硬过期时间(hard)，意思很明显：到了此时刻，定时器一定会到期，有了这个范围可以选
	择，定时器系统可以让范围接近的多个定时器在同一时刻同时到期，这种设计可以降低进程频繁地被hrtimer进行唤醒。
	*/
	ktime_t				_expires;//定时器的到期时间
	ktime_t				_softexpires;
	//定时器一旦到期，function字段指定的回调函数会被调用，该函数的返回值为一个枚举值，
	//它决定了该hrtimer是否需要被重新激活
	/*
		hrtimer的到期时间可以基于以下几种时间基准系统：
		HRTIMER_BASE_MONOTONIC,  // 单调递增的monotonic时间，不包含休眠时间	
		HRTIMER_BASE_REALTIME,	 // 平常使用的墙上真实时间  
		HRTIMER_BASE_BOOTTIME,	 // 单调递增的boottime，包含休眠时间  
		HRTIMER_MAX_CLOCK_BASES, // 用于后续数组的定义  
	*/
	enum hrtimer_restart		(*function)(struct hrtimer *);
	struct hrtimer_clock_base	*base;
	//state字段用于表示hrtimer当前的状态
	/*
		#define HRTIMER_STATE_INACTIVE  0x00  // 定时器未激活  
		#define HRTIMER_STATE_ENQUEUED  0x01  // 定时器已经被排入红黑树中  
		#define HRTIMER_STATE_CALLBACK  0x02  // 定时器的回调函数正在被调用  
		#define HRTIMER_STATE_MIGRATE   0x04  // 定时器正在CPU之间做迁移
	*/
	unsigned long			state;
#ifdef CONFIG_TIMER_STATS
	int				start_pid;
	void				*start_site;
	char				start_comm[16];
#endif
};

/**
 * struct hrtimer_sleeper - simple sleeper structure
 * @timer:	embedded timer structure
 * @task:	task to wake up
 *
 * task is set to NULL, when the timer expires.
 */
struct hrtimer_sleeper {
	struct hrtimer timer;
	struct task_struct *task;
};

/**
 * struct hrtimer_clock_base - the timer base for a specific clock
 * @cpu_base:		per cpu clock base
 * @index:		clock type index for per_cpu support when moving a
 *			timer to a base on another cpu.
 * @active:		red black tree root node for the active timers
 * @first:		pointer to the timer node which expires first
 * @resolution:		the resolution of the clock, in nanoseconds
 * @get_time:		function to retrieve the current time of the clock
 * @softirq_time:	the time when running the hrtimer queue in the softirq
 * @offset:		offset of this clock to the monotonic base
 */
 /*
 每个cpu有一个hrtimer_cpu_base结构；
 hrtimer_cpu_base结构管理着3种不同的时间基准系统的hrtimer，分别是：实时时间，启动时间和单调时间；
 每种时间基准系统通过它的active字段（timerqueue_head结构指针），指向它们各自的红黑树；
 红黑树上，按到期时间进行排序，最先到期的hrtimer位于最左下的节点，并被记录在active.next字段中；
 3中时间基准的最先到期时间可能不同，所以，它们之中最先到期的时间被记录在hrtimer_cpu_base的
 expires_next字段中。
 */

 //  时钟基础数据结构  
//   hrtimer组织成红黑树的形式  
struct hrtimer_clock_base {
	struct hrtimer_cpu_base	*cpu_base;// 指向所属cpu的hrtimer_cpu_base结构
	clockid_t		index;//用于区分CLOCK_MONOTONIC, CLOCK_REALTIME 
	struct rb_root		active;// 红黑树，包含了所有使用该时间基准系统的hrtimer
	struct rb_node		*first;//第一个到期的hrtimer 
	ktime_t			resolution;// 时间基准系统的分辨率 
	ktime_t			(*get_time)(void);// 获取该基准系统的时间函数
	ktime_t			softirq_time; //当用jiffies
#ifdef CONFIG_HIGH_RES_TIMERS
	ktime_t			offset;//时钟相对于单调时钟的偏移  
#endif
};

#define HRTIMER_MAX_CLOCK_BASES 2

/*
 * struct hrtimer_cpu_base - the per cpu clock bases
 * @lock:		lock protecting the base and associated clock bases
 *			and timers
 * @clock_base:		array of clock bases for this cpu
 * @curr_timer:		the timer which is executing a callback right now
 * @expires_next:	absolute time of the next event which was scheduled
 *			via clock_set_next_event()
 * @hres_active:	State of high resolution mode
 * @hang_detected:	The last hrtimer interrupt detected a hang
 * @nr_events:		Total number of hrtimer interrupt events
 * @nr_retries:		Total number of hrtimer interrupt retries
 * @nr_hangs:		Total number of hrtimer interrupt hangs
 * @max_hang_time:	Maximum time spent in hrtimer_interrupt
 */
 /*
 和低分辨率定时器一样，处于效率和上锁的考虑，每个cpu单独管理属于自己的hrtimer，
 为此，专门定义了一个结构hrtimer_cpu_base：
 */
 //  cpu时钟基础数据结构  
//      提供两种时钟基础  
//          CLOCK_MONOTOMIC,系统启动时从0开始，不会跳变，始终单调的运行  
//          CLOCK_REALTIME,系统的实际时间，当系统时间改变时，会发生跳变
struct hrtimer_cpu_base {
	raw_spinlock_t			lock;
	struct hrtimer_clock_base	clock_base[HRTIMER_MAX_CLOCK_BASES];//当前cpu的时钟基础
#ifdef CONFIG_HIGH_RES_TIMERS
	ktime_t				expires_next;//下一次事件到期的绝对时间
	int				hres_active;//高分辨率状态
	int				hang_detected;//最近一次hrtimer检测到挂起
	unsigned long			nr_events;//中断事件总数
	unsigned long			nr_retries;//有效的中断事件总数
	unsigned long			nr_hangs;//中断挂起的次数
	ktime_t				max_hang_time; //中断处理程序hrtimer_interrupt可花费的最大时间
#endif
};

static inline void hrtimer_set_expires(struct hrtimer *timer, ktime_t time)
{
	timer->_expires = time;
	timer->_softexpires = time;
}

static inline void hrtimer_set_expires_range(struct hrtimer *timer, ktime_t time, ktime_t delta)
{
	timer->_softexpires = time;
	timer->_expires = ktime_add_safe(time, delta);
}

static inline void hrtimer_set_expires_range_ns(struct hrtimer *timer, ktime_t time, unsigned long delta)
{
	timer->_softexpires = time;
	timer->_expires = ktime_add_safe(time, ns_to_ktime(delta));
}

static inline void hrtimer_set_expires_tv64(struct hrtimer *timer, s64 tv64)
{
	timer->_expires.tv64 = tv64;
	timer->_softexpires.tv64 = tv64;
}

static inline void hrtimer_add_expires(struct hrtimer *timer, ktime_t time)
{
	timer->_expires = ktime_add_safe(timer->_expires, time);
	timer->_softexpires = ktime_add_safe(timer->_softexpires, time);
}

static inline void hrtimer_add_expires_ns(struct hrtimer *timer, u64 ns)
{
	timer->_expires = ktime_add_ns(timer->_expires, ns);
	timer->_softexpires = ktime_add_ns(timer->_softexpires, ns);
}

static inline ktime_t hrtimer_get_expires(const struct hrtimer *timer)
{
	return timer->_expires;
}

static inline ktime_t hrtimer_get_softexpires(const struct hrtimer *timer)
{
	return timer->_softexpires;
}

static inline s64 hrtimer_get_expires_tv64(const struct hrtimer *timer)
{
	return timer->_expires.tv64;
}
static inline s64 hrtimer_get_softexpires_tv64(const struct hrtimer *timer)
{
	return timer->_softexpires.tv64;
}

static inline s64 hrtimer_get_expires_ns(const struct hrtimer *timer)
{
	return ktime_to_ns(timer->_expires);
}

static inline ktime_t hrtimer_expires_remaining(const struct hrtimer *timer)
{
    return ktime_sub(timer->_expires, timer->base->get_time());
}

#ifdef CONFIG_HIGH_RES_TIMERS
struct clock_event_device;

extern void clock_was_set(void);
extern void hres_timers_resume(void);
extern void hrtimer_interrupt(struct clock_event_device *dev);

/*
 * In high resolution mode the time reference must be read accurate
 */
static inline ktime_t hrtimer_cb_get_time(struct hrtimer *timer)
{
	return timer->base->get_time();
}

static inline int hrtimer_is_hres_active(struct hrtimer *timer)
{
	return timer->base->cpu_base->hres_active;
}

extern void hrtimer_peek_ahead_timers(void);

/*
 * The resolution of the clocks. The resolution value is returned in
 * the clock_getres() system call to give application programmers an
 * idea of the (in)accuracy of timers. Timer values are rounded up to
 * this resolution values.
 */
# define HIGH_RES_NSEC		1
# define KTIME_HIGH_RES		(ktime_t) { .tv64 = HIGH_RES_NSEC }
# define MONOTONIC_RES_NSEC	HIGH_RES_NSEC
# define KTIME_MONOTONIC_RES	KTIME_HIGH_RES

#else

# define MONOTONIC_RES_NSEC	LOW_RES_NSEC
# define KTIME_MONOTONIC_RES	KTIME_LOW_RES

/*
 * clock_was_set() is a NOP for non- high-resolution systems. The
 * time-sorted order guarantees that a timer does not expire early and
 * is expired in the next softirq when the clock was advanced.
 */
static inline void clock_was_set(void) { }
static inline void hrtimer_peek_ahead_timers(void) { }

static inline void hres_timers_resume(void) { }

/*
 * In non high resolution mode the time reference is taken from
 * the base softirq time variable.
 */
static inline ktime_t hrtimer_cb_get_time(struct hrtimer *timer)
{
	return timer->base->softirq_time;
}

static inline int hrtimer_is_hres_active(struct hrtimer *timer)
{
	return 0;
}
#endif

extern ktime_t ktime_get(void);
extern ktime_t ktime_get_real(void);


DECLARE_PER_CPU(struct tick_device, tick_cpu_device);


/* Exported timer functions: */

/* Initialize timers: */
extern void hrtimer_init(struct hrtimer *timer, clockid_t which_clock,
			 enum hrtimer_mode mode);

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS
extern void hrtimer_init_on_stack(struct hrtimer *timer, clockid_t which_clock,
				  enum hrtimer_mode mode);

extern void destroy_hrtimer_on_stack(struct hrtimer *timer);
#else
static inline void hrtimer_init_on_stack(struct hrtimer *timer,
					 clockid_t which_clock,
					 enum hrtimer_mode mode)
{
	hrtimer_init(timer, which_clock, mode);
}
static inline void destroy_hrtimer_on_stack(struct hrtimer *timer) { }
#endif

/* Basic timer operations: */
extern int hrtimer_start(struct hrtimer *timer, ktime_t tim,
			 const enum hrtimer_mode mode);
extern int hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
			unsigned long range_ns, const enum hrtimer_mode mode);
extern int
__hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
			 unsigned long delta_ns,
			 const enum hrtimer_mode mode, int wakeup);

extern int hrtimer_cancel(struct hrtimer *timer);
extern int hrtimer_try_to_cancel(struct hrtimer *timer);

static inline int hrtimer_start_expires(struct hrtimer *timer,
						enum hrtimer_mode mode)
{
	unsigned long delta;
	ktime_t soft, hard;
	soft = hrtimer_get_softexpires(timer);
	hard = hrtimer_get_expires(timer);
	delta = ktime_to_ns(ktime_sub(hard, soft));
	return hrtimer_start_range_ns(timer, soft, delta, mode);
}

static inline int hrtimer_restart(struct hrtimer *timer)
{
	return hrtimer_start_expires(timer, HRTIMER_MODE_ABS);
}

/* Query timers: */
extern ktime_t hrtimer_get_remaining(const struct hrtimer *timer);
extern int hrtimer_get_res(const clockid_t which_clock, struct timespec *tp);

extern ktime_t hrtimer_get_next_event(void);

/*
 * A timer is active, when it is enqueued into the rbtree or the callback
 * function is running.
 */
static inline int hrtimer_active(const struct hrtimer *timer)
{
	return timer->state != HRTIMER_STATE_INACTIVE;
}

/*
 * Helper function to check, whether the timer is on one of the queues
 */
static inline int hrtimer_is_queued(struct hrtimer *timer)
{
	return timer->state & HRTIMER_STATE_ENQUEUED;
}

/*
 * Helper function to check, whether the timer is running the callback
 * function
 */
static inline int hrtimer_callback_running(struct hrtimer *timer)
{
	return timer->state & HRTIMER_STATE_CALLBACK;
}

/* Forward a hrtimer so it expires after now: */
extern u64
hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval);

/* Forward a hrtimer so it expires after the hrtimer's current now */
static inline u64 hrtimer_forward_now(struct hrtimer *timer,
				      ktime_t interval)
{
	return hrtimer_forward(timer, timer->base->get_time(), interval);
}

/* Precise sleep: */
extern long hrtimer_nanosleep(struct timespec *rqtp,
			      struct timespec __user *rmtp,
			      const enum hrtimer_mode mode,
			      const clockid_t clockid);
extern long hrtimer_nanosleep_restart(struct restart_block *restart_block);

extern void hrtimer_init_sleeper(struct hrtimer_sleeper *sl,
				 struct task_struct *tsk);

extern int schedule_hrtimeout_range(ktime_t *expires, unsigned long delta,
						const enum hrtimer_mode mode);
extern int schedule_hrtimeout_range_clock(ktime_t *expires,
		unsigned long delta, const enum hrtimer_mode mode, int clock);
extern int schedule_hrtimeout(ktime_t *expires, const enum hrtimer_mode mode);

/* Soft interrupt function to run the hrtimer queues: */
extern void hrtimer_run_queues(void);
extern void hrtimer_run_pending(void);

/* Bootup initialization: */
extern void __init hrtimers_init(void);

#if BITS_PER_LONG < 64
extern u64 ktime_divns(const ktime_t kt, s64 div);
#else /* BITS_PER_LONG < 64 */
# define ktime_divns(kt, div)		(u64)((kt).tv64 / (div))
#endif

/* Show pending timers: */
extern void sysrq_timer_list_show(void);

#endif
