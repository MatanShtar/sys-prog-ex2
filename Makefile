CC := gcc
CFLAGS := -Wall -Wextra -std=gnu11 -O2
TARGET := mini_shell
SRC := mini_shell.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)
