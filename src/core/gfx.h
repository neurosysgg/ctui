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
 * Caller (ctui_app_render()'s Phase 4 dispatch, see widget.h's
 * ctui_widget_set_gfx_renderer()) is responsible for only calling this
 * once CTUI_GFX_KITTY is actually the negotiated g_gfx_mode -- this
 * function itself doesn't check. No-ops (logs E_WRN) if stdout isn't a
 * real terminal or the image is degenerate (non-positive dimensions or a
 * NULL buffer). */
void ctui_gfx_kitty_display(int row, int col, int cell_cols, int cell_rows,
                            const unsigned char *rgba, int width, int height,
                            unsigned int image_id);

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
 * ctui_gfx_kitty_display(). */
void ctui_gfx_kitty_delete(unsigned int image_id);

#endif
