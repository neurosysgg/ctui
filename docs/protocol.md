# Adding support for a new graphics protocol

A step-by-step recipe for extending ctui's graphics layer, distilled
from actually shipping `CTUI_GFX_ANSI256` and `CTUI_GFX_TRUECOLOR`
(`GFX_DESIGN.md` has the original design doc and per-decision reasoning
for both; `PROGRESS.md` has the concrete changelog entries). Use this
when adding the next tier -- Sixel, iTerm2 inline images, or filling in
Kitty (`CTUI_GFX_KITTY`, currently just a reserved bit).

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

Not yet built -- this is GFX_DESIGN.md's Phase 4, still a plan. The
gap it closes: everything above assumes a "protocol" is just a richer
way to color a text cell, so any widget can ignore it and still render
as plain text. A pixel-graphics protocol can't degrade that way -- a
widget built around it has nothing sensible to draw if the protocol
isn't available.

The planned shape:

- `ctui_widget_make()` auto-sets a baseline `supported_gfx_modes`
  bitmask covering the three text tiers, so every existing widget
  qualifies with zero code changes.
- A widget that fundamentally needs the new protocol calls
  `ctui_widget_set_gfx_renderer(widget, CTUI_GFX_KITTY, render_fn)` to
  register an alternate `render()` and narrow its declared support.
- `ctui_app_init()` validates every widget's declared
  `supported_gfx_modes` against the negotiated `g_gfx_mode` at startup
  -- hard fail on mismatch, since there's no text fallback to degrade
  to. This is the one hard-fail case below the mandatory floor.
- `ctui_app_render()`'s dispatch picks the mode-specific renderer if
  one's registered for the negotiated mode, otherwise the default.

`player`'s album art is the currently-plausible first candidate widget
for this, once it's built.

## Testing checklist

- `make` / `make all` build every binary warning-free
  (`-Wall -Wextra -std=c11`).
- `make test` still passes -- these don't exercise a real terminal, so
  they mainly guard against a new `color_mode` breaking existing
  widget/event/layout logic.
- Prove the new protocol's actual bytes, not just that it compiles: a
  throwaway non-pty C program that builds a widget, calls
  `ctui_app_render()`, and reads `screen->cells[...]` directly is the
  fastest way to confirm a cell's `color_mode` and encoding fields are
  what's expected. Redirect `ctui_screen_flush()`'s `stdout` write to a
  file and inspect the raw bytes to confirm the actual escape sequence
  emitted (`tools/pty_harness.py`'s `raw` step is currently broken --
  see PROGRESS.md's Known issues -- this is the workaround).
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
