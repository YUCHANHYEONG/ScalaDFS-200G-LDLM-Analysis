MODULES := mgc
mgc-objs := mgc_request.o lproc_mgc.o
#mgc-objs += mgc_request_server.o

EXTRA_DIST := $(mgc-objs:%.o=%.c) mgc_internal.h mgc_request_server.c

include /root/workspace_ych/FAST27/scaladfs-thread-model/lustre-sim-fifo/Rules
