PROGRAM_NAME = aesdsocket
SRC_FILES = aesdsocket.c

# If variables aren't define, define them
ifeq ($(CC),)
	CC = gcc
endif

ifeq ($(CFLAGS),)
	CFLAGS = -g -Werror -Wall
endif

ifeq ($(LDFLAGS),)
	LDFLAGS = -pthread -lrt
endif

all:
	$(CROSS_COMPILE)$(CC) $(CFLAGS) -I. ${SRC_FILES} -o $(PROGRAM_NAME) $(LDFLAGS)

clean:
	rm -f $(PROGRAM_NAME) *.o
