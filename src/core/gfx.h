#ifndef CTUI_GFX_H
#define CTUI_GFX_H

/* terminal-level graphics capability, negotiated once at ctui_init() time
 * (see term.h). Distinct from CTUI_COLOR_MODE_* (cell.h), which is a
 * per-cell encoding choice, not a terminal capability -- see
 * GFX_DESIGN.md's "Resolved open questions" for why these are two enums,
 * not one.
 *
 * Bit flags, not sequential indices: ctui_gfx_detect_caps() ORs together
 * every tier the terminal supports and callers test membership with a
 * plain &, so each tier needs its own bit. */
typedef enum {
  CTUI_GFX_ANSI16 = 1 << 0, /* mandatory floor -- always set, see below */
  CTUI_GFX_ANSI256 = 1 << 1,
  CTUI_GFX_TRUECOLOR = 1 << 2,
  CTUI_GFX_KITTY = 1 << 3, /* pixel graphics protocol */
} CTUI_GFX_MODE;

/* env-sniffing only for v1 (TERM, COLORTERM, KITTY_WINDOW_ID) -- no
 * escape-sequence capability querying yet (see GFX_DESIGN.md's Deferred
 * section). CTUI_GFX_ANSI16 is unconditionally set in the returned
 * bitmask: assumed universal in 2026, not derived from env vars that some
 * terminals may simply not bother setting. */
unsigned int ctui_gfx_detect_caps(void);

/* approximates one of the 9 CTUI_COLOR_* basic-palette indices (cell.h) as
 * a 24-bit RGB triple -- xterm's standard normal-intensity palette values,
 * close enough to "the same color" for a pixel-tier (Kitty) renderer that
 * has to draw the same zone/accent coloring its ANSI16 sibling uses, not
 * itself a claim about what any given user's terminal theme actually maps
 * those indices to. Promoted here once a second consumer (widgets/border.c's
 * Kitty gfx_render, alongside ctui-mus's own meter widget) needed the exact
 * same table -- see CLAUDE.md's promotion rule. */
void ctui_gfx_ansi16_rgb(unsigned char color, unsigned char *r,
                         unsigned char *g, unsigned char *b);

/* transmits and displays a width x height RGBA image via the Kitty
 * graphics protocol (https://sw.kovidgoyal.net/kitty/graphics-protocol/),
 * writing the APC escape sequence straight to stdout -- unlike every
 * ctui_widget_putc_*() variant, this bypasses CTUI_CELL entirely, since
 * pixel graphics can't be expressed as a colored character cell (see
 * GFX_DESIGN.md's "Non-degradable protocols"). row/col are 1-based
 * terminal cell coordinates (as consumed by the CUP cursor-position
 * escape, same convention ctui_screen_flush() uses); cell_cols/cell_rows
 * say how many character cells wide/tall the terminal should scale the
 * image to cover. rgba must be exactly width*height*4 bytes (8-bit RGBA,
 * row-major, no padding). image_id lets a caller re-transmit under the
 * same id on a later frame to replace the previous one in place, instead
 * of layering a new image on top each time -- pass any nonzero value
 * that's stable across a widget's redraws. The payload is always sent
 * with q=2 (quiet): ctui never reads the terminal's APC replies, success
 * or error, so there's nothing to parse them into.
 *
 * z is the Kitty protocol's own placement z-index (spec: "Z-stacking of
 * images"): z>=0 draws above all text (the protocol's default, and what
 * every ctui-mus Kitty widget before this parameter existed always got,
 * hence z=0 being the right value for any caller that isn't specifically
 * trying to out-stack another Kitty image); z<0 draws below text but above
 * cell background colors. Ties between two images at the same z-index
 * break by image_id (lower id = lower stacking), so a caller that needs to
 * guarantee it wins against every other Kitty-rendered widget in the app --
 * e.g. an overlay that has to occlude other widgets' images it has no
 * knowledge of, not just its own previous frame -- should pick a z clearly
 * above 0 (see widgets/modal.c's own doc comment for the concrete case
 * this was added for) rather than relying on id ordering.
 *
 * Caller (ctui_app_render()'s Phase 4 dispatch, see widget.h's
 * ctui_widget_set_gfx_renderer()) is responsible for only calling this
 * once CTUI_GFX_KITTY is actually the negotiated g_gfx_mode -- this
 * function itself doesn't check. No-ops (logs E_WRN) if stdout isn't a
 * real terminal or the image is degenerate (non-positive dimensions or a
 * NULL buffer).
 *
 * Phase 5: the wire bytes this builds (CUP escape, every chunk's key-list
 * prefix, base64 body, terminator) are appended to an internal batch
 * buffer rather than written to stdout directly -- see
 * ctui_gfx_kitty_flush(), the only thing that actually issues a write().
 * Before base64-encoding, the raw RGBA payload is run through
 * ctui_deflate_compress() (unless ctui_gfx_kitty_set_compression(0)
 * turned that off) and the compressed bytes are used instead -- with
 * `o=z` added to the escape's key list -- whenever that's actually
 * smaller than the raw buffer; otherwise this transmits raw exactly as it
 * always did. Phase 6: if ctui_gfx_kitty_probe_shm() determined this
 * terminal supports Kitty's t=s (shared-memory) transmission medium, the
 * payload goes through a POSIX shm object instead of base64+chunking
 * (still through the same batch buffer, just a much shorter control
 * string); otherwise this falls back to the original t=d path
 * unchanged. All of this is invisible to the caller: same signature, same
 * "one call transmits one frame" contract. */
void ctui_gfx_kitty_display(int row, int col, int cell_cols, int cell_rows,
                            const unsigned char *rgba, int width, int height,
                            unsigned int image_id, int z);

/* removes an image previously placed by ctui_gfx_kitty_display() under
 * image_id -- unlike a colored character cell, a Kitty-placed image is a
 * raster overlay independent of the text grid, so it stays on screen even
 * after whatever widget put it there stops being drawn (e.g. a toggled-off
 * panel that ctui_split_render() simply no longer traverses). A caller
 * whose image can become not-currently-visible must call this explicitly
 * when that happens; there's no implicit cleanup. Deletes both the
 * placement and the stored image data (`d=I`) -- safe here since
 * ctui_gfx_kitty_display() always retransmits the full pixel buffer on
 * every redisplay rather than relying on previously cached data. No-op
 * (logs E_WRN) if stdout isn't a real terminal, same convention as
 * ctui_gfx_kitty_display(). Like ctui_gfx_kitty_display(), its escape
 * bytes go through the Phase 5 batch buffer, not a direct write(). */
void ctui_gfx_kitty_delete(unsigned int image_id);

/* issues the one real write() for every Kitty escape batched this frame
 * by ctui_gfx_kitty_display()/ctui_gfx_kitty_delete() (kitty_batch_append(),
 * core/gfx.c), then resets the batch for the next frame. Collapses what
 * used to be N widgets x M chunks x 3 write()s into a single write() per
 * frame -- the syscall-count overhead GFX_DESIGN.md's Phase 5 profiling
 * numbers point at. Does not shrink the worst-case stall if the terminal
 * genuinely can't drain the bytes fast enough -- the same total byte
 * count still blocks on this one call, just consolidated instead of
 * scattered (see GFX_DESIGN.md's Phase 5a "what this fixes vs. doesn't").
 * No-op if nothing was batched this frame. Called by
 * ctui_widget_flush_gfx() (core/widget.c), right after its own loop over
 * gfx_pending[] -- the one place already guaranteed to run once per frame
 * after ctui_screen_flush(), for the same pixels-after-text-flush
 * ordering reason documented on ctui_widget_flush_gfx() itself. Not
 * expected to be called directly by app/widget code. */
void ctui_gfx_kitty_flush(void);

/* Phase 6: one-shot startup probe for Kitty's t=s (shared-memory)
 * transmission medium -- see GFX_DESIGN.md's Phase 6 and the doc comment
 * on this function's definition (core/gfx.c) for the full mechanism and
 * fallback behavior. Called by ctui_init() (core/term.c) once
 * CTUI_GFX_KITTY has been negotiated; every subsequent
 * ctui_gfx_kitty_display() call in the process transparently uses
 * whichever transport this determined is supported. Not expected to be
 * called directly by app/widget code, same convention as
 * ctui_gfx_kitty_flush(). */
void ctui_gfx_kitty_probe_shm(void);

/* Phase 6: unlinks any t=s shared-memory segment still outstanding --
 * i.e. one the terminal accepted the name of but never read (and
 * therefore never unlinked itself, as the spec makes it responsible for
 * doing). Called by ctui_shutdown() (core/term.c); a no-op on every
 * non-Kitty tier and whenever the terminal has been keeping up, which is
 * the overwhelmingly common case. See the doc comment on this function's
 * definition (core/gfx.c) for why the leak is worth bounding at all. Not
 * expected to be called directly by app/widget code, same convention as
 * ctui_gfx_kitty_flush(). */
void ctui_gfx_kitty_shm_reap(void);

/* opts the Kitty transport in (default) or out of Phase 5b's `o=z`
 * payload compression (ctui_deflate_compress(), core/deflate.h). This is
 * a runtime toggle, not a negotiated CTUI_GFX_MODE bit: the risk it
 * guards against is a bug in ctui's own new DEFLATE encoder, not a
 * terminal that can't handle o=z (that's spec-mandated for any real
 * Kitty-protocol terminal, so there's no capability to detect). Affects
 * every subsequent ctui_gfx_kitty_display() call process-wide; there's no
 * per-widget override. */
void ctui_gfx_kitty_set_compression(int enabled);

#endif
