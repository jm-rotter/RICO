# Out-of-tree build of rico.ko (kbuild).
#
#   make                         build against the running kernel's headers
#   make KDIR=/path/to/linux     build against another kernel tree / headers dir
#   make clean
#
# The module must be built against the SAME kernel the guest boots (vermagic +
# modversions). For the QEMU test the guest boots the host's kernel, so the
# default KDIR is right; for another guest, point KDIR at its build tree.

ifneq ($(KERNELRELEASE),)
# ---- kbuild side ----
obj-m := rico.o
rico-y := rico_drv.o
ccflags-y := -Wall

else
# ---- user side ----
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	@test -f "$(KDIR)/Makefile" || { \
	    echo "error: no kernel build tree at KDIR=$(KDIR)"; \
	    echo "       install the headers package (e.g. linux-headers-$$(uname -r)) or set KDIR="; \
	    exit 1; }
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	-$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f Module.symvers modules.order

.PHONY: all clean
endif
