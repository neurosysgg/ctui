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
  alternate screen buffer, `SIGWINCH` handling. Kitty protocol / better
  input handling is a possible future upgrade, not a near-term
  priority.
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
  for when to reach for which.

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
- **Found, not fixed: `tools/pty_harness.py`'s `raw` step is broken.**
  Every step's loop iteration unconditionally drains+feeds+clears the
  captured byte buffer into `grid` *before* dispatching on the step's own
  kind -- so by the time a `raw` step's own handler runs, whatever bytes
  arrived during the preceding `wait`/`key`/etc. have already been
  consumed and discarded into `grid`, and `raw` prints an effectively
  empty buffer. `dump` still works (it reads accumulated `grid` state,
  not `buf`). Worked around this pass by writing a small throwaway
  non-pty C program that calls `ctui_screen_flush()` directly and reads
  its `write()` output, rather than fixing the harness -- flagging here
  since `CLAUDE.md`'s Testing section calls `raw` out specifically for
  checking literal emitted ANSI bytes (color codes), which is exactly
  what doesn't currently work.

## Known issues / deliberately deferred

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

- Fix `tools/pty_harness.py`'s `raw` step (see Known issues above) --
  have it snapshot/print the buffer the preceding step's preamble just
  drained instead of a fresh (empty) one.
- `CTUI_GFX_TRUECOLOR`/`ctui_widget_putc_rgb()`/`puts_rgb()`, once a
  widget actually wants truecolor -- the cell fields and flush-side RGB
  emission already exist (see `GFX_DESIGN.md`).
- GFX Phase 4 (`supported_gfx_modes` on `CTUI_WIDGET`,
  `ctui_widget_set_gfx_renderer()`), once a widget exists that can't
  degrade to text -- a Kitty-graphics widget for `player`'s album art is
  the plausible first candidate per `GFX_DESIGN.md`.
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
- Per-instance event identity, if/when a second instance of the same
  widget kind needs to emit distinguishable events.
- True run-length diffing in `ctui_screen_flush` (skip ahead over runs
  of unchanged cells, not just per-cell compare).
