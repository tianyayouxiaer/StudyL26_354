#ifndef _LINUX_IPC_H
#define _LINUX_IPC_H

#include <linux/types.h>

#define IPC_PRIVATE ((__kernel_key_t) 0)  

/* Obsolete, used only for backwards compatibility and libc5 compiles */
//每一个ipc对象，系统共用一个ipc_perm的数据结构来存放权限信息，以确定一个ipc操作是否可以访问该ipc对象
struct ipc_perm
{
	__kernel_key_t	key;//键值
	__kernel_uid_t	uid;//所有者的uid
	__kernel_gid_t	gid;//所有者的组id
	__kernel_uid_t	cuid;//创建者uid
	__kernel_gid_t	cgid;//创建者组id
	__kernel_mode_t	mode; //读写权限
	unsigned short	seq;//存放用来计算该资源的ipc标示符所使用的位置使用序号
};

/* Include the definition of ipc64_perm */
#include <asm/ipcbuf.h>

/* resource get request flags */
#define IPC_CREAT  00001000   /* create if key is nonexistent */
#define IPC_EXCL   00002000   /* fail if key exists */
#define IPC_NOWAIT 00004000   /* return error on wait */

/* these fields are used by the DIPC package so the kernel as standard
   should avoid using them if possible */
   
#define IPC_DIPC 00010000  /* make it distributed */
#define IPC_OWN  00020000  /* this machine is the DIPC owner */

/* 
 * Control commands used with semctl, msgctl and shmctl 
 * see also specific commands in sem.h, msg.h and shm.h
 */
#define IPC_RMID 0     /* remove resource */
#define IPC_SET  1     /* set ipc_perm options */
#define IPC_STAT 2     /* get ipc_perm options */
#define IPC_INFO 3     /* see ipcs */

/*
 * Version flags for semctl, msgctl, and shmctl commands
 * These are passed as bitflags or-ed with the actual command
 */
#define IPC_OLD 0	/* Old version (no 32-bit UID support on many
			   architectures) */
#define IPC_64  0x0100  /* New version (support 32-bit UIDs, bigger
			   message sizes, etc. */

/*
 * These are used to wrap system calls.
 *
 * See architecture code for ugly details..
 */
struct ipc_kludge {
	struct msgbuf __user *msgp;
	long msgtyp;
};

#define SEMOP		 1
#define SEMGET		 2
#define SEMCTL		 3
#define SEMTIMEDOP	 4
#define MSGSND		11
#define MSGRCV		12
#define MSGGET		13
#define MSGCTL		14
#define SHMAT		21
#define SHMDT		22
#define SHMGET		23
#define SHMCTL		24

/* Used by the DIPC package, try and avoid reusing it */
#define DIPC            25

#define IPCCALL(version,op)	((version)<<16 | (op))

#ifdef __KERNEL__
#include <linux/spinlock.h>

//系统中同时运行的最大msg_queue的个数
#define IPCMNI 32768  /* <= MAX_INT limit for ipc arrays (including sysctl changes) */

/* used by in-kernel data structures */
//IPC对象属性结构kern_ipc_perm
//System V IPC在内核以统一的数据结构形式进行实现，它的对象包括System V的消息队列、信号量和共享内存
/*
进程在存取System V IPC对象时，规则如下：

如果进程有root权限，则可以存取对象。
如果进程的EUID是对象所有者或创建者的UID，那么检查相应的创建者许可比特位，查看是否有存取权限。
如果进程的EGID是对象所有者或创建者的GID，或者进程所属群组中某个群组的GID就是对象所有者或创建者的GID，
那么，检查相 应的创建者群组许可比特位是滞有权限。否则，在存取时检查相应的"其他人"许可比特位。


在结构kern_ipc_perm中，键值为公有和私有。如果键是公有的，则系统中所有的进程通过权限检查后，
均可以找到System V IPC 对象的识别号。如果键是私有的，则键值为0，说明每个进程都可以用键值0建
立一个专供其私用的对象。System V IPC对象的引用是通过识别号而不是通过键。

结构kern_ipc_perm是IPC对象的共有属性，每个具体的IPC对象结构将继承此结构。
*/

//然后在传递IPC对象的时候，传的也是struct kern_ipc_perm的指针，再用container_of这样的宏获得外面的struct，
//这样就能用同一个函数操作3种IPC对象，达到较好的代码重用。

struct kern_ipc_perm
{
	spinlock_t	lock;
	int		deleted;/*删除标识，表示该结构对象已删除*/
	int		id;/*id识别号，每个IPC对象的身份号，便于从IPC对象数组中获取该对象*/
	key_t		key; /*键值：公有或私有*/
	uid_t		uid;// IPC对象拥有者id
	gid_t		gid; // 组id
	uid_t		cuid; // 创建者id
	gid_t		cgid;
	mode_t		mode;  /*许可的组合*/
	unsigned long	seq;  /*在每个IPC对象类型中的序列号*/
	void		*security; /*与SELinux安全相关的变量*/
};

#endif /* __KERNEL__ */

#endif /* _LINUX_IPC_H */
