CC = cc
CFLAGS = -Wall -Wextra -std=c11 -Isrc -g
LDLIBS = -lm

CORE_SRC = $(wildcard src/*.c src/core/*.c src/widgets/*.c src/json/*.c src/look/*.c src/query/*.c)
CORE_HDR = $(wildcard src/*.h src/core/*.h src/widgets/*.h src/json/*.h src/look/*.h src/query/*.h)

TEST_SRC = $(wildcard tests/*.c)
TEST_BIN = $(patsubst tests/%.c,test-%,$(TEST_SRC))

.DEFAULT_GOAL := ctui-demo

ctui-demo: $(CORE_SRC) $(CORE_HDR) examples_apps/demo/main.c $(wildcard examples_apps/demo/widgets/*)
	$(CC) $(CFLAGS) -o ctui-demo $(CORE_SRC) examples_apps/demo/main.c $(wildcard examples_apps/demo/widgets/*.c) $(LDLIBS)

ctui-demo-advanced: $(CORE_SRC) $(CORE_HDR) examples_apps/demo-advanced/main.c $(wildcard examples_apps/demo-advanced/widgets/*)
	$(CC) $(CFLAGS) -o ctui-demo-advanced $(CORE_SRC) examples_apps/demo-advanced/main.c $(wildcard examples_apps/demo-advanced/widgets/*.c) $(LDLIBS)

ctui-hello: $(CORE_SRC) $(CORE_HDR) examples_apps/hello/main.c $(wildcard examples_apps/hello/widgets/*)
	$(CC) $(CFLAGS) -o ctui-hello $(CORE_SRC) examples_apps/hello/main.c $(wildcard examples_apps/hello/widgets/*.c) $(LDLIBS)

ctui-clock: $(CORE_SRC) $(CORE_HDR) examples_apps/clock/main.c $(wildcard examples_apps/clock/widgets/*)
	$(CC) $(CFLAGS) -o ctui-clock $(CORE_SRC) examples_apps/clock/main.c $(wildcard examples_apps/clock/widgets/*.c) $(LDLIBS)

ctui-file_browser: $(CORE_SRC) $(CORE_HDR) examples_apps/file_browser/main.c $(wildcard examples_apps/file_browser/widgets/*)
	$(CC) $(CFLAGS) -o ctui-file_browser $(CORE_SRC) examples_apps/file_browser/main.c $(wildcard examples_apps/file_browser/widgets/*.c) $(LDLIBS)

ctui-calculator: $(CORE_SRC) $(CORE_HDR) examples_apps/calculator/main.c examples_apps/calculator/calc.c examples_apps/calculator/calc.h $(wildcard examples_apps/calculator/widgets/*)
	$(CC) $(CFLAGS) -o ctui-calculator $(CORE_SRC) examples_apps/calculator/main.c examples_apps/calculator/calc.c $(wildcard examples_apps/calculator/widgets/*.c) $(LDLIBS)

ctui-flicker: $(CORE_SRC) $(CORE_HDR) examples_apps/flicker/main.c $(wildcard examples_apps/flicker/widgets/*)
	$(CC) $(CFLAGS) -o ctui-flicker $(CORE_SRC) examples_apps/flicker/main.c $(wildcard examples_apps/flicker/widgets/*.c) $(LDLIBS)

ctui-matrix: $(CORE_SRC) $(CORE_HDR) examples_apps/matrix/main.c $(wildcard examples_apps/matrix/widgets/*)
	$(CC) $(CFLAGS) -o ctui-matrix $(CORE_SRC) examples_apps/matrix/main.c $(wildcard examples_apps/matrix/widgets/*.c) $(LDLIBS)

ctui-player: $(CORE_SRC) $(CORE_HDR) examples_apps/player/main.c $(wildcard examples_apps/player/audio/*) $(wildcard examples_apps/player/decoders/*) $(wildcard examples_apps/player/outputs/*) $(wildcard examples_apps/player/widgets/*)
	$(CC) $(CFLAGS) -o ctui-player $(CORE_SRC) examples_apps/player/main.c $(wildcard examples_apps/player/decoders/*.c) $(wildcard examples_apps/player/outputs/*.c) $(wildcard examples_apps/player/widgets/*.c) -lasound $(LDLIBS)

ctui-kitty_demo: $(CORE_SRC) $(CORE_HDR) examples_apps/kitty_demo/main.c $(wildcard examples_apps/kitty_demo/widgets/*)
	$(CC) $(CFLAGS) -o ctui-kitty_demo $(CORE_SRC) examples_apps/kitty_demo/main.c $(wildcard examples_apps/kitty_demo/widgets/*.c) $(LDLIBS)

ctui-gallery: $(CORE_SRC) $(CORE_HDR) examples_apps/gallery/main.c $(wildcard examples_apps/gallery/widgets/*)
	$(CC) $(CFLAGS) -o ctui-gallery $(CORE_SRC) examples_apps/gallery/main.c $(wildcard examples_apps/gallery/widgets/*.c) $(LDLIBS)

examples: ctui-hello ctui-clock ctui-file_browser ctui-calculator ctui-flicker ctui-matrix ctui-player ctui-kitty_demo ctui-demo-advanced ctui-gallery

all: ctui-demo examples

test-%: $(CORE_SRC) $(CORE_HDR) tests/%.c tools/ctui_test.h
	$(CC) $(CFLAGS) -Itools -o $@ $(CORE_SRC) tests/$*.c $(LDLIBS)

# tools/harness/: the pty harness for the apps' screens (not in any app)
HARNESS_SRC = $(wildcard tools/harness/*.c)
test-harness_test: $(CORE_SRC) $(CORE_HDR) $(HARNESS_SRC) $(wildcard tools/harness/*.h) tests/harness_test.c tools/ctui_test.h
	$(CC) $(CFLAGS) -Itools -Itools/harness -o $@ $(CORE_SRC) $(HARNESS_SRC) tests/harness_test.c $(LDLIBS)

# src/query/ is libc only (ctui-mus's headless core links it): it must
# compile with nothing of ctui's on the include path
query-alone:
	@$(CC) -Wall -Wextra -std=c11 -fsyntax-only $(wildcard src/query/*.c) || \
		{ echo "src/query/ reaches outside itself: it must stay libc only"; exit 1; }

test: $(TEST_BIN) query-alone
	@for t in $(TEST_BIN); do echo "-- $$t --"; ./$$t || exit 1; done

coverage: $(CORE_SRC) $(CORE_HDR) $(TEST_SRC) tools/ctui_test.h tools/coverage.sh
	@bash tools/coverage.sh

clean:
	rm -f ctui-demo ctui-demo-advanced ctui-gallery ctui-hello ctui-clock ctui-file_browser ctui-calculator ctui-flicker ctui-matrix ctui-player ctui-kitty_demo $(TEST_BIN)
	rm -rf coverage

.PHONY: query-alone clean examples all test coverage
