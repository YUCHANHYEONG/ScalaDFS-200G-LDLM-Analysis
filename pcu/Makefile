# SPDX-License-Identifier: GPL-2.0
# Any varying coverage in these files is non-deterministic
# and is generally not a function of system call inputs.
KCOV_INSTRUMENT := n

ifeq ($(CONFIG_KCSAN),y)
KBUILD_CFLAGS += -g -fno-omit-frame-pointer
endif

obj-m += pcu.o

pcu-y += pcu_init.o

pcu-y += update.o sync.o
pcu-$(CONFIG_TREE_SRCU) += spcutree.o
pcu-$(CONFIG_TINY_SRCU) += spcutiny.o
pcu-$(CONFIG_RCU_TORTURE_TEST) += pcutorture.o
pcu-$(CONFIG_RCU_SCALE_TEST) += pcuscale.o
pcu-$(CONFIG_RCU_REF_SCALE_TEST) += refscale.o
pcu-$(CONFIG_TREE_RCU) += tree.o
pcu-$(CONFIG_TINY_RCU) += tiny.o
pcu-$(CONFIG_RCU_NEED_SEGCBLIST) += pcu_segcblist.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
