# ctui music player + viz — design notes

Planning doc for a lean terminal WAV player with basic audio-visualization,
built on ctui. Captures decisions made in design discussion, before any code
exists, so an implementation session can start straight from here. Read
`/CLAUDE.md` first for general project conventions — everything below builds
on top of those, doesn't replace them.

## Goal

A `examples_apps/player` app that: browses for a `.wav` file (reusing
`file_browser`'s list-navigation pattern), plays it out through ALSA, and
shows a basic live viz (level meter to start) while it plays. Lean on
purpose: WAV only for v1, no ffmpeg or other heavy decode deps. FLAC is the
planned second format, once the decoder interface below proves itself.

## Architecture: three independent stages, tee'd

```
decoder.decode(buf, N)  →  buf ──┬──→ output.write(buf, N)     (or null_output: drop)
                                  └──→ process (RMS/spectrum)  →  viz widget
```

- **Input (decoder)**: file → normalized frames + metadata. One
  implementation per format.
- **Output**: normalized frames → device, or nowhere (`null_output`). Fully
  independent of decoder internals — this is what makes new front-ends
  (headless, alternate backends) cheap later.
- **Process**: **read-only tap**, decided explicitly over an in-place
  transform stage. Process only computes numbers (RMS/spectrum) for the viz
  widget; it never modifies the buffer, and output always gets the
  decoder's untouched frames. Output and process are independent consumers
  of the same buffer — neither blocks or waits on the other. (An in-place
  transform stage, e.g. for a future EQ, was considered and explicitly
  deferred — not needed for v1, adds ordering coupling between process and
  output.)

Internal representation between all stages: **interleaved float32**. DSP
math (RMS/FFT) wants floats natively; every decoder normalizes its own bit
depth/encoding to this once, at the decode boundary, so downstream code
(process, output) never branches on source format. The one place raw
bit-depth reality reappears is inside the ALSA output backend, which
converts float → whatever the hardware wants at write time — a single,
contained conversion.

## Interfaces (sketched, not final — nail down exact fields when implementing)

```c
typedef struct CTUI_AUDIO_FORMAT {
  int sample_rate;
  int channels;      /* internal rep is always interleaved */
} CTUI_AUDIO_FORMAT;

typedef struct CTUI_TRACK_INFO {
  char title[128];
  int bitrate;
  double duration_sec;
  char sample_format[32]; /* e.g. "16-bit PCM" */
} CTUI_TRACK_INFO;

typedef struct CTUI_DECODER {
  void *state;
  int (*open)(void *state, const char *path, CTUI_AUDIO_FORMAT *fmt, CTUI_TRACK_INFO *info);
  size_t (*decode)(void *state, float *out, size_t out_frames); /* returns frames decoded, 0 = EOF */
  void (*close)(void *state);
} CTUI_DECODER;

typedef struct CTUI_AUDIO_OUTPUT {
  void *state;
  int (*open)(void *state, CTUI_AUDIO_FORMAT *fmt);
  size_t (*write)(void *state, const float *frames, size_t n); /* non-blocking; returns frames accepted */
  void (*close)(void *state);
} CTUI_AUDIO_OUTPUT;
```

Both shapes deliberately mirror `CTUI_WIDGET`'s existing `render`/`layout`
function-pointer dispatch — not new machinery, staying consistent with how
pluggability already works in this codebase.

## Audio feeding: single-threaded, timer-driven (not a playback thread)

Decided over introducing a playback thread + ring buffer. Feed audio via a
`CTUI_TIMER` (see `src/core/timer.h`, `src/widgets/periodic.c`) firing every
~20ms: each tick, decode the next chunk, write whatever fits into ALSA via
`SND_PCM_NONBLOCK`, and run process/viz off the same chunk. Fully
single-threaded, fits ctui's existing event-loop style with zero new
concurrency primitives (no mutexes, no thread lifecycle to manage). Revisit
only if underruns actually show up in testing — don't preemptively build
the threaded version.

## Reuse before writing new widgets

General rule going forward, not just for this app: **before writing a new
widget anywhere in this project, check whether an existing one (in
`src/widgets/` or another app's local `widgets/`) already covers it.**

Concretely for this app:
- File selection reuses `examples_apps/file_browser`'s local
  `widgets/list.c` navigation pattern (filtered to `*.wav` instead of `chdir`
  on Enter). This makes `player` the *second* app needing that widget —
  per `CLAUDE.md`'s promotion rule, `list.c` becomes a legitimate candidate
  to promote to `src/widgets/` once this is built (not before; still
  nothing promoted speculatively ahead of an actual second consumer).
- The viz widget (level meter, bars) is new — nothing existing covers it.
  Build it local to `examples_apps/player/widgets/` first, per the normal
  proving-ground rule; only promote later if a second app wants it too.

## File layout

```
examples_apps/player/
  main.c
  audio/format.h        (CTUI_AUDIO_FORMAT, CTUI_TRACK_INFO)
  audio/decoder.h        (CTUI_DECODER interface)
  audio/output.h          (CTUI_AUDIO_OUTPUT interface)
  decoders/wav.c/.h
  outputs/alsa.c/.h, outputs/null.c/.h
  widgets/meter.c/.h      (the viz widget)
```

Non-widget audio code (`audio/`, `decoders/`, `outputs/`) sits beside
`main.c`, not under `widgets/` — mirrors the existing
`examples_apps/calculator/calc.c`/`calc.h` precedent for app-local
non-widget logic.

Makefile: needs a new `ctui-player` target following the existing
per-app pattern, plus `-lasound` linked in for that target only (ALSA is
already present on this system via `libasound.so.2`; this is the one new
external link dependency the project takes on for this app — every other
existing target only depends on libc).

## Agreed build order

Incremental, so each step is independently testable before the next:

1. WAV parser, standalone — parse RIFF/`fmt `/`data`, canonical PCM only,
   malformed input rejected via log-and-no-op (not fatal), per project
   convention. No ctui or ALSA involved yet.
2. Headless ALSA playback — confirms audio actually comes out, still no
   ctui involved.
3. Wire into a `file_browser`-derived app for file selection.
4. Add the viz (level meter) widget last.

## Resolved

- `CTUI_TRACK_INFO` gets `duration_sec` (double) and `sample_format`
  (`char[32]`, e.g. `"16-bit PCM"`) alongside title/bitrate — see the
  struct above. Both are derivable straight from the WAV `fmt `/`data`
  chunks, nothing exotic needed for v1.
- Viz widget: single VU-style bar (peak/RMS), not multi-band. Multi-band
  implies an FFT the app doesn't otherwise need; a single meter matches
  "lean" and is all v1's build order calls for. Revisit only if a later
  app actually wants spectrum bars.
- Timer interval: 20ms, as a plain constant in `main.c`, not a config
  knob (nothing's asked for one yet). Retune the constant directly if
  real underrun/latency behavior in testing calls for it.
- FLAC decoder: confirmed out of scope for v1. The `CTUI_DECODER`
  interface is shaped so adding it later is a new `decoders/flac.c`
  implementation, no changes to `main.c` or the output/process stages.
