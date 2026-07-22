CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
KIN_INCLUDE ?= /usr/include

all: guacamole.module

guacamole.module: guacamole.module.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -shared -I$(KIN_INCLUDE) -o $@ $<

clean:
	rm -f guacamole.module

.PHONY: all clean
