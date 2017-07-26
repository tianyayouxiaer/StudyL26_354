/*
 * linux/ipc/msgutil.c
 * Copyright (C) 1999, 2004 Manfred Spraul
 *
 * This file is released under GNU General Public Licence version 2 or
 * (at your option) any later version.
 *
 * See the file COPYING for more details.
 */

#include <linux/spinlock.h>
#include <linux/init.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/ipc.h>
#include <linux/ipc_namespace.h>
#include <asm/uaccess.h>

#include "util.h"

//工具函数

DEFINE_SPINLOCK(mq_lock);

/*
 * The next 2 defines are here bc this is the only file
 * compiled when either CONFIG_SYSVIPC and CONFIG_POSIX_MQUEUE
 * and not CONFIG_IPC_NS.
 */
struct ipc_namespace init_ipc_ns = {
	.count		= ATOMIC_INIT(1),
#ifdef CONFIG_POSIX_MQUEUE
	.mq_queues_max   = DFLT_QUEUESMAX,
	.mq_msg_max      = DFLT_MSGMAX,
	.mq_msgsize_max  = DFLT_MSGSIZEMAX,
#endif
};

atomic_t nr_ipc_ns = ATOMIC_INIT(1);

/*
msg_msgseg只需要存储指向下一链表块的指针就行了，每个msg_msgseg也将占据一个page的空间，
其中一个page除了存储该结构体，剩下的部分都将用来存储message的数据。
*/
struct msg_msgseg {
	struct msg_msgseg* next;
	/* the next part of the message follows immediately */
};

#define DATALEN_MSG	(PAGE_SIZE-sizeof(struct msg_msg))
#define DATALEN_SEG	(PAGE_SIZE-sizeof(struct msg_msgseg))

//发送消息时使用
struct msg_msg *load_msg(const void __user *src, int len)
{
	struct msg_msg *msg;
	struct msg_msgseg **pseg;
	int err;
	int alen;
	
	// #define DATALEN_MSG (PAGE_SIZE-sizeof(struct msg_msg))  
	// DATALEN_MSG是一个page减去msg_msg结构体的大小，如前面所说，就是一个page可用来存储数据部分的大小  
	//这里首先处理了第一个page的问题  
	alen = len;
	if (alen > DATALEN_MSG)
		alen = DATALEN_MSG;

	//kmalloc为msg分配空间
	msg = kmalloc(sizeof(*msg) + alen, GFP_KERNEL);
	if (msg == NULL)
		return ERR_PTR(-ENOMEM);

	msg->next = NULL;
	msg->security = NULL;
	
	//该函数底层用汇编代码实现，主要工作是将msg的内容从用户态空间转移到内核态空间，
	//显然msg+1返回的地址就是page余下用来存储msg内容的基地址 
	if (copy_from_user(msg + 1, src, alen)) {
		err = -EFAULT;
		goto out_err;
	}

	//下面考虑如果msg的长度大于一个page的容纳量，将增加更多的page，不过增加的page的头部是msg_msgseg结构体
	len -= alen;
	src = ((char __user *)src) + alen;
	pseg = &msg->next;
	//while循环，保证足够多的page去存储所有的消息text
	while (len > 0) {
		struct msg_msgseg *seg;
		//这边的工作其实和之前一样，只是分配的结构体是msg_msgseg
		alen = len;
		if (alen > DATALEN_SEG)
			alen = DATALEN_SEG;
		seg = kmalloc(sizeof(*seg) + alen,
						 GFP_KERNEL);
		if (seg == NULL) {
			err = -ENOMEM;
			goto out_err;
		}
		*pseg = seg;
		seg->next = NULL;
		//同样是copy_from_user的操作
		if (copy_from_user(seg + 1, src, alen)) {
			err = -EFAULT;
			goto out_err;
		}
		pseg = &seg->next;
		len -= alen;
		src = ((char __user *)src) + alen;
	}

	err = security_msg_msg_alloc(msg);
	if (err)
		goto out_err;
		
	//返回第一个msg_msg的地址  
	//如果之前发生了error（相关代码已经省去），将会执行free_msg(msg)的工作进行内存释放，最后返回错误信息
	return msg;

out_err:
	free_msg(msg);
	return ERR_PTR(err);
}

//接收消息时使用，主要逻辑类似load_msg
int store_msg(void __user *dest, struct msg_msg *msg, int len)
{
	int alen;
	struct msg_msgseg *seg;

	alen = len;
	if (alen > DATALEN_MSG)
		alen = DATALEN_MSG;
	//对应于copy_from_user，这里copy_to_user的工作就是将消息从内核态空间调整到用户态，底层也是汇编实现的  
	if (copy_to_user(dest, msg + 1, alen))
		return -1;

	len -= alen;
	dest = ((char __user *)dest) + alen;
	seg = msg->next;
	while (len > 0) {
		alen = len;
		if (alen > DATALEN_SEG)
			alen = DATALEN_SEG;
		if (copy_to_user(dest, seg + 1, alen))
			return -1;
		len -= alen;
		dest = ((char __user *)dest) + alen;
		seg = seg->next;
	}
	return 0;
}

//用于在发生错误时，释放已经为msg结构分配的pages
void free_msg(struct msg_msg *msg)
{
	struct msg_msgseg *seg;

	security_msg_msg_free(msg);

	seg = msg->next;
	kfree(msg);
	while (seg != NULL) {
		struct msg_msgseg *tmp = seg->next;
		kfree(seg);
		seg = tmp;
	}
}
