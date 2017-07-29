#ifndef _LINUX_MSG_H
#define _LINUX_MSG_H

#include <linux/ipc.h>


//说明： http://blog.csdn.net/kipdoudou/article/details/50000765

/* ipcs ctl commands */
#define MSG_STAT 11
#define MSG_INFO 12

/* msgrcv options */
#define MSG_NOERROR     010000  /* no error if message is too big */
#define MSG_EXCEPT      020000  /* recv any msg except of specified type.*/

/* Obsolete, used only for backwards compatibility and libc5 compiles */
// 消息队列状态msqid_ds,是msg_queue上msg_message队列的数据结构
struct msqid_ds {
	struct ipc_perm msg_perm;
	struct msg *msg_first;		/* first message on queue,unused  */
	struct msg *msg_last;		/* last message in queue,unused */
	__kernel_time_t msg_stime;	/* last msgsnd time */
	__kernel_time_t msg_rtime;	/* last msgrcv time */
	__kernel_time_t msg_ctime;	/* last change time */
	unsigned long  msg_lcbytes;	/* Reuse junk fields for 32 bit */
	unsigned long  msg_lqbytes;	/* ditto */
	unsigned short msg_cbytes;	/* current number of bytes on queue */
	unsigned short msg_qnum;	/* number of messages in queue */
	unsigned short msg_qbytes;	/* max number of bytes on queue */
	__kernel_ipc_pid_t msg_lspid;	/* pid of last msgsnd */
	__kernel_ipc_pid_t msg_lrpid;	/* last receive pid */
};

/* Include the definition of msqid64_ds */
#include <asm/msgbuf.h>

/* message buffer for msgsnd and msgrcv calls */
struct msgbuf {
	long mtype;         /* type of message */
	char mtext[1];      /* message text */
};

/* buffer for msgctl calls IPC_INFO, MSG_INFO */
struct msginfo {
	int msgpool;
	int msgmap; 
	int msgmax; 
	int msgmnb; 
	int msgmni; 
	int msgssz; 
	int msgtql; 
	unsigned short  msgseg; 
};

/*
 * Scaling factor to compute msgmni:
 * the memory dedicated to msg queues (msgmni * msgmnb) should occupy
 * at most 1/MSG_MEM_SCALE of the lowmem (see the formula in ipc/msg.c):
 * up to 8MB          : msgmni = 16 (MSGMNI)
 * 4 GB         	  : msgmni = 8K
 * more than 16 GB : msgmni = 32K (IPCMNI)
 */
#define MSG_MEM_SCALE 32
//消息队列数
#define MSGMNI    16   /* <= IPCMNI */     /* max # of msg queue identifiers */
//消息的最大size，8KB
#define MSGMAX  8192   /* <= INT_MAX */   /* max size of message (bytes) */
//默认消息队列的最大大小16KB
#define MSGMNB 16384   /* <= INT_MAX */   /* default max size of a message queue */

/* unused */
#define MSGPOOL (MSGMNI * MSGMNB / 1024) /* size in kbytes of message pool */
#define MSGTQL  MSGMNB            /* number of system message headers */
#define MSGMAP  MSGMNB            /* number of entries in message map */
#define MSGSSZ  16                /* message segment size */
#define __MSGSEG ((MSGPOOL * 1024) / MSGSSZ) /* max no. of segments */
#define MSGSEG (__MSGSEG <= 0xffff ? __MSGSEG : 0xffff)

#ifdef __KERNEL__
#include <linux/list.h>

/* one msg_msg structure for each message */

/*
消息的基本数据结构，每个msg_msg将占据一个page的内容，其中一个page除了存储该结构体，
空余的部分直接用来存储消息的内容。其中记录了一条消息的type和size，next指针用于指向msg_msgseg结构。

在消息队列的设计中，若消息内容大于一个page，则会使用一个类似链表的结构，但是后面的节点不需要再标记
type和size等数据，后面的节点用msg_msgseg表示。

*/
struct msg_msg {
	struct list_head m_list; 
	long  m_type;  //消息类型        
	int m_ts;           /* message text size *///消息的文本大小
	struct msg_msgseg* next;//下一条消息
	void *security;
	/* the actual message follows immediately */
};

/* one msq_queue structure for each present queue on the system */
/*
消息队列msg_queue的数据结构，其中包含message、receiver、sender队列的链表指针，同时还包含其他一些相关数据
描述一个消息队列
*/
struct msg_queue {
	struct kern_ipc_perm q_perm;
	time_t q_stime;			/* last msgsnd time *///上次消息发送的时间　
	time_t q_rtime;			/* last msgrcv time */ //上次消息接收的时间
	time_t q_ctime;			/* last change time *///上次发生变化的时间
	unsigned long q_cbytes;		/* current number of bytes on queue *///队列上当前的字节数
	unsigned long q_qnum;		/* number of messages in queue *///队列里的消息数
	unsigned long q_qbytes;		/* max number of bytes on queue *///队列上的最大字节数
	pid_t q_lspid;			/* pid of last msgsnd *///上次发送消息进程的pid
	pid_t q_lrpid;			/* last receive pid *///上次接收消息进程的pid

	struct list_head q_messages;//消息队列
	struct list_head q_receivers;//消息接收进程链表
	struct list_head q_senders;//消息发送进程链表
};

/* Helper routines for sys_msgsnd and sys_msgrcv */
extern long do_msgsnd(int msqid, long mtype, void __user *mtext,
			size_t msgsz, int msgflg);
extern long do_msgrcv(int msqid, long *pmtype, void __user *mtext,
			size_t msgsz, long msgtyp, int msgflg);

#endif /* __KERNEL__ */

#endif /* _LINUX_MSG_H */
