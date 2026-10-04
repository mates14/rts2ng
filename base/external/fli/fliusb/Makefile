obj-m		:= fliusb.o

KDIR		?= /lib/modules/$(shell uname -r)/build
PWD		:= $(shell pwd)

ccflags-y		+= -O2 -Wall

#ccflags-y		+= -DDEBUG	# enable debug messages
#ccflags-y		+= -DASYNCWRITE	# enable asynchronous writes
ccflags-y		+= -DSGREAD	# enable scatter-gather reads

all: module cleanup

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

cleanup:
	rm -f *.o .*.cmd *.mod.c; rm -rf .tmp_versions

clean: cleanup
	rm -f *.ko test
