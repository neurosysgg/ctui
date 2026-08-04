CC = cc
CFLAGS = -Wall -Wextra -std=c11 -Isrc -g

CORE_SRC = $(wildcard src/*.c src/widgets/*.c)
CORE_HDR = $(wildcard src/*.h src/widgets/*.h)

.DEFAULT_GOAL := ctui-demo

ctui-demo: $(CORE_SRC) $(CORE_HDR) examples_apps/demo/main.c
	$(CC) $(CFLAGS) -o ctui-demo $(CORE_SRC) examples_apps/demo/main.c

ctui-clock: $(CORE_SRC) $(CORE_HDR) examples_apps/clock/main.c $(wildcard examples_apps/clock/widgets/*)
	$(CC) $(CFLAGS) -o ctui-clock $(CORE_SRC) examples_apps/clock/main.c $(wildcard examples_apps/clock/widgets/*.c)

ctui-file_browser: $(CORE_SRC) $(CORE_HDR) examples_apps/file_browser/main.c $(wildcard examples_apps/file_browser/widgets/*)
	$(CC) $(CFLAGS) -o ctui-file_browser $(CORE_SRC) examples_apps/file_browser/main.c $(wildcard examples_apps/file_browser/widgets/*.c)

examples: ctui-clock ctui-file_browser

all: ctui-demo examples

clean:
	rm -f ctui-demo ctui-clock ctui-file_browser

.PHONY: clean examples all
