# A GPU backend (Vulkan) — design plan

Started 2026-11-09 from syNESthesia (../syNESthesia/notes/gui.md), whose
window got a GUI in its letterbox bars painted on the card: one surface
in VRAM, widgets as rects of it, records (cells) instead of pixels, a
compute pass over the changed tiles only, each swapchain image catching
up by copy regions. That reimplements ctui's core on the card; this plan
makes it a ctui backend instead, so every ctui app and widget gets it,
and moves the pixel work kitty has been slow at onto the GPU.

## Where it stands (2026-11-09, end of the second session)

Steps 1-4 done (each below has its "how it came out"): core hooks, text
on the card, the host (ctui-mus `-w`), every kitty image path on the card
(pixels a frame, placed files, placeholders, put, the drawn look with no
PNG round trip). The user's verdict: 1:1 with kitty bar the fonts.
Second session: the baseline (~26 % playing with the visualizers), perf
on it, look's paint primitives by rows (-> ~10-12 %), the spectrogram's
grid scaled on the card via `ctui_gfx_display_px()` (main thread ~5 %),
and placeholder columns past 255 (the seek bar). Next, in this order:

1. ~~Toward the display's rate~~, the playbar: done (2026-11-09) --
   `ctui_gfx_wipe()` (a slider's track filled / empty / thumb, look's
   `CTUI_LOOK_CTL_PART_*`, composed on the card at a split that moves
   px_per_s from when it was set), `ctui_drawn_wipe_id()`, the host
   handing each frame's time to `ctui_vk_at()` and waking (a timerfd)
   exactly when the next column is due. The user: smooth. Left, if
   wanted: the plots' motion (bars' falloff, a spectrogram glide by
   `display_px`'s rect) on the same frame time.
2. ~~Fonts~~: done (2026-11-09), "Fonts, how it came out" below.
3. What's left of the ~5 %: kept for ctui-daw, post v0.1 (the user:
   every % counts there), only if measuring asks: spread thin now
   (perf, flat: `mus_tile_render` ~11 %, `box_px` ~9 %, memcpy ~25 %
   in all, `mark_id`, `ctui_icon_slot`, look hashing ~3-5 % each). perf
   records no call stacks here (perf_event_paranoid 2: `-e
   cpu-clock:u`, no `--call-graph dwarf`), so a target needs a
   `ctui_profile` span first. `bars` / `columns` / `polyline` on the
   card wait for the same.
4. ~~syNESthesia onto `src/vk/`~~: done (2026-11-09, syNESthesia
   cf58c84: app/ui.c the glue), then its input through `src/wl/`.

**A window** (2026-11-09): the host's Wayland window is ctui's now,
`src/wl/` (`CTUI_WL`, wl.h), promoted from ctui-mus's
src/gui/host/wl.c when syNESthesia, with a present path of its own,
wanted its input: xkbcommon keys, the pointer, repeats, cursor shapes,
plus what syNESthesia's own window had -- wp_presentation timing and
keys by evdev code before they're events (`ctui_wl_set_raw_key()`).
The rest of the host (the swapchain, the present on wait) stays in
ctui-mus until a second app wants it as it is.

**Several surfaces** (2026-11-10, ctui-wm's notes/kittyless.md z1):
`CTUI_WL_DPY` the connection, `CTUI_WL` a surface on it -- a window,
or a layer of the desktop (`src/wl/layer.h`, optional) -- so one
process paints a desktop's panels on one device. The second user the
host waited for was ctui-wm's panel host: it's in `src/vk/` now
(kittyless.md z2) -- `present.h`, a device (`CTUI_VK_GPU`) and a
presenter per surface (`CTUI_VK_WIN`), and `host.h`, the one-window
`CTUI_HOST` ctui-mus had, on top.

Loose ends: the host is ctui's since 2026-11-10 (above), so ctui-daw's
window builds on it. Tidied 2026-11-10: vk_test's hashes hold on lavapipe
too (`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
./test-vk_test`: 53 passed, the same three hashes as the RTX 3070 Ti);
look_test's literals designated, no -Wmissing-field-initializers left;
ctui-wm's xdg/png.c gone for `look/png_read.c` at its bump. Pushed:
everything but ctui's 74f0513 (`wl`) and ctui-mus's b1975d2, and
what came after them.

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
   an image -- or paints straight into the image, where storage images
   allow (`ctui_vk_set_direct()`, 2026-11-11: no surface buffer); it owns
   no window or swapchain, so it drops into
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
   - the hash holds on lavapipe too (checked 2026-11-10, see "Where it
     stands"); syNESthesia's side (its app/ui.c onto this) was its own
     repo's step (cf58c84).
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
   draws in the host unchanged; look's controls via `place_file`.
   Measured: ctui-mus in the host vs. in kitty, the CPU a tick and
   frames a second -- the baseline plots have to beat. Split in two,
   by what ctui-mus's widgets actually call:
   - **4a, pixels a frame -- done 2026-11-09** (`ctui_vk_gfx_sink()`):
     `display`, `image_begin/commit`, `delete`. Each image keeps its
     texels on the host (`image_begin()` hands that copy out: no copy
     there) and goes up into one arena buffer on the card at an offset
     (bumped; when full, made again at twice the live texels and
     everything goes up again from the host copies). Placements (cells,
     z) go up sorted by z then id; the painter composites per pixel as
     kitty does -- z < 0 under the glyph (below `Z_UNDER_BG` only on a
     default background), z >= 0 over it, alpha blended -- each image
     scaled to its rect by area average (at most 16 x 16 texels a side
     per pixel; nearest when enlarged). An image's old and new rects'
     tiles are marked, so the tiles/regions machinery repaints and
     presents only them. `image_free` keeps the image (kitty does).
     tests/vk/vk_test.c 25: four images pixel-exact against the
     reference (enlarged, shrunk, half transparent, under `UNDER_BG`),
     one tile for the same image again, a delete's tiles, the hash
     pinned. ctui-mus `-w` now negotiates `CTUI_GFX_KITTY`: its
     visualizers and covers draw on the card.
     **Measured (the user, playing, visualizers on screen): ctui-mus
     ~95 % of a core in kitty, ~9 % in its own window** -- but with the
     drawn look still off there (4a). Not the baseline: see 4b's.
   - **4b, the drawn look -- done 2026-11-09**:
     - ctui-wm's PNG decoder promoted to `look/png_read.c`
       (`ctui_look_png_decode/load/size`) with its test and Python-made
       cases (tests/png_test.c, tests/data/png/, tools/png_cases.py);
       ctui-wm drops its xdg/png.c at its next bump.
     - `CTUI_GFX_SINK` gains `put`/`unput` (core): only frame_file/
       animate and reply stay terminal-only.
     - placements are their own list, (image, placement id) -- display/
       commit placement 0, put any other; `delete` takes the image and
       all of them; replacing an image keeps them (kitty's graphics.c).
     - `place_file` decodes the PNG into an image with a virtual size;
       a placeholder cell carries a flag, its image row | column in the
       slot and the id as fg, the row/column resolved left to right as
       kitty reads the diacritics (and a cell right of a re-resolved one
       resolved again). The painter looks the id up in an image table
       (a few dozen icons: a linear scan, on dirty tiles only) and fits
       the image into its virtual cells, the aspect kept and centred,
       as a z 0 layer: over the under-text placements, under the over-
       text ones. Re-placing a file repaints the tiles of exactly the
       cells and placements showing it.
     - a placeholder's resolved column was 8 bits: a control wider than
       255 cells (the seek bar on a 3832 px screen) showed its left end
       again past them. Now 16 bits, the row in the attrs' byte (a
       placeholder draws none); vk_test runs one past 255.
     - vk_test 25 -> 33, pixel-exact; ctui-mus `-w` draws its look
       (faces, bevels, pictograms, sliders) on the card, validation
       clean.
     - **Then the CPU went up** (the user: higher than 4a's ~9 %): a
       drawn control that changes (the seek slider, every tick) was
       painted, written as a PNG and read back -- 0.04 ms painting,
       0.9 ms of PNG for a 1300 x 16 slider, ~4.7 % of a core at 50 Hz
       for that one. So `place_rgba` after all: `ctui_gfx_place_rgba()`
       (core: -1 without a sink) and the sink member; look/drawn.c,
       under a sink (`ctui_gfx_sunk()`), hands the pixels over and
       writes no file -- a static control once per slot
       (`ctui_icon_slot()`), a live one only when it changed.
       tests/gfx_sink_test.c checks both, and that no file appears.
       Idle in its window: ~1 % of a core. **The baseline step 5 is
       measured against: ctui-mus playing, look on, after this** -- the
       user's number: **ctui-mus `-w` playing, look on: ~26 % of a core
       with the visualizers on screen, 2-4 % without** (2026-11-09) --
       ~22 % is the visualizers' alone. Its log at that run: `app.frame`
       ~2.4 ms at ~60 frames/s (~14 % of a core), of which the
       spectrogram's `pixel_fill` ~0.6 ms, the bars' stamp + display
       ~0.06 ms; the oscilloscope and meters have no spans.
       **perf on that run** (main thread ~18 %, every other thread ~0):
       66 % of samples in `ctui_look_paint_box` + `_rect` -- each
       visualizer's well (`mus_viz_well`) repainted its whole picture a
       tick, a bounds-checked call a pixel, and `paint_box` visited every
       interior pixel even with no fill. Rows now (`paint_rect` one row
       then `memcpy`, `paint_box` only its ring and the corner cut's
       reach), pinned against the per-pixel rule in look_test: a
       1280 x 400 well 1.01 -> 0.064 ms. **ctui-mus `-w` playing,
       visualizers on: ~26 % -> ~10-12 % of a core** (the user). Next: the spectrogram's own
       render (~11 % of samples), which is what step 5's `grid` takes.
5. Plots: `bars` and `grid` first (ctui-mus's spectrum and spectrogram
   as the first users, beside their kitty renderers), then `columns`
   and `polyline` (ctui-daw), each measured against step 4's number
   for the same widget.
   **The grid, leaner than planned (2026-11-09, the user's call)**: the
   spectrogram's picture is its 96 x 192 colour grid scaled nearest to
   the panel, which the image path already does on the card. So no plot
   vocabulary in the core: `ctui_gfx_display_px()` (and the sink's
   `display_px`) shows an image in a pixel rect of its cells (in
   `ctui_cell_px()`'s pixels, kitty's X/Y offsets' cousin), -1 without a
   sink. ctui-mus's spectrogram, under a sink, sends the grid as a
   COLS x BINS image into the well's inset and the well as an image of
   its own, painted again only when it moves or the look changes. A
   panel shorter than 192 px now averages bins (the card's area
   average) where the CPU picks one. vk_test pins it against the
   reference; the same rect is what a sub-column glide would move.
   `bars`, `columns`, `polyline`: decided when measuring asks.

## Toward the display's rate, and what the card buys beyond it

Noted 2026-11-09, for step 5 (the user: at a native 240 Hz not
everything moves smoothly, the playbar most of all).

- **Why it isn't smooth yet: the app's clock, not the backend.**
  ctui-mus redraws on its `TICK_MS` (20 ms: 50 Hz); the host presents
  only frames the app drew, so a 240 Hz display shows each one ~5
  times. The playbar also moves by whole steps of its control (value of
  max, painted at a tick), and a text-mode bar by whole cells. Nothing
  in `src/vk/` caps the rate: a frame that changes is painted and
  presented within the loop's next wait.
- **Raising the tick on the CPU path costs in proportion**: every
  visualizer repaints its whole RGBA each tick. At 240 Hz that's ~5x
  today's work, which is what step 5 removes.
- **Plots on the card (step 5) are what make the display's rate cheap**:
  a widget hands data, not pixels -- a few KB a tick -- and the shader
  draws at every pixel. Beyond the CPU win:
  - *resolution*: bars as many as the panel has pixel columns, the
    FFT's bins interpolated (log frequency, cubic) instead of a few
    dozen bars; the spectrogram a ring in VRAM, a column up a tick,
    scrolled by a sub-column offset per frame so it glides; the
    oscilloscope's polyline antialiased.
  - *motion between ticks*: the record carries the state and its rate
    (a playbar's position, the time it was taken, playing or not), and
    the shader evaluates it per presented frame -- the playbar moves
    at 240 Hz while the app still ticks at 50. Bars' falloff and peak
    hold likewise, as functions of time rather than per-tick steps.
    This wants the present paced by the display (wp_presentation /
    frame callbacks, syNESthesia's present.c has them) rather than
    by the app's frames: a "something animates" flag in the record
    keeps presenting while it's set.
  - *looks*: gradients, glow, the look's ramps per pixel, at no cost
    worth measuring.
- **What isn't worth moving**: the FFT itself (8192 points every 20 ms
  is cheap on a CPU core; c-µdsp stays the DSP).
- **Fonts** (the user's other remark: 1:1 bar the fonts): Spleen is a
  bitmap at a few sizes. A real font means a glyph cache filled by a
  rasteriser (FreeType, or our own for TTF outlines) into the atlas,
  coverage instead of bits, blended in the shader -- the "atlas
  becomes a glyph cache" item above, with grey levels.

## Fonts, how it came out (2026-11-09)

- **JetBrains Mono** (the user's kitty font; 2.304 Regular, OFL, the
  file µOS vendors) in `src/vk/fonts/`, a C array made by xxd at build
  time (ctui-mus: under its build dir); `CTUI_FONT` names another
  TrueType file. Spleen and tools/bdf2h.py are gone.
- `src/vk/ttf.{c,h}`: µOS's parser and rasterizer (user/uui/font/),
  without its caches and AVX: format 4 cmap, glyf, translation-only
  composites, no hinting; exact coverage across a row, 16 samples down
  it (µOS: 4), drawn straight into a cell.
- **The cell is kitty's**: 11 pt at 96 dpi a logical pixel, times the
  window's scale; the advance x ascender to descender (+ line gap),
  rounded up; 9 x 20 at scale 1 (Spleen's 8 x 16 before).
- **A glyph cache, not a fixed atlas**: a slot drawn the first time a
  cell shows its codepoint, only new slots go up, the atlas on the card
  doubling when full. Coverage bytes, the shader blending fg over bg
  by them; bold a 1 px smear where no bold face was given, else that
  face (`ctui_vk_set_face()`, 2026-11-12: the difference showed in
  ctui-wm's terminal; italic and bold italic the same way), the face
  part of a slot's key.
- **Box drawing and blocks** (U+2500-259F): the font's box mapped onto
  the cell exactly, so lines meet the next cell's (kitty draws these
  itself; this is the font's own shapes, stretched).
- **What it lacks, counted**: of ctui's and ctui-mus's literals, 5 of
  45 non-ASCII (⏮⏭⏸⏹ in the toolbar's text tier, drawn chips under the
  look; ♪ in Spleen's table only); of the user's library (65,600
  title/artist/album strings), 7 with any: one Korean title, fullwidth
  quotes, an LRM, an emoji. No fallback font for that: a box, logged
  once. Recount with the same script if a library changes much.
- **What it lacks, from a provider (2026-11-10, ctui-wm's kittyless z4)**:
  `ctui_vk_set_glyph_fn()` -- a codepoint the font lacks is asked of
  the host's callback (its coverage at the cell's size, one or two
  cells; ctui-wm's panel host over fontconfig + freetype), else a box.
  A width-2 glyph is two slots, the lead's cell flagged `CTUI_VK_WIDE`
  and its CONT taking the next slot in the frame (`ctui_vk_cont()`, a
  CONT also redone when its lead changes); the cache is a hash map
  keyed by codepoint and width (past the BMP too; the 128 KB BMP table
  gone); a cluster is its first codepoint at the cluster's width.
  Colour emoji are not drawn (grey coverage only).
- **A terminal's palette and padding (2026-11-10, ctui-wm's panel
  host)**: `ctui_vk_set_palette()` for the 16 colours (a host gives its
  terminal's theme; xterm's until then), `ctui_vk_set_pad()` for an
  inset round the cells in logical px (kitty's window padding), the
  cells' origin a push constant and `ctui_wl_cell_origin()` for the
  pointer.

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

History: what came over before step 2. Spleen and tools/bdf2h.py have
since gone for JetBrains Mono ("Fonts, how it came out"), and cells.comp
was rewritten for ctui's grid.

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

Decided with the user: Spleen (since replaced by JetBrains Mono, the
user's kitty font), ctui's win95 look, keys and the mouse;
kitty stays the default everywhere; the GPU path is optional.
Then (2026-11-09): the host starts in ctui-mus (its first user;
syNESthesia keeps its own present path) and moves to `src/vk/` when
ctui-daw wants it too, by the examples_apps rule; the host reads keys
through xkbcommon (layouts, compose and dead keys for ctui's forms and
text inputs), unlike wl.c's evdev codes.
