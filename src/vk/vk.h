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
/* the slot drawn as a box: a codepoint the atlas lacks */
#define CTUI_VK_GLYPH_BOX 0xffffu

typedef struct CTUI_VK CTUI_VK;

/* the backend on the caller's device (any with a compute queue); NULL
 * when its pipeline can't be made (logged) */
CTUI_VK *ctui_vk_new(VkPhysicalDevice phys, VkDevice dev);
/* waits for the device to be idle first */
void ctui_vk_free(CTUI_VK *vk);

/* the surface w x h pixels, glyphs for scale120 (wp_fractional_scale's
 * 120ths: the largest Spleen size no taller than 16 px a logical pixel),
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

/* for ctui_screen_set_sink(screen, ctui_vk_screen_sink(), vk) */
const CTUI_SCREEN_SINK *ctui_vk_screen_sink(void);

/* into cmd (recording): what changed since the last record uploaded and
 * painted, between barriers, so the caller may copy from the surface
 * after it. The tiles painted (0: nothing recorded at all) */
unsigned ctui_vk_record(CTUI_VK *vk, VkCommandBuffer cmd);

/* the surface: w * h words, row-major, a storage and transfer source
 * buffer on the card */
VkBuffer ctui_vk_surface(const CTUI_VK *vk);

/* the copy regions swapchain image img (< CTUI_VK_IMAGES) is behind on,
 * into out (room for at least one region a tile row: h / 16 + 1); fresh
 * when the image holds nothing of the surface (just made, or cleared).
 * Their number; the image then holds everything recorded so far */
unsigned ctui_vk_regions(CTUI_VK *vk, unsigned img, int fresh,
                         VkBufferImageCopy *out);

/* a cell as it goes to the card (out: CTUI_VK_CELL_WORDS): the glyph's
 * slot, its attrs, its colours resolved to RGB (or flagged default). A
 * codepoint without a glyph is logged, once */
void ctui_vk_cell(CTUI_VK *vk, const CTUI_CELL *c, uint32_t *out);

/* the glyphs in use: w x h px, rows the atlas (256 glyphs of h words, a
 * row a word, bit 31 the leftmost pixel) */
void ctui_vk_glyphs(const CTUI_VK *vk, unsigned *w, unsigned *h,
                    const uint32_t **rows);

#endif
