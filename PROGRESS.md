# ctui — progress notes

A lean & mean widget-based C TUI library. Long-term goal: a personalized
shell to eventually replace plasmashell.

## Status

First substantial C project. Core building blocks exist and work
end-to-end (see `demo.c`) — a menu widget with keyboard nav and a status
widget reading shared state.

## Architecture

- **Rendering**: full redraw per frame today, diffed against a shadow
  buffer (`CTUI_SCREEN.buffer` vs `.cells`) so unchanged cells are
  skipped on flush. Cursor-movement/color escapes are only emitted when
  they actually change. True run-length diffing (skipping ahead over
  runs of unchanged cells) is not implemented yet but the groundwork is
  there.
- **Widgets**: each widget owns `x, y, w, h` + a vtable
  (`init`, `render`, `on_event`). Enables a compositor later, since
  every widget already knows its own screen region.
- **Events**: single global event loop (`ctui_app_run`), one scope
  (`CTUI_EVENT_SCOPE_GLOBAL`) today. Planned: event scoping, custom
  event types, proper routing (currently every widget sees every
  event and decides for itself whether to handle it).
- **Terminal I/O**: raw ANSI/terminfo escapes, termios raw mode,
  alternate screen buffer. Kitty protocol / better input handling is a
  possible future upgrade, not a near-term priority.

## Fixed / addressed

- [x] Include guard renamed from `CTUI` to `CTUI_H` (avoid future
      macro/type name collisions).
- [x] Vtable function pointers changed from empty-paren
      (`int (*init)();`) to explicit `(void)`/typed signatures for
      proper argument type-checking.

## Known issues / deliberately deferred

- **`ctui_screen_flush` buffer sizing is fragile.** `snprintf` returns
  the length it *would* write, not what it actually wrote if the
  destination is too small — `len` isn't guarded against this, so an
  undersized `cap` would silently overrun `out` rather than truncate
  safely. Current 24-bytes/cell budget is fine for realistic terminal
  sizes; no guardrail exists if that assumption ever breaks.
- **No malloc/calloc NULL checks** in `ctui_screen_create`. Accepted
  risk — core is a small char buffer, unlikely to hit OOM in practice.
- **Widgets don't clip to their own `w`/`h`.** Every widget currently
  writes at absolute screen coordinates and trusts itself not to
  overflow into a neighboring widget's region. This is the open
  problem: needs a "proper" compositor step (clipped sub-view of the
  screen buffer per widget, real z-ordering) before adding more than a
  couple of widgets to one screen.

## Next up

- First widgets: border (draw-around-an-area utility) and a simple
  clock.
- Compositor / clipping pass, once enough widgets exist to make the
  overlap problem concrete.
- Event scoping + routing, once more than one widget needs to react
  differently to the same event.
