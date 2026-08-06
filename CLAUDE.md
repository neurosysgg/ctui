# Working on ctui

Project background/architecture/status live in `README.md` and
`PROGRESS.md` — read those first. `docs/core-api.md` is the
subsystem-by-subsystem reference for `src/core/`'s public API (what's
in each header, the call sequence tying them together) — reach for it
before re-deriving a core function's contract from source. This file
is about *how* to work in this codebase: conventions, philosophy, and
patterns established so far, so a fresh session doesn't have to
re-derive them.

## Philosophy

KISS, but also clean and fast — mean & lean at the core especially.
Not a license for clever micro-optimization or premature tuning; it's
about noticing when the structurally elegant option is *also* the
simple one, instead of stopping at the first approach that works.

The canonical example: `CTUI_COMPOSITOR` is one allocation
(`comp->cells`) backing every widget's buffer — a widget's `buf` is
just a strided pointer into that single array, set up once by
`ctui_widget_init()`. No per-widget allocations, no widget-side memory
management, and `ctui_compositor_blit()` is one `memcpy()`. Simple to
build, simple to reason about, and fast — all at once, for free, not
traded off against each other. Same instinct elsewhere: the shadow-buffer
diff in `ctui_screen_flush()` (skip unchanged cells, track last
fg/bg so color escapes aren't repeated), and the event registry being
a flat realloc-grown array with linear-scan matching instead of a
hash map — a hash map would look more "proper," but at this scale
it's pure complexity with no payoff, while the array is simpler *and*
just as fast.

Weigh this against "minimal by default" (below): don't add
flexibility nothing asked for, but when building what *was* asked
for, look for the version that's also tight — especially in `src/core/`
itself, since every widget pays whatever it costs, forever.

This discipline matters more here than in a typical project: `.todo.md`
(untracked, local scratchpad — not a task list) sketches where this is
headed long-term — `ctui_sh`/`ctui_wm` as a home-grown kitty-compatible
desktop environment, `raw_86OS` piggybacking off `ctui` for its own
desktop layer. None of that is a near-term task. It's context for *why*
KISS-at-the-core isn't optional taste here: every one of those layers
inherits whatever complexity or sloppiness gets built into `src/core/`
today. Keep it mean & lean, or the whole stack stops being sane to
reason about a few layers up.

## Naming conventions

- Public structs: `CTUI_SHOUTING_CASE` (`CTUI_WIDGET`, `CTUI_BORDER`,
  `CTUI_MENU`). A widget's `widget_data` payload type is named after
  the widget, not suffixed `_DATA` (`CTUI_BORDER`, not
  `CTUI_BORDER_DATA`).
- Public functions: `ctui_snake_case`, prefixed by subsystem —
  `ctui_widget_*`, `ctui_group_*`, `ctui_split_*`, `ctui_event_*`. A
  widget's own callbacks are `ctui_<widget>_render`,
  `ctui_<widget>_handle_<event>` (e.g. `ctui_menu_handle_keypress`),
  never a bare `render`/`on_event` — once several widgets exist in one
  translation unit or get linked together, unprefixed names collide.
- Log tags: `[CTUI:SUBSYSTEM]` for anything that's a reusable widget or
  core mechanism (`[CTUI:WIDGET]`, `[CTUI:MENU]`, `[CTUI:EVENT]`) —
  applies whether it lives in `src/widgets/` already or is still staged
  under an example app's local `widgets/` pending promotion (see
  Examples apps below). `[<APPNAME>:APP]` (e.g. `[DEMO:APP]`,
  `[CLOCK:APP]`) is for code that only exists in that example app's own
  `main.c` — app-level composition, not a reusable widget.

## Code style

- No comments except where the *why* isn't obvious from the code: a
  hidden constraint, a subtle invariant, a workaround, a non-obvious
  ordering requirement. Never restate what a well-named line already
  says.
- Trust internal callers; validate at boundaries. Widget geometry,
  event payloads, etc. aren't re-checked defensively everywhere — the
  compositor-level bounds checks in `ctui_widget_putc()` are the one
  real safety net, and they're deliberately silent-reject-and-log, not
  fatal.
- Reject-and-log, not crash: an invalid call (out-of-bounds write, a
  string that won't fit, an unregistered app) logs `E_WRN` and no-ops
  rather than aborting. Matches the existing `ctui_log`/`ctui_logf`
  convention throughout `ctui.c`.
- Return convention: `0` success / `-1` failure for functions that can
  fail meaningfully (`ctui_util_center_h()`, `ctui_util_truncate_str()`);
  `1`/`0` for "did this produce a visible change" (`on_event`-style
  handlers, `ctui_handle_event()`'s aggregate return). No exceptions,
  no `errno`-style out-params beyond what's already there.
- Every non-trivial operation logs through `ctui_logf()` with
  `ctui_tick_advance()` for the timestamp — keep doing this for new
  code, it's how bugs in this project get diagnosed (grep the log,
  don't guess).
- Minimal by default: don't add config knobs, weights, or flexibility
  a feature doesn't need yet. `CTUI_SPLIT` only does even division
  because that's all that's been asked for — see `PROGRESS.md`'s
  "Next up" for what's deliberately deferred, not silently forgotten.

## Architecture patterns to follow

(`docs/core-api.md` covers every function referenced below in one
place, organized by header, if you need the full signature/contract
rather than just the pattern.)

- **Core (`src/core/`, fronted by `src/ctui.h`) vs. widgets
  (`src/widgets/`)**: the core has zero knowledge of any specific
  widget. Something belongs in the core only if it's a generic
  mechanism usable by *any* widget (`CTUI_GROUP`, `CTUI_SPLIT`, the
  event registry). A themed, content-bearing widget (border, menu,
  label, ...) belongs in `src/widgets/` as its own `.c`/`.h` pair,
  built entirely on the public `ctui.h` API — it should never need to
  reach into `src/core/` internals (or, for that matter, know that
  `src/core/` is split into multiple files at all).
  - `src/core/` is one `.c`/`.h` pair per subsystem (`screen`,
    `compositor`, `widget`, `event`, `app`, `group`, `split`, `term`,
    `input`, `log`, `util`, plus `cell.h` for the shared `CTUI_CELL`).
    `src/ctui.h` is the only header anything outside `src/core/`
    includes — it just `#include`s every `core/*.h` in dependency
    order, so adding a public declaration means adding it to the right
    `core/*.h`, not to `ctui.h` directly.
  - A few statics genuinely need to cross `src/core/`'s own internal
    file boundaries — `g_app` (set by `ctui_app_init()` in `app.c`,
    read by `ctui_event_register()`/`ctui_handle_event()` in
    `event.c`) and `g_resize_pending` (set by `term.c`'s `SIGWINCH`
    handler, polled by `input.c`). These live in
    `src/core/ctui_internal.h`, a header included only by `core/*.c`
    files, never by `ctui.h` — keep it that way; it's not part of the
    public surface.
- **`layout()` is how geometry becomes dynamic.** Don't compute a
  widget's `x/y/w/h` once and hardcode it — write a `layout()`
  callback that derives it from `comp->rows/cols` (and, for split
  children, from the parent's current geometry). It gets re-run by
  `ctui_widget_init()` automatically, both at startup and on every
  resize, so this is the only thing that needs to exist for a widget
  to reflow correctly — no special-casing resize elsewhere.
- **Groups vs. splits vs. independent widgets — three different
  relationships, pick the one that actually matches.**
  - `CTUI_GROUP`: members share the exact same `(x,y)` origin and paint
    over each other in sequence into the *same* cells (layered content
    within one box).
  - `CTUI_SPLIT`: divides one region into disjoint sub-areas, each
    child getting its own, different origin.
  - Neither: two widgets that don't overlap and don't need their
    positions managed relative to each other (e.g. a full-area border
    plus a separately, independently positioned inset content widget)
    are just two ordinary widgets with their own `x`/`y` — no
    group/split wrapper needed at all.

  Getting group vs. independent wrong is a real bug, not just a style
  issue: a group member positioned at a different origin than
  `members[0]` will silently draw at the *wrong* place, because
  `ctui_widget_putc()` addresses cells via `widget->buf +
  row*cols+col` — using the buf `ctui_group_init()` bound from
  `members[0]`'s origin — not the writing widget's own `x`/`y`.
- **Widgets talk through events, never through each other's structs.**
  If widget B needs to react to widget A's state changing, A emits a
  `CTUI_VALUE_CHANGED_EVENT` (or a new event type if none fits) via
  `ctui_handle_event()`, and B registers a handler with
  `ctui_event_register()`. B's handler should only ever read the event
  payload — never take a pointer to A's `widget_data` and reach into
  it directly. `source` is a plain string convention agreed between
  emitter and listener (`"menu"`, `"input"`, `"terminal"`), not derived
  from widget identity — see `PROGRESS.md`'s known issues for the
  current limits of that (no per-instance disambiguation yet).
- **Don't clear/reset ad hoc.** If something needs to reset state
  every frame (like the compositor), do it once in the shared render
  path (`ctui_app_render()`), not per-widget. The stale-content bug
  fixed this session was exactly a widget-by-widget assumption that
  didn't hold once geometry became dynamic.
- **`examples_apps/<name>/` is the widget stdlib's proving ground.**
  Every example app is `examples_apps/<name>/main.c` plus, if it needs a
  widget nothing else has needed yet, a local `examples_apps/<name>/
  widgets/`. Build it there first — an app-local widget is still built
  entirely on the public `ctui.h` API, still named/logged like a real
  widget (see Naming/Log tags above), it just isn't proven generic yet.
  It only moves to `src/widgets/` once a *second* app actually needs it;
  nothing gets promoted speculatively (see "minimal by default"). The
  `-Isrc` build lets `#include "widgets/foo.h"` resolve to an app's own
  local copy first and fall back to the real stdlib in `src/widgets/`
  only if there isn't one — so a promotion is just `git mv` plus
  deleting the app-local copy, no include-path surgery.

## Testing approach

Two harnesses, for two different layers — reach for the C one first;
it's faster and has no process/pty overhead. Drop to the pty one only
when the thing you're checking actually lives in the terminal I/O
layer.

### `tools/ctui_test.h` — widget/event/layout logic (default choice)

A header-only C driver, built purely on the public `ctui.h` API (see
`tests/menu_status_test.c` for a full example). A test file is
structured like a real app's `main()` minus `ctui_app_run()`: call
`ctui_log_init(verbosity)` instead of `ctui_init()` (no tty needed —
that's the whole point), build widgets and `ctui_event_register()`
wiring exactly like the app does, then drive it:

```c
ctui_app_render(&app, screen);
CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 4, "> alpha"), "...");
ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 5, "> beta"), "...");
ctui_test_resize(&app, screen, 40, 100);
return ctui_test_summary();
```

`ctui_test_key()`/`ctui_test_resize()` inject through the exact same
`ctui_handle_event()`/`ctui_app_resize()` paths a real keypress or
`SIGWINCH` would hit, then re-render — so `screen->cells` is the real
output, not a simulation of it. `ctui_test_cell()`/
`ctui_test_row_contains()` read it back directly for assertions.
`CTUI_TEST_ASSERT(cond, fmt, ...)` prints ok/FAIL per check;
`ctui_test_summary()` prints the tally and returns a process exit
code. New tests go in `tests/*.c`; `make test` builds and runs every
one, failing the build if any assertion fails.

This can't reach real-terminal concerns — it never calls
`ctui_input_loop()`, so it doesn't exercise raw-mode byte/ESC-sequence
decoding, actual `SIGWINCH` delivery, or the literal ANSI bytes
`ctui_screen_flush()` emits. For those, see below.

### `tools/pty_harness.py` — real terminal I/O layer

A pty-based driver for what only exists once a real terminal is
involved: `ctui_input_loop()`'s byte-level input parsing, genuine
`SIGWINCH` signal delivery, and the actual rendered ANSI stream. Spawns
the binary under a real pty, injects keystrokes/resizes on a timed
script, and prints back the rendered screen as a reconstructed text
grid instead of raw ANSI:

- **Quick smoke test**: `tools/pty_harness.py ./ctui-demo` — default
  steps (`wait:1,dump`) capture one rendered frame as a grid.
- **Interaction test**: `tools/pty_harness.py ./ctui-demo --steps
  "wait:0.3,key:DOWN,wait:0.1,key:ENTER,wait:0.2,dump"`. The initial
  `wait:0.3` before the first keystroke matters: `ctui_init()` calls
  `tcsetattr(..., TCSAFLUSH, ...)`, which discards any input already
  sitting in the terminal buffer, so a keystroke sent too early gets
  silently eaten. `key:` accepts `UP`/`DOWN`/`LEFT`/`RIGHT`/`ENTER`/
  `ESC`/`TAB` or any literal character.
- **Resize test**: `tools/pty_harness.py ./ctui-demo --rows 24 --cols 80
  --steps "wait:0.3,resize:40x100,wait:0.3,dump"` — a real
  `TIOCSWINSZ` ioctl on the pty, so the kernel delivers a genuine
  `SIGWINCH` to the child, same as an actual terminal resize.
- **Verifying rendered output**: always `dump` and read the
  reconstructed grid rather than eyeballing raw escape codes —
  stale-content and overlap bugs are easy to miss by inspection but
  obvious once rendered back to a grid. Use the `raw` step instead of
  `dump` only when you need the literal ANSI bytes (e.g. checking
  which color escapes were actually emitted).
- The same harness works for every binary under `examples_apps/`
  (`./ctui-clock`, `./ctui-file_browser`, ...) — just swap the binary
  path. Run `tools/pty_harness.py --help` for the full step vocabulary.

### Both harnesses

- **Cross-check against the log**: `ctui.log` (gitignored) records
  every `ctui_logf()` call. `pty_harness.py --grep '\[CTUI:EVENT\]'`
  (or `[CTUI:SPLIT]`, `putc out of widget bounds`, ...) prints matching
  log lines after the run; for `ctui_test.h` tests just `grep` it
  directly. Often faster and more precise than parsing terminal
  output, and catches internal state that never reaches the screen at
  all.
- **Always verify, don't just reason.** Several real bugs found this
  way (a `CTUI_GROUP` misuse, a split height overlapping a border, the
  stale-compositor-content bug) were only caught by actually running
  the app, not by re-reading the code. "Should work" isn't a
  substitute for actually running one of these.
- Clean up scratch test scripts/output/logs after verifying — nothing
  from a testing pass should linger in the repo or scratchpad.
  `tools/pty_harness.py`, `tools/ctui_test.h`, and everything under
  `tests/` are the testing infrastructure that *does* belong in the
  repo; don't delete them after a session.

## Workflow

- Build with `make` (`-Wall -Wextra -std=c11`) for the demo, `make all`
  for every binary under `examples_apps/`, `make test` for everything
  under `tests/`; a change isn't done until it compiles warning-free
  *and* has been run/verified per Testing above.
- Update `PROGRESS.md`/`README.md` when asked to document, or when a
  change is significant enough that a future session would otherwise
  have to rediscover it (new core mechanism, a real bug fix, a
  resolved "known issue"). Keep `PROGRESS.md`'s existing shape:
  Status / Architecture / Fixed-addressed / Known-issues / Next-up.
- Only commit when explicitly asked. Stage files explicitly (not
  `git add -A`) so nothing untracked slips in by accident.
- Never push without being asked — hand back the `git push` command
  instead and let the user run it.
