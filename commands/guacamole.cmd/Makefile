CC = gcc
CFLAGS = -Wall -g -I../../libraries/kin
EXTRA_CFLAGS ?=
CFLAGS += $(EXTRA_CFLAGS)
LDFLAGS = -L../../libraries -l:kin.library -Wl,-rpath,\$$ORIGIN/../libraries -ldl -lrt -lpthread
TARGET = guacamole
SOURCES = main.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

../../libraries/kin/kin.library:
	$(MAKE) -C ../../libraries/kin

$(TARGET): $(OBJECTS) | ../../libraries/kin/kin.library
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean
