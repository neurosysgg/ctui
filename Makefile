CC = cc
CFLAGS = -Wall -Wextra -std=c11 -Iinclude -g
SRC = src/ctui.c src/demo.c
BIN = ctui-demo

$(BIN): $(SRC) src/ctui.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

clean:
	rm -f $(BIN)

.PHONY: clean
