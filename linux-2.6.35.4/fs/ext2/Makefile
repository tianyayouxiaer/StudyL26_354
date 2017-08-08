#
# Makefile for the linux ext2-filesystem routines.
#
#如果想配置内核支持ext2文件系统的话，首先你需要配置上支持CONFIG_EXT2_FS
obj-$(CONFIG_EXT2_FS) += ext2.o
#ext2正常运行必须的文件
ext2-y := balloc.o dir.o file.o ialloc.o inode.o \
	  ioctl.o namei.o super.o symlink.o
#如果想配置支持文件属性，就配置CONFIG_EXT2_FS_XATTR，这个一般都需要配置的吧
ext2-$(CONFIG_EXT2_FS_XATTR)	 += xattr.o xattr_user.o xattr_trusted.o
#如果想配置支持ACL，就配置CONFIG_EXT2_FS_POSIX_ACL  
ext2-$(CONFIG_EXT2_FS_POSIX_ACL) += acl.o
#如果想配置支持安全操作，就配置CONFIG_EXT2_FS_SECURITY，这个视情况而定，不过一般pc应该都需要的 
ext2-$(CONFIG_EXT2_FS_SECURITY)	 += xattr_security.o
#如果想配置支持片内执行，就配置CONFIG_EXT2_FS_XIP，一般有的嵌入式设备可能为了空间考虑就没配置  
ext2-$(CONFIG_EXT2_FS_XIP)	 += xip.o
