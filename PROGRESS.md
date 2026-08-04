# ctui — progress notes

A lean & mean widget-based C TUI library. Long-term goal: a personalized
shell to eventually replace plasmashell.

## Status

First substantial C project. Core building blocks exist and work
end-to-end (see `demo.c`) — a 3-area (header/main/footer) layout with a
bordered, centered title; a keyboard-navigable menu whose items are
independently toggleable; a status line that updates itself by
listening for the menu's value-changed event; and a debug-info panel
that splits into view under the menu when its item is toggled on, and
closes again when toggled off. All three areas reflow correctly on
terminal resize.

## Architecture

- **Rendering**: full redraw per frame, diffed against a shadow buffer
  (`CTUI_SCREEN.buffer` vs `.cells`) so unchanged cells are skipped on
  flush. Cursor-movement/color escapes are only emitted when they
  actually change. Widgets draw into a `CTUI_COMPOSITOR` (one shared
  backing buffer, sliced per-widget by `ctui_widget_init()`).
  `ctui_app_render()` clears the whole compositor (`ctui_compositor_clear()`)
  before any widget draws, then blits it onto the screen once per frame
  after every widget has drawn — the clear matters because a cell a
  widget drew to last frame but doesn't touch this frame (it moved,
  shrank, or got hidden) would otherwise keep showing stale content
  forever, since nothing else ever writes there again. True run-length
  diffing (skipping ahead over runs of unchanged cells) is still not
  implemented, but the groundwork is there.
- **Widgets**: `x, y, w, h`, `widget_data`, a required `render()`, and
  an optional `layout()` that recomputes `x/y/w/h` from the
  compositor's current `rows`/`cols`. `layout()` is re-run by
  `ctui_widget_init()` — both at startup and on every resize — so
  "cols/2"-style dynamic geometry stays correct without any
  special-casing. No per-widget `on_event` anymore (see Events).
- **Groups** (`CTUI_GROUP`): an array of widgets that share a single
  compositor slice (bound once, from the first member's `(x,y)`) and
  draw into it in registration order — cheap layering (borders under
  content, etc.) without a real z-order/painter's-algorithm system.
- **Splits** (`CTUI_SPLIT`): a sub-compositor utility, not a themed
  widget (same category as `CTUI_GROUP`, lives in the core). Given a
  mode (`CTUI_SPLIT_V` stacks children top-to-bottom dividing height;
  `CTUI_SPLIT_H` arranges them left-to-right dividing width) and an
  array of children, `ctui_split_layout()` divides self's *current*
  `x/y/w/h` evenly across the first `count` children and rebinds each
  one via `ctui_widget_init()` against the same real compositor self
  is bound to — no virtual sub-buffer needed. Division is even-only for
  now (no per-child weights/ratios). `count` can change at runtime
  (e.g. from an event handler); the split caches its `comp` pointer on
  every layout call specifically so such code can call
  `ctui_split_layout(self, split->comp)` immediately, rather than
  waiting for the next resize to reveal/hide a pane.
- **Events**: a central registry, closer to `addEventListener` than the
  original "every widget sees every event" model. `ctui_event_register
  (source, type, widget, handler)` registers interest in one
  `(source, type)` pair; `ctui_handle_event(ev)` walks the registry and
  fires every matching handler. `source` is a plain string convention
  (`"input"` for keypresses, `"terminal"` for resizes, or whatever a
  widget chooses when emitting its own event, e.g. `"menu"`) — not tied
  to widget identity, so two instances of the same widget kind can't
  currently be told apart by source alone. A widget emits its own event
  by calling `ctui_handle_event()` again from inside a handler (see
  `ctui_menu_handle_keypress()`). `CTUI_EVENT_SCOPE` still only has one
  member (`_GLOBAL`) and isn't used for anything yet. The registry
  itself is a realloc-grown array with linear-scan matching, not a real
  hash map — plenty fast at this scale, and the public API
  (register-by-key / dispatch-by-key) would look identical either way.
- **Resize**: `SIGWINCH` sets a flag (signal-safe: just a
  `sig_atomic_t`), picked up by `ctui_input_loop()` — which relies on
  the interrupted blocking `read()` returning `EINTR` to notice
  promptly rather than waiting for the next keypress — and turned into
  a `CTUI_RESIZE_EVENT`. `ctui_app_resize()` reallocates the compositor
  and screen in place, force-clears the real terminal (a shrink could
  otherwise leave stale content outside the new bounds), re-runs
  `ctui_widget_init()` (and so every widget's `layout()`) for every
  widget, then dispatches the resize event through the registry.
- **Terminal I/O**: raw ANSI/terminfo escapes, termios raw mode,
  alternate screen buffer, `SIGWINCH` handling. Kitty protocol / better
  input handling is a possible future upgrade, not a near-term
  priority.

## Fixed / addressed

- [x] Include guard renamed from `CTUI` to `CTUI_H` (avoid future
      macro/type name collisions).
- [x] Vtable function pointers changed from empty-paren
      (`int (*init)();`) to explicit `(void)`/typed signatures for
      proper argument type-checking.
- [x] Compositor + per-widget clipping landed — widgets write through
      `ctui_widget_putc()`/`ctui_widget_puts()`, which reject writes
      outside a widget's own `w`/`h` or past the compositor's bounds,
      so one widget can no longer scribble into a neighbor's region.
- [x] First reusable widgets, split into `src/widgets/`: `border`
      (filled-block by default, with optional distinct corner glyphs),
      a generalized `label` (centers text both ways), `menu`,
      `status`, and `debug_info` — each its own `.c`/`.h` pair,
      decoupled from the core engine and, after the event-registry
      rework, from each other.
- [x] Two general-purpose string-layout utilities:
      `ctui_util_center_h()` and `ctui_util_truncate_str()`.
- [x] Terminal resize handling: `SIGWINCH` → reallocation → per-widget
      `layout()` re-run → event dispatch, all automatic once a widget
      opts in by providing a `layout()` callback.
- [x] Event system replaced: from "every widget's `on_event()` sees
      every event and decides for itself" to a central
      `(source, type) -> handler` registry.
- [x] `CTUI_SPLIT` sub-compositor utility added, alongside `CTUI_GROUP`.
      Demo's main area now hosts a split with `menu` and `debug_info`
      as children; `menu`'s items are independently toggleable
      (`CTUI_MENU_ITEM.enabled`, styled the same as the selected item),
      and toggling "debug info" opens/closes the split's second pane —
      driven entirely by an event handler reacting to
      `CTUI_VALUE_CHANGED_EVENT_DATA.enabled`, no direct coupling to the
      menu's internals.
- [x] Fixed stale-content bug: a widget that moved, shrank, or got
      hidden between frames (first caught via the split opening/closing)
      would leave its old drawn content behind forever, since nothing
      else ever re-touched those compositor cells. Root cause was
      `ctui_app_render()` never resetting the compositor between
      frames; fixed with `ctui_compositor_clear()`, called once per
      render pass before any widget draws.

## Known issues / deliberately deferred

- **`ctui_screen_flush` buffer sizing is fragile.** `snprintf` returns
  the length it *would* write, not what it actually wrote if the
  destination is too small — `len` isn't guarded against this, so an
  undersized `cap` would silently overrun `out` rather than truncate
  safely. Current 24-bytes/cell budget is fine for realistic terminal
  sizes; no guardrail exists if that assumption ever breaks.
- **No malloc/calloc/realloc NULL checks** in `ctui_screen_create`,
  `ctui_compositor_create`, or the event handler registry's realloc
  growth (`ctui_logf`'s own `malloc` is the one place that *is*
  checked). Accepted risk for now — small allocations, unlikely to hit
  OOM in practice.
- **Event `source` is a flat string namespace, not tied to widget
  identity.** Fine for the demo (one menu, one status), but two
  instances of the same widget kind emitting under the same source
  string can't be told apart by a listener. No per-instance
  source/identity scheme designed yet.
- **`CTUI_EVENT_SCOPE` is vestigial.** Every event is effectively
  global; the enum exists but nothing branches on it.
- **`CTUI_FOCUS_EVENT` and `CTUI_WIDGET_REDRAW` are declared (and
  named, in `ctui_eventtype_name()`) but never emitted.** Reserved
  event types with no producer yet.
- **No handler unregistration.** `ctui_event_register()` has no
  counterpart to remove a registration; fine while the demo's widgets
  live for the whole program, would matter for widgets that come and
  go dynamically.
- **`CTUI_SPLIT` only divides evenly, no per-child weights/ratios.** On
  a short terminal, the demo's 2-pane split (menu + debug_info, each
  wanting 7 rows) gets capped to whatever the main area's inner height
  actually has and split 50/50 — which can clip the menu's last item
  (silently rejected by `ctui_widget_putc()`'s existing bounds check,
  not a crash, just a visual gap). A weighted split would let
  `debug_info`'s smaller 3-line content claim less space than `menu`'s
  7 lines instead of splitting blind down the middle.

## Next up

- A simple clock widget (the `border`/companion piece from the original
  "first widgets" plan — border shipped, clock didn't).
- Weighted/unequal `CTUI_SPLIT` panes, once the even-split limitation
  above actually bites on a real layout.
- Per-instance event identity, if/when a second instance of the same
  widget kind needs to emit distinguishable events.
- True run-length diffing in `ctui_screen_flush` (skip ahead over runs
  of unchanged cells, not just per-cell compare).
