# The core library (`src/core/`)

A map of `src/core/`'s public API, one section per subsystem, plus the
call sequences that tie them together. Each header's own comments are
still the authoritative contract for any one function (this doc won't
repeat them in full and will drift if it tries) — treat this as the
entry point that tells you *which* header to open and *what order*
things get called in, not a replacement for reading `core/widget.h`
before you call `ctui_widget_putc()`. For the *why* behind each design
choice, see `PROGRESS.md`'s Architecture section and `CLAUDE.md`'s
Architecture patterns; for a worked example gluing all of this
together, see the README's `hello` walkthrough.

Nothing in `src/core/` knows about any specific widget — `border`,
`menu`, `label`, etc. are all built on top of this, in
`src/widgets/`, using nothing but the API below.

## Include structure

Apps and widgets `#include "ctui.h"` and nothing else from core —
that one header pulls in every `core/*.h` in dependency order:

```
cell.h → gfx.h → screen.h → compositor.h → widget.h → event.h →
timer.h → util.h → group.h → split.h → app.h → term.h → input.h → log.h
```

That order is also roughly the dependency order below: each
subsystem after `cell.h` builds on the ones before it.

## Life of a frame

This is the sequence `ctui_app_run()` (`core/app.c`) actually drives —
useful to have in one place since it's spread across four functions:

1. **Startup** (once, by the app's `main()`):
   `ctui_init()` → `ctui_app_init()`, which calls `ctui_widget_init()`
   for every top-level widget (running each one's `layout()`, then
   binding its `buf` into the compositor).
2. **Initial render**: `ctui_app_render()` (clears the compositor,
   calls `ctui_widget_dispatch_render()` per widget, blits into the
   screen's frame buffer) → `ctui_screen_flush()` (diffs against the
   previous frame, writes only what changed) → `ctui_widget_flush_gfx()`
   (fires any Kitty-style non-degradable renderer queued this frame —
   always *after* flush, never before).
3. **Loop**: `ctui_input_loop()` blocks for a key (or, with
   `tick_ms > 0`, up to `tick_ms` before emitting a `CTUI_TICK_EVENT`).
   - `CTUI_RESIZE_EVENT` → `ctui_app_resize()` (reallocate
     compositor/screen, re-run every widget's `layout()` + rebind,
     dispatch the event) → render/flush/flush_gfx again.
   - `CTUI_KEY_ESC` → break the loop.
   - anything else → `ctui_handle_event()` (fires registered
     handlers) and `ctui_timer_tick()` (fires due timers); if either
     reports a visible change, render/flush/flush_gfx again.
4. **Teardown**: `ctui_app_free()`, `ctui_screen_free()`,
   `ctui_shutdown()`.

Everything below is one of the pieces in that sequence.

## `cell.h` — `CTUI_CELL`

The unit everything ends up as: one character cell (`ch`, `fg`, `bg`,
plus `color_mode` and, for RGB cells, `fg_r/g/b`/`bg_r/g/b`).
`CTUI_COLOR_MODE_BASIC/256/RGB` says how `fg`/`bg` should be read —
independent of `CTUI_GFX_MODE` (`gfx.h`), which is what the *terminal
session* negotiated, not how one cell is encoded. No functions here,
just the type and the `CTUI_COLOR_*` basic-color enum.

## `gfx.h` — graphics capability negotiation

- `ctui_gfx_detect_caps()` — env-sniffs (`TERM`/`COLORTERM`/
  `KITTY_WINDOW_ID`) into a `CTUI_GFX_MODE` bitmask; `CTUI_GFX_ANSI16`
  is always set.
- `ctui_gfx_kitty_display(row, col, cell_cols, cell_rows, rgba, width,
  height, image_id)` / `ctui_gfx_kitty_delete(image_id)` — transmit/
  remove a Kitty pixel-graphics image directly to stdout, bypassing
  `CTUI_CELL` entirely. Only called from a widget's `gfx_render`
  (see `widget.h` below) — never call these from an ordinary `render()`.

## `screen.h` — `CTUI_SCREEN`

The terminal-facing frame buffer: `cells` (frame being built) vs.
`buffer` (what's currently on the real terminal). Apps rarely touch
this directly except at the two ends of its lifecycle and
`ctui_app_resize()`'s internals:

- `ctui_screen_create(rows, cols)` / `ctui_screen_free(s)`
- `ctui_screen_clear(s)`, `ctui_screen_putc/puts(...)` — low-level,
  mostly superseded by the widget-scoped `ctui_widget_putc/puts()`
  below; still what the compositor ultimately blits into.
- `ctui_screen_flush(s)` — diffs `cells` against `buffer`, writes only
  changed cells to the real terminal. Called once per render pass by
  `ctui_app_run()`, never by widget code.
- `ctui_screen_resize(s, rows, cols)` — reallocates in place, forces a
  full redraw next flush, clears the real terminal outright (a shrink
  could otherwise leave stale content outside the new bounds).

## `compositor.h` — `CTUI_COMPOSITOR`

One allocation (`cells`) backing every widget's buffer — see
`CLAUDE.md`'s Philosophy section for why this shape specifically.

- `ctui_compositor_create/free(...)`
- `ctui_compositor_clear(comp)` — blanks every cell; called once per
  render pass by `ctui_app_render()` *before* any widget draws, so a
  widget that moved/shrank/hid doesn't leave stale content behind.
- `ctui_compositor_blit(comp, screen)` — one `memcpy`-shaped copy onto
  `screen`'s frame; called once per render pass, after every widget
  has drawn.
- `ctui_compositor_resize(comp, rows, cols)` — reallocates in place;
  every widget's `buf` is dangling until rebound via
  `ctui_widget_init()` (`ctui_app_resize()` does this for you).

## `widget.h` — `CTUI_WIDGET`

The widget contract: `x, y, w, h`, `widget_data`, a required
`render()`, an optional `layout()`, plus the Phase 4 (non-degradable
graphics) fields (`supported_gfx_modes`, `gfx_render_mode`,
`gfx_render`) that ordinary widgets never touch, plus `parent`/
`is_active_child` — event-bubbling plumbing (see `event.h` below and
`EVENT_DESIGN.md`) that only `ctui_split_layout()`/`ctui_group_init()`
ever set; every other widget leaves both `NULL`.

- `ctui_widget_make(x, y, w, h, widget_data, render, layout)` — always
  construct a `CTUI_WIDGET` through this, not a struct literal (a
  `_Static_assert` guards against silently zero-filling a field added
  later without updating this function).
- `ctui_widget_init(widget, comp)` — runs `layout()` if set, then binds
  `widget->buf` to its slice of `comp`. Called by `ctui_app_init()`
  once per widget at startup and by `ctui_app_resize()` again on every
  resize — this one call is the entire reflow mechanism, no
  resize-specific code needed elsewhere.
- `ctui_widget_putc/puts(widget, comp, row, col, ch, fg, bg)` — the
  normal way a `render()` draws, in coordinates local to the widget
  (0,0 = its own top-left). Silently rejects (logs `E_WRN`) writes
  outside the widget's bounds or before `ctui_widget_init()` has run.
  `_256`/`_rgb` variants exist for richer color (see `cell.h`'s
  `CTUI_COLOR_MODE_*`) — opt-in per call site, not per widget.
- `ctui_widget_tick_advance(widget)` — per-widget frame counter, for
  debugging/perf; called by `ctui_app_render()` immediately before and
  after each widget's render.
- `ctui_widget_set_gfx_renderer(widget, mode, render)` /
  `ctui_widget_dispatch_render(widget, comp)` /
  `ctui_widget_flush_gfx(comp)` — the Phase 4 non-degradable-protocol
  machinery (Kitty images and whatever comes after). See
  `docs/protocol.md`'s "Non-degradable protocols" section before
  touching these; almost no widget needs them.

## `event.h` — the event registry

addEventListener()-style: widgets register interest in one
`(source, type)` pair instead of implementing a catch-all handler.

- `CTUI_EVENTTYPE` — `CTUI_KEYPRESS_EVENT`, `CTUI_RESIZE_EVENT`,
  `CTUI_TICK_EVENT`, `CTUI_TIMER_EVENT`, `CTUI_VALUE_CHANGED_EVENT`,
  plus unused `CTUI_FOCUS_EVENT`/`CTUI_WIDGET_REDRAW`/`CTUI_DUMMY_EVENT`.
- `CTUI_EVENT_SCOPE` — `CTUI_EVENT_SCOPE_GLOBAL` (default; every
  matching `(source, type)` handler runs, `origin` ignored) or
  `CTUI_EVENT_SCOPE_BUBBLE` (only handlers registered on `ev->origin`
  or one of its ancestors via `CTUI_WIDGET.parent` run, pruned at any
  `CTUI_SPLIT` step where the child isn't currently active — see
  `EVENT_DESIGN.md`). `origin`'s own handlers always run regardless of
  pruning; pruning only gates whether the walk continues past it.
- `ctui_event_register(source, type, widget, handler)` — `source` is a
  plain string convention agreed between emitter and listener
  (`"input"`, `"terminal"`, `"menu"`, ...), *not* derived from widget
  identity — two instances of the same widget kind can't be told apart
  by source alone under `GLOBAL` scope (`CTUI_EVENT_SCOPE_BUBBLE` +
  `.origin = self` fixes this per-registration, opt-in). Requires
  `ctui_app_init()` to have run first (registrations live on `g_app`).
- `ctui_handle_event(ev)` — under `GLOBAL`, fires every handler whose
  `(source, type)` matches `ev`, in registration order; under `BUBBLE`,
  fires only handlers whose `(source, type, widget)` all match a widget
  actually on `ev->origin`'s (pruned) ancestor chain. Returns `1` if any
  handler returned `1` (a visible change occurred → caller should
  re-render). A widget emits its own event by building a `CTUI_EVENT`
  and calling this again from inside a handler — see
  `ctui_menu_handle_keypress()` for the pattern (and `.origin = self`,
  set at all four `CTUI_VALUE_CHANGED_EVENT` emit sites in
  `src/widgets/`, ready for a listener to opt into `BUBBLE`).

## `timer.h` — self-scheduling widgets

A second, independent ticking mechanism alongside `CTUI_TICK_EVENT`,
for a widget that wants its own period instead of sharing the app's
single `tick_ms`.

- `ctui_timer_register(duration_ms, widget, handler)` — an independent
  countdown, never synced to anything else.
- `ctui_timer_register_synchronized(duration_ms, widget, handler)` —
  every registration sharing the same `duration_ms` fires together off
  one shared deadline, instead of drifting apart independently.
- `ctui_timer_tick()` — fires every due timer/group, dispatching a
  `CTUI_TIMER_EVENT` (source `"timer"`) directly to its `(widget,
  handler)` pair, bypassing the `ctui_handle_event()` registry.
  `ctui_app_run()` calls this once per loop iteration; real firing
  resolution is bounded by whatever `tick_ms` the run loop was given.
- `ctui_timer_reset()` — called by `ctui_app_init()`/`ctui_app_free()`;
  apps never call this themselves.

Widget-level glue lives in `src/widgets/periodic.c` (`ctui_periodic_
register()`), not here — see `PROGRESS.md`'s Timers entry.

## `group.h` — `CTUI_GROUP`

Layering: members share one compositor slice (bound from
`members[0]`'s `(x,y)`) and draw into it in order.

- `ctui_group_make(group_id, members, size)`
- `ctui_group_init(group, comp)` — binds every member to the *same*
  slice; members after the first do **not** get a slice from their own
  `(x,y)` — see `CLAUDE.md`'s "Getting group vs. independent wrong is
  a real bug" note before reaching for this. Also sets every member's
  `->parent` to `group->parent` (a plain `CTUI_GROUP` field, `NULL`
  unless the caller sets it — see `EVENT_DESIGN.md`, since a group
  isn't itself wrapped in a `CTUI_WIDGET` the way a split is, so there's
  no `self` to derive this from). Never sets `is_active_child` on a
  member — a group has no active/inactive subset for one to check.
- `ctui_group_render(group, comp)` — calls each member's `render()` in
  order via `ctui_widget_dispatch_render()`; doesn't blit itself.

## `split.h` — `CTUI_SPLIT`

Partitioning: divides one region into disjoint sub-areas.

- `CTUI_SPLIT_V` (stack, divide height), `CTUI_SPLIT_H` (side by side,
  divide width), `CTUI_SPLIT_GRID` (near-square auto grid, row-major,
  purely a function of `count`).
- `ctui_split_layout(self, comp)` — divides `self`'s *current*
  `x/y/w/h` evenly across `children[0..count-1]`, rebinding each via
  `ctui_widget_init()` against the same real compositor (no virtual
  sub-buffer). Assign directly as a widget's `layout()` for a
  fixed-position split, or call at the end of your own `layout()` if
  the split's own geometry is itself dynamic. Also sets each active
  child's `->parent = self` and `self->is_active_child` to a checker
  that reports whether a given child is currently in
  `children[0..count)` — the plumbing `CTUI_EVENT_SCOPE_BUBBLE` walks
  in `event.c`; see `EVENT_DESIGN.md`.
- `ctui_split_render(self, comp)` — renders `children[0..count-1]` in
  order via `ctui_widget_dispatch_render()`; the split draws nothing
  of its own.
- `count` can change at runtime (e.g. from an event handler); the
  split caches its `comp` pointer so such code can call
  `ctui_split_layout(self, split->comp)` immediately instead of
  waiting for the next resize.

## `app.h` — `CTUI_APP`

Owns the compositor and the event-handler registry; ties everything
above into the loop described in "Life of a frame".

- `ctui_app_init(app, widgets, count, rows, cols)` — allocates the
  compositor, binds every top-level widget, validates each one's
  `supported_gfx_modes` against the negotiated graphics mode (hard
  fail, `-1`, only for a widget that opted into a non-degradable
  protocol it didn't get — see `docs/protocol.md`). `0`/`-1`, same
  convention as `ctui_init()`.
- `ctui_app_free(app)` — frees `app->comp` and `app->handlers`.
- `ctui_app_render(app, screen)` / `ctui_app_resize(app, screen, rows,
  cols)` / `ctui_app_run(app, screen, tick_ms)` — see "Life of a
  frame" above for exactly what each does and in what order.

## `term.h` — terminal lifecycle

- `ctui_init(verbosity, mode)` — raw mode, alternate screen buffer,
  graphics negotiation. `mode` is in/out: pass the tier you want, read
  back the tier you actually got (only the `CTUI_GFX_ANSI16` floor is
  a hard failure). Must be called before anything else.
- `ctui_shutdown()` — restores the terminal.
- `ctui_get_termsize(rows, cols)` — current terminal dimensions, for
  the initial `ctui_app_init()`/`ctui_screen_create()` call.

## `input.h` — the blocking read loop

- `ctui_input_loop(ev, tick_ms)` — blocks for one key (raw-mode
  byte/ESC-sequence decoding happens here), or up to `tick_ms` before
  emitting a `CTUI_TICK_EVENT` if nothing arrived. Returns `0` on
  EOF/error. Not usually called directly by app code — `ctui_app_run()`
  is the one caller; reach for this yourself only if you're building a
  custom run loop instead of using `ctui_app_run()`.

## `log.h` — logging

- `ctui_log_init(verbosity)` / `ctui_log_shutdown()` — split out of
  `ctui_init()`/`ctui_shutdown()` specifically so headless callers
  (`tools/ctui_test.h`) can get a working logger without a real tty.
  `ctui_init()` calls `ctui_log_init()` itself — normal apps never
  call it directly.
- `ctui_log(level, str)` / `ctui_logf(level, fmt, ...)` — `level` is
  exactly one of `E_DBG`/`E_WRN`/`E_INF`/`E_ERR` (`src/logger.h`).
  Every non-trivial core operation logs through these — grep
  `ctui.log` (gitignored) when debugging rather than guessing.
- `ctui_tick_advance()` — the global tick counter used as the
  timestamp in most `ctui_logf()` calls.

## `util.h` — layout/encoding helpers

Not tied to any specific widget:

- `ctui_util_center_h(center_str, line, fill)` /
  `ctui_util_truncate_str(str, desired, trunc)` — string-layout helpers
  for building a `render()`'s text before pushing it through
  `ctui_widget_puts()`.
- `ctui_util_rescale_i(value, in_min, in_max, out_min, out_max)` —
  integer linear rescale, clamped, for cell/pixel/color-channel math.
- `CTUI_MARGIN`, `ctui_margin_uniform(n)`, `ctui_util_inset(content,
  outer, margin)` — the "outer draws its full box, content is inset
  inside it" pattern (a border + its content, see `src/widgets/
  border.h`). Call from content's own `layout()`; requires outer's
  `layout()` to have already run (list outer first in `widgets[]`/the
  enclosing split or group).
- `ctui_util_base64_len(len)` / `ctui_util_base64_encode(src, len, dst,
  dst_cap)` — generic base64, first used by the Kitty transmission
  payload in `gfx.c`.

## `ctui_internal.h` — not public

Three `extern` statics shared only between `core/*.c` translation
units (`g_app`, `g_resize_pending`, `g_gfx_mode`). Never included by
`ctui.h`; nothing outside `src/core/` should reference these. Listed
here only so you know they exist and aren't a reason to reach into
`src/core/` from a widget — if you find yourself wanting one of these
from outside `core/`, that's a sign the operation belongs as a proper
public function instead (e.g. add it next to `ctui_get_termsize()` in
`term.h`, not by exposing the static).

## Recipes

**Writing a new widget** — `widget_data` struct + `render()` +
optional `layout()`, built via `ctui_widget_make()`. See the README's
`hello` walkthrough for the minimal end-to-end version, and
`CLAUDE.md`'s "`examples_apps/<name>/` is the widget stdlib's proving
ground" for where a new widget's code should live before it's proven
generic enough for `src/widgets/`.

**Two widgets talking to each other** — never reach into another
widget's `widget_data`. Emitter calls `ctui_handle_event()` with a
`CTUI_VALUE_CHANGED_EVENT` (or a new type) and an agreed `ev_source`
string; listener calls `ctui_event_register()` for that
`(source, type)` and reads only the event payload. See
`ctui_menu_handle_keypress()` for a real emitter and `CLAUDE.md`'s
"Widgets talk through events" section for the rule this is enforcing.

**Laying out a region with sub-panes** — `CTUI_SPLIT` (disjoint
areas, each child its own origin) vs. `CTUI_GROUP` (same origin,
layered) vs. two independent widgets with their own fixed/dynamic
`x`/`y` and no wrapper at all — pick based on the actual relationship,
per `CLAUDE.md`'s "Groups vs. splits vs. independent widgets" section.

**Making a widget redraw on its own schedule** — `src/widgets/
periodic.c`'s `ctui_periodic_register()` wraps `core/timer.h` so you
never call `ctui_timer_register()` directly; see `examples_apps/
flicker` for independent vs. synchronized timers side by side.

**Adding a new graphics protocol tier** — a whole recipe of its own,
see `docs/protocol.md`.

## See also

- `README.md` — build instructions, project layout, the `hello`
  walkthrough.
- `PROGRESS.md` — the running design log: *why* each subsystem looks
  the way it does, known issues, what's deliberately deferred.
- `CLAUDE.md` — naming/style conventions, architecture patterns, and
  the testing workflow (`tools/ctui_test.h` vs. `tools/pty_harness.py`)
  for verifying a change to any of the above.
- `docs/protocol.md` — the recipe for adding a new graphics protocol
  tier, and the deeper write-up on `gfx.h`'s Phase 4 mechanism.
- `EVENT_DESIGN.md` — the deeper write-up on `CTUI_EVENT_SCOPE_BUBBLE`:
  why it exists (the `ctui-mus` case study), the `parent`/
  `is_active_child` mechanism, and the open questions resolved before
  it was implemented.
