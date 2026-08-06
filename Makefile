CC = cc
CFLAGS = -Wall -Wextra -std=c11 -Isrc -g

CORE_SRC = $(wildcard src/*.c src/core/*.c src/widgets/*.c)
CORE_HDR = $(wildcard src/*.h src/core/*.h src/widgets/*.h)

TEST_SRC = $(wildcard tests/*.c)
TEST_BIN = $(patsubst tests/%.c,test-%,$(TEST_SRC))

.DEFAULT_GOAL := ctui-demo

ctui-demo: $(CORE_SRC) $(CORE_HDR) examples_apps/demo/main.c $(wildcard examples_apps/demo/widgets/*)
	$(CC) $(CFLAGS) -o ctui-demo $(CORE_SRC) examples_apps/demo/main.c $(wildcard examples_apps/demo/widgets/*.c)

ctui-hello: $(CORE_SRC) $(CORE_HDR) examples_apps/hello/main.c $(wildcard examples_apps/hello/widgets/*)
	$(CC) $(CFLAGS) -o ctui-hello $(CORE_SRC) examples_apps/hello/main.c $(wildcard examples_apps/hello/widgets/*.c)

ctui-clock: $(CORE_SRC) $(CORE_HDR) examples_apps/clock/main.c $(wildcard examples_apps/clock/widgets/*)
	$(CC) $(CFLAGS) -o ctui-clock $(CORE_SRC) examples_apps/clock/main.c $(wildcard examples_apps/clock/widgets/*.c)

ctui-file_browser: $(CORE_SRC) $(CORE_HDR) examples_apps/file_browser/main.c $(wildcard examples_apps/file_browser/widgets/*)
	$(CC) $(CFLAGS) -o ctui-file_browser $(CORE_SRC) examples_apps/file_browser/main.c $(wildcard examples_apps/file_browser/widgets/*.c)

ctui-calculator: $(CORE_SRC) $(CORE_HDR) examples_apps/calculator/main.c examples_apps/calculator/calc.c examples_apps/calculator/calc.h $(wildcard examples_apps/calculator/widgets/*)
	$(CC) $(CFLAGS) -o ctui-calculator $(CORE_SRC) examples_apps/calculator/main.c examples_apps/calculator/calc.c $(wildcard examples_apps/calculator/widgets/*.c)

ctui-flicker: $(CORE_SRC) $(CORE_HDR) examples_apps/flicker/main.c $(wildcard examples_apps/flicker/widgets/*)
	$(CC) $(CFLAGS) -o ctui-flicker $(CORE_SRC) examples_apps/flicker/main.c $(wildcard examples_apps/flicker/widgets/*.c)

ctui-matrix: $(CORE_SRC) $(CORE_HDR) examples_apps/matrix/main.c $(wildcard examples_apps/matrix/widgets/*)
	$(CC) $(CFLAGS) -o ctui-matrix $(CORE_SRC) examples_apps/matrix/main.c $(wildcard examples_apps/matrix/widgets/*.c)

ctui-player: $(CORE_SRC) $(CORE_HDR) examples_apps/player/main.c $(wildcard examples_apps/player/audio/*) $(wildcard examples_apps/player/decoders/*) $(wildcard examples_apps/player/outputs/*) $(wildcard examples_apps/player/widgets/*)
	$(CC) $(CFLAGS) -o ctui-player $(CORE_SRC) examples_apps/player/main.c $(wildcard examples_apps/player/decoders/*.c) $(wildcard examples_apps/player/outputs/*.c) $(wildcard examples_apps/player/widgets/*.c) -lasound -lm

ctui-kitty_demo: $(CORE_SRC) $(CORE_HDR) examples_apps/kitty_demo/main.c $(wildcard examples_apps/kitty_demo/widgets/*)
	$(CC) $(CFLAGS) -o ctui-kitty_demo $(CORE_SRC) examples_apps/kitty_demo/main.c $(wildcard examples_apps/kitty_demo/widgets/*.c)

examples: ctui-hello ctui-clock ctui-file_browser ctui-calculator ctui-flicker ctui-matrix ctui-player ctui-kitty_demo

all: ctui-demo examples

test-%: $(CORE_SRC) $(CORE_HDR) tests/%.c tools/ctui_test.h
	$(CC) $(CFLAGS) -Itools -o $@ $(CORE_SRC) tests/$*.c

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do echo "-- $$t --"; ./$$t || exit 1; done

coverage: $(CORE_SRC) $(CORE_HDR) $(TEST_SRC) tools/ctui_test.h tools/coverage.sh
	@bash tools/coverage.sh

clean:
	rm -f ctui-demo ctui-hello ctui-clock ctui-file_browser ctui-calculator ctui-flicker ctui-matrix ctui-player ctui-kitty_demo $(TEST_BIN)
	rm -rf coverage

.PHONY: clean examples all test coverage
