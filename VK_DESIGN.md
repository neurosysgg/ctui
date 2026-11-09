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
     write escapes; a vtable lets a backend take the same calls instead.
     Kitty stays the default, its path unchanged. Callers use three
     image paths, and the vtable is cut at their *semantic* calls, never
     at the wire (`*_escape`, `apc_*`, `reply*`, `probe_shm`, `shm_*`,
     `set_compression` stay kitty's own, below the vtable; ctui-wm's
     pass-through needs them, nothing else does):
     - **pixels a frame**: `display()`, `image_new/begin/commit/free()`
       (ctui-mus's five widgets, ctui-daw's screen, ctui-wm's
       audio/photo/viz, kitty_image). `begin()` already hands out a
       buffer the caller paints into, so a backend can hand out mapped
       staging memory: no copy before the upload.
     - **placed files + placeholder cells**: `place_file()` +
       `ctui_widget_put_kitty_placeholder()` (look/drawn.c's controls,
       look/icon.c, ctui-wm's widgets). Look paints RGBA, writes a PNG
       (look/png.c, write-only) and the terminal decodes it -- but
       icon.c places PNGs it didn't make (icon themes, a file's own
       picture), so the backend decodes PNG either way: the sink takes
       `place_file` as is, and the decoder is ctui-wm's xdg/png.c
       (already on core's `ctui_inflate_zlib`), promoted by the second
       consumer rule when `src/vk/` needs it. A `place_rgba` skipping
       look's write-then-read is an optimisation for later, if
       measuring asks (a control's PNG is written once per state, keyed
       by its hash). The placeholders need nothing new:
       a cell already carries the image id (fg RGB) and its row/col
       (`kitty_row/col`), so the text painter samples the image in the
       same pass as the glyphs; z under/over text is a per-image flag.
     - **animation** (`frame_file`, `animate`, `put/unput`): ctui-wm's
       webview only; left kitty-only until something outside ctui-wm
       wants it.
   - **the tier, without a tty**: `ctui_g_gfx_mode` is set only by
     `ctui_init()`, which a window host never calls -- a setter
     (`ctui_gfx_set_mode()`) for a sink host. Done, with the sink
     (`CTUI_GFX_SINK`, `ctui_gfx_set_sink()`, gfx.h; tests/gfx_sink_test.c). With the sink above, a
     widget's existing kitty renderer runs unchanged on the GPU (the
     tier means "pixel images available", not "a kitty terminal"), so
     the host negotiates `CTUI_GFX_KITTY` and every kitty widget works
     on day one. **`CTUI_GFX_GPU`** is only the extra tier for plots.
   - a widget's **gfx renderers per mode** (today one `gfx_render_mode`,
     matched exactly): a short table, tried GPU -> KITTY -> text, the
     first the session has runs. Only the plot widgets register a GPU
     one. This and `CTUI_GFX_GPU` wait for step 5: nothing has a GPU
     renderer before plots, so nothing could test them.
   - the **cell's pixel size**: already there -- `ctui_cell_px_set()`
     (term.h) overrides `TIOCGWINSZ`, and `input.c`'s pixel mouse reads
     it. The host calls it on every scale change.
2. **The Vulkan backend** (`src/vk/`, optional: built where Vulkan and
   glslc are, like ctui-player's ALSA): a screen sink + graphics sink on
   the *caller's* device, queue and command buffer. Per frame it records
   its passes and hands back the surface buffer and the copy regions for
   an image; it owns no window or swapchain, so it drops into
   syNESthesia's present path, and into a host (layer 3).
   - **Text**: cells -> records (glyph slot, fg/bg RGB, attrs), the
     changed cells' tiles painted (the sink's diff). `CTUI_CELL` already
     has RGB, 256 and basic colours and attrs: what widens is the
     *record* (cells.comp's `glyph | fg << 8 | bg << 12`: 8-bit glyph,
     4-bit roles), not the cell. To RGB the backend resolves basic/256
     (`ctui_gfx_ansi16_rgb()`, a 256 table) and `CTUI_COLOR_DEFAULT`,
     which in a terminal is the terminal's -- the host supplies a
     default fg/bg (the look's). Attrs from the 1-bit atlas: bold as a
     1 px smear right, dim as fg halfway to bg, underline/strike as
     rows, italic ignored at first. The atlas becomes a glyph cache
     keyed by codepoint with a 16-bit slot in the record (Spleen's ~980
     glyphs in the BDFs; the header ships 256), a missing glyph a
     visible box, logged once per codepoint. `CTUI_CELL_CONT` paints
     nothing (its lead spans two cells); clusters are boxes until a
     second font exists.
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

1. Core hooks (layer 1): the graphics sink and the tier setter, with
   tests (gfx_sink_test: a recording sink gets a kitty widget's calls
   in order after the screen sink's frame, flush last, nothing on
   stdout; kitty_protocol_test unchanged for the kitty path's bytes).
   **Done 2026-11-09.** Left for a pty: a put/unput under a sink
   dropped at the flush (only reaches the batch with a tty).
2. `src/vk/`: text through the sink, the painter moved from
   syNESthesia; a headless test pinning a frame against a host
   reference. syNESthesia's app/ui.c becomes the glue; its window's
   panels become ctui widgets. The test runs on whatever device there
   is, lavapipe (mesa's CPU Vulkan) where there's no card, and is
   skipped -- not failed -- with no Vulkan at all, so `make test` stays
   green on a machine without it; the pinned hash has to hold on both.
   **ctui's side done 2026-11-09** (`src/vk/vk.{c,h}`,
   `tests/vk/vk_test.c`, 15 checks, the validation layer clean on the
   RTX 3070 Ti). How it came out:
   - the whole screen is one grid, so no slices: the cells live in a
     buffer on the card between frames, the sink diffs each frame into a
     host copy of them, and a record uploads only the changed cells'
     *runs* (one VkBufferCopy each, from a mapped staging buffer a slot)
     plus the tile list, then one dispatch over the touched 16 x 16
     tiles. Barriers before, between and after, so the caller copies
     out of the surface right after `ctui_vk_record()`.
   - a cell on the card is 3 words: the glyph's slot (16 bits, a box at
     0xffff) | attrs, fg, bg. `CTUI_COLOR_DEFAULT` is a flag the shader
     resolves from push constants, so `ctui_vk_set_default()` repaints
     every tile without a cell going up again.
   - per swapchain image and tile row, the span it's behind on; regions
     merge rows with the same span (a fresh image: one region).
   - attrs: bold a 1 px smear, dim fg halfway to bg, underline and
     strike rows; italic not drawn. A missing glyph is a box, logged
     once; a width-2 glyph's CONT blank; placeholders blank until step 4.
   - Vulkan 1.0 calls only, the shader built for vulkan1.0: nothing
     asked of the caller's device but a compute queue.
   - not done: lavapipe isn't installed here, so the hash is pinned on
     the card only; syNESthesia's side (its app/ui.c onto this) is its
     own repo's step.
3. The host (layer 3), text only, with ctui-mus in it: its kitty widgets
   show their text fallback until step 4. Earlier than it was, because
   the host is where every later step gets measured, and it's the only
   way ctui-mus/-daw see any of this.
   **Done 2026-11-09** (ctui-mus `src/gui/host/`, `ctui-mus -w`; built
   where Vulkan, glslc, wayland-client and its protocols, xkbcommon are).
   How it came out:
   - no new core mechanism: the window is a `CTUI_INPUT_SOURCE` (an
     epoll fd over the connection, the key repeat's timerfd and a kick
     eventfd), its configure a `CTUI_RESIZE_EVENT` the run loop already
     handles, and the present runs from `ctui_input_on_wait()` -- after
     the frame is drawn, only when `ctui_vk_pending()` says something
     changed: an idle app acquires no image at all.
   - keys through xkbcommon (the compositor's keymap, compose and dead
     keys by locale, repeats at the compositor's rate), mapped to what a
     legacy terminal's bytes give: ctrl+letter its control byte,
     Backspace 0x7f, shift+Tab BACKTAB, shift folded into the char. The
     pointer as mouse events on the cell under it, with pixels; a wheel
     notch a scroll; cursor-shape-v1 for the arrow.
   - no `ctui_init()`: the host sets the tier (TRUECOLOR until step 4)
     and points stdout at /dev/null while open, so mouse modes and OSC
     titles written for a terminal never reach the launching shell.
     Core fix found on the way: `ctui_shutdown()` restored a never-saved
     (zeroed) termios when `ctui_init()` hadn't run; now it only gives
     back a terminal it took.
   - Vulkan 1.0 calls, FIFO, a recording a slot (two), the protocols'
     glue and the shader generated under ctui-mus's build dir, never
     in `vendor/ctui`.
   - verified: ctui-mus in its own window on river (2115x1360, 264x85
     cells, the RTX 3070 Ti), validation clean, its 59 suites and lint
     unchanged. One swapchain rebuild right after the first present
     (NVIDIA says suboptimal once), then steady. Left for the user's
     hands: typing, the mouse, resizing and closing it interactively.
4. Images (layer 2) through the sink: every existing kitty widget now
   draws in the host unchanged; look's controls via `place_rgba`.
   Measured: ctui-mus in the host vs. in kitty, the CPU a tick and
   frames a second -- the baseline plots have to beat.
5. Plots: `bars` and `grid` first (ctui-mus's spectrum and spectrogram
   as the first users, beside their kitty renderers), then `columns`
   and `polyline` (ctui-daw), each measured against step 4's number
   for the same widget.

## Open, the user's call

- Whether ctui-wm's own windows should ever present this way (its
  compositor), or kitty stays its display. **Parked as future ctui-wm
  research**, a rabbit hole worth its own notes: going *fully* (within
  reason) independent -- no river, no tuigreet -- a GPU Vulkan desktop
  of ctui-wm's own: its compositor presenting ctui apps through this
  backend, its own login, a Wayland compositor of its own or straight
  to KMS/DRM. Nothing in steps 1-5 depends on it; the backend owning
  no window or swapchain is what keeps that door open.

## Handoff from syNESthesia (2026-11-09)

Copied here, ready for step 2:

- `src/vk/fonts/spleen.h` + `LICENSE.spleen`: the atlases (8x16, 12x24,
  16x32, 32x64; 256 glyphs: Latin-1 + 32 extras). Regenerate with
  `tools/bdf2h.py OUT.h spleen-{8x16,12x24,16x32,32x64}.bdf` from
  https://github.com/fcambus/spleen/releases/download/2.2.0/spleen-2.2.0.tar.gz.
  12x24 lacks the arrows; every size lacks ▶◀✓✗ (draw them).
- `src/vk/shaders/cells.comp`: syNESthesia's painter (app/shaders/ui.comp)
  as it stands (notes/gui.md planned 8 x 8 tiles; the shader has
  16 x 16 -- the tile list's unit, decided when step 2 measures it): a
  workgroup a listed 16 x 16 tile; records = roles[16],
  slices (8 words: x y w h cell_off cols rows bg), tiles (slice, x | y <<
  16), cells (glyph | fg << 8 | bg << 12, roles not RGB yet: step 2
  widens a cell to RGB + attrs). Since rewritten for ctui's grid (step
  2 above); the Makefile builds `src/vk/` where `VK_OK`.

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
Then (2026-11-09): the host starts in ctui-mus (its first user;
syNESthesia keeps its own present path) and moves to `src/vk/` when
ctui-daw wants it too, by the examples_apps rule; the host reads keys
through xkbcommon (layouts, compose and dead keys for ctui's forms and
text inputs), unlike wl.c's evdev codes.
