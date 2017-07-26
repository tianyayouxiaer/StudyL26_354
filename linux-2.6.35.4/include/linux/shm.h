#ifndef _LINUX_SHM_H_
#define _LINUX_SHM_H_

#include <linux/ipc.h>
#include <linux/errno.h>
#ifdef __KERNEL__
#include <asm/page.h>
#else
#include <unistd.h>
#endif

/*
 * SHMMAX, SHMMNI and SHMALL are upper limits are defaults which can
 * be increased by sysctl
 */
 
/*
说明：同一块共享内存在内核中至少有3个标识符

1、IPC对象id（IPC对象是保存IPC信息的数据结构）

2、进程虚拟内存中文件的inode，即每个进程中的共享内存也是以文件的方式存在的，但并不是显式的。
   可以通过某个vm_area_struct->vm_file->f_dentry->d_inode->i_ino表示。
   
3、IPC对象的key。如果在shmget()中传入同一个key可以获取到同一块共享内存。
但由于key是用户指定的，可能重复，而且也很少程序写之前会约定一个key，所以这种方法不是很常用。
通常System V这种共享内存的方式是用于有父子关系的进程的。或者用ftok()函数用路径名来生成一个key。
*/

#define SHMMAX 0x2000000		 /* max shared seg size (bytes) */
#define SHMMIN 1			 /* min shared seg size (bytes) */
#define SHMMNI 4096			 /* max num of segs system wide */
#ifdef __KERNEL__
#define SHMALL (SHMMAX/PAGE_SIZE*(SHMMNI/16)) /* max shm system wide (pages) */
#else
#define SHMALL (SHMMAX/getpagesize()*(SHMMNI/16))
#endif
#define SHMSEG SHMMNI			 /* max shared segs per process */

#ifdef __KERNEL__
#include <asm/shmparam.h>
#endif

/* Obsolete, used only for backwards compatibility and libc5 compiles */
//每一个新创建的共享内存由一个shmid_ds数据结构表示，如果shmget成功创建了一块共享内存，则返回一个可以用于
//引用该共享内存的shmid_ds数据结构标示符
struct shmid_ds {
	struct ipc_perm		shm_perm;	/* operation perms *///存取权限
	int			shm_segsz;	/* size of segment (bytes) */
	__kernel_time_t		shm_atime;	/* last attach time */
	__kernel_time_t		shm_dtime;	/* last detach time *///最后一次断开时间
	__kernel_time_t		shm_ctime;	/* last change time */
	__kernel_ipc_pid_t	shm_cpid;	/* pid of creator *///创建该共享内存的进程id
	__kernel_ipc_pid_t	shm_lpid;	/* pid of last operator *///最后一个操作该共享内存的进程id
	unsigned short		shm_nattch;	/* no. of current attaches *///连接该共享内存的进程数目
	unsigned short 		shm_unused;	/* compatibility */
	void 			*shm_unused2;	/* ditto - used by DIPC */
	void			*shm_unused3;	/* unused */
};

/* Include the definition of shmid64_ds and shminfo64 */
#include <asm/shmbuf.h>

/* permission flag for shmget */
#define SHM_R		0400	/* or S_IRUGO from <linux/stat.h> */
#define SHM_W		0200	/* or S_IWUGO from <linux/stat.h> */

/* mode for attach */
#define	SHM_RDONLY	010000	/* read-only access */
#define	SHM_RND		020000	/* round attach address to SHMLBA boundary */
#define	SHM_REMAP	040000	/* take-over region on attach */
#define	SHM_EXEC	0100000	/* execution access */

/* super user shmctl commands */
#define SHM_LOCK 	11
#define SHM_UNLOCK 	12

/* ipcs ctl commands */
#define SHM_STAT 	13
#define SHM_INFO 	14

/* Obsolete, used only for backwards compatibility */
struct	shminfo {
	int shmmax;
	int shmmin;
	int shmmni;
	int shmseg;
	int shmall;
};

struct shm_info {
	int used_ids;
	unsigned long shm_tot;	/* total allocated shm */
	unsigned long shm_rss;	/* total resident shm */
	unsigned long shm_swp;	/* total swapped shm */
	unsigned long swap_attempts;
	unsigned long swap_successes;
};

#ifdef __KERNEL__
//表示一块共享内存的数据结构
struct shmid_kernel /* private to the kernel */
{	
	struct kern_ipc_perm	shm_perm;// 权限，这个结构体中还有一些重要的内容，后面会提到
	struct file *		shm_file; // 表示这块共享内存的内核文件，文件内容即共享内存的内容
	unsigned long		shm_nattch;// 连接到这块共享内存的进程数
	unsigned long		shm_segsz;// 大小，字节为单位
	time_t			shm_atim;// 最后一次连接时间
	time_t			shm_dtim;// 最后一次断开时间
	time_t			shm_ctim;// 最后一次更改信息的时间
	pid_t			shm_cprid;// 创建者进程id
	pid_t			shm_lprid;// 最后操作者进程id
	struct user_struct	*mlock_user;
};

/* shm_mode upper byte flags */
#define	SHM_DEST	01000	/* segment will be destroyed on last detach */
#define SHM_LOCKED      02000   /* segment will not be swapped */
#define SHM_HUGETLB     04000   /* segment will use huge TLB pages */
#define SHM_NORESERVE   010000  /* don't check for reservations */

#ifdef CONFIG_SYSVIPC
long do_shmat(int shmid, char __user *shmaddr, int shmflg, unsigned long *addr);
extern int is_file_shm_hugepages(struct file *file);
#else
static inline long do_shmat(int shmid, char __user *shmaddr,
				int shmflg, unsigned long *addr)
{
	return -ENOSYS;
}
static inline int is_file_shm_hugepages(struct file *file)
{
	return 0;
}
#endif

#endif /* __KERNEL__ */

#endif /* _LINUX_SHM_H_ */
