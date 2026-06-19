# Makefile for Trading System Test Suite
CC       = gcc
CFLAGS   = -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE
LDFLAGS  = -lpthread -lrt
TARGET   = test_trading
SRC      = test_trading.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: $(TARGET)
	@echo "=== Running test suite ==="
	./$(TARGET) $(HOST) $(PORT)

clean:
	rm -f $(TARGET)
