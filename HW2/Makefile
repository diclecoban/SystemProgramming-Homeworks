CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
TARGET  = procSearch
SRCS    = main.c pattern.c worker.c output.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Header dependencies
main.o:    main.c pattern.h worker.h output.h
pattern.o: pattern.c pattern.h
worker.o:  worker.c worker.h pattern.h
output.o:  output.c output.h

clean:
	rm -f $(OBJS) $(TARGET)
