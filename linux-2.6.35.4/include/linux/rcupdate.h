/*
 * Read-Copy Update mechanism for mutual exclusion
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright IBM Corporation, 2001
 *
 * Author: Dipankar Sarma <dipankar@in.ibm.com>
 *
 * Based on the original work by Paul McKenney <paulmck@us.ibm.com>
 * and inputs from Rusty Russell, Andrea Arcangeli and Andi Kleen.
 * Papers:
 * http://www.rdrop.com/users/paulmck/paper/rclockpdcsproof.pdf
 * http://lse.sourceforge.net/locking/rclock_OLS.2001.05.01c.sc.pdf (OLS2001)
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *		http://lse.sourceforge.net/locking/rcupdate.html
 *
 */

#ifndef __LINUX_RCUPDATE_H
#define __LINUX_RCUPDATE_H

#include <linux/cache.h>
#include <linux/spinlock.h>
#include <linux/threads.h>
#include <linux/cpumask.h>
#include <linux/seqlock.h>
#include <linux/lockdep.h>
#include <linux/completion.h>

#ifdef CONFIG_RCU_TORTURE_TEST
extern int rcutorture_runnable; /* for sysctl */
#endif /* #ifdef CONFIG_RCU_TORTURE_TEST */

/*
	http://blog.sina.com.cn/s/blog_6d7fa49b01014q9s.html
	http://blog.csdn.net/nevil/article/details/7718375
	
	RCU 是READCOPY UPDATE的简写，设计思想的来源是，对读写锁进行优化，减少写者之间的同步，
	即如果同时有几个写者进行竞争，那么写者就对资源进行拷贝，从而允许多个写者对资源进行修改，
	最后由系统决定什么时候更新。


	首先我们从概念上理解下什么叫RCU，其中读(Read)：读者不需要获得任何锁就可访问RCU保护的临界区；
	拷贝(Copy)：写者在访问临界区时，写者“自己”将先拷贝一个临界区副本，然后对副本进行修改；
	更新(Update)：RCU机制将在在适当时机使用一个回调函数把指向原来临界区的指针重新指向新的被修改的临界区，
	锁机制中的垃圾收集器负责回调函数的调用。总结即是读-拷贝-更新
	
	这个时机是：所有引用该共享临界区的CPU都退出对临界区的操作。即没有CPU再去操作这段临界区后，
	这段临界区即可回收了，此时回调函数即被调用。

	RCU是一种机制，它允许读写并发执行，对于读者来说，读者间没有任何同步开销，因为可随时读取临界区，
	和其他读者没有相互影响；但对于不同的写者来说，它们之间如果存在同步开销，则写者间的同步开销则取
	决于写者间的采用同步机制，和RCU并没有直接的关系。

	RCU允许多个读者同时访问被保护的数据，也允许多个读者在有写者时访问被保护的数据（但是注意：
	是否可以有多个写者并行访问取决于写者之间使用的同步机制）。
*/

/**
 * struct rcu_head - callback structure for use with RCU
 * @next: next update requests in a list
 * @func: actual update function to call after the grace period.
 */
struct rcu_head {
	struct rcu_head *next;
	void (*func)(struct rcu_head *head);//回调函数
};

/* Exported common interfaces */
extern void rcu_barrier(void);
extern void rcu_barrier_bh(void);
extern void rcu_barrier_sched(void);
extern void synchronize_sched_expedited(void);
extern int sched_expedited_torture_stats(char *page);

/* Internal to kernel */
extern void rcu_init(void);

#if defined(CONFIG_TREE_RCU) || defined(CONFIG_TREE_PREEMPT_RCU)
#include <linux/rcutree.h>
#elif defined(CONFIG_TINY_RCU)
#include <linux/rcutiny.h>
#else
#error "Unknown RCU implementation specified to kernel configuration"
#endif

#define RCU_HEAD_INIT	{ .next = NULL, .func = NULL }
#define RCU_HEAD(head) struct rcu_head head = RCU_HEAD_INIT
#define INIT_RCU_HEAD(ptr) do { \
       (ptr)->next = NULL; (ptr)->func = NULL; \
} while (0)

static inline void init_rcu_head_on_stack(struct rcu_head *head)
{
}

static inline void destroy_rcu_head_on_stack(struct rcu_head *head)
{
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC

extern struct lockdep_map rcu_lock_map;
# define rcu_read_acquire() \
		lock_acquire(&rcu_lock_map, 0, 0, 2, 1, NULL, _THIS_IP_)
# define rcu_read_release()	lock_release(&rcu_lock_map, 1, _THIS_IP_)

extern struct lockdep_map rcu_bh_lock_map;
# define rcu_read_acquire_bh() \
		lock_acquire(&rcu_bh_lock_map, 0, 0, 2, 1, NULL, _THIS_IP_)
# define rcu_read_release_bh()	lock_release(&rcu_bh_lock_map, 1, _THIS_IP_)

extern struct lockdep_map rcu_sched_lock_map;
# define rcu_read_acquire_sched() \
		lock_acquire(&rcu_sched_lock_map, 0, 0, 2, 1, NULL, _THIS_IP_)
# define rcu_read_release_sched() \
		lock_release(&rcu_sched_lock_map, 1, _THIS_IP_)

extern int debug_lockdep_rcu_enabled(void);

/**
 * rcu_read_lock_held - might we be in RCU read-side critical section?
 *
 * If CONFIG_DEBUG_LOCK_ALLOC is selected, returns nonzero iff in an RCU
 * read-side critical section.  In absence of CONFIG_DEBUG_LOCK_ALLOC,
 * this assumes we are in an RCU read-side critical section unless it can
 * prove otherwise.
 *
 * Check debug_lockdep_rcu_enabled() to prevent false positives during boot
 * and while lockdep is disabled.
 */
static inline int rcu_read_lock_held(void)
{
	if (!debug_lockdep_rcu_enabled())
		return 1;
	return lock_is_held(&rcu_lock_map);
}

/*
 * rcu_read_lock_bh_held() is defined out of line to avoid #include-file
 * hell.
 */
extern int rcu_read_lock_bh_held(void);

/**
 * rcu_read_lock_sched_held - might we be in RCU-sched read-side critical section?
 *
 * If CONFIG_DEBUG_LOCK_ALLOC is selected, returns nonzero iff in an
 * RCU-sched read-side critical section.  In absence of
 * CONFIG_DEBUG_LOCK_ALLOC, this assumes we are in an RCU-sched read-side
 * critical section unless it can prove otherwise.  Note that disabling
 * of preemption (including disabling irqs) counts as an RCU-sched
 * read-side critical section.
 *
 * Check debug_lockdep_rcu_enabled() to prevent false positives during boot
 * and while lockdep is disabled.
 */
#ifdef CONFIG_PREEMPT
static inline int rcu_read_lock_sched_held(void)
{
	int lockdep_opinion = 0;

	if (!debug_lockdep_rcu_enabled())
		return 1;
	if (debug_locks)
		lockdep_opinion = lock_is_held(&rcu_sched_lock_map);
	return lockdep_opinion || preempt_count() != 0 || irqs_disabled();
}
#else /* #ifdef CONFIG_PREEMPT */
static inline int rcu_read_lock_sched_held(void)
{
	return 1;
}
#endif /* #else #ifdef CONFIG_PREEMPT */

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

# define rcu_read_acquire()		do { } while (0)
# define rcu_read_release()		do { } while (0)
# define rcu_read_acquire_bh()		do { } while (0)
# define rcu_read_release_bh()		do { } while (0)
# define rcu_read_acquire_sched()	do { } while (0)
# define rcu_read_release_sched()	do { } while (0)

static inline int rcu_read_lock_held(void)
{
	return 1;
}

static inline int rcu_read_lock_bh_held(void)
{
	return 1;
}

#ifdef CONFIG_PREEMPT
static inline int rcu_read_lock_sched_held(void)
{
	return preempt_count() != 0 || irqs_disabled();
}
#else /* #ifdef CONFIG_PREEMPT */
static inline int rcu_read_lock_sched_held(void)
{
	return 1;
}
#endif /* #else #ifdef CONFIG_PREEMPT */

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

#ifdef CONFIG_PROVE_RCU

extern int rcu_my_thread_group_empty(void);

#define __do_rcu_dereference_check(c)					\
	do {								\
		static bool __warned;					\
		if (debug_lockdep_rcu_enabled() && !__warned && !(c)) {	\
			__warned = true;				\
			lockdep_rcu_dereference(__FILE__, __LINE__);	\
		}							\
	} while (0)

/**
 * rcu_dereference_check - rcu_dereference with debug checking
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * Do an rcu_dereference(), but check that the conditions under which the
 * dereference will take place are correct.  Typically the conditions indicate
 * the various locking conditions that should be held at that point.  The check
 * should return true if the conditions are satisfied.
 *
 * For example:
 *
 *	bar = rcu_dereference_check(foo->bar, rcu_read_lock_held() ||
 *					      lockdep_is_held(&foo->lock));
 *
 * could be used to indicate to lockdep that foo->bar may only be dereferenced
 * if either the RCU read lock is held, or that the lock required to replace
 * the bar struct at foo->bar is held.
 *
 * Note that the list of conditions may also include indications of when a lock
 * need not be held, for example during initialisation or destruction of the
 * target struct:
 *
 *	bar = rcu_dereference_check(foo->bar, rcu_read_lock_held() ||
 *					      lockdep_is_held(&foo->lock) ||
 *					      atomic_read(&foo->usage) == 0);
 */
#define rcu_dereference_check(p, c) \
	({ \
		__do_rcu_dereference_check(c); \
		rcu_dereference_raw(p); \
	})

/**
 * rcu_dereference_protected - fetch RCU pointer when updates prevented
 *
 * Return the value of the specified RCU-protected pointer, but omit
 * both the smp_read_barrier_depends() and the ACCESS_ONCE().  This
 * is useful in cases where update-side locks prevent the value of the
 * pointer from changing.  Please note that this primitive does -not-
 * prevent the compiler from repeating this reference or combining it
 * with other references, so it should not be used without protection
 * of appropriate locks.
 */
#define rcu_dereference_protected(p, c) \
	({ \
		__do_rcu_dereference_check(c); \
		(p); \
	})

#else /* #ifdef CONFIG_PROVE_RCU */

#define rcu_dereference_check(p, c)	rcu_dereference_raw(p)
#define rcu_dereference_protected(p, c) (p)

#endif /* #else #ifdef CONFIG_PROVE_RCU */

/**
 * rcu_access_pointer - fetch RCU pointer with no dereferencing
 *
 * Return the value of the specified RCU-protected pointer, but omit the
 * smp_read_barrier_depends() and keep the ACCESS_ONCE().  This is useful
 * when the value of this pointer is accessed, but the pointer is not
 * dereferenced, for example, when testing an RCU-protected pointer against
 * NULL.  This may also be used in cases where update-side locks prevent
 * the value of the pointer from changing, but rcu_dereference_protected()
 * is a lighter-weight primitive for this use case.
 */
#define rcu_access_pointer(p)	ACCESS_ONCE(p)

/**
 * rcu_read_lock - mark the beginning of an RCU read-side critical section.
 *
 * When synchronize_rcu() is invoked on one CPU while other CPUs
 * are within RCU read-side critical sections, then the
 * synchronize_rcu() is guaranteed to block until after all the other
 * CPUs exit their critical sections.  Similarly, if call_rcu() is invoked
 * on one CPU while other CPUs are within RCU read-side critical
 * sections, invocation of the corresponding RCU callback is deferred
 * until after the all the other CPUs exit their critical sections.
 *
 * Note, however, that RCU callbacks are permitted to run concurrently
 * with RCU read-side critical sections.  One way that this can happen
 * is via the following sequence of events: (1) CPU 0 enters an RCU
 * read-side critical section, (2) CPU 1 invokes call_rcu() to register
 * an RCU callback, (3) CPU 0 exits the RCU read-side critical section,
 * (4) CPU 2 enters a RCU read-side critical section, (5) the RCU
 * callback is invoked.  This is legal, because the RCU read-side critical
 * section that was running concurrently with the call_rcu() (and which
 * therefore might be referencing something that the corresponding RCU
 * callback would free up) has completed before the corresponding
 * RCU callback is invoked.
 *
 * RCU read-side critical sections may be nested.  Any deferred actions
 * will be deferred until the outermost RCU read-side critical section
 * completes.
 *
 * It is illegal to block while in an RCU read-side critical section.
 */
 //rcu_read_lock()和rcu_read_unlock()用来保持一个读者的RCU临界区.在该临界区内不允许发生上下文切换。
 //为什么不能发生切换呢？因为内核要根据“是否发生过切换”来判断读者是否已结束读操作
 
 //读者函数
static inline void rcu_read_lock(void)
{
	__rcu_read_lock();
	//以下两个函数是选择编译函数，可以忽略不看
	__acquire(RCU);
	rcu_read_acquire();
}

/*
 * So where is rcu_write_lock()?  It does not exist, as there is no
 * way for writers to lock out RCU readers.  This is a feature, not
 * a bug -- this property is what provides RCU's performance benefits.
 * Of course, writers must coordinate with each other.  The normal
 * spinlock primitives work well for this, but any other technique may be
 * used as well.  RCU does not care how the writers keep out of each
 * others' way, as long as they do so.
 */

/**
 * rcu_read_unlock - marks the end of an RCU read-side critical section.
 *
 * See rcu_read_lock() for more information.
 */
 //读解锁
static inline void rcu_read_unlock(void)
{
	rcu_read_release();
	//以下两个函数是选择编译函数，可以忽略不看
	__release(RCU);
	__rcu_read_unlock();
}

/**
 * rcu_read_lock_bh - mark the beginning of a softirq-only RCU critical section
 *
 * This is equivalent of rcu_read_lock(), but to be used when updates
 * are being done using call_rcu_bh(). Since call_rcu_bh() callbacks
 * consider completion of a softirq handler to be a quiescent state,
 * a process in RCU read-side critical section must be protected by
 * disabling softirqs. Read-side critical sections in interrupt context
 * can use just rcu_read_lock().
 *
 */
 //禁止中断下半部
static inline void rcu_read_lock_bh(void)
{
	__rcu_read_lock_bh();
	__acquire(RCU_BH);
	rcu_read_acquire_bh();
}

/*
 * rcu_read_unlock_bh - marks the end of a softirq-only RCU critical section
 *
 * See rcu_read_lock_bh() for more information.
 */
 //使能中断下半部
static inline void rcu_read_unlock_bh(void)
{
	rcu_read_release_bh();
	__release(RCU_BH);
	__rcu_read_unlock_bh();
}

/**
 * rcu_read_lock_sched - mark the beginning of a RCU-classic critical section
 *
 * Should be used with either
 * - synchronize_sched()
 * or
 * - call_rcu_sched() and rcu_barrier_sched()
 * on the write-side to insure proper synchronization.
 */
static inline void rcu_read_lock_sched(void)
{
	preempt_disable();
	__acquire(RCU_SCHED);
	rcu_read_acquire_sched();
}

/* Used by lockdep and tracing: cannot be traced, cannot call lockdep. */
static inline notrace void rcu_read_lock_sched_notrace(void)
{
	preempt_disable_notrace();
	__acquire(RCU_SCHED);
}

/*
 * rcu_read_unlock_sched - marks the end of a RCU-classic critical section
 *
 * See rcu_read_lock_sched for more information.
 */
static inline void rcu_read_unlock_sched(void)
{
	rcu_read_release_sched();
	__release(RCU_SCHED);
	preempt_enable();
}

/* Used by lockdep and tracing: cannot be traced, cannot call lockdep. */
static inline notrace void rcu_read_unlock_sched_notrace(void)
{
	__release(RCU_SCHED);
	preempt_enable_notrace();
}


/**
 * rcu_dereference_raw - fetch an RCU-protected pointer
 *
 * The caller must be within some flavor of RCU read-side critical
 * section, or must be otherwise preventing the pointer from changing,
 * for example, by holding an appropriate lock.  This pointer may later
 * be safely dereferenced.  It is the caller's responsibility to have
 * done the right thing, as this primitive does no checking of any kind.
 *
 * Inserts memory barriers on architectures that require them
 * (currently only the Alpha), and, more importantly, documents
 * exactly which pointers are protected by RCU.
 */
#define rcu_dereference_raw(p)	({ \
				typeof(p) _________p1 = ACCESS_ONCE(p); \
				smp_read_barrier_depends(); \
				(_________p1); \
				})

/**
 * rcu_dereference - fetch an RCU-protected pointer, checking for RCU
 *
 * Makes rcu_dereference_check() do the dirty work.
 */
 //读者调用它来获得一个被RCU保护的指针
// 为了cache一致性，插上了内存屏障。可以让其它的读者/写者可以看到保护指针的最新值.
#define rcu_dereference(p) \
	rcu_dereference_check(p, rcu_read_lock_held())

/**
 * rcu_dereference_bh - fetch an RCU-protected pointer, checking for RCU-bh
 *
 * Makes rcu_dereference_check() do the dirty work.
 */
#define rcu_dereference_bh(p) \
		rcu_dereference_check(p, rcu_read_lock_bh_held())

/**
 * rcu_dereference_sched - fetch RCU-protected pointer, checking for RCU-sched
 *
 * Makes rcu_dereference_check() do the dirty work.
 */
#define rcu_dereference_sched(p) \
		rcu_dereference_check(p, rcu_read_lock_sched_held())

/**
 * rcu_assign_pointer - assign (publicize) a pointer to a newly
 * initialized structure that will be dereferenced by RCU read-side
 * critical sections.  Returns the value assigned.
 *
 * Inserts memory barriers on architectures that require them
 * (pretty much all of them other than x86), and also prevents
 * the compiler from reordering the code that initializes the
 * structure after the pointer assignment.  More importantly, this
 * call documents which pointers will be dereferenced by RCU read-side
 * code.
 */

//写者使用该函数来为被RCU保护的指针分配一个新的值
//强制编译器和CPU在给v的各个域赋值之后再把指针赋值给p
#define rcu_assign_pointer(p, v) \
	({ \
		if (!__builtin_constant_p(v) || \
		    ((v) != NULL)) \
			smp_wmb(); \
		(p) = (v); \
	})

/* Infrastructure to implement the synchronize_() primitives. */

//rcu核心数据结构，挂起写者，等待读者都退出后释放老的数据
struct rcu_synchronize {
	struct rcu_head head;
	struct completion completion;
};

extern void wakeme_after_rcu(struct rcu_head  *head);

/**
 * call_rcu - Queue an RCU callback for invocation after a grace period.
 * @head: structure to be used for queueing the RCU updates.
 * @func: actual update function to be invoked after the grace period
 *
 * The update function will be invoked some time after a full grace
 * period elapses, in other words after all currently executing RCU
 * read-side critical sections have completed.  RCU read-side critical
 * sections are delimited by rcu_read_lock() and rcu_read_unlock(),
 * and may be nested.
 */
extern void /(struct rcu_head *head,
			      void (*func)(struct rcu_head *head));

/**
 * call_rcu_bh - Queue an RCU for invocation after a quicker grace period.
 * @head: structure to be used for queueing the RCU updates.
 * @func: actual update function to be invoked after the grace period
 *
 * The update function will be invoked some time after a full grace
 * period elapses, in other words after all currently executing RCU
 * read-side critical sections have completed. call_rcu_bh() assumes
 * that the read-side critical sections end on completion of a softirq
 * handler. This means that read-side critical sections in process
 * context must not be interrupted by softirqs. This interface is to be
 * used when most of the read-side critical sections are in softirq context.
 * RCU read-side critical sections are delimited by :
 *  - rcu_read_lock() and  rcu_read_unlock(), if in interrupt context.
 *  OR
 *  - rcu_read_lock_bh() and rcu_read_unlock_bh(), if in process context.
 *  These may be nested.
 */
extern void call_rcu_bh(struct rcu_head *head,
			void (*func)(struct rcu_head *head));

#endif /* __LINUX_RCUPDATE_H */
