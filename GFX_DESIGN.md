# Graphics protocol support — design plan

Status: **Phases 1-6 implemented and verified**, including Phase 6's
`t=s` success path against a real Kitty terminal -- see its "Resolved
open questions" entry below. Phases 1-3 (RGB cell fields,
flush-side emission, `ctui_widget_putc_rgb()`/`puts_rgb()`, and
`debug_info`'s truecolor hue-sweep proving-ground widget) landed for
`CTUI_GFX_ANSI256`/`CTUI_GFX_TRUECOLOR` first. Phase 4 (per-widget
renderer declaration for non-degradable protocols) landed against
`CTUI_GFX_KITTY` — the actual Kitty graphics protocol wire emission
(`ctui_gfx_kitty_display()`, `core/gfx.c`) plus
`supported_gfx_modes`/`ctui_widget_set_gfx_renderer()` on `CTUI_WIDGET`
and `ctui_app_init()`/`ctui_app_render()`'s validate/dispatch, proven
out by `examples_apps/kitty_demo`. See "Resolved open questions" below
for what changed on the way to real code in both passes, `PROGRESS.md`
for the concrete changelog entries, and `docs/protocol.md` for the
recipe this generalized into for the *next* protocol (Sixel, iTerm2
images). See `CLAUDE.md` for the philosophy/conventions this plan is
written to follow.

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
- **Phase 4's validation check needed to mask out the text tiers, not
  compare `supported_gfx_modes` against `g_gfx_mode` directly.**
  `g_gfx_mode` is always exactly *one* `CTUI_GFX_MODE` bit (the single
  tier `ctui_init()` negotiated — see the bullet above), never a
  bitmask of everything the terminal supports. An ordinary widget's
  `supported_gfx_modes` is `ANSI16 | ANSI256 | TRUECOLOR` (bits 0-2). If
  `g_gfx_mode` were `CTUI_GFX_KITTY` (bit 3) — an app that asked for and
  got Kitty — a plain `supported_gfx_modes & g_gfx_mode` test would be
  `0` for *every ordinary text widget in the app*, since none of them
  carry bit 3, and `ctui_app_init()` would hard-fail an app for having
  a border widget. Fixed by masking `CTUI_GFX_ANSI16 | CTUI_GFX_ANSI256
  | CTUI_GFX_TRUECOLOR` out of a widget's `supported_gfx_modes` before
  the check: those three bits are unconditionally satisfiable no matter
  what got negotiated (text rendering goes through each cell's own
  `color_mode`, never `g_gfx_mode` — see the very first bullet in this
  section), so what's left (`required`) is only ever nonzero for a
  widget that actually opted into a specific non-text protocol via
  `ctui_widget_set_gfx_renderer()`. `kitty_demo`'s own border/label
  widgets are what surfaced this — they failed `ctui_app_init()` in
  first-draft testing, on the exact terminal (real Kitty) the whole app
  was built to run on.
- **A gfx-mode widget's pixels can't be drawn during
  `ctui_app_render()` itself — they have to happen after that frame's
  `ctui_screen_flush()`.** `ctui_gfx_kitty_display()` writes straight to
  stdout, bypassing the compositor. If it ran during the normal render
  pass (before `ctui_screen_flush()`), the widget's compositor cells
  would still be sitting at `ctui_compositor_clear()`'s default (the
  widget never calls any `ctui_widget_putc*()` variant) — on the first
  frame, that reads as "changed" against the screen's shadow buffer
  (which starts intentionally blank-but-different, forcing a full first
  draw), so `ctui_screen_flush()` would emit space characters directly
  on top of the pixels `ctui_gfx_kitty_display()` had just written a
  moment earlier. Fixed by never calling a gfx-mode widget's renderer
  during the render pass at all —
  `ctui_widget_dispatch_render()` (see the Phase 4 section below) queues
  it instead of drawing — and adding a `ctui_widget_flush_gfx()` step
  that `ctui_app_run()` calls immediately after every
  `ctui_screen_flush()` (all three call sites: initial draw, post-resize,
  post-event-change). On later frames this is moot either way (the
  widget's untouched cells compare equal to the previous frame's, so
  flush skips them and emits nothing there regardless of ordering) — but
  the first frame needs the ordering to be right, and there was no clean
  way to special-case "first frame only" that wouldn't be far more
  convoluted than just always drawing gfx widgets after flush.
- **A widget nested inside a `CTUI_SPLIT`/`CTUI_GROUP` never got its
  `gfx_render` dispatched at all in the first cut of Phase 4** —
  `ctui_split_render()`/`ctui_group_render()` called each child's
  `render()` directly, and only `ctui_app_render()`'s own loop knew to
  check `gfx_render_mode` first. This surfaced folding `CTUI_KITTY_IMAGE`
  into `demo` as a toggleable panel (`docs/protocol.md`'s "Integrating a
  non-degradable protocol" has the full narrative) — `demo`'s shared
  split-pane slot is exactly where the widget needed to live, and the
  first pass worked around the gap with an independent, always-top-level
  widget whose `layout()` duplicated the split's own division math to
  land in the same box. That workaround is gone: the fix was to pull the
  render-vs-gfx_render decision out of `ctui_app_render()` entirely, into
  the shared `ctui_widget_dispatch_render()`/`ctui_widget_flush_gfx()`
  pair every caller that walks widgets now uses (`ctui_app_render()`,
  `ctui_split_render()`, `ctui_group_render()`). A nested Phase 4 widget
  needs no special handling at all now — see the Phase 4 section — and
  "hidden" for a split/group child is free too (an inactive child is
  simply never dispatched, never queued, never retransmitted), unlike a
  truly independent gfx-mode widget, which still needs the `w=0,h=0`
  hiding convention documented in `docs/protocol.md`.
- **Testing the `demo` integration surfaced an unrelated, pre-existing
  bug in `main_split_handle_panel_toggle()`** (`examples_apps/demo/
  main.c`), not a Phase 4 issue at all: swapping the split's shared
  second-pane slot directly from one open pane to another (e.g. `kitty
  image` → `debug info`, without ever closing the pane in between) left
  the newly-swapped-in widget's `buf` unbound (`ctui_widget_putc()`
  rejecting every write, logged `E_WRN`, drawing nothing) — the handler
  only re-ran `ctui_split_layout()`'s binding when `split->count` itself
  changed, not when `children[1]`'s *identity* changed while `count`
  stayed at `2` the whole time. Latent since the original two-candidate
  (`debug info`/`dump palette`) version, never actually hit until a third
  candidate made the "switch without closing" path easy to reach in
  practice. Fixed by tracking whether the pane itself changed
  (`pane_changed`), not just whether `count` changed, and re-laying-out
  whenever either did.

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

**Shipped, against `CTUI_GFX_KITTY`, including a generalization the
original plan didn't anticipate** (see "Resolved open questions" for
the two follow-up fixes below). The one place existing widgets could
need *anything* — stayed a no-op for all of them:

- `ctui_widget_make()` auto-sets a baseline `supported_gfx_modes`
  bitmask covering all three text tiers (`ANSI16 | ANSI256 |
  TRUECOLOR`), so `border`/`menu`/`label`/etc. qualify silently with
  zero code changes.
- Only a widget that fundamentally can't degrade to text
  (`CTUI_KITTY_IMAGE`, `src/widgets/kitty_image.c` — a real toggleable
  panel in `demo`, not just `kitty_demo`'s standalone proving ground)
  calls `ctui_widget_set_gfx_renderer(widget, CTUI_GFX_KITTY,
  render_fn)` to register an alternate `render()` callback and narrow
  its declared support to *just* that protocol — not "include" it
  alongside the text tiers as originally drafted here, since a
  pixel-only widget has no text content to fall back to anyway (see
  "Resolved open questions" below for why the earlier wording was
  misleading).
- `ctui_app_init()` (now returning `int`, not `void` — same `0`/`-1`
  convention `ctui_init()` already used) validates every *top-level*
  widget's declared `supported_gfx_modes` against `g_gfx_mode` at
  startup: mismatch → `ctui_log(E_ERR, ...)` + hard fail (`-1`). This
  only ever fires for a top-level widget that opted into a protocol it
  doesn't actually support — never for ordinary text widgets, but *not*
  because their baseline bitmask "always covers whatever text tier got
  negotiated" (that's false whenever `g_gfx_mode` is `CTUI_GFX_KITTY`
  itself, see below) — because the check masks the three text-tier bits
  out of a widget's `supported_gfx_modes` before comparing, so text
  widgets are unconditionally exempt regardless of what got negotiated.
  A widget nested inside a `CTUI_SPLIT`/`CTUI_GROUP` isn't in the
  top-level `widgets[]` array `ctui_app_init()` walks at all, so it's
  never validated this way — see the dispatch bullet below for why that
  turns out fine rather than being a gap.
- **`ctui_widget_dispatch_render()`/`ctui_widget_flush_gfx()`**
  (`src/core/widget.c`/`.h`) are the single, centralized
  render-vs-gfx_render decision — not duplicated per caller. A widget
  routed through `ctui_widget_dispatch_render(widget, comp)` either
  calls its plain `render()` immediately (mismatch, or no gfx renderer
  at all) or gets queued in a small file-static array (same
  realloc-grown shape as `core/timer.c`'s own registries) for
  `ctui_widget_flush_gfx(comp)` to fire later. `ctui_app_render()`,
  `ctui_split_render()`, and `ctui_group_render()` all call this instead
  of `widget->render()` directly — originally only `ctui_app_render()`
  did (see "Resolved open questions" for the gap that left and how it
  got found). `ctui_app_run()` calls `ctui_widget_flush_gfx()` right
  after every `ctui_screen_flush()`, never before — see the first
  "Resolved open questions" bullet on this below for why the ordering
  matters.

## Phase 5 — Kitty write batching + payload compression (implemented)

Prompted by ctui-mus's own profiling pass (`.todo.md`'s "profile fft
kitty performance" entry, real numbers from `pty_harness.py` runs, not
guesses): at 80x24, `gfx.kitty_write` (the chunked `write()` loop in
`ctui_gfx_kitty_display()`) averages 85-111us with a 2.3ms max; at
180x50, same mechanism, 880-2350us avg with a **112ms max**. `gfx.
kitty_encode` (base64) only grows ~8-9x over the same resolution jump;
the write side blows up ~49x — worse than raw byte-count scaling alone
explains. Root cause isn't bytes, it's syscall count: every widget's
`ctui_gfx_kitty_display()` call does its own `write()` per escape
prefix, per 4096-byte base64 chunk, per chunk terminator (`\x1b\\`) —
for one modestly-sized image that's dozens of `write()`s, and
`ctui_widget_flush_gfx()` fires every Kitty widget on screen back-to-back,
synchronously, on the one thread also responsible for input handling and
the rest of that frame's render. A slow-draining pty turns that into a
visible freeze. Two sub-phases, decided together as one design pass
(see the two open questions each resolved below) but landing as
separate, independently-testable diffs.

### Phase 5a — batch every frame's Kitty writes into one `write()`

Same pattern `ctui_screen_flush()` (`core/screen.c`) already uses for
text: accumulate into a reused buffer, one `write()` at the very end,
instead of a `write()` per fragment. Today `ctui_gfx_kitty_display()`
writes straight to `STDOUT_FILENO` for the CUP escape, every chunk's
key-list prefix, every chunk's base64 body, and every chunk's `\x1b\\`
terminator — Phase 5a makes all of those append to a growable
file-static buffer instead (`core/gfx.c`, same realloc-doubling shape
`gfx_pending` already uses in `core/widget.c` — capacity grows to a
high-water mark and stays there across frames, never shrinks, len
resets to 0 each flush):

```c
/* core/gfx.c */
static char *g_kitty_batch = NULL;
static size_t g_kitty_batch_len = 0, g_kitty_batch_cap = 0;

static void kitty_batch_append(const void *data, size_t len) {
  if (g_kitty_batch_len + len > g_kitty_batch_cap) {
    g_kitty_batch_cap = g_kitty_batch_cap ? g_kitty_batch_cap * 2 : 8192;
    while (g_kitty_batch_len + len > g_kitty_batch_cap) g_kitty_batch_cap *= 2;
    g_kitty_batch = realloc(g_kitty_batch, g_kitty_batch_cap);
  }
  memcpy(g_kitty_batch + g_kitty_batch_len, data, len);
  g_kitty_batch_len += len;
}

void ctui_gfx_kitty_flush(void) {
  CTUI_PROFILE_SPAN sp = ctui_profile_begin();
  if (g_kitty_batch_len > 0) write(STDOUT_FILENO, g_kitty_batch, g_kitty_batch_len);
  ctui_profile_end(sp, "gfx.kitty_batch_flush");
  g_kitty_batch_len = 0;
}
```

`ctui_gfx_kitty_display()`/`ctui_gfx_kitty_delete()` change their
`write()` calls to `kitty_batch_append()`; every other line (chunking
logic, escape construction, `q=2`/`C=1` semantics) is untouched. Every
widget's `gfx_render()` — `loudness_meter.c`, `oscilloscope.c`,
`spectrum_bars.c`, `spectrogram.c`, `playlist_gfx.c`, `status_bar.c`,
`border.c`'s Kitty path, `kitty_image.c` — needs zero changes, same
"widget never sees the transport layer" property Phase 4 already
established.

**Hook point: `ctui_widget_flush_gfx()`** (`core/widget.c`), right
after its existing loop over `gfx_pending[]`. This is deliberate, not
incidental — it's the one place that already runs once per frame after
`ctui_screen_flush()`, for exactly the ordering reason "Resolved open
questions" documents above (gfx pixels must land after the text flush,
never before). Every one of `ctui_app_run()`'s three call sites
(initial draw, post-resize, post-event-change) gets the batching for
free without any new call site — same "centralize in the shared
dispatch function, not per-caller" precedent Phase 4's own fix used.

**What this fixes vs. doesn't** (the concurrency question resolved
this session): collapses N widgets × M chunks × 3 `write()`s into one
`write()` per frame, which is the multiplicative syscall overhead the
profiling numbers point at. It does **not** change the worst-case
stall duration if the terminal genuinely can't drain the bytes fast
enough — the same total byte count still blocks on that one call,
just consolidated into a single blocking write instead of many. Moving
transmission off the GUI thread entirely (a writer thread + pending
queue) would fix that residual case too, but ctui's GUI side has no
threading model to extend today and player-core/audiovis-core's
existing threads aren't reachable from here (`CLAUDE.md`'s "one hard
boundary" in ctui-mus's own `CLAUDE.md` — `src/gui/` is the only thing
allowed to touch `ctui.h` in the first place). Deferred, not ruled
out — revisit only if Phase 5a's real-terminal numbers still show
freezes once it's landed and re-profiled.

**Profiling continuity**: `gfx.kitty_write`'s meaning changes once
appends are cheap `memcpy()`s instead of syscalls — rename that span
to `gfx.kitty_batch_append` and add the new `gfx.kitty_batch_flush`
span above as the one that actually maps to the old `gfx.kitty_write`
syscall-cost numbers, so a before/after profile dump compares like
with like instead of silently comparing two different things under
the same name.

### Phase 5b — payload compression (`o=z`, zlib-wrapped DEFLATE)

Kitty's graphics protocol supports transmitting a zlib-compressed
payload (`o=z`) instead of raw bytes — orthogonal to `f=` (still `f=32`
raw RGBA, just deflate-compressed-then-base64'd instead of
base64'd-directly), and part of the core protocol spec rather than an
optional tier the way Sixel/iTerm2 images are. **Hand-rolled DEFLATE, no
external dependency** (`-lz` was the alternative — rejected to keep
ctui's zero-external-deps stance rather than take the smaller, safer
implementation). This was real new work, not a "simplest format first"
scale task like `src/image/decoders/bmp.c`:

- New `core/deflate.{h,c}`, same allocation-once-per-call,
  `CLAUDE.md`-conventions module as everything else in `core/` —
  compress-only (ctui never reads a Kitty APC reply at all, `q=2`
  everywhere already), so no inflate path needed.
- **v1 scope, deliberately narrow**: fixed Huffman codes only (RFC
  1951 §3.2.6) — skip dynamic Huffman tables (real zlib's biggest ratio
  win over fixed, and meaningfully more implementation complexity: a
  frequency pass + code-length optimization, not just a fixed lookup
  table). A plain greedy LZ77 hash-chain match finder (min-match 3,
  max-match 258, 32KB window per spec, capped chain-search depth) — not
  lazy matching or any of real zlib's optimal-parse tricks. zlib framing
  (RFC 1950: 2-byte header + Adler-32 trailer, `o=z`'s actual wire
  format) needs a small Adler-32 implementation alongside it.
- **Always compress, compare, keep the smaller — below a measured size
  ceiling (see the new "Resolved open questions" entry below).** No
  size-*ratio* heuristic — `ctui_gfx_kitty_display()` runs the raw RGBA
  through `ctui_deflate_compress()`, and only uses the compressed bytes
  (+ `o=z` in the escape key-list) if the result is actually smaller
  than the raw buffer; otherwise transmits raw exactly as today. This is
  a real invariant (never worse than today on the bytes actually sent),
  not a tuned-and-therefore-fragile threshold — matters because several
  existing Kitty widgets are tiny (`loudness_meter.c`'s baked-in text
  row, `self->h == 1`), where DEFLATE's own block overhead could
  plausibly lose against raw on a small enough image. `CTUI_DEFLATE_MAX_INPUT`
  (`core/deflate.c`) is a *different* kind of ceiling, added after
  landing this — a hard cap on even *attempting* compression above a
  measured size, independent of whether it would have won the ratio
  comparison. See below for why that turned out to be necessary.
- **Runtime opt-out, not a negotiated `CTUI_GFX_MODE` bit.** This
  isn't a terminal-capability question the way ANSI256/truecolor/Kitty
  itself are (no env var signals "this terminal's `o=z` support is
  unusually broken," and `o=z` is spec-mandated for any real Kitty-
  protocol terminal) — the actual risk is *our own new encoder* having
  a bug, not terminal incompatibility. So this gets a narrower knob
  than the `CTUI_GFX_MODE` bitmask machinery: a single
  `ctui_gfx_kitty_set_compression(int enabled)` (default on), independent
  of `ctui_init()`'s existing signature — adding a new required arg
  there for something that isn't a real per-terminal negotiation would
  touch all 7 `examples_apps/*/main.c` call sites for no real benefit.
  Same layering distinction "Resolved open questions" already draws
  between `CTUI_COLOR_MODE_*` (per-cell encoding choice) and
  `CTUI_GFX_MODE` (terminal-level protocol) — compression is an encoding
  choice *within* the already-negotiated Kitty tier, not a new tier of
  its own.
- New profile span, `gfx.kitty_compress`, wrapping just the deflate
  attempt — kept distinct from `gfx.kitty_encode` (base64) so a later
  profile dump can see whether compression's own CPU cost is actually
  worth what it saves on the write side, instead of the two blurring
  together. See "Resolved open questions" for what a real dump against
  ctui-mus actually showed.

### Phase 5 — resolved open questions

- **Verifying `ctui_deflate_compress()`'s output without an inflate
  path**: resolved in favor of shelling out to python3's stdlib `zlib`
  module at test time (`tests/deflate_test.c`), not vendoring a
  reference decompressor into the tree. Not a new class of dependency —
  `tools/pty_harness.py` already requires a `python3` on `PATH` to run
  any of this project's real-terminal tests, so this doesn't lower the
  bar for what a dev/CI environment needs. Each test case round-trips
  through a real subprocess (`mkstemp()` a compressed-bytes file,
  `system("python3 -c '...zlib.decompress...'")`, compare the recovered
  bytes against the original) rather than trusting the encoder's own
  logic to check itself. Covers: degenerate input, a small repeated
  phrase (confirms it actually shrinks), a 100000-byte input that spans
  past the 32KB window more than three times over (confirms the
  "chain only gets older" window-boundary logic doesn't produce a
  too-far back-reference), and incompressible random bytes (confirms
  the encoder still emits *valid* DEFLATE even when it can't win the
  size comparison — that case is exactly why `ctui_gfx_kitty_display()`
  never trusts compression to always help, see the "always compress,
  compare, keep the smaller" bullet above).
- **A hard cap on input size (`CTUI_DEFLATE_MAX_INPUT`, `core/
  deflate.c`) had to be added after the encoder was otherwise working**,
  found only by actually profiling it against real `ctui-mus` widgets
  under `pty_harness.py` (not reasoned out in advance — see `CLAUDE.md`'s
  "always verify" testing philosophy). `border.c`'s full-terminal
  background is a real, common case (every top-level Kitty border in
  `ctui-mus`, not a synthetic stress test): at a 140x40-ish terminal it's
  a ~2.4MB raw RGBA buffer, and compressing it cost ~14ms of GUI-thread
  CPU — the exact kind of stall Phase 5 exists to eliminate, just moved
  from the write side to the compress side. The surprising part:
  **lowering `CTUI_DEFLATE_MAX_CHAIN` from 64 down to 2 barely changed
  this number** (13.4-14.6ms across the whole range, measured with a
  synthetic border-shaped gradient buffer). Real image content resolves
  most hash-chain lookups in one or two probes regardless of the depth
  cap, so the cap was never the actual cost driver — the O(n) per-byte
  constant (hash computation, match-length extension, and especially
  `bw_put_huffman()`'s bit-at-a-time emission) is, and it scales
  linearly regardless: ~5.9us/KB held steady from a 238KB widget
  (~1.4ms) up to the 2.4MB border image (~14ms). Since a chain-depth cap
  can't bound this, `ctui_deflate_compress()` now declines outright
  above 512KB (comfortably above every *individual viz widget* size
  actually observed in a real run, up to ~493KB, while excluding the
  border and a couple of oversized ~1.1MB background widgets) — falling
  back to the existing raw-transmission path, which every caller already
  has to handle for the ratio-comparison case anyway. This is a
  different kind of decision than "no size-threshold heuristic" above:
  that one is about the *ratio outcome* being uncertain on small images;
  this one is a hard ceiling on *attempting the work at all*, justified
  by CPU cost alone, independent of what the outcome would have been.
  **Caveat, not fully resolved**: declining compression for the
  border-sized case doesn't make the underlying stall disappear, it
  relocates it — the same live profile run showed `gfx.kitty_batch_flush`
  (the one write() Phase 5a introduced) spike to a 14ms max once that
  widget's full, uncompressed ~3.2MB base64 payload had to go out in a
  single write() instead of a compressed ~24KB one. Both numbers were
  captured under `pty_harness.py`'s own Python-side reader, which (per
  the original Phase 5 profiling note) may itself drain slower than a
  real compiled terminal emulator, so neither 14ms figure should be read
  as what a real terminal would show. The cap is kept anyway because it
  trades an *unbounded-by-terminal-speed, always-paid* CPU cost for a
  *bounded, terminal-dependent* write cost that Phase 5a's batching
  already minimizes for every other widget — but the border-sized case
  specifically remains an open, unresolved tradeoff between the two, not
  a solved problem. A writer thread (already noted as deferred in Phase
  5a's own "what this fixes vs. doesn't") is what would actually resolve
  it, by taking either cost off the GUI thread entirely.
- **Update, later pass**: `bw_put_huffman()`'s bit-at-a-time emission
  (nbits individual 1-bit `bw_put_bits()` calls per Huffman code, each
  with its own byte-flush check) was replaced with a single reversal
  loop (plain shift/or, no function-call or flush overhead per bit)
  feeding one `bw_put_bits()` call per code — same output, since a
  fixed Huffman code packed as one LSB-first `nbits`-wide write is
  bit-for-bit identical to the old bit-by-bit MSB-first emission
  (verified: `tests/deflate_test.c`'s python3-zlib round-trip still
  passes, and a direct before/after benchmark produced byte-identical
  compressed output). Measured ~27% faster compression at both the
  238KB widget size and the largest real in-scope size just under the
  512KB cap (~493KB): 9.12ms → 6.70ms and 19.28ms → 14.00ms
  respectively. Doesn't change the ~5.9us/KB-scales-linearly shape or
  the border-image-exceeds-the-cap tradeoff above — Huffman emission
  was one contributor to that per-byte constant, not the whole of it —
  but it's no longer the easy, un-fixed part of it.

## Phase 6 — shared-memory Kitty transmission (implemented)

Prompted by a direct question: can the whole
`rgba → deflate → base64 → chunked-escape-writes → terminal decodes back
to rgba` round trip be skipped for a local terminal? Checked against the
actual Kitty graphics protocol spec (not assumed): yes.
`t=s` is a third transmission medium alongside today's `t=d` (direct,
what `ctui_gfx_kitty_display()` uses now) — the client writes raw RGBA
into a POSIX shared-memory object and sends a control string with only
the *object name* base64-encoded, not the pixel payload:

```
\x1b_Gs=<w>,v=<h>,t=s,i=<id>;<base64-encoded-shm-name>\x1b\\
```

The terminal `shm_open()`s that name, reads the pixels directly out of
the mapped region, and — per spec — **is itself responsible for
`shm_unlink()`/closing it**, not the client. That removes the
synchronization worry a naive reading of "shared memory" raises (client
doesn't need to know when it's safe to tear down the segment).
`o=z` compression is still a legal combination with `t=s` per the
spec's own example, but the point of this phase is that skipping it
becomes cheap to consider on its own merits: with no per-byte base64 or
chunking cost left to amortize against, compression's only remaining
job is shrinking the `write()` itself, not the CPU-bound encode — worth
re-measuring once this lands rather than assuming either way.

**What this eliminates from the current `t=d` path**: `ctui_deflate_compress()`'s
call (optional either way, see above), `ctui_util_base64_encode()` over
the full pixel payload (currently the second-largest cost after
compression per Phase 5b's profiling), and the `CTUI_KITTY_CHUNK`
chunking loop with its per-chunk escape framing — replaced by one
`shm_open()` + `ftruncate()` + `mmap()` + a pixel write (ideally the
widget renders straight into the mapped region instead of a separate
malloc'd `rgba` buffer that then gets `memcpy`'d in, though that's an
optimization to confirm is worth the API change once the basic path
works, not a v1 requirement) + one short control-string `write()`.

### Why this needs a capability probe first — the actual prerequisite

Nothing in the protocol tells a client in advance whether `t=s` will
work, and `ctui`'s Kitty path runs with `q=2` (all responses
suppressed) today — silence either way, no signal to fall back on. Real
Kitty client tools handle this by sending one *quiet-mode-off* probe
image early (a trivial 1x1) with `t=s` and reading back whether the
terminal actually acknowledged it, then remembering that result for
the rest of the session. This is exactly the "escape-sequence-based
capability querying" item `GFX_DESIGN.md` has carried in "Deferred"
since Phase 2 (env-sniffing only, v1) — Phase 6 is what finally forces
that to get built, rather than a speculative nice-to-have. Scope for
Phase 6 is therefore two pieces landing together, not one:

1. A one-shot startup probe (temporarily un-suppress responses, send a
   minimal `t=s` test image, parse the terminal's APC reply, cache the
   result in a new `g_kitty_shm_supported` alongside the existing
   `g_gfx_mode`) — falls back to today's `t=d` path permanently for the
   session on any failure (unsupported terminal, `shm_open()` failing
   locally, no reply within some bound).
2. `ctui_gfx_kitty_display()` branches on that cached result: `t=s` path
   when supported, today's `t=d` path unchanged otherwise. Every
   existing widget (`loudness_meter.c`, `oscilloscope.c`,
   `spectrum_bars.c`, `spectrogram.c`, `playlist_gfx.c`, `status_bar.c`,
   `border.c`, `kitty_image.c`) needs zero changes either way — same
   "transport is invisible to widgets" property every prior gfx phase
   has held.

### Remote/SSH — noted, deliberately not the design driver

The spec is explicit that shared-memory transmission is a local-only
mechanism (remote clients "must send the pixel data directly using
escape codes") — running over SSH, the client and terminal don't share
a filesystem or memory space, so `t=s` has to degrade to today's `t=d`
path automatically. The capability probe above already produces that
fallback for free (a probe sent over an SSH-forwarded pty simply never
succeeds, same code path as any other unsupported terminal — no
separate SSH-detection branch needed as a v1 requirement). Not
over-engineering around this case: ctui-mus is a local terminal music
player, not a remote-multi-user service — the realistic user is running
a real terminal on their own machine, and the SSH case degrading to
"exactly what already ships today" is an acceptable, low-priority
outcome rather than something worth a `$SSH_CONNECTION` special case
up front.

### Resolved open questions

- **Where the probe lives**: inside `ctui_init()` (`term.c`), right
  after raw-mode `tcsetattr()` (unbuffered byte-level stdin reads are a
  prerequisite for reading a bounded reply) and before the
  alternate-screen switch, gated on `g_gfx_mode == CTUI_GFX_KITTY` —
  every other negotiated tier never calls
  `ctui_gfx_kitty_probe_shm()` at all, verified directly
  (`TERM=xterm-256color` under `pty_harness.py` shows no probe log line;
  `TERM=xterm-kitty` shows one). This keeps the probe a true one-shot
  cost paid only by apps that actually asked for Kitty, not a tax on
  every `ctui_init()` call.
- **`a=q` (query) chosen over a real `a=T` transmission for the probe
  itself** — `a=q` per spec never displays or persists anything, so
  there's no visible flash and, on success, nothing left for the
  terminal to eventually `shm_unlink()` (unlike a real `t=s`
  transmission, where the terminal owns cleanup by design — see
  "Problem" above). The probe unlinks its own shm object unconditionally
  after the bounded wait either way, since `a=q` means it's the only
  side that could ever clean it up if the terminal doesn't understand
  `t=s`/`a=q` at all.
- **`q=0` (not `q=1`) for the probe specifically** — this is the one
  call in the entire Kitty path that isn't `q=2`. `q=1` only suppresses
  the *success* reply, and the probe needs exactly that positive signal
  (silence is ambiguous: "supported and quietly OK" is indistinguishable
  from "not a Kitty terminal, no reply ever coming" without it).
  Confirmed the ambiguity is real and not just a hypothetical: every
  `pty_harness.py` run in this environment produces exactly that
  silence, since it isn't a Kitty-protocol terminal at all.
- **Every shm segment gets a unique name** (`/ctui-k-<pid>-<seq>`, a
  per-process monotonic counter), not a name reused per `image_id`
  across frames — sidesteps the "is the previous frame's segment done
  being read yet" question entirely rather than trying to answer it,
  since the terminal (not the client) decides when to `shm_unlink()` a
  real transmission and nothing on the client side observes that event.
- **Local shm failures don't disable `t=s` process-wide.** `shm_open()`/
  `ftruncate()`/`mmap()` failing for one frame (e.g. a momentarily full
  `/dev/shm`) falls back to `t=d` for *that call only*, leaving
  `g_kitty_shm_supported` at 1 — a transient local resource problem
  isn't evidence the terminal itself lacks `t=s` support, so it
  shouldn't have that lasting an effect.
- **Compression's interaction with `t=s` stayed exactly what "worth
  re-measuring once this lands rather than assuming either way" said above:
  unresolved by design, not by omission.** The implementation makes no
  special case — whatever `ctui_gfx_kitty_display()`'s existing
  compress-and-compare step already decided (raw or `o=z`) just becomes
  the bytes written into the shm segment, same as it becomes the bytes
  base64'd for `t=d`. Nothing here re-measures whether compression is
  still worth its CPU cost once the write-side savings `t=s` provides
  are already in play; that measurement needs a real Kitty terminal, not
  this environment.
- **What's verified.** The probe→timeout→fallback mechanics were first
  verified in a real-terminal-less environment via `pty_harness.py` +
  `TERM=xterm-kitty` (adds no observable delay/behavior change for any
  non-Kitty terminal, leaves nothing behind in `/dev/shm` on failure).
  The actual `t=s` *success* path was then confirmed against a real
  Kitty terminal running `ctui-mus` fullscreen on a 4K display: the
  probe's `a=q` query got a genuine 11-byte `i=1;OK` reply
  (`ctui.log`'s `"kitty shm probe result: supported (t=s)"`),
  `g_kitty_shm_supported` latched to 1 for the session, and every
  subsequent `kitty_display (t=s)` call transmitted real payloads —
  including several individual images over 2MB (`payload=2140160` at
  608x880px, uncompressed since raw beat `o=z` at that size) — with
  zero `shm_open()`/`ftruncate()`/`mmap()` failures logged across the
  entire run. Visuals reported as flawless at 60+FPS perceived, well
  within `app.frame`'s measured ~220/s raw render throughput at that
  session's terminal size. This closes the one item this section
  originally flagged as needing "a real Kitty terminal, not just the
  spec prose" — the `a=q`/`t=s` reply format assumed above turned out
  to match what a real terminal actually sends.
- **Not resolved, deliberately left open**: `shm_open()`
  naming/permissions portability beyond this Linux/glibc environment
  (macOS's POSIX-shm quirks, a sandboxed `ctui-mus` not sharing
  `/dev/shm` with the terminal's mount namespace) and whether rendering
  widgets directly into the mapped shm region (vs. `memcpy`-ing an
  existing `malloc`'d `rgba` buffer into it, which is what shipped) is
  worth a widget-facing API change — both stay exactly as originally
  scoped below, unresolved until something actually needs them.

## Deferred (not this pass)

- Sixel, iTerm2 inline images — same Phase 4 mechanism, new wire-format
  functions following `docs/protocol.md`'s recipe.
- Any automatic degrade-to-text-art fallback for a graphics-only
  widget that doesn't get its requested protocol.
- A *real* Kitty-graphics widget with actual image content (decoded
  PNG/JPEG album art, say) — `kitty_demo`'s `CTUI_KITTY_IMAGE` proves
  the mechanism with a procedurally-generated gradient instead, deliberately
  avoiding an image-codec dependency; `player`'s album art is still the
  plausible next consumer once something actually decodes image bytes.
- Escape-sequence-based capability *querying* (vs. env-sniffing) for
  anything **other than** the Phase 6 `t=s` probe — e.g. querying
  ANSI256/truecolor support directly instead of env-sniffing stays
  deferred; Phase 6 only needs its own narrow one-shot probe, not a
  general querying mechanism.
- Phase 5's writer-thread / non-blocking-transmission alternative (see
  Phase 5a) — revisit only if batching alone doesn't hold up under a
  real terminal, not preemptively. The border-sized-image caveat in
  Phase 5's own "resolved open questions" is the concrete case that
  would motivate revisiting this first, if a real terminal (not just
  `pty_harness.py`) ever shows it stalling.
- Dynamic Huffman coding for Phase 5b's DEFLATE encoder — fixed-only
  for v1, see that section.

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
- Phase 5 (implemented): `core/gfx.c`/`gfx.h` gained the batch buffer +
  `ctui_gfx_kitty_flush()` + `ctui_gfx_kitty_set_compression()` (5a/5b);
  new `core/deflate.{h,c}`; `core/widget.c`'s `ctui_widget_flush_gfx()`
  gained one call to `ctui_gfx_kitty_flush()`; new `tests/deflate_test.c`.
  `src/ctui.h` gained one include (`core/deflate.h`). Zero changes
  required in `src/widgets/*`, any existing widget's Kitty call sites,
  or ctui-mus's `src/gui/widgets/*` — same "transport is invisible to
  widgets" property as Phase 4 (verified directly: `ctui-mus` rebuilt
  and ran correctly against this Phase 5 vendor/ctui with no source
  changes of its own beyond one stale profile-span-name comment in
  `src/gui/main.c`).
- Phase 6 (implemented): `core/gfx.c` gained
  `ctui_gfx_kitty_probe_shm()` + `g_kitty_shm_supported` +
  `kitty_shm_name()`/`ctui_gfx_kitty_reply_is_ok()`, and
  `ctui_gfx_kitty_display()`'s body split into `kitty_display_td()`
  (the pre-Phase-6 base64/chunking path, unchanged) and the new
  `kitty_display_shm()`; `gfx.h` gained `ctui_gfx_kitty_probe_shm()`'s
  declaration. `core/term.c`'s `ctui_init()` gained one conditional call
  to it. New `tests/kitty_protocol_test.c` cases for the reply parser.
  Zero changes required in `src/widgets/*`, any existing widget's Kitty
  call sites, `ctui_init()`'s call signature (all 7
  `examples_apps/*/main.c` sites untouched), or ctui-mus's
  `src/gui/widgets/*` — same "transport is invisible to the caller"
  property as Phases 4 and 5.
