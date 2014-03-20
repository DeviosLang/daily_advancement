obj-m = rb.o
all:
	make -C /lib/modules/`uname -r`/build M=`pwd` 
clean: 
	rm -rf *.o *.ko *.mod.c modules.order Module.symvers
