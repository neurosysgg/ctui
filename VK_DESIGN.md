# A GPU backend (Vulkan) — design plan

Started 2026-11-09 from syNESthesia (../syNESthesia/notes/gui.md), whose
window got a GUI in its letterbox bars painted on the card: one surface
in VRAM, widgets as rects of it, records (cells) instead of pixels, a
compute pass over the changed tiles only, each swapchain image catching
up by copy regions. That reimplements ctui's core on the card; this plan
makes it a ctui backend instead, so every ctui app and widget gets it,
and moves the pixel work kitty has been slow at onto the GPU.

## Problem

- **Kitty's image path is the bottleneck wherever ctui draws pixels.**
  ctui-wm/notes/image-perf.md: a picture first drawn at a size costs
  13-46 ms of area-average + dither + PNG on the UI thread, then kitty
  decodes it again. ctui-wm/notes/daw-pixels.md: ctui-daw's timeline as
  one image at 60 fps is 85 % of a core (3444 x 2040 px, shm, t=s); 7
  fps without shm. ctui-mus's spectrum, spectrogram, oscilloscope and
  meters each paint RGBA on the CPU every tick (`*_kitty_gfx_render`),
  only to upload it.
- **What those widgets draw is small data**: 64 bars of dBFS, a 256 x N
  byte grid, a few hundred min/max/RMS columns, a handful of control
  states. The pixels are a pure function of it and a look. On the card
  that's a dispatch over a few KB, not megabytes through a pipe.
- syNESthesia needs ctui's widgets (menus, lists, forms, dialogs, focus)
  in its window, not a second toolkit.

## What exists already

- **Frames without a terminal**: `ctui_screen_set_sink()` (PROGRESS.md,
  2026-09-29, the OLED): a sink gets each changed frame, with the frame
  it took before -- a per-cell diff, i.e. exactly the dirty tiles.
- **Input without a terminal**: `ctui_input_set_source()`,
  `ctui_input_post()`: a window's keys and pointer become ordinary
  `CTUI_KEYPRESS_EVENT` / `CTUI_MOUSE_EVENT`s.
- **Per-widget renderers by negotiated mode**: GFX_DESIGN.md Phase 4,
  `ctui_widget_set_gfx_renderer()`. A GPU mode is one more tier.
- **Looks as data, controls as functions of state**: `look/` -- the
  split a shader wants.
- **The card's side, proven in syNESthesia**: the painter
  (app/shaders/ui.comp: cells through an atlas, integers only, pinned
  against a host reference), Spleen as the glyph atlas (BSD-2, 8x16 to
  32x64, a size per scale), the per-image generations, a Wayland window
  with fractional scale and presentation timing (app/wl.c,
  app/present.c).

## Shape

Three layers; only the first touches what every ctui app links.

1. **Core hooks, backend-neutral, libc-only** (`src/core/`):
   - a **graphics sink** beside the screen sink: today `ctui_gfx_kitty_*`
     write escapes; a vtable lets a backend take "image id, RGBA, put at
     cells, under/over text" instead. Kitty stays the default, its path
     unchanged.
   - **`CTUI_GFX_GPU`**, a negotiated tier, so `look/` draws controls
     and widgets pick their GPU renderer.
   - a widget's **gfx renderers per mode** (today one `gfx_render_mode`):
     a widget with both a kitty and a GPU renderer keeps both, the
     negotiated one runs.
   - the **cell's pixel size** from the backend, not `TIOCGWINSZ`.
2. **The Vulkan backend** (`src/vk/`, optional: built where Vulkan and
   glslc are, like ctui-player's ALSA): a screen sink + graphics sink on
   the *caller's* device, queue and command buffer. Per frame it records
   its passes and hands back the surface buffer and the copy regions for
   an image; it owns no window or swapchain, so it drops into
   syNESthesia's present path, and into a host (layer 3).
   - **Text**: cells -> records (glyph slot, fg/bg RGB, attrs), the
     changed cells' tiles painted (the sink's diff). The atlas becomes a
     glyph cache (Spleen's ~980 glyphs; width-2 and clusters later).
   - **Images**: kitty's model kept -- an image id, placements in cells,
     z under or over text -- but an image is a VRAM buffer, scaled and
     filtered by a shader at placement (area average, the halftone
     dither, whole-factor nearest for masks), so ctui-wm's 13-46 ms
     filter and the PNG round trip go.
   - **Plots** -- the new part, GPU-native: a widget hands over data and
     a kind, a shader draws it through the look: `bars` (values, a
     ramp: spectrum_bars, meters), `grid` (a byte matrix scrolled by a
     column a tick, a ramp: the spectrogram, kept as a ring in VRAM so a
     tick uploads one column), `columns` (min/max/RMS per column: a
     waveform, an oscilloscope), `polyline` (automation, curves). The
     record is the state, the look is a uniform; a plot's pixels are a
     pure function of both, so tests pin them like the painter's.
   - **Controls** (look/control.c): painted on the CPU as now and placed
     as images (cached by hash, uploaded once a change) to begin with;
     moved into shaders later only if measuring asks for it.
3. **A host**: a ctui app in a native Wayland window (syNESthesia's
   wl.c/present.c generalised): `ctui_app_run()` with the window's fd as
   an io watch, the sinks above, the swapchain. This is what makes the
   GPU path reach ctui-mus and ctui-daw: **inside kitty the output still
   goes through kitty's upload**, so their pixel widgets only get faster
   in a window of their own (or, later, if ctui-wm's own compositor
   presents them).

## Steps, each measured or pinned before the next

1. Core hooks (layer 1), with tests (screen_test, a new gfx sink test;
   ctui-wm's suites unchanged).
2. `src/vk/`: text through the sink, the painter moved from
   syNESthesia; a headless test pinning a frame against a host
   reference. syNESthesia's app/ui.c becomes the glue; its window's
   panels become ctui widgets.
3. Images (layer 2), the kitty_image widget and look's controls on it.
4. Plots: `bars` and `grid` first (ctui-mus's spectrum and spectrogram
   as the first users, beside their kitty renderers), then `columns`
   and `polyline` (ctui-daw).
5. The host (layer 3); ctui-mus in a window, measured against its
   kitty path (the CPU a tick, frames a second).

## Open, the user's call

- Where the host lives: in ctui (`src/vk/host.c`) or an app of its own.
- Whether ctui-wm's own windows should ever present this way (its
  compositor), or kitty stays its display.

## Handoff from syNESthesia (2026-11-09)

Copied here, ready for step 2:

- `src/vk/fonts/spleen.h` + `LICENSE.spleen`: the atlases (8x16, 12x24,
  16x32, 32x64; 256 glyphs: Latin-1 + 32 extras). Regenerate with
  `tools/bdf2h.py OUT.h spleen-{8x16,12x24,16x32,32x64}.bdf` from
  https://github.com/fcambus/spleen/releases/download/2.2.0/spleen-2.2.0.tar.gz.
  12x24 lacks the arrows; every size lacks ▶◀✓✗ (draw them).
- `src/vk/shaders/cells.comp`: syNESthesia's painter (app/shaders/ui.comp)
  as it stands: a workgroup a listed 16 x 16 tile; records = roles[16],
  slices (8 words: x y w h cell_off cols rows bg), tiles (slice, x | y <<
  16), cells (glyph | fg << 8 | bg << 12, roles not RGB yet: step 2
  widens a cell to RGB + attrs). Nothing builds `src/vk/` yet; the
  Makefile's globs don't reach it.

Read in place (syNESthesia, commit 7237758), to port rather than copy:

- `app/ui.c`, `ui.h`: the host side -- widgets as rects, the per-slot
  record buffers (the host writes one while the card reads the other),
  the tile list, per swapchain image and widget the generation held
  (`ui_regions`, `ui_pending`), `ui_glyphs()` (the largest size no
  taller than 16 px a logical pixel).
- `tests/ui_test.c`: the pattern for the vk test -- the card's pixels
  equal a host reference painted by the same rule, then the hash pinned.
- `app/gpu.c`, `gpu.h`: the device, storage buffers, pipelines from
  embedded SPIR-V (glslc `-mfmt=c`), two recordings in flight; what
  `src/vk/` takes from its caller.
- `app/present.c`: the swapchain, the bars cleared once per image,
  `syn_present_copy` taking the GUI's regions after the picture.
- `app/wl.c`: the Wayland window for the host (xdg-shell, fractional
  scale + viewporter, wp_presentation, keys by evdev code, no
  xkbcommon), and notes/window.md for what its timing taught.
- `notes/gui.md`: the step-1 write-up and its measurements (within noise
  of no GUI at 1646 x 1360; a repaint once a second for the status).

Decided with the user: Spleen, ctui's win95 look, keys and the mouse;
kitty stays the default everywhere; the GPU path is optional.
