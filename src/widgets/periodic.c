#include "periodic.h"

CTUI_TIMER *ctui_periodic_register(CTUI_WIDGET *widget, int duration_ms,
                                   int synchronized,
                                   int (*handler)(CTUI_WIDGET *self,
                                                  CTUI_EVENT *ev)) {
  return synchronized
             ? ctui_timer_register_synchronized(duration_ms, widget, handler)
             : ctui_timer_register(duration_ms, widget, handler);
}
