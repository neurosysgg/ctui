# ctui-demo-advanced — build process notes

## preamble

advanced-ish demo app for the ctui ui library, built entirely by claude code in one prompt with zero refinement. the main reason for this approach is to prove API sane-ness and simplicity.

A record of how this app got built in one session, kept for the same reason
`player/DESIGN.md` exists: so a future session (or a future me) doesn't have
to re-derive the reasoning from the diff alone. This one is written
*after* the fact rather than as a planning doc — it documents what was
asked, what was explored, and why each decision landed where it did.

## The prompt, verbatim

> lets make a demo-advanced example app.
>
> in principle, it's the same as the existing app, BUT;
>
> - truecolor (v1)
> - header stays
> - add a new navbar widget below the header
> -- includes current menu items, functionality is same
> - main area displays a small splash on startup, navbar-entries make it
>   split vertically and then share the lower split.
> -- use the integer resize util(s) on the splash img for this. use
>    paddings/margins/etc as needed to ensure an integer split if necessary
> - status area stays
> - make it pretty but dont overdo it

## Exploration, before writing anything

Read, in order: `examples_apps/demo/main.c` in full (the app being
extended), `src/core/util.h`/`.c` (`ctui_util_rescale_i()`,
`ctui_util_inset()`, `CTUI_MARGIN` — the "integer resize util(s)" and
"paddings/margins" the prompt named directly), `src/core/cell.h` and
`src/core/widget.h` (`CTUI_COLOR_MODE_RGB`, `ctui_widget_putc_rgb()` — how
truecolor actually gets to the screen), `src/core/split.h`/`.c`
(`ctui_split_layout()`'s exact binding order — confirmed a split child's
own `layout()` still runs, via `ctui_widget_init()`, *after* the split
assigns it a box, which is what makes the splash's own padding possible),
`src/widgets/menu.c`, `status.c`, `border.c`, `label.c`, `debug_info.c`, and
`examples_apps/demo/widgets/dump_palette.c` (every widget being reused or
mirrored), and `src/core/term.c`'s `ctui_init()` (confirmed requesting
`CTUI_GFX_TRUECOLOR` never gets silently upgraded to `CTUI_GFX_KITTY` on a
Kitty-capable terminal — negotiation only ever goes *down* from what's
asked).

## Decisions

**Truecolor (v1)**: request `CTUI_GFX_TRUECOLOR` instead of `demo`'s
`CTUI_GFX_KITTY` in `ctui_init()`. Confirmed in `term.c` that this pins
`g_gfx_mode` to truecolor on any truecolor-capable terminal (including a
Kitty one), rather than leaving it to whatever the terminal happens to
support — the literal reading of "truecolor (v1)" as the tier this app
proves out, the way `demo` proves out Kitty.

**Header / status stay**: reused `ctui_border_render`/`ctui_label_render`
and `ctui_border_render`/`ctui_status_render` verbatim, same colors, same
inset pattern (`ctui_util_inset()` off a sibling `*_border_widget`) as
`demo`. No reason to touch what the prompt said should stay.

**Navbar**: a new app-local widget (`widgets/navbar.h/.c`) rather than
reworking `CTUI_MENU` in place — it's a materially different render
(horizontal, LEFT/RIGHT instead of UP/DOWN) built on the same
`CTUI_MENU_ITEM` data shape, which is exactly "app-local widget, proven out
before promotion" territory per `CLAUDE.md`, not a stdlib change. Kept the
*same six items* `demo`'s menu has (`debug info`, `dump palette`,
`kitty image`, `whatever`, `1337`, `quit`) per "includes current menu
items, functionality is same" — including `kitty image`, even though this
app never negotiates `CTUI_GFX_KITTY` and so it always shows the existing
`ctui_kitty_image_render()` degrade text. That's not a bug: it's the same
graceful-degrade path the framework already guarantees
(`ctui_widget_dispatch_render()`), and dropping the item to avoid ever
exercising it would have contradicted "current menu items" more than
including it does.

**Splash + integer resize**: the prompt named the mechanism
(`ctui_util_rescale_i()`) directly, so the design worked backward from it —
a *real* small source resolution (29x12) that genuinely differs from
whatever cell area the layout gives it, so the rescale is doing real work
rather than being decorative. `ctui_splash_make()` procedurally builds a
radial hue-sweep "aura" once (no external image dependency, same spirit as
`ctui_kitty_image_make()`'s gradient), stored as an owned `rgba` buffer
with alpha marking "outside the aura." `ctui_splash_render()` maps every
destination cell back to a source pixel via `ctui_util_rescale_i()` on
both axes (nearest-neighbor) and paints via `ctui_widget_putc_rgb()`,
skipping alpha-0 cells so the shape reads as round rather than a hard box.
For the "paddings/margins as needed" half of that instruction: the splash
is a plain `CTUI_SPLIT_V` child, so `ctui_split_layout()` hands it a full
box before `ctui_widget_init()` calls its own `layout()` — `splash_layout()`
takes that box, copies it as its own "outer," and insets it by a 1-cell
margin via `ctui_util_inset()`, giving the aura breathing room inside
whatever pane it's currently sharing.

**Splash/panel split**: mirrors `demo`'s `main_split_layout()` /
`main_split_handle_panel_toggle()` pattern almost exactly — a
`CTUI_SPLIT_V` with the splash and a `CTUI_SPLIT_GRID` of panels as its two
children, `count` starting at 1 (splash fills the whole main area) and
flipping to 2 the moment any of `debug info`/`dump palette`/`kitty image`
gets toggled on via the navbar's `CTUI_VALUE_CHANGED_EVENT` (`source =
"navbar"`). Deliberately did **not** invent a weighted/uneven split (e.g.
"splash always gets 60%") — `CTUI_SPLIT_V`'s even division is what the
core already does and nothing in the prompt asked for more, per this
repo's "minimal by default" rule.

**dump_palette promotion**: `demo-advanced` needing `dump_palette` (for its
`kitty image`-adjacent panel grid) is literally the "a second app needs
it" trigger `CLAUDE.md` documents for moving a widget from an app-local
`widgets/` to `src/widgets/`. `git mv`'d both files; no include-path
changes needed since the `-Isrc` build already falls back to `src/widgets/`
once the app-local copy is gone.

## What got touched

- `examples_apps/demo-advanced/main.c` — new app, same 4-band layout
  (header/navbar/main/footer) as `demo`'s 3-band one.
- `examples_apps/demo-advanced/widgets/navbar.{h,c}` — new, app-local.
- `examples_apps/demo-advanced/widgets/splash.{h,c}` — new, app-local.
- `src/widgets/dump_palette.{h,c}` — promoted from
  `examples_apps/demo/widgets/` (git mv, not a copy).
- `Makefile` — new `ctui-demo-advanced` target, added to `examples`/`all`/
  `clean`.
- `README.md` — widget catalog list and example-app list updated (both
  were now stale: `dump_palette` existed in `src/widgets/` but wasn't
  listed; `demo-advanced` didn't exist).
- `PROGRESS.md` — a `Fixed / addressed` entry, same style/verbosity as the
  existing entries there.

## Verification

No visual-only "should work" — per `CLAUDE.md`'s testing philosophy,
actually ran it:

- `make ctui-demo-advanced`, then `make clean && make all && make test`
  from a clean tree — warning-free, all existing tests still pass (the
  `dump_palette` promotion didn't break `demo`'s build).
- `tools/pty_harness.py ./ctui-demo-advanced --steps "wait:0.3,raw"` —
  grepped the raw ANSI stream for `48;2;r;g;b` truecolor background
  escapes, confirming the splash's aura is genuinely painting in RGB, not
  just computing colors nobody emits.
- `tools/pty_harness.py` with `key:RIGHT`/`key:LEFT`/`key:ENTER` sequences
  — confirmed navbar selection moves, `*` marks toggle correctly, the
  status line picks up `you picked: <item>` from the `"navbar"`-sourced
  event, and the main area actually splits to reveal `debug info` (real
  text), `dump palette` (real color grid), and `kitty image` (the expected
  `[kitty required]` degrade text, since `CTUI_GFX_KITTY` is never
  negotiated here).
- `tools/pty_harness.py` with a `resize:` step, both before and after
  toggling a panel on — confirmed the splash and panel grid both reflow,
  and grepped `ctui.log` for `E_WRN` after each run: zero, both times.
- Deleted every scratch `ctui.log` produced along the way, per
  `CLAUDE.md`'s "clean up after verifying" rule — `tools/pty_harness.py`
  and `tests/` themselves are the only testing infra meant to persist.
