CC=gcc
CFLAGS=-O2 -ggdb -Wall -Werror -Wextra -pedantic -std=c11
LIBDIR?=/usr/lib/$(shell dpkg-architecture -qDEB_HOST_MULTIARCH)/lirc/plugins

all: irgatedrv.so

install: all
	install -m0644 irgatedrv.so $(LIBDIR)/irgatedrv.so

irgatedrv.so: irgatedrv.c serial.c
	$(CC) $(CFLAGS) -shared -fPIC irgatedrv.c serial.c -o $@
