CC = gcc
CFLAGS = -Wall -Wextra -O2 -fsanitize=address,undefined -g
INCLUDES = -I./salloc
TARGET = main

SRCS = main.c salloc/main.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJS) $(TARGET)

.PHONY: all clean
