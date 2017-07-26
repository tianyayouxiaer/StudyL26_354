#ifndef __IPC_NAMESPACE_H__
#define __IPC_NAMESPACE_H__

#include <linux/err.h>
#include <linux/idr.h>
#include <linux/rwsem.h>
#include <linux/notifier.h>

/*
Linux内核将信号量、消息队列和共享内存三类IPC的通用操作进行抽象，形成通用的方法函数（函数名中含有"ipc"）。
每类具体的操作函数通 过在函数参数中传入通用函数名的方法，继承通用方法函数。

System V IPC数据结构
System V IPC数据结构包括名字空间结构ipc_namespace、ID集结构ipc_ids和IPC对象属性结构kern_ipc_perm。

其中，结构 ipc_namespace是所有IPC对象ID集的入口，结构ipc_ids描述每一类IPC对象（如：信号量对象）的ID，
结构 kern_ipc_perm描述IPC对象许可的共有对象，被每一类IPC对象结构继承。

*/

/*
 * ipc namespace events
 */
#define IPCNS_MEMCHANGED   0x00000001   /* Notify lowmem size changed */
#define IPCNS_CREATED  0x00000002   /* Notify new ipc namespace created */
#define IPCNS_REMOVED  0x00000003   /* Notify ipc namespace removed */

#define IPCNS_CALLBACK_PRI 0

//描述每一类IPC对象（如：信号量对象）的ID
/*
每个System V IPC 对象有一个ID号，每一类System V IPC 对象（如：信号量对象）的所有ID构成ID集，
ID集用结构ipc_ids描述，对象指针与ID通过IDR机制关联起来。IDR机制是一种用radix树 存放ID和对象映射，
作用类似于以ID为序号的数组，但不受数组尺寸的限制。
*/

/*
比较重要的是其中的ids，它存的是所用IPC对象的id，其中共享内存都存在ids[2]中。而在ids[2]中真正负责管理数据的是ipcs_idr，
它也是内核中一个煞费苦心弄出来的id管理机制，一个id可以对应任意唯一确定的对象。把它理解成一个数组就好。它们之间的关
系大概如下图所示。

                                                 		        [0] struct kern_ipc_perm <==> struct shmid_kernel
struct ipc_namespace => struct ipc_ids => struct idr => [1] struct kern_ipc_perm <==> struct shmid_kernel
                                                		        [2] struct kern_ipc_perm <==> struct shmid_kernel

*/
struct ipc_ids {
	int in_use;//已分配ipc资源数
	unsigned short seq;//下一个分配位置使用序号
	unsigned short seq_max;//最大位置使用序号
	struct rw_semaphore rw_mutex;//保护ipc_ids数据结构信号量
	struct idr ipcs_idr;/*通过IDR机制将ID与结构kern_ipc_perm类型指针建立关联*/
};

/*
linux很多资源是全局管理的，例如系统中所有的进程是通过pid标识的，这意味着内核管理着一个全局pid表，
进程号必须为唯一的。类似的还有内核的文件系统挂载点数据信息、用户ID号等。我们知道，要实现虚拟化必
须要有独立的资源分配，才能使容器之间不互相影响，那如何使这些全局表局域化呢？答案是namespace。
Namespace将传统的全局资源变为某个名字空间的局域资源。

现的namespace主要有: 

1.Mount namespace(CLONE_NEWNS):系统挂载点 				  //提供磁盘挂载点和文件系统的隔离能力
2.UTS namespace (CLONE_NEWUTS):Hostname等信息  //提供主机名隔离能力 
3.IPC namespace(CLONE_NEWIPC):进程间通讯 				  //提供进程间通信的隔离能力
4.PID namespace(CLONE_NEWPID):进程号 					  //提供进程隔离能力
5.Network namespace(CLONE_NEWNET):网络相关资源 	//提供网络隔离能力
6.User namespace(CLONE_NEWUSER):用户ID 				//提供用户隔离能力

可以看出，以上的这些系统资源在没有引入namespace时是由内核全局管理的。linux内核为了支持容器虚拟化功能，
加入了以上6种namespace，实现这些全局系统资源局域化，使每一个namespace空间都拥有独立的一套系统资源。
由于本文主要讲述Docker虚拟化的实现原理，考虑到篇幅，将主要从内核角度简介linux的PID namespace。
PID namespace使属于不同的名字空间的进程可以拥有相同的进程号，对实现docker的虚拟化至关重要。

*/
// ipc_namespace是所有IPC对象ID集的入口

// 进程通过系统调用进入内核空间后，通过结构ipc_namespace类型全局变量init_ipc_ns找到
// 对应类型IPC对象的ID 集，由ID集找到ID，再由ID找到对象的描述结构，从对象描述结构中获取通信数据了。

/*
所有System V IPC 对象的ID集存放在结构ipc_namespace类型的全局变量init_ipc_ns中，
用户空间的进程进入内核空间后为内核空间的线程，内核空间的 线程共享全局变量。因此，
不同的进程可以通过全局变量init_ipc_ns查询IPC对象的信息，从而实现进程间通信。
*/

struct ipc_namespace {
	atomic_t	count;
	struct ipc_ids	ids[3];
	//分别对应信号量、消息队列和共享内存的ID集
	int		sem_ctls[4];
	int		used_sems;

	int		msg_ctlmax;
	int		msg_ctlmnb;
	int		msg_ctlmni;
	atomic_t	msg_bytes;
	atomic_t	msg_hdrs;
	int		auto_msgmni;

	size_t		shm_ctlmax;
	size_t		shm_ctlall;
	int		shm_ctlmni;
	int		shm_tot;

	struct notifier_block ipcns_nb;

	/* The kern_mount of the mqueuefs sb.  We take a ref on it */
	struct vfsmount	*mq_mnt;

	/* # queues in this ns, protected by mq_lock */
	unsigned int    mq_queues_count;

	/* next fields are set through sysctl */
	unsigned int    mq_queues_max;   /* initialized to DFLT_QUEUESMAX */
	unsigned int    mq_msg_max;      /* initialized to DFLT_MSGMAX */
	unsigned int    mq_msgsize_max;  /* initialized to DFLT_MSGSIZEMAX */

};

extern struct ipc_namespace init_ipc_ns;
extern atomic_t nr_ipc_ns;

extern spinlock_t mq_lock;

#ifdef CONFIG_SYSVIPC
extern int register_ipcns_notifier(struct ipc_namespace *);
extern int cond_register_ipcns_notifier(struct ipc_namespace *);
extern void unregister_ipcns_notifier(struct ipc_namespace *);
extern int ipcns_notify(unsigned long);
#else /* CONFIG_SYSVIPC */
static inline int register_ipcns_notifier(struct ipc_namespace *ns)
{ return 0; }
static inline int cond_register_ipcns_notifier(struct ipc_namespace *ns)
{ return 0; }
static inline void unregister_ipcns_notifier(struct ipc_namespace *ns) { }
static inline int ipcns_notify(unsigned long l) { return 0; }
#endif /* CONFIG_SYSVIPC */

#ifdef CONFIG_POSIX_MQUEUE
extern int mq_init_ns(struct ipc_namespace *ns);
/* default values */
#define DFLT_QUEUESMAX 256     /* max number of message queues */
#define DFLT_MSGMAX    10      /* max number of messages in each queue */
#define HARD_MSGMAX    (32768*sizeof(void *)/4)
#define DFLT_MSGSIZEMAX 8192   /* max message size */
#else
static inline int mq_init_ns(struct ipc_namespace *ns) { return 0; }
#endif

#if defined(CONFIG_IPC_NS)
extern struct ipc_namespace *copy_ipcs(unsigned long flags,
				       struct ipc_namespace *ns);
static inline struct ipc_namespace *get_ipc_ns(struct ipc_namespace *ns)
{
	if (ns)
		atomic_inc(&ns->count);
	return ns;
}

extern void put_ipc_ns(struct ipc_namespace *ns);
#else
static inline struct ipc_namespace *copy_ipcs(unsigned long flags,
		struct ipc_namespace *ns)
{
	if (flags & CLONE_NEWIPC)
		return ERR_PTR(-EINVAL);

	return ns;
}

static inline struct ipc_namespace *get_ipc_ns(struct ipc_namespace *ns)
{
	return ns;
}

static inline void put_ipc_ns(struct ipc_namespace *ns)
{
}
#endif

#ifdef CONFIG_POSIX_MQUEUE_SYSCTL

struct ctl_table_header;
extern struct ctl_table_header *mq_register_sysctl_table(void);

#else /* CONFIG_POSIX_MQUEUE_SYSCTL */

static inline struct ctl_table_header *mq_register_sysctl_table(void)
{
	return NULL;
}

#endif /* CONFIG_POSIX_MQUEUE_SYSCTL */
#endif
