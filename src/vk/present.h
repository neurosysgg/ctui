#ifndef CTUI_VK_PRESENT_H
#define CTUI_VK_PRESENT_H

/* ctui's frames presented to Wayland surfaces (src/wl/): a device for
 * them all (CTUI_VK_GPU: the instance, one queue that computes and
 * presents, the command pool) and a presenter per surface (CTUI_VK_WIN:
 * its swapchain, its recordings, its painter, a CTUI_VK). One process
 * paints any number of surfaces on one device -- on nvidia a device costs
 * a process ~23 MB private and 5 MB of VRAM (ctui-wm's
 * notes/kittyless.md), so a desktop's panels share one.
 *
 * A presenter presents only when its painter has something
 * (ctui_vk_pending()), FIFO, and only once the compositor took its last
 * frame (its frame callback, src/wl's ctui_wl_frame()): an idle surface
 * costs nothing, and one the compositor doesn't show (under a fullscreen
 * window) holds its frames instead of blocking the thread. Built from
 * ctui-mus's window host (host.h is that host now, on top of this).
 * Needs src/wl/ and Vulkan's Wayland surfaces. */

#include "vk.h"
#include "wl/wl.h"

typedef struct CTUI_VK_GPU CTUI_VK_GPU;
typedef struct CTUI_VK_WIN CTUI_VK_WIN;

/* the device for d's surfaces (a discrete card first). Lavapipe isn't
 * loaded unless nothing else presents, or the user picked drivers
 * (VK_DRIVER_FILES, VK_ICD_FILENAMES, VK_LOADER_DRIVERS_*): it costs a
 * process ~33 MB PSS for nothing. CTUI_HOST_VALIDATION=1: Khronos's
 * validation layer, its warnings logged and counted. NULL, or why in
 * err */
CTUI_VK_GPU *ctui_vk_gpu_open(CTUI_WL_DPY *d, char *err, unsigned n);
/* its presenters freed first */
void ctui_vk_gpu_close(CTUI_VK_GPU *g);
/* the validation layer's warnings so far (0 without it) */
unsigned ctui_vk_gpu_warnings(const CTUI_VK_GPU *g);

/* a presenter for wl (configured: a window, a layer surface) at its
 * size; *rows / *cols its grid. The painter is sized at once, the
 * swapchain made at the first present (the driver's slow part: 3-13 ms
 * on nvidia), so a caller can hand the grid to its app first and the
 * app draws meanwhile. NULL, or why in err */
CTUI_VK_WIN *ctui_vk_win_new(CTUI_VK_GPU *g, CTUI_WL *wl, int *rows, int *cols,
                             char *err, unsigned n);
/* waits for its frames first */
void ctui_vk_win_free(CTUI_VK_WIN *w);
CTUI_VK *ctui_vk_win_vk(CTUI_VK_WIN *w);
CTUI_WL *ctui_vk_win_wl(CTUI_VK_WIN *w);

/* the surface resized (ctui_wl_size() said so): painter and grid at its
 * new size, the swapchain made again at the next present, which repaints
 * it all. 0, or -1 */
int ctui_vk_win_resize(CTUI_VK_WIN *w, int *rows, int *cols);

/* the painter's changes, if it has any, recorded, copied into an image
 * and presented; a swapchain made first when due (new, resized, stale). Moving wipes are
 * where now_ns puts them (ctui_vk_at()); *next_ns, when it isn't NULL,
 * gets when the next column is due (0: nothing moves). 1 presented, 0
 * nothing to or held (the last frame not taken yet: again when
 * ctui_wl_dpy_flush() says a callback came; no image free within 100
 * ms), -1 no swapchain or image to present to (logged) */
int ctui_vk_win_present(CTUI_VK_WIN *w, uint64_t now_ns, uint64_t *next_ns);

/* the swapchain's composite alpha premultiplied: the painter's opacity
 * (ctui_vk_set_opacity()) below 255 and the surface offering it. The
 * swapchain is made again at the next present when the opacity crosses
 * 255; until then this is the last one's */
int ctui_vk_win_see_through(const CTUI_VK_WIN *w);

#endif
