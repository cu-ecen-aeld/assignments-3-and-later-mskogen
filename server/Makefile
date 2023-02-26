PROGRAM_NAME = aesdsocket
SRC_FILES = aesdsocket.c
CC = gcc
CFLAGS = -g -Werror -Wall

all:
	$(CROSS_COMPILE)$(CC) $(CFLAGS) -I. ${SRC_FILES} -o $(PROGRAM_NAME)

clean:
	rm -f $(PROGRAM_NAME) *.o
