CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pthread
TARGET = cargoGTU
OBJS = main.o order_queue.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

main.o: main.c order_queue.h
	$(CC) $(CFLAGS) -c main.c

order_queue.o: order_queue.c order_queue.h
	$(CC) $(CFLAGS) -c order_queue.c

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
