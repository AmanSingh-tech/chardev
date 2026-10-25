CC      ?= gcc
CFLAGS  := -Wall -Wextra -std=gnu11 -I../include -pthread
LDFLAGS := -pthread
TARGET  := chardev_test

all: CFLAGS += -O2
all: $(TARGET)

# Debug build: no optimisation, full symbols (useful under gdb and valgrind).
debug: CFLAGS += -O0 -g3
debug: clean $(TARGET)

$(TARGET): chardev_test.c ../include/chardev_ioctl.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all debug clean
