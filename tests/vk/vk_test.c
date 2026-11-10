/* `make test` (where Vulkan's headers and glslc are): the backend's text
 * painter on the card (src/vk/vk.h), a ctui app's frame through its
 * screen sink, against a reference painted here by cells.comp's rule
 * from the cells as they went up, then the surface's hash pinned. And
 * its dirty tracking: unchanged is nothing recorded, a changed cell its
 * tile, the copy regions a swapchain image is behind on. Skipped, not
 * failed, without a device (or without one with a compute queue) */
#define _POSIX_C_SOURCE 200809L

#include "ctui.h"
#include "ctui_test.h"
#include "look/png.h"
#include "vk/ttf.h"
#include "vk/vk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 232u
#define H 128u
/* the cell, as the font gives it at scale 1 (9 x 20: 25 x 6 cells) */
static unsigned GW, GH;
#define VALIDATION "VK_LAYER_KHRONOS_validation"

static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static VkQueue g_queue;
static VkCommandPool g_pool;
static VkCommandBuffer g_cmd;
static VkFence g_fence;
static VkDebugUtilsMessengerEXT g_msgr;
static unsigned g_warnings;
static int g_validation;

static VKAPI_ATTR VkBool32 VKAPI_CALL
on_message(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
           VkDebugUtilsMessageTypeFlagsEXT type,
           const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
  (void)type, (void)user;
  if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    g_warnings++;
    fprintf(stderr, "vulkan: %s\n", data->pMessage);
  }
  return VK_FALSE;
}

static int has_layer(const char *name) {
  uint32_t n = 0;
  vkEnumerateInstanceLayerProperties(&n, NULL);
  VkLayerProperties props[64];
  n = n > 64 ? 64 : n;
  vkEnumerateInstanceLayerProperties(&n, props);
  for (uint32_t i = 0; i < n; i++) {
    if (!strcmp(props[i].layerName, name)) {
      return 1;
    }
  }
  return 0;
}

/* a device with a compute queue, the validation layer when installed;
 * NULL, or why there's none */
static const char *open_device(void) {
  g_validation = has_layer(VALIDATION);
  const char *layers[] = {VALIDATION};
  const char *exts[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
  VkDebugUtilsMessengerCreateInfoEXT dbg = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
      .pfnUserCallback = on_message,
  };
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "ctui vk_test",
                           .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = g_validation ? &dbg : NULL,
      .pApplicationInfo = &app,
      .enabledLayerCount = g_validation ? 1u : 0u,
      .ppEnabledLayerNames = layers,
      .enabledExtensionCount = g_validation ? 1u : 0u,
      .ppEnabledExtensionNames = exts,
  };
  if (vkCreateInstance(&ici, NULL, &g_inst) != VK_SUCCESS) {
    return "no Vulkan instance";
  }
  if (g_validation) {
    PFN_vkCreateDebugUtilsMessengerEXT mk =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            g_inst, "vkCreateDebugUtilsMessengerEXT");
    if (mk) {
      mk(g_inst, &dbg, NULL, &g_msgr);
    }
  }
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(g_inst, &n, NULL);
  VkPhysicalDevice devs[16];
  n = n > 16 ? 16 : n;
  vkEnumeratePhysicalDevices(g_inst, &n, devs);
  uint32_t family = 0;
  for (uint32_t i = 0; i < n && !g_phys; i++) {
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, NULL);
    VkQueueFamilyProperties q[16];
    nq = nq > 16 ? 16 : nq;
    vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, q);
    for (uint32_t f = 0; f < nq; f++) {
      if (q[f].queueFlags & VK_QUEUE_COMPUTE_BIT) {
        g_phys = devs[i];
        family = f;
        break;
      }
    }
  }
  if (!g_phys) {
    return "no device with a compute queue";
  }
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family,
      .queueCount = 1,
      .pQueuePriorities = &prio,
  };
  VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
  };
  if (vkCreateDevice(g_phys, &dci, NULL, &g_dev) != VK_SUCCESS) {
    return "no logical device";
  }
  vkGetDeviceQueue(g_dev, family, 0, &g_queue);
  VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = family,
  };
  VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateCommandPool(g_dev, &pci, NULL, &g_pool) != VK_SUCCESS ||
      (cai.commandPool = g_pool,
       vkAllocateCommandBuffers(g_dev, &cai, &g_cmd) != VK_SUCCESS) ||
      vkCreateFence(g_dev, &fci, NULL, &g_fence) != VK_SUCCESS) {
    return "no command buffer";
  }
  return NULL;
}

static void close_device(void) {
  if (g_dev) {
    vkDeviceWaitIdle(g_dev);
    if (g_fence) {
      vkDestroyFence(g_dev, g_fence, NULL);
    }
    if (g_pool) {
      vkDestroyCommandPool(g_dev, g_pool, NULL);
    }
    vkDestroyDevice(g_dev, NULL);
  }
  if (g_msgr) {
    PFN_vkDestroyDebugUtilsMessengerEXT rm =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            g_inst, "vkDestroyDebugUtilsMessengerEXT");
    if (rm) {
      rm(g_inst, g_msgr, NULL);
    }
  }
  if (g_inst) {
    vkDestroyInstance(g_inst, NULL);
  }
}

/* a host-visible buffer the surface is copied back into */
static VkBuffer g_back;
static VkDeviceMemory g_back_mem;
static uint32_t *g_px;

static int back_new(void) {
  VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = W * H * 4u,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
  };
  if (vkCreateBuffer(g_dev, &bci, NULL, &g_back) != VK_SUCCESS) {
    return -1;
  }
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(g_dev, g_back, &req);
  VkPhysicalDeviceMemoryProperties mem;
  vkGetPhysicalDeviceMemoryProperties(g_phys, &mem);
  VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
    if ((req.memoryTypeBits & (1u << i)) &&
        (mem.memoryTypes[i].propertyFlags & want) == want) {
      VkMemoryAllocateInfo mai = {
          .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
          .allocationSize = req.size,
          .memoryTypeIndex = i,
      };
      void *map;
      if (vkAllocateMemory(g_dev, &mai, NULL, &g_back_mem) != VK_SUCCESS ||
          vkBindBufferMemory(g_dev, g_back, g_back_mem, 0) != VK_SUCCESS ||
          vkMapMemory(g_dev, g_back_mem, 0, VK_WHOLE_SIZE, 0, &map) !=
              VK_SUCCESS) {
        return -1;
      }
      g_px = map;
      return 0;
    }
  }
  return -1;
}

static void back_free(void) {
  if (g_back) {
    vkDestroyBuffer(g_dev, g_back, NULL);
  }
  if (g_back_mem) {
    vkFreeMemory(g_dev, g_back_mem, NULL);
  }
}

/* a recording: the backend's record, the surface copied back; the tiles */
static unsigned paint(CTUI_VK *vk) {
  VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  vkResetCommandBuffer(g_cmd, 0);
  vkBeginCommandBuffer(g_cmd, &bi);
  unsigned n = ctui_vk_record(vk, g_cmd);
  VkBufferCopy c = {0, 0, W * H * 4u};
  vkCmdCopyBuffer(g_cmd, ctui_vk_surface(vk), g_back, 1, &c);
  VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0,
                       NULL);
  vkEndCommandBuffer(g_cmd);
  VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                     .commandBufferCount = 1,
                     .pCommandBuffers = &g_cmd};
  vkQueueSubmit(g_queue, 1, &si, g_fence);
  vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX);
  vkResetFences(g_dev, 1, &g_fence);
  return n;
}

static uint32_t rgba(uint32_t rgb) {
  return (rgb >> 16 & 0xffu) | (rgb & 0xff00u) | (rgb & 0xffu) << 16 |
         0xff000000u;
}

/* the images the reference composites, as placed (cells, 0-based), in
 * z order */
typedef struct {
  int row, col, cols, rows, z;
  const uint8_t *rgba;
  int w, h;
  int px, py, pw, ph; /* display_px()'s rect; pw 0: the cells */
} PL;
static PL g_pl[4];
static unsigned g_npl;

/* the placed file placeholder cells show (place_file()): its id, texels,
 * virtual cells */
static unsigned g_ph_id;
static const uint8_t *g_ph_rgba;
static int g_ph_w, g_ph_h, g_ph_cols, g_ph_rows;
/* its wipe (ctui_gfx_wipe()): the split (-1: none), the left image's
 * texels, the marker's */
static int g_wipe_split = -1;
static const uint8_t *g_wipe_left, *g_wipe_mark;
static int g_wipe_mw, g_wipe_mh;

/* src/vk/vk.c's placeholder_at(): the row and column from the left */
static void ph_at(uint32_t *w, const uint32_t *left) {
  unsigned row = w[0] >> 16 & 0xffu, col = w[0] & 0xffffu;
  int same = left && (left[0] >> 16 & CTUI_VK_PLACEHOLDER) && left[1] == w[1];
  unsigned lrow = left ? left[0] >> 16 & 0xffu : 0, lcol = left ? left[0] & 0xffffu : 0;
  row = row ? row - 1 : same ? lrow : 0;
  col = col ? col - 1 : same && lrow == row ? lcol + 1 : 0;
  w[0] = (w[0] & 0xff000000u) | col | row << 16;
}

/* cells.comp's texel(): rgb << 8 | a, the footprint averaged */
static uint32_t texel(const PL *p, unsigned gw, unsigned gh, unsigned x,
                      unsigned y) {
  unsigned px0 = (unsigned)p->col * gw, py0 = (unsigned)p->row * gh;
  unsigned pw = (unsigned)p->cols * gw, ph = (unsigned)p->rows * gh;
  if (p->pw > 0) {
    px0 += (unsigned)p->px;
    py0 += (unsigned)p->py;
    pw = (unsigned)p->pw;
    ph = (unsigned)p->ph;
  }
  if (x < px0 || y < py0 || x >= px0 + pw || y >= py0 + ph) {
    return 0;
  }
  unsigned iw = (unsigned)p->w, ih = (unsigned)p->h, dx = x - px0, dy = y - py0;
  unsigned sx0 = dx * iw / pw, sy0 = dy * ih / ph;
  unsigned sx1 = (dx + 1) * iw / pw, sy1 = (dy + 1) * ih / ph;
  sx1 = sx1 > sx0 + 1 ? sx1 : sx0 + 1;
  sy1 = sy1 > sy0 + 1 ? sy1 : sy0 + 1;
  unsigned stx = (sx1 - sx0 + 15) / 16, sty = (sy1 - sy0 + 15) / 16;
  stx = stx ? stx : 1;
  sty = sty ? sty : 1;
  unsigned c[4] = {0}, n = 0;
  for (unsigned sy = sy0; sy < sy1; sy += sty) {
    for (unsigned sx = sx0; sx < sx1; sx += stx) {
      for (int k = 0; k < 4; k++) {
        c[k] += p->rgba[(sy * iw + sx) * 4 + (unsigned)k];
      }
      n++;
    }
  }
  return (c[0] / n) << 24 | (c[1] / n) << 16 | (c[2] / n) << 8 | c[3] / n;
}

static uint32_t over(uint32_t dst, uint32_t t) {
  uint32_t a = t & 0xffu, src = t >> 8, o = 0;
  if (a == 0) {
    return dst;
  }
  if (a == 255) {
    return src;
  }
  for (unsigned sh = 0; sh < 24; sh += 8) {
    o |= (((src >> sh & 0xffu) * a + (dst >> sh & 0xffu) * (255 - a) + 127) /
          255)
         << sh;
  }
  return o;
}

/* cells.comp's placeholder(): the image fit into its virtual cells */
static uint32_t ph_texel(unsigned id, unsigned gw, unsigned gh, unsigned dx,
                         unsigned dy) {
  if (id != g_ph_id || !g_ph_rgba) {
    return 0;
  }
  unsigned iw = (unsigned)g_ph_w, ih = (unsigned)g_ph_h;
  unsigned pw = (unsigned)g_ph_cols * gw, ph = (unsigned)g_ph_rows * gh;
  unsigned dw = pw, dh = ph;
  if (iw * ph > ih * pw) {
    dh = ih * pw / iw ? ih * pw / iw : 1;
  } else {
    dw = iw * ph / ih ? iw * ph / ih : 1;
  }
  unsigned ox = (pw - dw) / 2, oy = (ph - dh) / 2;
  if (dx < ox || dy < oy || dx >= ox + dw || dy >= oy + dh) {
    return 0;
  }
  /* texel() of a 1 x 1 cell placement whose cell is dw x dh */
  unsigned ex = dx - ox, ey = dy - oy, sx = ex * iw / dw;
  int wipe = g_wipe_split >= 0;
  PL p = {0, 0, 1, 1, 0, wipe && (int)sx < g_wipe_split ? g_wipe_left : g_ph_rgba,
          g_ph_w, g_ph_h, 0, 0, 0, 0};
  uint32_t t = texel(&p, dw, dh, ex, ey);
  int mx = (int)sx - (g_wipe_split - g_wipe_mw / 2);
  if (wipe && g_wipe_mark && mx >= 0 && mx < g_wipe_mw) {
    const uint8_t *m =
        g_wipe_mark + ((ey * ih / dh) * (unsigned)g_wipe_mh / ih * (unsigned)g_wipe_mw + (unsigned)mx) * 4;
    if (m[3]) {
      t = (uint32_t)m[0] << 24 | (uint32_t)m[1] << 16 | (uint32_t)m[2] << 8 | m[3];
    }
  }
  return t;
}

/* cells.comp's coverage() and blend() */
static unsigned coverage(const uint32_t *slots, unsigned sw, unsigned gw, unsigned g,
                         unsigned gx, unsigned gy) {
  unsigned i = gy * gw + gx;
  return slots[g * sw + i / 4] >> (8 * (i % 4)) & 0xffu;
}

static uint32_t blend(uint32_t bg, uint32_t fg, unsigned c) {
  uint32_t o = 0;
  for (unsigned s = 0; s < 24; s += 8) {
    o |= (((fg >> s & 0xffu) * c + (bg >> s & 0xffu) * (255u - c) + 127u) / 255u) << s;
  }
  return o;
}

/* the 16 x 16 tiles under a rect of pixels */
/* VK_TEST_PPM=FILE: the surface as it is, to look at */
static void dump_ppm(const char *what) {
  const char *path = getenv("VK_TEST_PPM");
  FILE *f = path ? fopen(path, "wb") : NULL;
  if (!f) {
    return;
  }
  fprintf(f, "P6\n%u %u\n255\n", W, H);
  for (unsigned i = 0; i < W * H; i++) {
    unsigned char rgb[3] = {(unsigned char)g_px[i], (unsigned char)(g_px[i] >> 8),
                            (unsigned char)(g_px[i] >> 16)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  printf("  (%s written to %s)\n", what, path);
}

static unsigned tiles_under(unsigned x, unsigned y, unsigned w, unsigned h) {
  unsigned x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
  return ((x1 - 1) / 16 - x / 16 + 1) * ((y1 - 1) / 16 - y / 16 + 1);
}

/* cells.comp's rule, on the host, from the screen's cells */
static void reference(CTUI_VK *vk, const CTUI_SCREEN *s, uint32_t *px) {
  unsigned gw, gh, sw;
  const uint32_t *slots;
  ctui_vk_glyphs(vk, &gw, &gh, &slots, &sw);
  uint32_t dfg, dbg;
  ctui_vk_defaults(vk, &dfg, &dbg);
  for (unsigned y = 0; y < H; y++) {
    for (unsigned x = 0; x < W; x++) {
      unsigned cx = x / gw, cy = y / gh, gx = x % gw, gy = y % gh;
      uint32_t rgb = dbg, fg = dfg;
      unsigned cov = 0;
      int bg_default = 1;
      uint32_t ph = 0;
      if (cx < (unsigned)s->cols && cy < (unsigned)s->rows) {
        uint32_t c[CTUI_VK_CELL_WORDS] = {0}, l[CTUI_VK_CELL_WORDS];
        /* the row's placeholders resolved left to right, as the frame */
        for (unsigned k = 0; k <= cx; k++) {
          memcpy(l, c, sizeof l);
          const CTUI_CELL *sc = &s->buffer[cy * (unsigned)s->cols + k];
          ctui_vk_cell(vk, sc, c);
          if (c[0] >> 16 & CTUI_VK_PLACEHOLDER) {
            ph_at(c, k ? l : NULL);
          } else if (sc->ch == CTUI_CELL_CONT) {
            ctui_vk_cont(c, k ? l : NULL);
          }
        }
        uint32_t g = c[0] & 0xffffu, a = c[0] >> 16;
        fg = a & CTUI_VK_FG_DEFAULT ? dfg : c[1];
        uint32_t bg = a & CTUI_VK_BG_DEFAULT ? dbg : c[2];
        bg_default = (a & CTUI_VK_BG_DEFAULT) != 0;
        if (a & CTUI_VK_PLACEHOLDER) {
          ph = ph_texel(c[1], gw, gh, g * gw + gx, (a & 0xffu) * gh + gy);
          a &= CTUI_VK_FG_DEFAULT | CTUI_VK_BG_DEFAULT | CTUI_VK_PLACEHOLDER;
        } else if (g == CTUI_VK_GLYPH_BOX) {
          cov = gx == 0 || gx == gw - 1 || gy == gh / 8 || gy == gh - 1 - gh / 8 ? 255 : 0;
        } else {
          cov = coverage(slots, sw, gw, g, gx, gy);
          if ((a & CTUI_ATTR_BOLD) && gx > 0) {
            unsigned l = coverage(slots, sw, gw, g, gx - 1, gy);
            cov = l > cov ? l : cov;
          }
        }
        if (((a & CTUI_ATTR_UNDERLINE) && gy == gh - 1 - gh / 16) ||
            ((a & CTUI_ATTR_STRIKE) && gy == gh / 2)) {
          cov = 255;
        }
        if (a & CTUI_ATTR_DIM) {
          fg = ((fg & 0xfefefeu) >> 1) + ((bg & 0xfefefeu) >> 1);
        }
        rgb = bg;
      }
      unsigned p = 0;
      for (; p < g_npl && g_pl[p].z < 0; p++) {
        if (g_pl[p].z >= CTUI_GFX_KITTY_Z_UNDER_BG || bg_default) {
          rgb = over(rgb, texel(&g_pl[p], gw, gh, x, y));
        }
      }
      rgb = blend(rgb, fg, cov);
      rgb = over(rgb, ph);
      for (; p < g_npl; p++) {
        rgb = over(rgb, texel(&g_pl[p], gw, gh, x, y));
      }
      px[y * W + x] = rgba(rgb);
    }
  }
}

static unsigned differ(CTUI_VK *vk, const CTUI_SCREEN *s) {
  static uint32_t want[W * H];
  reference(vk, s, want);
  unsigned bad = 0;
  for (unsigned i = 0; i < W * H; i++) {
    bad += g_px[i] != want[i];
  }
  return bad;
}

static uint64_t hash(void) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (unsigned i = 0; i < W * H; i++) {
    h = (h ^ g_px[i]) * 0x100000001b3ull;
  }
  return h;
}

static const char *g_title = "ctui on the card";
/* a placeholder run past column 255: 4 cells at (5, 20), the first one
 * column 254 of the image, the rest from the left */
static int g_ph_wide;
/* a wiped control's cells: 5 x 1 at (5, 18) */
static int g_ph_row5;

static void render_text(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  ctui_widget_puts(self, comp, 0, 0, g_title, CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  CTUI_CELL bold = {.fg = CTUI_COLOR_YELLOW,
                    .bg = CTUI_COLOR_BLUE,
                    .attr = CTUI_ATTR_BOLD};
  ctui_widget_puts_cell(self, comp, 1, 0, "bold", (size_t)-1, &bold);
  ctui_widget_putc(self, comp, 1, 5, ' ', CTUI_COLOR_DEFAULT, CTUI_COLOR_RED);
  CTUI_CELL under = {.color_mode = CTUI_COLOR_MODE_256,
                     .fg = 208,
                     .bg = 236,
                     .attr = CTUI_ATTR_UNDERLINE};
  ctui_widget_puts_cell(self, comp, 2, 0, "under 256", (size_t)-1, &under);
  CTUI_CELL dim = {.color_mode = CTUI_COLOR_MODE_RGB,
                   .fg_r = 0xf0, .fg_g = 0x80, .fg_b = 0x10,
                   .bg_r = 0x10, .bg_g = 0x20, .bg_b = 0x30,
                   .attr = CTUI_ATTR_DIM | CTUI_ATTR_STRIKE};
  ctui_widget_puts_cell(self, comp, 3, 0, "dim rgb", (size_t)-1, &dim);
  /* box drawing (extras), é (Latin-1), ☃ (none: a box), 中 (width 2,
   * none: a box and its CONT blank) */
  ctui_widget_puts(self, comp, 4, 0, "\xe2\x94\x8c\xe2\x94\x80\xc3\xa9"
                                     "\xe2\x98\x83\xe4\xb8\xad|",
                   CTUI_COLOR_GREEN, CTUI_COLOR_DEFAULT);
  ctui_widget_putc_rgb_fg(self, comp, 5, 0, '#', 0x40, 0xc0, 0xff,
                          CTUI_COLOR_MAGENTA);
  if (g_ph_id && g_ph_row5) {
    ctui_widget_put_kitty_placeholder(self, comp, 5, 18, g_ph_id, 5, 1,
                                      CTUI_COLOR_DEFAULT);
  } else if (g_ph_id && !g_ph_wide) {
    ctui_widget_put_kitty_placeholder(self, comp, 2, 14, g_ph_id, 3, 2,
                                      CTUI_COLOR_DEFAULT);
  }
  if (g_ph_id && g_ph_wide) {
    ctui_widget_put_kitty_placeholder(self, comp, 5, 20, g_ph_id, 4, 1,
                                      CTUI_COLOR_DEFAULT);
    self->buf[5 * comp->cols + 20].kitty_col = 255;
  }
}

/* kitty's pixel images through the graphics sink: a 2 x 2 RGBA one
 * enlarged over two cells of text, half transparent; a 48 x 48 gradient
 * shrunk into 3 x 2 cells under the text; one below UNDER_BG, showing
 * only where the background is the default */
static void test_images(CTUI_VK *vk, CTUI_SCREEN *screen) {
  static uint8_t small[2 * 2 * 4] = {255, 0, 0,   128, 0,   255, 0, 128,
                                     0,   0, 255, 255, 255, 255, 0, 0};
  static uint8_t grad[48 * 48 * 4];
  for (int y = 0; y < 48; y++) {
    for (int x = 0; x < 48; x++) {
      uint8_t *t = grad + (y * 48 + x) * 4;
      t[0] = (uint8_t)(x * 5);
      t[1] = (uint8_t)(y * 5);
      t[2] = (uint8_t)((x + y) * 2);
      t[3] = 255;
    }
  }
  static uint8_t under[4 * 4];
  for (int i = 0; i < 16; i += 4) {
    under[i] = 9, under[i + 1] = 99, under[i + 2] = 199, under[i + 3] = 255;
  }
  ctui_gfx_set_sink(ctui_vk_gfx_sink(), vk);
  ctui_gfx_set_mode(CTUI_GFX_KITTY);
  ctui_gfx_kitty_display(1, 2, 3, 2, grad, 48, 48, 2, -1);
  ctui_gfx_kitty_display(2, 1, 2, 1, small, 2, 2, 1, 0);
  /* row 2 (0-based 1): "bold" on blue, then the red space: neither
   * default, so under UNDER_BG nothing shows there; row 6: past the
   * text, the default background */
  ctui_gfx_kitty_display(2, 1, 6, 1, under, 2, 2, 3,
                         CTUI_GFX_KITTY_Z_UNDER_BG - 1);
  CTUI_GFX_KITTY_IMAGE *img = ctui_gfx_kitty_image_new(4);
  unsigned char *b = ctui_gfx_kitty_image_begin(img, 2, 2, 0);
  CTUI_TEST_ASSERT(b != NULL, "image_begin hands out the host copy");
  if (b) {
    memcpy(b, under, sizeof under);
    ctui_gfx_kitty_image_commit(img, 6, 20, 4, 1, CTUI_GFX_KITTY_Z_UNDER_BG - 1);
  }
  g_pl[0] = (PL){1, 0, 6, 1, CTUI_GFX_KITTY_Z_UNDER_BG - 1, under, 2, 2, 0, 0, 0, 0};
  g_pl[1] = (PL){5, 19, 4, 1, CTUI_GFX_KITTY_Z_UNDER_BG - 1, under, 2, 2, 0, 0, 0, 0};
  g_pl[2] = (PL){0, 1, 3, 2, -1, grad, 48, 48, 0, 0, 0, 0};
  g_pl[3] = (PL){1, 0, 2, 1, 0, small, 2, 2, 0, 0, 0, 0};
  g_npl = 4;
  CTUI_TEST_ASSERT(ctui_vk_pending(vk), "placed images: something to paint");
  unsigned tiles = paint(vk);
  unsigned bad = differ(vk, screen);
  CTUI_TEST_ASSERT(tiles > 0 && bad == 0,
                   "four images composited as the reference: %u differ (%u "
                   "tiles)",
                   bad, tiles);
  /* (1, 0) cell's first pixel: small's red, alpha 128, over the bold
   * cell's blue background (0x0000ee) */
  CTUI_TEST_ASSERT(g_px[GH * W] == rgba(over(0x0000ee, 0xff000080u)),
                   "half-transparent red over blue: %08x", g_px[GH * W]);
  /* row 5, col 20: under UNDER_BG, the default background: it shows */
  CTUI_TEST_ASSERT(g_px[(5 * GH + 3) * W + 20 * GW + 3] == rgba(0x0963c7),
                   "below UNDER_BG on a default background: shown (%08x)",
                   g_px[(5 * GH + 3) * W + 20 * GW + 3]);

  ctui_gfx_kitty_display(2, 1, 2, 1, small, 2, 2, 1, 0);
  tiles = paint(vk);
  CTUI_TEST_ASSERT(tiles == tiles_under(0, GH, 2 * GW, GH),
                   "the same image again: its tiles alone (%u)", tiles);

  ctui_gfx_kitty_delete(2);
  g_pl[2] = g_pl[3];
  g_npl = 3;
  tiles = paint(vk);
  bad = differ(vk, screen);
  CTUI_TEST_ASSERT(tiles == tiles_under(GW, 0, 3 * GW, 2 * GH) && bad == 0,
                   "deleted: its tiles painted without it, as the reference "
                   "(%u tiles, %u differ)",
                   tiles, bad);
  CTUI_TEST_ASSERT(hash() == 0x5030fa8b1bb4f372ull, "the images' hash: %016llx",
                   (unsigned long long)hash());

  /* a 2 x 3 picture into a rect 3 px in from its 4 x 2 cells' corner,
   * 20 x 25 px: enlarged by the card, a frame's worth inside the cells */
  static const uint8_t grid[2 * 3 * 4] = {200, 10, 10, 255, 10, 200, 10, 255,
                                          10, 10, 200, 255, 90, 90, 9, 255,
                                          9, 90, 90, 255, 250, 250, 250, 255};
  CTUI_TEST_ASSERT(ctui_gfx_display_px(4, 3, 4, 2, 3, 3, 20, 25, grid, 2, 3, 6,
                                       0) == 0,
                   "display_px goes to the sink");
  g_pl[g_npl++] = (PL){3, 2, 4, 2, 0, grid, 2, 3, 3, 3, 20, 25};
  tiles = paint(vk);
  bad = differ(vk, screen);
  CTUI_TEST_ASSERT(tiles > 0 && bad == 0,
                   "display_px: scaled into its pixel rect as the reference "
                   "(%u tiles, %u differ)",
                   tiles, bad);
  unsigned in = (3 * GH + 3) * W + 2 * GW + 3, out = (3 * GH + 2) * W + 2 * GW + 2;
  CTUI_TEST_ASSERT(g_px[in] == rgba(0xc80a0a) && g_px[out] != rgba(0xc80a0a),
                   "its first texel starts 3 px in, not at the cell (%08x, %08x)",
                   g_px[in], g_px[out]);
  ctui_gfx_kitty_display(4, 3, 4, 2, grid, 2, 3, 6, 0);
  g_pl[g_npl - 1].pw = 0;
  tiles = paint(vk);
  bad = differ(vk, screen);
  CTUI_TEST_ASSERT(bad == 0,
                   "a plain display() after it: the whole cells again (%u differ)",
                   bad);
  ctui_gfx_kitty_delete(6);
  g_npl--;
  CTUI_TEST_ASSERT(ctui_gfx_display_px(1, 1, 1, 1, 0, 0, 8, 16, grid, 2, 3, 7,
                                       0) == 0 &&
                       (ctui_gfx_set_sink(NULL, NULL),
                        ctui_gfx_display_px(1, 1, 1, 1, 0, 0, 8, 16, grid, 2,
                                            3, 7, 0) == -1),
                   "display_px without a sink: -1, nothing to do");
  ctui_gfx_set_sink(ctui_vk_gfx_sink(), vk);
  ctui_gfx_kitty_delete(7);
  ctui_gfx_kitty_image_free(img);
  ctui_gfx_kitty_delete(1);
  ctui_gfx_kitty_delete(3);
  ctui_gfx_kitty_delete(4);
  g_npl = 0;
  paint(vk);
  CTUI_TEST_ASSERT(differ(vk, screen) == 0, "all gone: the text alone again");
  ctui_gfx_set_sink(NULL, NULL);
}

/* look's controls and icons: a PNG placed with place_file() and shown by
 * placeholder cells, fit into them (4 x 4 texels into 3 x 2 cells: 24 x
 * 32 px, so 24 x 24 centred); the same image put() under text, then
 * unput(); the file placed again: only its cells repainted */
static void test_placed(CTUI_VK *vk, CTUI_APP *app, CTUI_SCREEN *screen) {
  static uint8_t px[4 * 4 * 4], px2[4 * 4 * 4];
  for (int i = 0; i < 16; i++) {
    px[i * 4] = (uint8_t)(i * 16), px[i * 4 + 1] = 200, px[i * 4 + 2] = 40;
    px[i * 4 + 3] = (uint8_t)(i % 4 == 3 ? 0 : 255);
    px2[i * 4] = 10, px2[i * 4 + 1] = (uint8_t)(i * 15), px2[i * 4 + 2] = 90;
    px2[i * 4 + 3] = 255;
  }
  char path[] = "/tmp/vk_test.XXXXXX";
  int fd = mkstemp(path);
  if (fd >= 0) {
    close(fd);
  }
  CTUI_TEST_ASSERT(ctui_look_png_write(path, px, 4, 4) == 0, "a PNG written");
  ctui_gfx_set_sink(ctui_vk_gfx_sink(), vk);
  ctui_gfx_kitty_place_file(9, path, 3, 2);
  g_ph_id = 9, g_ph_rgba = px, g_ph_w = 4, g_ph_h = 4, g_ph_cols = 3,
  g_ph_rows = 2;
  ctui_app_render(app, screen);
  ctui_screen_flush(screen);
  paint(vk);
  unsigned bad = differ(vk, screen);
  CTUI_TEST_ASSERT(bad == 0,
                   "placeholder cells show the placed file, fit and centred, "
                   "as the reference: %u differ",
                   bad);
  /* (2, 14): the image's first texel where the fit puts it (a square
   * centred in 3 x 2 cells): rgb (0, 200, 40) */
  unsigned side = 3 * GW < 2 * GH ? 3 * GW : 2 * GH;
  unsigned first = (2 * GH + (2 * GH - side) / 2) * W + 14 * GW + (3 * GW - side) / 2;
  CTUI_TEST_ASSERT(g_px[first] == rgba(0x00c828) && g_px[first - W] != rgba(0x00c828),
                   "its first texel where the fit puts it: %08x", g_px[first]);

  ctui_gfx_kitty_put(9, 2, 6, 1, 4, 1, CTUI_GFX_KITTY_Z_UNDER_BG - 1);
  g_pl[0] = (PL){5, 0, 4, 1, CTUI_GFX_KITTY_Z_UNDER_BG - 1, px, 4, 4, 0, 0, 0, 0};
  g_npl = 1;
  paint(vk);
  bad = differ(vk, screen);
  CTUI_TEST_ASSERT(bad == 0,
                   "put under the background: as the reference (%u differ)",
                   bad);
  ctui_gfx_kitty_unput(9, 2);
  g_npl = 0;
  unsigned tiles = paint(vk);
  CTUI_TEST_ASSERT(tiles == tiles_under(0, 5 * GH, 4 * GW, GH) && differ(vk, screen) == 0,
                   "unput: its tiles painted without it (%u)", tiles);

  ctui_gfx_place_rgba(9, px2, 4, 4, 3, 2);
  g_ph_rgba = px2;
  tiles = paint(vk);
  bad = differ(vk, screen);
  CTUI_TEST_ASSERT(tiles == tiles_under(14 * GW, 2 * GH, 3 * GW, 2 * GH) && bad == 0,
                   "placed again from its pixels (place_rgba, no file): its "
                   "cells' tiles alone, the new texels (%u tiles, %u differ)",
                   tiles, bad);
  CTUI_TEST_ASSERT(hash() == 0x3687e6278e4e8b33ull, "the placed file's hash: %016llx",
                   (unsigned long long)hash());

  ctui_gfx_kitty_delete(9);
  g_ph_id = 0;
  ctui_app_render(app, screen);
  ctui_screen_flush(screen);
  paint(vk);
  CTUI_TEST_ASSERT(differ(vk, screen) == 0,
                   "deleted, the cells gone: the text alone");

  /* a control wider than 255 cells (a seek bar on a wide screen): its
   * columns go on past 255 instead of wrapping to its left end */
  enum { WIDE = 300 };
  static uint8_t wide[WIDE * 4];
  for (int i = 0; i < WIDE; i++) {
    wide[i * 4] = (uint8_t)(i & 0xff), wide[i * 4 + 1] = (uint8_t)(i >> 8 ? 200 : 0);
    wide[i * 4 + 2] = 77, wide[i * 4 + 3] = 255;
  }
  ctui_gfx_place_rgba(11, wide, WIDE, 1, WIDE, 1);
  g_ph_id = 11, g_ph_rgba = wide, g_ph_w = WIDE, g_ph_h = 1, g_ph_cols = WIDE,
  g_ph_rows = 1, g_ph_wide = 1;
  ctui_app_render(app, screen);
  ctui_screen_flush(screen);
  paint(vk);
  bad = differ(vk, screen);
  /* the fit: 300 x 1 texels into 300 cells, GW px tall and centred;
   * column 256 is cell 22 */
  uint32_t at = g_px[(5 * GH + GH / 2) * W + 22 * GW + 3];
  CTUI_TEST_ASSERT(bad == 0 && at == rgba(0x00c84d),
                   "a placeholder run past column 255 goes on (256: %08x), as "
                   "the reference (%u differ)",
                   at, bad);
  ctui_gfx_kitty_delete(11);
  g_ph_id = 0, g_ph_wide = 0;
  ctui_app_render(app, screen);
  ctui_screen_flush(screen);
  paint(vk);
  ctui_gfx_set_sink(NULL, NULL);
  unlink(path);
}

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* a slider in parts (ctui_gfx_wipe()): its empty track the cells' image,
 * the filled one left of the split, the thumb over it there -- still,
 * then moving with the frame's time, then held at its end */
static void test_wipe(CTUI_VK *vk, CTUI_APP *app, CTUI_SCREEN *screen) {
  /* the track exactly its 5 x 1 cells' pixels */
  enum { MW = 5, TWMAX = 64, THMAX = 32 };
  const int TW = 5 * (int)GW, TH = (int)GH;
  static uint8_t empty[TWMAX * THMAX * 4], full[TWMAX * THMAX * 4], thumb[MW * THMAX * 4];
  for (int i = 0; i < TW * TH; i++) {
    empty[i * 4] = 20, empty[i * 4 + 1] = 40, empty[i * 4 + 2] = (uint8_t)(100 + i % TW);
    full[i * 4] = (uint8_t)(150 + i % TW), full[i * 4 + 1] = 30, full[i * 4 + 2] = 30;
    empty[i * 4 + 3] = full[i * 4 + 3] = 255;
  }
  for (int i = 0; i < MW * TH; i++) {
    thumb[i * 4] = 10, thumb[i * 4 + 1] = (uint8_t)(200 + i % MW), thumb[i * 4 + 2] = 10;
    thumb[i * 4 + 3] = i / MW == 0 || i / MW == TH - 1 ? 0 : 255;
  }
  ctui_gfx_set_sink(ctui_vk_gfx_sink(), vk);
  ctui_gfx_place_rgba(12, empty, TW, TH, 5, 1);
  ctui_gfx_place_rgba(13, full, TW, TH, 5, 1);
  ctui_gfx_place_rgba(14, thumb, MW, TH, 1, 1);
  CTUI_TEST_ASSERT(ctui_gfx_wipe(12, 13, 14, 10.4, 0.0, 30.0) == 0, "a wipe goes to the sink");
  g_ph_id = 12, g_ph_rgba = empty, g_ph_w = TW, g_ph_h = TH, g_ph_cols = 5, g_ph_rows = 1;
  g_ph_row5 = 1;
  g_wipe_split = 10, g_wipe_left = full, g_wipe_mark = thumb, g_wipe_mw = MW, g_wipe_mh = TH;
  ctui_app_render(app, screen);
  ctui_screen_flush(screen);
  uint64_t next = 0;
  CTUI_TEST_ASSERT(ctui_vk_at(vk, now_ns(), &next) == 0 && next == 0,
                   "a still wipe: nothing moving");
  paint(vk);
  unsigned bad = differ(vk, screen);
  /* row 5's cells from x 18 * GW: the split 10 px in, the thumb 8..12 */
  unsigned row = (5 * GH + 4) * W + 18 * GW;
  CTUI_TEST_ASSERT(bad == 0 && g_px[row + 7] == rgba(0x9d1e1e) &&
                       g_px[row + 8] == rgba(0x0ac80a) && g_px[row + 12] == rgba(0x0acc0a) &&
                       g_px[row + 13] == rgba(0x142871),
                   "the track filled left of its split, empty right, the thumb "
                   "centred on it, as the reference (%u differ)",
                   bad);

  uint64_t t0 = now_ns();
  ctui_gfx_wipe(12, 13, 14, 10.0, 100.0, 30.0);
  /* 55 ms on: x 15.5 less the moment the call took to stamp its start */
  int moving = ctui_vk_at(vk, t0 + 55000000u, &next);
  g_wipe_split = 15;
  paint(vk);
  bad = differ(vk, screen);
  CTUI_TEST_ASSERT(moving && next > 0 && next <= 5100000u && bad == 0,
                   "moving at 100 px/s, 55 ms on: the split at 15, the next "
                   "column due within 5 ms (%llu ns), as the reference (%u differ)",
                   (unsigned long long)next, bad);
  unsigned tiles = paint(vk);
  CTUI_TEST_ASSERT(tiles == 0, "between columns nothing to paint (%u)", tiles);
  moving = ctui_vk_at(vk, t0 + 2000000000u, &next);
  g_wipe_split = 30;
  paint(vk);
  bad = differ(vk, screen);
  CTUI_TEST_ASSERT(!moving && next == 0 && bad == 0,
                   "past its end: held at 30, nothing moving (%u differ)", bad);
  ctui_gfx_wipe(12, 0, 0, 0, 0, 0);
  g_wipe_split = -1;
  ctui_vk_at(vk, now_ns(), NULL);
  paint(vk);
  CTUI_TEST_ASSERT(differ(vk, screen) == 0, "the wipe ended: the image alone");
  for (unsigned id = 12; id <= 14; id++) {
    ctui_gfx_kitty_delete(id);
  }
  g_ph_id = 0, g_ph_row5 = 0;
  ctui_app_render(app, screen);
  ctui_screen_flush(screen);
  paint(vk);
  ctui_gfx_set_sink(NULL, NULL);
}

/* the font on its own (ttf.h): kitty's cell from its metrics, box
 * drawing meeting its neighbours at every edge */
static void test_ttf(void) {
  FILE *f = fopen("src/vk/fonts/JetBrainsMono-Regular.ttf", "rb");
  static uint8_t bytes[300000], cov[64 * 64];
  size_t n = f ? fread(bytes, 1, sizeof bytes, f) : 0;
  if (f) {
    fclose(f);
  }
  CTUI_TTF t;
  CTUI_TEST_ASSERT(n > 0 && ctui_ttf_load(&t, bytes, n) == 0 && !ctui_ttf_load(&t, bytes, n),
                   "JetBrains Mono loads (%zu bytes)", n);
  CTUI_TEST_ASSERT(ctui_ttf_load(&t, bytes, 40) == -1, "a truncated file doesn't");
  ctui_ttf_load(&t, bytes, n);
  int cw, ch, base;
  float em = 11.0f * 96.0f / 72.0f;
  ctui_ttf_cell(&t, em, &cw, &ch, &base);
  CTUI_TEST_ASSERT(cw == 9 && ch == 20 && base == 15,
                   "11 pt at 96 dpi: a 9x20 cell, the baseline at 15 (%dx%d, %d)", cw, ch,
                   base);
  CTUI_TEST_ASSERT(ctui_ttf_glyph(&t, 'A') && ctui_ttf_glyph(&t, 0x2500) &&
                       !ctui_ttf_glyph(&t, 0x2603) && !ctui_ttf_glyph(&t, 0x1f602),
                   "A and box drawing in its cmap; no snowman, nothing past the BMP");
  ctui_ttf_draw(&t, ctui_ttf_glyph(&t, 0x2500), em, cw, ch, base, 1, cov);
  int across = 0;
  for (int y = 0; y < ch; y++) {
    across |= cov[y * cw] > 200 && cov[y * cw + cw - 1] > 200;
  }
  ctui_ttf_draw(&t, ctui_ttf_glyph(&t, 0x2502), em, cw, ch, base, 1, cov);
  int down = 0;
  for (int x = 0; x < cw; x++) {
    down |= cov[x] > 200 && cov[(ch - 1) * cw + x] > 200;
  }
  CTUI_TEST_ASSERT(across && down,
                   "stretched to the cell: \u2500 reaches both sides, \u2502 top and bottom");
  ctui_ttf_draw(&t, ctui_ttf_glyph(&t, 'H'), em, cw, ch, base, 0, cov);
  int ink = 0, top = ch;
  for (int i = 0; i < cw * ch; i++) {
    ink += cov[i] > 128;
    top = cov[i] > 128 && i / cw < top ? i / cw : top;
  }
  CTUI_TEST_ASSERT(ink > 20 && top >= 3 && top <= 5 && cov[base * cw] == 0,
                   "H: inked, its cap height's top at row %d, nothing below the "
                   "baseline",
                   top);
  setenv("CTUI_FONT", "/dev/null", 1);
  CTUI_VK *vk = ctui_vk_new(g_phys, g_dev);
  int rows, cols;
  CTUI_TEST_ASSERT(vk && ctui_vk_resize(vk, W, H, 120, 0, &rows, &cols) == 0 && cols == 25,
                   "CTUI_FONT not a font: the built-in one (%d cols)", cols);
  ctui_vk_free(vk);
  unsetenv("CTUI_FONT");
}

/* the glyph provider's calls; it draws a codepoint's cells as levels it
 * can be told by: 0x40 the left cell, 0xc0 the right (a column 0..gw-1
 * of each set, the rest clear), 0x80 a narrow one; U+1F47B it lacks */
static unsigned g_asked;
static uint32_t g_asked_cp[16];
static int g_asked_cells[16];

static int provide(void *ctx, uint32_t cp, int cells, int gw, int gh, int base,
                   float em, uint8_t *cov) {
  (void)ctx, (void)base, (void)em;
  if (g_asked < 16) {
    g_asked_cp[g_asked] = cp;
    g_asked_cells[g_asked] = cells;
  }
  g_asked++;
  if (cp == 0x1f47b) {
    return -1;
  }
  for (int y = 0; y < gh; y++) {
    for (int x = 0; x < cells * gw; x++) {
      cov[y * cells * gw + x] = (uint8_t)(cells == 1 ? 0x80
                                          : x < gw   ? 0x40
                                                     : 0xc0);
    }
  }
  return 0;
}

static const char *g_glyphs;

static void render_glyphs(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  ctui_widget_puts(self, comp, 0, 0, g_glyphs, CTUI_COLOR_WHITE,
                   CTUI_COLOR_DEFAULT);
}

/* the asked codepoint's index, -1 if never asked */
static int asked(uint32_t cp, int cells) {
  for (unsigned i = 0; i < g_asked && i < 16; i++) {
    if (g_asked_cp[i] == cp && g_asked_cells[i] == cells) {
      return (int)i;
    }
  }
  return -1;
}

/* what the font lacks from the provider: width 2 across two slots, past
 * the BMP, a cluster as its first codepoint */
static void test_glyphs(void) {
  CTUI_VK *vk = ctui_vk_new(g_phys, g_dev);
  int rows = 0, cols = 0;
  if (!vk || ctui_vk_resize(vk, W, H, 120, 0, &rows, &cols) != 0) {
    CTUI_TEST_ASSERT(0, "a second backend");
    ctui_vk_free(vk);
    return;
  }
  CTUI_WIDGET text =
      ctui_widget_make(0, 0, cols, rows, NULL, render_glyphs, NULL);
  CTUI_WIDGET *ws[] = {&text};
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);
  ctui_screen_set_sink(screen, ctui_vk_screen_sink(), vk);
  CTUI_APP app;
  ctui_app_init(&app, ws, 1, rows, cols);

  /* 中, then 😀 (both width 2), 𝐀 (U+1D400, width 1, past the BMP), e +
   * U+0301 (a cluster: the font's e), 👻 (the provider lacks it) */
  g_glyphs = "\xe4\xb8\xad\xf0\x9f\x98\x80\xf0\x9d\x90\x80"
             "e\xcc\x81\xf0\x9f\x91\xbb|";
  ctui_app_render(&app, screen);
  ctui_screen_flush(screen);
  uint32_t c[CTUI_VK_CELL_WORDS];
  ctui_vk_cell(vk, &screen->buffer[0], c);
  CTUI_TEST_ASSERT((c[0] & 0xffffu) == CTUI_VK_GLYPH_BOX &&
                       !(c[0] >> 16 & CTUI_VK_WIDE),
                   "no provider: 中 a box, not wide");
  ctui_vk_set_glyph_fn(vk, provide, NULL);
  /* the boxes' cells drawn again from their next change */
  const char *had = g_glyphs;
  g_glyphs = "";
  ctui_app_render(&app, screen);
  ctui_screen_flush(screen);
  g_glyphs = had;
  ctui_app_render(&app, screen);
  ctui_screen_flush(screen);
  memset(g_px, 0, W * H * 4u);
  paint(vk);
  CTUI_TEST_ASSERT(asked(0x4e2d, 2) >= 0 && asked(0x1f600, 2) >= 0 &&
                       asked(0x1d400, 1) >= 0 && asked(0x1f47b, 2) >= 0 &&
                       asked('e', 1) < 0,
                   "asked of the provider: 中 and 😀 at 2 cells, U+1D400 at 1, "
                   "👻; e (the font's) not (%u asked)",
                   g_asked);
  unsigned gw, gh, sw;
  const uint32_t *slots;
  ctui_vk_glyphs(vk, &gw, &gh, &slots, &sw);
  uint32_t lead[CTUI_VK_CELL_WORDS], cont[CTUI_VK_CELL_WORDS];
  ctui_vk_cell(vk, &screen->buffer[0], lead);
  ctui_vk_cell(vk, &screen->buffer[1], cont);
  ctui_vk_cont(cont, lead);
  unsigned l = lead[0] & 0xffffu, r = cont[0] & 0xffffu;
  CTUI_TEST_ASSERT((lead[0] >> 16 & CTUI_VK_WIDE) && r == l + 1 &&
                       coverage(slots, sw, gw, l, 0, 0) == 0x40 &&
                       coverage(slots, sw, gw, l, gw - 1, gh - 1) == 0x40 &&
                       coverage(slots, sw, gw, r, 0, 0) == 0xc0 &&
                       coverage(slots, sw, gw, r, gw - 1, gh - 1) == 0xc0,
                   "中: wide, its halves in slots %u and %u", l, r);
  uint32_t narrow[CTUI_VK_CELL_WORDS], clus[CTUI_VK_CELL_WORDS],
      ghost[CTUI_VK_CELL_WORDS];
  ctui_vk_cell(vk, &screen->buffer[4], narrow);
  ctui_vk_cell(vk, &screen->buffer[5], clus);
  ctui_vk_cell(vk, &screen->buffer[6], ghost);
  CTUI_CELL e = {.ch = 'e', .fg = CTUI_COLOR_WHITE};
  uint32_t plain[CTUI_VK_CELL_WORDS];
  ctui_vk_cell(vk, &e, plain);
  CTUI_TEST_ASSERT(
      (screen->buffer[5].ch & CTUI_CELL_CLUSTER) &&
          !(narrow[0] >> 16 & CTUI_VK_WIDE) &&
          coverage(slots, sw, gw, narrow[0] & 0xffffu, 1, 1) == 0x80 &&
          (clus[0] & 0xffffu) == (plain[0] & 0xffffu) &&
          (ghost[0] & 0xffffu) == CTUI_VK_GLYPH_BOX,
      "U+1D400 narrow from the provider, é as a cluster the font's e, "
      "👻 a box");
  CTUI_TEST_ASSERT(differ(vk, screen) == 0,
                   "the card's pixels are the reference's");
  CTUI_TEST_ASSERT(g_px[gh / 2 * W + gw + gw / 2] != g_px[gh / 2 * W + gw / 2],
                   "中's right half painted apart from its left (%08x %08x)",
                   g_px[gh / 2 * W + gw + gw / 2], g_px[gh / 2 * W + gw / 2]);

  /* a wide glyph for another: the CONT cell itself unchanged */
  unsigned before = g_asked;
  g_glyphs = "\xf0\x9f\x98\x80\xe4\xb8\xad\xf0\x9d\x90\x80"
             "e\xcc\x81\xf0\x9f\x91\xbb|";
  ctui_app_render(&app, screen);
  ctui_screen_flush(screen);
  paint(vk);
  ctui_vk_cell(vk, &screen->buffer[0], lead);
  CTUI_TEST_ASSERT(
      g_asked == before && differ(vk, screen) == 0,
      "中 and 😀 swapped: nothing asked again, the CONT cells follow "
      "their leads");
  ctui_app_free(&app);
  ctui_screen_free(screen);
  ctui_vk_free(vk);
}

static void test_vk(void) {
  CTUI_VK *vk = ctui_vk_new(g_phys, g_dev);
  int rows = 0, cols = 0;
  int ok = vk && ctui_vk_resize(vk, W, H, 120, 0, &rows, &cols) == 0;
  const uint32_t *slots;
  unsigned sw;
  if (ok) {
    ctui_vk_glyphs(vk, &GW, &GH, &slots, &sw);
  }
  CTUI_TEST_ASSERT(ok && GW == 9 && GH == 20 && rows == 6 && cols == 25,
                   "a %ux%u surface at scale 1: JetBrains Mono's 11 pt cell "
                   "9x20 (%ux%u), %dx%d cells",
                   W, H, GW, GH, cols, rows);
  int cw = 0, ch = 0;
  ctui_cell_px(&cw, &ch);
  CTUI_TEST_ASSERT(cw == (int)GW && ch == (int)GH, "the cell's pixels told to core: %dx%d",
                   cw, ch);
  if (!ok || back_new() != 0) {
    CTUI_TEST_ASSERT(0, "a readback buffer");
    ctui_vk_free(vk);
    return;
  }

  CTUI_WIDGET text = ctui_widget_make(0, 0, cols, rows, NULL, render_text,
                                      NULL);
  CTUI_WIDGET *ws[] = {&text};
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);
  ctui_screen_set_sink(screen, ctui_vk_screen_sink(), vk);
  CTUI_APP app;
  ctui_app_init(&app, ws, 1, rows, cols);
  ctui_app_render(&app, screen);
  ctui_screen_flush(screen);

  CTUI_TEST_ASSERT(ctui_vk_pending(vk), "a first frame: something to paint");
  memset(g_px, 0, W * H * 4u);
  unsigned tiles = paint(vk);
  CTUI_TEST_ASSERT(tiles == tiles_under(0, 0, W, H), "the first record paints every tile: %u",
                   tiles);
  unsigned bad = differ(vk, screen);
  CTUI_TEST_ASSERT(bad == 0, "the card's pixels are the reference's: %u differ",
                   bad);
  /* the space at (1, 5): red, top-left pixel; row 0 col 0's top row: the
   * default background; past the last whole row: the default background */
  CTUI_TEST_ASSERT(g_px[GH * W + 5 * GW] == rgba(0xcd0000) &&
                       g_px[0] == rgba(0x000000) &&
                       g_px[(H - 2) * W + 3] == rgba(0x000000),
                   "basic red, the default background, the strip past the "
                   "cells (%08x %08x %08x)",
                   g_px[GH * W + 5 * GW], g_px[0], g_px[(H - 2) * W + 3]);
  uint32_t c[CTUI_VK_CELL_WORDS];
  ctui_vk_cell(vk, &screen->buffer[4 * cols + 3], c);
  uint32_t snow = c[0] & 0xffff;
  ctui_vk_cell(vk, &screen->buffer[4 * cols + 5], c);
  uint32_t cont = c[0] & 0xffff;
  ctui_vk_cell(vk, &screen->buffer[2 * cols], c);
  CTUI_TEST_ASSERT(snow == CTUI_VK_GLYPH_BOX && cont == 0 &&
                       c[1] == 0xff8700 && c[2] == 0x303030 &&
                       (c[0] >> 16 & CTUI_ATTR_UNDERLINE),
                   "a missing glyph a box, a width-2 glyph's CONT blank, 256 "
                   "colours resolved (%06x on %06x)",
                   c[1], c[2]);
  dump_ppm("the first frame");
  uint64_t h = hash();
  CTUI_TEST_ASSERT(h == 0xde3d70220e4ef88full, "the surface's hash: %016llx",
                   (unsigned long long)h);

  VkBufferImageCopy reg[H / 16 + 1];
  unsigned n = ctui_vk_regions(vk, 0, 1, reg);
  CTUI_TEST_ASSERT(n == 1 && reg[0].imageExtent.width == W &&
                       reg[0].imageExtent.height == H,
                   "a fresh image: the whole surface, one region (%u)", n);
  CTUI_TEST_ASSERT(ctui_vk_regions(vk, 0, 0, reg) == 0,
                   "then nothing for it");

  ctui_app_render(&app, screen);
  ctui_screen_flush(screen);
  CTUI_TEST_ASSERT(!ctui_vk_pending(vk) && paint(vk) == 0,
                   "unchanged: nothing pending, nothing recorded");

  g_title = "ctui on the cart";
  ctui_app_render(&app, screen);
  ctui_screen_flush(screen);
  tiles = paint(vk);
  CTUI_TEST_ASSERT(tiles == tiles_under(15 * GW, 0, GW, GH) && differ(vk, screen) == 0,
                   "one cell changed: its tiles alone, the reference's (%u)",
                   tiles);
  n = ctui_vk_regions(vk, 0, 0, reg);
  unsigned tx0 = 15 * GW / 16 * 16, tx1 = (16 * GW - 1) / 16 * 16 + 16;
  CTUI_TEST_ASSERT(n == 1 && reg[0].imageOffset.x == (int)tx0 &&
                       reg[0].imageOffset.y == 0 &&
                       reg[0].imageExtent.width == tx1 - tx0 &&
                       reg[0].imageExtent.height == (GH - 1) / 16 * 16 + 16 &&
                       reg[0].bufferOffset == tx0 * 4u &&
                       reg[0].bufferRowLength == W,
                   "an image behind on it: that tile's region alone (%u)", n);
  n = ctui_vk_regions(vk, 1, 0, reg);
  CTUI_TEST_ASSERT(n == 1 && reg[0].imageExtent.width == W &&
                       reg[0].imageExtent.height == H,
                   "an image never presented from: behind on everything "
                   "since the resize, one region (%u)",
                   n);

  ctui_vk_set_default(vk, 0x202020, 0xe0e0e0);
  tiles = paint(vk);
  CTUI_TEST_ASSERT(tiles == tiles_under(0, 0, W, H) && differ(vk, screen) == 0 &&
                       g_px[0] == rgba(0xe0e0e0),
                   "new default colours: every tile repainted with them, "
                   "no cell up again (%u)",
                   tiles);

  test_images(vk, screen);
  test_placed(vk, &app, screen);
  test_wipe(vk, &app, screen);
  test_glyphs();

  ctui_app_free(&app);
  ctui_screen_free(screen);
  back_free();
  ctui_vk_free(vk);
}

int main(void) {
  ctui_log_init(E_ALL);
  const char *why = open_device();
  if (why) {
    printf("  skipped: %s\n", why);
    close_device();
    return ctui_test_summary();
  }
  test_ttf();
  test_vk();
  if (g_validation) {
    CTUI_TEST_ASSERT(g_warnings == 0, "the validation layer: %u warnings",
                     g_warnings);
  }
  close_device();
  return ctui_test_summary();
}
