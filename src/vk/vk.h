#ifndef CTUI_VK_H
#define CTUI_VK_H

/* The Vulkan backend (VK_DESIGN.md), text for now: a screen sink whose
 * frames are painted on the card into one surface buffer, the window's
 * pixels. It runs on the caller's device and records into the caller's
 * command buffer; it owns no window, queue or swapchain, so it drops
 * into a host's present path (a ctui app's window, syNESthesia's bars).
 *
 * The cells live on the card between frames: a frame's changed cells go
 * up as copy regions of their runs, and one dispatch (shaders/cells.comp)
 * repaints only the 16 x 16 tiles they touch. Each swapchain image the
 * caller presents from catches up by copy regions of the tiles it's
 * behind on, so an idle app costs nothing.
 *
 * Text in JetBrains Mono (fonts/, OFL; CTUI_FONT names another TrueType
 * file), a glyph drawn into the cache the first time a cell shows it
 * (ttf.h: grey levels), box drawing stretched to its cell. A codepoint
 * the font lacks is asked of ctui_vk_set_glyph_fn()'s provider (a host
 * with fontconfig: ctui itself links no font library), else drawn as a
 * box. A width-2 glyph spans its two cells; a cluster is drawn as its
 * first codepoint at the cluster's width (one font, no shaping).
 *
 * Per frame: ctui_screen_flush() (the sink takes the changes), then
 * ctui_vk_record() in a recording, then per image ctui_vk_regions() for
 * a vkCmdCopyBufferToImage from ctui_vk_surface(). At most
 * CTUI_VK_SLOTS recordings with something in them may be in flight:
 * the one before last must have finished before the next record. */

#include "ctui.h"

#include <stdint.h>
#include <vulkan/vulkan.h>

#define CTUI_VK_TILE 16u
#define CTUI_VK_SLOTS 2u
/* swapchain images whose catching up is tracked */
#define CTUI_VK_IMAGES 8u

/* a cell on the card: CTUI_VK_CELL_WORDS words, the glyph's slot in the
 * atlas | attrs << 16 (CTUI_ATTR_*, and the two flags below), then the
 * foreground and background as 0xRRGGBB. A CTUI_COLOR_DEFAULT colour is
 * a flag, not a colour: the painter takes ctui_vk_set_default()'s, so
 * changing those repaints without the cells going up again */
#define CTUI_VK_CELL_WORDS 3u
#define CTUI_VK_FG_DEFAULT (1u << 8)
#define CTUI_VK_BG_DEFAULT (1u << 9)
/* a kitty placeholder cell (CTUI_GFX_KITTY_PLACEHOLDER): its slot is the
 * image's column (16 bits; its row in the attrs' byte, which a
 * placeholder doesn't draw), its fg the image's id, painted from the
 * image place_file() gave that id, fit into its virtual cells */
#define CTUI_VK_PLACEHOLDER (1u << 10)
/* the lead of a width-2 glyph: its right half is the next slot, which
 * the CTUI_CELL_CONT right of it takes (ctui_vk_cont()) */
#define CTUI_VK_WIDE (1u << 11)
/* the slot drawn as a box: a codepoint the atlas lacks */
#define CTUI_VK_GLYPH_BOX 0xffffu

typedef struct CTUI_VK CTUI_VK;

/* the backend on the caller's device (any with a compute queue); NULL
 * when its pipeline can't be made (logged) */
CTUI_VK *ctui_vk_new(VkPhysicalDevice phys, VkDevice dev);
/* waits for the device to be idle first */
void ctui_vk_free(CTUI_VK *vk);

/* the surface w x h pixels, glyphs for scale120 (wp_fractional_scale's
 * 120ths: kitty's default size, 11 pt at 96 dpi a logical pixel; the
 * cell the font's advance x ascender to descender, as kitty's),
 * words in the swapchain's order (bgra != 0: BGRA). *rows / *cols: the
 * grid, for ctui_screen_resize() / ctui_app_resize(); the cell's pixel
 * size goes to ctui_cell_px_set(). Waits for the device to be idle; the
 * next record repaints everything. 0, or -1 (logged) */
int ctui_vk_resize(CTUI_VK *vk, unsigned w, unsigned h, int scale120,
                   int bgra, int *rows, int *cols);

/* CTUI_COLOR_DEFAULT's colours, which a terminal would have supplied
 * (0xRRGGBB; light grey on black until set); the next record repaints
 * every tile, no cell uploaded. ctui_vk_defaults() reads them back */
void ctui_vk_set_default(CTUI_VK *vk, uint32_t fg, uint32_t bg);
void ctui_vk_defaults(const CTUI_VK *vk, uint32_t *fg, uint32_t *bg);

/* the default background's alpha (0 clear .. 255 opaque, the default),
 * as kitty's background_opacity: only pixels on that background take it
 * (a cell with CTUI_COLOR_DEFAULT's, the inset, past the last cell);
 * glyphs and images composite "over" it, so the surface comes out
 * premultiplied -- a glyph on a clear background is its coverage, an
 * opaque image stays opaque. A presenter (present.h) asks the compositor
 * to blend it while it's below 255. The next record repaints every tile.
 * ctui_vk_opacity() reads it back */
void ctui_vk_set_opacity(CTUI_VK *vk, unsigned alpha);
unsigned ctui_vk_opacity(const CTUI_VK *vk);

/* the 16 colours a CTUI_COLOR_MODE_256 index below 16 and a basic
 * CTUI_COLOR_* one (BLACK..WHITE: 0..7) take, as a terminal's palette
 * does (0xRRGGBB; NULL: xterm's, the default). Cells drawn from then on */
void ctui_vk_set_palette(CTUI_VK *vk, const uint32_t pal[16]);

/* an inset around the cells as a terminal's padding: top, right, bottom,
 * left in logical pixels (NULL: none, the default), at the next
 * ctui_vk_resize() (scaled by its scale120). The cells start at its top
 * left, what they leave over goes right and below, all of it in the
 * default background; one too big for a cell is ignored. The cells'
 * origin it made, in buffer pixels: ctui_vk_origin() */
void ctui_vk_set_pad(CTUI_VK *vk, const unsigned pad[4]);
void ctui_vk_origin(const CTUI_VK *vk, unsigned *x, unsigned *y);

/* a glyph the font lacks: cp's coverage into cov (cells * gw x gh, a
 * byte a pixel, rows of cells * gw, cleared), at em px an em with the
 * baseline at row base -- the built-in font's cell, which a fallback
 * font fits itself into. cells: 2 for a width-2 glyph. 0 drawn, -1 not
 * had (the box). Called while a frame goes in, on its thread */
typedef int (*CTUI_VK_GLYPH_FN)(void *ctx, uint32_t cp, int cells, int gw,
                                int gh, int base, float em, uint8_t *cov);
/* the provider of what the font lacks (NULL: none); codepoints drawn as
 * boxes so far are asked again, from their cells' next change. Set it
 * before the first frame; it stays across resizes */
void ctui_vk_set_glyph_fn(CTUI_VK *vk, CTUI_VK_GLYPH_FN fn, void *ctx);

/* a styled face: the TrueType font cells with attrs (CTUI_ATTR_BOLD,
 * CTUI_ATTR_ITALIC or both) are drawn in, at the regular font's cell
 * (its glyphs centred in it, as a fallback's are). data is the caller's,
 * outliving vk (one copy can serve every CTUI_VK); NULL drops the face.
 * Without one, bold is the regular glyph smeared a pixel right and
 * italic is upright; bold italic falls back to bold, then italic. A
 * glyph the face lacks comes from the regular font, then the provider
 * (smeared for bold). Glyphs already drawn in that style are drawn again
 * from their cells' next change. 0 (set, or dropped), or -1 (attrs 0,
 * or not a TrueType font: logged, the face dropped) */
int ctui_vk_set_face(CTUI_VK *vk, unsigned attrs, const uint8_t *data, size_t size);

/* for ctui_screen_set_sink(screen, ctui_vk_screen_sink(), vk) */
const CTUI_SCREEN_SINK *ctui_vk_screen_sink(void);

/* for ctui_gfx_set_sink(ctui_vk_gfx_sink(), vk) (core/gfx.h): kitty's
 * pixel images as images on the card. display() and image_begin()/
 * commit() keep a copy of the texels on the host and place them at
 * cells; the painter composites them per pixel as kitty does -- z < 0
 * under the glyphs (below CTUI_GFX_KITTY_Z_UNDER_BG only where the cell's
 * background is the default), z >= 0 over them, by z then id, alpha
 * blended -- each scaled to its cells by area average (nearest when
 * enlarged). delete() takes one away with its placements; put()/unput()
 * place an image held already (placement 0 is display()'s). place_file()
 * decodes the PNG (look/png.h) for the placeholder cells naming its id,
 * place_rgba() takes the pixels (look's drawn controls: no file at all):
 * fit into its cols x rows as kitty does (the aspect kept, centred) */
const CTUI_GFX_SINK *ctui_vk_gfx_sink(void);

/* something to paint: the next record wouldn't be empty. A host asks
 * before acquiring a swapchain image, so an idle app presents nothing */
int ctui_vk_pending(const CTUI_VK *vk);

/* the frame's time (CLOCK_MONOTONIC ns, the one ctui_gfx_wipe() counts
 * from): each moving wipe's split where it is then, its cells marked
 * when it moved a column. 1 while something still moves, with the ns
 * until the next column in *next_ns (a host wakes then to present it:
 * the motion at the display's rate, nothing presented in between) */
int ctui_vk_at(CTUI_VK *vk, uint64_t now_ns, uint64_t *next_ns);

/* into cmd (recording): what changed since the last record uploaded and
 * painted, between barriers, so the caller may copy from the surface
 * after it. The tiles painted (0: nothing recorded at all; always, direct) */
unsigned ctui_vk_record(CTUI_VK *vk, VkCommandBuffer cmd);

/* the surface: w * h words, row-major, a storage and transfer source
 * buffer on the card */
VkBuffer ctui_vk_surface(const CTUI_VK *vk);

/* painting straight into the caller's images instead (on != 0): no
 * surface buffer (w * h * 4 bytes of VRAM saved, 11 MB for a 2112 x 1310
 * panel), ctui_vk_record_image() in place of ctui_vk_record() +
 * ctui_vk_regions(). The device needs shaderStorageImageWriteWithoutFormat
 * enabled and the images VK_IMAGE_USAGE_STORAGE_BIT, their views given by
 * ctui_vk_set_images(). Any time (the device waited for), every tile
 * painted again after. 0, or -1: not built with shaders/cells_direct.spv.h
 * (glslc -DDIRECT) or its pipeline not made -- the buffer stays */
int ctui_vk_set_direct(CTUI_VK *vk, int on);
int ctui_vk_direct(const CTUI_VK *vk);
/* direct: the swapchain's n views (n <= CTUI_VK_IMAGES), image i the one
 * ctui_vk_record_image(..., i, ...) paints. With the device idle (none of
 * the old ones in use), after the swapchain is made */
void ctui_vk_set_images(CTUI_VK *vk, unsigned n, const VkImageView *views);
/* direct, into cmd (recording): what changed uploaded, then painted into
 * image img -- the tiles changed since the last record and the ones it's
 * behind on (every one when fresh: it holds nothing of the surface) --
 * between barriers. The image in VK_IMAGE_LAYOUT_GENERAL from before the
 * call (its last access done), its writes the compute stage's for the
 * caller's barrier after. The tiles painted */
unsigned ctui_vk_record_image(CTUI_VK *vk, VkCommandBuffer cmd, unsigned img,
                              int fresh);

/* the copy regions swapchain image img (< CTUI_VK_IMAGES) is behind on,
 * into out (room for at least one region a tile row: h / 16 + 1); fresh
 * when the image holds nothing of the surface (just made, or cleared).
 * Their number; the image then holds everything recorded so far */
unsigned ctui_vk_regions(CTUI_VK *vk, unsigned img, int fresh,
                         VkBufferImageCopy *out);

/* a cell as it goes to the card (out: CTUI_VK_CELL_WORDS): the glyph's
 * slot, its attrs, its colours resolved to RGB (or flagged default). A
 * codepoint without a glyph is logged, once. A CTUI_CELL_CONT comes out
 * blank: the frame resolves it from the cell left of it, ctui_vk_cont() */
void ctui_vk_cell(CTUI_VK *vk, const CTUI_CELL *c, uint32_t *out);
/* a CTUI_CELL_CONT's words (w, from ctui_vk_cell()) given the cell left
 * of it as it goes to the card (left: NULL at the row's start): the
 * right half of a CTUI_VK_WIDE lead's glyph, else blank */
void ctui_vk_cont(uint32_t *w, const uint32_t *left);

/* the glyph cache: w x h px a glyph, slot k's coverage from slots +
 * k * slot_words (a byte a pixel, rows of w, four to a word from the
 * low byte); slot 0 blank. A slot is the one ctui_vk_cell() gave */
void ctui_vk_glyphs(const CTUI_VK *vk, unsigned *w, unsigned *h,
                    const uint32_t **slots, unsigned *slot_words);

#endif
