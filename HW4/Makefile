CC = gcc
CFLAGS = -D_GNU_SOURCE -Wall -Wextra -std=c11 -pthread
TARGET = analyzer
SRCS = main.c common.c shm.c reader.c dispatcher.c analyzer.c aggregator.c watchdog.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
