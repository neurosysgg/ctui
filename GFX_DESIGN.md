# Graphics protocol support — design plan

Status: **Phases 1-3 implemented for `CTUI_GFX_ANSI256`** (this pass).
Phase 4 (per-widget renderer declaration, needed for Kitty and other
non-degradable protocols) is still just a plan — see "Resolved open
questions" below for what changed on the way to real code, and
`PROGRESS.md` for the concrete result. See `CLAUDE.md` for the
philosophy/conventions this plan is written to follow.

## Resolved open questions

The plan below was accurate in shape but glossed over a few things that
only surface once you try to actually write the code:

- **`CTUI_GFX_MODE` has to be a real bit-flag enum, not sequential
  indices.** As drafted (`ANSI16=0, ANSI256=1, TRUECOLOR=2, KITTY=3`),
  `caps & requested_mode` can't work — `ANSI16=0` ORs into nothing and
  matches any bitmask. Values are `1<<0 .. 1<<3` instead. `ANSI16`'s bit
  is unconditionally set by `ctui_gfx_detect_caps()` itself (no env
  sniffing for it at all) rather than relying on every terminal's env
  vars happening to imply it — that's the direct code expression of "no
  fallback path needs to exist below it."
- **`CTUI_COLOR_MODE_*` (per-cell: how `CTUI_CELL` encodes its own
  fg/bg) and `CTUI_GFX_MODE` (terminal-level: what the negotiated
  session supports) are two different enums, not one.** The former
  lives in `cell.h` next to `CTUI_COLOR_*`, since it's describing a
  cell's own representation, independent of what got negotiated at
  startup. Easy to conflate because both have a "256" tier; they're not
  the same axis.
- **The widget-level 256 entry point needed a name the doc's snippet
  never gave it.** Since `fg`/`bg` already reused as a 0-255 index is
  exactly the same width as the existing basic-palette `unsigned char`
  params, `ctui_widget_putc_256()`/`ctui_widget_puts_256()` have the
  *same signature* as `ctui_widget_putc()`/`puts()` — they differ only
  in setting `color_mode = CTUI_COLOR_MODE_256` on the cell. Added now;
  the `_rgb()` counterparts stay deferred until something actually
  needs truecolor (see Deferred).
- **Per-frame clearing has to reset `color_mode` explicitly, not just
  `ch`/`fg`/`bg`.** `ctui_compositor_clear()` runs before every widget
  draws each frame; if it left a cell's `color_mode` alone, a cell
  drawn via `putc_256` one frame and plain `putc` the next (at the same
  coordinate, e.g. a resized/re-laid-out widget) would keep
  interpreting the new plain `fg`/`bg` as a 256-index instead of a
  basic one. Fixed by having `ctui_compositor_clear()`/
  `ctui_screen_clear()` reset `color_mode` to `BASIC` alongside the
  existing `ch`/`fg`/`bg` reset, and having the plain (non-`_256`)
  `putc`/`puts` paths set `color_mode = CTUI_COLOR_MODE_BASIC`
  explicitly too rather than assuming the last clear already did it —
  belt-and-suspenders, cheap, and it's a real correctness invariant
  rather than defensive-for-its-own-sake validation.
- **The screen-flush cell diff (`ctui_compare_ctuicell`, shadow-buffer
  skip-unchanged-cells check) had to grow to cover `color_mode` and the
  rgb fields.** It only compared `ch`/`fg`/`bg` before; two cells with
  identical `fg`/`bg` numeric values but different `color_mode` (e.g.
  index 2 = green in `BASIC` vs. index 2 = a totally different color in
  `256`) would otherwise be wrongly treated as "unchanged" and never
  get their new escape emitted.
- **`g_gfx_mode` (the negotiated mode from `ctui_init()`) is not
  consulted by `ctui_screen_flush()` at all.** Per-cell emission is
  driven purely by each cell's own `color_mode` — that's what "widget
  just says 'this cell is yellow'; the flush layer decides how to
  encode that" means in Phase 3. `g_gfx_mode` exists for startup
  negotiation (this pass) and for Phase 4's widget-support validation
  (deferred); nothing today stops a widget from calling
  `ctui_widget_putc_256()` even under a session negotiated at
  `CTUI_GFX_ANSI16`. That's an accepted gap for this pass, consistent
  with "trust internal callers" — closing it is exactly what Phase 4's
  `ctui_app_init()` validation is for, once it exists.
- **Follow-up after Phase 1-3 shipped: hard-failing the whole app over
  one unmet *optional* graphics request was too blunt.** `ctui_init()`
  now takes `CTUI_GFX_MODE *mode` instead of a plain `CTUI_GFX_MODE`
  value — an unmet request (above the mandatory `ANSI16` floor, which is
  still a hard fail) rewrites `*mode` to the terminal's actual best tier
  and lets `ctui_init()` succeed, instead of returning -1. This makes
  "just ask for the nicest thing and gracefully degrade" the default
  shape rather than something every app has to hand-roll, and it's why
  `demo` (the only one of the 7 apps requesting anything above the
  floor) now runs unmodified on a plain terminal instead of refusing to
  start — verified directly: `TERM=xterm` negotiates `0x2 → 0x1` and
  `dump_palette` skips its ramp, `TERM=xterm-256color` negotiates `0x2`
  cleanly and the ramp draws. The negotiated value is equality-checked
  by the caller (`*mode == CTUI_GFX_ANSI256`), not bitmask-tested —
  correct specifically because `ctui_init()` never negotiates *up* past
  what was requested, only down, so the result is always either exactly
  the request or the floor it got downgraded to.

## Problem

Today "rendering" means exactly one thing: `CTUI_CELL{ch, fg, bg}` with
`fg`/`bg` a basic 9-value ANSI palette index (`CTUI_COLOR_*`), hardcoded
into ANSI SGR codes 30-37/40-47 by `ansi_fg_code`/`ansi_bg_code` inside
`ctui_screen_flush()` (`src/core/screen.c`). There's no notion of the
app asking for richer color, no capability check against the real
terminal, and no way for a widget to render via a fundamentally
different protocol (e.g. pixel graphics).

## Constraints going in

- `CTUI_CELL` stays the backing element — extend it, don't replace the
  concept.
- Apps must be able to request a specific graphics mode. **Revised after
  Phase 1-3 shipped** (see "Resolved open questions"): above the
  mandatory floor, an unmet request degrades instead of failing —
  `ctui_init()` takes the requested mode as an in/out pointer and
  overwrites it with whatever the terminal actually supports, so the app
  itself decides how to degrade (skip a richer-only widget, fall back to
  plain rendering, etc.) rather than the library refusing to start at
  all over one optional feature.
- Widgets must explicitly declare which renderer to use for which
  graphics mode; the existing widget API/logic (`ctui_widget_putc`/
  `puts`, `render()`/`layout()`) should not need to change for widgets
  that don't care about this.
- Basic ANSI (`CTUI_GFX_ANSI16`) is the mandatory floor: `ctui_init()`
  hard-fails if even *that* isn't available, independent of whatever
  richer mode the app additionally asked for. It's a reasonable
  assumption in 2026 that every real terminal clears this bar — no
  fallback path needs to exist below it. This is the *only* hard
  failure left at the terminal-capability level (see revision above);
  a widget-level requirement (Phase 4, deferred) is still a planned hard
  fail, since there's no text degrade-path for a graphics-only widget.

## Phase 1 — extend `CTUI_CELL`, don't replace it

Keep `ch`, `fg`, `bg` exactly as they are today (plain `unsigned char`,
`CTUI_COLOR_*` semantics unchanged) so every existing
`(CTUI_CELL){.fg = CTUI_COLOR_YELLOW, ...}` literal and every
`ctui_widget_putc(..., CTUI_COLOR_GREEN, CTUI_COLOR_BLACK)` call across
`src/widgets/*` and `examples_apps/*/main.c` keeps compiling completely
untouched.

Append new fields, defaulting to zero (`CTUI_COLOR_MODE_BASIC`) via
plain designated-initializer zero-fill:

```c
typedef struct {
  char ch;
  unsigned char fg, bg;      /* existing basic-palette index, unchanged */
  unsigned char color_mode;  /* CTUI_COLOR_MODE_BASIC (0, default)
                              * | CTUI_COLOR_MODE_256 | CTUI_COLOR_MODE_RGB */
  unsigned char fg_r, fg_g, fg_b;
  unsigned char bg_r, bg_g, bg_b;
} CTUI_CELL;
```

`fg`/`bg` double as the 0-255 index when `color_mode ==
CTUI_COLOR_MODE_256`; only `CTUI_COLOR_MODE_RGB` needs the extra bytes.
`ctui_widget_putc`/`puts` signatures are unchanged (still take basic
`unsigned char fg, bg`); add new opt-in `ctui_widget_putc_rgb()`/
`ctui_widget_puts_rgb()` for widgets that explicitly want truecolor.

## Phase 2 — capability detection + app-level negotiation

New `core/gfx.c`/`gfx.h`:

```c
typedef enum {
  CTUI_GFX_ANSI16 = 1 << 0,   /* mandatory floor, always set */
  CTUI_GFX_ANSI256 = 1 << 1,
  CTUI_GFX_TRUECOLOR = 1 << 2,
  CTUI_GFX_KITTY = 1 << 3,    /* pixel graphics protocol */
} CTUI_GFX_MODE;

unsigned int ctui_gfx_detect_caps(void);  /* bitmask of supported modes */
```

(bit flags, not the sequential `0,1,2,3` originally drafted here — see
"Resolved open questions".)

Detection is env-sniffing only for v1 (`COLORTERM=truecolor`/`24bit`,
`TERM` containing `256color`, `TERM=xterm-kitty` or `KITTY_WINDOW_ID`
set) — no escape-sequence capability queries yet.

`ctui_init(verbosity, CTUI_GFX_MODE *mode)` gains a second, **in/out**
argument (7 call sites, all in `examples_apps/*/main.c`; 6 just pass the
address of a local set to `CTUI_GFX_ANSI16` to stay behaviorally
identical to today, `demo` passes `CTUI_GFX_ANSI256` as this feature's
proving ground). Negotiation inside `ctui_init()`:

1. If `CTUI_GFX_ANSI16` itself isn't in the detected caps, `ctui_log(E_ERR, ...)`
   and hard-fail (`ctui_init` returns -1) — regardless of what was
   requested. This is the unconditional floor check, and the only
   hard-fail path left.
2. If `*mode` isn't in the detected caps, don't fail — log at `E_WRN`
   and overwrite `*mode` with the highest tier the terminal actually
   supports instead, then continue. The caller reads `*mode` back after
   the call to see what it actually got (e.g. `demo`'s `dump_palette`
   widget only draws its 256-color ramp if `*mode` still reads
   `CTUI_GFX_ANSI256` afterward — see `PROGRESS.md`).

Negotiated mode is stored as a new cross-file static in
`ctui_internal.h` (`g_gfx_mode`), same pattern as `g_app`/
`g_resize_pending` — reserved for Phase 4's future widget-support
validation, not read by `ctui_screen_flush()` itself (see "Resolved
open questions").

## Phase 3 — `ctui_screen_flush` honors the negotiated mode automatically

Escape emission switches on each cell's `color_mode`: `BASIC` → today's
codes, `256` → `38;5;n`/`48;5;n`, `RGB` → `38;2;r;g;b`/`48;2;r;g;b`. No
widget changes needed anywhere — this is purely a rendering-backend
concern, since color richness is orthogonal to widget logic (a widget
just says "this cell is yellow"; the flush layer decides how to encode
that for the negotiated mode).

## Phase 4 — per-widget renderer declaration, scoped to actual protocol widgets

The one place existing widgets could need *anything* — made a no-op
for all of them:

- `ctui_widget_make()` auto-sets a baseline `supported_gfx_modes`
  bitmask covering all three text tiers (`ANSI16 | ANSI256 |
  TRUECOLOR`), so `border`/`menu`/`label`/etc. qualify silently with
  zero code changes.
- Only a widget that fundamentally can't degrade to text (e.g. a
  future image/album-art widget) calls a new
  `ctui_widget_set_gfx_renderer(widget, CTUI_GFX_KITTY, render_fn)` to
  register an alternate `render()` callback and narrow its declared
  support to include that protocol.
- `ctui_app_init()` validates every widget's declared
  `supported_gfx_modes` against `g_gfx_mode` at startup: mismatch →
  `ctui_log(E_ERR, ...)` + hard fail. This only ever fires for a widget
  that opted into a protocol it doesn't actually support — never for
  ordinary text widgets, since their baseline bitmask always covers
  whatever text tier got negotiated.
- `ctui_app_render()`'s dispatch: use the mode-specific renderer if the
  widget registered one for `g_gfx_mode`, otherwise fall back to the
  widget's default `render`.

## Deferred (not this pass)

- `ctui_widget_putc_rgb()`/`puts_rgb()` and any widget actually using
  `CTUI_COLOR_MODE_RGB` — the cell fields and flush-side emission exist
  (cheap to build alongside 256 since it's the same switch), but no
  widget-facing entry point until something needs truecolor specifically.
- Phase 4 in full: `supported_gfx_modes` on `CTUI_WIDGET`,
  `ctui_widget_set_gfx_renderer()`, and `ctui_app_init()`/
  `ctui_app_render()` validating/dispatching on it. Not needed until a
  widget exists that can't degrade to text (a Kitty-graphics widget).
- Sixel, iTerm2 inline images.
- Any automatic degrade-to-text-art fallback for a graphics-only
  widget that doesn't get its requested protocol.
- The first real Kitty-graphics widget itself (proving-ground
  candidate later — `player`'s album art is a plausible fit).
- Escape-sequence-based capability *querying* (vs. env-sniffing).

## Blast radius summary

- `src/core/cell.h` — extended, not replaced.
- `src/core/screen.c` — `ctui_screen_flush()` gains mode-aware SGR
  emission.
- `src/core/term.c` / `src/core/term.h` — `ctui_init()` signature grows
  an in/out `CTUI_GFX_MODE *mode` param (negotiates down instead of
  failing above the floor; only the floor itself remains a hard fail).
- New `src/core/gfx.c` / `src/core/gfx.h`.
- `src/core/widget.h` / `widget.c` — new optional fields + setter,
  `ctui_widget_make()` unchanged in signature.
- `src/core/app.c` — `ctui_app_init()` validates widget/mode
  compatibility; `ctui_app_render()`'s dispatch picks the right
  renderer.
- 7 call sites in `examples_apps/*/main.c` — one new arg to
  `ctui_init()` (a `CTUI_GFX_MODE` local, passed by address), otherwise
  untouched, except `demo`, which also threads that same variable's
  address into `dump_palette` as `widget_data` so the widget can see
  whether its requested tier actually got granted.
- Zero changes required in `src/widgets/*` or any existing widget's
  color/render call sites.
