/* clock_gettime() is POSIX, not C11 */
#define _POSIX_C_SOURCE 200809L

#include "vk.h"

#include "look/png.h"
#include "ttf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const uint32_t k_cells_spv[] =
#include "shaders/cells.spv.h"
    ;
/* the painter into a storage image (glslc -DDIRECT): optional, so an app
 * whose build makes only cells.spv.h keeps the surface buffer */
#if defined(__has_include)
#if __has_include("shaders/cells_direct.spv.h")
#define HAVE_DIRECT 1
static const uint32_t k_cells_direct_spv[] =
#include "shaders/cells_direct.spv.h"
    ;
#endif
#endif

#include "fonts/JetBrainsMono-Regular.ttf.h"

typedef struct {
  VkBuffer buf;
  VkDeviceMemory mem;
  void *map; /* host-visible ones */
} BUF;

typedef struct {
  uint32_t surf_w, surf_h, gw, gh, cols, rows, bgra, def_fg, def_bg, nplace,
      ntab, pad_x, pad_y;
} PUSH;

/* the shader's bindings: tiles, cells, atlas, surface, placements, the
 * images' texels, the image table (placeholder cells' images by id) */
#define BINDINGS 7u
/* the surface: a buffer, or direct the image painted into */
#define SURF_BINDING 3u
/* an image on the card for placeholder cells: id, offset, w h (texels),
 * its virtual placement's cols rows; a wipe's split column (NO_WIPE:
 * none), its left image's offset, its marker's offset, w h (w 0: none) */
#define TAB_WORDS 12u
#define NO_WIPE 0xffffffffu
/* a placement on the card: x y w h (pixels), the image's offset in the
 * arena (words), its w h (texels), z */
#define PLACE_WORDS 8u

/* an image (gfx.h's sink): its texels kept on the host too (what goes
 * up, and again when the arena is compacted), the virtual placement
 * placeholder cells show it in */
typedef struct {
  unsigned id;
  int w, h;
  uint32_t *px;       /* RGBA bytes, w * h words */
  size_t cap;         /* words px holds */
  int vcols, vrows;   /* place_file()'s cells; 0: none */
  uint32_t off, room; /* where in the arena, how many words */
  int up;             /* texels to go up */
  /* a wipe (gfx.h's): left of split from left, the marker over it; x
   * from x0 at t0 (CLOCK_MONOTONIC ns) at v px a second, to x_end */
  unsigned left, marker;
  double x0, v, x_end;
  uint64_t t0;
  int split;
} IMG;

/* a placement: display()/image_commit() are placement 0 of their image,
 * put() any other */
typedef struct {
  unsigned id, pid;
  int row, col, cols, rows, z; /* 0-based cells */
  int px, py, pw, ph; /* display_px()'s rect in the cells' pixels; pw 0: the cells */
} PLC;

/* the font's size: kitty's default, 11 pt at 96 dpi a logical pixel */
#define FONT_EM_PX (11.0f * 96.0f / 72.0f)
/* the glyph cache's first size on the card, in slots (it doubles) */
#define GLYPHS_FIRST 512u
/* the glyph map's first size, in entries (a power of two; it doubles
 * past half full) */
#define GMAP_FIRST 1024u
/* a map key's flag: the glyph drawn across two cells */
#define GKEY_WIDE 0x80000000u

/* the glyph map's entry: key 0 empty, else a codepoint (| GKEY_WIDE) and
 * its slot (CTUI_VK_GLYPH_BOX: no font has it) */
typedef struct {
  uint32_t key;
  uint32_t slot;
} GENT;

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
  /* painting into the caller's images (ctui_vk_set_direct()): no surface
   * buffer, a set per swapchain image, its binding 3 that image's view */
  int direct;
  VkShaderModule dmodule;
  VkDescriptorSetLayout dset_layout;
  VkPipelineLayout dlayout;
  VkPipeline dpipe;
  VkDescriptorPool dpool;
  VkDescriptorSet dset[CTUI_VK_IMAGES];
  /* on the card: the surface, the cells, the tile list, the atlas */
  BUF surf, cells, tiles, atlas;
  /* mapped, one a recording in flight: a record's tile list, the atlas
   * when it goes up, the changed cells' runs */
  BUF stage[CTUI_VK_SLOTS];
  unsigned slot;
  VkBufferCopy *copies;
  unsigned w, h, gw, gh, cols, rows, tx, ty;
  int bgra, sized;
  uint32_t def_fg, def_bg;
  /* the 16 colours a 256 / basic index takes (ctui_vk_set_palette()) */
  uint32_t pal[16];
  /* the inset asked (logical px: top, right, bottom, left) and the cells'
   * origin it made at the last resize (buffer px) */
  unsigned pad[4], px0, py0;
  /* the font (CTUI_FONT's file, else the one built in) and its size */
  CTUI_TTF ttf;
  uint8_t *ttf_file;
  float em;
  int base;
  /* the glyph cache: the codepoints drawn so far (gmap, open addressing
   * of gmap_cap entries, gmap_n used), each slot's coverage on the host
   * (slot_words words, 4 bytes a word, rows of gw), slot 0 blank; a wide
   * glyph is two slots, its left half then its right; slots from up_from
   * go up with the next record, into an atlas on the card of atlas_cap
   * slots */
  GENT *gmap;
  unsigned gmap_cap, gmap_n;
  uint32_t *glyphs;
  unsigned slot_words, nslots, slots_cap, up_from, atlas_cap;
  uint8_t *cov; /* a wide glyph's coverage: 2 * gw x gh */
  /* what the font lacks is asked of this, when set */
  CTUI_VK_GLYPH_FN glyph_fn;
  void *glyph_ctx;
  uint32_t *rec;         /* the cells as on the card, cols * rows * WORDS */
  unsigned char *dirty;  /* a cell changed since the last record */
  unsigned char *tdirty; /* a tile to paint */
  /* per swapchain image and tile row, the span of tiles it's behind on:
   * [0] first, [1] last; first > last when none */
  int *behind;

  IMG *imgs;
  unsigned nimg, img_cap;
  PLC *plcs;
  unsigned nplc, plc_cap;
  BUF tab;
  size_t tab_cap;
  unsigned shown, tabbed; /* placements, images in the table, on the card */
  /* on the card: every image's texels at its offset (a bump allocation,
   * compacted when full), the placements, sorted by z */
  BUF arena, places;
  size_t arena_words, arena_used, places_cap;
  int places_up;
  size_t stage_bytes[CTUI_VK_SLOTS];
  VkBufferCopy *icopies;
  unsigned icopies_cap;
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

/* CTUI_FONT's file when it reads as a font, else the one built in */
static void load_font(CTUI_VK *vk) {
  const char *path = getenv("CTUI_FONT");
  FILE *fp = path && *path ? fopen(path, "rb") : NULL;
  if (fp) {
    long n = fseek(fp, 0, SEEK_END) == 0 ? ftell(fp) : -1;
    uint8_t *bytes = n > 0 ? malloc((size_t)n) : NULL;
    if (bytes && fseek(fp, 0, SEEK_SET) == 0 && fread(bytes, 1, (size_t)n, fp) == (size_t)n &&
        ctui_ttf_load(&vk->ttf, bytes, (size_t)n) == 0) {
      vk->ttf_file = bytes;
      ctui_logf(E_INF, "[CTUI:VK] - font %s\n", path);
    } else {
      free(bytes);
      ctui_logf(E_WRN, "[CTUI:VK] - CTUI_FONT %s isn't a TrueType font: the built-in one\n",
                path);
    }
    fclose(fp);
  } else if (path && *path) {
    ctui_logf(E_WRN, "[CTUI:VK] - CTUI_FONT %s can't be read: the built-in one\n", path);
  }
  if (!vk->ttf_file && ctui_ttf_load(&vk->ttf, ctui_vk_font_ttf, ctui_vk_font_ttf_len) != 0) {
    ctui_logf(E_ERR, "[CTUI:VK] - the built-in font doesn't load\n");
  }
}

/* the cache emptied for glyphs gw x gh: slot 0 the blank */
static int glyphs_reset(CTUI_VK *vk) {
  vk->slot_words = (vk->gw * vk->gh + 3) / 4;
  free(vk->gmap);
  free(vk->glyphs);
  free(vk->cov);
  vk->gmap_cap = GMAP_FIRST;
  vk->gmap_n = 0;
  vk->gmap = calloc(vk->gmap_cap, sizeof *vk->gmap);
  vk->slots_cap = GLYPHS_FIRST;
  vk->glyphs = calloc((size_t)vk->slots_cap * vk->slot_words, 4);
  vk->cov = malloc((size_t)vk->gw * 2u * vk->gh);
  if (!vk->gmap || !vk->glyphs || !vk->cov) {
    return -1;
  }
  vk->nslots = 1;
  vk->up_from = 0;
  vk->atlas_cap = 0;
  return 0;
}

static void free_sized(CTUI_VK *vk) {
  buf_free(vk, &vk->surf);
  buf_free(vk, &vk->cells);
  buf_free(vk, &vk->tiles);
  buf_free(vk, &vk->atlas);
  buf_free(vk, &vk->arena);
  buf_free(vk, &vk->places);
  buf_free(vk, &vk->tab);
  for (unsigned i = 0; i < CTUI_VK_SLOTS; i++) {
    buf_free(vk, &vk->stage[i]);
    vk->stage_bytes[i] = 0;
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
  for (unsigned i = 0; i < vk->nimg; i++) {
    free(vk->imgs[i].px);
  }
  free(vk->imgs);
  free(vk->plcs);
  free(vk->icopies);
  free(vk->gmap);
  free(vk->glyphs);
  free(vk->cov);
  free(vk->ttf_file);
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
  if (vk->dpool) {
    vkDestroyDescriptorPool(vk->dev, vk->dpool, NULL);
  }
  if (vk->dpipe) {
    vkDestroyPipeline(vk->dev, vk->dpipe, NULL);
  }
  if (vk->dlayout) {
    vkDestroyPipelineLayout(vk->dev, vk->dlayout, NULL);
  }
  if (vk->dset_layout) {
    vkDestroyDescriptorSetLayout(vk->dev, vk->dset_layout, NULL);
  }
  if (vk->dmodule) {
    vkDestroyShaderModule(vk->dev, vk->dmodule, NULL);
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
  ctui_vk_set_palette(vk, NULL);
  vkGetPhysicalDeviceMemoryProperties(phys, &vk->mem);
  load_font(vk);

  VkDescriptorSetLayoutBinding bind[BINDINGS];
  for (unsigned i = 0; i < BINDINGS; i++) {
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
      .bindingCount = BINDINGS,
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
  VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, BINDINGS};
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

static void all_tiles(CTUI_VK *vk) {
  memset(vk->tdirty, 1, (size_t)vk->tx * vk->ty);
}

/* the buffers into the set the painter binds: direct, every image's set
 * but its binding 3 (the image, ctui_vk_set_images()); called with the
 * device idle */
static void describe(CTUI_VK *vk) {
  const BUF *b[BINDINGS] = {&vk->tiles,  &vk->cells, &vk->atlas, &vk->surf,
                            &vk->places, &vk->arena, &vk->tab};
  VkDescriptorBufferInfo info[BINDINGS];
  VkWriteDescriptorSet wr[BINDINGS * CTUI_VK_IMAGES];
  unsigned n = 0, sets = vk->direct ? CTUI_VK_IMAGES : 1;
  for (unsigned i = 0; i < BINDINGS; i++) {
    info[i] = (VkDescriptorBufferInfo){b[i]->buf, 0, VK_WHOLE_SIZE};
  }
  for (unsigned k = 0; k < sets; k++) {
    for (unsigned i = 0; i < BINDINGS; i++) {
      if (vk->direct && i == SURF_BINDING) {
        continue;
      }
      wr[n++] = (VkWriteDescriptorSet){
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = vk->direct ? vk->dset[k] : vk->set,
          .dstBinding = i,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &info[i],
      };
    }
  }
  vkUpdateDescriptorSets(vk->dev, n, wr, 0, NULL);
}

/* the direct painter's pipeline and sets, made the first time */
static int direct_new(CTUI_VK *vk) {
#ifdef HAVE_DIRECT
  if (vk->dpipe) {
    return 0;
  }
  VkDescriptorSetLayoutBinding bind[BINDINGS];
  for (unsigned i = 0; i < BINDINGS; i++) {
    bind[i] = (VkDescriptorSetLayoutBinding){
        .binding = i,
        .descriptorType = i == SURF_BINDING ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                            : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
  }
  VkShaderModuleCreateInfo smi = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof k_cells_direct_spv,
      .pCode = k_cells_direct_spv,
  };
  VkDescriptorSetLayoutCreateInfo dli = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = BINDINGS,
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
  VkDescriptorPoolSize size[2] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (BINDINGS - 1) * CTUI_VK_IMAGES},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CTUI_VK_IMAGES},
  };
  VkDescriptorPoolCreateInfo dpi = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = CTUI_VK_IMAGES,
      .poolSizeCount = 2,
      .pPoolSizes = size,
  };
  if (vkCreateShaderModule(vk->dev, &smi, NULL, &vk->dmodule) != VK_SUCCESS ||
      vkCreateDescriptorSetLayout(vk->dev, &dli, NULL, &vk->dset_layout) !=
          VK_SUCCESS ||
      (pli.pSetLayouts = &vk->dset_layout,
       vkCreatePipelineLayout(vk->dev, &pli, NULL, &vk->dlayout) != VK_SUCCESS)) {
    return -1;
  }
  VkComputePipelineCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = vk->dmodule,
                .pName = "main"},
      .layout = vk->dlayout,
  };
  VkDescriptorSetLayout layouts[CTUI_VK_IMAGES];
  for (unsigned i = 0; i < CTUI_VK_IMAGES; i++) {
    layouts[i] = vk->dset_layout;
  }
  VkDescriptorSetAllocateInfo dai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorSetCount = CTUI_VK_IMAGES,
      .pSetLayouts = layouts,
  };
  if (vkCreateComputePipelines(vk->dev, VK_NULL_HANDLE, 1, &cpi, NULL,
                               &vk->dpipe) != VK_SUCCESS) {
    vk->dpipe = VK_NULL_HANDLE;
    return -1;
  }
  if (vkCreateDescriptorPool(vk->dev, &dpi, NULL, &vk->dpool) != VK_SUCCESS ||
      (dai.descriptorPool = vk->dpool,
       vkAllocateDescriptorSets(vk->dev, &dai, vk->dset) != VK_SUCCESS)) {
    vkDestroyPipeline(vk->dev, vk->dpipe, NULL);
    vk->dpipe = VK_NULL_HANDLE;
    return -1;
  }
  return 0;
#else
  (void)vk;
  return -1;
#endif
}

int ctui_vk_set_direct(CTUI_VK *vk, int on) {
  on = !!on;
  if (on == vk->direct) {
    return 0;
  }
  if (on && direct_new(vk) != 0) {
    ctui_logf(E_WRN, "[CTUI:VK] - no direct painter: the surface buffer stays\n");
    return -1;
  }
  vkDeviceWaitIdle(vk->dev);
  if (vk->sized) {
    if (on) {
      buf_free(vk, &vk->surf);
    } else if (buf_new(vk, &vk->surf, (size_t)vk->w * vk->h * 4u, 0)) {
      ctui_logf(E_ERR, "[CTUI:VK] - no memory for the surface buffer\n");
      return -1;
    }
    /* what's on the card is in neither: every tile again */
    all_tiles(vk);
    for (unsigned i = 0; i < CTUI_VK_IMAGES * vk->ty; i++) {
      vk->behind[2 * i] = 0;
      vk->behind[2 * i + 1] = (int)vk->tx - 1;
    }
  }
  vk->direct = on;
  if (vk->sized) {
    describe(vk);
  }
  ctui_logf(E_INF, "[CTUI:VK] - painting %s @ tick %d\n",
            on ? "into the images" : "into the surface buffer", ctui_tick_advance());
  return 0;
}

int ctui_vk_direct(const CTUI_VK *vk) { return vk->direct; }

void ctui_vk_set_images(CTUI_VK *vk, unsigned n, const VkImageView *views) {
  VkDescriptorImageInfo info[CTUI_VK_IMAGES];
  VkWriteDescriptorSet wr[CTUI_VK_IMAGES];
  n = n > CTUI_VK_IMAGES ? CTUI_VK_IMAGES : n;
  if (!vk->direct) {
    return;
  }
  for (unsigned i = 0; i < n; i++) {
    info[i] = (VkDescriptorImageInfo){.imageView = views[i],
                                      .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    wr[i] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = vk->dset[i],
        .dstBinding = SURF_BINDING,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .pImageInfo = &info[i],
    };
  }
  vkUpdateDescriptorSets(vk->dev, n, wr, 0, NULL);
}

static size_t live_words(const CTUI_VK *vk) {
  size_t n = 0;
  for (unsigned i = 0; i < vk->nimg; i++) {
    n += (size_t)vk->imgs[i].w * (size_t)vk->imgs[i].h;
  }
  return n;
}

/* the arena for at least words texels, every image to go up again at a
 * fresh offset (the device idle: the old one may be in use) */
static int arena_new(CTUI_VK *vk, size_t words) {
  size_t want = words * 2 > (1u << 18) ? words * 2 : (1u << 18);
  buf_free(vk, &vk->arena);
  if (buf_new(vk, &vk->arena, want * 4u, 0)) {
    vk->arena_words = 0;
    return -1;
  }
  vk->arena_words = want;
  vk->arena_used = 0;
  for (unsigned i = 0; i < vk->nimg; i++) {
    vk->imgs[i].room = 0;
    vk->imgs[i].up = 1;
  }
  vk->places_up = 1;
  return 0;
}

/* room for n placements and n images in the table */
static int places_new(CTUI_VK *vk, size_t n) {
  size_t want = n * 2 > 16 ? n * 2 : 16;
  buf_free(vk, &vk->places);
  buf_free(vk, &vk->tab);
  if (buf_new(vk, &vk->places, want * PLACE_WORDS * 4u, 0) ||
      buf_new(vk, &vk->tab, want * TAB_WORDS * 4u, 0)) {
    vk->places_cap = 0;
    return -1;
  }
  vk->places_cap = want;
  vk->places_up = 1;
  return 0;
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
  vk->em = FONT_EM_PX * (float)(scale120 > 0 ? scale120 : 120) / 120.0f;
  int cw, ch;
  ctui_ttf_cell(&vk->ttf, vk->em, &cw, &ch, &vk->base);
  vk->gw = (unsigned)(cw > 0 ? cw : 1);
  vk->gh = (unsigned)(ch > 0 ? ch : 1);
  if (glyphs_reset(vk)) {
    ctui_logf(E_ERR, "[CTUI:VK] - no memory for the glyph cache\n");
    return -1;
  }
  /* the inset at this scale, as kitty pads: the cells from its top left,
   * what they leave over right and below; the cells first when it leaves
   * none */
  unsigned s120 = (unsigned)(scale120 > 0 ? scale120 : 120), p[4];
  for (int i = 0; i < 4; i++) {
    p[i] = (vk->pad[i] * s120 + 60u) / 120u;
  }
  if (p[1] + p[3] + vk->gw > w || p[0] + p[2] + vk->gh > h) {
    p[0] = p[1] = p[2] = p[3] = 0;
  }
  vk->px0 = p[3];
  vk->py0 = p[0];
  vk->cols = (w - p[1] - p[3]) / vk->gw;
  vk->rows = (h - p[0] - p[2]) / vk->gh;
  vk->tx = (w + CTUI_VK_TILE - 1) / CTUI_VK_TILE;
  vk->ty = (h + CTUI_VK_TILE - 1) / CTUI_VK_TILE;
  size_t ncells = (size_t)vk->cols * vk->rows, ntiles = (size_t)vk->tx * vk->ty;
  size_t cell_bytes = ncells * CTUI_VK_CELL_WORDS * 4u;
  size_t atlas_bytes = (size_t)vk->slots_cap * vk->slot_words * 4u;
  vk->rec = malloc(cell_bytes ? cell_bytes : 4);
  vk->dirty = calloc(ncells ? ncells : 1, 1);
  vk->tdirty = calloc(ntiles, 1);
  vk->behind = malloc(CTUI_VK_IMAGES * vk->ty * 2 * sizeof *vk->behind);
  /* a run is at least one cell between two that aren't: half of them,
   * and the tile list's and the atlas's */
  vk->copies = malloc((ncells / 2 + 3) * sizeof *vk->copies);
  int bad = !vk->rec || !vk->dirty || !vk->tdirty || !vk->behind ||
            !vk->copies ||
            (!vk->direct && buf_new(vk, &vk->surf, (size_t)w * h * 4u, 0)) ||
            buf_new(vk, &vk->cells, cell_bytes, 0) ||
            buf_new(vk, &vk->tiles, ntiles * 4u, 0) ||
            buf_new(vk, &vk->atlas, atlas_bytes, 0) ||
            arena_new(vk, live_words(vk)) ||
            places_new(vk, vk->nplc > vk->nimg ? vk->nplc : vk->nimg);
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
  vk->atlas_cap = vk->slots_cap;

  describe(vk);
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

void ctui_vk_set_palette(CTUI_VK *vk, const uint32_t pal[16]) {
  /* xterm's */
  static const uint32_t k16[16] = {
      0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd,
      0x00cdcd, 0xe5e5e5, 0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
      0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff};
  for (int i = 0; i < 16; i++) {
    vk->pal[i] = (pal ? pal[i] : k16[i]) & 0xffffffu;
  }
}

void ctui_vk_set_pad(CTUI_VK *vk, const unsigned pad[4]) {
  for (int i = 0; i < 4; i++) {
    vk->pad[i] = pad ? pad[i] : 0;
  }
}

void ctui_vk_origin(const CTUI_VK *vk, unsigned *x, unsigned *y) {
  *x = vk->px0;
  *y = vk->py0;
}

void ctui_vk_defaults(const CTUI_VK *vk, uint32_t *fg, uint32_t *bg) {
  *fg = vk->def_fg;
  *bg = vk->def_bg;
}

void ctui_vk_glyphs(const CTUI_VK *vk, unsigned *w, unsigned *h,
                    const uint32_t **slots, unsigned *slot_words) {
  *w = vk->gw;
  *h = vk->gh;
  *slots = vk->glyphs;
  *slot_words = vk->slot_words;
}

VkBuffer ctui_vk_surface(const CTUI_VK *vk) { return vk->surf.buf; }

/* xterm's 256: the palette's 16, a 6 x 6 x 6 cube, 24 greys */
static uint32_t rgb256(const CTUI_VK *vk, unsigned i) {
  if (i < 16) {
    return vk->pal[i];
  }
  if (i < 232) {
    static const uint32_t lv[6] = {0, 95, 135, 175, 215, 255};
    i -= 16;
    return lv[i / 36] << 16 | lv[i / 6 % 6] << 8 | lv[i % 6];
  }
  uint32_t g = 8 + 10 * (i - 232);
  return g << 16 | g << 8 | g;
}

/* a CTUI_COLOR_* index (BLACK..WHITE: the palette's 0..7) */
static uint32_t rgb_basic(const CTUI_VK *vk, unsigned char c) {
  return c >= CTUI_COLOR_BLACK && c <= CTUI_COLOR_WHITE
             ? vk->pal[c - CTUI_COLOR_BLACK]
             : 0;
}

/* box drawing and blocks: stretched to the cell, so lines meet */
static int stretched(uint32_t ch) { return ch >= 0x2500 && ch <= 0x259f; }

static uint32_t gmap_hash(uint32_t key) {
  key ^= key >> 16;
  key *= 0x7feb352du;
  key ^= key >> 15;
  return key;
}

/* key's entry: its own, or the empty one it would go in */
static GENT *gmap_find(const CTUI_VK *vk, uint32_t key) {
  unsigned mask = vk->gmap_cap - 1, i = gmap_hash(key) & mask;
  while (vk->gmap[i].key && vk->gmap[i].key != key) {
    i = (i + 1) & mask;
  }
  return &vk->gmap[i];
}

/* key -> slot in the map, doubled first when half full; -1 without
 * memory (the map as it was) */
static int gmap_put(CTUI_VK *vk, uint32_t key, uint32_t slot) {
  if ((vk->gmap_n + 1) * 2 > vk->gmap_cap) {
    GENT *old = vk->gmap;
    unsigned cap = vk->gmap_cap;
    GENT *more = calloc((size_t)cap * 2, sizeof *more);
    if (!more) {
      return -1;
    }
    vk->gmap = more;
    vk->gmap_cap = cap * 2;
    for (unsigned i = 0; i < cap; i++) {
      if (old[i].key) {
        *gmap_find(vk, old[i].key) = old[i];
      }
    }
    free(old);
  }
  GENT *e = gmap_find(vk, key);
  if (!e->key) {
    vk->gmap_n++;
  }
  e->key = key;
  e->slot = slot;
  return 0;
}

void ctui_vk_set_glyph_fn(CTUI_VK *vk, CTUI_VK_GLYPH_FN fn, void *ctx) {
  vk->glyph_fn = fn;
  vk->glyph_ctx = ctx;
  /* what was a box may be drawn now: the map rebuilt without them */
  if (!vk->gmap) {
    return;
  }
  GENT *old = vk->gmap;
  unsigned cap = vk->gmap_cap;
  vk->gmap = calloc(cap, sizeof *vk->gmap);
  if (!vk->gmap) {
    vk->gmap = old;
    return;
  }
  vk->gmap_n = 0;
  for (unsigned i = 0; i < cap; i++) {
    if (old[i].key && old[i].slot != CTUI_VK_GLYPH_BOX) {
      *gmap_find(vk, old[i].key) = old[i];
      vk->gmap_n++;
    }
  }
  free(old);
}

/* cov (cells * gw x gh) into the slots from slot on, a cell each */
static void slots_fill(CTUI_VK *vk, unsigned slot, unsigned cells) {
  unsigned stride = cells * vk->gw;
  for (unsigned k = 0; k < cells; k++) {
    uint32_t *w = vk->glyphs + (size_t)(slot + k) * vk->slot_words;
    memset(w, 0, vk->slot_words * 4u);
    for (unsigned y = 0, i = 0; y < vk->gh; y++) {
      const uint8_t *row = vk->cov + (size_t)y * stride + k * vk->gw;
      for (unsigned x = 0; x < vk->gw; x++, i++) {
        w[i / 4] |= (uint32_t)row[x] << (8 * (i % 4));
      }
    }
  }
}

/* cp drawn into cov (cells * gw x gh): the font's glyph, else the
 * provider's. 0, or -1 when neither has it */
static int glyph_draw(CTUI_VK *vk, uint32_t cp, unsigned cells) {
  int cw = (int)(cells * vk->gw);
  uint16_t g = ctui_ttf_glyph(&vk->ttf, cp);
  if (g) {
    if (ctui_ttf_draw(&vk->ttf, g, vk->em, cw, (int)vk->gh, vk->base,
                      cells == 1 && stretched(cp), vk->cov)) {
      ctui_logf(E_WRN, "[CTUI:VK] - glyph 0x%x's outline unreadable: blank\n",
                (unsigned)cp);
    }
    return 0;
  }
  if (vk->glyph_fn) {
    memset(vk->cov, 0, (size_t)cw * vk->gh);
    if (vk->glyph_fn(vk->glyph_ctx, cp, (int)cells, (int)vk->gw, (int)vk->gh,
                     vk->base, vk->em, vk->cov) == 0) {
      return 0;
    }
  }
  return -1;
}

/* ch's slot in the cache (a wide glyph's left half: its right is the next
 * slot), drawn into it the first time. A cluster is its first codepoint
 * (one font, no shaping) at the cluster's width; what no font has is the
 * box, logged once */
static uint32_t glyph_slot(CTUI_VK *vk, uint32_t ch, int *wide) {
  *wide = 0;
  if (ch == 0 || ch == ' ' || ch == CTUI_CELL_CONT) {
    return 0;
  }
  uint32_t cp = ch;
  if (ch & CTUI_CELL_CLUSTER) {
    int n = 0;
    const uint32_t *cps = ctui_cell_cluster(ch, &n);
    if (!cps || n < 1) {
      return CTUI_VK_GLYPH_BOX;
    }
    cp = cps[0];
  }
  if (cp > 0x10ffffu) {
    return CTUI_VK_GLYPH_BOX;
  }
  unsigned cells = ctui_cell_width(ch) == 2 ? 2u : 1u;
  uint32_t key = cp | (cells == 2 ? GKEY_WIDE : 0u);
  const GENT *e = gmap_find(vk, key);
  if (e->key) {
    *wide = e->slot != CTUI_VK_GLYPH_BOX && cells == 2;
    return e->slot;
  }
  if (vk->nslots + cells >= CTUI_VK_GLYPH_BOX || glyph_draw(vk, cp, cells)) {
    ctui_logf(E_WRN, "[CTUI:VK] - no glyph for 0x%x, drawn as a box\n",
              (unsigned)cp);
    (void)gmap_put(vk, key, CTUI_VK_GLYPH_BOX);
    return CTUI_VK_GLYPH_BOX;
  }
  while (vk->nslots + cells > vk->slots_cap) {
    uint32_t *more = realloc(vk->glyphs, (size_t)vk->slots_cap * 2 * vk->slot_words * 4);
    if (!more) {
      ctui_logf(E_ERR, "[CTUI:VK] - no memory for glyph 0x%x\n", (unsigned)cp);
      return CTUI_VK_GLYPH_BOX;
    }
    vk->glyphs = more;
    vk->slots_cap *= 2;
  }
  unsigned slot = vk->nslots;
  if (gmap_put(vk, key, slot)) {
    ctui_logf(E_ERR, "[CTUI:VK] - no memory for glyph 0x%x\n", (unsigned)cp);
    return CTUI_VK_GLYPH_BOX;
  }
  vk->nslots += cells;
  slots_fill(vk, slot, cells);
  *wide = cells == 2;
  return slot;
}

void ctui_vk_cell(CTUI_VK *vk, const CTUI_CELL *c, uint32_t *out) {
  uint32_t flags = 0, fg = 0, bg = 0;
  switch (c->color_mode) {
  case CTUI_COLOR_MODE_RGB:
    fg = (uint32_t)c->fg_r << 16 | (uint32_t)c->fg_g << 8 | c->fg_b;
    bg = (uint32_t)c->bg_r << 16 | (uint32_t)c->bg_g << 8 | c->bg_b;
    break;
  case CTUI_COLOR_MODE_256:
    fg = rgb256(vk, c->fg);
    bg = rgb256(vk, c->bg);
    break;
  case CTUI_COLOR_MODE_RGB_FG:
    fg = (uint32_t)c->fg_r << 16 | (uint32_t)c->fg_g << 8 | c->fg_b;
    if (c->bg == CTUI_COLOR_DEFAULT) {
      flags |= CTUI_VK_BG_DEFAULT;
    } else {
      bg = rgb_basic(vk, c->bg);
    }
    break;
  default:
    if (c->fg == CTUI_COLOR_DEFAULT) {
      flags |= CTUI_VK_FG_DEFAULT;
    } else {
      fg = rgb_basic(vk, c->fg);
    }
    if (c->bg == CTUI_COLOR_DEFAULT) {
      flags |= CTUI_VK_BG_DEFAULT;
    } else {
      bg = rgb_basic(vk, c->bg);
    }
  }
  if (c->ch == CTUI_GFX_KITTY_PLACEHOLDER) {
    /* the image's id is the fg; its column (16 bits: a control wider
     * than 255 cells goes on from the left) and row (in the attrs' byte,
     * which a placeholder doesn't draw), kitty_col/row + 1 or 0 for "from
     * the left", are resolved by the frame, left to right */
    out[0] = (uint32_t)c->kitty_col | (uint32_t)c->kitty_row << 16 |
             (flags | CTUI_VK_PLACEHOLDER) << 16;
    out[1] = fg;
    out[2] = bg;
    return;
  }
  int wide;
  uint32_t slot = glyph_slot(vk, c->ch, &wide);
  if (wide) {
    flags |= CTUI_VK_WIDE;
  }
  out[0] = slot | (c->attr | flags) << 16;
  out[1] = fg;
  out[2] = bg;
}

void ctui_vk_cont(uint32_t *w, const uint32_t *left) {
  uint32_t slot = 0;
  if (left && (left[0] >> 16 & CTUI_VK_WIDE) &&
      !(left[0] >> 16 & CTUI_VK_PLACEHOLDER)) {
    slot = (left[0] & 0xffffu) + 1u;
  }
  w[0] = (w[0] & 0xffff0000u & ~((uint32_t)CTUI_VK_WIDE << 16)) | slot;
}

/* a placeholder's row and column as kitty reads them: given, else taken
 * on from the placeholder to its left (left: NULL, or its record) of the
 * same image -- the same row, the next column */
static void placeholder_at(uint32_t *w, const uint32_t *left) {
  unsigned row = w[0] >> 16 & 0xffu, col = w[0] & 0xffffu;
  int same = left && (left[0] >> 16 & CTUI_VK_PLACEHOLDER) && left[1] == w[1];
  unsigned lrow = left ? left[0] >> 16 & 0xffu : 0, lcol = left ? left[0] & 0xffffu : 0;
  row = row ? row - 1 : same ? lrow : 0;
  col = col ? col - 1 : same && lrow == row ? lcol + 1 : 0;
  w[0] = (w[0] & 0xff000000u) | (col & 0xffffu) | (row & 0xffu) << 16;
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
    /* a placeholder resolved again: the one right of it may follow it;
     * a cell changed: a CONT right of it takes its glyph's right half */
    int chain = 0, lead = 0;
    for (unsigned c = 0; c < cols; c++) {
      size_t si = (size_t)r * (unsigned)s->cols + c;
      const CTUI_CELL *cell = &s->cells[si];
      int follows = (chain && cell->ch == CTUI_GFX_KITTY_PLACEHOLDER &&
                     (!cell->kitty_col || !cell->kitty_row)) ||
                    (lead && cell->ch == CTUI_CELL_CONT);
      chain = 0;
      lead = 0;
      if (!follows && !memcmp(cell, &s->buffer[si], sizeof *cell)) {
        continue;
      }
      size_t k = (size_t)r * vk->cols + c;
      uint32_t w[CTUI_VK_CELL_WORDS];
      ctui_vk_cell(vk, cell, w);
      if (w[0] >> 16 & CTUI_VK_PLACEHOLDER) {
        placeholder_at(w, c ? vk->rec + (k - 1) * CTUI_VK_CELL_WORDS : NULL);
        chain = 1;
      } else if (cell->ch == CTUI_CELL_CONT) {
        ctui_vk_cont(w, c ? vk->rec + (k - 1) * CTUI_VK_CELL_WORDS : NULL);
      }
      lead = 1;
      uint32_t *have = vk->rec + k * CTUI_VK_CELL_WORDS;
      if (!memcmp(w, have, sizeof w)) {
        continue;
      }
      memcpy(have, w, sizeof w);
      vk->dirty[k] = 1;
      unsigned x0 = (vk->px0 + c * vk->gw) / CTUI_VK_TILE,
               x1 = (vk->px0 + (c + 1) * vk->gw - 1) / CTUI_VK_TILE,
               y0 = (vk->py0 + r * vk->gh) / CTUI_VK_TILE,
               y1 = (vk->py0 + (r + 1) * vk->gh - 1) / CTUI_VK_TILE;
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

static uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* a wipe's x at now, held at x_end */
static double wipe_x(const IMG *im, uint64_t now) {
  double t = now > im->t0 ? (double)(now - im->t0) / 1e9 : 0.0;
  double x = im->x0 + im->v * t;
  return im->v >= 0 ? (x < im->x_end ? x : im->x_end)
                    : (x > im->x_end ? x : im->x_end);
}

static int split_at(double x) { return x < 0 ? 0 : (int)x; }

int ctui_vk_pending(const CTUI_VK *vk) {
  return vk->sized && memchr(vk->tdirty, 1, (size_t)vk->tx * vk->ty) != NULL;
}

static IMG *img_find(CTUI_VK *vk, unsigned id) {
  for (unsigned i = 0; i < vk->nimg; i++) {
    if (vk->imgs[i].id == id) {
      return &vk->imgs[i];
    }
  }
  return NULL;
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

/* the tiles under a rect of pixels to paint */
static void mark_rect(CTUI_VK *vk, long x, long y, long w, long h) {
  if (!vk->sized || w <= 0 || h <= 0) {
    return;
  }
  long x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  long x1 = x + w > (long)vk->w ? (long)vk->w : x + w;
  long y1 = y + h > (long)vk->h ? (long)vk->h : y + h;
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  unsigned tx0 = (unsigned)x0 / CTUI_VK_TILE, tx1 = (unsigned)(x1 - 1) / CTUI_VK_TILE;
  unsigned ty0 = (unsigned)y0 / CTUI_VK_TILE, ty1 = (unsigned)(y1 - 1) / CTUI_VK_TILE;
  for (unsigned ty = ty0; ty <= ty1; ty++) {
    memset(vk->tdirty + (size_t)ty * vk->tx + tx0, 1, tx1 - tx0 + 1);
  }
}

/* where a placement shows, in the surface's pixels */
static void plc_rect(const CTUI_VK *vk, const PLC *p, long r[4]) {
  r[0] = (long)vk->px0 + (long)p->col * vk->gw;
  r[1] = (long)vk->py0 + (long)p->row * vk->gh;
  r[2] = (long)p->cols * vk->gw;
  r[3] = (long)p->rows * vk->gh;
  if (p->pw > 0) {
    r[0] += p->px;
    r[1] += p->py;
    r[2] = p->pw;
    r[3] = p->ph;
  }
}

static void mark_plc(CTUI_VK *vk, const PLC *p) {
  long r[4];
  plc_rect(vk, p, r);
  mark_rect(vk, r[0], r[1], r[2], r[3]);
}

/* everywhere image id shows: its placements, the placeholder cells */
static void mark_id(CTUI_VK *vk, unsigned id) {
  for (unsigned i = 0; i < vk->nplc; i++) {
    if (vk->plcs[i].id == id) {
      mark_plc(vk, &vk->plcs[i]);
    }
  }
  if (!vk->sized) {
    return;
  }
  size_t ncells = (size_t)vk->cols * vk->rows;
  for (size_t k = 0; k < ncells; k++) {
    const uint32_t *w = vk->rec + k * CTUI_VK_CELL_WORDS;
    if ((w[0] >> 16 & CTUI_VK_PLACEHOLDER) && w[1] == id) {
      mark_rect(vk, (long)vk->px0 + (long)(k % vk->cols) * vk->gw,
                (long)vk->py0 + (long)(k / vk->cols) * vk->gh, vk->gw, vk->gh);
    }
  }
}

/* the staging buffer of slot i holding at least bytes: made again when
 * not (the recording that used it last is done, by CTUI_VK_SLOTS) */
static int stage_room(CTUI_VK *vk, unsigned i, size_t bytes) {
  if (vk->stage_bytes[i] >= bytes) {
    return 0;
  }
  size_t want = bytes + bytes / 2;
  buf_free(vk, &vk->stage[i]);
  if (buf_new(vk, &vk->stage[i], want, 1)) {
    vk->stage_bytes[i] = 0;
    return -1;
  }
  vk->stage_bytes[i] = want;
  return 0;
}

static int by_z(const void *a, const void *b) {
  const PLC *x = a, *y = b;
  if (x->z != y->z) {
    return x->z < y->z ? -1 : 1;
  }
  if (x->id != y->id) {
    return x->id < y->id ? -1 : 1;
  }
  return x->pid < y->pid ? -1 : x->pid > y->pid;
}

/* offsets in the arena for the images going up; 0, or -1 when it can't
 * be made big enough */
static int arena_place(CTUI_VK *vk) {
  for (int pass = 0; pass < 2; pass++) {
    int full = 0;
    for (unsigned i = 0; i < vk->nimg && !full; i++) {
      IMG *im = &vk->imgs[i];
      size_t words = (size_t)im->w * (size_t)im->h;
      if (!im->up || words <= im->room) {
        continue;
      }
      if (vk->arena_used + words > vk->arena_words) {
        full = 1;
        break;
      }
      im->off = (uint32_t)vk->arena_used;
      im->room = (uint32_t)words;
      vk->arena_used += words;
      vk->places_up = 1;
    }
    if (!full) {
      return 0;
    }
    /* compacted into a new one, the device idle: the old may be in use */
    vkDeviceWaitIdle(vk->dev);
    if (arena_new(vk, live_words(vk))) {
      return -1;
    }
    describe(vk);
    ctui_logf(E_INF, "[CTUI:VK] - image arena now %zu words @ tick %d\n",
              vk->arena_words, ctui_tick_advance());
  }
  return -1;
}

/* what changed up, then painted: into the surface buffer (img < 0), or
 * into image img directly -- this frame's tiles, the ones it's behind
 * on, every one when fresh */
static unsigned record(CTUI_VK *vk, VkCommandBuffer cmd, int img, int fresh) {
  if (!vk->sized) {
    return 0;
  }
  unsigned nup = 0, nplace = vk->nplc, ntab = vk->nimg;
  size_t img_words = 0;
  unsigned most = nplace > ntab ? nplace : ntab;
  if (most > vk->places_cap) {
    vkDeviceWaitIdle(vk->dev);
    if (places_new(vk, most)) {
      ctui_logf(E_ERR, "[CTUI:VK] - no memory for %u placements\n", most);
      return 0;
    }
    describe(vk);
  }
  if (arena_place(vk)) {
    ctui_logf(E_ERR, "[CTUI:VK] - no memory for the images\n");
    return 0;
  }
  if (vk->slots_cap > vk->atlas_cap) {
    /* made again bigger, the device idle (the old may be in use), every
     * slot up again */
    vkDeviceWaitIdle(vk->dev);
    buf_free(vk, &vk->atlas);
    if (buf_new(vk, &vk->atlas, (size_t)vk->slots_cap * vk->slot_words * 4u, 0)) {
      ctui_logf(E_ERR, "[CTUI:VK] - no memory for %u glyphs\n", vk->slots_cap);
      return 0;
    }
    vk->atlas_cap = vk->slots_cap;
    vk->up_from = 0;
    describe(vk);
    ctui_logf(E_INF, "[CTUI:VK] - glyph atlas now %u slots @ tick %d\n", vk->atlas_cap,
              ctui_tick_advance());
  }
  size_t glyph_words = (size_t)(vk->nslots - vk->up_from) * vk->slot_words;
  for (unsigned i = 0; i < vk->nimg; i++) {
    if (vk->imgs[i].up && vk->imgs[i].room) {
      nup++;
      img_words += (size_t)vk->imgs[i].w * (size_t)vk->imgs[i].h;
    }
  }
  if (!ctui_vk_pending(vk) && !nup && !vk->places_up && !glyph_words &&
      img < 0) {
    return 0;
  }
  size_t ncells = (size_t)vk->cols * vk->rows;
  const size_t cw = CTUI_VK_CELL_WORDS;
  unsigned next = (vk->slot + 1) % CTUI_VK_SLOTS;
  size_t need = ((size_t)vk->tx * vk->ty + glyph_words + ncells * cw +
                 (size_t)nplace * PLACE_WORDS + (size_t)ntab * TAB_WORDS +
                 img_words) * 4u;
  if (stage_room(vk, next, need)) {
    ctui_logf(E_ERR, "[CTUI:VK] - no memory to stage %zu bytes\n", need);
    return 0;
  }
  if (nup + 1 > vk->icopies_cap) {
    VkBufferCopy *c = realloc(vk->icopies, (nup + 1) * 2 * sizeof *c);
    if (!c) {
      return 0;
    }
    vk->icopies = c;
    vk->icopies_cap = (nup + 1) * 2;
  }
  vk->slot = next;
  uint32_t *st = vk->stage[vk->slot].map;
  unsigned nt = 0;
  for (unsigned y = 0; y < vk->ty; y++) {
    int first = -1, last = -1;
    /* direct: the span img is behind on, painted with this frame's */
    int *own = img >= 0 ? vk->behind + ((unsigned)img * vk->ty + y) * 2 : NULL;
    int bf = own ? (fresh ? 0 : own[0]) : 1, bl = own ? (fresh ? (int)vk->tx - 1 : own[1]) : 0;
    for (unsigned x = 0; x < vk->tx; x++) {
      int now = vk->tdirty[y * vk->tx + x];
      if (now || ((int)x >= bf && (int)x <= bl)) {
        st[nt++] = x | y << 16;
      }
      if (now) {
        first = first < 0 ? (int)x : first;
        last = (int)x;
      }
    }
    if (own) {
      own[0] = 1, own[1] = 0;
    }
    for (unsigned k = 0; first >= 0 && k < CTUI_VK_IMAGES; k++) {
      if ((int)k == img) {
        continue;
      }
      int *b = vk->behind + (k * vk->ty + y) * 2;
      if (b[0] > b[1]) {
        b[0] = first, b[1] = last;
      } else {
        b[0] = first < b[0] ? first : b[0];
        b[1] = last > b[1] ? last : b[1];
      }
    }
  }
  memset(vk->tdirty, 0, (size_t)vk->tx * vk->ty);
  size_t off = (size_t)vk->tx * vk->ty; /* in words */
  VkBufferCopy atlas = {0};
  if (glyph_words) {
    atlas = (VkBufferCopy){off * 4u, (size_t)vk->up_from * vk->slot_words * 4u,
                           glyph_words * 4u};
    memcpy(st + off, vk->glyphs + (size_t)vk->up_from * vk->slot_words, atlas.size);
    off += glyph_words;
  }
  unsigned nc = 0;
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
  VkBufferCopy places = {0}, tab = {0};
  unsigned shown = 0; /* placements whose image is on the card */
  if (vk->places_up) {
    /* none yet: plcs is NULL, which qsort mustn't get even for 0 */
    if (nplace) {
      qsort(vk->plcs, nplace, sizeof *vk->plcs, by_z);
    }
    places.srcOffset = off * 4u;
    for (unsigned k = 0; k < nplace; k++) {
      const PLC *pl = &vk->plcs[k];
      const IMG *im = img_find(vk, pl->id);
      if (!im || !im->room) {
        continue;
      }
      uint32_t *p = st + off;
      long r[4];
      plc_rect(vk, pl, r);
      for (int q = 0; q < 4; q++) {
        p[q] = (uint32_t)r[q];
      }
      p[4] = im->off;
      p[5] = (uint32_t)im->w;
      p[6] = (uint32_t)im->h;
      p[7] = (uint32_t)pl->z;
      off += PLACE_WORDS;
      shown++;
    }
    places.size = (size_t)shown * PLACE_WORDS * 4u;
    tab.srcOffset = off * 4u;
    for (unsigned k = 0; k < ntab; k++, off += TAB_WORDS) {
      const IMG *im = &vk->imgs[k];
      uint32_t *t = st + off;
      t[0] = im->id;
      t[1] = im->off;
      t[2] = im->room ? (uint32_t)im->w : 0;
      t[3] = (uint32_t)im->h;
      t[4] = (uint32_t)im->vcols;
      t[5] = (uint32_t)im->vrows;
      const IMG *lf = im->left ? img_find(vk, im->left) : NULL;
      const IMG *mk = im->marker ? img_find(vk, im->marker) : NULL;
      int wipe = lf && lf->room && lf->w == im->w && lf->h == im->h;
      t[6] = wipe ? (uint32_t)im->split : NO_WIPE;
      t[7] = wipe ? lf->off : 0;
      t[8] = mk && mk->room ? mk->off : 0;
      t[9] = mk && mk->room ? (uint32_t)mk->w : 0;
      t[10] = mk && mk->room ? (uint32_t)mk->h : 0;
      t[11] = 0;
    }
    tab.size = (size_t)ntab * TAB_WORDS * 4u;
    vk->shown = shown;
    vk->tabbed = ntab;
  }
  vk->places_up = 0;
  unsigned ni = 0;
  for (unsigned i = 0; i < vk->nimg; i++) {
    IMG *im = &vk->imgs[i];
    if (!im->up || !im->room) {
      continue;
    }
    size_t words = (size_t)im->w * (size_t)im->h;
    memcpy(st + off, im->px, words * 4u);
    vk->icopies[ni++] = (VkBufferCopy){off * 4u, (size_t)im->off * 4u,
                                       words * 4u};
    off += words;
    im->up = 0;
  }

  VkBuffer stage = vk->stage[vk->slot].buf;
  /* the last record's dispatch and the caller's copies out of the
   * surface done before anything here writes */
  barrier(cmd,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
  if (nt) {
    VkBufferCopy tiles = {0, 0, nt * 4u};
    vkCmdCopyBuffer(cmd, stage, vk->tiles.buf, 1, &tiles);
  }
  if (glyph_words) {
    vkCmdCopyBuffer(cmd, stage, vk->atlas.buf, 1, &atlas);
    vk->up_from = vk->nslots;
  }
  if (nc) {
    vkCmdCopyBuffer(cmd, stage, vk->cells.buf, nc, vk->copies);
  }
  if (places.size) {
    vkCmdCopyBuffer(cmd, stage, vk->places.buf, 1, &places);
  }
  if (tab.size) {
    vkCmdCopyBuffer(cmd, stage, vk->tab.buf, 1, &tab);
  }
  if (ni) {
    vkCmdCopyBuffer(cmd, stage, vk->arena.buf, ni, vk->icopies);
  }
  barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  if (nt) {
    PUSH p = {vk->w,      vk->h,      vk->gw,
              vk->gh,     vk->cols,   vk->rows,
              (uint32_t)vk->bgra, vk->def_fg, vk->def_bg,
              vk->shown,  vk->tabbed, vk->px0,
              vk->py0};
    VkPipelineLayout lay = img >= 0 ? vk->dlayout : vk->layout;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      img >= 0 ? vk->dpipe : vk->pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1,
                            img >= 0 ? &vk->dset[img] : &vk->set, 0, NULL);
    vkCmdPushConstants(cmd, lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cmd, nt, 1, 1);
  }
  barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  ctui_logf(E_DBG,
            "[CTUI:VK] - recorded %u tiles, %u cell runs, %u images up, %u "
            "placed @ tick %d\n",
            nt, nc, ni, nplace, ctui_tick_advance());
  return nt;
}

unsigned ctui_vk_record(CTUI_VK *vk, VkCommandBuffer cmd) {
  return vk->direct ? 0 : record(vk, cmd, -1, 0);
}

unsigned ctui_vk_record_image(CTUI_VK *vk, VkCommandBuffer cmd, unsigned img,
                              int fresh) {
  if (!vk->direct || img >= CTUI_VK_IMAGES) {
    return 0;
  }
  return record(vk, cmd, (int)img, fresh);
}

/* --- the graphics sink: kitty's image calls as images on the card --- */

/* id's image at w x h texels, its host copy ready to write; NULL when
 * out of memory */
static IMG *img_get(CTUI_VK *vk, unsigned id, int w, int h) {
  IMG *im = img_find(vk, id);
  if (!im) {
    if (vk->nimg == vk->img_cap) {
      unsigned cap = vk->img_cap ? vk->img_cap * 2 : 8;
      IMG *n = realloc(vk->imgs, cap * sizeof *n);
      if (!n) {
        return NULL;
      }
      vk->imgs = n;
      vk->img_cap = cap;
    }
    im = &vk->imgs[vk->nimg++];
    memset(im, 0, sizeof *im);
    im->id = id;
    vk->places_up = 1;
  }
  size_t words = (size_t)w * (size_t)h;
  if (words > im->cap) {
    uint32_t *px = realloc(im->px, words * 4u);
    if (!px) {
      return NULL;
    }
    im->px = px;
    im->cap = words;
  }
  if (im->w != w || im->h != h) {
    vk->places_up = 1;
  }
  im->w = w;
  im->h = h;
  im->up = 1;
  return im;
}

/* placement (id, pid) at kitty's 1-based (row, col), cols x rows cells,
 * z: its old place and the new one painted again */
static void plc_set(CTUI_VK *vk, unsigned id, unsigned pid, int row, int col,
                    int cols, int rows, int z, const int px[4]) {
  PLC *p = NULL;
  for (unsigned i = 0; i < vk->nplc && !p; i++) {
    if (vk->plcs[i].id == id && vk->plcs[i].pid == pid) {
      p = &vk->plcs[i];
    }
  }
  if (!p) {
    if (vk->nplc == vk->plc_cap) {
      unsigned cap = vk->plc_cap ? vk->plc_cap * 2 : 8;
      PLC *n = realloc(vk->plcs, cap * sizeof *n);
      if (!n) {
        ctui_logf(E_ERR, "[CTUI:VK] - no memory for a placement\n");
        return;
      }
      vk->plcs = n;
      vk->plc_cap = cap;
    }
    p = &vk->plcs[vk->nplc++];
  } else {
    mark_plc(vk, p);
  }
  *p = (PLC){id, pid, row - 1, col - 1, cols, rows, z, 0, 0, 0, 0};
  if (px) {
    p->px = px[0];
    p->py = px[1];
    p->pw = px[2];
    p->ph = px[3];
  }
  mark_plc(vk, p);
  vk->places_up = 1;
}

static void display_at(CTUI_VK *vk, int row, int col, int cell_cols,
                       int cell_rows, const int px[4],
                       const unsigned char *rgba, int width, int height,
                       unsigned int image_id, int z) {
  if (width <= 0 || height <= 0 || !rgba || cell_cols <= 0 || cell_rows <= 0) {
    ctui_logf(E_WRN, "[CTUI:VK] - display of image %u rejected (%dx%d)\n",
              image_id, width, height);
    return;
  }
  IMG *im = img_get(vk, image_id, width, height);
  if (!im) {
    ctui_logf(E_ERR, "[CTUI:VK] - no memory for image %u\n", image_id);
    return;
  }
  memcpy(im->px, rgba, (size_t)width * (size_t)height * 4u);
  mark_id(vk, image_id);
  plc_set(vk, image_id, 0, row, col, cell_cols, cell_rows, z, px);
}

static void s_display(void *ctx, int row, int col, int cell_cols,
                      int cell_rows, const unsigned char *rgba, int width,
                      int height, unsigned int image_id, int z) {
  display_at(ctx, row, col, cell_cols, cell_rows, NULL, rgba, width, height,
             image_id, z);
}

static void s_display_px(void *ctx, int row, int col, int cell_cols,
                         int cell_rows, int x, int y, int w, int h,
                         const unsigned char *rgba, int width, int height,
                         unsigned int image_id, int z) {
  if (w <= 0 || h <= 0) {
    ctui_logf(E_WRN, "[CTUI:VK] - display_px of image %u rejected (%dx%d px)\n",
              image_id, w, h);
    return;
  }
  const int px[4] = {x, y, w, h};
  display_at(ctx, row, col, cell_cols, cell_rows, px, rgba, width, height,
             image_id, z);
}

static unsigned char *s_begin(void *ctx, unsigned int image_id, int width,
                              int height, int keep) {
  /* keep: the host copy holds the last frame already */
  (void)keep;
  IMG *im = width > 0 && height > 0
                ? img_get(ctx, image_id, width, height)
                : NULL;
  return im ? (unsigned char *)im->px : NULL;
}

static void s_commit(void *ctx, unsigned int image_id, int row, int col,
                     int cell_cols, int cell_rows, int z) {
  CTUI_VK *vk = ctx;
  if (img_find(vk, image_id)) {
    mark_id(vk, image_id);
    plc_set(vk, image_id, 0, row, col, cell_cols, cell_rows, z, NULL);
  }
}

/* kitty keeps an image a freed CTUI_GFX_KITTY_IMAGE showed: so do we */
static void s_free(void *ctx, unsigned int image_id) {
  (void)ctx, (void)image_id;
}

static void s_wipe(void *ctx, unsigned int image_id, unsigned int left_id,
                   unsigned int marker_id, double x, double px_per_s,
                   double x_end) {
  CTUI_VK *vk = ctx;
  IMG *im = img_find(vk, image_id);
  if (!im) {
    ctui_logf(E_WRN, "[CTUI:VK] - wipe of image %u: no such image\n", image_id);
    return;
  }
  uint64_t now = mono_ns();
  if (left_id && im->left == left_id && im->marker == marker_id &&
      im->v == px_per_s && im->x_end == x_end) {
    double at = wipe_x(im, now) - x;
    if (at > -1.0 && at < 1.0) {
      return; /* where it is already: no step back */
    }
  }
  im->left = left_id;
  im->marker = marker_id;
  im->x0 = x;
  im->v = left_id ? px_per_s : 0.0;
  im->x_end = x_end;
  im->t0 = now;
  im->split = split_at(x);
  mark_id(vk, image_id);
  vk->places_up = 1;
}

int ctui_vk_at(CTUI_VK *vk, uint64_t now_ns, uint64_t *next_ns) {
  uint64_t next = 0;
  int moving = 0;
  for (unsigned i = 0; i < vk->nimg; i++) {
    IMG *im = &vk->imgs[i];
    if (!im->left) {
      continue;
    }
    double x = wipe_x(im, now_ns);
    int split = split_at(x);
    if (split != im->split) {
      im->split = split;
      mark_id(vk, im->id);
      vk->places_up = 1;
    }
    if (im->v == 0.0 || x == im->x_end) {
      continue;
    }
    moving = 1;
    /* when the split next moves a column (or x reaches its end) */
    double to = im->v > 0 ? (double)split + 1.0 : (double)split;
    if (im->v > 0 ? to > im->x_end : to < im->x_end) {
      to = im->x_end;
    }
    double dt = (to - x) / im->v;
    uint64_t in = dt > 0 ? (uint64_t)(dt * 1e9) + 1u : 1u;
    if (!next || in < next) {
      next = in;
    }
  }
  if (next_ns) {
    *next_ns = next;
  }
  return moving;
}

static void s_delete(void *ctx, unsigned int image_id) {
  CTUI_VK *vk = ctx;
  mark_id(vk, image_id);
  for (unsigned i = 0; i < vk->nplc;) {
    if (vk->plcs[i].id == image_id) {
      vk->plcs[i] = vk->plcs[--vk->nplc];
      vk->places_up = 1;
    } else {
      i++;
    }
  }
  IMG *im = img_find(vk, image_id);
  if (im) {
    free(im->px);
    *im = vk->imgs[--vk->nimg];
    vk->places_up = 1;
  }
}

/* w x h RGBA as image_id, shown by its placeholder cells, fit into cols
 * x rows of them (kitty's virtual placement): replacing the image keeps
 * its placements */
static void s_place_rgba(void *ctx, unsigned int image_id,
                         const unsigned char *rgba, int w, int h, int cols,
                         int rows) {
  CTUI_VK *vk = ctx;
  IMG *im = w > 0 && h > 0 && cols > 0 && rows > 0 && rgba
                ? img_get(vk, image_id, w, h)
                : NULL;
  if (!im) {
    ctui_logf(E_WRN, "[CTUI:VK] - image %u not placed (%dx%d)\n", image_id, w,
              h);
    return;
  }
  memcpy(im->px, rgba, (size_t)w * (size_t)h * 4u);
  im->vcols = cols;
  im->vrows = rows;
  vk->places_up = 1;
  mark_id(vk, image_id);
}

/* the same from the PNG at path */
static void s_place_file(void *ctx, unsigned int image_id, const char *path,
                         int cols, int rows) {
  unsigned char *rgba = NULL;
  int w = 0, h = 0;
  if (ctui_look_png_load(path, &rgba, &w, &h)) {
    ctui_logf(E_WRN, "[CTUI:VK] - place_file %u: %s not placed\n", image_id,
              path);
    return;
  }
  s_place_rgba(ctx, image_id, rgba, w, h, cols, rows);
  free(rgba);
}

static void s_put(void *ctx, unsigned int image_id, unsigned int placement_id,
                  int row, int col, int cols, int rows, int z) {
  if (cols <= 0 || rows <= 0) {
    ctui_logf(E_WRN, "[CTUI:VK] - put of image %u rejected (%dx%d)\n",
              image_id, cols, rows);
    return;
  }
  plc_set(ctx, image_id, placement_id, row, col, cols, rows, z, NULL);
}

static void s_unput(void *ctx, unsigned int image_id,
                    unsigned int placement_id) {
  CTUI_VK *vk = ctx;
  for (unsigned i = 0; i < vk->nplc; i++) {
    if (vk->plcs[i].id == image_id && vk->plcs[i].pid == placement_id) {
      mark_plc(vk, &vk->plcs[i]);
      vk->plcs[i] = vk->plcs[--vk->nplc];
      vk->places_up = 1;
      return;
    }
  }
}

static void s_flush(void *ctx) { (void)ctx; }

const CTUI_GFX_SINK *ctui_vk_gfx_sink(void) {
  static const CTUI_GFX_SINK sink = {
      s_display,    s_begin,      s_commit,     s_free,
      s_delete,     s_place_file, s_place_rgba, s_display_px,
      s_wipe,       s_put,        s_unput,      s_flush};
  return &sink;
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
