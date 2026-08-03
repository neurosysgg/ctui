# ctui

A lean & mean widget-based C TUI library. Long-term goal: a personalized
shell to eventually replace plasmashell.

## Status

Early / first substantial C project. Core building blocks exist and work
end-to-end — a menu widget with keyboard nav and a status widget reading
shared state. See `PROGRESS.md` for the running design log, known issues,
and what's next.

## Build

```sh
make
./ctui-demo
```

Requires a C11 compiler and a real terminal (raw mode + alternate screen
buffer). `make clean` removes the built binary.

## Architecture at a glance

- **Rendering**: full redraw per frame, diffed against a shadow buffer so
  only changed cells hit the terminal.
- **Widgets**: `x, y, w, h` + a small vtable (`init`, `render`,
  `on_event`) per widget.
- **Events**: single global event loop, one scope today; every widget
  sees every event and decides for itself whether to handle it.
- **Terminal I/O**: raw ANSI/terminfo escapes via termios, no external
  dependencies.

Details and rationale live in `PROGRESS.md`.

## License

MIT, see `LICENSE`.
