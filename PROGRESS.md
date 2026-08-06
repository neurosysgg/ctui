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
  to widget identity, so two instances of the same widget kind can't be
  told apart by source alone *under `CTUI_EVENT_SCOPE_GLOBAL`* — see
  `CTUI_EVENT_SCOPE_BUBBLE` below for the opt-in fix. A widget emits its
  own event by calling `ctui_handle_event()` again from inside a handler
  (see `ctui_menu_handle_keypress()`). The registry itself is a
  realloc-grown array with linear-scan matching, not a real hash map —
  plenty fast at this scale, and the public API (register-by-key /
  dispatch-by-key) would look identical either way.
- **Event bubbling (`CTUI_EVENT_SCOPE_BUBBLE`)**: opt-in per event,
  `CTUI_EVENT_SCOPE_GLOBAL` stays the default and is unchanged. Every
  `CTUI_WIDGET` now carries `parent` (set by `ctui_split_layout()` for
  each active child, and by `ctui_group_init()` from `CTUI_GROUP.parent`
  — see `EVENT_DESIGN.md`'s Resolved open question 1, since a group has
  no natural "self" widget to derive this from the way a split does) and
  `is_active_child` (set only by `ctui_split_layout()`; `NULL` on a
  group parent, since `CTUI_GROUP` has no active/inactive subset at
  all — every member's always active). An emitter that wants
  disambiguated, instance-scoped delivery sets `CTUI_EVENT.origin =
  self` and `.scope = CTUI_EVENT_SCOPE_BUBBLE`; `ctui_handle_event()`
  then walks `origin`, then `origin->parent`, etc., running only
  handlers registered against each widget actually on that chain.
  `origin`'s own handlers always fire; `is_active_child` only gates
  whether the walk is allowed to continue past a step to that step's
  parent — pruning a still-registered handler on a widget that's no
  longer one of its split's active children, with no per-app "is this
  currently visible" check needed. `list.c`/`menu.c`/`navbar.c`/
  `grid.c`'s own `CTUI_VALUE_CHANGED_EVENT` emit sites all set `.origin
  = self` now (still `.scope = GLOBAL` there — bubbling is per-app,
  opt-in at the registration end, not forced by the emit site). See
  `EVENT_DESIGN.md` for the full design and `tests/event_test.c`'s
  `test_bubble()` for the sibling-isolation / ancestor-reach /
  active-pruning behavior verified end to end.
- **Resize**: `SIGWINCH` sets a flag (signal-safe: just a
  `sig_atomic_t`), picked up by `ctui_input_loop()` — which relies on
  the interrupted blocking `read()` returning `EINTR` to notice
  promptly rather than waiting for the next keypress — and turned into
  a `CTUI_RESIZE_EVENT`. `ctui_app_resize()` reallocates the compositor
  and screen in place, force-clears the real terminal (a shrink could
  otherwise leave stale content outside the new bounds), re-runs
  `ctui_widget_init()` (and so every widget's `layout()`) for every
  widget, then dispatches the resize event through the registry.
- **Ticking**: `ctui_input_loop()` takes an optional `tick_ms`; when
  `> 0` it waits on stdin via `select()` with that timeout (instead of a
  plain blocking `read()`) and, on timeout with nothing pending, emits a
  `CTUI_TICK_EVENT` (source `"timer"`, no payload) rather than reading.
  `EINTR` from a real `SIGWINCH` during the wait still falls through to
  the resize check first, so a resize during a tick wait is never
  mistaken for a tick. `ctui_app_run()` threads `tick_ms` straight
  through; `<= 0` is the original blocking-on-input-only behavior, so
  every existing caller is unaffected by just passing `0`. No special
  casing needed in the run loop itself — `CTUI_TICK_EVENT` isn't
  `RESIZE` or `ESC`, so it already falls through to the generic
  `ctui_handle_event()` → re-render-if-changed path. Added to drive the
  `examples_apps/clock` app.
- **Timers** (`src/core/timer.c`/`timer.h`, new subsystem): a second,
  independent ticking mechanism alongside `CTUI_TICK_EVENT` above, for
  widgets that each want their *own* period instead of sharing the
  app's one `tick_ms`. `ctui_timer_register(duration_ms, widget,
  handler)` registers a standalone timer with its own private
  countdown. `ctui_timer_register_synchronized(duration_ms, widget,
  handler)` instead buckets every registration sharing the same
  `duration_ms` into one `CTUI_TIMER_GROUP` with a single shared
  deadline, so e.g. three widgets all registered at 300ms fire in the
  same `ctui_timer_tick()` call, frame after frame, rather than three
  independently-scheduled 300ms timers that would eventually drift
  apart. Both public functions share a `make_timer()` helper for the
  actual `CTUI_TIMER` allocation/fill (`group` is `NULL` for an
  independent timer, or the shared group being joined otherwise) —
  they differ only in which array the new timer gets appended to and
  whether that array is a group's `members` or the flat independent
  list. Both registries are file-static in `timer.c` (not stored on
  `CTUI_APP`, unlike the event handler registry) — simpler, and there's
  only ever one live app per process anyway; `ctui_timer_reset()`
  clears them, called by both `ctui_app_init()` (fresh slate) and
  `ctui_app_free()` (teardown). `ctui_app_run()` calls
  `ctui_timer_tick()` once per loop iteration and folds its return into
  the same changed-so-render check `ctui_handle_event()` already
  feeds — so a timer's real firing resolution is bounded by whatever
  `tick_ms` the app's `ctui_app_run()` call was given, same limitation
  `CTUI_TICK_EVENT` already has. A fired timer dispatches a
  `CTUI_TIMER_EVENT` (source `"timer"`, no payload) directly to its own
  `(widget, handler)` pair — not through `ctui_handle_event()`'s
  registry, since the whole point is the timer subsystem already knows
  exactly who to call. `src/widgets/periodic.c`/`periodic.h` is timer
  glue and nothing else: `ctui_periodic_register(widget, duration_ms,
  synchronized, handler)` wraps `ctui_timer_register()`/
  `ctui_timer_register_synchronized()` so a caller never touches
  `core/timer.h` directly, just this one widget-level call — it holds
  no `widget_data` or render of its own, `handler` is the caller's.
  `CTUI_FLICKER` (`examples_apps/flicker/widgets/flicker.c`/`flicker.h`)
  is the actual content built on top: a hashed random char-or-blank
  fill, reseeded on every timer fire it's wired to via
  `ctui_periodic_register()`. Keeping these separate (rather than one
  widget owning both the render and its own registration, an earlier
  pass at this got called out for) means `periodic.c` stays reusable
  by any future self-scheduling widget, not just this one. Only
  `periodic.c`/`periodic.h` went straight into `src/widgets/` — a
  deliberate exception to the usual second-app-needs-it promotion bar
  (see `CLAUDE.md`), since `ctui_periodic_register()` reads as
  generically reusable timer glue on its own; `CTUI_FLICKER` itself is
  the specific, unproven-generic content, so it stays staged
  app-local like any other new widget until a second app actually
  wants a hashed-fill effect. Demonstrated by the `examples_apps/
  flicker` app: six `CTUI_FLICKER` panes, three synchronized at
  300ms, two more at 600ms, and one independent widget at 450ms that
  visibly drifts in and out of phase with both groups. See
  `tests/timer_test.c` for real
  wall-clock (`nanosleep`-driven) proof that a synchronized pair fires
  together while an independent timer fires on its own schedule.
- **`examples_apps/`**: real, runnable ctui apps now live here, one
  subfolder per app (`main.c` + an optional local `widgets/`), moved out
  of `src/` so `src/` stays core-engine-and-stdlib-only. `demo` moved in
  unchanged (`src/demo.c` → `examples_apps/demo/main.c`); `clock` and
  `file_browser` are new. Each app's local `widgets/` is a staging area:
  an app-local widget is built exactly like a real one (same naming,
  same `[CTUI:...]` log tags) but only promotes to `src/widgets/` once a
  *second* app needs it — nothing promoted speculatively. The `-Isrc`
  build (replacing the previously-unused `-Iinclude`) makes
  `#include "widgets/foo.h"` resolve to an app's own local copy first,
  falling back to the real stdlib only if there isn't one, so a
  promotion is just `git mv` + deleting the local copy.
- **Terminal I/O**: raw ANSI/terminfo escapes, termios raw mode,
  alternate screen buffer, `SIGWINCH` handling. The Kitty *graphics*
  protocol (pixel images, `CTUI_GFX_KITTY`) landed -- see the changelog
  entry below and `GFX_DESIGN.md`'s Phase 4. Kitty's separate *keyboard*
  protocol (richer key-event reporting) is still a possible future
  upgrade, not a near-term priority.
- **Testing**: `ctui_init()` was split so its logger setup is now its
  own public function, `ctui_log_init()` — `ctui_init()` still calls it
  as the first step, but headless callers (namely `tools/ctui_test.h`)
  can now get a working `ctui_logf()` without a real tty, since the
  rest of `ctui_init()` (`tcgetattr`/`tcsetattr` raw mode) would
  otherwise fail outside one. `tools/ctui_test.h` and `tests/*.c` (run
  via `make test`) exercise widget/event/layout logic directly —
  inject a key or resize through the real `ctui_handle_event()`/
  `ctui_app_resize()` paths, assert against `screen->cells` — with no
  pty or subprocess involved. `tools/pty_harness.py` still covers what
  that can't reach: raw-mode byte/ESC-sequence decoding in
  `ctui_input_loop()`, real `SIGWINCH` delivery, and the literal ANSI
  bytes `ctui_screen_flush()` emits. See `CLAUDE.md`'s Testing approach
  for when to reach for which. `make coverage` (`tools/coverage.sh`,
  plain `gcov`) reports current line coverage per `src/core/*.c` file
  — see the changelog entry below for the current numbers and which
  gaps are structural (pty-only) vs. actually missing tests.

## Fixed / addressed

- [x] `src/ctui.c`/`src/ctui.h` split into `src/core/`, one `.c`/`.h`
      pair per subsystem (`screen`, `compositor`, `widget`, `event`,
      `app`, `group`, `split`, `term`, `input`, `log`, `util`, plus a
      shared `cell.h`). `src/ctui.h` is now a thin aggregator that
      `#include`s every `core/*.h` in dependency order, so apps/widgets
      are unaffected — they still only ever `#include "ctui.h"`.
      Cross-file statics that used to be plain file-scope globals in
      the monolithic `ctui.c` (`g_app`, `g_resize_pending`) now live
      behind `extern` declarations in a private `src/core/
      ctui_internal.h`, included only by `core/*.c` files. Confirmed
      warning-free build (`make all`), all `make test` assertions still
      pass, and a `pty_harness.py` smoke run of `ctui-demo` renders
      identically to before.
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
- [x] `examples_apps/` added, `demo.c` moved in as `examples_apps/demo/
      main.c`. Two new apps: `clock` (a ticking clock widget, currently
      staged in `examples_apps/clock/widgets/`, not yet promoted to
      `src/widgets/`) and `file_browser` (a scrollable, navigable
      directory listing via a new `CTUI_LIST` widget, same staging
      status, currently in `examples_apps/file_browser/widgets/`) — the
      long-standing "simple clock widget" Next-up item below, done.
- [x] `CTUI_TICK_EVENT` / `ctui_app_run(..., tick_ms)`: apps can now
      redraw on a timer, not just on keypress/resize (needed by `clock`).
      See Architecture above.
- [x] `calculator` example app added (`CTUI_DISPLAY` readout + a keyed
      grid of buttons), with its 4-function engine kept entirely free of
      `ctui.h` — `calc.h`/`calc.c` know nothing about widgets or events;
      `main.c` is a thin translation layer mapping grid presses to
      `CALC_TOKEN`s and copying `CALC_RESULT.text` back into the display.
      The grid itself started as an app-local `CTUI_KEYPAD`, then was
      generalized into `CTUI_GRID` (rows x cols of navigable, keyable
      cells; arrows + Enter, or type a cell's shortcut char directly) and
      promoted straight to `src/widgets/` on request — the only widget so
      far promoted before a second app needed it, rather than through the
      usual staging-in-`examples_apps/*/widgets/`-first path.
- [x] Demo's `"vm stats"` menu placeholder renamed to `"dump palette"` and
      given real behavior: a new app-local widget,
      `examples_apps/demo/widgets/dump_palette.c` (staged, not yet
      promoted — only demo needs it so far), renders every basic ANSI
      color as a filled swatch in the fewest rows (>=2) that evenly divide
      the color count into columns (3x3 for the current 9 colors), each
      swatch's name centered on top in default fg/bg so it stays legible
      regardless of the swatch's own color. Reveals below the menu the
      same way `"debug info"` already did, via a generalized
      `main_split_handle_panel_toggle()` (replacing the old
      `main_split_handle_debug_toggle()`) that lets `"debug info"` and
      `"dump palette"` share the split's one optional second-pane slot —
      toggling one on swaps it into `split->children[1]`; toggling one off
      only closes the pane if it's the one currently occupying that slot.
      Two file-scope pointers (`debug_info_widget`/`dump_palette_widget`)
      let the handler identify which built widget a toggled label
      corresponds to, since it only receives the split widget itself as
      `self` — same single-module-state pattern `ctui.c` already uses for
      `g_app`.
- [x] Headless C testing added: `ctui_init()` split into `ctui_log_init()`
      (logger only) + the existing tty setup, and `ctui_screen_resize()`'s
      `\x1b[2J` terminal-clear guarded behind `isatty(STDOUT_FILENO)` — it
      was unconditional before, which spammed a raw escape sequence into
      stdout whenever `ctui_app_resize()` ran without a real terminal
      behind it (harmless-looking but wrong regardless of testing: any
      non-tty stdout, not just a test run, would have gotten corrupted
      output). `tools/ctui_test.h` + `tests/menu_status_test.c` (real
      `CTUI_MENU`/`CTUI_STATUS` wired through actual event registration,
      7 assertions covering selection, event propagation, and resize) are
      the first use of both. `make test` target added.
- [x] `CTUI_TIMER` core mechanism added (`src/core/timer.c`/`timer.h`):
      `ctui_timer_register()` for independent per-widget periods,
      `ctui_timer_register_synchronized()` to bucket same-duration
      registrations into one shared-deadline group. Wired into
      `ctui_app_init()`/`ctui_app_free()`/`ctui_app_run()`; new
      `CTUI_TIMER_EVENT` added alongside `CTUI_TICK_EVENT` in
      `core/event.h`. New `examples_apps/flicker` app (+ app-local
      `CTUI_FLICKER` widget) demonstrates it: six panes reseeding a
      hashed random fill, three synced at 300ms, two synced at 600ms,
      one independent at 450ms. `tests/timer_test.c` proves the
      grouping/independence with real `nanosleep()`-driven timing.
      Verifying this against `tools/pty_harness.py` surfaced a real,
      pre-existing bug in the harness itself: `drain()`'s inner loop
      reused its caller-supplied `timeout` on every `select()` call
      instead of budgeting total elapsed time, so a target that keeps
      emitting output on a cadence shorter than that timeout (true of
      flicker's staggered sub-second timers, unlike e.g. `clock`'s
      once-a-second cadence with long quiet gaps) could keep `drain()`
      finding fresh data forever and never return. Fixed by giving
      `drain()` its own deadline computed once up front.

- [x] `examples_apps/player` added: a terminal WAV player with a live
      VU-meter level display, playing through ALSA. Design decided up
      front in `examples_apps/player/DESIGN.md` before any code existed
      (three independent stages — decoder, output, process — tee'd off
      the same interleaved-float32 buffer; a single ~20ms `CTUI_TIMER`
      tick drives all three each frame, no playback thread/ring buffer).
      `CTUI_DECODER`/`CTUI_AUDIO_OUTPUT` (`examples_apps/player/audio/`)
      mirror `CTUI_WIDGET`'s render/layout function-pointer dispatch.
      `decoders/wav.c` is a from-scratch canonical-PCM RIFF parser
      (8/16/24/32-bit integer, rejects IEEE float and
      WAVE_FORMAT_EXTENSIBLE) kept entirely free of `ctui.h`, same
      precedent as calculator's `calc.c` — verified standalone against a
      synthetic WAV before anything else was wired to it.
      `outputs/alsa.c`/`outputs/null.c` implement `CTUI_AUDIO_OUTPUT`;
      ALSA is opened `SND_PCM_NONBLOCK` and `main.c` falls back to
      `null_output` (drops frames, keeps the meter/UI alive) if opening
      the real device fails, so the app still runs headless without
      audio hardware. `alsa.c`/`alsa.h` need `_GNU_SOURCE` defined as the
      including `.c` file's first line (before any `#include`) —
      `alsa/asoundlib.h`'s own `struct timespec` fallback collides with
      glibc's under plain `-std=c11` otherwise; same convention already
      used for `_POSIX_C_SOURCE` in `core/term.c`/`core/input.c`/
      `core/timer.c`/`file_browser`'s `main.c`. The viz is a single
      VU-style bar (`examples_apps/player/widgets/meter.c`, app-local —
      nothing else needs it yet), fed each timer tick by an RMS
      computed over the same chunk the decoder handed output, per the
      design's read-only-tap decision (process never touches the
      buffer output plays). File selection reuses `file_browser`'s list
      navigation unchanged; since `player` is a second consumer,
      `CTUI_LIST` (`list.c`/`list.h`) was promoted from
      `examples_apps/file_browser/widgets/` straight to `src/widgets/`
      per `CLAUDE.md`'s promotion rule (`git mv`, no code changes, no
      include-path surgery — `file_browser`'s own `#include
      "widgets/list.h"` now falls back to the stdlib copy automatically).
      New `ctui-player` Makefile target links `-lasound -lm` for that
      target only, every other binary still depends on libc alone.
      Verified via `tools/pty_harness.py`: directory navigation filtered
      to `*.wav` + subdirs, playback start/EOF/stop lifecycle, the
      meter's RMS-driven level and color zones (confirmed via raw ANSI —
      `30;42` while a mid-volume tone played, `30;40` for the unfilled
      remainder), and a live resize mid-layout.

- [x] `CTUI_GFX_ANSI256` support landed (Phases 1-3 of `GFX_DESIGN.md`; Phase 4
      -- per-widget renderer declaration for non-degradable protocols like
      Kitty -- is still just a plan, not needed until a widget exists that
      can't degrade to text). `CTUI_CELL` (`src/core/cell.h`) gained
      `color_mode` (`CTUI_COLOR_MODE_BASIC`/`_256`/`_RGB`) plus `fg_r/g/b`,
      `bg_r/g/b`, all defaulting to `BASIC`/zero so every existing
      `CTUI_COLOR_*` literal and `ctui_widget_putc()` call keeps compiling
      and behaving identically. New `src/core/gfx.c`/`gfx.h`:
      `CTUI_GFX_MODE` is a bit-flag enum (`ANSI16=1<<0` .. `KITTY=1<<3`,
      not sequential indices -- the doc's original draft used sequential
      values, which can't be tested with `&`), and
      `ctui_gfx_detect_caps()` env-sniffs `TERM`/`COLORTERM`/
      `KITTY_WINDOW_ID`, unconditionally setting the `ANSI16` bit rather
      than deriving it (2026-era floor assumption, see `GFX_DESIGN.md`).
      `ctui_init()` gained a `CTUI_GFX_MODE *mode` param -- in/out, not a
      plain value: it hard-fails (logged, `-1`) only if the mandatory
      `ANSI16` floor itself is missing; if the *requested* tier is
      missing, it instead logs a warning and overwrites `*mode` with the
      highest tier the terminal actually supports, then continues, so an
      app asking for something above the floor still starts on a plainer
      terminal rather than refusing to run. Either way the (possibly
      downgraded) result is stored in `g_gfx_mode` (new cross-file
      static, same pattern as `g_app`/`g_resize_pending`). All 7
      `examples_apps/*/main.c` call sites now pass the address of a local
      `CTUI_GFX_MODE` set to `CTUI_GFX_ANSI16` (behaviorally unchanged --
      the floor can only be negotiated *to*, never below, so these 6
      never need to inspect it afterward) except `demo`, which requests
      `CTUI_GFX_ANSI256` as this feature's proving ground and does
      inspect the result. New `ctui_widget_putc_256()`/`puts_256()`
      (same signature as the plain versions -- `fg`/`bg` are already a
      `0-255`-wide `unsigned char`, just reinterpreted as a palette
      index) let a widget opt in; `demo`'s `dump_palette` widget draws a
      256-color-cube ramp (indices 16-231) across its bottom row via
      `putc_256()` whenever it has >= 4 rows *and* `main()`'s negotiated
      `CTUI_GFX_MODE` (passed down as `widget_data`, a `CTUI_GFX_MODE*`)
      still reads `CTUI_GFX_ANSI256` -- so `demo` runs correctly either
      way: full ramp on a 256-color terminal, plain swatches-only on a
      basic one, verified both ways via `pty_harness.py` under
      `TERM=xterm` (negotiates `0x2 -> 0x1`, ramp skipped) and
      `TERM=xterm-256color` (negotiates `0x2` cleanly, ramp drawn).
      `ctui_screen_flush()` now switches on each cell's own `color_mode`
      to emit `38;5;n`/`48;5;n` (256) or the original `30-37`/`40-47`
      (`BASIC`) SGR codes -- driven purely by the cell, not by
      `g_gfx_mode`, so richer color is orthogonal to widget logic exactly
      as designed. Two correctness fixes this surfaced: the shadow-buffer
      diff (`ctui_compare_ctuicell`) and the last-emitted-color tracking
      in `ctui_screen_flush` both had to grow to compare `color_mode`
      (and the rgb fields, when relevant) alongside `fg`/`bg`, or a
      256-mode cell with the same numeric `fg`/`bg` as a prior basic-mode
      cell would wrongly read as "unchanged"/"same color" and never get
      its real escape emitted; and `ctui_compositor_clear()`/
      `ctui_screen_clear()` now reset `color_mode` to `BASIC` every frame
      (previously only `ch`/`fg`/`bg`), otherwise a cell drawn via
      `putc_256()` one frame and plain `putc()` the next at the same
      coordinate would keep misreading the new basic `fg`/`bg` as a
      256-index. Verified via a direct (non-pty) C harness against
      `ctui_screen_flush()`'s actual emitted bytes (confirmed
      `\x1b[38;5;196;48;5;21m` for a `putc_256(fg=196,bg=21)` cell, plain
      cells unchanged, and a second flush with no changes correctly
      emitting nothing but the trailing reset -- proving the diff fix
      works); the floor hard-fail and the requested-tier
      downgrade-instead-of-fail path both verified directly against
      `ctui_init()`'s return code/`*mode` output and `[CTUI:GFX]` log
      lines under different `TERM`/`COLORTERM` combinations; and
      `tools/pty_harness.py` against the real `ctui-demo` binary in both
      directions -- `TERM=xterm-256color` (caps detected, `ANSI256`
      negotiated cleanly, `dump_palette` panel rendered with its ramp)
      and `TERM=xterm` (negotiated down to `ANSI16`, `demo` still starts
      and runs, `dump_palette` correctly omits the ramp) -- with no new
      bounds warnings from the new code across several resizes.
      Truecolor/RGB widget entry points
      (`ctui_widget_putc_rgb()`/`puts_rgb()`) and Phase 4 stay deferred
      until something actually needs them -- see `GFX_DESIGN.md`.
- [x] `CTUI_GFX_TRUECOLOR` widget entry points landed:
      `ctui_widget_putc_rgb()`/`puts_rgb()` (`src/core/widget.c`/`.h`),
      same shape as the `_256` pair but taking `fg_r/g/b`, `bg_r/g/b`
      directly and setting `color_mode = CTUI_COLOR_MODE_RGB` -- the cell
      fields and `ctui_screen_flush()`'s `38;2;r;g;b`/`48;2;r;g;b`
      emission already existed from the ANSI256 pass, so this was purely
      the widget-facing entry point that was deferred at the time.
      `debug_info` (`src/widgets/debug_info.c`) is the proving ground:
      `widget_data` is now optional (`NULL`, unchanged behavior, or a
      `CTUI_GFX_MODE*` -- same convention as `demo`'s `dump_palette`),
      and shows a `"gfx: <tier>"` line plus a `"color_mode: <basic|256|
      rgb>"` line -- the two are deliberately separate lines, not one,
      since `CTUI_GFX_MODE` (terminal capability) and
      `CTUI_COLOR_MODE_*` (per-cell encoding) are different enums (see
      "Resolved open questions" above); `gfx_mode_color_mode()` maps one
      to the other for display. Then, only when the negotiated mode
      reads back `CTUI_GFX_TRUECOLOR` and `self->h >= 6`, a bottom row
      sweeps a full-saturation HSV hue gradient via
      `ctui_widget_putc_rgb()` -- a smooth 24-bit gradient a 256-color
      ramp can only band, so it's a direct visual proof truecolor is
      actually active, not just requested. `demo`'s `main()` now requests
      `CTUI_GFX_TRUECOLOR` (up from `CTUI_GFX_ANSI256`) as the richer
      proving-ground tier, threading `&gfx_mode` into `debug_info` the
      same way it already did for `dump_palette`. This meant
      `dump_palette`'s existing 256-ramp condition (`*gfx_mode ==
      CTUI_GFX_ANSI256`) had to widen to `== CTUI_GFX_ANSI256 ||  ==
      CTUI_GFX_TRUECOLOR`, since `ctui_init()` can now legitimately hand
      back either tier depending on what the terminal actually supports,
      and a truecolor terminal is a 256-color terminal too (per
      `ctui_gfx_detect_caps()`) -- without the widen, `dump_palette`'s
      ramp would have silently stopped rendering on the *best* terminals.
      Verified directly: a non-pty C harness against `ctui_app_render()`
      confirmed the hue-sweep cells carry `color_mode ==
      CTUI_COLOR_MODE_RGB` with a smoothly varying `bg_r/g/b` ramp, and
      against `ctui_screen_flush()`'s actual emitted bytes (40 real
      `\x1b[38;2;r;g;b;48;2;r;g;b m` escapes, one per hue-sweep cell);
      `tools/pty_harness.py` against the real `ctui-demo` binary across
      three `TERM`/`COLORTERM` combinations -- full truecolor (`gfx:
      truecolor` line, `dump_palette` ramp still renders), 256-only
      (`ctui_init()` negotiates down to `0x2`, ramp still renders via the
      widened condition), and a plain terminal (`env -u COLORTERM -u
      KITTY_WINDOW_ID TERM=xterm`, negotiates down to `0x1`, shows `gfx:
      ansi16`, hue sweep correctly omitted, no crash). `dump_palette` got
      the same truecolor row as a follow-up, not just the ramp-condition
      widen: below its existing 256-color ramp, a second bottom row (only
      when the granted tier is `CTUI_GFX_TRUECOLOR` specifically and
      `self->h >= 6`) sweeps the same hue gradient via
      `ctui_widget_putc_rgb()` -- `hue_to_rgb()` duplicated locally in
      `examples_apps/demo/widgets/dump_palette.c` rather than shared,
      since it's a demo-app-local widget, not library code. Verified the
      same way: a non-pty C harness confirmed row 4 is `color_mode ==
      CTUI_COLOR_MODE_256` (the ramp) and row 5 is `CTUI_COLOR_MODE_RGB`
      (the sweep) when truecolor is granted at `self->h == 6`, and that
      the ramp alone shifts down to fill the freed row (no RGB row) when
      only `CTUI_GFX_ANSI256` is granted. Phase 4
      (`supported_gfx_modes`/`ctui_widget_set_gfx_renderer()`) stays
      deferred, same reasoning as before.
- **Kitty graphics protocol landed: the real wire emission plus all of
  GFX Phase 4** (`docs/protocol.md`'s recipe, `GFX_DESIGN.md`'s
  "Non-degradable protocols" section). `CTUI_GFX_KITTY` detection
  already existed (env-sniffing `KITTY_WINDOW_ID`/`TERM=xterm-kitty`);
  what didn't exist yet was anything that could actually *use* it, or
  any mechanism for a widget to declare "I need this specific protocol,
  I have no text to degrade to."
  - `ctui_util_base64_encode()`/`ctui_util_base64_len()`
    (`src/core/util.c`/`.h`): generic RFC 4648 base64, not
    ctui-specific -- Kitty's transmission payload just happens to be the
    first thing that needs it.
  - `ctui_gfx_kitty_display()` (`src/core/gfx.c`/`.h`): builds and
    writes the actual `\x1b_G...\x1b\\` APC escape(s) straight to
    stdout, bypassing `CTUI_CELL` entirely (pixel graphics can't be
    expressed as a colored character cell) -- `a=T,f=32` (transmit +
    display, raw RGBA), `s=`/`v=` pixel dimensions, `c=`/`r=` cell
    scaling, `q=2` (quiet -- ctui never reads APC replies), chunked at
    4096 encoded bytes with `m=1`/`m=0` continuation flags per the
    protocol's own chunking rule (split points are valid anywhere in
    the *encoded* string, not tied to 3-byte raw boundaries). No-ops
    (logged) if stdout isn't a real tty or the image is degenerate.
  - Phase 4 on `CTUI_WIDGET` (`src/core/widget.h`/`.c`):
    `supported_gfx_modes` (defaults to all three text tiers via
    `ctui_widget_make()` -- every existing widget qualifies with zero
    code changes, since text rendering degrades via each cell's own
    `color_mode` regardless of what got negotiated) plus
    `ctui_widget_set_gfx_renderer(widget, mode, render)`, which narrows
    `supported_gfx_modes` to exactly `mode` and registers an alternate
    `render()` for it -- a pixel-graphics widget has no sensible text
    fallback, so there's nothing left to degrade to.
  - `ctui_app_init()` (`src/core/app.c`/`.h`) now returns `int` (was
    `void`) and hard-fails (`-1`, logged) if a widget's declared
    non-text `supported_gfx_modes` don't overlap the negotiated
    `g_gfx_mode` -- masking the three always-satisfiable text-tier bits
    out of the check first is what keeps this a no-op for every
    ordinary widget; it only ever fires for one that opted into
    `ctui_widget_set_gfx_renderer()`. All 7 `examples_apps/*/main.c`
    call sites (plus `tests/*.c`, unaffected since their widgets never
    narrow) updated to check the return, same `!= 0` pattern
    `ctui_init()` already used.
  - `ctui_app_render()`'s dispatch skips a matched gfx widget's
    `render()` entirely (nothing to draw into the compositor); its
    pixels are drawn by a new `render_gfx_widgets()` static helper,
    called from `ctui_app_run()` immediately *after* every
    `ctui_screen_flush()`, never before -- `ctui_screen_flush()`'s
    shadow-buffer diff would otherwise paint blank text cells back over
    whatever pixels were just written, since the widget's (untouched,
    still-default) compositor cells look unchanged frame to frame.
  - Proving-ground: `examples_apps/kitty_demo` (new Makefile target
    `ctui-kitty_demo`), requesting `CTUI_GFX_KITTY` specifically. Its
    one widget, app-local `CTUI_KITTY_IMAGE`
    (`examples_apps/kitty_demo/widgets/`), generates a procedural RGB
    diagonal gradient at runtime (no image-decoding dependency) and
    displays it via `ctui_gfx_kitty_display()`.
  - Verified: `tests/kitty_protocol_test.c` (headless, `make test`) --
    base64 against known RFC 4648 vectors, `ctui_widget_make()`'s
    default `supported_gfx_modes`, `ctui_widget_set_gfx_renderer()`'s
    narrowing, and `ctui_app_init()`'s pass/hard-fail paths (simulated
    by redeclaring the private `g_gfx_mode` extern locally, the same
    gray-box trick `ctui_test.h` already relies on for reading
    `screen->cells` directly -- there's no `ctui_init()` in a headless
    test to set it for real). The actual APC bytes were checked against
    `ctui-kitty_demo` under `tools/pty_harness.py` (env forcing
    `TERM=xterm-kitty KITTY_WINDOW_ID=1`) -- see the next entry below
    for what the harness needed fixed first before that check was
    possible.
- **`tools/pty_harness.py`'s `raw` step fixed for real this pass** (see
  the Kitty graphics protocol entry below for why it was actually
  needed this time, not just nice-to-have). The bug was subtler than the
  earlier "found, not fixed" note here described: it's not that the
  *preceding* step's preamble drains first -- a fast target (any ctui
  app; there's no artificial delay anywhere in the render/flush path)
  routinely finishes writing its *entire* frame before even the *first*
  step's own preamble drain returns, so whichever step happens to run
  first ends up draining, feeding, and clearing the real bytes, and
  every step after it finds nothing left. `dump` "worked" by accident --
  it displays cumulative `grid` state, which the first step's preamble
  had already updated, not anything its own handler drained. Fixed with
  a `history` bytearray that every drain folds into and nothing ever
  clears; `raw` now prints `history` in full (genuinely cumulative, "so
  far" as the step's own doc-comment always claimed) instead of
  whatever scraps a single step's transient `buf` still held. Verified
  against `ctui-kitty_demo`'s real Kitty APC output: reassembled 22
  chunks in sequence, correct `m=1`/`m=0` framing, header keys matching
  what `ctui_gfx_kitty_display()` sent.
- **`CTUI_KITTY_IMAGE` promoted to `src/widgets/` and folded into `demo`
  as a real toggleable panel** -- `demo` is now a second consumer, so
  per `CLAUDE.md`'s promotion rule this is just the file move (`mv
  examples_apps/kitty_demo/widgets/kitty_image.{c,h} src/widgets/`, no
  code changes, matching how `CTUI_LIST` was promoted earlier). The
  integration itself surfaced three problems a single-purpose standalone
  app like `kitty_demo` never hits, all generic to any future
  non-degradable protocol, written up in full as
  `docs/protocol.md`'s new "Integrating a non-degradable protocol into a
  real, multi-widget app" section:
  1. A Phase 4 widget can't be a `CTUI_SPLIT`/`CTUI_GROUP` child --
     `ctui_split_render()`/`ctui_group_render()` call each child's
     `render()` directly, never checking `gfx_render_mode`; only the
     top-level `ctui_app_render()`/`render_gfx_widgets()` loop does that
     dispatch. `demo`'s new `kitty_image_layout()`
     (`examples_apps/demo/main.c`) reproduces the exact box a real 2nd
     `main_split` pane would occupy instead of actually being one.
  2. `render_gfx_widgets()` calls every matching widget's `gfx_render`
     unconditionally, every frame -- there's no existing "skip this
     widget this frame" mechanism the way a split naturally stops calling
     a de-activated child's `render()`. Fixed by convention: `layout()`
     collapses to `w=0, h=0` when not the active panel, and
     `ctui_kitty_image_gfx_render()` (now `src/widgets/kitty_image.c`)
     checks `self->w <= 0 || self->h <= 0` first and skips
     transmitting -- reuses fields every widget already has rather than
     inventing a separate visibility flag.
  3. `demo`'s requested tier changed from `CTUI_GFX_TRUECOLOR` to
     `CTUI_GFX_KITTY` (still degrades the same way on a plain terminal --
     `ctui_init()` only ever negotiates down). The new "kitty image" menu
     item always exists in `data.items[]`, on every terminal, but
     `kitty_image`/`kitty_image_data` are only constructed -- and
     `kitty_image_widget` only set non-NULL, only appended to the
     `widgets[]` array passed to `ctui_app_init()` -- inside an `if
     (gfx_mode == CTUI_GFX_KITTY)` branch in `main()`. This is the load-
     bearing choice: `ctui_app_init()`'s Phase 4 validation never even
     sees the widget on a terminal that can't support it, so there's
     nothing to hard-fail. Picking "kitty image" on such a terminal is a
     plain no-op in `main_split_handle_panel_toggle()`
     (`kitty_image_widget == NULL` short-circuits first) -- status still
     echoes "you picked: kitty image" via the existing generic handler,
     just no panel opens. All three menu items (debug info/dump
     palette/kitty image) are mutually exclusive over the same visual
     slot, same as debug info/dump palette were before this pass.
  Verified: `tools/pty_harness.py` against `ctui-demo` under both a
  plain terminal (negotiates to `0x1`, "kitty image" selectable but a
  no-op, no crash, `debug info`/`dump palette` unaffected) and
  `TERM=xterm-kitty KITTY_WINDOW_ID=1` (negotiates `0x8`; toggling
  "kitty image" produces a real, correctly-chunked Kitty APC transmission
  at the expected cell geometry, `c=48,r=7`; toggling `debug info`
  afterward correctly collapses and stops retransmitting the kitty
  panel, confirmed via the log's `"kitty image" enabled` /`"debug info"
  enabled, splitting main area` sequence).
- **Generalized the fix above instead of leaving it as `demo`-local
  plumbing: `ctui_widget_dispatch_render()`/`ctui_widget_flush_gfx()`
  (`src/core/widget.c`/`.h`) are now the one place render-vs-gfx_render
  gets decided, and every caller that walks widgets uses them.** The
  previous pass's `main_split`/split-child limitation (see just above)
  wasn't really a `demo` problem, it was `ctui_app_render()` being the
  *only* caller that knew about Phase 4 dispatch at all —
  `ctui_split_render()`/`ctui_group_render()` (`src/core/split.c`/
  `group.c`) both called `child->render()`/`w->render()` directly.
  Pulling the decision out into two shared functions and having all
  three renderers call `ctui_widget_dispatch_render(widget, comp)`
  instead of `widget->render(widget, comp)` fixes it at the root: a
  Phase 4 widget now works identically whether it's top-level or nested
  arbitrarily deep in splits/groups, with zero special-casing in the
  caller. `ctui_widget_dispatch_render()` either calls `render()`
  immediately (mismatch or no `gfx_render` registered) or queues the
  widget in a small file-static realloc-grown array (same shape/
  reasoning as `core/timer.c`'s own registries -- simpler than living on
  `CTUI_APP`, and there's only ever one live app per process anyway);
  `ctui_widget_flush_gfx(comp)` fires every queued widget and clears the
  queue, called by `ctui_app_run()` right after each `ctui_screen_flush()`
  (replacing the old `app.c`-local `render_gfx_widgets()`).
  `ctui_app_init()`'s hard-fail validation still only covers *top-level*
  `widgets[]` (a nested widget was never reachable from that array to
  begin with) -- now explicitly documented as the intended behavior
  rather than a gap, since a mismatched nested widget just falls back to
  its own `render()` via the same dispatch path everything else uses.
  This unlocked a real simplification of `demo`'s own integration:
  `kitty_image` is now a genuine `main_split.children[1]` candidate,
  built unconditionally as plain as `debug_info`/`dump_palette` (one
  extra `ctui_widget_set_gfx_renderer()` call), with
  `ctui_kitty_image_render()`'s `"[kitty required]"` text as its real,
  live fallback -- every piece of the previous pass's workaround
  (`kitty_image_layout()` duplicating the split's own division math, the
  `kitty_visible` flag, the `kitty_image_widget == NULL` no-op guard, the
  conditional `if (gfx_mode == CTUI_GFX_KITTY)` construction) is gone.
  Selecting "kitty image" on a non-Kitty terminal now opens the pane and
  shows the placeholder text instead of silently no-opping, a UX
  improvement that fell out of the fix for free.
  **Testing this surfaced an unrelated, pre-existing bug**, latent since
  the original two-candidate (`debug info`/`dump palette`) version of
  `main_split_handle_panel_toggle()`: swapping the shared pane slot
  directly from one open pane to another (`split->count` staying at `2`
  the whole time, only `children[1]`'s identity actually changing) never
  re-ran `ctui_split_layout()`'s binding, since the handler only checked
  whether `count` itself changed. The newly-swapped-in widget's `buf`
  stayed `NULL` (never bound), so its `render()` silently dropped every
  `ctui_widget_putc()` call (logged `E_WRN` "not bound", drew nothing).
  Three candidates instead of two made the "switch without closing
  first" path easy to reach in practice, where two candidates apparently
  never had. Fixed by tracking `pane_changed` explicitly and
  re-laying-out whenever count *or* pane identity changed, not just
  count. Verified via `pty_harness.py`: 20 "putc rejected... not bound"
  warnings in the log for the direct kitty-image→debug-info switch
  before the fix, zero after, and `[CTUI:SPLIT] - ... laid out` /
  `[DEMO:APP] - "debug info" enabled, splitting main area` now correctly
  fire on that transition.
  `make all`/`make test` warning-free throughout (25 assertions).
- **Kitty-placed images now get explicitly deleted when their pane closes.**
  Reported bug: toggling "kitty image" off in `demo` left the raster
  overlay on screen. Root cause: a Kitty-protocol image is a raster
  overlay the terminal keeps showing independent of the text grid, unlike
  a colored character cell — closing/swapping the pane just stopped
  calling `ctui_kitty_image_gfx_render()` (`ctui_split_render()` only
  dispatches `children[0..count-1]`), but nothing ever told the terminal
  to actually remove the placement, so it just stayed there. Added
  `ctui_gfx_kitty_delete()` (`src/core/gfx.c`/`.h`) — sends `a=d,d=I,i=` to
  delete both the placement and the stored image data by id (safe since
  `ctui_gfx_kitty_display()` always retransmits full pixel data on every
  redisplay rather than relying on cached data). Same calling convention
  as `ctui_gfx_kitty_display()`: this function doesn't check
  `g_gfx_mode` itself, the caller does — `demo`'s handler gates it on a
  `kitty_gfx_negotiated` flag captured once after `ctui_init()`, so a
  plain terminal that never actually got an image transmitted doesn't get
  a pointless (if harmless) delete APC either. Verified via
  `pty_harness.py` + log: `[CTUI:GFX] - kitty_delete @ tick ... (id=1)`
  fires exactly once, right before the pane closes, only when
  `CTUI_GFX_KITTY` was actually negotiated.
- **`CTUI_SPLIT` gained a third mode, `CTUI_SPLIT_GRID`, for a variable
  number of simultaneously-visible panes.** Reported bug: `demo`'s
  "debug info"/"dump palette"/"kitty image" menu items are independent
  `CTUI_MENU_ITEM` checkboxes (any subset can be enabled at once), but
  `main_split_handle_panel_toggle()` still only had one shared slot
  (`children[1]`) — enabling a second one while the first was still
  checked silently replaced it instead of showing both, an inconsistency
  between what the menu displayed (multiple checked) and what was
  actually visible (only the most recently toggled one).
  `CTUI_SPLIT_GRID` (`src/core/split.h`/`.c`) is the fix, built as a new
  mode on the existing `CTUI_SPLIT` rather than a parallel type — same
  `children`/`count` shape, so "grow/shrink count later to reveal more
  panes" already meant something for V/H and needed no new concept, just
  a new way to partition self's area: near-square rows × cols
  (`cols = ceil(sqrt(count))`, computed with a small integer loop, no
  `<math.h>`), row-major, recomputed by `ctui_split_layout()` on every
  call exactly like V/H already are. `demo` now has a `panels_grid`
  (`CTUI_SPLIT_GRID`) nested as `main_split`'s second child in place of
  the old single pane; `main_split_handle_panel_toggle()` tracks all
  three panels' enabled state independently (`panels_enabled[]`) and
  rebuilds `panels_grid`'s active `children[0..count-1]` from scratch on
  every change, in menu order — 1 enabled fills the area, 2 sit side by
  side, 3 form a 2×2 grid with one empty cell. Nesting a split inside a
  split needed no new plumbing: `panels_grid_widget`'s own `.layout` is
  set to `ctui_split_layout` directly (per `split.h`'s existing "assign
  this directly as a widget's `layout()`" doc comment), so
  `main_split`'s own `ctui_split_layout()` calling `ctui_widget_init()`
  on it cascades into its own re-partition automatically. Verified via
  `pty_harness.py` + log across enable/disable sequences in both
  orders: `[CTUI:SPLIT] - ... (GRID, N active children, ...)` tracks N
  correctly through 1→2→3→2→1→0, `kitty_delete` still fires at the right
  transition when three-panel kitty gets toggled off, and `main_split`
  correctly collapses back to just the menu (`V, 1 active children`)
  once the last panel is disabled. `make all`/`make test` warning-free
  throughout (25 assertions).
- [x] Two new `core/util.c`/`util.h` geometry helpers: `ctui_util_rescale_i()`
      (clamped linear integer rescale from one range to another — e.g.
      mapping a column index to a 256-color cube index) and
      `ctui_util_inset()`/`ctui_margin_uniform()` (sets a content
      widget's `x/y/w/h` to an outer widget's *current* `x/y/w/h`, inset
      by a `CTUI_MARGIN` on each side). The latter formalizes the
      "border draws its full box, content is a separate, inset widget"
      pattern the demo already did by hand — `demo`'s
      `header_title_layout()` now calls `ctui_util_inset(self,
      header_border_widget, ctui_margin_uniform(1))` instead of
      re-deriving the header's box from scratch, reading `header_border`'s
      already-laid-out geometry off a new file-scope widget pointer
      (same pattern as `debug_info_widget` etc.). `dump_palette`'s
      256-color ramp now builds its per-column index via
      `ctui_util_rescale_i(col, 0, self->w - 1, 16, 231)` instead of
      inline `16 + col * 216 / self->w` math. Both fail (log `E_WRN`,
      leave output untouched) rather than produce a degenerate result —
      `rescale_i` on `in_min == in_max`, `inset` on a margin that
      consumes the whole outer width/height. `tests/util_test.c` covers
      both directly (clamping, midpoint, zero-width degenerate case,
      symmetric/asymmetric margins, rejected-inset leaves geometry
      untouched); `make all`/`make test` warning-free, `pty_harness.py`
      confirms the demo's header title still renders identically.

- **Two perf fixes to the core, found during a review pass with no
  profiler involved — both just misplaced work, not algorithmic
  problems.**
  1. `ctui_logf()` (`src/core/log.c`) was formatting every message
     (`vsnprintf` twice, `malloc`, `free`) *before* `log_entry()`
     (`src/logger.c`) checked whether the verbosity mask would even
     keep it. Harmless in isolation, but `ctui_logf(E_DBG, ...)` sits
     in the actual hot path — once per character in
     `ctui_widget_putc()`/`ctui_screen_putc()`, twice per widget per
     frame in `ctui_app_render()`/`ctui_group_render()`/
     `ctui_split_render()` — so a normal app run (`E_INF|E_WRN|E_ERR`,
     no `E_DBG`; see every `examples_apps/*/main.c`'s `ctui_init()`
     call) was paying full format-and-discard cost on every one of
     those calls for a string nothing ever reads. Fixed by moving the
     identical `verbosity & level` check to the top of `ctui_logf()`,
     ahead of any formatting — same short-circuit `log_entry()`
     already did, just far enough upstream to actually save the work.
  2. `ctui_screen_flush()` (`src/core/screen.c`) `malloc`'d and
     `free`'d its `rows*cols*64`-byte ANSI scratch buffer on *every*
     call — once per rendered frame, the entire time an app runs, for
     memory whose worst-case size never changes between frames.
     Turned into `CTUI_SCREEN`'s own `out`/`out_cap` fields, sized once
     in `screen_alloc()` (both `ctui_screen_create()` and
     `ctui_screen_resize()` go through it, so a resize still resizes
     the scratch buffer correctly) and freed once in
     `ctui_screen_free()`/on the next resize — `ctui_screen_flush()`
     itself no longer allocates anything.
  Neither changes any observable behavior — both are pure
  moved-earlier/made-persistent fixes. Considered, deliberately not
  done: adding a separate log level so `E_DBG` could relax its
  `fflush()`-per-write in `log_entry()`. Grepping call sites confirmed
  `E_DBG` is already the *only* level used for per-primitive tracing
  (`E_INF` is per-frame/per-widget, not per-character; `E_WRN`/`E_ERR`
  are rare anomalies), and it's already opt-in — only `tests/*.c`
  (`E_ALL`) and a deliberate debugging session ever enable it. Turning
  `E_DBG` on already *is* the "I want a reliable, flush-per-line trace
  even if the process crashes mid-run" signal; a new level would just
  restate that. `log_entry()` left as-is.
  Verified: `make`/`make all`/`make test` warning-free, all 40
  assertions across `tests/*.c` still pass. `tools/pty_harness.py`
  against `ctui-demo` at 24x80 and again after a live `resize:40x100`
  (exercises `screen_alloc()`'s resize path for the new `out` buffer)
  renders identically to before the change. Confirmed the fixes
  introduce no new `[CTUI:WIDGET] - putc out of widget bounds`
  warnings by diffing `ctui.log` against an unmodified-`main` run of
  the same steps — the 22 that do show up are a pre-existing,
  unrelated demo-layout warning present on `main` too.
- [x] Follow-up consistency fix from the same review: `ctui_widget_dispatch_render()`'s
      `gfx_pending` registry (`src/core/widget.c`, a realloc-grown
      file-static array, same shape as `core/timer.c`'s own timer
      registries) had no reset counterpart, unlike `timer.c`'s
      `ctui_timer_reset()` right next to it in `ctui_app_init()`/
      `ctui_app_free()`. Not a real leak (`gfx_pending_count` already
      resets to `0` every frame in `ctui_widget_flush_gfx()`, so
      nothing stale is ever read) — just an inconsistency in the
      pattern. Added `ctui_widget_gfx_reset()` (`widget.c`/`.h`),
      doc-commented identically to `ctui_timer_reset()`, called from
      the same two spots in `ctui_app_init()`/`ctui_app_free()` right
      alongside it. Verified: `make`/`make all`/`make test`
      warning-free, all 40 assertions pass; `tools/pty_harness.py`
      against `ctui-kitty_demo` under `TERM=xterm-kitty
      KITTY_WINDOW_ID=1` confirms the reset fires once at init and the
      real Kitty APC transmission still goes out correctly afterward,
      no new "not bound" warnings in the log.

- [x] Test suite expanded to cover every `src/core/` subsystem that had
      no dedicated test yet, plus a `make coverage` target to measure
      it. New: `tests/compositor_test.c`, `tests/event_test.c`,
      `tests/group_test.c`, `tests/split_test.c`, `tests/widget_test.c`,
      `tests/screen_test.c`, `tests/log_test.c`; `tests/util_test.c`
      extended with `ctui_util_center_h()`/`ctui_util_truncate_str()`
      coverage it was previously missing entirely. `tools/coverage.sh`
      (invoked via `make coverage`) builds every `tests/*.c` against a
      `--coverage`-instrumented core+widgets, all sharing one `-o` name
      (`coverage/run`) across builds so gcc emits identical
      `.gcno`/`.gcda` paths for every translation unit and gcov's
      runtime *adds* each run's counts into the existing `.gcda`
      instead of overwriting it — one merged view of what the whole
      suite exercises, not just whichever binary ran last. Chose plain
      `gcov` (ships with gcc) over `lcov`/`gcovr` — neither was
      installed, and a per-file "N% of M lines" table is enough to
      answer "what's untested"; no HTML report needed yet.
      Result: `src/core` line coverage 57.5% → 71.1%. The real find was
      `ctui_screen_flush()` (`src/core/screen.c`) — its shadow-buffer
      diff/color-cache engine (contiguous-run cursor-position skip,
      unchanged-cell skip, BASIC/256/RGB escape emission) was
      **completely unexercised** (`screen.c` at 25.8%): every existing
      test asserted against `screen->cells` directly rather than the
      actual ANSI byte stream `flush()` writes to `STDOUT_FILENO`.
      `tests/screen_test.c` closes this to 95.8% by `dup2()`-ing
      `STDOUT_FILENO` to a pipe for the duration of each
      `ctui_screen_flush()` call (flush writes via a raw `write(2)`,
      not stdio, so this is the only way to capture it headlessly) and
      asserting byte-exact escape sequences. `event.c` (59.4% → 97.1%)
      and `widget.c` (85.7% → 98.1%) picked up their enum-name helpers,
      the "no app registered yet" reject-and-log paths (only reachable
      before any `ctui_app_init()` call in a process, since `g_app` has
      no public way to be unset afterward), and `puts_256()`/
      `puts_rgb()`.
      Deliberately not chased further: `gfx.c`/`term.c`/`input.c`
      sitting at 0% and most of `app.c`'s remaining 36% are
      real-terminal-only code (`ctui_input_loop()`'s byte decoding,
      real `SIGWINCH`, Kitty APC transmission, `ctui_app_run()`'s
      blocking loop) that `tools/ctui_test.h`'s headless harness
      structurally can't reach — see `CLAUDE.md`'s Testing section for
      why that's `tools/pty_harness.py`'s job, not a gap in the C
      suite. `log.c`'s remaining 7 lines are `vsnprintf`/`malloc`
      failure branches, not reachable without fault injection.
      Verified: `make test` — 126 assertions across 11 binaries, all
      passing; `make`/`make all` still warning-free.

- [x] `examples_apps/demo-advanced/` added: same header/main/footer shape as
      `demo`, but requests `CTUI_GFX_TRUECOLOR` outright (rather than
      `CTUI_GFX_KITTY`) so `g_gfx_mode` lands on truecolor on any capable
      terminal instead of whatever the highest tier happens to be, adds a
      new app-local `CTUI_NAVBAR` widget (`widgets/navbar.h/.c`) below the
      header — the same `CTUI_MENU_ITEM` list/toggle contract as `CTUI_MENU`,
      just walked LEFT/RIGHT instead of UP/DOWN and highlighted with a
      truecolor accent plus a full-width hue-sweep rule — and a new
      app-local `CTUI_SPLASH` widget (`widgets/splash.h/.c`) filling the
      main area: a small (29x12) procedurally-generated radial hue "aura",
      nearest-neighbor resampled up to whatever cell area its pane
      currently has via `ctui_util_rescale_i()` walked over both axes, the
      first consumer of that utility for a 2-D image resize rather than a
      single ramp/coordinate. Toggling a navbar item (debug info/dump
      palette/kitty image) splits the main area vertically exactly like the
      original `demo`'s menu-driven panel grid, just with the splash
      staying visible in the top pane instead of a menu. Promoted
      `dump_palette` from `examples_apps/demo/widgets/` to `src/widgets/`
      in the process — this is the second app to need it, which is exactly
      this repo's promotion trigger (see `CLAUDE.md`). Verified via
      `tools/pty_harness.py`: truecolor `48;2;r;g;b` backgrounds confirmed
      in the raw ANSI stream for the splash's aura, navbar LEFT/RIGHT/ENTER
      correctly drives the same split-reveal behavior as the original
      demo's menu (including the `kitty image` panel's existing
      "[kitty required]" text degrade, since this app never negotiates
      `CTUI_GFX_KITTY`), and a resize mid-toggle reflows both the splash
      and the revealed panel grid with zero new `E_WRN` log lines.
      `make`/`make all`/`make test` all warning-free.
- [x] `examples_apps/demo-advanced/` upgraded to request `CTUI_GFX_KITTY`
      (v1 requested `CTUI_GFX_TRUECOLOR` outright — see the entry above),
      same negotiate-down-on-a-plain-terminal contract the original `demo`
      already relies on. `CTUI_SPLASH` (`widgets/splash.h/.c`) now carries a
      real `CTUI_GFX_KITTY` render path via `ctui_widget_set_gfx_renderer()`
      — the same dual text/gfx pattern `src/widgets/kitty_image.c`
      established — transmitting a second, much higher-resolution (256x128)
      pixel buffer through `ctui_gfx_kitty_display()`, independent of the
      small 29x12 buffer still used for the truecolor char-cell degrade.
      Splash's content changed from a plain radial hue-sweep aura to a
      swirling "nebula": the same angle-driven hue sweep now perturbed by a
      3-term sine plasma (both hue and brightness), plus scattered
      single-pixel star sparkles in the pixel buffer (real resolution only
      the Kitty path has enough of for a sparkle to read as a point rather
      than noise). The char-cell version keeps the aura's circular alpha
      mask; the pixel version is unmasked and fills its whole rectangle,
      since a real image doesn't need transparency to avoid looking blocky.
      Verified via `tools/pty_harness.py` with `KITTY_WINDOW_ID`/`TERM`
      forcing the Kitty tier: both the splash (`i=2`) and the `kitty image`
      panel (`i=1`) transmit distinct Kitty APC payloads at the expected
      cell geometry, reflow correctly across a panel toggle and a resize,
      and re-running without `KITTY_WINDOW_ID` still negotiates down to
      truecolor and renders the char-cell nebula (confirmed via `48;2;r;g;b`
      backgrounds in the raw stream) — zero `E_WRN` log lines either way.
      `make`/`make all`/`make test` all warning-free.

## Known issues / deliberately deferred

- **`src/core` coverage gaps left by design, not oversight**: `gfx.c`/
  `term.c`/`input.c` (0%) and most of `ctui_app_run()` in `app.c`
  require a real terminal (`ctui_input_loop()`'s byte-level decoding,
  genuine `SIGWINCH`, Kitty APC transmission) that `tools/ctui_test.h`'s
  headless harness can't provide — `tools/pty_harness.py` is the
  intended tool for that layer, not more C-side tests. `log.c`'s last
  few lines are `vsnprintf`/`malloc` failure branches, unreachable
  without fault injection. Run `make coverage` for the current
  per-file breakdown.

- **`player` stutters every few seconds on longer real-world WAV files**
  (reported against an FL Studio export; a full clean stop-then-restart,
  not pitch tearing/glitching mid-sample, and it catches up ~100ms
  later). Not yet reproduced with the short synthetic test tones used
  during initial verification, so root cause is unconfirmed — leading
  suspects are the single-threaded 20ms timer tick occasionally missing
  its deadline (main loop busy elsewhere that tick) or an ALSA
  underrun/`-EPIPE` recovery cycle in `alsa_write()`. DESIGN.md flagged
  this exact tradeoff up front ("Revisit only if underruns actually show
  up in testing — don't preemptively build the threaded version"); this
  is that signal, but not chased down yet. Next step would be logging
  `alsa_write()`'s returned frame count and `playback_tick()`'s own call
  interval to see which side is actually missing its deadline.
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
- **Event `source` is still a flat string namespace under
  `CTUI_EVENT_SCOPE_GLOBAL`.** Fixed for an emitter that opts into
  `CTUI_EVENT_SCOPE_BUBBLE` (sets `.origin = self`) — see
  `EVENT_DESIGN.md` and the "Event bubbling" Architecture entry above —
  but `GLOBAL` itself is unchanged, so two instances sharing a source
  string still collide for any listener that hasn't opted in.
- ~~`CTUI_EVENT_SCOPE` is vestigial.~~ **Fixed** — `CTUI_EVENT_SCOPE_
  BUBBLE` now exists alongside `_GLOBAL`; see `EVENT_DESIGN.md`.
- **`CTUI_FOCUS_EVENT` and `CTUI_WIDGET_REDRAW` are declared (and
  named, in `ctui_eventtype_name()`) but never emitted.** Reserved
  event types with no producer yet.
- **No handler unregistration.** `ctui_event_register()` has no
  counterpart to remove a registration; fine while the demo's widgets
  live for the whole program, would matter for widgets that come and
  go dynamically.
- **No timer unregistration either**, same reasoning as event handlers
  above — `ctui_timer_register()`/`ctui_timer_register_synchronized()`
  return a `CTUI_TIMER *` but there's no `ctui_timer_cancel()` yet to
  do anything with it.
- **`CTUI_SPLIT` only divides evenly, no per-child weights/ratios.** On
  a short terminal, the demo's 2-pane split (menu + debug_info, each
  wanting 7 rows) gets capped to whatever the main area's inner height
  actually has and split 50/50 — which can clip the menu's last item
  (silently rejected by `ctui_widget_putc()`'s existing bounds check,
  not a crash, just a visual gap). A weighted split would let
  `debug_info`'s smaller 3-line content claim less space than `menu`'s
  7 lines instead of splitting blind down the middle.

## Next up

- Promote `CTUI_CLOCK` from its `examples_apps/clock/widgets/` staging
  spot to `src/widgets/`, once a second app actually needs it — see the
  `examples_apps/` entry above for the promotion criterion. (`CTUI_LIST`
  made this jump already, promoted alongside `player`.)
- `player`'s `CTUI_METER` (currently staged in
  `examples_apps/player/widgets/`) is a similar promotion candidate once
  a second app wants a level meter/viz.
- FLAC decoder for `player`, once the WAV decoder + `CTUI_DECODER`
  interface have proven themselves further — deferred from v1 by design,
  see `examples_apps/player/DESIGN.md`.
- Weighted/unequal `CTUI_SPLIT` panes, once the even-split limitation
  above actually bites on a real layout.
- ~~Per-instance event identity~~ — done via `CTUI_EVENT_SCOPE_BUBBLE`
  (`.origin = self`); see `EVENT_DESIGN.md`. Focus for raw input events
  (`CTUI_KEYPRESS_EVENT` has no origin widget without a focus concept)
  is still deferred — that's a separate design (`EVENT_DESIGN.md`'s
  Deferred section).
- True run-length diffing in `ctui_screen_flush` (skip ahead over runs
  of unchanged cells, not just per-cell compare).
