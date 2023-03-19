# If variables aren't define, define them
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -g -Wall -Werror
LDFLAGS ?= -lpthread -lrt

# Project specific flags
TARGET ?= aesdsocket
SOURCES = aesdsocket.c
INCLUDES = -I.
CFLAGS += -DUSE_AESD_CHAR_DEVICE=1

all:
	$(CC) $(CFLAGS) $(INCLUDES) ${SOURCES} -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(PROGRAM_NAME) *.o
