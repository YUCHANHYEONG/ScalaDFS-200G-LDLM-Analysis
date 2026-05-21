MODULES := osd_ldiskfs
osd_ldiskfs-objs = osd_handler.o osd_oi.o osd_lproc.o osd_iam.o \
		   osd_iam_lfix.o osd_iam_lvar.o osd_io.o osd_compat.o \
		   osd_scrub.o osd_dynlocks.o osd_quota.o osd_quota_fmt.o

osd_ldiskfs-objs += osd_integrity.o

EXTRA_PRE_CFLAGS := -I/root/workspace_ych/FAST27/scaladfs-thread-model/linux-kernel/fs -I/root/workspace_ych/FAST27/scaladfs-thread-model/lustre-sim-fifo -I/root/workspace_ych/FAST27/scaladfs-thread-model/lustre-sim-fifo/ldiskfs

include /root/workspace_ych/FAST27/scaladfs-thread-model/lustre-sim-fifo/Rules
