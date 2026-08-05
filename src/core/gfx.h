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

#endif
