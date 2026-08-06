# Adding support for a new graphics protocol

A step-by-step recipe for extending ctui's graphics layer, distilled
from actually shipping `CTUI_GFX_ANSI256`, `CTUI_GFX_TRUECOLOR`, and
`CTUI_GFX_KITTY` (`GFX_DESIGN.md` has the original design doc and
per-decision reasoning for all three; `PROGRESS.md` has the concrete
changelog entries). Use this when adding the next protocol -- Sixel or
iTerm2 inline images are the plausible next candidates, following the
same "Non-degradable protocols" path Kitty just did.

Two shapes of "new protocol," two different amounts of this recipe
apply:

- **A richer *color* encoding that a `CTUI_CELL` can degrade to/from
  text** (what ANSI256 and TRUECOLOR are) -- follow all of Steps 1-6.
- **A fundamentally different renderer that can't be expressed as a
  colored character cell at all** (Kitty pixel graphics, Sixel, inline
  images) -- Steps 1-2 still apply, but Step 3 onward gets replaced by
  Phase 4's per-widget renderer declaration, which is designed but not
  yet built (see "Non-degradable protocols" below).

## Step 1 -- add the capability bit

`CTUI_GFX_MODE` (`src/core/gfx.h`) is a bit-flag enum, not sequential
indices -- `caps & requested_mode` has to work, so the new tier needs
its own `1 << n` bit, not the next integer. Add detection logic to
`ctui_gfx_detect_caps()` (`src/core/gfx.c`): env-sniffing only, no
escape-sequence capability queries (see GFX_DESIGN.md's Deferred
section for why). If the new tier implies an existing one (the way
`COLORTERM=truecolor` implies 256-color support, and Kitty implies
both text tiers below it), OR that tier's bit in too -- callers rely on
this, not on re-deriving the implication themselves.

## Step 2 -- decide if `CTUI_CELL` needs new fields

Only needed if the protocol is a color encoding, not a pixel/image
protocol (those don't go through `CTUI_CELL` at all -- see "Non-degradable
protocols"). If it does: extend `CTUI_CELL` (`src/core/cell.h`) with new
fields, defaulting to zero/`BASIC` via plain designated-initializer
zero-fill, so every existing color literal and `ctui_widget_putc()` call
keeps compiling and behaving identically. Add a new `CTUI_COLOR_MODE_*`
value alongside `BASIC`/`256`/`RGB` for how a cell should interpret the
new fields.

Don't conflate this with `CTUI_GFX_MODE`. They're deliberately two
different enums: `CTUI_GFX_MODE` is what the terminal session
negotiated at startup; `CTUI_COLOR_MODE_*` is how one specific cell's
`fg`/`bg` (or new) fields should be read, independent of what got
negotiated. A widget can call a richer `putc_*` variant even under a
session that didn't negotiate that tier -- nothing at this layer
enforces the two line up (see Step 5's Phase 4 note).

## Step 3 -- widget-facing entry points

Add `ctui_widget_putc_<suffix>()`/`puts_<suffix>()` to
`src/core/widget.c`/`.h`, same shape as `ctui_widget_putc()`/`puts()`
but taking whatever the new encoding needs, and setting
`cell->color_mode = CTUI_COLOR_MODE_<NEW>` instead of `BASIC`. The
`_256` entry points reuse the existing `unsigned char fg, bg` params
(same width, reinterpreted as a palette index); `_rgb` needed new
`fg_r/g/b, bg_r/g/b` params since 24-bit color doesn't fit in one byte.
Existing `ctui_widget_putc()`/`puts()` signatures never change -- this
is purely additive.

## Step 4 -- `ctui_screen_flush()` emission

Add a case to `emit_color()`'s switch in `src/core/screen.c` for the
new `color_mode`, emitting whatever SGR (or other) escape the protocol
needs. This is the only place that needs to know the actual escape
sequence -- driven purely by each cell's own `color_mode`, never by the
negotiated `CTUI_GFX_MODE`, so richness stays orthogonal to widget
logic (a widget says "this cell is $COLOR"; flush decides how to
encode that for whatever's actually emitting bytes).

Two easy-to-miss spots that have to grow in lockstep, both bitten
during the ANSI256 pass:

- **The shadow-buffer diff** (`ctui_compare_ctuicell()`) and **the
  last-emitted-color tracking** (`color_changed()`) both compare fields
  by hand, not via `memcmp`. Both need a case for the new
  `color_mode` (compare whatever new fields it added), or two cells
  with the same *old* fields but different `color_mode` will wrongly
  read as unchanged/same-color and never get a fresh escape.
- **`ctui_compositor_clear()`/`ctui_screen_clear()`** must reset
  `color_mode` back to `BASIC` every frame, not just `ch`/`fg`/`bg` --
  otherwise a cell drawn via the new `putc_*` one frame and plain
  `putc()` the next, at the same coordinate (a resized/re-laid-out
  widget), keeps misinterpreting the new frame's basic `fg`/`bg` as the
  old richer encoding.

## Step 5 -- `ctui_init()` negotiation (only if introducing a new tier
above the current ceiling)

`ctui_init(int verbosity, CTUI_GFX_MODE *mode)` takes the requested
mode as an in/out pointer (`src/core/term.c`). It never negotiates
*up* past what was requested, only down to the terminal's actual best
tier -- the mandatory `ANSI16` floor is the only hard-fail case. This
usually needs no code changes for a new tier (the negotiation logic is
generic over the bitmask), but double check `gfx_max_supported()`
still picks the right "highest tier" if the new bit doesn't sort
correctly against the existing ones.

## Step 6 -- a proving-ground widget

Don't ship a new tier without something that actually exercises it.
The established pattern (`demo`'s `dump_palette` and `debug_info`
widgets): `main()` passes the address of its own `CTUI_GFX_MODE` local
as a widget's `widget_data`, post-negotiation, so the widget can read
back what tier it actually got and degrade its own rendering
accordingly -- e.g. `debug_info` only draws its truecolor hue-sweep row
when `*widget_data == CTUI_GFX_TRUECOLOR` *and* there's enough height,
falling back to just the text lines otherwise.

Watch the equality-vs-widen trap: `ctui_init()` only negotiates down,
so if `main()` requests the new tier, a widget checking for an *older*
tier's exact value (e.g. `dump_palette`'s `*gfx_mode ==
CTUI_GFX_ANSI256`) needs widening to also accept the new tier when the
new tier implies the old one (`*gfx_mode == CTUI_GFX_ANSI256 ||
*gfx_mode == CTUI_GFX_TRUECOLOR`) -- otherwise bumping the app's
requested tier silently regresses that widget's older demo on exactly
the terminals that support the most.

## Non-degradable protocols (Kitty, Sixel, inline images)

Built -- this is GFX_DESIGN.md's Phase 4, shipped for `CTUI_GFX_KITTY`
(see `PROGRESS.md`'s changelog entry). The gap it closes: everything
above assumes a "protocol" is just a richer way to color a text cell,
so any widget can ignore it and still render as plain text. A
pixel-graphics protocol can't degrade that way -- a widget built around
it has nothing sensible to draw if the protocol isn't available. The
shape, worked out against Kitty and generic enough for the next
non-degradable protocol (Sixel, iTerm2 inline images) to reuse as-is:

- `ctui_widget_make()` auto-sets a baseline `supported_gfx_modes`
  bitmask covering the three text tiers, so every existing widget
  qualifies with zero code changes.
- A widget that fundamentally needs the new protocol calls
  `ctui_widget_set_gfx_renderer(widget, CTUI_GFX_KITTY, render_fn)` to
  register an alternate `render()` and narrow its declared support to
  exactly that one bit -- there's no text tier left to fall back to.
- `ctui_app_init()` (now returning `int`, `0`/`-1` same convention as
  `ctui_init()`) validates every *top-level* widget's declared
  `supported_gfx_modes` against the negotiated `g_gfx_mode` at startup:
  the three text-tier bits are masked out of the check first (always
  satisfiable, since text rendering doesn't depend on `g_gfx_mode` at
  all -- see Step 4's `g_gfx_mode`-isn't-consulted-by-flush note above),
  so this only ever actually fires for a top-level widget that opted
  into a specific non-text protocol and didn't get it. Hard fail (`-1`,
  logged) on mismatch, since there's no text fallback to degrade to for
  something central to the app. "Top-level" matters: a widget nested
  inside a `CTUI_SPLIT`/`CTUI_GROUP` isn't reachable from the
  `widgets[]` array `ctui_app_init()` walks at all, so it's never
  validated this way -- see "Integrating a non-degradable protocol"
  below for why that's actually fine.
- **The actual render-vs-gfx_render decision is centralized in
  `ctui_widget_dispatch_render()`/`ctui_widget_flush_gfx()`**
  (`src/core/widget.c`/`.h`), not duplicated in every caller that walks
  widgets. `ctui_app_render()`, `ctui_split_render()`, and
  `ctui_group_render()` all route every widget through
  `ctui_widget_dispatch_render(widget, comp)` instead of calling
  `widget->render()` directly: a mismatched `gfx_render_mode` (0, or any
  tier other than the negotiated one) falls through to the ordinary
  `render()`; a match gets queued (a small file-static realloc-grown
  array in `widget.c`, same shape/reasoning as `core/timer.c`'s own
  registries) rather than drawn into the compositor immediately.
  `ctui_widget_flush_gfx(comp)` fires every queued widget's `gfx_render`
  and clears the queue -- `ctui_app_run()` calls it right after every
  `ctui_screen_flush()`, never before. That ordering is the one place
  Kitty-specific reasoning leaks into otherwise-generic plumbing, worth
  restating for the next protocol too: a pixel protocol's transmit
  function (`ctui_gfx_kitty_display()` for Kitty) writes bytes straight
  to stdout, bypassing `CTUI_CELL`/the compositor entirely. Firing it
  *before* that frame's flush would let the flush's shadow-buffer diff
  paint blank text back over the widget's cell region right afterward --
  those compositor cells are still their untouched default (the widget
  never calls any `ctui_widget_putc*()` variant), which reads as
  "changed" against the previous frame's shadow buffer on at least the
  first render.
  Because the decision point is shared, a Phase 4 widget behaves
  identically whether it's top-level or nested arbitrarily deep in
  splits/groups -- callers that walk widgets never need their own
  gfx-aware special case.

`kitty_demo` (`examples_apps/kitty_demo/`) is the minimal proving-ground
app this shipped with -- a procedurally-generated RGB gradient (no image
codec dependency) via `CTUI_KITTY_IMAGE`, not real album art; `player`'s
album art remains a plausible *content* candidate for this same
mechanism later, once something actually decodes image bytes.
`CTUI_KITTY_IMAGE` itself has since been promoted to `src/widgets/` (a
second consumer -- `demo`, see below -- needed it, per `CLAUDE.md`'s
promotion rule) and is a real toggleable `CTUI_SPLIT` child in `demo`,
not just a standalone app.

## Integrating a non-degradable protocol into a real, multi-widget app

A standalone single-purpose app like `kitty_demo` (one widget, one
`CTUI_GFX_MODE` request, nothing else competing for screen space) barely
exercises Phase 4 -- it never has to coexist with ordinary text widgets,
never needs to appear/disappear at runtime, and is *itself* the feature,
so refusing to start without it is the right call. Folding
`CTUI_KITTY_IMAGE` into `demo` (`examples_apps/demo/main.c`) as a
toggleable panel alongside `debug_info`/`dump_palette` -- an *optional*
extra in an app that has to keep working without it -- is a different
problem, and surfaced a real limitation plus two conventions worth
carrying into the next non-degradable protocol.

**A Phase 4 widget genuinely nested in a `CTUI_SPLIT`/`CTUI_GROUP`
needs nothing special at all -- build it exactly like any other
candidate pane.** This wasn't always true: the first pass at this
integration found that `ctui_split_render()`/`ctui_group_render()`
called each child's `render()` directly, so a nested widget's
`gfx_render` could never fire -- only the top-level `ctui_app_render()`
loop did the Phase 4 dispatch. Rather than route around that
per-protocol, it got fixed generically: `ctui_widget_dispatch_render()`
(see above) is now what `ctui_split_render()`/`ctui_group_render()` call
too, not just `ctui_app_render()`. The practical result in `demo`:
`kitty_image` is constructed exactly like `debug_info`/`dump_palette`
(`ctui_widget_make()`, one more line for
`ctui_widget_set_gfx_renderer()`), given a real `render()` fallback
(`ctui_kitty_image_render()`'s plain `"[kitty required]"` text) the same
way every widget needs a `render()` regardless, and swapped into
`main_split.children[1]` by the *same* generic toggle handler
`debug_info`/`dump_palette` already used -- no independent positioning,
no duplicated split-geometry math, no per-widget "is this terminal
capable" branching in `main()` at all. Picking "kitty image" on a
terminal that didn't negotiate `CTUI_GFX_KITTY` just shows the
placeholder text, `ctui_widget_dispatch_render()`'s ordinary fallback
path -- not a special case, the *same* path every mismatched widget
takes.

**Hiding still needs a convention, but nesting inside a split/group
gets it for free.** `ctui_widget_flush_gfx()` fires every widget queued
by `ctui_widget_dispatch_render()` that frame -- and a widget is only
ever queued if something actually called `ctui_widget_dispatch_render()`
on it. `ctui_split_render()` only visits `children[0..count-1]`; an
inactive pane (`count` too small to reach it) is never dispatched, never
queued, never retransmitted -- exactly the same "just don't call
render() on it" mechanism that already made a normal widget's split-pane
hide/show work, no extra code needed for the gfx-mode case. A Phase 4
widget that *isn't* reached through a split/group's own active-set
bookkeeping (an independent, self-positioned widget that needs to hide
itself) doesn't get this for free and still needs its own convention:
collapse to `w=0, h=0` in `layout()` and have `gfx_render` check
`self->w <= 0 || self->h <= 0` first and return without transmitting
(`ctui_kitty_image_gfx_render()` in `src/widgets/kitty_image.c` still
does this, defensively, even though `demo`'s actual usage never
exercises it now that the widget is a real split child) -- moving it
off-screen instead would fail `ctui_widget_init()`'s bounds check and
spam `E_WRN` every frame.

**A *top-level* non-degradable widget is still a harder commitment than
a nested one, by design.** `ctui_app_init()`'s hard fail is real and
intentional for a top-level widget (see above) -- it means "this app
cannot do its job without this protocol," which is exactly right for
`kitty_demo`'s single widget, and exactly wrong for an optional panel in
an app that has plenty else to show. The fix if you do want an optional
*top-level* (non-nested) gfx-mode widget: check `gfx_mode ==
CTUI_GFX_KITTY` once in `main()` and only construct/append it to
`widgets[]` inside that branch, same as any conditional feature. `demo`
doesn't need this trick for `kitty_image` specifically -- it sidesteps
the question entirely by never putting a non-degradable widget at the
top level to begin with, preferring nesting-with-a-real-fallback over
conditional-construction whenever the widget can naturally live inside
an existing split/group.

## Testing checklist

- `make` / `make all` build every binary warning-free
  (`-Wall -Wextra -std=c11`).
- `make test` still passes -- these don't exercise a real terminal, so
  they mainly guard against a new `color_mode` breaking existing
  widget/event/layout logic.
- Prove the new protocol's actual bytes, not just that it compiles. For
  a *degradable* color encoding: a throwaway non-pty C program that
  builds a widget, calls `ctui_app_render()`, and reads
  `screen->cells[...]` directly is the fastest way to confirm a cell's
  `color_mode` and encoding fields are what's expected, plus
  `tools/pty_harness.py`'s `raw` step against the real binary to confirm
  the actual escape sequence emitted. For a *non-degradable* protocol
  (no `CTUI_CELL` involved at all -- see above): `raw` against the real
  binary is the only way, since there's no cell to inspect -- this is
  exactly how `ctui_gfx_kitty_display()`'s chunking/framing got verified
  (reassembling the captured chunks and checking `m=1`/`m=0` sequencing,
  header keys, chunk count against the expected base64 length).
- `tools/pty_harness.py` against the real binary under different
  `TERM`/`COLORTERM` env combinations, to verify: the new tier renders
  when granted, degrades cleanly (correct fallback content, no crash)
  when the terminal doesn't support it, and doesn't regress an
  *existing* widget's behavior at an older tier (the
  equality-vs-widen trap from Step 6).
- Delete scratch test programs/binaries and `ctui.log` after verifying
  -- nothing from a testing pass should linger in the repo.

## Docs to update

- `GFX_DESIGN.md` -- append to "Resolved open questions" if anything
  in the original plan turned out to be wrong or incomplete once
  actually built (this has happened for every tier so far).
- `PROGRESS.md` -- a `[x]` entry under the changelog with what shipped,
  what broke and got fixed along the way, and how it was verified;
  remove the corresponding deferred bullet from "Next up" if there was
  one.
- This file, if the recipe itself needed correcting.
