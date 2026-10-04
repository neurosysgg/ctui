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
cell.h → utf8.h → gfx.h → screen.h → compositor.h → widget.h →
event.h → timer.h → io.h → util.h → group.h → split.h → app.h →
term.h → input.h → log.h
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
3. **Loop**: `ctui_input_loop()` sleeps in one `select()` over stdin,
   every `ctui_io_watch()`ed fd, the next timer deadline and (with
   `tick_ms > 0`) the tick deadline, and returns whichever fires
   first as one event.
   - `CTUI_RESIZE_EVENT` → `ctui_app_resize()` (reallocate
     compositor/screen, re-run every widget's `layout()` + rebind,
     dispatch the event) → render/flush/flush_gfx again.
   - `CTUI_KEY_ESC` → break the loop, unless the app set
     `quit_on_esc = 0` (then it's an ordinary key).
   - `CTUI_IO_EVENT` → `ctui_io_dispatch()` (the watch's own handler).
   - `CTUI_TIMER_EVENT` (a deadline wake) → nothing beyond the
     `ctui_timer_tick()` below.
   - anything else (keys, mouse, ticks) → `ctui_handle_event()` (fires
     registered handlers).
   - then always `ctui_timer_tick()` (fires due timers); if anything
     reported a visible change, render/flush/flush_gfx again.
   - `ctui_app_quit()` from any handler ends the loop after that
     iteration.
4. **Teardown**: `ctui_app_free()`, `ctui_screen_free()`,
   `ctui_shutdown()`.

Everything below is one of the pieces in that sequence.

## `cell.h` — `CTUI_CELL`

The unit everything ends up as: one character cell (`ch`, `fg`, `bg`,
plus `color_mode` and, for RGB cells, `fg_r/g/b`/`bg_r/g/b`). `ch` is
one Unicode codepoint (`uint32_t`); a width-2 glyph (CJK, emoji) takes
its own cell plus a `CTUI_CELL_CONT` cell to its right, which
`ctui_screen_flush()` skips since the terminal already advanced past
it.
`CTUI_COLOR_MODE_BASIC/256/RGB/RGB_FG` says how `fg`/`bg` should be
read (`RGB_FG`: a 24-bit fg over a basic `bg`); `attr` holds
`CTUI_ATTR_BOLD/DIM/ITALIC/UNDERLINE/STRIKE` bits (0 = plain, what every
zero-filled cell is), sent by the flush as SGR in the same escape as the
colours; `kitty_row` is the
image row of a Kitty placeholder cell (see `widget.h`) —
independent of `CTUI_GFX_MODE` (`gfx.h`), which is what the *terminal
session* negotiated, not how one cell is encoded. No functions here,
just the type and the `CTUI_COLOR_*` basic-color enum.

## `utf8.h` — codepoints and column widths

- `ctui_utf8_decode(s, &cp)` / `ctui_utf8_encode(cp, out)` — one
  codepoint at a time; malformed input decodes as U+FFFD consuming one
  byte, so a walk always progresses.
- `ctui_utf8_cpwidth(cp)` — one codepoint's terminal columns (0, 1,
  2) as kitty draws it: a table generated from the Unicode data with
  kitty's rules (`core/utf8_props.h`, `tools/gen_utf8_props.py`),
  independent of libc and locale.
- `ctui_utf8_cluster(s, n, &ch, &width)` — one grapheme cluster (UAX
  #29, the way kitty segments: é as e + U+0301, a flag, 👍🏽, a ZWJ
  sequence, Hangul jamo) and the columns kitty gives it (VS16 widens a
  text-style emoji, VS15 narrows, Thai SARA AM widens). `ch` is the
  cell value: the codepoint itself, or an interned `CTUI_CELL_CLUSTER`
  (`cell.h`; `ctui_cell_cluster()` gives its codepoints back,
  `ctui_cell_width()` / `ctui_cell_encode()` work on either). puts,
  `ctui_utf8_width(s)` and `ctui_utf8_prefix()` go cluster by cluster;
  zero-width codepoints with nothing to join are dropped (a leading
  mark, a ZWSP), control characters are clusters of their own.
  `tools/check_widths.py` compares all of it with a running kitty
  (every codepoint, GraphemeBreakTest.txt, random mixes; dev-time).
  Measure strings with `ctui_utf8_width()`, never `strlen()` or a sum of
  `ctui_utf8_cpwidth()`, before laying them out.
- `ctui_utf8_prefix(s, cols, &width)` — longest byte prefix fitting in
  `cols` columns without splitting a glyph.
- `ctui_cell_set_ch(...)` — core-internal: writes a glyph while
  keeping wide lead/`CTUI_CELL_CONT` pairs consistent (overwriting
  either half blanks the other). Widgets go through
  `ctui_widget_putc/puts()` instead.

## `gfx.h` — graphics capability negotiation

- `ctui_gfx_detect_caps()` — env-sniffs (`TERM`/`COLORTERM`/
  `KITTY_WINDOW_ID`) into a `CTUI_GFX_MODE` bitmask; `CTUI_GFX_ANSI16`
  is always set.
- `ctui_gfx_kitty_display(row, col, cell_cols, cell_rows, rgba, width,
  height, image_id)` / `ctui_gfx_kitty_delete(image_id)` — transmit/
  remove a Kitty pixel-graphics image directly to stdout, bypassing
  `CTUI_CELL` entirely. Only called from a widget's `gfx_render`
  (see `widget.h` below) — never call these from an ordinary `render()`.
- `ctui_gfx_kitty_image_new(image_id)` / `_begin(img, w, h, keep)` /
  `_commit(img, row, col, cell_cols, cell_rows, z)` / `_free(img)` — a
  Kitty image painted in place: `begin()` hands out an RGBA buffer that
  *is* a mapped shm object (64-byte aligned), `commit()` hard-links it
  under a fresh name and sends that name (`t=s`, raw, never deflated):
  no copy, no per-frame `shm_open`/`ftruncate`/`mmap`. Up to
  `CTUI_GFX_KITTY_IMAGE_BUFS` (3) buffers rotate; one is free again once
  the terminal has unlinked its frame name (kitty does that after reading
  it all), or after `CTUI_GFX_KITTY_IMAGE_STALE_MS`. `begin()` returns
  NULL while all are unread: skip the frame. `keep` gives the last
  committed frame back (its own buffer when free, else a copy) for
  painters that only redraw what changed. Without `t=s` (or when
  `link()` fails) it's one heap buffer sent through
  `ctui_gfx_kitty_display()`. `commit()` only from a `gfx_render`, like
  `ctui_gfx_kitty_display()`.
- `ctui_gfx_kitty_place_file(image_id, path, cols, rows)` — the other
  way to show a Kitty image: the terminal loads a PNG from `path` itself
  (`f=100,t=f`) as a *virtual placement* (`U=1`), shown wherever cells
  hold `CTUI_GFX_KITTY_PLACEHOLDER` (U+10EEEE) with the id as their fg —
  see `ctui_widget_put_kitty_placeholder()` below. Unlike
  `ctui_gfx_kitty_display()` this is safe from a plain `render()`: the
  image *is* text cells, so it moves, clips and clears with the rest of
  the widget, and one widget can mix icons and text. Batched like every
  Kitty escape; ids up to 24 bits, paths up to 2048 bytes (kitty's
  limit).

- `ctui_gfx_kitty_frame_file(image_id, path, gap_ms)` /
  `ctui_gfx_kitty_animate(image_id, first_gap_ms)` — an animation of an
  image `ctui_gfx_kitty_place_file()` placed: each further frame a PNG of
  the same size by path (`a=f`, shown `gap_ms`; 0 = kitty's default),
  then the frames run in a loop (`a=a`, frame 1 shown `first_gap_ms`).
  Kitty plays them itself: nothing redraws, the placeholders stay.
  Re-placing the id drops the frames.

- `ctui_gfx_kitty_put(image_id, placement_id, row, col, cols, rows, z)`
  / `ctui_gfx_kitty_unput(image_id, placement_id)` — a placement of an
  image the terminal already holds (one `ctui_gfx_kitty_place_file()`
  loaded, say) at a cell, scaled to `cols` x `rows`, at z-index `z`
  (`a=p`; the same pair again moves it), and taking it away (`d=i`, the
  image kept). Not a cell: below 0 text draws over it, and below
  `CTUI_GFX_KITTY_Z_UNDER_BG` only cells on the default background show
  it -- a bevel under a label, which anything painted over hides. The
  caller tracks what it put and takes away what moved or went. Batched,
  so safe from a plain `render()`.

Every kitty escape these send is built by `kitty.h` below.

## `kitty.h` — kitty's escape codes, built so they parse

- `CTUI_KITTY_GFX` — one field per key of kitty's graphics command
  (`a`, `t`, `f`, `i`, `s`, `v`, `z`, ...: the field is named after its
  key); 0 = not sent, which is kitty's default for every key.
- `ctui_kitty_gfx_check(g, payload, n)` — NULL when kitty accepts the
  command with that payload (pixels or compressed data for `t=d`, the
  file/shm name for `t=f/t/s`; `payload` may be NULL where only the total
  size matters), else kitty's own refusal as it would reply, code first:
  `"EINVAL: Filename too long"`, `"ENODATA: Insufficient image data"`, an
  id and a number together, a side past 10000 px, a shm name without
  `/`, a virtual placement with a parent, ... What only the terminal's
  state decides (an unknown id, a full cache, file permissions) passes.
- `ctui_kitty_gfx_build(g, payload, n, out, cap, &err)` — the whole
  escape (checked, keys in kitty's order, the payload base64'd), or 0
  with the refusal in `err` (`ENOBUFS`/`E2BIG` when it doesn't fit the
  buffer / kitty's 256 KiB escape limit).
- `ctui_kitty_gfx_head(g, has_payload, out, cap)` — just `ESC _ G` and
  the keys (plus `;`), for a chunked transmission that slices one base64
  string: check the whole command first, then the first chunk carries
  every key and `m=1`, the rest only `m`.
- `CTUI_KITTY_DND` / `ctui_kitty_dnd_check()` / `ctui_kitty_dnd_build()`
  — the same for drag and drop (OSC 72): its keys, `:`-separated, the
  payload sent as is (no ESC or BEL in it), the thumbnail rules.
- `tools/check_kitty_protocol.py` (dev-time) diffs the key tables in
  `core/kitty.c` against kitty's `gen/apc_parsers.py` (a protocol change
  in a new kitty shows as a diff) and runs builder output -- random valid
  commands and every refusal -- through kitty's own parser: accepted ones
  answer OK, refused ones fail in kitty with the same code.

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
- `ctui_screen_set_sink(s, sink, ctx)` — frames go to
  `sink->frame(ctx, s)` instead of the terminal (an OLED, a remote
  viewer): called only when a cell changed, with `s->cells` the new frame
  and `s->buffer` the last one the sink took (all `'\0'` after a create,
  resize or set: draw everything); `0` = taken, `-1` = hand it over again
  next flush. A sink screen never writes the terminal (resize included);
  kitty graphics stay terminal-only. `NULL` goes back to the terminal
  with a full redraw.

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
  (0,0 = its own top-left). `ch` is a codepoint (char literals work
  for ASCII) and `puts` takes UTF-8, advancing by column width, not
  bytes. A wide glyph that wouldn't fit before the widget's right edge
  becomes a space. Silently rejects (logs `E_WRN`) writes
  outside the widget's bounds or before `ctui_widget_init()` has run.
  `_256`/`_rgb` variants exist for richer color (see `cell.h`'s
  `CTUI_COLOR_MODE_*`) — opt-in per call site, not per widget.
  `ctui_widget_putc_rgb_fg()` is a truecolor fg over a basic bg.
- `ctui_widget_putc_cell/puts_cell(widget, comp, row, col, ch|str[, n],
  style)` — the same with a whole `CTUI_CELL` as the style: its colours
  in any `color_mode` plus its `attr` (bold, italic, underline, ...);
  `style->ch` is ignored, `n` = `(size_t)-1` for the whole string.
  Return the columns written. The way to draw styled text (markdown,
  a link); every other put writes plain cells (`attr` 0).
- `ctui_widget_puts_cut(widget, comp, row, col, str, width, fg, bg)` —
  `puts` cut to `width` columns, a string that doesn't fit ending in "…"
  (U+2026) in the last one; returns the columns written. The way to draw
  anything (a title, a name) that may be longer than its room.
- `ctui_widget_puts_n(widget, comp, row, col, str, n, fg, bg)` — puts of
  `str`'s first `n` bytes (stopping early at a NUL): a slice of a longer
  text, such as a line from `ctui_util_wrap()`, drawn without copying it
  out. Returns the columns written.
- `ctui_widget_put_kitty_placeholder(widget, comp, row, col, image_id,
  cols, rows, bg)` — a `cols` x `rows` block of placeholder cells showing
  an image placed with `ctui_gfx_kitty_place_file()` (placed at the same
  size). Taller than one row, each cell carries its image row in
  `kitty_row` (`cell.h`), which the flush sends as kitty's row diacritic;
  columns are counted from the left edge, so keep it visible.
- `ctui_widget_contains(widget, row, col)` — hit test of an absolute
  cell against `x/y/w/h`, for `CTUI_MOUSE_EVENT_DATA`.
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

- `CTUI_EVENTTYPE` — `CTUI_KEYPRESS_EVENT`, `CTUI_MOUSE_EVENT`,
  `CTUI_RESIZE_EVENT`, `CTUI_TICK_EVENT`, `CTUI_TIMER_EVENT`,
  `CTUI_IO_EVENT`, `CTUI_VALUE_CHANGED_EVENT`, `CTUI_FOCUS_EVENT`,
  `CTUI_OSC_EVENT`, plus unused `CTUI_WIDGET_REDRAW`/`CTUI_DUMMY_EVENT`.
- `CTUI_KEYPRESS_EVENT_DATA` — `type` (arrows, ENTER/ESC/TAB, HOME/END/
  PGUP/PGDN/INSERT/DELETE/BACKTAB, F1-F12 (`CTUI_KEY_F1 + n - 1`), CHAR, or NONE for an unrecognised
  sequence), `ch` (a codepoint for CHAR) and `mods`
  (`CTUI_MOD_SHIFT/ALT/CTRL`, where the terminal reports them; on
  Enter/Tab/Backspace only after `ctui_kitty_keys_enable()`).
- `CTUI_MOUSE_EVENT_DATA` — `action` (press/release/motion/scroll),
  `button` (0 left, 1 middle, 2 right, `CTUI_MOUSE_BUTTON_BACK` /
  `_FORWARD` for a mouse's side buttons, 5-6 beyond), absolute
  `row/col`, `mods`. Only produced after
  `ctui_mouse_enable()` (`term.h`); every listener gets every report
  and hit-tests with `ctui_widget_contains()`.
- `CTUI_FOCUS_EVENT_DATA` — `focused` (1 = the terminal window gained
  keyboard focus, 0 = lost it). Only produced after
  `ctui_focus_enable()` (`term.h`), source `"input"`.
- `CTUI_OSC_EVENT_DATA` — an OSC sequence the terminal sent (`ESC ]
  ... ESC \` or BEL), source `"input"`: `code` (the number before the
  first `;`, `-1` if none), `text` (the body after it, NUL-terminated;
  static, valid until the next input event), `len`, `truncated` (longer
  than `CTUI_OSC_MAX`, 16 KiB: `text` is its start). Terminals send them
  as answers (an OSC 52 read, kitty's OSC 72 drag and drop); the app
  writes its own requests to stdout.
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
  `ctui_app_init()` to have run first (registrations live on `ctui_g_app`).
- `ctui_event_unregister(widget)` — drops every registration made
  against `widget`; safe mid-dispatch (tombstoned, compacted after the
  outermost `ctui_handle_event()` returns).
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
  `ctui_app_run()` calls this once per loop iteration, and
  `ctui_input_loop()` wakes the loop at the next deadline
  (`ctui_timer_ms_until_due()`), so timers fire on time regardless of
  `tick_ms`.
- `ctui_timer_cancel(timer)` — stops and frees one registration (an
  emptied synchronized group goes with it); safe from inside any
  timer handler, including its own.
- `ctui_timer_reset()` — called by `ctui_app_init()`/`ctui_app_free()`;
  apps never call this themselves.

Widget-level glue lives in `src/widgets/periodic.c` (`ctui_periodic_
register()`), not here — see `PROGRESS.md`'s Timers entry.

## `io.h` — fd watches

For widgets fed by something other than the keyboard: a DBus
connection, a unix socket, a child's pipe, inotify.

- `ctui_io_watch(fd, CTUI_IO_READ | CTUI_IO_WRITE, widget, handler)` —
  adds `fd` to the run loop's `select()`. Readiness arrives as a
  `CTUI_IO_EVENT` dispatched straight to `handler` (like timers, not
  through the registry), with a `CTUI_IO_EVENT_DATA` (`fd`, `ready`
  bits). Level-triggered, so drain what's there. The caller keeps
  owning `fd`. A watch added from inside a handler first fires in the
  next round (it may reuse an fd number a handler just closed).
- `ctui_io_set_events(watch, events)` — change the mask (e.g. only
  ask for `CTUI_IO_WRITE` while output is queued; `0` pauses).
- `ctui_io_unwatch(watch)` — stop and free; safe mid-dispatch. Close
  the fd after this, not before.
- `ctui_io_fill/ready/dispatch/reset` — core-internal plumbing for
  `ctui_input_loop()`/`ctui_app_run()`/`ctui_app_init()`.

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
- `weights` — optional per-child sizing for V/H (`NULL` = even): a
  negative weight `-n` pins a child to exactly `n` cells, positive
  weights share whatever's left proportionally, `0` gets nothing. See
  `CTUI_SPLIT.weights` in `split.h` for rounding/clipping rules.
- `ctui_split_layout(self, comp)` — divides `self`'s *current*
  `x/y/w/h` across `children[0..count-1]` (evenly, or per
  `weights`), rebinding each via
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
- `ctui_app_free(app)` — frees `app->comp` and `app->handlers` (and
  resets the timer and fd-watch registries).
- `app->quit_on_esc` — `1` after `ctui_app_init()`; set it to `0` for
  an app where ESC is an ordinary key (a shell, a launcher).
- `app->render_begin(arg)` / `app->rendered(widget, comp, arg)` /
  `app->render_end(comp, arg)` (+ `app->render_arg`) — optional, `NULL`
  after `ctui_app_init()`: `ctui_app_render()` calls the first after
  clearing the compositor, the second after every widget
  `ctui_widget_dispatch_render()` draws (nested ones first, then the one
  drawing them), the third before the blit. For state an app keeps across
  a frame's widgets (images placed under text that a later widget may draw
  over).
- `ctui_app_quit()` — ends `ctui_app_run()` once the current event
  finishes; callable from any handler.
- `ctui_app_run_until(app, screen, tick_ms, &done)` — the run loop again
  from inside a handler (a modal dialog whose answer the caller waits
  for: a script's `alert()` / `confirm()` / `prompt()`), until `done` is
  set by a handler, timer or fd watch: `1`; `0` if the app quit or input
  ended meanwhile, and the `ctui_app_run()` around it then returns too.
  Events go to the same handlers (the app routes keys to its dialog);
  the current frame is drawn first.
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
- `ctui_mouse_enable(track_motion)` — opt into SGR mouse reports
  (`CTUI_MOUSE_EVENT`); `ctui_shutdown()` turns them back off. While
  on, the terminal's own click-to-select needs shift held.
  `track_motion`: `CTUI_MOUSE_TRACK_CLICKS` (0: presses, releases,
  wheel), `CTUI_MOUSE_TRACK_DRAG` (2: plus motion while a button is
  held, for sliders and drag handles) or `CTUI_MOUSE_TRACK_ANY` (1: plus
  every motion, for hover; chatty). The terminal has one mode, so calls
  only raise it: widgets can each ask for their own level in any order.
- `ctui_focus_enable()` — opt into focus reports (`CTUI_FOCUS_EVENT`)
  when the terminal window gains/loses keyboard focus;
  `ctui_shutdown()` turns them back off.
- `ctui_kitty_keys_enable()` — opt into the kitty keyboard protocol's
  "disambiguate" level, so shift/ctrl+Enter, ctrl+Tab and
  ctrl/shift+Backspace carry their `mods` (legacy terminals send the same
  byte either way). Every key still decodes to the event its legacy bytes
  would give (ctrl+letter is still the control byte); terminals without
  the protocol ignore it; `ctui_shutdown()` restores the previous mode.

- `ctui_suspend()` / `ctui_resume()` — hand the terminal to a child (an
  editor, a pager) and take it back: suspend leaves the alternate screen,
  turns the modes above off and restores the termios `ctui_init()` found;
  resume brings all of that back and makes the next `ctui_input_loop()`
  report a `CTUI_RESIZE_EVENT`, so `ctui_app_run()` redraws every cell.
  Call them from a handler around the child's `fork()`/`waitpid()`.

## `input.h` — the blocking read loop

- `ctui_input_loop(ev, tick_ms)` — blocks until the first of: a key
  or mouse report on stdin (CSI/SS3/SGR/UTF-8 decoding happens here;
  unknown sequences are consumed whole and come back as
  `CTUI_KEY_NONE`), a watched fd (`CTUI_IO_EVENT`), a timer deadline
  (`CTUI_TIMER_EVENT`), or `tick_ms` without input
  (`CTUI_TICK_EVENT`). Returns `0` on EOF/error. Not usually called directly by app code — `ctui_app_run()`
  is the one caller; reach for this yourself only if you're building a
  custom run loop instead of using `ctui_app_run()`.
- `ctui_input_set_source(src)` — events from `src` instead of the
  terminal (`NULL`: back to it). `src->fd` is selected for reading (`-1`:
  none, only timers/fd watches/ticks wake the loop); `src->next(ctx, ev,
  readable)` never blocks: called with `0` before every wait (hand out an
  event already buffered) and `1` once the fd is readable; it returns `1`
  with `*ev` filled (its data in the source's own storage), `0` for none,
  `-1` at the end (the loop returns `0`, ending `ctui_app_run()`). With a
  sink screen this runs an app with no terminal at all.

## `log.h` — logging

- `ctui_log_init(verbosity)` / `ctui_log_shutdown()` — split out of
  `ctui_init()`/`ctui_shutdown()` specifically so headless callers
  (`tools/ctui_test.h`) can get a working logger without a real tty.
  `ctui_init()` calls `ctui_log_init()` itself — normal apps never
  call it directly.
- `ctui_log_set_path(path)` — the file `ctui_log_init()` opens
  (default `ctui.log` in the cwd); call before `ctui_init()`. For an app
  whose cwd is the user's (a file manager). The path is kept, not copied.
- `ctui_log(level, str)` / `ctui_logf(level, fmt, ...)` — `level` is
  exactly one of `E_DBG`/`E_WRN`/`E_INF`/`E_ERR` (`src/logger.h`).
  Every non-trivial core operation logs through these — grep
  `ctui.log` (gitignored) when debugging rather than guessing.
  Before `ctui_log_init()` both drop the line and return 0 (a helper
  that logs may run before an app starts ctui).
- `ctui_tick_advance()` — the global tick counter used as the
  timestamp in most `ctui_logf()` calls.

## `util.h` — layout/encoding helpers

Not tied to any specific widget:

- `ctui_util_center_h(center_str, line, fill)` /
  `ctui_util_truncate_str(str, desired, trunc)` — string-layout helpers
  for building a `render()`'s text before pushing it through
  `ctui_widget_puts()`. Both count display columns, not bytes;
  `center_h`'s buffer needs extra room for multi-byte glyphs (see
  `util.h`).
- `ctui_util_wrap(s, width, &next)` — one word-wrapped line of `s`: the
  bytes it shows (blanks at its end left out) and, in `next`, where the
  following line starts (past the blanks at the break and a `'\n'`).
  Words stay whole where they fit, longer ones are cut by columns; each
  line takes at least one glyph, so `while (*s)` over it always ends.
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
units (`ctui_g_app`, `ctui_g_resize_pending`, `ctui_g_gfx_mode`). Never included by
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
