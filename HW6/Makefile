CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pedantic

COMMON = magic_common.o
CLIENT_COMMON = client_common.o

.PHONY: all clean

all: hogwarts wizard professor

hogwarts: hogwarts.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ hogwarts.o $(COMMON)

wizard: wizard.o $(COMMON) $(CLIENT_COMMON)
	$(CC) $(CFLAGS) -o $@ wizard.o $(COMMON) $(CLIENT_COMMON)

professor: professor.o $(COMMON) $(CLIENT_COMMON)
	$(CC) $(CFLAGS) -o $@ professor.o $(COMMON) $(CLIENT_COMMON)

hogwarts.o: hogwarts.c magic_common.h
	$(CC) $(CFLAGS) -c hogwarts.c

wizard.o: wizard.c client_common.h
	$(CC) $(CFLAGS) -c wizard.c

professor.o: professor.c client_common.h
	$(CC) $(CFLAGS) -c professor.c

magic_common.o: magic_common.c magic_common.h
	$(CC) $(CFLAGS) -c magic_common.c

client_common.o: client_common.c client_common.h magic_common.h
	$(CC) $(CFLAGS) -c client_common.c

clean:
	rm -f hogwarts wizard professor *.o
