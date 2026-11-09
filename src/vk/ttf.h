#ifndef CTUI_VK_TTF_H
#define CTUI_VK_TTF_H

#include <stddef.h>
#include <stdint.h>

/* TrueType glyphs into a cell's coverage, for src/vk/'s glyph cache:
 * µOS's parser and rasterizer (user/uui/font/, checked there against an
 * independent render) without its caches and AVX. The file's bytes are a
 * trust boundary: every offset is bounds-checked. glyf outlines only (no
 * CFF), a format 4 cmap (the BMP), composites by translation only, no
 * hinting: exact coverage across a row, 16 samples down it. */

typedef struct {
  const uint8_t *data; /* the caller's, outliving this */
  size_t size;
  uint16_t upem, glyphs, hmetrics;
  int16_t loca_long;
  int16_t ascender, descender, line_gap; /* hhea, font units */
  uint32_t hmtx, loca, loca_len, glyf, glyf_len, cmap4, cmap4_len;
} CTUI_TTF;

/* 0, or -1 for a file it can't read (logged by the caller) */
int ctui_ttf_load(CTUI_TTF *f, const uint8_t *data, size_t size);

/* codepoint's glyph, 0 (.notdef) where the font has none */
uint16_t ctui_ttf_glyph(const CTUI_TTF *f, uint32_t cp);

/* the cell at em px an em: its width (the advance, rounded up), height
 * (ascender to descender plus the line gap, rounded up) and the
 * baseline's row (the ascender, rounded up) -- kitty's, from the same
 * metrics */
void ctui_ttf_cell(const CTUI_TTF *f, float em, int *cw, int *ch, int *base);

/* glyph into cov (cw x ch bytes, 0..255, overwritten): its advance
 * centred in the cell, the baseline at row base. stretch: the font's box
 * (0..advance across, ascender..descender down) mapped onto the cell
 * exactly instead -- box drawing, whose lines have to meet the next
 * cell's. 0, or -1 for an outline it can't read (cov left clear) */
int ctui_ttf_draw(const CTUI_TTF *f, uint16_t glyph, float em, int cw, int ch,
                  int base, int stretch, uint8_t *cov);

#endif
