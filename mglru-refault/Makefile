CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra
LDFLAGS ?= -pthread

BIN := mglru_workload

all: $(BIN)

$(BIN): src/mglru_workload.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BIN)

.PHONY: all clean
