PROGRAM_NAME = aesdsocket
SRC_FILES = src/main.c src/server.c
CC = gcc
CFLAGS = -Werror -Wall

all:
	$(CROSS_COMPILE)$(CC) $(CFLAGS) -I. ${SRC_FILES} -o $(PROGRAM_NAME)

clean:
	rm -f $(PROGRAM_NAME) *.o
