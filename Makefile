CC = cc
CFLAGS = -Wall -Wextra -std=c11 -Isrc -g
LDLIBS = -lm

CORE_SRC = $(wildcard src/*.c src/core/*.c src/widgets/*.c src/json/*.c src/look/*.c src/query/*.c src/node/*.c src/link/*.c)
CORE_HDR = $(wildcard src/*.h src/core/*.h src/widgets/*.h src/json/*.h src/look/*.h src/query/*.h src/node/*.h src/link/*.h)

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

# src/vk/: the Vulkan backend (VK_DESIGN.md), optional like ctui-player's
# ALSA: built where Vulkan's headers and glslc are, its shaders by glslc
# into C arrays it includes. Its test, tests/vk/, is outside tests/*.c
# so a machine without Vulkan builds every other one; without a device
# it skips itself
HASH := \#
VK_OK := $(shell command -v glslc >/dev/null && echo '$(HASH)include <vulkan/vulkan.h>' | \
  $(CC) -E -x c - >/dev/null 2>&1 && echo y)
VK_LIBS := $(shell pkg-config --libs vulkan 2>/dev/null || echo -lvulkan)
VK_SRC = src/vk/vk.c src/vk/ttf.c
# what presents it to Wayland surfaces (present.h, host.h): src/wl/ too
VK_WL_SRC = src/vk/present.c src/vk/host.c
VK_SPV = $(patsubst %.comp,%.spv.h,$(wildcard src/vk/shaders/*.comp)) \
  src/vk/shaders/cells_direct.spv.h
src/vk/shaders/%.spv.h: src/vk/shaders/%.comp
	glslc --target-env=vulkan1.0 -O -mfmt=c -o $@ $<
# the painter into a storage image (vk.h ctui_vk_set_direct())
src/vk/shaders/cells_direct.spv.h: src/vk/shaders/cells.comp
	glslc --target-env=vulkan1.0 -O -mfmt=c -DDIRECT -o $@ $<
# the built-in font as a C array (xxd, made const: read-only data)
VK_FONT = src/vk/fonts/JetBrainsMono-Regular.ttf.h
$(VK_FONT): src/vk/fonts/JetBrainsMono-Regular.ttf
	xxd -i -n ctui_vk_font_ttf $< | sed 's/^unsigned/static const unsigned/' > $@
test-vk_test: $(CORE_SRC) $(CORE_HDR) $(VK_SRC) src/vk/vk.h src/vk/ttf.h $(VK_SPV) $(VK_FONT) tests/vk/vk_test.c tools/ctui_test.h
	$(CC) $(CFLAGS) -Itools -o $@ $(CORE_SRC) $(VK_SRC) tests/vk/vk_test.c $(LDLIBS) $(VK_LIBS)

# src/wl/: a Wayland window (src/wl/wl.h), optional the same way: built
# where wayland-client, its scanner and protocols, and xkbcommon are. The
# protocols' glue is generated under wl-gen/ (gitignored); a consumer
# generates its own. Its test never opens a window
WL_PKGS = wayland-client wayland-protocols xkbcommon
WL_OK := $(shell command -v wayland-scanner >/dev/null && pkg-config --exists $(WL_PKGS) && echo y)
WL_PROTOCOLS := $(if $(WL_OK),$(shell pkg-config --variable=pkgdatadir wayland-protocols))
WL_XML = $(addprefix $(WL_PROTOCOLS)/,stable/xdg-shell/xdg-shell.xml \
  stable/viewporter/viewporter.xml staging/fractional-scale/fractional-scale-v1.xml \
  staging/cursor-shape/cursor-shape-v1.xml stable/tablet/tablet-v2.xml \
  stable/presentation-time/presentation-time.xml)
WL_NAMES = $(basename $(notdir $(WL_XML)))
WL_GEN_H = $(patsubst %,wl-gen/%-client-protocol.h,$(WL_NAMES))
WL_GEN_C = $(patsubst %,wl-gen/%-protocol.c,$(WL_NAMES))
# src/wl/layer.c's on top (wlr-layer-shell isn't in wayland-protocols:
# ctui's copy, river's upstream one)
WL_LAYER_XML = protocols/wlr-layer-shell-unstable-v1.xml
WL_LAYER_GEN_H = wl-gen/wlr-layer-shell-unstable-v1-client-protocol.h
WL_LAYER_GEN_C = wl-gen/wlr-layer-shell-unstable-v1-protocol.c
vpath %.xml $(sort $(dir $(WL_XML))) protocols
wl-gen/%-client-protocol.h: %.xml
	@mkdir -p wl-gen
	wayland-scanner client-header $< $@
wl-gen/%-protocol.c: %.xml
	@mkdir -p wl-gen
	wayland-scanner private-code $< $@
test-wl_test: $(CORE_SRC) $(CORE_HDR) src/wl/wl.c src/wl/dnd.c src/wl/wl.h src/wl/wl_internal.h $(WL_GEN_H) $(WL_GEN_C) tests/wl/wl_test.c tools/ctui_test.h
	$(CC) $(CFLAGS) -Itools -Iwl-gen -o $@ $(CORE_SRC) src/wl/wl.c src/wl/dnd.c $(WL_GEN_C) tests/wl/wl_test.c $(LDLIBS) $(shell pkg-config --libs wayland-client xkbcommon)

test-wl_layer_test: $(CORE_SRC) $(CORE_HDR) src/wl/wl.c src/wl/dnd.c src/wl/wl.h src/wl/wl_internal.h src/wl/layer.c src/wl/layer.h $(WL_GEN_H) $(WL_GEN_C) $(WL_LAYER_GEN_H) $(WL_LAYER_GEN_C) tests/wl/layer_test.c tools/ctui_test.h
	$(CC) $(CFLAGS) -Itools -Iwl-gen -o $@ $(CORE_SRC) src/wl/wl.c src/wl/layer.c src/wl/dnd.c $(WL_GEN_C) $(WL_LAYER_GEN_C) tests/wl/layer_test.c $(LDLIBS) $(shell pkg-config --libs wayland-client xkbcommon)

# the presenters (src/vk/present.c, host.c) on layer surfaces and a
# window, in a headless sway on lavapipe (skipped without either)
test-present_test: $(CORE_SRC) $(CORE_HDR) $(VK_SRC) $(VK_WL_SRC) $(wildcard src/vk/*.h) $(VK_SPV) $(VK_FONT) src/wl/wl.c src/wl/layer.c src/wl/dnd.c $(wildcard src/wl/*.h) $(WL_GEN_H) $(WL_GEN_C) $(WL_LAYER_GEN_H) $(WL_LAYER_GEN_C) tests/vk/present_test.c tools/ctui_test.h
	$(CC) $(CFLAGS) -Itools -Iwl-gen -o $@ $(CORE_SRC) $(VK_SRC) $(VK_WL_SRC) src/wl/wl.c src/wl/layer.c src/wl/dnd.c $(WL_GEN_C) $(WL_LAYER_GEN_C) tests/vk/present_test.c $(LDLIBS) $(VK_LIBS) $(shell pkg-config --libs wayland-client xkbcommon)

# replays recorded apps' output through flush: bytes, time, the same cells
flushbench: $(CORE_SRC) $(CORE_HDR) tools/harness/vt.c tools/harness/vt.h tools/flushbench.c
	$(CC) $(CFLAGS) -O2 -Itools -o $@ $(CORE_SRC) tools/harness/vt.c tools/flushbench.c $(LDLIBS)

# src/query/ is libc only (ctui-mus's headless core links it): it must
# compile with nothing of ctui's on the include path
query-alone:
	@$(CC) -Wall -Wextra -std=c11 -fsyntax-only $(wildcard src/query/*.c) || \
		{ echo "src/query/ reaches outside itself: it must stay libc only"; exit 1; }

VKWL_TEST = $(if $(VK_OK),$(if $(WL_OK),test-present_test))
test: $(TEST_BIN) query-alone $(if $(VK_OK),test-vk_test) $(if $(WL_OK),test-wl_test test-wl_layer_test) $(VKWL_TEST)
	@for t in $(TEST_BIN) $(if $(VK_OK),test-vk_test) $(if $(WL_OK),test-wl_test test-wl_layer_test) $(VKWL_TEST); do echo "-- $$t --"; ./$$t || exit 1; done
	@$(if $(VK_OK),,echo "test-vk_test: no Vulkan headers or glslc: skipped")
	@$(if $(WL_OK),,echo "test-wl_test: no wayland-client, its scanner and protocols, or xkbcommon: skipped")

coverage: $(CORE_SRC) $(CORE_HDR) $(TEST_SRC) tools/ctui_test.h tools/coverage.sh
	@bash tools/coverage.sh

clean:
	rm -f ctui-demo ctui-demo-advanced ctui-gallery ctui-hello ctui-clock ctui-file_browser ctui-calculator ctui-flicker ctui-matrix ctui-player ctui-kitty_demo $(TEST_BIN) test-vk_test test-wl_test test-wl_layer_test test-present_test $(VK_SPV) $(VK_FONT)
	rm -rf coverage wl-gen

.PHONY: query-alone clean examples all test coverage
