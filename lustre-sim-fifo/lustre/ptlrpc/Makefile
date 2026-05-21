MODULES := ptlrpc
ptlrpc_dir := $(dir $(lastword $(MAKEFILE_LIST)))
LDLM := ../../lustre/ldlm/
TARGET := ../../lustre/target/

ptlrpc_objs := client.o recover.o connection.o niobuf.o pack_generic.o
ptlrpc_objs += events.o ptlrpc_module.o service.o pinger.o
ptlrpc_objs += llog_net.o llog_client.o import.o ptlrpcd.o
ptlrpc_objs += pers.o lproc_ptlrpc.o wiretest.o layout.o
ptlrpc_objs += sec.o sec_ctx.o sec_bulk.o sec_gc.o sec_config.o sec_lproc.o
ptlrpc_objs += sec_null.o sec_plain.o nrs.o nrs_fifo.o nrs_delay.o heap.o
ptlrpc_objs += errno.o batch.o

nrs_server_objs := nrs_crr.o nrs_orr.o nrs_tbf.o

nodemap_objs := nodemap_handler.o nodemap_lproc.o nodemap_range.o
nodemap_objs += nodemap_idmap.o nodemap_rbtree.o nodemap_member.o
nodemap_objs += nodemap_storage.o

-include $(ptlrpc_dir)/../ldlm/Makefile
#-include $(ptlrpc_dir)/../target/Makefile

ptlrpc-objs := $(ptlrpc_objs)
ptlrpc-objs += $(patsubst %,$(LDLM)%,$(ldlm_objs))
#ptlrpc-objs += $(nodemap_objs) $(nrs_server_objs)
#ptlrpc-objs += pack_server.o llog_server.o
#ptlrpc-objs += $(patsubst %,$(TARGET)%,$(target_objs))

#obj-m += gss/

default: all

EXTRA_DIST := $(ptlrpc_objs:.o=.c) ptlrpc_internal.h
EXTRA_DIST += $(nodemap_objs:.o=.c) nodemap_internal.h heap.h
EXTRA_DIST += $(nrs_server_objs:.o=.c)
EXTRA_DIST += pack_server.c
EXTRA_DIST += llog_server.c

EXTRA_PRE_CFLAGS := -I/root/workspace_ych/FAST27/scaladfs-thread-model/lustre-sim-fifo/lustre/ldlm -I/root/workspace_ych/FAST27/scaladfs-thread-model/lustre-sim-fifo/lustre/target

include /root/workspace_ych/FAST27/scaladfs-thread-model/lustre-sim-fifo/Rules
