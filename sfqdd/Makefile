obj-m += lab2.o

KERNELDIR:=~/msm
PWD:=$(shell pwd)
ARCH=arm
CROSS_COMPILE=arm-eabi-

default:
	$(MAKE) -C $(KERNELDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) modules
clean:
	$(MAKE) -C $(KERNELDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) clean
