#you can export kernel dir like this, on your shell
#export KERNELDIR=/lib/modules/`uname -r`/build
obj-m = hello.o
all:
#	make -C /lib/modules/`uname -r`/build M=`pwd`
	make -C $(KERNELDIR) M=`pwd`
clean:
	rm -rf *.o *.ko *.mod.c modules.order Module.* 
