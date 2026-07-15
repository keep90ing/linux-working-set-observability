CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra

BIN := idle_workload

all: $(BIN)

$(BIN): src/idle_workload.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BIN)

.PHONY: all clean
