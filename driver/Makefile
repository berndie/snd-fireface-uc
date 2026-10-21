# SPDX-License-Identifier: GPL-2.0-only
#
# Out-of-tree build of snd-fireface-uc.
#
#   make                 build against the running kernel
#   make KDIR=/path      build against another kernel tree
#   sudo make install    install into /lib/modules/$(uname -r)/updates

snd-fireface-uc-y := ffuc.o ffuc-ctl.o
obj-m += snd-fireface-uc.o

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

install: all
	$(MAKE) -C $(KDIR) M=$(CURDIR) INSTALL_MOD_DIR=updates modules_install
	depmod -a $(KVER)

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all install clean
