MODULES := obdecho
obdecho-objs := echo_client.o debug.o
#obdecho-objs += echo.o

EXTRA_DIST = $(obdecho-objs:%.o=%.c) echo_internal.h
EXTRA_DIST += echo.c

include /root/workspace_ych/FAST27/scaladfs-thread-model/lustre-sim-fifo/Rules
