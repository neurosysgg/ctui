# Working on ctui

Project background/architecture/status live in `README.md` and
`PROGRESS.md` — read those first. This file is about *how* to work in
this codebase: conventions, philosophy, and patterns established so
far, so a fresh session doesn't have to re-derive them.

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
for, look for the version that's also tight — especially in `ctui.c`
itself, since every widget pays whatever it costs, forever.

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

- **Core (`ctui.c`/`ctui.h`) vs. widgets (`src/widgets/`)**: the core
  has zero knowledge of any specific widget. Something belongs in the
  core only if it's a generic mechanism usable by *any* widget
  (`CTUI_GROUP`, `CTUI_SPLIT`, the event registry). A themed,
  content-bearing widget (border, menu, label, ...) belongs in
  `src/widgets/` as its own `.c`/`.h` pair, built entirely on the
  public `ctui.h` API — it should never need to reach into `ctui.c`
  internals.
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

This is a raw-mode terminal app — you can't just run it and read
stdout normally. What's worked this session:

- **Quick smoke test**: `script -qc "timeout 1 ./ctui-demo" /dev/null | cat -v`
  captures one rendered frame's raw ANSI as text you can read directly.
- **Interaction test**: pipe delayed keystrokes in —
  `(sleep 0.3; printf '\x1b[B'; sleep 0.1; printf '\r') | script -qc "timeout 1 ./ctui-demo" /dev/null`.
  The `sleep 0.3` before the first keystroke matters: `ctui_init()`
  calls `tcsetattr(..., TCSAFLUSH, ...)`, which discards any input
  already sitting in the terminal buffer, so a keystroke sent too
  early gets silently eaten.
- **Resize test**: needs a real controlling terminal, which `script`
  doesn't give you enough control over. Use Python's `pty.fork()`
  (it sets up the child as a session leader with the pty as its
  controlling terminal for you) plus `fcntl.ioctl(master_fd,
  termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))` — the
  kernel delivers real `SIGWINCH` to the child when the size actually
  changes. Use non-blocking reads with `select()`-based "drain until
  quiet" polling, not blocking reads assuming EOF — the demo never
  sends EOF, it just idles waiting for input, so a blocking read loop
  hangs forever.
- **Verifying rendered output**: for anything beyond "does this
  substring appear," reconstruct the actual screen grid from the raw
  ANSI stream (track cursor-position escapes, place characters,
  handle `\x1b[2J` as a full clear) rather than eyeballing escape
  codes — stale-content and overlap bugs are easy to miss by
  inspection but obvious once rendered back to a grid.
- **Cross-check against the log**: `ctui.log` (gitignored) records
  every `ctui_logf()` call. Grepping it for specific tags
  (`[CTUI:EVENT]`, `[CTUI:SPLIT]`, `putc out of widget bounds`, ...)
  is often faster and more precise than parsing terminal output, and
  catches internal state that never reaches the screen at all.
- **Always verify, don't just reason.** Several real bugs this session
  (a `CTUI_GROUP` misuse, a split height overlapping a border, the
  stale-compositor-content bug) were only caught by actually running
  the demo, not by re-reading the code. "Should work" isn't a
  substitute for a pty run.
- Clean up scratch test scripts/output/logs after verifying — nothing
  from a testing pass should linger in the repo or scratchpad.
- The same patterns apply verbatim to every binary under `examples_apps/`
  (`./ctui-clock`, `./ctui-file_browser`, ...) — just swap the binary
  name.

## Workflow

- Build with `make` (`-Wall -Wextra -std=c11`) for the demo, `make all`
  for every binary under `examples_apps/`; a change isn't done until it
  compiles warning-free *and* has been run/verified per Testing above.
- Update `PROGRESS.md`/`README.md` when asked to document, or when a
  change is significant enough that a future session would otherwise
  have to rediscover it (new core mechanism, a real bug fix, a
  resolved "known issue"). Keep `PROGRESS.md`'s existing shape:
  Status / Architecture / Fixed-addressed / Known-issues / Next-up.
- Only commit when explicitly asked. Stage files explicitly (not
  `git add -A`) so nothing untracked slips in by accident.
- Never push without being asked — hand back the `git push` command
  instead and let the user run it.
