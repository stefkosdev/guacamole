# Tests for the guacamole.service management layer.
#
# These compile directly against ../management.c and have NO external
# dependencies (no libguac, no kin.library), so they run anywhere gcc does.

CC = gcc
CFLAGS = -Wall -g -pthread -I..

BIN = test_management
TICKET_BIN = test_tickets

all: run

$(BIN): test_management.c ../management.c ../management.h
	$(CC) $(CFLAGS) -o $@ test_management.c ../management.c

$(TICKET_BIN): test_tickets.c ../tunnel.c ../tunnel.h
	$(CC) $(CFLAGS) -o $@ test_tickets.c ../tunnel.c -lcrypto

run: $(BIN) $(TICKET_BIN)
	./$(BIN)
	./$(TICKET_BIN)

clean:
	rm -f $(BIN) $(TICKET_BIN)

.PHONY: all run clean
