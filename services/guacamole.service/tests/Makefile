# Tests for the guacamole.service management layer.
#
# These compile directly against ../management.c and have NO external
# dependencies (no libguac, no kin.library), so they run anywhere gcc does.

CC = gcc
CFLAGS = -Wall -g -pthread -I..

BIN = test_management

all: run

$(BIN): test_management.c ../management.c ../management.h
	$(CC) $(CFLAGS) -o $@ test_management.c ../management.c

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all run clean
