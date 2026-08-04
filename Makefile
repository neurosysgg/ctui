CC = cc
CFLAGS = -Wall -Wextra -std=c11 -Iinclude -g
SRC = $(wildcard src/*.c)
HDR = $(wildcard src/*.h)
BIN = ctui-demo

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

clean:
	rm -f $(BIN)

.PHONY: clean
