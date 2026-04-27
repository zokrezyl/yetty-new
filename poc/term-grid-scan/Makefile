CC      ?= cc
CFLAGS  ?= -O3 -std=c11 -Wall -Wextra -Wpedantic -march=native
LDFLAGS ?=

BIN := term-grid-scan

all: $(BIN)

$(BIN): main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all run clean
