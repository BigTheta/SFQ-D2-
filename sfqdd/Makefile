
obj-m += sfqd.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

install:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	cp dmcache.ko bin/

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
