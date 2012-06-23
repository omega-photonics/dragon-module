obj-m := dragon.o

path := $(shell uname -r)

all:
	make -C /lib/modules/$(path)/build M=/root/dragon-module modules
clean:
	make -C /lib/modules/$(path)/build M=/root/dragon-module clean
