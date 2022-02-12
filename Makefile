CC=gcc
CFLAGS=-O2 -ggdb -Wall -Werror -Wextra -pedantic -std=c11

all: irgatedrv.so

install: all
	install -m0644 irgatedrv.so /usr/lib/x86_64-linux-gnu/lirc/plugins/irgatedrv.so

irgatedrv.so: irgatedrv.c serial.c
	$(CC) $(CFLAGS) -shared -fPIC irgatedrv.c serial.c -o $@
