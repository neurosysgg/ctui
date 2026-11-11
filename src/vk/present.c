#define _POSIX_C_SOURCE 200809L
#define VK_USE_PLATFORM_WAYLAND_KHR
#include "present.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS CTUI_VK_SLOTS
#define IMAGES CTUI_VK_IMAGES
#define VALIDATION "VK_LAYER_KHRONOS_validation"
/* the longest an acquire may wait (ns) */
#define ACQUIRE_NS 100000000u

struct CTUI_VK_GPU {
  VkInstance inst;
  VkDebugUtilsMessengerEXT msgr;
  VkPhysicalDevice phys;
  VkDevice dev;
  VkQueue queue;
  uint32_t family;
  VkCommandPool pool;
  struct wl_display *dpy;
  unsigned warnings;
};

struct CTUI_VK_WIN {
  CTUI_VK_GPU *g;
  CTUI_WL *wl;
  CTUI_VK *vk;
  /* a recording a slot: the one before last done before it's reused */
  VkCommandBuffer cmd[SLOTS];
  VkFence fence[SLOTS];
  VkSemaphore acquired[SLOTS];
  int pending[SLOTS];
  unsigned slot;

  VkSurfaceKHR surf;
  VkSwapchainKHR sc;
  VkFormat fmt;
  int bgra;
  uint32_t n;
  VkImage img[IMAGES];
  VkSemaphore done[IMAGES];
  int ready[IMAGES]; /* presented since the swapchain was made */
  unsigned sw, sh;
  int stale; /* 1 suboptimal, 2 out of date: made again before the next */
  int viewport;
  VkBufferImageCopy *regions;
};

/* --- the device --- */

static VKAPI_ATTR VkBool32 VKAPI_CALL
on_message(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
           VkDebugUtilsMessageTypeFlagsEXT type,
           const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
  (void)type;
  if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    ((CTUI_VK_GPU *)user)->warnings++;
    ctui_logf(E_WRN, "[CTUI:VK] - vulkan: %s\n", data->pMessage);
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

/* the user picked the drivers: theirs stands */
static int drivers_chosen(void) {
  static const char *const vars[] = {
      "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES",
      "VK_LOADER_DRIVERS_SELECT", "VK_LOADER_DRIVERS_DISABLE"};
  for (unsigned i = 0; i < sizeof vars / sizeof *vars; i++) {
    const char *v = getenv(vars[i]);
    if (v && *v) {
      return 1;
    }
  }
  return 0;
}

static int make_instance(CTUI_VK_GPU *g, int validation, int no_lvp) {
  const char *layers[] = {VALIDATION};
  const char *exts[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
                        VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
  VkDebugUtilsMessengerCreateInfoEXT dbg = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
      .pfnUserCallback = on_message,
      .pUserData = g,
  };
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "ctui",
                           .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = validation ? &dbg : NULL,
      .pApplicationInfo = &app,
      .enabledLayerCount = validation ? 1u : 0u,
      .ppEnabledLayerNames = layers,
      .enabledExtensionCount = validation ? 3u : 2u,
      .ppEnabledExtensionNames = exts,
  };
  /* the loader reads it at the instance's creation only: nothing this
   * process starts afterwards inherits it */
  if (no_lvp) {
    setenv("VK_LOADER_DRIVERS_DISABLE", "*lvp*", 1);
  }
  VkResult r = vkCreateInstance(&ici, NULL, &g->inst);
  if (no_lvp) {
    unsetenv("VK_LOADER_DRIVERS_DISABLE");
  }
  if (r != VK_SUCCESS) {
    g->inst = VK_NULL_HANDLE;
    return -1;
  }
  if (validation) {
    PFN_vkCreateDebugUtilsMessengerEXT mk =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            g->inst, "vkCreateDebugUtilsMessengerEXT");
    if (mk) {
      mk(g->inst, &dbg, NULL, &g->msgr);
    }
  }
  return 0;
}

static void drop_instance(CTUI_VK_GPU *g) {
  if (g->msgr) {
    PFN_vkDestroyDebugUtilsMessengerEXT rm =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            g->inst, "vkDestroyDebugUtilsMessengerEXT");
    if (rm) {
      rm(g->inst, g->msgr, NULL);
    }
    g->msgr = VK_NULL_HANDLE;
  }
  if (g->inst) {
    vkDestroyInstance(g->inst, NULL);
    g->inst = VK_NULL_HANDLE;
  }
}

/* a device whose queue computes and presents to the display, a discrete
 * card first: 0, or -1 for none */
static int pick(CTUI_VK_GPU *g) {
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(g->inst, &n, NULL);
  VkPhysicalDevice devs[16];
  n = n > 16 ? 16 : n;
  vkEnumeratePhysicalDevices(g->inst, &n, devs);
  int best = -1, best_score = 0;
  for (uint32_t i = 0; i < n; i++) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(devs[i], &p);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, NULL);
    VkQueueFamilyProperties q[16];
    nq = nq > 16 ? 16 : nq;
    vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, q);
    for (uint32_t f = 0; f < nq; f++) {
      if (!(q[f].queueFlags & VK_QUEUE_COMPUTE_BIT) ||
          !vkGetPhysicalDeviceWaylandPresentationSupportKHR(devs[i], f,
                                                            g->dpy)) {
        continue;
      }
      int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
      if (score > best_score) {
        best = (int)i, best_score = score, g->family = f;
      }
      break;
    }
  }
  if (best < 0) {
    return -1;
  }
  g->phys = devs[best];
  return 0;
}

CTUI_VK_GPU *ctui_vk_gpu_open(CTUI_WL_DPY *d, char *err, unsigned n) {
  CTUI_VK_GPU *g = calloc(1, sizeof *g);
  if (!g) {
    snprintf(err, n, "out of memory");
    return NULL;
  }
  g->dpy = ctui_wl_dpy_display(d);
  const char *v = getenv("CTUI_HOST_VALIDATION");
  int validation = v && *v && *v != '0' && has_layer(VALIDATION);
  int no_lvp = !drivers_chosen();
  const char *why = NULL;
  if (make_instance(g, validation, no_lvp) != 0) {
    why = "no Vulkan instance with Wayland surfaces";
  } else if (pick(g) != 0) {
    drop_instance(g);
    /* lavapipe after all, when it's the one that presents here */
    if (!no_lvp || make_instance(g, validation, 0) != 0 || pick(g) != 0) {
      why = "no device that computes and presents to the display";
    }
  }
  float prio = 1.0f;
  const char *dev_ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = g->family,
      .queueCount = 1,
      .pQueuePriorities = &prio,
  };
  VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = dev_ext,
  };
  if (!why && vkCreateDevice(g->phys, &dci, NULL, &g->dev) != VK_SUCCESS) {
    g->dev = VK_NULL_HANDLE;
    why = "no logical device";
  }
  VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = g->family,
  };
  if (!why) {
    vkGetDeviceQueue(g->dev, g->family, 0, &g->queue);
    if (vkCreateCommandPool(g->dev, &pci, NULL, &g->pool) != VK_SUCCESS) {
      g->pool = VK_NULL_HANDLE;
      why = "no command pool";
    }
  }
  if (why) {
    snprintf(err, n, "%s", why);
    ctui_vk_gpu_close(g);
    return NULL;
  }
  VkPhysicalDeviceProperties p;
  vkGetPhysicalDeviceProperties(g->phys, &p);
  ctui_logf(E_INF, "[CTUI:VK] - device: %s%s @ tick %d\n", p.deviceName,
            validation ? " (validation on)" : "", ctui_tick_advance());
  return g;
}

void ctui_vk_gpu_close(CTUI_VK_GPU *g) {
  if (!g) {
    return;
  }
  if (g->dev) {
    vkDeviceWaitIdle(g->dev);
    if (g->pool) {
      vkDestroyCommandPool(g->dev, g->pool, NULL);
    }
    vkDestroyDevice(g->dev, NULL);
  }
  drop_instance(g);
  ctui_logf(E_INF, "[CTUI:VK] - device closed (%u validation warnings)\n",
            g->warnings);
  free(g);
}

unsigned ctui_vk_gpu_warnings(const CTUI_VK_GPU *g) { return g->warnings; }

/* --- a presenter --- */

static void idle(CTUI_VK_WIN *w) {
  VkDevice dev = w->g->dev;
  for (unsigned i = 0; i < SLOTS; i++) {
    if (w->pending[i]) {
      vkWaitForFences(dev, 1, &w->fence[i], VK_TRUE, UINT64_MAX);
      vkResetFences(dev, 1, &w->fence[i]);
      w->pending[i] = 0;
    }
  }
  /* the presents' semaphores too: only the queue knows they're done */
  vkQueueWaitIdle(w->g->queue);
}

/* the swapchain at w x h (made, or made again): RGBA8 or BGRA8 UNORM
 * (the painter writes either), FIFO, as few images as allowed. 0, or -1 */
static int swapchain(CTUI_VK_WIN *w, unsigned pw, unsigned ph) {
  CTUI_VK_GPU *g = w->g;
  idle(w);
  VkSurfaceCapabilitiesKHR caps;
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g->phys, w->surf, &caps) !=
      VK_SUCCESS) {
    return -1;
  }
  uint32_t nf = 0;
  VkSurfaceFormatKHR fmts[64];
  vkGetPhysicalDeviceSurfaceFormatsKHR(g->phys, w->surf, &nf, NULL);
  nf = nf > 64 ? 64 : nf;
  vkGetPhysicalDeviceSurfaceFormatsKHR(g->phys, w->surf, &nf, fmts);
  w->fmt = VK_FORMAT_UNDEFINED;
  for (uint32_t i = 0; i < nf; i++) {
    if (fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
        (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
         (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM &&
          w->fmt == VK_FORMAT_UNDEFINED))) {
      w->fmt = fmts[i].format;
    }
  }
  if (w->fmt == VK_FORMAT_UNDEFINED) {
    return -1;
  }
  w->bgra = w->fmt == VK_FORMAT_B8G8R8A8_UNORM;
  /* Wayland leaves the size to us (currentExtent 0xffffffff) */
  if (caps.currentExtent.width != UINT32_MAX) {
    pw = caps.currentExtent.width;
    ph = caps.currentExtent.height;
  }
  pw = pw < caps.minImageExtent.width ? caps.minImageExtent.width : pw;
  ph = ph < caps.minImageExtent.height ? caps.minImageExtent.height : ph;
  pw = pw > caps.maxImageExtent.width ? caps.maxImageExtent.width : pw;
  ph = ph > caps.maxImageExtent.height ? caps.maxImageExtent.height : ph;
  /* as few images as the driver allows, 2 at least: one shown, one
   * painted. Paced by the frame callbacks (ctui_vk_win_present()), a third
   * would only ever wait -- and cost the VRAM of an image (a 4K bar's 0.6
   * MB, a big panel's 11) */
  uint32_t count = caps.minImageCount < 2u ? 2u : caps.minImageCount;
  if (caps.maxImageCount && count > caps.maxImageCount) {
    count = caps.maxImageCount;
  }
  count = count > IMAGES ? IMAGES : count;
  VkSwapchainKHR old = w->sc;
  VkSwapchainCreateInfoKHR ci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = w->surf,
      .minImageCount = count,
      .imageFormat = w->fmt,
      .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = {pw, ph},
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = caps.currentTransform,
      .compositeAlpha =
          caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
              ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
              : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR,
      .clipped = VK_TRUE,
      .oldSwapchain = old,
  };
  VkResult r = vkCreateSwapchainKHR(g->dev, &ci, NULL, &w->sc);
  if (old) {
    vkDestroySwapchainKHR(g->dev, old, NULL);
  }
  if (r != VK_SUCCESS) {
    w->sc = VK_NULL_HANDLE;
    return -1;
  }
  w->n = IMAGES;
  vkGetSwapchainImagesKHR(g->dev, w->sc, &w->n, w->img);
  memset(w->ready, 0, sizeof w->ready);
  w->sw = pw;
  w->sh = ph;
  w->stale = 0;
  ctui_logf(E_INF,
            "[CTUI:VK] - swapchain %ux%u, %u images (%u at least), %s @ tick "
            "%d\n",
            pw, ph, w->n, caps.minImageCount, w->bgra ? "BGRA" : "RGBA",
            ctui_tick_advance());
  return 0;
}

int ctui_vk_win_resize(CTUI_VK_WIN *w, int *rows, int *cols) {
  int pw, ph;
  ctui_wl_size(w->wl, &pw, &ph);
  if (swapchain(w, (unsigned)pw, (unsigned)ph) != 0 ||
      ctui_vk_resize(w->vk, w->sw, w->sh, ctui_wl_scale120(w->wl), w->bgra,
                     rows, cols) != 0) {
    ctui_logf(E_ERR, "[CTUI:VK] - no swapchain or surface at %dx%d\n", pw, ph);
    return -1;
  }
  unsigned gw, gh, slot_words;
  const uint32_t *slots;
  ctui_vk_glyphs(w->vk, &gw, &gh, &slots, &slot_words);
  ctui_wl_cell(w->wl, (int)gw, (int)gh);
  unsigned ox, oy;
  ctui_vk_origin(w->vk, &ox, &oy);
  ctui_wl_cell_origin(w->wl, (int)ox, (int)oy);
  free(w->regions);
  w->regions = malloc((w->sh / CTUI_VK_TILE + 1) * sizeof *w->regions);
  w->viewport = 1;
  return w->regions ? 0 : -1;
}

CTUI_VK_WIN *ctui_vk_win_new(CTUI_VK_GPU *g, CTUI_WL *wl, int *rows, int *cols,
                             char *err, unsigned n) {
  CTUI_VK_WIN *w = calloc(1, sizeof *w);
  if (!w) {
    snprintf(err, n, "out of memory");
    return NULL;
  }
  w->g = g;
  w->wl = wl;
  const char *why = NULL;
  VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = g->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = SLOTS,
  };
  VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkSemaphoreCreateInfo sci = {.sType =
                                   VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  if (vkAllocateCommandBuffers(g->dev, &cai, w->cmd) != VK_SUCCESS) {
    memset(w->cmd, 0, sizeof w->cmd);
    why = "no command buffers";
  }
  for (unsigned i = 0; !why && i < SLOTS; i++) {
    if (vkCreateFence(g->dev, &fci, NULL, &w->fence[i]) != VK_SUCCESS ||
        vkCreateSemaphore(g->dev, &sci, NULL, &w->acquired[i]) != VK_SUCCESS) {
      why = "no fences or semaphores";
    }
  }
  for (unsigned i = 0; !why && i < IMAGES; i++) {
    if (vkCreateSemaphore(g->dev, &sci, NULL, &w->done[i]) != VK_SUCCESS) {
      why = "no semaphores";
    }
  }
  VkWaylandSurfaceCreateInfoKHR wci = {
      .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
      .display = g->dpy,
      .surface = ctui_wl_surface(wl),
  };
  if (!why &&
      vkCreateWaylandSurfaceKHR(g->inst, &wci, NULL, &w->surf) != VK_SUCCESS) {
    w->surf = VK_NULL_HANDLE;
    why = "no Vulkan surface for it";
  }
  if (!why && !(w->vk = ctui_vk_new(g->phys, g->dev))) {
    why = "the painter's pipeline couldn't be made";
  }
  if (!why && ctui_vk_win_resize(w, rows, cols) != 0) {
    why = "no swapchain for it";
  }
  if (why) {
    snprintf(err, n, "%s", why);
    ctui_vk_win_free(w);
    return NULL;
  }
  return w;
}

void ctui_vk_win_free(CTUI_VK_WIN *w) {
  if (!w) {
    return;
  }
  VkDevice dev = w->g->dev;
  idle(w);
  ctui_vk_free(w->vk);
  free(w->regions);
  if (w->sc) {
    vkDestroySwapchainKHR(dev, w->sc, NULL);
  }
  if (w->surf) {
    vkDestroySurfaceKHR(w->g->inst, w->surf, NULL);
  }
  for (unsigned i = 0; i < IMAGES; i++) {
    if (w->done[i]) {
      vkDestroySemaphore(dev, w->done[i], NULL);
    }
  }
  for (unsigned i = 0; i < SLOTS; i++) {
    if (w->acquired[i]) {
      vkDestroySemaphore(dev, w->acquired[i], NULL);
    }
    if (w->fence[i]) {
      vkDestroyFence(dev, w->fence[i], NULL);
    }
  }
  if (w->cmd[0]) {
    vkFreeCommandBuffers(dev, w->g->pool, SLOTS, w->cmd);
  }
  free(w);
}

CTUI_VK *ctui_vk_win_vk(CTUI_VK_WIN *w) { return w->vk; }
CTUI_WL *ctui_vk_win_wl(CTUI_VK_WIN *w) { return w->wl; }

static void image_barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from,
                          VkImageLayout to, VkAccessFlags src_access,
                          VkAccessFlags dst_access, VkPipelineStageFlags dst) {
  VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = src_access,
      .dstAccessMask = dst_access,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dst, 0, 0, NULL, 0,
                       NULL, 1, &b);
}

int ctui_vk_win_present(CTUI_VK_WIN *w, uint64_t now_ns, uint64_t *next_ns) {
  CTUI_VK_GPU *g = w->g;
  uint64_t next = 0;
  if (!ctui_vk_at(w->vk, now_ns, &next)) {
    next = 0;
  }
  if (next_ns) {
    *next_ns = next;
  }
  if (!w->sc || !ctui_vk_pending(w->vk)) {
    return 0;
  }
  /* the last frame not shown yet: the compositor isn't showing the
   * surface (a fullscreen window over it), or not yet. FIFO would queue
   * this one behind it and the next acquire or present wait for a frame
   * nobody sees -- every surface of the thread with it. Held back
   * instead: the painter keeps its changes, the callback's flush says
   * when (ctui_wl_dpy_flush()) */
  if (ctui_wl_frame_waiting(w->wl)) {
    return 0;
  }
  uint32_t img = 0;
  for (int attempt = 0;; attempt++) {
    if (w->stale) {
      ctui_logf(E_INF, "[CTUI:VK] - the swapchain went stale (%s)\n",
                w->stale == 2 ? "out of date" : "suboptimal");
      if (swapchain(w, w->sw, w->sh) != 0) {
        return -1;
      }
    }
    w->slot = (w->slot + 1) % SLOTS;
    if (w->pending[w->slot]) {
      vkWaitForFences(g->dev, 1, &w->fence[w->slot], VK_TRUE, UINT64_MAX);
      vkResetFences(g->dev, 1, &w->fence[w->slot]);
      w->pending[w->slot] = 0;
    }
    /* bounded: paced by the frame callbacks an image is free by now, so
     * a compositor holding them all mustn't hang the thread either */
    VkResult r =
        vkAcquireNextImageKHR(g->dev, w->sc, ACQUIRE_NS, w->acquired[w->slot],
                              VK_NULL_HANDLE, &img);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
      w->stale = r == VK_SUBOPTIMAL_KHR;
      break;
    }
    if (r == VK_TIMEOUT || r == VK_NOT_READY) {
      ctui_logf(E_WRN, "[CTUI:VK] - no image free within %d ms: frame held\n",
                (int)(ACQUIRE_NS / 1000000u));
      return 0;
    }
    w->stale = 2;
    if (r != VK_ERROR_OUT_OF_DATE_KHR || attempt) {
      ctui_logf(E_WRN, "[CTUI:VK] - no image to present (VkResult %d)\n",
                (int)r);
      return -1;
    }
  }
  VkCommandBuffer cmd = w->cmd[w->slot];
  VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  vkResetCommandBuffer(cmd, 0);
  vkBeginCommandBuffer(cmd, &bi);
  unsigned tiles = ctui_vk_record(w->vk, cmd);
  int fresh = !w->ready[img];
  image_barrier(cmd, w->img[img],
                fresh ? VK_IMAGE_LAYOUT_UNDEFINED
                      : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  unsigned n = ctui_vk_regions(w->vk, img, fresh, w->regions);
  if (n) {
    vkCmdCopyBufferToImage(cmd, ctui_vk_surface(w->vk), w->img[img],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, w->regions);
  }
  image_barrier(cmd, w->img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT,
                0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  vkEndCommandBuffer(cmd);
  VkPipelineStageFlags wait_at = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &w->acquired[w->slot],
      .pWaitDstStageMask = &wait_at,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &w->done[img],
  };
  if (vkQueueSubmit(g->queue, 1, &si, w->fence[w->slot]) != VK_SUCCESS) {
    ctui_logf(E_ERR, "[CTUI:VK] - a frame's submit failed\n");
    return -1;
  }
  w->pending[w->slot] = 1;
  w->ready[img] = 1;
  if (w->viewport) {
    ctui_wl_viewport(w->wl);
    w->viewport = 0;
  }
  VkPresentInfoKHR pi = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &w->done[img],
      .swapchainCount = 1,
      .pSwapchains = &w->sc,
      .pImageIndices = &img,
  };
  ctui_wl_frame(w->wl); /* on the commit the present makes */
  VkResult r = vkQueuePresentKHR(g->queue, &pi);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
    w->stale = r == VK_ERROR_OUT_OF_DATE_KHR ? 2 : 1;
  }
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
    /* no commit, so no callback: the next present asks again */
    ctui_wl_frame_cancel(w->wl);
  }
  ctui_logf(E_DBG,
            "[CTUI:VK] - presented image %u: %u tiles, %u regions%s @ tick "
            "%d\n",
            img, tiles, n, fresh ? " (fresh)" : "", ctui_tick_advance());
  return 1;
}
