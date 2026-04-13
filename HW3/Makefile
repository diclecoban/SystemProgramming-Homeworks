CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -Wno-deprecated-declarations -std=c11 \
          -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
LDFLAGS = -pthread
TARGET  = hw3
SRCS    = hw3.c shared_memory.c word_carrier.c letter_carrier.c sorting.c elevator.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c hw3.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) output.txt

run: $(TARGET)
	./$(TARGET) -f 4 -w 2 -l 1 -s 2 -c 3 -d 3 -r 2 -i sample_input.txt -o output.txt
