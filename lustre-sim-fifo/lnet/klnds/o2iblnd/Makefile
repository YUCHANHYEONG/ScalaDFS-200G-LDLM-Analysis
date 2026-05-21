MODULES := ko2iblnd
ko2iblnd-objs := o2iblnd.o o2iblnd_cb.o o2iblnd_modparams.o

# Need to make sure that an external OFED source pool overrides
# any in-kernel OFED sources
NOSTDINC_FLAGS +=   -I/root/workspace_ych/FAST27/scaladfs-thread-model/linux-kernel/include -I/root/workspace_ych/FAST27/scaladfs-thread-model/linux-kernel/include/uapi

include /root/workspace_ych/FAST27/scaladfs-thread-model/lustre-sim-fifo/Rules
