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
removes all built binaries.

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
  `menu`, `status`, `debug_info`, `grid`), each a small `.c`/`.h` pair
  built entirely on the public `ctui.h` API.
- `examples_apps/` — real, runnable ctui apps, one subfolder each
  (`examples_apps/<name>/main.c` + an optional local `widgets/`):
  `demo` (the original 3-area header/main/footer layout demonstrating
  resizing and event wiring), `clock` (a ticking clock, driving the
  `CTUI_TICK_EVENT` timer mechanism), `file_browser` (a scrollable,
  navigable directory listing), and `calculator` (a 4-function calculator —
  a right-aligned `CTUI_DISPLAY` readout above a navigable `CTUI_GRID` of
  buttons; the arithmetic itself is a small, ctui-independent state machine
  in its own `calc.h`/`calc.c`, with `main.c` translating between `CTUI_GRID`
  presses and its tokens). An app's local `widgets/` is where new stdlib
  candidates get proven out before graduating to `src/widgets/` once a
  second app needs them.
- `tests/` — headless C tests, run via `make test`. Each one wires up
  real widgets/events like an app's `main()` would, injects input via
  `tools/ctui_test.h`, and asserts against the rendered screen buffer.
- `tools/` — testing infrastructure: `ctui_test.h` (the headless C test
  driver `tests/` builds on) and `pty_harness.py` (drives a binary
  under a real pty for terminal-I/O-layer testing — raw input decoding,
  actual `SIGWINCH`, real ANSI output).

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
