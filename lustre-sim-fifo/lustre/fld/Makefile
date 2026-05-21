MODULES := fld
fld-objs := fld_request.o fld_cache.o lproc_fld.o
#fld-objs += fld_handler.o fld_index.o

EXTRA_PRE_CFLAGS := -I/root/workspace_ych/FAST27/scaladfs-thread-model/lustre-sim-fifo/lustre
EXTRA_DIST := $(fld-objs:%.o=%.c) fld_internal.h
EXTRA_DIST += fld_handler.c fld_index.c

include /root/workspace_ych/FAST27/scaladfs-thread-model/lustre-sim-fifo/Rules
