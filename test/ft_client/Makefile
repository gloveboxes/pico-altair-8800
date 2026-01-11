# FT Protocol Test Client
CC = gcc
CFLAGS = -Wall -Wextra -O2

TARGET = ft_test_client
SRC = ft_test_client.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)

.PHONY: all clean
