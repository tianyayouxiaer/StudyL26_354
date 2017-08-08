/*
  File: fs/ext2/acl.h

  (C) 2001 Andreas Gruenbacher, <a.gruenbacher@computer.org>
*/

#include <linux/posix_acl_xattr.h>


/*
http://blog.csdn.net/sanwenyublog/article/details/50880609

首先我想说一下什么是acl，之前的linux系统对于文件的权限管理都是9位的管理方式，
user，group，other各三位，分别是rwx，读写执行三个权限，但是后来发现这种方式有缺陷，
所以呢，访问控制列表ACL就应运而生。比如user1，user2和user3都在group组里边，但是user1
的文件只想让user2查看，而不想让user3查看，为此，user1需要建立一个只有user1和user2的组，
这不但带来了不必要的开销，也是有安全隐患的。而现在用acl机制，可以给予用户，
目录以单独的权限，使用命令  setfacl -m user:user3:--- file 就可以使 user3 对 file 没有任何权限。
*/

#define EXT2_ACL_VERSION	0x0001

/*ext2文件系统的acl结构体，遵循posix标准，和POSIX标准的一样*/ 
typedef struct {
	//在 Posix 标准中规定一共有 6 种 e_tag ，分别是 ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_GROUP, ACL_MASK, ACL_OTHER，
	//分别代表文件主，文件其他用户，文件组主，文件其他组，文件掩码，其他人。
	__le16		e_tag;
	//e_perm代表权限，就是rwx
	__le16		e_perm;
	//e_id除了ACL_USER和ACL_GROUP都是空。
	__le32		e_id;
} ext2_acl_entry;
/*ext2文件系统的简短的结构体，和posix标准的区别是没有了e_id字段*/  

typedef struct {
	__le16		e_tag;
	__le16		e_perm;
} ext2_acl_entry_short;
/*ext2的头部，仅仅有一个版本号*/
typedef struct {
	__le32		a_version;
} ext2_acl_header;

/*内联函数，从acl项目的数目获得ext2的acl大小*/
static inline size_t ext2_acl_size(int count)
{
    /*由于e_id字段除了ACL_USER和ACL_GROUP都是空，所以如果count<=4的话，就是没有e_id的4个，
      就是acl头的大小加上count乘上ext2_acl_entry_short的大小*/ 
	if (count <= 4) {
		return sizeof(ext2_acl_header) +
		       count * sizeof(ext2_acl_entry_short);
	} else {
		/*如果大于4，说明有ACL_USER和ACL_GROUP这两个字段，e_id不为空，
		  所以除了头的大小和四个没有e_id字段的大小，加上*/ 
		return sizeof(ext2_acl_header) +
		       4 * sizeof(ext2_acl_entry_short) +
		       (count - 4) * sizeof(ext2_acl_entry);
	}
}

/*从acl控制结构体的大小返回acl项的数目*/ 
static inline int ext2_acl_count(size_t size)
{
	ssize_t s;
	/*所有的acl都有acl头，所以先去除头结构体的大小*/ 
	size -= sizeof(ext2_acl_header);
	/*然后减去4个默认的 ACL_USER_OBJ, ACL_GROUP_OBJ, ACL_MASK, ACL_OTHER*/
	s = size - 4 * sizeof(ext2_acl_entry_short);
	/*如果小于零，说明没有e_id不为零的项，直接减去header的大小除以ext2_acl_entry_short的大小就是数目*/
	if (s < 0) {
		if (size % sizeof(ext2_acl_entry_short))
			return -1;
		return size / sizeof(ext2_acl_entry_short);
	} else {
		/*如果大于0，说明有e_id不为0的项，所以余下的大小除以ext2_acl_entry就得到数目*/
		if (s % sizeof(ext2_acl_entry))
			return -1;
		return s / sizeof(ext2_acl_entry) + 4;
	}
}

/*如果配置了CONFIG_EXT2_FS_POSIX_ACL，就设置一些宏，否则设置宏和函数为空*/ 
#ifdef CONFIG_EXT2_FS_POSIX_ACL

/* acl.c */
extern int ext2_check_acl (struct inode *, int);
extern int ext2_acl_chmod (struct inode *);
extern int ext2_init_acl (struct inode *, struct inode *);

#else
/*如果没有配置这个宏*/  
#include <linux/sched.h>
#define ext2_check_acl	NULL
#define ext2_get_acl	NULL
#define ext2_set_acl	NULL

static inline int
ext2_acl_chmod (struct inode *inode)
{
	return 0;
}

static inline int ext2_init_acl (struct inode *inode, struct inode *dir)
{
	return 0;
}
#endif

