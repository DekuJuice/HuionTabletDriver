KERNELDIR := /lib/modules/$(shell uname -r)/build
obj-m += huion.o
all:
	make -C $(KERNELDIR) M=$(PWD) modules
clean:
	make -C $(KERNELDIR) M=$(PWD) clean
