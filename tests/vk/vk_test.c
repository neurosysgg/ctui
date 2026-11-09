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
#include "vk/vk.h"

#include <stdio.h>
#include <string.h>

#define W 200u
#define H 100u
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

/* cells.comp's rule, on the host, from the screen's cells */
static void reference(CTUI_VK *vk, const CTUI_SCREEN *s, uint32_t *px) {
  unsigned gw, gh;
  const uint32_t *font;
  ctui_vk_glyphs(vk, &gw, &gh, &font);
  uint32_t dfg, dbg;
  ctui_vk_defaults(vk, &dfg, &dbg);
  for (unsigned y = 0; y < H; y++) {
    for (unsigned x = 0; x < W; x++) {
      unsigned cx = x / gw, cy = y / gh, gx = x % gw, gy = y % gh;
      uint32_t rgb = dbg;
      if (cx < (unsigned)s->cols && cy < (unsigned)s->rows) {
        uint32_t c[CTUI_VK_CELL_WORDS];
        ctui_vk_cell(vk, &s->buffer[cy * (unsigned)s->cols + cx], c);
        uint32_t g = c[0] & 0xffffu, a = c[0] >> 16;
        uint32_t fg = a & CTUI_VK_FG_DEFAULT ? dfg : c[1];
        uint32_t bg = a & CTUI_VK_BG_DEFAULT ? dbg : c[2];
        int on;
        if (g == CTUI_VK_GLYPH_BOX) {
          on = gx == 0 || gx == gw - 1 || gy == gh / 8 ||
               gy == gh - 1 - gh / 8;
        } else {
          uint32_t bits = font[g * gh + gy];
          if (a & CTUI_ATTR_BOLD) {
            bits |= bits >> 1;
          }
          on = bits >> (31u - gx) & 1u;
        }
        on |= (a & CTUI_ATTR_UNDERLINE) && gy == gh - 1 - gh / 16;
        on |= (a & CTUI_ATTR_STRIKE) && gy == gh / 2;
        if (a & CTUI_ATTR_DIM) {
          fg = ((fg & 0xfefefeu) >> 1) + ((bg & 0xfefefeu) >> 1);
        }
        rgb = on ? fg : bg;
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
}

static void test_vk(void) {
  CTUI_VK *vk = ctui_vk_new(g_phys, g_dev);
  int rows = 0, cols = 0;
  int ok = vk && ctui_vk_resize(vk, W, H, 120, 0, &rows, &cols) == 0;
  CTUI_TEST_ASSERT(ok && rows == 6 && cols == 25,
                   "a %ux%u surface at scale 1: 8x16 glyphs, %dx%d cells", W,
                   H, cols, rows);
  int cw = 0, ch = 0;
  ctui_cell_px(&cw, &ch);
  CTUI_TEST_ASSERT(cw == 8 && ch == 16, "the cell's pixels told to core: %dx%d",
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
  CTUI_TEST_ASSERT(tiles == 13 * 7, "the first record paints every tile: %u",
                   tiles);
  unsigned bad = differ(vk, screen);
  CTUI_TEST_ASSERT(bad == 0, "the card's pixels are the reference's: %u differ",
                   bad);
  /* the space at (1, 5): red, top-left pixel; row 0 col 0's top row: the
   * default background; past the last whole row: the default background */
  CTUI_TEST_ASSERT(g_px[16 * W + 5 * 8] == rgba(0xcd0000) &&
                       g_px[0] == rgba(0x000000) &&
                       g_px[99 * W + 3] == rgba(0x000000),
                   "basic red, the default background, the strip past the "
                   "cells (%08x %08x %08x)",
                   g_px[16 * W + 40], g_px[0], g_px[99 * W + 3]);
  uint32_t c[CTUI_VK_CELL_WORDS];
  ctui_vk_cell(vk, &screen->buffer[4 * cols + 3], c);
  uint32_t snow = c[0] & 0xffff;
  ctui_vk_cell(vk, &screen->buffer[4 * cols + 5], c);
  uint32_t cont = c[0] & 0xffff;
  ctui_vk_cell(vk, &screen->buffer[2 * cols], c);
  CTUI_TEST_ASSERT(snow == CTUI_VK_GLYPH_BOX && cont == ' ' &&
                       c[1] == 0xff8700 && c[2] == 0x303030 &&
                       (c[0] >> 16 & CTUI_ATTR_UNDERLINE),
                   "a missing glyph a box, a width-2 glyph's CONT blank, 256 "
                   "colours resolved (%06x on %06x)",
                   c[1], c[2]);
  uint64_t h = hash();
  CTUI_TEST_ASSERT(h == 0x661844024fc2294full, "the surface's hash: %016llx",
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
  CTUI_TEST_ASSERT(tiles == 1 && differ(vk, screen) == 0,
                   "one cell changed: its one tile, the reference's (%u)",
                   tiles);
  n = ctui_vk_regions(vk, 0, 0, reg);
  CTUI_TEST_ASSERT(n == 1 && reg[0].imageOffset.x == 15 * 8 / 16 * 16 &&
                       reg[0].imageOffset.y == 0 &&
                       reg[0].imageExtent.width == 16 &&
                       reg[0].imageExtent.height == 16 &&
                       reg[0].bufferOffset == 112u * 4u &&
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
  CTUI_TEST_ASSERT(tiles == 13 * 7 && differ(vk, screen) == 0 &&
                       g_px[0] == rgba(0xe0e0e0),
                   "new default colours: every tile repainted with them, "
                   "no cell up again (%u)",
                   tiles);

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
  test_vk();
  if (g_validation) {
    CTUI_TEST_ASSERT(g_warnings == 0, "the validation layer: %u warnings",
                     g_warnings);
  }
  close_device();
  return ctui_test_summary();
}
