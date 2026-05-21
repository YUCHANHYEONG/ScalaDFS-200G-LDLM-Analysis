default: all

MODULES := ldiskfs

# copy makefile over to not break patches
backfs_extra := $(wildcard /root/workspace_ych/FAST27/scaladfs-thread-model/linux-kernel/fs/ext4/Makefile)

backfs_headers := $(wildcard /*.h)
linux_headers := $(wildcard /root/workspace_ych/FAST27/scaladfs-thread-model/linux-kernel/include/linux/ext4*.h)
linux_new_headers := htree_lock.h
trace_headers := $(wildcard /root/workspace_ych/FAST27/scaladfs-thread-model/linux-kernel/include/trace/events/ext4*.h)

backfs_sources := $(filter-out %.mod.c %/inode-test.c ,$(wildcard /*.c))

new_sources := mmp.c htree_lock.c critical_encode.h encdata.h
new_headers :=

ldiskfs_patched_sources := $(notdir $(backfs_sources) $(backfs_headers)) $(new_sources) $(new_headers)
ldiskfs_sources := $(ldiskfs_patched_sources)

ldiskfs-objs := $(filter %.o,$(ldiskfs_sources:.c=.o))

EXTRA_PRE_CFLAGS := -I/root/workspace_ych/FAST27/scaladfs-thread-model/linux-kernel/fs -I/root/workspace_ych/FAST27/scaladfs-thread-model/lustre-sim-fifo/ldiskfs

include /root/workspace_ych/FAST27/scaladfs-thread-model/lustre-sim-fifo/Rules
