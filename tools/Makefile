CFLAGS = -Wall -Wextra -DLOG_USE_COLOR
objects := $(wildcard *.c)

ifeq ($(ARCH), arm64)
	CC := aarch64-linux-gnu-gcc
else ifeq ($(ARCH), riscv)
	CC := riscv64-linux-gnu-gcc
endif

.PHONY: all clean
all: 
	$(CC) $(CFLAGS) -g -o hvisor $(objects) -I../driver/ -I./includes/ -lpthread

asm:
	$(CC) $(CFLAGS) -S htool.s $(objects) -I../driver/ -I./includes/ -lpthread
clean:
	rm hvisor
	rm *.s