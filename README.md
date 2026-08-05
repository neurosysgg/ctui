# ctui

A lean & mean widget-based C TUI library. Long-term goal: a personalized
shell to eventually replace plasmashell.

## Status

Early / first substantial C project. Core building blocks exist and work
end-to-end — a bordered header, a keyboard-navigable menu whose items
toggle on/off, a status line that updates itself via the event system,
and a debug-info panel that splits into view under the menu when its
item is toggled on (and closes again when toggled off) — all laid out
across three areas that reflow correctly on terminal resize. See
`PROGRESS.md` for the running design log, known issues, and what's next.

## Build

```sh
make          # builds ctui-demo
make all      # builds every example app under examples_apps/
make test     # builds and runs everything under tests/
./ctui-demo
```

Requires a C11 compiler and a real terminal (raw mode + alternate screen
buffer). `make examples` builds just the non-demo apps; `make clean`
removes all built binaries. `ctui-player` additionally links `-lasound`
(ALSA dev headers/lib) — every other target only depends on libc.

## Usage

The smallest complete app is
[`examples_apps/hello/main.c`](examples_apps/hello/main.c) — a full-screen
border with a centered label inside it, quitting on ESC. Build and run it
with:

```sh
make ctui-hello
./ctui-hello
```

Walking through it (full file:
[`examples_apps/hello/main.c`](examples_apps/hello/main.c)):

**[Includes](examples_apps/hello/main.c#L1-L3)** — apps only ever include
the single public `ctui.h` front door, plus one header per widget they use.

```c
#include "ctui.h"
#include "widgets/border.h"
#include "widgets/label.h"
```

**[`layout()` callbacks](examples_apps/hello/main.c#L7-L19)** — each
widget's `layout()` derives its `x/y/w/h` from the compositor's current
`rows`/`cols` instead of a hardcoded size, so it's re-run automatically on
every terminal resize. The label insets by one cell on each side so it
never shares a cell with the border.

```c
static void border_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 0;
  self->y = 0;
  self->w = comp->cols;
  self->h = comp->rows;
}

static void label_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 1;
  self->y = 1;
  self->w = comp->cols - 2;
  self->h = comp->rows - 2;
}
```

**[`ctui_init()`](examples_apps/hello/main.c#L22-L26)** — sets up the
terminal (raw mode, alternate screen buffer) and negotiates a graphics
mode; every app needs this before touching anything else.

```c
CTUI_GFX_MODE gfx_mode = CTUI_GFX_ANSI16;
if (ctui_init(E_INF | E_WRN | E_ERR, &gfx_mode) != 0) {
  fprintf(stderr, "failed to init ctui\n");
  return 1;
}
```

**[Widget data + construction](examples_apps/hello/main.c#L33-L43)** —
each widget is a small `widget_data` struct (`CTUI_BORDER`, `CTUI_LABEL`)
plus a `CTUI_WIDGET` binding it to a `render()` and `layout()` via
`ctui_widget_make()`. Position/size are left `0,0,0,0` here since
`layout()` computes them.

```c
CTUI_BORDER border_style = ctui_border_make(CTUI_COLOR_WHITE);
CTUI_LABEL label_data = {
    .text = "hello, ctui! (ESC to quit)",
    .fg = CTUI_COLOR_CYAN,
    .bg = CTUI_COLOR_DEFAULT,
};

CTUI_WIDGET border = ctui_widget_make(0, 0, 0, 0, &border_style,
                                      ctui_border_render, border_layout);
CTUI_WIDGET label = ctui_widget_make(0, 0, 0, 0, &label_data,
                                     ctui_label_render, label_layout);
```

**[`ctui_app_init()`](examples_apps/hello/main.c#L45-L47)** — allocates
the shared compositor and binds every widget to its slice of it.

```c
CTUI_WIDGET *widgets[] = {&border, &label};
CTUI_APP app;
ctui_app_init(&app, widgets, 2, rows, cols);
```

**[`ctui_app_run()`](examples_apps/hello/main.c#L51)** — the blocking
event loop; renders, flushes to the terminal, and handles input until ESC.
The final `0` argument means "block on input only" — pass a millisecond
interval instead to also wake on a timer (see
[`examples_apps/clock/main.c`](examples_apps/clock/main.c) for a
`CTUI_TICK_EVENT`-driven widget).

```c
ctui_app_run(&app, screen, 0);
```

**[Teardown](examples_apps/hello/main.c#L55-L57)** — free the app, free
the screen, restore the terminal.

```c
ctui_app_free(&app);
ctui_screen_free(screen);
ctui_shutdown();
```

For anything beyond this — events between widgets, splits, groups,
resizing, timers — see `PROGRESS.md` for the current architecture and the
other apps under `examples_apps/` for worked examples of each. Deeper
per-topic docs live under `docs/` as they're written — currently just
`docs/protocol.md` (how to add a new graphics protocol); this README and
`PROGRESS.md` remain the source of truth for everything else.

## Project layout

- `src/ctui.h` — the single public header; apps and widgets only ever
  `#include "ctui.h"`. It's a thin aggregator that pulls in every header
  under `src/core/`, in dependency order.
- `src/core/` — the core engine, one `.c`/`.h` pair per subsystem (screen
  buffer, compositor, widget lifecycle, groups, splits, event registry,
  terminal I/O, string-layout utilities, logging) plus a private
  `ctui_internal.h` for the handful of statics (`g_app`,
  `g_resize_pending`) shared only between core translation units. Has no
  knowledge of any specific widget.
- `src/widgets/` — the built-in widget catalog (`border`, `label`,
  `menu`, `status`, `debug_info`, `grid`, `list`), each a small `.c`/`.h`
  pair built entirely on the public `ctui.h` API.
- `examples_apps/` — real, runnable ctui apps, one subfolder each
  (`examples_apps/<name>/main.c` + an optional local `widgets/`):
  `hello` (the minimal border + label app walked through in Usage above),
  `demo` (the original 3-area header/main/footer layout demonstrating
  resizing and event wiring), `clock` (a ticking clock, driving the
  `CTUI_TICK_EVENT` timer mechanism), `file_browser` (a scrollable,
  navigable directory listing), `calculator` (a 4-function calculator —
  a right-aligned `CTUI_DISPLAY` readout above a navigable `CTUI_GRID` of
  buttons; the arithmetic itself is a small, ctui-independent state machine
  in its own `calc.h`/`calc.c`, with `main.c` translating between `CTUI_GRID`
  presses and its tokens), and `player` (a WAV player with a live VU-meter
  viz, playing through ALSA — see `examples_apps/player/DESIGN.md` for the
  full design notes on its decoder/output/process pipeline). An app's local
  `widgets/` is where new stdlib candidates get proven out before
  graduating to `src/widgets/` once a second app needs them.
- `tests/` — headless C tests, run via `make test`. Each one wires up
  real widgets/events like an app's `main()` would, injects input via
  `tools/ctui_test.h`, and asserts against the rendered screen buffer.
- `tools/` — testing infrastructure: `ctui_test.h` (the headless C test
  driver `tests/` builds on) and `pty_harness.py` (drives a binary
  under a real pty for terminal-I/O-layer testing — raw input decoding,
  actual `SIGWINCH`, real ANSI output).
- `docs/` — per-topic docs, written up once a topic's substantial enough
  to outgrow a `PROGRESS.md` entry. Currently just `protocol.md` (the
  recipe for adding a new graphics protocol tier).

## Architecture at a glance

- **Rendering**: full redraw per frame into a `CTUI_COMPOSITOR` (one
  shared backing buffer, sliced per-widget), diffed against a shadow
  buffer so only changed cells hit the terminal.
- **Widgets**: `x, y, w, h` + `widget_data` + a required `render()` +
  an optional `layout()` that recomputes `x/y/w/h` from the
  compositor's current size — how widgets reflow on resize. No
  `on_event` on the widget itself; see Events below.
- **Groups**: `CTUI_GROUP` lets multiple widgets share one compositor
  slice and draw into it in order, for cheap layering (e.g. a border
  widget drawn first, content on top).
- **Splits**: `CTUI_SPLIT` is a sub-compositor — it divides its own
  `x/y/w/h` evenly among a set of child widgets (`CTUI_SPLIT_V` stacks
  them, `CTUI_SPLIT_H` puts them side by side) and binds each child
  against the same real compositor at its own computed position. The
  active child count can change at runtime (e.g. an event handler
  revealing a second pane), re-partitioning immediately rather than
  waiting for the next resize.
- **Events**: a central registry (`ctui_event_register()` /
  `ctui_handle_event()`), addEventListener()-style — widgets register
  interest in a specific `(source, type)` pair instead of every widget
  seeing every event and deciding for itself. Widgets emit their own
  events (e.g. `menu`'s value-changed) by calling `ctui_handle_event()`
  from within a handler.
- **Resize**: `SIGWINCH` is caught and turned into a `CTUI_RESIZE_EVENT`;
  `ctui_app_resize()` reallocates the compositor/screen, re-runs every
  widget's `layout()`, and dispatches the event to any registered
  listeners.
- **Ticking**: `ctui_app_run(app, screen, tick_ms)` optionally wakes on
  its own — `tick_ms > 0` dispatches a `CTUI_TICK_EVENT` (source
  `"timer"`) whenever no input arrives within that interval, so a widget
  like `clock` can redraw without waiting on a keypress. `tick_ms <= 0`
  is the original blocking-on-input-only behavior.
- **Terminal I/O**: raw ANSI/terminfo escapes via termios, no external
  dependencies.

Details and rationale live in `PROGRESS.md`.

## License

MIT, see `LICENSE`.
