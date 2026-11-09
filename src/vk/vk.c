#include "vk.h"

#include "fonts/spleen.h"

#include <stdlib.h>
#include <string.h>

static const uint32_t k_cells_spv[] =
#include "shaders/cells.spv.h"
    ;

typedef struct {
  VkBuffer buf;
  VkDeviceMemory mem;
  void *map; /* host-visible ones */
} BUF;

typedef struct {
  uint32_t surf_w, surf_h, gw, gh, cols, rows, bgra, def_fg, def_bg;
} PUSH;

/* codepoints a log was given for, so each missing one is logged once */
#define MISSING_LOGGED 64u

struct CTUI_VK {
  VkPhysicalDevice phys;
  VkDevice dev;
  VkPhysicalDeviceMemoryProperties mem;
  VkShaderModule module;
  VkDescriptorSetLayout set_layout;
  VkPipelineLayout layout;
  VkPipeline pipe;
  VkDescriptorPool pool;
  VkDescriptorSet set;
  /* on the card: the surface, the cells, the tile list, the atlas */
  BUF surf, cells, tiles, atlas;
  /* mapped, one a recording in flight: a record's tile list, the atlas
   * when it goes up, the changed cells' runs */
  BUF stage[CTUI_VK_SLOTS];
  unsigned slot;
  VkBufferCopy *copies;
  unsigned w, h, gw, gh, cols, rows, tx, ty;
  int bgra, sized;
  const uint32_t *font;
  uint32_t def_fg, def_bg;
  uint32_t *rec;         /* the cells as on the card, cols * rows * WORDS */
  unsigned char *dirty;  /* a cell changed since the last record */
  unsigned char *tdirty; /* a tile to paint */
  int atlas_up;          /* the atlas to go up with the next record */
  /* per swapchain image and tile row, the span of tiles it's behind on:
   * [0] first, [1] last; first > last when none */
  int *behind;
  uint32_t extras[64];
  unsigned nextras;
  uint32_t missing[MISSING_LOGGED];
  unsigned nmissing;
};

static int mem_type(const CTUI_VK *vk, uint32_t bits,
                    VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < vk->mem.memoryTypeCount; i++) {
    if ((bits & (1u << i)) &&
        (vk->mem.memoryTypes[i].propertyFlags & want) == want) {
      return (int)i;
    }
  }
  return -1;
}

static void buf_free(CTUI_VK *vk, BUF *b) {
  if (b->buf) {
    vkDestroyBuffer(vk->dev, b->buf, NULL);
  }
  if (b->mem) {
    vkFreeMemory(vk->dev, b->mem, NULL);
  }
  memset(b, 0, sizeof *b);
}

static int buf_new(CTUI_VK *vk, BUF *b, size_t bytes, int host) {
  memset(b, 0, sizeof *b);
  VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = bytes ? bytes : 4,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  if (vkCreateBuffer(vk->dev, &bci, NULL, &b->buf) != VK_SUCCESS) {
    return -1;
  }
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(vk->dev, b->buf, &req);
  int t = mem_type(vk, req.memoryTypeBits,
                   host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = (uint32_t)t,
  };
  if (t < 0 || vkAllocateMemory(vk->dev, &mai, NULL, &b->mem) != VK_SUCCESS ||
      vkBindBufferMemory(vk->dev, b->buf, b->mem, 0) != VK_SUCCESS ||
      (host && vkMapMemory(vk->dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) !=
                   VK_SUCCESS)) {
    buf_free(vk, b);
    return -1;
  }
  return 0;
}

/* UI_FONT_EXTRAS' codepoints, in their slots' order */
static void decode_extras(CTUI_VK *vk) {
  const unsigned char *e = (const unsigned char *)UI_FONT_EXTRAS;
  while (*e && vk->nextras < sizeof vk->extras / sizeof vk->extras[0]) {
    int len = *e < 0xe0 ? 2 : *e < 0xf0 ? 3 : 4;
    uint32_t c = *e & (len == 2 ? 0x1fu : len == 3 ? 0x0fu : 0x07u);
    for (int k = 1; k < len; k++) {
      c = c << 6 | (e[k] & 0x3fu);
    }
    vk->extras[vk->nextras++] = c;
    e += len;
  }
}

static void free_sized(CTUI_VK *vk) {
  buf_free(vk, &vk->surf);
  buf_free(vk, &vk->cells);
  buf_free(vk, &vk->tiles);
  buf_free(vk, &vk->atlas);
  for (unsigned i = 0; i < CTUI_VK_SLOTS; i++) {
    buf_free(vk, &vk->stage[i]);
  }
  free(vk->copies);
  free(vk->rec);
  free(vk->dirty);
  free(vk->tdirty);
  free(vk->behind);
  vk->copies = NULL;
  vk->rec = NULL;
  vk->dirty = vk->tdirty = NULL;
  vk->behind = NULL;
  vk->sized = 0;
}

void ctui_vk_free(CTUI_VK *vk) {
  if (!vk) {
    return;
  }
  vkDeviceWaitIdle(vk->dev);
  free_sized(vk);
  if (vk->pool) {
    vkDestroyDescriptorPool(vk->dev, vk->pool, NULL);
  }
  if (vk->pipe) {
    vkDestroyPipeline(vk->dev, vk->pipe, NULL);
  }
  if (vk->layout) {
    vkDestroyPipelineLayout(vk->dev, vk->layout, NULL);
  }
  if (vk->set_layout) {
    vkDestroyDescriptorSetLayout(vk->dev, vk->set_layout, NULL);
  }
  if (vk->module) {
    vkDestroyShaderModule(vk->dev, vk->module, NULL);
  }
  free(vk);
}

CTUI_VK *ctui_vk_new(VkPhysicalDevice phys, VkDevice dev) {
  CTUI_VK *vk = calloc(1, sizeof *vk);
  if (!vk) {
    return NULL;
  }
  vk->phys = phys;
  vk->dev = dev;
  vk->def_fg = 0xc0c0c0u;
  vk->def_bg = 0x000000u;
  vkGetPhysicalDeviceMemoryProperties(phys, &vk->mem);
  decode_extras(vk);

  VkDescriptorSetLayoutBinding bind[4];
  for (unsigned i = 0; i < 4; i++) {
    bind[i] = (VkDescriptorSetLayoutBinding){
        .binding = i,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
  }
  VkShaderModuleCreateInfo smi = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof k_cells_spv,
      .pCode = k_cells_spv,
  };
  VkDescriptorSetLayoutCreateInfo dli = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 4,
      .pBindings = bind,
  };
  VkPushConstantRange range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .size = sizeof(PUSH),
  };
  VkPipelineLayoutCreateInfo pli = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &range,
  };
  VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
  VkDescriptorPoolCreateInfo dpi = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &size,
  };
  if (vkCreateShaderModule(dev, &smi, NULL, &vk->module) != VK_SUCCESS ||
      vkCreateDescriptorSetLayout(dev, &dli, NULL, &vk->set_layout) !=
          VK_SUCCESS ||
      (pli.pSetLayouts = &vk->set_layout,
       vkCreatePipelineLayout(dev, &pli, NULL, &vk->layout) != VK_SUCCESS)) {
    goto bad;
  }
  VkComputePipelineCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = vk->module,
                .pName = "main"},
      .layout = vk->layout,
  };
  VkDescriptorSetAllocateInfo dai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorSetCount = 1,
      .pSetLayouts = &vk->set_layout,
  };
  if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL,
                               &vk->pipe) != VK_SUCCESS ||
      vkCreateDescriptorPool(dev, &dpi, NULL, &vk->pool) != VK_SUCCESS ||
      (dai.descriptorPool = vk->pool,
       vkAllocateDescriptorSets(dev, &dai, &vk->set) != VK_SUCCESS)) {
    goto bad;
  }
  ctui_logf(E_INF, "[CTUI:VK] - backend made @ tick %d\n",
            ctui_tick_advance());
  return vk;
bad:
  ctui_logf(E_ERR, "[CTUI:VK] - the painter's pipeline couldn't be made\n");
  ctui_vk_free(vk);
  return NULL;
}

static void glyphs_for(int scale120, unsigned *w, unsigned *h,
                       const uint32_t **rows) {
  unsigned n = sizeof k_ui_fonts / sizeof k_ui_fonts[0], best = 0;
  unsigned s = (unsigned)(scale120 > 0 ? scale120 : 120);
  for (unsigned i = 0; i < n; i++) {
    if (k_ui_fonts[i].h * 120u <= 16u * s) {
      best = i;
    }
  }
  *w = k_ui_fonts[best].w;
  *h = k_ui_fonts[best].h;
  *rows = k_ui_fonts[best].rows;
}

static void all_tiles(CTUI_VK *vk) {
  memset(vk->tdirty, 1, (size_t)vk->tx * vk->ty);
}

int ctui_vk_resize(CTUI_VK *vk, unsigned w, unsigned h, int scale120,
                   int bgra, int *rows, int *cols) {
  if (!w || !h || w > 0xffffu * CTUI_VK_TILE || h > 0xffffu * CTUI_VK_TILE) {
    ctui_logf(E_WRN, "[CTUI:VK] - resize to %ux%u rejected @ tick %d\n", w, h,
              ctui_tick_advance());
    return -1;
  }
  vkDeviceWaitIdle(vk->dev);
  free_sized(vk);
  vk->w = w;
  vk->h = h;
  vk->bgra = bgra;
  glyphs_for(scale120, &vk->gw, &vk->gh, &vk->font);
  vk->cols = w / vk->gw;
  vk->rows = h / vk->gh;
  vk->tx = (w + CTUI_VK_TILE - 1) / CTUI_VK_TILE;
  vk->ty = (h + CTUI_VK_TILE - 1) / CTUI_VK_TILE;
  size_t ncells = (size_t)vk->cols * vk->rows, ntiles = (size_t)vk->tx * vk->ty;
  size_t cell_bytes = ncells * CTUI_VK_CELL_WORDS * 4u;
  size_t atlas_bytes = 256u * vk->gh * 4u;
  size_t stage_bytes = ntiles * 4u + atlas_bytes + cell_bytes;
  vk->rec = malloc(cell_bytes ? cell_bytes : 4);
  vk->dirty = calloc(ncells ? ncells : 1, 1);
  vk->tdirty = calloc(ntiles, 1);
  vk->behind = malloc(CTUI_VK_IMAGES * vk->ty * 2 * sizeof *vk->behind);
  /* a run is at least one cell between two that aren't: half of them,
   * and the tile list's and the atlas's */
  vk->copies = malloc((ncells / 2 + 3) * sizeof *vk->copies);
  int bad = !vk->rec || !vk->dirty || !vk->tdirty || !vk->behind ||
            !vk->copies || buf_new(vk, &vk->surf, (size_t)w * h * 4u, 0) ||
            buf_new(vk, &vk->cells, cell_bytes, 0) ||
            buf_new(vk, &vk->tiles, ntiles * 4u, 0) ||
            buf_new(vk, &vk->atlas, atlas_bytes, 0);
  for (unsigned i = 0; i < CTUI_VK_SLOTS && !bad; i++) {
    bad = buf_new(vk, &vk->stage[i], stage_bytes, 1);
  }
  if (bad) {
    ctui_logf(E_ERR, "[CTUI:VK] - no memory for a %ux%u surface\n", w, h);
    free_sized(vk);
    return -1;
  }
  for (size_t i = 0; i < ncells; i++) {
    uint32_t *r = vk->rec + i * CTUI_VK_CELL_WORDS;
    r[0] = ' ' | (CTUI_VK_FG_DEFAULT | CTUI_VK_BG_DEFAULT) << 16;
    r[1] = r[2] = 0;
  }
  memset(vk->dirty, 1, ncells);
  all_tiles(vk);
  for (unsigned i = 0; i < CTUI_VK_IMAGES * vk->ty; i++) {
    vk->behind[2 * i] = 1;
    vk->behind[2 * i + 1] = 0;
  }
  vk->atlas_up = 1;

  VkDescriptorBufferInfo info[4] = {
      {vk->tiles.buf, 0, VK_WHOLE_SIZE},
      {vk->cells.buf, 0, VK_WHOLE_SIZE},
      {vk->atlas.buf, 0, VK_WHOLE_SIZE},
      {vk->surf.buf, 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet wr[4];
  for (unsigned i = 0; i < 4; i++) {
    wr[i] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = vk->set,
        .dstBinding = i,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &info[i],
    };
  }
  vkUpdateDescriptorSets(vk->dev, 4, wr, 0, NULL);
  vk->sized = 1;
  ctui_cell_px_set((int)vk->gw, (int)vk->gh);
  *rows = (int)vk->rows;
  *cols = (int)vk->cols;
  ctui_logf(E_INF,
            "[CTUI:VK] - surface %ux%u, glyphs %ux%u, %ux%u cells @ tick %d\n",
            w, h, vk->gw, vk->gh, vk->cols, vk->rows, ctui_tick_advance());
  return 0;
}

void ctui_vk_set_default(CTUI_VK *vk, uint32_t fg, uint32_t bg) {
  vk->def_fg = fg & 0xffffffu;
  vk->def_bg = bg & 0xffffffu;
  if (vk->sized) {
    all_tiles(vk);
  }
}

void ctui_vk_defaults(const CTUI_VK *vk, uint32_t *fg, uint32_t *bg) {
  *fg = vk->def_fg;
  *bg = vk->def_bg;
}

void ctui_vk_glyphs(const CTUI_VK *vk, unsigned *w, unsigned *h,
                    const uint32_t **rows) {
  *w = vk->gw;
  *h = vk->gh;
  *rows = vk->font;
}

VkBuffer ctui_vk_surface(const CTUI_VK *vk) { return vk->surf.buf; }

/* xterm's 256: the 16, a 6 x 6 x 6 cube, 24 greys */
static uint32_t rgb256(unsigned i) {
  static const uint32_t k16[16] = {
      0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd,
      0x00cdcd, 0xe5e5e5, 0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
      0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff};
  if (i < 16) {
    return k16[i];
  }
  if (i < 232) {
    static const uint32_t lv[6] = {0, 95, 135, 175, 215, 255};
    i -= 16;
    return lv[i / 36] << 16 | lv[i / 6 % 6] << 8 | lv[i % 6];
  }
  uint32_t g = 8 + 10 * (i - 232);
  return g << 16 | g << 8 | g;
}

static uint32_t rgb_basic(unsigned char c) {
  unsigned char r, g, b;
  ctui_gfx_ansi16_rgb(c, &r, &g, &b);
  return (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}

static uint32_t glyph_slot(CTUI_VK *vk, uint32_t ch) {
  if (ch == 0 || ch == CTUI_CELL_CONT || ch == CTUI_GFX_KITTY_PLACEHOLDER) {
    return ' ';
  }
  if (!(ch & CTUI_CELL_CLUSTER)) {
    if ((ch >= 0x20 && ch <= 0x7e) || (ch >= 0xa0 && ch <= 0xff)) {
      return ch;
    }
    for (unsigned i = 0; i < vk->nextras; i++) {
      if (vk->extras[i] == ch) {
        return k_ui_font_extra_slot[i];
      }
    }
  }
  for (unsigned i = 0; i < vk->nmissing; i++) {
    if (vk->missing[i] == ch) {
      return CTUI_VK_GLYPH_BOX;
    }
  }
  if (vk->nmissing < MISSING_LOGGED) {
    vk->missing[vk->nmissing++] = ch;
    ctui_logf(E_WRN, "[CTUI:VK] - no glyph for 0x%x, drawn as a box\n",
              (unsigned)ch);
  }
  return CTUI_VK_GLYPH_BOX;
}

void ctui_vk_cell(CTUI_VK *vk, const CTUI_CELL *c, uint32_t *out) {
  uint32_t flags = 0, fg = 0, bg = 0;
  switch (c->color_mode) {
  case CTUI_COLOR_MODE_RGB:
    fg = (uint32_t)c->fg_r << 16 | (uint32_t)c->fg_g << 8 | c->fg_b;
    bg = (uint32_t)c->bg_r << 16 | (uint32_t)c->bg_g << 8 | c->bg_b;
    break;
  case CTUI_COLOR_MODE_256:
    fg = rgb256(c->fg);
    bg = rgb256(c->bg);
    break;
  case CTUI_COLOR_MODE_RGB_FG:
    fg = (uint32_t)c->fg_r << 16 | (uint32_t)c->fg_g << 8 | c->fg_b;
    if (c->bg == CTUI_COLOR_DEFAULT) {
      flags |= CTUI_VK_BG_DEFAULT;
    } else {
      bg = rgb_basic(c->bg);
    }
    break;
  default:
    if (c->fg == CTUI_COLOR_DEFAULT) {
      flags |= CTUI_VK_FG_DEFAULT;
    } else {
      fg = rgb_basic(c->fg);
    }
    if (c->bg == CTUI_COLOR_DEFAULT) {
      flags |= CTUI_VK_BG_DEFAULT;
    } else {
      bg = rgb_basic(c->bg);
    }
  }
  out[0] = glyph_slot(vk, c->ch) | (c->attr | flags) << 16;
  out[1] = fg;
  out[2] = bg;
}

static int vk_frame(void *ctx, const CTUI_SCREEN *s) {
  CTUI_VK *vk = ctx;
  if (!vk->sized) {
    return 0;
  }
  unsigned rows = (unsigned)s->rows < vk->rows ? (unsigned)s->rows : vk->rows;
  unsigned cols = (unsigned)s->cols < vk->cols ? (unsigned)s->cols : vk->cols;
  if ((unsigned)s->rows != vk->rows || (unsigned)s->cols != vk->cols) {
    ctui_logf(E_WRN,
              "[CTUI:VK] - a %dx%d frame on a %ux%u grid: the overlap "
              "painted @ tick %d\n",
              s->cols, s->rows, vk->cols, vk->rows, ctui_tick_advance());
  }
  unsigned changed = 0;
  for (unsigned r = 0; r < rows; r++) {
    for (unsigned c = 0; c < cols; c++) {
      size_t si = (size_t)r * (unsigned)s->cols + c;
      if (!memcmp(&s->cells[si], &s->buffer[si], sizeof s->cells[si])) {
        continue;
      }
      size_t k = (size_t)r * vk->cols + c;
      uint32_t w[CTUI_VK_CELL_WORDS];
      ctui_vk_cell(vk, &s->cells[si], w);
      uint32_t *have = vk->rec + k * CTUI_VK_CELL_WORDS;
      if (!memcmp(w, have, sizeof w)) {
        continue;
      }
      memcpy(have, w, sizeof w);
      vk->dirty[k] = 1;
      unsigned x0 = c * vk->gw / CTUI_VK_TILE,
               x1 = ((c + 1) * vk->gw - 1) / CTUI_VK_TILE,
               y0 = r * vk->gh / CTUI_VK_TILE,
               y1 = ((r + 1) * vk->gh - 1) / CTUI_VK_TILE;
      for (unsigned y = y0; y <= y1; y++) {
        memset(vk->tdirty + (size_t)y * vk->tx + x0, 1, x1 - x0 + 1);
      }
      changed++;
    }
  }
  ctui_logf(E_DBG, "[CTUI:VK] - frame: %u cells changed @ tick %d\n", changed,
            ctui_tick_advance());
  return 0;
}

const CTUI_SCREEN_SINK *ctui_vk_screen_sink(void) {
  static const CTUI_SCREEN_SINK sink = {vk_frame};
  return &sink;
}

int ctui_vk_pending(const CTUI_VK *vk) {
  return vk->sized && memchr(vk->tdirty, 1, (size_t)vk->tx * vk->ty) != NULL;
}

static void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src,
                    VkAccessFlags src_access, VkPipelineStageFlags dst,
                    VkAccessFlags dst_access) {
  VkMemoryBarrier mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = src_access,
      .dstAccessMask = dst_access,
  };
  vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}

unsigned ctui_vk_record(CTUI_VK *vk, VkCommandBuffer cmd) {
  if (!vk->sized) {
    return 0;
  }
  vk->slot = (vk->slot + 1) % CTUI_VK_SLOTS;
  uint32_t *st = vk->stage[vk->slot].map;
  unsigned nt = 0;
  for (unsigned y = 0; y < vk->ty; y++) {
    int first = -1, last = -1;
    for (unsigned x = 0; x < vk->tx; x++) {
      if (vk->tdirty[y * vk->tx + x]) {
        st[nt++] = x | y << 16;
        first = first < 0 ? (int)x : first;
        last = (int)x;
      }
    }
    for (unsigned img = 0; first >= 0 && img < CTUI_VK_IMAGES; img++) {
      int *b = vk->behind + (img * vk->ty + y) * 2;
      if (b[0] > b[1]) {
        b[0] = first, b[1] = last;
      } else {
        b[0] = first < b[0] ? first : b[0];
        b[1] = last > b[1] ? last : b[1];
      }
    }
  }
  if (!nt) {
    return 0;
  }
  memset(vk->tdirty, 0, (size_t)vk->tx * vk->ty);
  size_t off = (size_t)vk->tx * vk->ty; /* in words */
  VkBufferCopy atlas = {0};
  if (vk->atlas_up) {
    atlas = (VkBufferCopy){off * 4u, 0, 256u * vk->gh * 4u};
    memcpy(st + off, vk->font, atlas.size);
    off += 256u * vk->gh;
  }
  unsigned nc = 0;
  size_t ncells = (size_t)vk->cols * vk->rows;
  const size_t cw = CTUI_VK_CELL_WORDS;
  for (size_t i = 0; i < ncells; i++) {
    if (!vk->dirty[i]) {
      continue;
    }
    size_t j = i;
    while (j < ncells && vk->dirty[j]) {
      vk->dirty[j++] = 0;
    }
    memcpy(st + off, vk->rec + i * cw, (j - i) * cw * 4u);
    vk->copies[nc++] = (VkBufferCopy){off * 4u, i * cw * 4u, (j - i) * cw * 4u};
    off += (j - i) * cw;
    i = j;
  }

  VkBuffer stage = vk->stage[vk->slot].buf;
  /* the last record's dispatch and the caller's copies out of the
   * surface done before anything here writes */
  barrier(cmd,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
  VkBufferCopy tiles = {0, 0, nt * 4u};
  vkCmdCopyBuffer(cmd, stage, vk->tiles.buf, 1, &tiles);
  if (vk->atlas_up) {
    vkCmdCopyBuffer(cmd, stage, vk->atlas.buf, 1, &atlas);
    vk->atlas_up = 0;
  }
  if (nc) {
    vkCmdCopyBuffer(cmd, stage, vk->cells.buf, nc, vk->copies);
  }
  barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  PUSH p = {vk->w,    vk->h,          vk->gw,     vk->gh,    vk->cols,
            vk->rows, (uint32_t)vk->bgra, vk->def_fg, vk->def_bg};
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->pipe);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->layout, 0,
                          1, &vk->set, 0, NULL);
  vkCmdPushConstants(cmd, vk->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p,
                     &p);
  vkCmdDispatch(cmd, nt, 1, 1);
  barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  ctui_logf(E_DBG, "[CTUI:VK] - recorded %u tiles, %u cell runs @ tick %d\n",
            nt, nc, ctui_tick_advance());
  return nt;
}

unsigned ctui_vk_regions(CTUI_VK *vk, unsigned img, int fresh,
                         VkBufferImageCopy *out) {
  if (!vk->sized || img >= CTUI_VK_IMAGES) {
    return 0;
  }
  int *b = vk->behind + img * vk->ty * 2;
  unsigned n = 0;
  int pf = -1, pl = -1; /* the region being grown: its tile span */
  for (unsigned y = 0; y < vk->ty; y++) {
    int f = fresh ? 0 : b[2 * y], l = fresh ? (int)vk->tx - 1 : b[2 * y + 1];
    b[2 * y] = 1;
    b[2 * y + 1] = 0;
    if (f > l) {
      pf = pl = -1;
      continue;
    }
    unsigned py = y * CTUI_VK_TILE,
             ph = vk->h - py < CTUI_VK_TILE ? vk->h - py : CTUI_VK_TILE;
    if (n && f == pf && l == pl) {
      out[n - 1].imageExtent.height += ph;
      continue;
    }
    unsigned px = (unsigned)f * CTUI_VK_TILE,
             pr = ((unsigned)l + 1) * CTUI_VK_TILE;
    pr = pr < vk->w ? pr : vk->w;
    out[n++] = (VkBufferImageCopy){
        .bufferOffset = ((VkDeviceSize)py * vk->w + px) * 4u,
        .bufferRowLength = vk->w,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {(int32_t)px, (int32_t)py, 0},
        .imageExtent = {pr - px, ph, 1},
    };
    pf = f;
    pl = l;
  }
  return n;
}
