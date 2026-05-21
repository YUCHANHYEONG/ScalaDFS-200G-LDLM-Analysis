MODULES := osd_zfs
osd_zfs-objs := osd_handler.o osd_lproc.o osd_quota.o
osd_zfs-objs += osd_object.o osd_io.o osd_oi.o osd_xattr.o osd_index.o
osd_zfs-objs += osd_scrub.o

#EXTRA_PRE_CFLAGS += -include /spl_config.h
#EXTRA_PRE_CFLAGS += -I -I/include
EXTRA_PRE_CFLAGS += -include /zfs_config.h
EXTRA_PRE_CFLAGS += -I/include/os/linux/kernel
EXTRA_PRE_CFLAGS += -I/include/os/linux/spl -I/include/os/linux/zfs
EXTRA_PRE_CFLAGS += -I -I/include
EXTRA_PRE_CFLAGS += -I/include/spl

include /root/workspace_ych/FAST27/scaladfs-thread-model/lustre-sim-fifo/Rules
