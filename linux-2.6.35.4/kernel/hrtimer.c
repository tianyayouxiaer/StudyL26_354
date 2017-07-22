/*
 *  linux/kernel/hrtimer.c
 *
 *  Copyright(C) 2005-2006, Thomas Gleixner <tglx@linutronix.de>
 *  Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 *  Copyright(C) 2006-2007  Timesys Corp., Thomas Gleixner
 *
 *  High-resolution kernel timers
 *
 *  In contrast to the low-resolution timeout API implemented in
 *  kernel/timer.c, hrtimers provide finer resolution and accuracy
 *  depending on system configuration and capabilities.
 *
 *  These timers are currently used for:
 *   - itimers
 *   - POSIX timers
 *   - nanosleep
 *   - precise in-kernel timing
 *
 *  Started by: Thomas Gleixner and Ingo Molnar
 *
 *  Credits:
 *	based on kernel/timer.c
 *
 *	Help, testing, suggestions, bugfixes, improvements were
 *	provided by:
 *
 *	George Anzinger, Andrew Morton, Steven Rostedt, Roman Zippel
 *	et. al.
 *
 *  For licencing details see kernel-base/COPYING
 */

#include <linux/cpu.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/hrtimer.h>
#include <linux/notifier.h>
#include <linux/syscalls.h>
#include <linux/kallsyms.h>
#include <linux/interrupt.h>
#include <linux/tick.h>
#include <linux/seq_file.h>
#include <linux/err.h>
#include <linux/debugobjects.h>
#include <linux/sched.h>
#include <linux/timer.h>

#include <asm/uaccess.h>

#include <trace/events/timer.h>

/*
 * The timer bases:
 *
 * Note: If we want to add new timer bases, we have to skip the two
 * clock ids captured by the cpu-timers. We do this by holding empty
 * entries rather than doing math adjustment of the clock ids.
 * This ensures that we capture erroneous accesses to these clock ids
 * rather than moving them into the range of valid clock id's.
 */
DEFINE_PER_CPU(struct hrtimer_cpu_base, hrtimer_bases) =
{

	.clock_base =
	{
		{
			.index = CLOCK_REALTIME,
			.get_time = &ktime_get_real,
			.resolution = KTIME_LOW_RES,
		},
		{
			.index = CLOCK_MONOTONIC,
			.get_time = &ktime_get,
			.resolution = KTIME_LOW_RES,
		},
	}
};

/*
 * Get the coarse grained time at the softirq based on xtime and
 * wall_to_monotonic.
 */
static void hrtimer_get_softirq_time(struct hrtimer_cpu_base *base)
{
	ktime_t xtim, tomono;
	struct timespec xts, tom;
	unsigned long seq;

	do {
		seq = read_seqbegin(&xtime_lock);
		xts = __current_kernel_time();
		tom = wall_to_monotonic;
	} while (read_seqretry(&xtime_lock, seq));

	xtim = timespec_to_ktime(xts);
	tomono = timespec_to_ktime(tom);
	base->clock_base[CLOCK_REALTIME].softirq_time = xtim;
	base->clock_base[CLOCK_MONOTONIC].softirq_time =
		ktime_add(xtim, tomono);
}

/*
 * Functions and macros which are different for UP/SMP systems are kept in a
 * single place
 */
#ifdef CONFIG_SMP

/*
 * We are using hashed locking: holding per_cpu(hrtimer_bases)[n].lock
 * means that all timers which are tied to this base via timer->base are
 * locked, and the base itself is locked too.
 *
 * So __run_timers/migrate_timers can safely modify all timers which could
 * be found on the lists/queues.
 *
 * When the timer's base is locked, and the timer removed from list, it is
 * possible to set timer->base = NULL and drop the lock: the timer remains
 * locked.
 */
static
struct hrtimer_clock_base *lock_hrtimer_base(const struct hrtimer *timer,
					     unsigned long *flags)
{
	struct hrtimer_clock_base *base;

	for (;;) {
		base = timer->base;
		if (likely(base != NULL)) {
			raw_spin_lock_irqsave(&base->cpu_base->lock, *flags);
			if (likely(base == timer->base))
				return base;
			/* The timer has migrated to another CPU: */
			raw_spin_unlock_irqrestore(&base->cpu_base->lock, *flags);
		}
		cpu_relax();
	}
}


/*
 * Get the preferred target CPU for NOHZ
 */
static int hrtimer_get_target(int this_cpu, int pinned)
{
#ifdef CONFIG_NO_HZ
	if (!pinned && get_sysctl_timer_migration() && idle_cpu(this_cpu)) {
		int preferred_cpu = get_nohz_load_balancer();

		if (preferred_cpu >= 0)
			return preferred_cpu;
	}
#endif
	return this_cpu;
}

/*
 * With HIGHRES=y we do not migrate the timer when it is expiring
 * before the next event on the target cpu because we cannot reprogram
 * the target cpu hardware and we would cause it to fire late.
 *
 * Called with cpu_base->lock of target cpu held.
 */
static int
hrtimer_check_target(struct hrtimer *timer, struct hrtimer_clock_base *new_base)
{
#ifdef CONFIG_HIGH_RES_TIMERS
	ktime_t expires;

	if (!new_base->cpu_base->hres_active)
		return 0;

	expires = ktime_sub(hrtimer_get_expires(timer), new_base->offset);
	return expires.tv64 <= new_base->cpu_base->expires_next.tv64;
#else
	return 0;
#endif
}

/*
 * Switch the timer base to the current CPU when possible.
 */
static inline struct hrtimer_clock_base *
switch_hrtimer_base(struct hrtimer *timer, struct hrtimer_clock_base *base,
		    int pinned)
{
	struct hrtimer_clock_base *new_base;
	struct hrtimer_cpu_base *new_cpu_base;
	int this_cpu = smp_processor_id();
	int cpu = hrtimer_get_target(this_cpu, pinned);

again:
	new_cpu_base = &per_cpu(hrtimer_bases, cpu);
	new_base = &new_cpu_base->clock_base[base->index];

	if (base != new_base) {
		/*
		 * We are trying to move timer to new_base.
		 * However we can't change timer's base while it is running,
		 * so we keep it on the same CPU. No hassle vs. reprogramming
		 * the event source in the high resolution case. The softirq
		 * code will take care of this when the timer function has
		 * completed. There is no conflict as we hold the lock until
		 * the timer is enqueued.
		 */
		if (unlikely(hrtimer_callback_running(timer)))
			return base;

		/* See the comment in lock_timer_base() */
		timer->base = NULL;
		raw_spin_unlock(&base->cpu_base->lock);
		raw_spin_lock(&new_base->cpu_base->lock);

		if (cpu != this_cpu && hrtimer_check_target(timer, new_base)) {
			cpu = this_cpu;
			raw_spin_unlock(&new_base->cpu_base->lock);
			raw_spin_lock(&base->cpu_base->lock);
			timer->base = base;
			goto again;
		}
		timer->base = new_base;
	}
	return new_base;
}

#else /* CONFIG_SMP */

static inline struct hrtimer_clock_base *
lock_hrtimer_base(const struct hrtimer *timer, unsigned long *flags)
{
	struct hrtimer_clock_base *base = timer->base;

	raw_spin_lock_irqsave(&base->cpu_base->lock, *flags);

	return base;
}

# define switch_hrtimer_base(t, b, p)	(b)

#endif	/* !CONFIG_SMP */

/*
 * Functions for the union type storage format of ktime_t which are
 * too large for inlining:
 */
#if BITS_PER_LONG < 64
# ifndef CONFIG_KTIME_SCALAR
/**
 * ktime_add_ns - Add a scalar nanoseconds value to a ktime_t variable
 * @kt:		addend
 * @nsec:	the scalar nsec value to add
 *
 * Returns the sum of kt and nsec in ktime_t format
 */
ktime_t ktime_add_ns(const ktime_t kt, u64 nsec)
{
	ktime_t tmp;

	if (likely(nsec < NSEC_PER_SEC)) {
		tmp.tv64 = nsec;
	} else {
		unsigned long rem = do_div(nsec, NSEC_PER_SEC);

		tmp = ktime_set((long)nsec, rem);
	}

	return ktime_add(kt, tmp);
}

EXPORT_SYMBOL_GPL(ktime_add_ns);

/**
 * ktime_sub_ns - Subtract a scalar nanoseconds value from a ktime_t variable
 * @kt:		minuend
 * @nsec:	the scalar nsec value to subtract
 *
 * Returns the subtraction of @nsec from @kt in ktime_t format
 */
ktime_t ktime_sub_ns(const ktime_t kt, u64 nsec)
{
	ktime_t tmp;

	if (likely(nsec < NSEC_PER_SEC)) {
		tmp.tv64 = nsec;
	} else {
		unsigned long rem = do_div(nsec, NSEC_PER_SEC);

		tmp = ktime_set((long)nsec, rem);
	}

	return ktime_sub(kt, tmp);
}

EXPORT_SYMBOL_GPL(ktime_sub_ns);
# endif /* !CONFIG_KTIME_SCALAR */

/*
 * Divide a ktime value by a nanosecond value
 */
u64 ktime_divns(const ktime_t kt, s64 div)
{
	u64 dclc;
	int sft = 0;

	dclc = ktime_to_ns(kt);
	/* Make sure the divisor is less than 2^32: */
	while (div >> 32) {
		sft++;
		div >>= 1;
	}
	dclc >>= sft;
	do_div(dclc, (unsigned long) div);

	return dclc;
}
#endif /* BITS_PER_LONG >= 64 */

/*
 * Add two ktime values and do a safety check for overflow:
 */
ktime_t ktime_add_safe(const ktime_t lhs, const ktime_t rhs)
{
	ktime_t res = ktime_add(lhs, rhs);

	/*
	 * We use KTIME_SEC_MAX here, the maximum timeout which we can
	 * return to user space in a timespec:
	 */
	if (res.tv64 < 0 || res.tv64 < lhs.tv64 || res.tv64 < rhs.tv64)
		res = ktime_set(KTIME_SEC_MAX, 0);

	return res;
}

EXPORT_SYMBOL_GPL(ktime_add_safe);

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS

static struct debug_obj_descr hrtimer_debug_descr;

/*
 * fixup_init is called when:
 * - an active object is initialized
 */
static int hrtimer_fixup_init(void *addr, enum debug_obj_state state)
{
	struct hrtimer *timer = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		hrtimer_cancel(timer);
		debug_object_init(timer, &hrtimer_debug_descr);
		return 1;
	default:
		return 0;
	}
}

/*
 * fixup_activate is called when:
 * - an active object is activated
 * - an unknown object is activated (might be a statically initialized object)
 */
static int hrtimer_fixup_activate(void *addr, enum debug_obj_state state)
{
	switch (state) {

	case ODEBUG_STATE_NOTAVAILABLE:
		WARN_ON_ONCE(1);
		return 0;

	case ODEBUG_STATE_ACTIVE:
		WARN_ON(1);

	default:
		return 0;
	}
}

/*
 * fixup_free is called when:
 * - an active object is freed
 */
static int hrtimer_fixup_free(void *addr, enum debug_obj_state state)
{
	struct hrtimer *timer = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		hrtimer_cancel(timer);
		debug_object_free(timer, &hrtimer_debug_descr);
		return 1;
	default:
		return 0;
	}
}

static struct debug_obj_descr hrtimer_debug_descr = {
	.name		= "hrtimer",
	.fixup_init	= hrtimer_fixup_init,
	.fixup_activate	= hrtimer_fixup_activate,
	.fixup_free	= hrtimer_fixup_free,
};

static inline void debug_hrtimer_init(struct hrtimer *timer)
{
	debug_object_init(timer, &hrtimer_debug_descr);
}

static inline void debug_hrtimer_activate(struct hrtimer *timer)
{
	debug_object_activate(timer, &hrtimer_debug_descr);
}

static inline void debug_hrtimer_deactivate(struct hrtimer *timer)
{
	debug_object_deactivate(timer, &hrtimer_debug_descr);
}

static inline void debug_hrtimer_free(struct hrtimer *timer)
{
	debug_object_free(timer, &hrtimer_debug_descr);
}

static void __hrtimer_init(struct hrtimer *timer, clockid_t clock_id,
			   enum hrtimer_mode mode);

void hrtimer_init_on_stack(struct hrtimer *timer, clockid_t clock_id,
			   enum hrtimer_mode mode)
{
	debug_object_init_on_stack(timer, &hrtimer_debug_descr);
	__hrtimer_init(timer, clock_id, mode);
}
EXPORT_SYMBOL_GPL(hrtimer_init_on_stack);

void destroy_hrtimer_on_stack(struct hrtimer *timer)
{
	debug_object_free(timer, &hrtimer_debug_descr);
}

#else
static inline void debug_hrtimer_init(struct hrtimer *timer) { }
static inline void debug_hrtimer_activate(struct hrtimer *timer) { }
static inline void debug_hrtimer_deactivate(struct hrtimer *timer) { }
#endif

static inline void
debug_init(struct hrtimer *timer, clockid_t clockid,
	   enum hrtimer_mode mode)
{
	debug_hrtimer_init(timer);
	trace_hrtimer_init(timer, clockid, mode);
}

static inline void debug_activate(struct hrtimer *timer)
{
	debug_hrtimer_activate(timer);
	trace_hrtimer_start(timer);
}

static inline void debug_deactivate(struct hrtimer *timer)
{
	debug_hrtimer_deactivate(timer);
	trace_hrtimer_cancel(timer);
}

/* High resolution timer related functions */
#ifdef CONFIG_HIGH_RES_TIMERS

/*
 * High resolution timer enabled ?
 */
static int hrtimer_hres_enabled __read_mostly  = 1;

/*
 * Enable / Disable high resolution mode
 */
static int __init setup_hrtimer_hres(char *str)
{
	if (!strcmp(str, "off"))
		hrtimer_hres_enabled = 0;
	else if (!strcmp(str, "on"))
		hrtimer_hres_enabled = 1;
	else
		return 0;
	return 1;
}

__setup("highres=", setup_hrtimer_hres);

/*
 * hrtimer_high_res_enabled - query, if the highres mode is enabled
 */
static inline int hrtimer_is_hres_enabled(void)
{
	return hrtimer_hres_enabled;
}

/*
 * Is the high resolution mode active ?
 */
static inline int hrtimer_hres_active(void)
{
	return __get_cpu_var(hrtimer_bases).hres_active;
}

/*
 * Reprogram the event source with checking both queues for the
 * next event
 * Called with interrupts disabled and base->lock held
 */
static void
hrtimer_force_reprogram(struct hrtimer_cpu_base *cpu_base, int skip_equal)
{
	int i;
	struct hrtimer_clock_base *base = cpu_base->clock_base;
	ktime_t expires, expires_next;

	expires_next.tv64 = KTIME_MAX;

	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++, base++) {
		struct hrtimer *timer;

		if (!base->first)
			continue;
		timer = rb_entry(base->first, struct hrtimer, node);
		expires = ktime_sub(hrtimer_get_expires(timer), base->offset);
		/*
		 * clock_was_set() has changed base->offset so the
		 * result might be negative. Fix it up to prevent a
		 * false positive in clockevents_program_event()
		 */
		if (expires.tv64 < 0)
			expires.tv64 = 0;
		if (expires.tv64 < expires_next.tv64)
			expires_next = expires;
	}

	if (skip_equal && expires_next.tv64 == cpu_base->expires_next.tv64)
		return;

	cpu_base->expires_next.tv64 = expires_next.tv64;

	if (cpu_base->expires_next.tv64 != KTIME_MAX)
		tick_program_event(cpu_base->expires_next, 1);
}

/*
 * Shared reprogramming for clock_realtime and clock_monotonic
 *
 * When a timer is enqueued and expires earlier than the already enqueued
 * timers, we have to check, whether it expires earlier than the timer for
 * which the clock event device was armed.
 *
 * Called with interrupts disabled and base->cpu_base.lock held
 */
static int hrtimer_reprogram(struct hrtimer *timer,
			     struct hrtimer_clock_base *base)
{
	struct hrtimer_cpu_base *cpu_base = &__get_cpu_var(hrtimer_bases);
	ktime_t expires = ktime_sub(hrtimer_get_expires(timer), base->offset);
	int res;

	WARN_ON_ONCE(hrtimer_get_expires_tv64(timer) < 0);

	/*
	 * When the callback is running, we do not reprogram the clock event
	 * device. The timer callback is either running on a different CPU or
	 * the callback is executed in the hrtimer_interrupt context. The
	 * reprogramming is handled either by the softirq, which called the
	 * callback or at the end of the hrtimer_interrupt.
	 */
	if (hrtimer_callback_running(timer))
		return 0;

	/*
	 * CLOCK_REALTIME timer might be requested with an absolute
	 * expiry time which is less than base->offset. Nothing wrong
	 * about that, just avoid to call into the tick code, which
	 * has now objections against negative expiry values.
	 */
	if (expires.tv64 < 0)
		return -ETIME;

	if (expires.tv64 >= cpu_base->expires_next.tv64)
		return 0;

	/*
	 * If a hang was detected in the last timer interrupt then we
	 * do not schedule a timer which is earlier than the expiry
	 * which we enforced in the hang detection. We want the system
	 * to make progress.
	 */
	if (cpu_base->hang_detected)
		return 0;

	/*
	 * Clockevents returns -ETIME, when the event was in the past.
	 */
	res = tick_program_event(expires, 0);
	if (!IS_ERR_VALUE(res))
		cpu_base->expires_next = expires;
	return res;
}


/*
 * Retrigger next event is called after clock was set
 *
 * Called with interrupts disabled via on_each_cpu()
 */
static void retrigger_next_event(void *arg)
{
	struct hrtimer_cpu_base *base;
	struct timespec realtime_offset;
	unsigned long seq;

	if (!hrtimer_hres_active())
		return;

	do {
		seq = read_seqbegin(&xtime_lock);
		set_normalized_timespec(&realtime_offset,
					-wall_to_monotonic.tv_sec,
					-wall_to_monotonic.tv_nsec);
	} while (read_seqretry(&xtime_lock, seq));

	base = &__get_cpu_var(hrtimer_bases);

	/* Adjust CLOCK_REALTIME offset */
	raw_spin_lock(&base->lock);
	base->clock_base[CLOCK_REALTIME].offset =
		timespec_to_ktime(realtime_offset);

	hrtimer_force_reprogram(base, 0);
	raw_spin_unlock(&base->lock);
}

/*
 * Clock realtime was set
 *
 * Change the offset of the realtime clock vs. the monotonic
 * clock.
 *
 * We might have to reprogram the high resolution timer interrupt. On
 * SMP we call the architecture specific code to retrigger _all_ high
 * resolution timer interrupts. On UP we just disable interrupts and
 * call the high resolution interrupt code.
 */
void clock_was_set(void)
{
	/* Retrigger the CPU local events everywhere */
	on_each_cpu(retrigger_next_event, NULL, 1);
}

/*
 * During resume we might have to reprogram the high resolution timer
 * interrupt (on the local CPU):
 */
void hres_timers_resume(void)
{
	WARN_ONCE(!irqs_disabled(),
		  KERN_INFO "hres_timers_resume() called with IRQs enabled!");

	retrigger_next_event(NULL);
}

/*
 * Initialize the high resolution related parts of cpu_base
 */
static inline void hrtimer_init_hres(struct hrtimer_cpu_base *base)
{
	////下一次事件到期的绝对时间，KTIME_MAX表示没有hrtimer会到期  
	base->expires_next.tv64 = KTIME_MAX;
	////高分辨率未激活状态 
	base->hres_active = 0;
}

/*
 * Initialize the high resolution related parts of a hrtimer
 */
static inline void hrtimer_init_timer_hres(struct hrtimer *timer)
{
}


/*
 * When High resolution timers are active, try to reprogram. Note, that in case
 * the state has HRTIMER_STATE_CALLBACK set, no reprogramming and no expiry
 * check happens. The timer gets enqueued into the rbtree. The reprogramming
 * and expiry check is done in the hrtimer_interrupt or in the softirq.
 */
static inline int hrtimer_enqueue_reprogram(struct hrtimer *timer,
					    struct hrtimer_clock_base *base,
					    int wakeup)
{
	if (base->cpu_base->hres_active && hrtimer_reprogram(timer, base)) {
		if (wakeup) {
			raw_spin_unlock(&base->cpu_base->lock);
			raise_softirq_irqoff(HRTIMER_SOFTIRQ);
			raw_spin_lock(&base->cpu_base->lock);
		} else
			__raise_softirq_irqoff(HRTIMER_SOFTIRQ);

		return 1;
	}

	return 0;
}

/*
 * Switch to high resolution mode
 */
//  切换高分辨率模式  
//  函数任务：  
//      1.更新clockevent为单触发模式，安装高分辨率事件处理程序  
//      2.初始化动态时钟  
//          2.1 更新高分辨率激活标志  
//          2.2 更新时钟基础为高分辨率标志  
//      3.初始化动态时钟  
//      4.更新tick device到期时间为时钟基础中所有hrtimer最短的到期时间
static int hrtimer_switch_to_hres(void)
{
	int cpu = smp_processor_id();
	struct hrtimer_cpu_base *base = &per_cpu(hrtimer_bases, cpu);
	unsigned long flags;

	if (base->hres_active)
		return 1;

	local_irq_save(flags);
	
	//更新clockevent为单触发模式，安装高分辨率事件处理程序
	if (tick_init_highres()) {
		local_irq_restore(flags);
		printk(KERN_WARNING "Could not switch to high resolution "
				    "mode on CPU %d\n", cpu);
		return 0;
	}
	//高分辨率激活标志
	base->hres_active = 1;
	base->clock_base[CLOCK_REALTIME].resolution = KTIME_HIGH_RES;
	base->clock_base[CLOCK_MONOTONIC].resolution = KTIME_HIGH_RES;

	//初始化动态时钟 
	tick_setup_sched_timer();

	/* "Retrigger" the interrupt to get things going */
	//更新tick device到期时间为时钟基础中所有hrtimer最短的到期时间
	/*
	最后，因为tick_device被高精度定时器接管，它将不会再提供原有的tick事件机制，
	所以需要由高精度定时器系统模拟一个tick事件设备，继续为系统提供tick事件能力，
	这个工作由tick_setup_sched_timer函数完成。因为刚刚完成切换，tick_device的到
	期时间并没有被正确地设置为下一个到期定时器的时间，这里使用retrigger_next_event函数，
	传入参数NULL，使得tick_device立刻产生到期中断，hrtimer_interrupt被调用一次，
	然后下一个到期的定时器的时间会编程到tick_device中，从而完成了到高精度模式的切换：
	*/
	retrigger_next_event(NULL);
	local_irq_restore(flags);
	return 1;
}

#else

static inline int hrtimer_hres_active(void) { return 0; }
static inline int hrtimer_is_hres_enabled(void) { return 0; }
static inline int hrtimer_switch_to_hres(void) { return 0; }
static inline void
hrtimer_force_reprogram(struct hrtimer_cpu_base *base, int skip_equal) { }
static inline int hrtimer_enqueue_reprogram(struct hrtimer *timer,
					    struct hrtimer_clock_base *base,
					    int wakeup)
{
	return 0;
}
static inline void hrtimer_init_hres(struct hrtimer_cpu_base *base) { }
static inline void hrtimer_init_timer_hres(struct hrtimer *timer) { }

#endif /* CONFIG_HIGH_RES_TIMERS */

static inline void timer_stats_hrtimer_set_start_info(struct hrtimer *timer)
{
#ifdef CONFIG_TIMER_STATS
	if (timer->start_site)
		return;
	timer->start_site = __builtin_return_address(0);
	memcpy(timer->start_comm, current->comm, TASK_COMM_LEN);
	timer->start_pid = current->pid;
#endif
}

static inline void timer_stats_hrtimer_clear_start_info(struct hrtimer *timer)
{
#ifdef CONFIG_TIMER_STATS
	timer->start_site = NULL;
#endif
}

static inline void timer_stats_account_hrtimer(struct hrtimer *timer)
{
#ifdef CONFIG_TIMER_STATS
	if (likely(!timer_stats_active))
		return;
	timer_stats_update_stats(timer, timer->start_pid, timer->start_site,
				 timer->function, timer->start_comm, 0);
#endif
}

/*
 * Counterpart to lock_hrtimer_base above:
 */
static inline
void unlock_hrtimer_base(const struct hrtimer *timer, unsigned long *flags)
{
	raw_spin_unlock_irqrestore(&timer->base->cpu_base->lock, *flags);
}

/**
 * hrtimer_forward - forward the timer expiry
 * @timer:	hrtimer to forward
 * @now:	forward past this time
 * @interval:	the interval to forward
 *
 * Forward the timer expiry so it will expire in the future.
 * Returns the number of overruns.
 */
u64 hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval)
{
	u64 orun = 1;
	ktime_t delta;

	delta = ktime_sub(now, hrtimer_get_expires(timer));

	if (delta.tv64 < 0)
		return 0;

	if (interval.tv64 < timer->base->resolution.tv64)
		interval.tv64 = timer->base->resolution.tv64;

	if (unlikely(delta.tv64 >= interval.tv64)) {
		s64 incr = ktime_to_ns(interval);

		orun = ktime_divns(delta, incr);
		hrtimer_add_expires_ns(timer, incr * orun);
		if (hrtimer_get_expires_tv64(timer) > now.tv64)
			return orun;
		/*
		 * This (and the ktime_add() below) is the
		 * correction for exact:
		 */
		orun++;
	}
	hrtimer_add_expires(timer, interval);

	return orun;
}
EXPORT_SYMBOL_GPL(hrtimer_forward);

/*
 * enqueue_hrtimer - internal function to (re)start a timer
 *
 * The timer is inserted in expiry order. Insertion into the
 * red black tree is O(log(n)). Must hold the base lock.
 *
 * Returns 1 when the new timer is the leftmost timer in the tree.
 */
static int enqueue_hrtimer(struct hrtimer *timer,
			   struct hrtimer_clock_base *base)
{
	struct rb_node **link = &base->active.rb_node;
	struct rb_node *parent = NULL;
	struct hrtimer *entry;
	int leftmost = 1;

	debug_activate(timer);

	/*
	 * Find the right place in the rbtree:
	 */
	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct hrtimer, node);
		/*
		 * We dont care about collisions. Nodes with
		 * the same expiry time stay together.
		 */
		if (hrtimer_get_expires_tv64(timer) <
				hrtimer_get_expires_tv64(entry)) {
			link = &(*link)->rb_left;
		} else {
			link = &(*link)->rb_right;
			leftmost = 0;
		}
	}

	/*
	 * Insert the timer to the rbtree and check whether it
	 * replaces the first pending timer
	 */
	if (leftmost)
		base->first = &timer->node;

	rb_link_node(&timer->node, parent, link);
	rb_insert_color(&timer->node, &base->active);
	/*
	 * HRTIMER_STATE_ENQUEUED is or'ed to the current state to preserve the
	 * state of a possibly running callback.
	 */
	timer->state |= HRTIMER_STATE_ENQUEUED;

	return leftmost;
}

/*
 * __remove_hrtimer - internal function to remove a timer
 *
 * Caller must hold the base lock.
 *
 * High resolution timer mode reprograms the clock event device when the
 * timer is the one which expires next. The caller can disable this by setting
 * reprogram to zero. This is useful, when the context does a reprogramming
 * anyway (e.g. timer interrupt)
 */
static void __remove_hrtimer(struct hrtimer *timer,
			     struct hrtimer_clock_base *base,
			     unsigned long newstate, int reprogram)
{
	if (!(timer->state & HRTIMER_STATE_ENQUEUED))
		goto out;

	/*
	 * Remove the timer from the rbtree and replace the first
	 * entry pointer if necessary.
	 */
	if (base->first == &timer->node) {
		base->first = rb_next(&timer->node);
#ifdef CONFIG_HIGH_RES_TIMERS
		/* Reprogram the clock event device. if enabled */
		if (reprogram && hrtimer_hres_active()) {
			ktime_t expires;

			expires = ktime_sub(hrtimer_get_expires(timer),
					    base->offset);
			if (base->cpu_base->expires_next.tv64 == expires.tv64)
				hrtimer_force_reprogram(base->cpu_base, 1);
		}
#endif
	}
	rb_erase(&timer->node, &base->active);
out:
	timer->state = newstate;
}

/*
 * remove hrtimer, called with base lock held
 */
static inline int
remove_hrtimer(struct hrtimer *timer, struct hrtimer_clock_base *base)
{
	if (hrtimer_is_queued(timer)) {
		int reprogram;

		/*
		 * Remove the timer and force reprogramming when high
		 * resolution mode is active and the timer is on the current
		 * CPU. If we remove a timer on another CPU, reprogramming is
		 * skipped. The interrupt event on this CPU is fired and
		 * reprogramming happens in the interrupt handler. This is a
		 * rare case and less expensive than a smp call.
		 */
		debug_deactivate(timer);
		timer_stats_hrtimer_clear_start_info(timer);
		reprogram = base->cpu_base == &__get_cpu_var(hrtimer_bases);
		__remove_hrtimer(timer, base, HRTIMER_STATE_INACTIVE,
				 reprogram);
		return 1;
	}
	return 0;
}

//  调用路径：hrtimer_start->hrtimer_start_range_ns->__hrtimer_start_range_ns  
//  函数任务：  
//      1.初始化hrtimer的base字段  
//      2.初始化作为红黑树的节点的node字段 

int __hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
		unsigned long delta_ns, const enum hrtimer_mode mode,
		int wakeup)
{
	struct hrtimer_clock_base *base, *new_base;
	unsigned long flags;
	int ret, leftmost;

	/* 取得hrtimer_clock_base指针 */ 
	base = lock_hrtimer_base(timer, &flags);

	/* Remove an active timer from the queue: */
	/* 如果已经在红黑树中，先移除它: */ 
	ret = remove_hrtimer(timer, base);

	/* Switch the timer base, if necessary: */
	 /* 如果是相对时间，则需要加上当前时间，因为内部是使用绝对时间 */
	new_base = switch_hrtimer_base(timer, base, mode & HRTIMER_MODE_PINNED);

	if (mode & HRTIMER_MODE_REL) {
		tim = ktime_add_safe(tim, new_base->get_time());
		/*
		 * CONFIG_TIME_LOW_RES is a temporary way for architectures
		 * to signal that they simply return xtime in
		 * do_gettimeoffset(). In this case we want to round up by
		 * resolution when starting a relative timer, to avoid short
		 * timeouts. This will go away with the GTOD framework.
		 */
#ifdef CONFIG_TIME_LOW_RES
		tim = ktime_add_safe(tim, base->resolution);
#endif
	}
	
	/* 设置到期的时间范围 */ 
	hrtimer_set_expires_range_ns(timer, tim, delta_ns);

	timer_stats_hrtimer_set_start_info(timer);

	/* 把hrtime按到期时间排序，加入到对应时间基准系统的红黑树中 */  
	/* 如果该定时器的是最早到期的，将会返回true */ 
	leftmost = enqueue_hrtimer(timer, new_base);

	/*
	 * Only allow reprogramming if the new base is on this CPU.
	 * (it might still be on another CPU if the timer was pending)
	 *
	 * XXX send_remote_softirq() ?
	 */
	 //定时器比之前的到期时间要早，所以需要重新对tick_device进行编程，重新设定的的到期时间
	if (leftmost && new_base->cpu_base == &__get_cpu_var(hrtimer_bases))
		hrtimer_enqueue_reprogram(timer, new_base, wakeup);

	unlock_hrtimer_base(timer, &flags);

	return ret;
}

/**
 * hrtimer_start_range_ns - (re)start an hrtimer on the current CPU
 * @timer:	the timer to be added
 * @tim:	expiry time
 * @delta_ns:	"slack" range for the timer
 * @mode:	expiry mode: absolute (HRTIMER_ABS) or relative (HRTIMER_REL)
 *
 * Returns:
 *  0 on success
 *  1 when the timer was active
 */
int hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
		unsigned long delta_ns, const enum hrtimer_mode mode)
{
	return __hrtimer_start_range_ns(timer, tim, delta_ns, mode, 1);
}
EXPORT_SYMBOL_GPL(hrtimer_start_range_ns);

/**
 * hrtimer_start - (re)start an hrtimer on the current CPU
 * @timer:	the timer to be added
 * @tim:	expiry time
 * @mode:	expiry mode: absolute (HRTIMER_ABS) or relative (HRTIMER_REL)
 *
 * Returns:
 *  0 on success
 *  1 when the timer was active
 */
int
hrtimer_start(struct hrtimer *timer, ktime_t tim, const enum hrtimer_mode mode)
{
	return __hrtimer_start_range_ns(timer, tim, 0, mode, 1);
}
EXPORT_SYMBOL_GPL(hrtimer_start);


/**
 * hrtimer_try_to_cancel - try to deactivate a timer
 * @timer:	hrtimer to stop
 *
 * Returns:
 *  0 when the timer was not active
 *  1 when the timer was active
 * -1 when the timer is currently excuting the callback function and
 *    cannot be stopped
 */
int hrtimer_try_to_cancel(struct hrtimer *timer)
{
	struct hrtimer_clock_base *base;
	unsigned long flags;
	int ret = -1;

	base = lock_hrtimer_base(timer, &flags);

	if (!hrtimer_callback_running(timer))
		ret = remove_hrtimer(timer, base);

	unlock_hrtimer_base(timer, &flags);

	return ret;

}
EXPORT_SYMBOL_GPL(hrtimer_try_to_cancel);

/**
 * hrtimer_cancel - cancel a timer and wait for the handler to finish.
 * @timer:	the timer to be cancelled
 *
 * Returns:
 *  0 when the timer was not active
 *  1 when the timer was active
 */
int hrtimer_cancel(struct hrtimer *timer)
{
	for (;;) {
		int ret = hrtimer_try_to_cancel(timer);

		if (ret >= 0)
			return ret;
		cpu_relax();
	}
}
EXPORT_SYMBOL_GPL(hrtimer_cancel);

/**
 * hrtimer_get_remaining - get remaining time for the timer
 * @timer:	the timer to read
 */
ktime_t hrtimer_get_remaining(const struct hrtimer *timer)
{
	struct hrtimer_clock_base *base;
	unsigned long flags;
	ktime_t rem;

	base = lock_hrtimer_base(timer, &flags);
	rem = hrtimer_expires_remaining(timer);
	unlock_hrtimer_base(timer, &flags);

	return rem;
}
EXPORT_SYMBOL_GPL(hrtimer_get_remaining);

#ifdef CONFIG_NO_HZ
/**
 * hrtimer_get_next_event - get the time until next expiry event
 *
 * Returns the delta to the next expiry event or KTIME_MAX if no timer
 * is pending.
 */
ktime_t hrtimer_get_next_event(void)
{
	struct hrtimer_cpu_base *cpu_base = &__get_cpu_var(hrtimer_bases);
	struct hrtimer_clock_base *base = cpu_base->clock_base;
	ktime_t delta, mindelta = { .tv64 = KTIME_MAX };
	unsigned long flags;
	int i;

	raw_spin_lock_irqsave(&cpu_base->lock, flags);

	if (!hrtimer_hres_active()) {
		for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++, base++) {
			struct hrtimer *timer;

			if (!base->first)
				continue;

			timer = rb_entry(base->first, struct hrtimer, node);
			delta.tv64 = hrtimer_get_expires_tv64(timer);
			delta = ktime_sub(delta, base->get_time());
			if (delta.tv64 < mindelta.tv64)
				mindelta.tv64 = delta.tv64;
		}
	}

	raw_spin_unlock_irqrestore(&cpu_base->lock, flags);

	if (mindelta.tv64 < 0)
		mindelta.tv64 = 0;
	return mindelta;
}
#endif


//  调用路径：hrtimer_init->__hrtimer_init  
//  函数任务：  
//      1.初始化hrtimer的base字段  
//      2.初始化作为红黑树的节点的node字段 
static void __hrtimer_init(struct hrtimer *timer, clockid_t clock_id,
			   enum hrtimer_mode mode)
{
	struct hrtimer_cpu_base *cpu_base;

	memset(timer, 0, sizeof(struct hrtimer));

	cpu_base = &__raw_get_cpu_var(hrtimer_bases);

	if (clock_id == CLOCK_REALTIME && mode != HRTIMER_MODE_ABS)
		clock_id = CLOCK_MONOTONIC;

	timer->base = &cpu_base->clock_base[clock_id];
	hrtimer_init_timer_hres(timer);

#ifdef CONFIG_TIMER_STATS
	timer->start_site = NULL;
	timer->start_pid = -1;
	memset(timer->start_comm, 0, TASK_COMM_LEN);
#endif
}

/**
 * hrtimer_init - initialize a timer to the given clock
 * @timer:	the timer to be initialized
 * @clock_id:	the clock to be used
 * @mode:	timer mode abs/rel
 */

 /*
 要添加一个hrtimer，系统提供了一些api供我们使用，首先我们需要定义一个hrtimer结构的实例，
 然后用hrtimer_init函数对它进行初始化

 which_clock可以是CLOCK_REALTIME、CLOCK_MONOTONIC、CLOCK_BOOTTIME中的一种，
 mode则可以是相对时间HRTIMER_MODE_REL，也可以是绝对时间HRTIMER_MODE_ABS。
 */
void hrtimer_init(struct hrtimer *timer, clockid_t clock_id,
		  enum hrtimer_mode mode)
{
	debug_init(timer, clock_id, mode);
	__hrtimer_init(timer, clock_id, mode);
}
EXPORT_SYMBOL_GPL(hrtimer_init);

/**
 * hrtimer_get_res - get the timer resolution for a clock
 * @which_clock: which clock to query
 * @tp:		 pointer to timespec variable to store the resolution
 *
 * Store the resolution of the clock selected by @which_clock in the
 * variable pointed to by @tp.
 */
int hrtimer_get_res(const clockid_t which_clock, struct timespec *tp)
{
	struct hrtimer_cpu_base *cpu_base;

	cpu_base = &__raw_get_cpu_var(hrtimer_bases);
	*tp = ktime_to_timespec(cpu_base->clock_base[which_clock].resolution);

	return 0;
}
EXPORT_SYMBOL_GPL(hrtimer_get_res);
//  运行hrtimer  
//  调用路径：hrtimer_interrupt->__run_hrtimer  
//  函数任务：  
//      1.从红黑树中删除hrtimer  
//      2.设置其为执行状态  
//      3.执行hrtimer回调函数  
//          3.1 如果hrtimer需要继续执行，重新建hrtimer加入到红黑树中  
//      4.清除其正在执行标志 

static void __run_hrtimer(struct hrtimer *timer, ktime_t *now)
{
	struct hrtimer_clock_base *base = timer->base;
	struct hrtimer_cpu_base *cpu_base = base->cpu_base;
	enum hrtimer_restart (*fn)(struct hrtimer *);
	int restart;

	WARN_ON(!irqs_disabled());

	debug_deactivate(timer);
	//从红黑树中删除hrtimer，设置其为执行状态
	__remove_hrtimer(timer, base, HRTIMER_STATE_CALLBACK, 0);
	timer_stats_account_hrtimer(timer);
	fn = timer->function;

	/*
	 * Because we run timers from hardirq context, there is no chance
	 * they get migrated to another cpu, therefore its safe to unlock
	 * the timer base.
	 */
	raw_spin_unlock(&cpu_base->lock);
	trace_hrtimer_expire_entry(timer, now);
	 //执行hrtimer回调函数 
	restart = fn(timer);
	trace_hrtimer_expire_exit(timer);
	raw_spin_lock(&cpu_base->lock);

	/*
	 * Note: We clear the CALLBACK bit after enqueue_hrtimer and
	 * we do not reprogramm the event hardware. Happens either in
	 * hrtimer_start_range_ns() or in hrtimer_interrupt()
	 */
	 //将hrtimer重新入队
	if (restart != HRTIMER_NORESTART) {
		BUG_ON(timer->state != HRTIMER_STATE_CALLBACK);
		enqueue_hrtimer(timer, base);
	}
	//清除其正在执行标志
	timer->state &= ~HRTIMER_STATE_CALLBACK;
}

#ifdef CONFIG_HIGH_RES_TIMERS

/*
 * High resolution timer interrupt
 * Called with interrupts disabled
 */
//  高分辨率tick device事件处理函数  
//      遍历执行本cpu上的hrtimer  
//  调用路径：  
//      1.run_hrtimer_softirq->....->hrtimer_interrupt 软中断路径。  
//      2.当激活高分辨率模式时，安装此函数作为tick device的事件处理函数。  
//  函数任务：  
//      1.遍历时钟基础  
//          1.1 运行到期的hrtimer  
//      2.如果处理完所有hrtimer，关闭时钟事件设备，退出  
//      3.重复步骤1，最多遍历3次  
//      4.更新统计信息  
//          4.1 cpu_base->nr_hangs统计一次hrtimer未能处理完所有hrtimer的次数  
//          4.2 cpu_base->hang_detected指示有hrtimer待处理  
//          4.3 cpu_base->max_hang_time记录hrtimer_interrupt花费的最大时间，即hrtimer可能被延迟的最大时间  
//      5.根据花费的时间计算时钟事件设备下一次到期的时间  
//          5.1 如果处理hrtimer花费时间超过1ms，推迟下一次到期时间为1ms后  
//          5.2 否则下一次到期时间为花费的时间  


//一旦开启了hrtimer，tick_device所关联的clock_event_device的事件回调函数会被修改为：hrtimer_interrupt，
//并且会被设置成工作于CLOCK_EVT_MODE_ONESHOT单触发模式。
void hrtimer_interrupt(struct clock_event_device *dev)
{
	struct hrtimer_cpu_base *cpu_base = &__get_cpu_var(hrtimer_bases);
	struct hrtimer_clock_base *base;
	ktime_t expires_next, now, entry_time, delta;
	int i, retries = 0;

	BUG_ON(!cpu_base->hres_active);
	cpu_base->nr_events++;
	dev->next_event.tv64 = KTIME_MAX;

	entry_time = now = ktime_get();
retry:
	expires_next.tv64 = KTIME_MAX;
	//防止此段时间内其他cpu的hrtimer迁移到本cpu时
	raw_spin_lock(&cpu_base->lock);
	/*
	 * We set expires_next to KTIME_MAX here with cpu_base->lock
	 * held to prevent that a timer is enqueued in our queue via
	 * the migration code. This does not affect enqueueing of
	 * timers which run their callback and need to be requeued on
	 * this CPU.
	 */
	cpu_base->expires_next.tv64 = KTIME_MAX;

	base = cpu_base->clock_base;
	//遍历所有时钟基础的hrtime
	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++) {
		ktime_t basenow;
		struct rb_node *node;
		//时钟基础的时间与实际时间可能有偏移，hrtimer的到期时间为实际时间的绝对值
		basenow = ktime_add(now, base->offset);

		while ((node = base->first)) {
			struct hrtimer *timer;

			timer = rb_entry(node, struct hrtimer, node);

			/*
			 * The immediate goal for using the softexpires is
			 * minimizing wakeups, not running timers at the
			 * earliest interrupt after their soft expiration.
			 * This allows us to avoid using a Priority Search
			 * Tree, which can answer a stabbing querry for
			 * overlapping intervals and instead use the simple
			 * BST we already have.
			 * We don't add extra wakeups by delaying timers that
			 * are right-of a not yet expired timer, because that
			 * timer will have to trigger a wakeup anyway.
			 */
			//如果当前hrtimer还没有到期
			if (basenow.tv64 < hrtimer_get_softexpires_tv64(timer)) {
				ktime_t expires;
				//更新时钟事件下一次到期时间 
				expires = ktime_sub(hrtimer_get_expires(timer),
						    base->offset);
				if (expires.tv64 < expires_next.tv64)
					expires_next = expires;
				break;
			}
			//否则立即执行定时器
			__run_hrtimer(timer, &basenow);
		}
		base++;
	}

	/*
	 * Store the new expiry value so the migration code can verify
	 * against it.
	 */
	 //更新时钟基础下一次到期时间
	cpu_base->expires_next = expires_next;
	raw_spin_unlock(&cpu_base->lock);

	/* Reprogramming necessary ? */
	//没有要到期的hrtimer，关闭时间事件设备
	if (expires_next.tv64 == KTIME_MAX ||
	    !tick_program_event(expires_next, 0)) {
		cpu_base->hang_detected = 0;
		return;
	}

	/*
	 * The next timer was already expired due to:
	 * - tracing
	 * - long lasting callbacks
	 * - being scheduled away when running in a VM
	 *
	 * We need to prevent that we loop forever in the hrtimer
	 * interrupt routine. We give it 3 attempts to avoid
	 * overreacting on some spurious event.
	 */
	 //防止一直处理hrtimer，最大遍历次数3
	 /*
	 如果这时的tick_program_event返回了非0值，表示过期时间已经在当前时间的前面，这通常由以下原因造成：
	 系统正在被调试跟踪，导致时间在走，程序不走；
	 定时器的回调函数花了太长的时间；
	 系统运行在虚拟机中，而虚拟机被调度导致停止运行；
	 为了避免这些情况的发生，接下来系统提供3次机会，重新执行前面的循环，处理到期的定时器：
	 */
	now = ktime_get();
	cpu_base->nr_retries++;
	if (++retries < 3)
		goto retry;

	/*
	如果3次循环后还无法完成到期处理，系统不再循环，转为计算本次总循环的时间，
	然后把tick_device的到期时间强制设置为当前时间加上本次的总循环时间，不过推后的时间被限制在100ms以内：

	*/
	/*
	 * Give the system a chance to do something else than looping
	 * here. We stored the entry time, so we know exactly how long
	 * we spent here. We schedule the next event this amount of
	 * time away.
	 */
	 //有挂起的hrtimer需要处理
	cpu_base->nr_hangs++;
	cpu_base->hang_detected = 1;
	//计算hrtimer_interrupt处理hrtimer花费的时间
	delta = ktime_sub(now, entry_time);
	if (delta.tv64 > cpu_base->max_hang_time.tv64)
		cpu_base->max_hang_time = delta;
	/*
	 * Limit it to a sensible value as we enforce a longer
	 * delay. Give the CPU at least 100ms to catch up.
	 */
	 //根据花费的时间计算时钟事件设备下一次到期的时间  
    //  如果处理hrtimer花费时间超过1ms，推迟下一次到期时间为1ms后  
    //  否则下一次到期时间为花费的时间 
	if (delta.tv64 > 100 * NSEC_PER_MSEC)
		expires_next = ktime_add_ns(now, 100 * NSEC_PER_MSEC);
	else
		expires_next = ktime_add(now, delta);
	tick_program_event(expires_next, 1);
	printk_once(KERN_WARNING "hrtimer: interrupt took %llu ns\n",
		    ktime_to_ns(delta));
}

/*
 * local version of hrtimer_peek_ahead_timers() called with interrupts
 * disabled.
 */
static void __hrtimer_peek_ahead_timers(void)
{
	struct tick_device *td;

	if (!hrtimer_hres_active())
		return;

	td = &__get_cpu_var(tick_cpu_device);
	if (td && td->evtdev)
		hrtimer_interrupt(td->evtdev);
}

/**
 * hrtimer_peek_ahead_timers -- run soft-expired timers now
 *
 * hrtimer_peek_ahead_timers will peek at the timer queue of
 * the current cpu and check if there are any timers for which
 * the soft expires time has passed. If any such timers exist,
 * they are run immediately and then removed from the timer queue.
 *
 */
//  遍历运行hrtimer  
//  函数任务：  
//      1.关中断  
//      2.运行hrtimer  
//  调用路径：run_hrtimer_softirq->hrtimer_peek_ahead_timers  
void hrtimer_peek_ahead_timers(void)
{
	unsigned long flags;

	local_irq_save(flags);
	__hrtimer_peek_ahead_timers();
	local_irq_restore(flags);
}

//  高分辨率下的定时器软中断  
//      当增加一个hrtimer到rbtree中后，会raise高分辨率定时器软中断  
//  函数任务：  
//      1.关中断下，运行所有hrtimer  
//  注：  
//      在hrtimers_init中，open_softirq(HRTIMER_SOFTIRQ, run_hrtimer_softirq);  
static void run_hrtimer_softirq(struct softirq_action *h)
{
	//遍历运行所有到期的hrtimer
	hrtimer_peek_ahead_timers();
}

#else /* CONFIG_HIGH_RES_TIMERS */

static inline void __hrtimer_peek_ahead_timers(void) { }

#endif	/* !CONFIG_HIGH_RES_TIMERS */

/*
 * Called from timer softirq every jiffy, expire hrtimers:
 *
 * For HRT its the fall back code to run the softirq in the timer
 * softirq context in case the hrtimer initialization failed or has
 * not been done yet.
 */
//  切换低分辨率动态时钟模式、或高分辨率模式  
//  调用路径：run_timer_softirq->hrtimer_run_pending  
//  函数任务：  
//      1.如果高分辨率定时器框架已经激活，则直接返回  
//      2.切换到高分辨率模式的条件：  
//          2.1 没有开启低分辨率动态时钟  
//          2.2 有高分辨率的clocksource  
//          2.3 clockevent设备支持单触发模式  
//      3.切换到低分辨率动态时钟的条件：  
//          3.1 启动时，没有激活高分辨率率定时框架  
//          3.2 clockevent设备支持单触发模式  
/*
尽管内核配置成支持高精度定时器，但并不是一开始就工作于高精度模式，系统在启动的开始阶段，
还是按照传统的模式在运行：tick_device按HZ频率定期地产生tick事件，这时的hrtimer工作在低
分辨率模式，到期事件在每个tick事件中断中由hrtimer_run_queues函数处理，同时，在低分辨率
定时器（时间轮）的软件中断TIMER_SOFTIRQ中，hrtimer_run_pending会被调用，系统在这个函数
中判断系统的条件是否满足切换到高精度模式，如果条件满足，则会切换至高分辨率模式，另外提
一下，NO_HZ模式也是在该函数中判断并切换。
*/
void hrtimer_run_pending(void)
{
	/*
	因为不管系统是否工作于高精度模式，每个TIMER_SOFTIRQ期间，该函数都会被调用，所以函数一开
	始先用hrtimer_hres_active判断目前高精度模式是否已经激活，如果已经激活，则说明之前的调用
	中已经切换了工作模式，不必再次切换，

	*/
	if (hrtimer_hres_active())
		return;

	/*
	 * This _is_ ugly: We have to check in the softirq context,
	 * whether we can switch to highres and / or nohz mode. The
	 * clocksource switch happens in the timer interrupt with
	 * xtime_lock held. Notification from there only sets the
	 * check bit in the tick_oneshot code, otherwise we might
	 * deadlock vs. xtime_lock.
	 */
	 /*
	 tick_check_oneshot_change函数的参数决定了现在是要切换至低分辨率动态时钟模式，
	 还是高精度定时器模式，我们现在假设系统不支持高精度定时器模式，hrtimer_is_hres_enabled会直接返回false，
	 对应的tick_check_oneshot_change函数的参数则是true，表明需要切换至动态时钟模式
	 */
	if (tick_check_oneshot_change(!hrtimer_is_hres_enabled()))
	////切换到高分辨率模式
		hrtimer_switch_to_hres();
}

/*
 * Called from hardirq context every jiffy
 */
//  运行高分辨率定时器  
//  调用路径：  
//      update_process_times->run_local_timers->hrtimer_run_queues  
//      当没有开启高分辨率模式时，高分辨率定时器由低分辨率时钟软中断调用，此函数在低分辨率中衔接高分辨率  
//  函数任务：  
//      1.检查高分辨率率定时器框架是否已经被激活  
//          1.1 当高分辨率定时器框架被激活时，hrtimer已经通过hrtimer_interrupt被运行  
//      2.获取当前时间  
//      3.遍历时钟基础，如果hrtimer到期，运行其回调函数  
//  注：  
//      当高分辨率率定时器框架为激活时，高分辨率定时器通过时钟中断运行，频率为jiffiy。 

/*
低精度模式  因为系统并不是一开始就会支持高精度模式，而是在系统启动后的某个阶段，等待所有的条件都满足后，
才会切换到高精度模式，当系统还没有切换到高精度模式时，所有的高精度定时器运行在低精度模式下，在每个
jiffie的tick事件中断中进行到期定时器的查询和处理，显然这时候的精度和低分辨率定时器是一样的（HZ级别）。
低精度模式下，每个tick事件中断中，hrtimer_run_queues函数会被调用，由它完成定时器的到期处理。
hrtimer_run_queues首先判断目前高精度模式是否已经启用，如果已经切换到了高精度模式，什么也不做，直接返回：

*/
void hrtimer_run_queues(void)
{
	struct rb_node *node;
	struct hrtimer_cpu_base *cpu_base = &__get_cpu_var(hrtimer_bases);
	struct hrtimer_clock_base *base;
	int index, gettime = 1;

	//如果高分辨率定时器框架已经激活，则hrtimer已经通过hrtimer_interrupt被运行，直接返回
	if (hrtimer_hres_active())
		return;

	/*
	如果hrtimer_hres_active返回false，说明目前处于低精度模式下，则继续处理，
	它用一个for循环遍历各个时间基准系统，查询每个hrtimer_clock_base对应红黑树
	的左下节点，判断它的时间是否到期，如果到期，通过__run_hrtimer函数，对到期
	定时器进行处理，包括：调用定时器的回调函数、从红黑树中移除该定时器、根据回
	调函数的返回值决定是否重新启动该定时器等等：
	*/
	
	//遍历cpu的时钟基础，运行到期的hrtimer 
	for (index = 0; index < HRTIMER_MAX_CLOCK_BASES; index++) {
		base = &cpu_base->clock_base[index];
		//当前时钟基础已经没有active的hrtimer，则遍历下一个

		if (!base->first)
			continue;
		//获取当前时间
		if (gettime) {
			hrtimer_get_softirq_time(cpu_base);
			gettime = 0;
		}

		raw_spin_lock(&cpu_base->lock);
		//获取下一个active的hrtimer

		while ((node = base->first)) {
			struct hrtimer *timer;

			timer = rb_entry(node, struct hrtimer, node);
			 //如果hrtimer到期，则运行其回调函数
			if (base->softirq_time.tv64 <=
					hrtimer_get_expires_tv64(timer))
				break;

			__run_hrtimer(timer, &base->softirq_time);
		}
		raw_spin_unlock(&cpu_base->lock);
	}
}

/*
 * Sleep related functions:
 */
static enum hrtimer_restart hrtimer_wakeup(struct hrtimer *timer)
{
	struct hrtimer_sleeper *t =
		container_of(timer, struct hrtimer_sleeper, timer);
	struct task_struct *task = t->task;

	t->task = NULL;
	if (task)
		wake_up_process(task);

	return HRTIMER_NORESTART;
}

void hrtimer_init_sleeper(struct hrtimer_sleeper *sl, struct task_struct *task)
{
	sl->timer.function = hrtimer_wakeup;
	sl->task = task;
}
EXPORT_SYMBOL_GPL(hrtimer_init_sleeper);

static int __sched do_nanosleep(struct hrtimer_sleeper *t, enum hrtimer_mode mode)
{
	/*
	它首先通过hrtimer_init_sleeper函数，把定时器的回调函数设置为hrtimer_wakeup，
	把当前进程的task_struct结构指针保存在hrtimer_sleeper结构的task字段中
	*/
	hrtimer_init_sleeper(t, current);

	/*
	通过一个do/while循环内：启动定时器，挂起当前进程，等待定时器或其它事件唤醒进程。
	这里的循环体实现比较怪异，它使用hrtimer_active函数间接地判断定时器是否到期，
	如果hrtimer_active返回false，说明定时器已经过期，然后把hrtimer_sleeper结构的task
	字段设置为NULL，从而导致循环体的结束，另一个结束条件是当前进程收到了信号事件，
	所以，当因为是定时器到期而退出时，do_nanosleep返回true，否则返回false，
	上述的hrtimer_nanosleep正是利用了这一特性来决定它的返回值。
	*/
	do {
		set_current_state(TASK_INTERRUPTIBLE);
		hrtimer_start_expires(&t->timer, mode);
		if (!hrtimer_active(&t->timer))
			t->task = NULL;

		if (likely(t->task))
			schedule();

		hrtimer_cancel(&t->timer);
		mode = HRTIMER_MODE_ABS;

	} while (t->task && !signal_pending(current));

	__set_current_state(TASK_RUNNING);

	return t->task == NULL;
}

static int update_rmtp(struct hrtimer *timer, struct timespec __user *rmtp)
{
	struct timespec rmt;
	ktime_t rem;

	rem = hrtimer_expires_remaining(timer);
	if (rem.tv64 <= 0)
		return 0;
	rmt = ktime_to_timespec(rem);

	if (copy_to_user(rmtp, &rmt, sizeof(*rmtp)))
		return -EFAULT;

	return 1;
}

long __sched hrtimer_nanosleep_restart(struct restart_block *restart)
{
	struct hrtimer_sleeper t;
	struct timespec __user  *rmtp;
	int ret = 0;

	hrtimer_init_on_stack(&t.timer, restart->nanosleep.index,
				HRTIMER_MODE_ABS);
	hrtimer_set_expires_tv64(&t.timer, restart->nanosleep.expires);

	if (do_nanosleep(&t, HRTIMER_MODE_ABS))
		goto out;

	rmtp = restart->nanosleep.rmtp;
	if (rmtp) {
		ret = update_rmtp(&t.timer, rmtp);
		if (ret <= 0)
			goto out;
	}

	/* The other values in restart are already filled in */
	ret = -ERESTART_RESTARTBLOCK;
out:
	destroy_hrtimer_on_stack(&t.timer);
	return ret;
}

long hrtimer_nanosleep(struct timespec *rqtp, struct timespec __user *rmtp,
		       const enum hrtimer_mode mode, const clockid_t clockid)
{
	struct restart_block *restart;
	struct hrtimer_sleeper t;
	int ret = 0;
	unsigned long slack;

	slack = current->timer_slack_ns;
	if (rt_task(current))
		slack = 0;

	//hrtimer_nanosleep函数首先在堆栈中创建一个高精度定时器，设置它的到期时间
	hrtimer_init_on_stack(&t.timer, clockid, mode);
	hrtimer_set_expires_range_ns(&t.timer, timespec_to_ktime(*rqtp), slack);
	/* do_nanosleep完成最终的延时工作, 当前进程在挂起相应的延时时间后，退出do_nanosleep函数，
	销毁堆栈中的定时器并返回0值表示执行成功。不过do_nanosleep可能在没有达到所需延时数量时由
	于其它原因退出，如果出现这种情况，hrtimer_nanosleep的最后部分把剩余的延时时间记入进程的
	restart_block中，并返回ERESTART_RESTARTBLOCK错误代码，系统或者用户空间可以根据此返回值决定
	是否重新调用nanosleep以便把剩余的延时继续执行完成。
	*/
	if (do_nanosleep(&t, mode))
		goto out;

	/* Absolute timers do not update the rmtp value and restart: */
	if (mode == HRTIMER_MODE_ABS) {
		ret = -ERESTARTNOHAND;
		goto out;
	}

	if (rmtp) {
		ret = update_rmtp(&t.timer, rmtp);
		if (ret <= 0)
			goto out;
	}

	restart = &current_thread_info()->restart_block;
	restart->fn = hrtimer_nanosleep_restart;
	restart->nanosleep.index = t.timer.base->index;
	restart->nanosleep.rmtp = rmtp;
	restart->nanosleep.expires = hrtimer_get_expires_tv64(&t.timer);

	ret = -ERESTART_RESTARTBLOCK;
out:
	destroy_hrtimer_on_stack(&t.timer);
	return ret;
}

SYSCALL_DEFINE2(nanosleep, struct timespec __user *, rqtp,
		struct timespec __user *, rmtp)
{
	struct timespec tu;

	if (copy_from_user(&tu, rqtp, sizeof(tu)))
		return -EFAULT;

	if (!timespec_valid(&tu))
		return -EINVAL;

	return hrtimer_nanosleep(&tu, rmtp, HRTIMER_MODE_REL, CLOCK_MONOTONIC);
}

/*
 * Functions related to boot-time initialization:
 */
 //  初始化cpu的时钟基础  
//  函数任务：  
//      1.初始化REALTIME、MONOTOMIC两个时钟基础  
//      2.更新时钟基础到期时间为KTIME_MAX，即没有hrtimer会到期  
//      3.更新高分辨率定时器未激活状态  
//  调用路径：init_hrtimers_cpu->init_hrtimers_cpu 
static void __cpuinit init_hrtimers_cpu(int cpu)
{
	////per cpu时钟基础设备，用于维护该cpu的hrtimer 
	struct hrtimer_cpu_base *cpu_base = &per_cpu(hrtimer_bases, cpu);
	int i;

	raw_spin_lock_init(&cpu_base->lock);

	//初始化cpu时钟基础
	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++)
		cpu_base->clock_base[i].cpu_base = cpu_base;

	hrtimer_init_hres(cpu_base);
}

#ifdef CONFIG_HOTPLUG_CPU

static void migrate_hrtimer_list(struct hrtimer_clock_base *old_base,
				struct hrtimer_clock_base *new_base)
{
	struct hrtimer *timer;
	struct rb_node *node;

	while ((node = rb_first(&old_base->active))) {
		timer = rb_entry(node, struct hrtimer, node);
		BUG_ON(hrtimer_callback_running(timer));
		debug_deactivate(timer);

		/*
		 * Mark it as STATE_MIGRATE not INACTIVE otherwise the
		 * timer could be seen as !active and just vanish away
		 * under us on another CPU
		 */
		__remove_hrtimer(timer, old_base, HRTIMER_STATE_MIGRATE, 0);
		timer->base = new_base;
		/*
		 * Enqueue the timers on the new cpu. This does not
		 * reprogram the event device in case the timer
		 * expires before the earliest on this CPU, but we run
		 * hrtimer_interrupt after we migrated everything to
		 * sort out already expired timers and reprogram the
		 * event device.
		 */
		enqueue_hrtimer(timer, new_base);

		/* Clear the migration state bit */
		timer->state &= ~HRTIMER_STATE_MIGRATE;
	}
}

static void migrate_hrtimers(int scpu)
{
	struct hrtimer_cpu_base *old_base, *new_base;
	int i;

	BUG_ON(cpu_online(scpu));
	tick_cancel_sched_timer(scpu);

	local_irq_disable();
	old_base = &per_cpu(hrtimer_bases, scpu);
	new_base = &__get_cpu_var(hrtimer_bases);
	/*
	 * The caller is globally serialized and nobody else
	 * takes two locks at once, deadlock is not possible.
	 */
	raw_spin_lock(&new_base->lock);
	raw_spin_lock_nested(&old_base->lock, SINGLE_DEPTH_NESTING);

	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++) {
		migrate_hrtimer_list(&old_base->clock_base[i],
				     &new_base->clock_base[i]);
	}

	raw_spin_unlock(&old_base->lock);
	raw_spin_unlock(&new_base->lock);

	/* Check, if we got expired work to do */
	__hrtimer_peek_ahead_timers();
	local_irq_enable();
}

#endif /* CONFIG_HOTPLUG_CPU */

//  处理cpu状态变化  
//  函数任务：  
//      1.cpu up，初始化cpu的时钟基础  
//      2.针对热插拔cpu  
//          2.1 cpu dying、frozen，选择cpu，接管do_timer  
//          2.2 cpu dead、frozen  
//              2.2.1 通知clockevent设备管理，cpu dead  
//              2.2.2 迁移dead cpu上的hrtimer到本cpu  

static int __cpuinit hrtimer_cpu_notify(struct notifier_block *self,
					unsigned long action, void *hcpu)
{
	int scpu = (long)hcpu;

	switch (action) {

	//cpu up，初始化此cpu的时钟基础 
	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:
		init_hrtimers_cpu(scpu);
		break;
 	//下列情况只针对热插拔cpu  
#ifdef CONFIG_HOTPLUG_CPU
	case CPU_DYING:
	case CPU_DYING_FROZEN:
	//选择cpu接管do_timer
		clockevents_notify(CLOCK_EVT_NOTIFY_CPU_DYING, &scpu);
		break;
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
	{
		//通知clockevent设备管理，cpu dead
		clockevents_notify(CLOCK_EVT_NOTIFY_CPU_DEAD, &scpu);
		//迁移cpu上的定时任务到本cpu 
		migrate_hrtimers(scpu);
		break;
	}
#endif

	default:
		break;
	}

	return NOTIFY_OK;
}

//  cpu状态监听控制块 
static struct notifier_block __cpuinitdata hrtimers_nb = {
	.notifier_call = hrtimer_cpu_notify,
};

//  高分辨率定时器框架初始化  
//  调用路径：start_kernel->hrtimers_init  
//  函数任务：  
//      1.创建cpu时钟基础  
//      2.注册监听cpu状态变化  
//      3.注册高分辨率模式下的定时器软中断  
//  注：  
//      1.高分辨率定时器框架的通用部分总是编译进内核  
//      2.高分辨率定时器框架初始为未激活状态，由低分辨率定时器软中断中切换到高分辨率 

void __init hrtimers_init(void)
{
	//通知clockevent设备管理，创建cpu时钟基础 
	hrtimer_cpu_notify(&hrtimers_nb, (unsigned long)CPU_UP_PREPARE,
			  (void *)(long)smp_processor_id());
	//注册监听cpu 状态信息  
	register_cpu_notifier(&hrtimers_nb);
	 //高分辨率定时功能通过预处理编译进内核  
#ifdef CONFIG_HIGH_RES_TIMERS
	////注册高分辨率模式下的定时器软中断
	open_softirq(HRTIMER_SOFTIRQ, run_hrtimer_softirq);
#endif
}

/**
 * schedule_hrtimeout_range_clock - sleep until timeout
 * @expires:	timeout value (ktime_t)
 * @delta:	slack in expires timeout (ktime_t)
 * @mode:	timer mode, HRTIMER_MODE_ABS or HRTIMER_MODE_REL
 * @clock:	timer clock, CLOCK_MONOTONIC or CLOCK_REALTIME
 */
 //使得当前进程休眠指定的时间范围，可以自行指定计时系统；
int __sched
schedule_hrtimeout_range_clock(ktime_t *expires, unsigned long delta,
			       const enum hrtimer_mode mode, int clock)
{
	struct hrtimer_sleeper t;

	/*
	 * Optimize when a zero timeout value is given. It does not
	 * matter whether this is an absolute or a relative time.
	 */
	if (expires && !expires->tv64) {
		__set_current_state(TASK_RUNNING);
		return 0;
	}

	/*
	 * A NULL parameter means "inifinte"
	 */
	if (!expires) {
		schedule();
		__set_current_state(TASK_RUNNING);
		return -EINTR;
	}

	hrtimer_init_on_stack(&t.timer, clock, mode);
	hrtimer_set_expires_range_ns(&t.timer, *expires, delta);

	hrtimer_init_sleeper(&t, current);

	hrtimer_start_expires(&t.timer, mode);
	if (!hrtimer_active(&t.timer))
		t.task = NULL;

	if (likely(t.task))
		schedule();

	hrtimer_cancel(&t.timer);
	destroy_hrtimer_on_stack(&t.timer);

	__set_current_state(TASK_RUNNING);

	return !t.task ? 0 : -EINTR;
}

/**
 * schedule_hrtimeout_range - sleep until timeout
 * @expires:	timeout value (ktime_t)
 * @delta:	slack in expires timeout (ktime_t)
 * @mode:	timer mode, HRTIMER_MODE_ABS or HRTIMER_MODE_REL
 *
 * Make the current task sleep until the given expiry time has
 * elapsed. The routine will return immediately unless
 * the current task state has been set (see set_current_state()).
 *
 * The @delta argument gives the kernel the freedom to schedule the
 * actual wakeup to a time that is both power and performance friendly.
 * The kernel give the normal best effort behavior for "@expires+@delta",
 * but may decide to fire the timer earlier, but no earlier than @expires.
 *
 * You can set the task state as follows -
 *
 * %TASK_UNINTERRUPTIBLE - at least @timeout time is guaranteed to
 * pass before the routine returns.
 *
 * %TASK_INTERRUPTIBLE - the routine may return early if a signal is
 * delivered to the current task.
 *
 * The current task state is guaranteed to be TASK_RUNNING when this
 * routine returns.
 *
 * Returns 0 when the timer has expired otherwise -EINTR
 */
 //使得当前进程休眠指定的时间范围，使用CLOCK_MONOTONIC计时系统
int __sched schedule_hrtimeout_range(ktime_t *expires, unsigned long delta,
				     const enum hrtimer_mode mode)
{
	return schedule_hrtimeout_range_clock(expires, delta, mode,
					      CLOCK_MONOTONIC);
}
EXPORT_SYMBOL_GPL(schedule_hrtimeout_range);

/**
 * schedule_hrtimeout - sleep until timeout
 * @expires:	timeout value (ktime_t)
 * @mode:	timer mode, HRTIMER_MODE_ABS or HRTIMER_MODE_REL
 *
 * Make the current task sleep until the given expiry time has
 * elapsed. The routine will return immediately unless
 * the current task state has been set (see set_current_state()).
 *
 * You can set the task state as follows -
 *
 * %TASK_UNINTERRUPTIBLE - at least @timeout time is guaranteed to
 * pass before the routine returns.
 *
 * %TASK_INTERRUPTIBLE - the routine may return early if a signal is
 * delivered to the current task.
 *
 * The current task state is guaranteed to be TASK_RUNNING when this
 * routine returns.
 *
 * Returns 0 when the timer has expired otherwise -EINTR
 */
 //使得当前进程休眠指定的时间，使用CLOCK_MONOTONIC计时系统
int __sched schedule_hrtimeout(ktime_t *expires,
			       const enum hrtimer_mode mode)
{
	return schedule_hrtimeout_range(expires, 0, mode);
}
EXPORT_SYMBOL_GPL(schedule_hrtimeout);
