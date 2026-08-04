CC = cc
CFLAGS = -Wall -Wextra -std=c11 -Isrc -g

CORE_SRC = $(wildcard src/*.c src/widgets/*.c)
CORE_HDR = $(wildcard src/*.h src/widgets/*.h)

TEST_SRC = $(wildcard tests/*.c)
TEST_BIN = $(patsubst tests/%.c,test-%,$(TEST_SRC))

.DEFAULT_GOAL := ctui-demo

ctui-demo: $(CORE_SRC) $(CORE_HDR) examples_apps/demo/main.c $(wildcard examples_apps/demo/widgets/*)
	$(CC) $(CFLAGS) -o ctui-demo $(CORE_SRC) examples_apps/demo/main.c $(wildcard examples_apps/demo/widgets/*.c)

ctui-clock: $(CORE_SRC) $(CORE_HDR) examples_apps/clock/main.c $(wildcard examples_apps/clock/widgets/*)
	$(CC) $(CFLAGS) -o ctui-clock $(CORE_SRC) examples_apps/clock/main.c $(wildcard examples_apps/clock/widgets/*.c)

ctui-file_browser: $(CORE_SRC) $(CORE_HDR) examples_apps/file_browser/main.c $(wildcard examples_apps/file_browser/widgets/*)
	$(CC) $(CFLAGS) -o ctui-file_browser $(CORE_SRC) examples_apps/file_browser/main.c $(wildcard examples_apps/file_browser/widgets/*.c)

ctui-calculator: $(CORE_SRC) $(CORE_HDR) examples_apps/calculator/main.c examples_apps/calculator/calc.c examples_apps/calculator/calc.h $(wildcard examples_apps/calculator/widgets/*)
	$(CC) $(CFLAGS) -o ctui-calculator $(CORE_SRC) examples_apps/calculator/main.c examples_apps/calculator/calc.c $(wildcard examples_apps/calculator/widgets/*.c)

examples: ctui-clock ctui-file_browser ctui-calculator

all: ctui-demo examples

test-%: $(CORE_SRC) $(CORE_HDR) tests/%.c tools/ctui_test.h
	$(CC) $(CFLAGS) -Itools -o $@ $(CORE_SRC) tests/$*.c

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do echo "-- $$t --"; ./$$t || exit 1; done

clean:
	rm -f ctui-demo ctui-clock ctui-file_browser ctui-calculator $(TEST_BIN)

.PHONY: clean examples all test
