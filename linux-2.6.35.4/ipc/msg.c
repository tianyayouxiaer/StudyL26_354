/*
 * linux/ipc/msg.c
 * Copyright (C) 1992 Krishna Balasubramanian
 *
 * Removed all the remaining kerneld mess
 * Catch the -EFAULT stuff properly
 * Use GFP_KERNEL for messages as in 1.2
 * Fixed up the unchecked user space derefs
 * Copyright (C) 1998 Alan Cox & Andi Kleen
 *
 * /proc/sysvipc/msg support (c) 1999 Dragos Acostachioaie <dragos@iname.com>
 *
 * mostly rewritten, threaded and wake-one semantics added
 * MSGMAX limit removed, sysctl's added
 * (c) 1999 Manfred Spraul <manfred@colorfullife.com>
 *
 * support for audit of ipc object properties and permission changes
 * Dustin Kirkland <dustin.kirkland@us.ibm.com>
 *
 * namespaces support
 * OpenVZ, SWsoft Inc.
 * Pavel Emelianov <xemul@openvz.org>
 */

#include <linux/capability.h>
#include <linux/msg.h>
#include <linux/spinlock.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/list.h>
#include <linux/security.h>
#include <linux/sched.h>
#include <linux/syscalls.h>
#include <linux/audit.h>
#include <linux/seq_file.h>
#include <linux/rwsem.h>
#include <linux/nsproxy.h>
#include <linux/ipc_namespace.h>

#include <asm/current.h>
#include <asm/uaccess.h>
#include "util.h"


/*
消息队列就是一个消息的链表。具有权限的一个或者多个进程进程可对消息队列进行读写。

System V的消息队列实现是在内核内存中建立消息队列的结构缓存区，通过自定义的消息队列ID，
在全局变量static struct ipc_ids msg_ids中定位找到消息队列的结构缓存区，并最终找到消息。
全局数据结构struct ipc_ids msg_ids可以访问到每个消息队列头的第一个成员：struct kern_ipc_perm；
而每个struct kern_ipc_perm能够与具体的消息队列对应起来，是因为在该结构中，有一个key_t类型成员key，
而key则惟一确定一个消息队列。 

*/

/*
 * one msg_receiver structure for each sleeping receiver:
 */
 //在队列上睡眠的receiver数据结构
 //每个正在睡眠的接收者用一个msg_receiver结构描述
struct msg_receiver {
	struct list_head	r_list;
	struct task_struct	*r_tsk;//进行读操作的进程

	int			r_mode;//读的方式
	long			r_msgtype;//读的消息类型
	long			r_maxsize;//读消息的最大尺寸

	struct msg_msg		*volatile r_msg;//最后将用于存储取得的消息
};

/* one msg_sender for each sleeping sender */
//每个正在睡眠的发送者用一个msg_sender结构描述
struct msg_sender {
	struct list_head	list;
	struct task_struct	*tsk; //发送消息的进程
};

#define SEARCH_ANY		1
#define SEARCH_EQUAL		2
#define SEARCH_NOTEQUAL		3
#define SEARCH_LESSEQUAL	4

//Task_struct -> nsproxy -> ipc_ns-> ids[3]
//#define IPC_SEM_IDS    0
//#define IPC_MSG_IDS    1
//#define IPC_SHM_IDS    2
#define msg_ids(ns)	((ns)->ids[IPC_MSG_IDS])

#define msg_unlock(msq)		ipc_unlock(&(msq)->q_perm)

static void freeque(struct ipc_namespace *, struct kern_ipc_perm *);
static int newque(struct ipc_namespace *, struct ipc_params *);
#ifdef CONFIG_PROC_FS
static int sysvipc_msg_proc_show(struct seq_file *s, void *it);
#endif

/*
 * Scale msgmni with the available lowmem size: the memory dedicated to msg
 * queues should occupy at most 1/MSG_MEM_SCALE of lowmem.
 * Also take into account the number of nsproxies created so far.
 * This should be done staying within the (MSGMNI , IPCMNI/nr_ipc_ns) range.
 */
void recompute_msgmni(struct ipc_namespace *ns)
{
	struct sysinfo i;
	unsigned long allowed;
	int nb_ns;

	si_meminfo(&i);
	allowed = (((i.totalram - i.totalhigh) / MSG_MEM_SCALE) * i.mem_unit)
		/ MSGMNB;
	nb_ns = atomic_read(&nr_ipc_ns);
	allowed /= nb_ns;

	if (allowed < MSGMNI) {
		ns->msg_ctlmni = MSGMNI;
		return;
	}

	if (allowed > IPCMNI / nb_ns) {
		ns->msg_ctlmni = IPCMNI / nb_ns;
		return;
	}

	ns->msg_ctlmni = allowed;
}

void msg_init_ns(struct ipc_namespace *ns)
{
	ns->msg_ctlmax = MSGMAX;//默认8KB
	ns->msg_ctlmnb = MSGMNB;//默认16348

	//根据系统内存资源计算ns->msg_ctlmni
	recompute_msgmni(ns);

	atomic_set(&ns->msg_bytes, 0);
	atomic_set(&ns->msg_hdrs, 0);
	ipc_init_ids(&ns->ids[IPC_MSG_IDS]);
}

#ifdef CONFIG_IPC_NS
void msg_exit_ns(struct ipc_namespace *ns)
{
	free_ipcs(ns, &msg_ids(ns), freeque);
	idr_destroy(&ns->ids[IPC_MSG_IDS].ipcs_idr);
}
#endif

void __init msg_init(void)
{
	msg_init_ns(&init_ipc_ns);

	printk(KERN_INFO "msgmni has been set to %d\n",
		init_ipc_ns.msg_ctlmni);

	ipc_init_proc_interface("sysvipc/msg",
				"       key      msqid perms      cbytes       qnum lspid lrpid   uid   gid  cuid  cgid      stime      rtime      ctime\n",
				IPC_MSG_IDS, sysvipc_msg_proc_show);
}

/*
 * msg_lock_(check_) routines are called in the paths where the rw_mutex
 * is not held.
 */
static inline struct msg_queue *msg_lock(struct ipc_namespace *ns, int id)
{
	struct kern_ipc_perm *ipcp = ipc_lock(&msg_ids(ns), id);

	if (IS_ERR(ipcp))
		return (struct msg_queue *)ipcp;

	return container_of(ipcp, struct msg_queue, q_perm);
}

static inline struct msg_queue *msg_lock_check(struct ipc_namespace *ns,
						int id)
{
	struct kern_ipc_perm *ipcp = ipc_lock_check(&msg_ids(ns), id);

	if (IS_ERR(ipcp))
		return (struct msg_queue *)ipcp;

	return container_of(ipcp, struct msg_queue, q_perm);
}

static inline void msg_rmid(struct ipc_namespace *ns, struct msg_queue *s)
{
	ipc_rmid(&msg_ids(ns), &s->q_perm);
}

/**
 * newque - Create a new msg queue
 * @ns: namespace
 * @params: ptr to the structure that contains the key and msgflg
 *
 * Called with msg_ids.rw_mutex held (writer)
 */
 //创建一个新的msg_queue
static int newque(struct ipc_namespace *ns, struct ipc_params *params)
{
	struct msg_queue *msq;
	int id, retval;
	
	//获取参数
	key_t key = params->key;
	int msgflg = params->flg;

	msq = ipc_rcu_alloc(sizeof(*msq));
	if (!msq)
		return -ENOMEM;

	msq->q_perm.mode = msgflg & S_IRWXUGO;
	msq->q_perm.key = key;

	msq->q_perm.security = NULL;
	retval = security_msg_queue_alloc(msq);
	if (retval) {
		ipc_rcu_putref(msq);
		return retval;
	}

	/*
	 * ipc_addid() locks msq
	 */
	id = ipc_addid(&msg_ids(ns), &msq->q_perm, ns->msg_ctlmni);
	if (id < 0) {
		security_msg_queue_free(msq);
		ipc_rcu_putref(msq);
		return id;
	}

	msq->q_stime = msq->q_rtime = 0;
	msq->q_ctime = get_seconds();
	msq->q_cbytes = msq->q_qnum = 0;
	msq->q_qbytes = ns->msg_ctlmnb;
	msq->q_lspid = msq->q_lrpid = 0;
	INIT_LIST_HEAD(&msq->q_messages);
	INIT_LIST_HEAD(&msq->q_receivers);
	INIT_LIST_HEAD(&msq->q_senders);

	msg_unlock(msq);

	return msq->q_perm.id;
}

static inline void ss_add(struct msg_queue *msq, struct msg_sender *mss)
{
	mss->tsk = current;
	current->state = TASK_INTERRUPTIBLE;
	list_add_tail(&mss->list, &msq->q_senders);
}

static inline void ss_del(struct msg_sender *mss)
{
	if (mss->list.next != NULL)
		list_del(&mss->list);
}

static void ss_wakeup(struct list_head *h, int kill)
{
	struct list_head *tmp;

	tmp = h->next;
	while (tmp != h) {
		struct msg_sender *mss;

		mss = list_entry(tmp, struct msg_sender, list);
		tmp = tmp->next;
		if (kill)
			mss->list.next = NULL;
		wake_up_process(mss->tsk);
	}
}

static void expunge_all(struct msg_queue *msq, int res)
{
	struct list_head *tmp;

	tmp = msq->q_receivers.next;
	while (tmp != &msq->q_receivers) {
		struct msg_receiver *msr;

		msr = list_entry(tmp, struct msg_receiver, r_list);
		tmp = tmp->next;
		msr->r_msg = NULL;
		wake_up_process(msr->r_tsk);
		smp_mb();
		msr->r_msg = ERR_PTR(res);
	}
}

/*
 * freeque() wakes up waiters on the sender and receiver waiting queue,
 * removes the message queue from message queue ID IDR, and cleans up all the
 * messages associated with this queue.
 *
 * msg_ids.rw_mutex (writer) and the spinlock for this message queue are held
 * before freeque() is called. msg_ids.rw_mutex remains locked on exit.
 */
static void freeque(struct ipc_namespace *ns, struct kern_ipc_perm *ipcp)
{
	struct list_head *tmp;
	struct msg_queue *msq = container_of(ipcp, struct msg_queue, q_perm);

	expunge_all(msq, -EIDRM);
	ss_wakeup(&msq->q_senders, 1);
	msg_rmid(ns, msq);
	msg_unlock(msq);

	tmp = msq->q_messages.next;
	while (tmp != &msq->q_messages) {
		struct msg_msg *msg = list_entry(tmp, struct msg_msg, m_list);

		tmp = tmp->next;
		atomic_dec(&ns->msg_hdrs);
		free_msg(msg);
	}
	atomic_sub(msq->q_cbytes, &ns->msg_bytes);
	security_msg_queue_free(msq);
	ipc_rcu_putref(msq);
}

/*
 * Called with msg_ids.rw_mutex and ipcp locked.
 */
static inline int msg_security(struct kern_ipc_perm *ipcp, int msgflg)
{
	struct msg_queue *msq = container_of(ipcp, struct msg_queue, q_perm);

	return security_msg_queue_associate(msq, msgflg);
}

//系统调用，消息队列的获取或创建
SYSCALL_DEFINE2(msgget, key_t, key, int, msgflg)
{
	struct ipc_namespace *ns;
	struct ipc_ops msg_ops;
	struct ipc_params msg_params;

	//当前进程的命名空间，current是当前进程的task_struct
	ns = current->nsproxy->ipc_ns;

	//注册接口函数
	msg_ops.getnew = newque;
	msg_ops.associate = msg_security;
	msg_ops.more_checks = NULL;

	//保存用户传递的ipc参数
	/*
   	  选择IPC关键字，可以使用如下三种方式:
       a)IPC_PRIVATE。由内核负责选择一个关键字然后生成一个IPC对象并把IPC标识符直接传递给另一个进程。
       b)直接选择一个关键字。
       c)使用ftok()函数生成一个关键字
	*/
	
	msg_params.key = key;
	msg_params.flg = msgflg;
	//调用ipcget函数，ipc模块统一处理
	return ipcget(ns, &msg_ids(ns), &msg_ops, &msg_params);
}

static inline unsigned long
copy_msqid_to_user(void __user *buf, struct msqid64_ds *in, int version)
{
	switch(version) {
	case IPC_64:
		return copy_to_user(buf, in, sizeof(*in));
	case IPC_OLD:
	{
		struct msqid_ds out;

		memset(&out, 0, sizeof(out));

		ipc64_perm_to_ipc_perm(&in->msg_perm, &out.msg_perm);

		out.msg_stime		= in->msg_stime;
		out.msg_rtime		= in->msg_rtime;
		out.msg_ctime		= in->msg_ctime;

		if (in->msg_cbytes > USHRT_MAX)
			out.msg_cbytes	= USHRT_MAX;
		else
			out.msg_cbytes	= in->msg_cbytes;
		out.msg_lcbytes		= in->msg_cbytes;

		if (in->msg_qnum > USHRT_MAX)
			out.msg_qnum	= USHRT_MAX;
		else
			out.msg_qnum	= in->msg_qnum;

		if (in->msg_qbytes > USHRT_MAX)
			out.msg_qbytes	= USHRT_MAX;
		else
			out.msg_qbytes	= in->msg_qbytes;
		out.msg_lqbytes		= in->msg_qbytes;

		out.msg_lspid		= in->msg_lspid;
		out.msg_lrpid		= in->msg_lrpid;

		return copy_to_user(buf, &out, sizeof(out));
	}
	default:
		return -EINVAL;
	}
}

static inline unsigned long
copy_msqid_from_user(struct msqid64_ds *out, void __user *buf, int version)
{
	switch(version) {
	case IPC_64:
		if (copy_from_user(out, buf, sizeof(*out)))
			return -EFAULT;
		return 0;
	case IPC_OLD:
	{
		struct msqid_ds tbuf_old;

		if (copy_from_user(&tbuf_old, buf, sizeof(tbuf_old)))
			return -EFAULT;

		out->msg_perm.uid      	= tbuf_old.msg_perm.uid;
		out->msg_perm.gid      	= tbuf_old.msg_perm.gid;
		out->msg_perm.mode     	= tbuf_old.msg_perm.mode;

		if (tbuf_old.msg_qbytes == 0)
			out->msg_qbytes	= tbuf_old.msg_lqbytes;
		else
			out->msg_qbytes	= tbuf_old.msg_qbytes;

		return 0;
	}
	default:
		return -EINVAL;
	}
}

/*
 * This function handles some msgctl commands which require the rw_mutex
 * to be held in write mode.
 * NOTE: no locks must be held, the rw_mutex is taken inside this function.
 */
static int msgctl_down(struct ipc_namespace *ns, int msqid, int cmd,
		       struct msqid_ds __user *buf, int version)
{
	struct kern_ipc_perm *ipcp;
	struct msqid64_ds uninitialized_var(msqid64);
	struct msg_queue *msq;
	int err;

	if (cmd == IPC_SET) {
		if (copy_msqid_from_user(&msqid64, buf, version))
			return -EFAULT;
	}

	ipcp = ipcctl_pre_down(&msg_ids(ns), msqid, cmd,
			       &msqid64.msg_perm, msqid64.msg_qbytes);
	if (IS_ERR(ipcp))
		return PTR_ERR(ipcp);

	msq = container_of(ipcp, struct msg_queue, q_perm);

	err = security_msg_queue_msgctl(msq, cmd);
	if (err)
		goto out_unlock;

	switch (cmd) {
	case IPC_RMID:
		freeque(ns, ipcp);
		goto out_up;
	case IPC_SET:
		if (msqid64.msg_qbytes > ns->msg_ctlmnb &&
		    !capable(CAP_SYS_RESOURCE)) {
			err = -EPERM;
			goto out_unlock;
		}

		msq->q_qbytes = msqid64.msg_qbytes;

		ipc_update_perm(&msqid64.msg_perm, ipcp);
		msq->q_ctime = get_seconds();
		/* sleeping receivers might be excluded by
		 * stricter permissions.
		 */
		expunge_all(msq, -EAGAIN);
		/* sleeping senders might be able to send
		 * due to a larger queue size.
		 */
		ss_wakeup(&msq->q_senders, 0);
		break;
	default:
		err = -EINVAL;
	}
out_unlock:
	msg_unlock(msq);
out_up:
	up_write(&msg_ids(ns).rw_mutex);
	return err;
}

//设置
SYSCALL_DEFINE3(msgctl, int, msqid, int, cmd, struct msqid_ds __user *, buf)
{
	struct msg_queue *msq;
	int err, version;
	struct ipc_namespace *ns;

	if (msqid < 0 || cmd < 0)
		return -EINVAL;

	version = ipc_parse_version(&cmd);
	ns = current->nsproxy->ipc_ns;

	switch (cmd) {
	case IPC_INFO:
	case MSG_INFO:
	{
		struct msginfo msginfo;
		int max_id;

		if (!buf)
			return -EFAULT;
		/*
		 * We must not return kernel stack data.
		 * due to padding, it's not enough
		 * to set all member fields.
		 */
		err = security_msg_queue_msgctl(NULL, cmd);
		if (err)
			return err;

		memset(&msginfo, 0, sizeof(msginfo));
		msginfo.msgmni = ns->msg_ctlmni;
		msginfo.msgmax = ns->msg_ctlmax;
		msginfo.msgmnb = ns->msg_ctlmnb;
		msginfo.msgssz = MSGSSZ;
		msginfo.msgseg = MSGSEG;
		down_read(&msg_ids(ns).rw_mutex);
		if (cmd == MSG_INFO) {
			msginfo.msgpool = msg_ids(ns).in_use;
			msginfo.msgmap = atomic_read(&ns->msg_hdrs);
			msginfo.msgtql = atomic_read(&ns->msg_bytes);
		} else {
			msginfo.msgmap = MSGMAP;
			msginfo.msgpool = MSGPOOL;
			msginfo.msgtql = MSGTQL;
		}
		max_id = ipc_get_maxid(&msg_ids(ns));
		up_read(&msg_ids(ns).rw_mutex);
		if (copy_to_user(buf, &msginfo, sizeof(struct msginfo)))
			return -EFAULT;
		return (max_id < 0) ? 0 : max_id;
	}
	case MSG_STAT:	/* msqid is an index rather than a msg queue id */
	case IPC_STAT:
	{
		struct msqid64_ds tbuf;
		int success_return;

		if (!buf)
			return -EFAULT;

		if (cmd == MSG_STAT) {
			msq = msg_lock(ns, msqid);
			if (IS_ERR(msq))
				return PTR_ERR(msq);
			success_return = msq->q_perm.id;
		} else {
			msq = msg_lock_check(ns, msqid);
			if (IS_ERR(msq))
				return PTR_ERR(msq);
			success_return = 0;
		}
		err = -EACCES;
		if (ipcperms(&msq->q_perm, S_IRUGO))
			goto out_unlock;

		err = security_msg_queue_msgctl(msq, cmd);
		if (err)
			goto out_unlock;

		memset(&tbuf, 0, sizeof(tbuf));

		kernel_to_ipc64_perm(&msq->q_perm, &tbuf.msg_perm);
		tbuf.msg_stime  = msq->q_stime;
		tbuf.msg_rtime  = msq->q_rtime;
		tbuf.msg_ctime  = msq->q_ctime;
		tbuf.msg_cbytes = msq->q_cbytes;
		tbuf.msg_qnum   = msq->q_qnum;
		tbuf.msg_qbytes = msq->q_qbytes;
		tbuf.msg_lspid  = msq->q_lspid;
		tbuf.msg_lrpid  = msq->q_lrpid;
		msg_unlock(msq);
		if (copy_msqid_to_user(buf, &tbuf, version))
			return -EFAULT;
		return success_return;
	}
	case IPC_SET:
	case IPC_RMID:
		err = msgctl_down(ns, msqid, cmd, buf, version);
		return err;
	default:
		return  -EINVAL;
	}

out_unlock:
	msg_unlock(msq);
	return err;
}

static int testmsg(struct msg_msg *msg, long type, int mode)
{
	switch(mode)
	{
		case SEARCH_ANY:
			return 1;
		case SEARCH_LESSEQUAL:
			if (msg->m_type <=type)
				return 1;
			break;
		case SEARCH_EQUAL:
			if (msg->m_type == type)
				return 1;
			break;
		case SEARCH_NOTEQUAL:
			if (msg->m_type != type)
				return 1;
			break;
	}
	return 0;
}

//发送消息时会调用，条件是在发送进程进行发送的时候恰巧接收进程同时也准备好接收了
static inline int pipelined_send(struct msg_queue *msq, struct msg_msg *msg)
{
	struct list_head *tmp;
	
	//轮询每一个receiver队列中的receiver
	tmp = msq->q_receivers.next;
	while (tmp != &msq->q_receivers) {
		struct msg_receiver *msr;
		
		//获取msg_receiver队列的地址
		msr = list_entry(tmp, struct msg_receiver, r_list);
		tmp = tmp->next;
		//testmsg通过消息的type和mode等信息判断该消息是否是接收进程需要的消息
		if (testmsg(msg, msr->r_msgtype, msr->r_mode) &&
		    !security_msg_queue_msgrcv(msq, msg, msr->r_tsk,
					       msr->r_msgtype, msr->r_mode)) {
			//符合条件则取出该receiver 
			list_del(&msr->r_list);
			//消息太大放不下时会报错，算是消息接收失败
			if (msr->r_maxsize < msg->m_ts) {
				msr->r_msg = NULL;
				wake_up_process(msr->r_tsk);
				//多处理器相关，避免编译器优化代码 
				smp_mb();
				msr->r_msg = ERR_PTR(-E2BIG);
			} else {
				//成功取得消息  
				msr->r_msg = NULL;
				//msg_queue的一些变量设置  
　　				//设置last receieved pid
				msq->q_lrpid = task_pid_vnr(msr->r_tsk);
				//设置last msgrcv time
				msq->q_rtime = get_seconds();
				//唤醒接收进程
				wake_up_process(msr->r_tsk);
				smp_mb();
				//获取消息内容
				msr->r_msg = msg;

				return 1;
			}
		}
	}
	return 0;
}

long do_msgsnd(int msqid, long mtype, void __user *mtext,
		size_t msgsz, int msgflg)
{
	struct msg_queue *msq;
	struct msg_msg *msg;
	int err;
	struct ipc_namespace *ns;

	ns = current->nsproxy->ipc_ns;

	//发送小小大小不能大于msg_ctlmax
	if (msgsz > ns->msg_ctlmax || (long) msgsz < 0 || msqid < 0)
		return -EINVAL;
	if (mtype < 1)
		return -EINVAL;

	//把消息放到struct msg中，如果超高一页，则放到msg_msgseg中
	msg = load_msg(mtext, msgsz);
	if (IS_ERR(msg))
		return PTR_ERR(msg);

	msg->m_type = mtype;
	msg->m_ts = msgsz;

	//通过id找到消息队列
	msq = msg_lock_check(ns, msqid);
	if (IS_ERR(msq)) {
		err = PTR_ERR(msq);
		goto out_free;
	}

	for (;;) {
		struct msg_sender s;

		err = -EACCES;
		//权限检查
		if (ipcperms(&msq->q_perm, S_IWUGO))
			goto out_unlock_free;

		err = security_msg_queue_msgsnd(msq, msg, msgflg);
		if (err)
			goto out_unlock_free;

		//队列上数据限制检查
		if (msgsz + msq->q_cbytes <= msq->q_qbytes &&
				1 + msq->q_qnum <= msq->q_qbytes) {
			break;
		}

		//消息队列满了
		/* queue full, wait: */
		if (msgflg & IPC_NOWAIT) {
			err = -EAGAIN;
			goto out_unlock_free;
		}

		//在消息队列尾部添加发送节点
		ss_add(msq, &s);
		ipc_rcu_getref(msq);
		msg_unlock(msq);
		//cpu调度切换了
		schedule();

		ipc_lock_by_ptr(&msq->q_perm);
		ipc_rcu_putref(msq);
		if (msq->q_perm.deleted) {
			err = -EIDRM;
			goto out_unlock_free;
		}

		//删除消息发送
		ss_del(&s);

		if (signal_pending(current)) {
			err = -ERESTARTNOHAND;
			goto out_unlock_free;
		}
	}

	//发送消息
	msq->q_lspid = task_tgid_vnr(current);
	msq->q_stime = get_seconds();

	//可能此时已经有接收者准备接收该消息
	if (!pipelined_send(msq, msg)) {
		/* noone is waiting for this message, enqueue it */
		//如果没有人接收该消息，则把发添加到发送队列上
		list_add_tail(&msg->m_list, &msq->q_messages);
		msq->q_cbytes += msgsz;
		msq->q_qnum++;
		atomic_add(msgsz, &ns->msg_bytes);
		atomic_inc(&ns->msg_hdrs);
	}

	err = 0;
	msg = NULL;

out_unlock_free:
	msg_unlock(msq);
out_free:
	if (msg != NULL)
		free_msg(msg);
	return err;
}

//向消息队列发送消息
SYSCALL_DEFINE4(msgsnd, int, msqid, struct msgbuf __user *, msgp, size_t, msgsz,
		int, msgflg)
{
	long mtype;

	if (get_user(mtype, &msgp->mtype))
		return -EFAULT;
	return do_msgsnd(msqid, mtype, msgp->mtext, msgsz, msgflg);
}

static inline int convert_mode(long *msgtyp, int msgflg)
{
	/*
	 *  find message of correct type.
	 *  msgtyp = 0 => get first.
	 *  msgtyp > 0 => get first message of matching type.
	 *  msgtyp < 0 => get message with least type must be < abs(msgtype).
	 */
	if (*msgtyp == 0)
		return SEARCH_ANY;
	if (*msgtyp < 0) {
		*msgtyp = -*msgtyp;
		return SEARCH_LESSEQUAL;
	}
	if (msgflg & MSG_EXCEPT)
		return SEARCH_NOTEQUAL;
	return SEARCH_EQUAL;
}

long do_msgrcv(int msqid, long *pmtype, void __user *mtext,
		size_t msgsz, long msgtyp, int msgflg)
{
	struct msg_queue *msq;
	struct msg_msg *msg;
	int mode;
	struct ipc_namespace *ns;

	if (msqid < 0 || (long) msgsz < 0)
		return -EINVAL;
	
	mode = convert_mode(&msgtyp, msgflg);
	ns = current->nsproxy->ipc_ns;

	//根据id找到消息队列
	msq = msg_lock_check(ns, msqid);
	if (IS_ERR(msq))
		return PTR_ERR(msq);

	for (;;) {
		struct msg_receiver msr_d;
		struct list_head *tmp;

		msg = ERR_PTR(-EACCES);
		if (ipcperms(&msq->q_perm, S_IRUGO))
			goto out_unlock;

		msg = ERR_PTR(-EAGAIN);
		tmp = msq->q_messages.next;
		while (tmp != &msq->q_messages) {
			struct msg_msg *walk_msg;

			walk_msg = list_entry(tmp, struct msg_msg, m_list);
			if (testmsg(walk_msg, msgtyp, mode) &&
			    !security_msg_queue_msgrcv(msq, walk_msg, current,
						       msgtyp, mode)) {

				msg = walk_msg;
				if (mode == SEARCH_LESSEQUAL &&
						walk_msg->m_type != 1) {
					msg = walk_msg;
					msgtyp = walk_msg->m_type - 1;
				} else {
					msg = walk_msg;
					break;
				}
			}
			tmp = tmp->next;
		}
		if (!IS_ERR(msg)) {
			/*
			 * Found a suitable message.
			 * Unlink it from the queue.
			 */
			if ((msgsz < msg->m_ts) && !(msgflg & MSG_NOERROR)) {
				msg = ERR_PTR(-E2BIG);
				goto out_unlock;
			}
			list_del(&msg->m_list);
			msq->q_qnum--;
			msq->q_rtime = get_seconds();
			msq->q_lrpid = task_tgid_vnr(current);
			msq->q_cbytes -= msg->m_ts;
			atomic_sub(msg->m_ts, &ns->msg_bytes);
			atomic_dec(&ns->msg_hdrs);
			ss_wakeup(&msq->q_senders, 0);
			msg_unlock(msq);
			break;
		}
		/* No message waiting. Wait for a message */
		if (msgflg & IPC_NOWAIT) {
			msg = ERR_PTR(-ENOMSG);
			goto out_unlock;
		}
		list_add_tail(&msr_d.r_list, &msq->q_receivers);
		msr_d.r_tsk = current;
		msr_d.r_msgtype = msgtyp;
		msr_d.r_mode = mode;
		if (msgflg & MSG_NOERROR)
			msr_d.r_maxsize = INT_MAX;
		else
			msr_d.r_maxsize = msgsz;
		msr_d.r_msg = ERR_PTR(-EAGAIN);
		current->state = TASK_INTERRUPTIBLE;
		msg_unlock(msq);

		schedule();

		/* Lockless receive, part 1:
		 * Disable preemption.  We don't hold a reference to the queue
		 * and getting a reference would defeat the idea of a lockless
		 * operation, thus the code relies on rcu to guarantee the
		 * existance of msq:
		 * Prior to destruction, expunge_all(-EIRDM) changes r_msg.
		 * Thus if r_msg is -EAGAIN, then the queue not yet destroyed.
		 * rcu_read_lock() prevents preemption between reading r_msg
		 * and the spin_lock() inside ipc_lock_by_ptr().
		 */
		rcu_read_lock();

		/* Lockless receive, part 2:
		 * Wait until pipelined_send or expunge_all are outside of
		 * wake_up_process(). There is a race with exit(), see
		 * ipc/mqueue.c for the details.
		 */
		msg = (struct msg_msg*)msr_d.r_msg;
		while (msg == NULL) {
			cpu_relax();
			msg = (struct msg_msg *)msr_d.r_msg;
		}

		/* Lockless receive, part 3:
		 * If there is a message or an error then accept it without
		 * locking.
		 */
		if (msg != ERR_PTR(-EAGAIN)) {
			rcu_read_unlock();
			break;
		}

		/* Lockless receive, part 3:
		 * Acquire the queue spinlock.
		 */
		ipc_lock_by_ptr(&msq->q_perm);
		rcu_read_unlock();

		/* Lockless receive, part 4:
		 * Repeat test after acquiring the spinlock.
		 */
		msg = (struct msg_msg*)msr_d.r_msg;
		if (msg != ERR_PTR(-EAGAIN))
			goto out_unlock;

		list_del(&msr_d.r_list);
		if (signal_pending(current)) {
			msg = ERR_PTR(-ERESTARTNOHAND);
out_unlock:
			msg_unlock(msq);
			break;
		}
	}
	if (IS_ERR(msg))
		return PTR_ERR(msg);

	msgsz = (msgsz > msg->m_ts) ? msg->m_ts : msgsz;
	*pmtype = msg->m_type;
	if (store_msg(mtext, msg, msgsz))
		msgsz = -EFAULT;

	free_msg(msg);

	return msgsz;
}

//消息接收
SYSCALL_DEFINE5(msgrcv, int, msqid, struct msgbuf __user *, msgp, size_t, msgsz,
		long, msgtyp, int, msgflg)
{
	long err, mtype;

	err =  do_msgrcv(msqid, &mtype, msgp->mtext, msgsz, msgtyp, msgflg);
	if (err < 0)
		goto out;

	if (put_user(mtype, &msgp->mtype))
		err = -EFAULT;
out:
	return err;
}

#ifdef CONFIG_PROC_FS
static int sysvipc_msg_proc_show(struct seq_file *s, void *it)
{
	struct msg_queue *msq = it;

	return seq_printf(s,
			"%10d %10d  %4o  %10lu %10lu %5u %5u %5u %5u %5u %5u %10lu %10lu %10lu\n",
			msq->q_perm.key,
			msq->q_perm.id,
			msq->q_perm.mode,
			msq->q_cbytes,
			msq->q_qnum,
			msq->q_lspid,
			msq->q_lrpid,
			msq->q_perm.uid,
			msq->q_perm.gid,
			msq->q_perm.cuid,
			msq->q_perm.cgid,
			msq->q_stime,
			msq->q_rtime,
			msq->q_ctime);
}
#endif
