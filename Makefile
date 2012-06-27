obj-m := dragon.o

path := $(shell uname -r)
dir  := $(shell pwd)

all:
	make -C /lib/modules/$(path)/build M=$(dir) modules
clean:
	make -C /lib/modules/$(path)/build M=$(dir) clean
