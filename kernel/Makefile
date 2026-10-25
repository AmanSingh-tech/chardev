# Kbuild part: read by the kernel build system when it recurses into M=.
obj-m := chardev.o
ccflags-y := -I$(src)/../include

# Normal make part: hand off to the kernel's own build tree.
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
