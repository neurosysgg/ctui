/* must precede every #include -- clock_gettime() is POSIX, not C11 (see
 * term.c's identical comment for why this has to come before the first
 * system header, even a transitive one). */
#define _POSIX_C_SOURCE 200809L

#include "timer.h"

#include "log.h"

#include <stdlib.h>
#include <time.h>

/* one registered (widget, handler) pair. Independent timers own
 * next_fire_ms outright; synchronized ones leave it unused and defer to
 * group->next_fire_ms instead, so every member of a group fires off the
 * exact same deadline rather than N deadlines that happen to start equal
 * and then drift apart. */
struct CTUI_TIMER {
  CTUI_WIDGET *widget;
  int (*handler)(CTUI_WIDGET *self, CTUI_EVENT *ev);
  int duration_ms;
  long next_fire_ms;
  struct CTUI_TIMER_GROUP *group; /* NULL for independent timers */
};

typedef struct CTUI_TIMER_GROUP {
  int duration_ms;
  long next_fire_ms;
  CTUI_TIMER **members;
  int member_count, member_cap;
} CTUI_TIMER_GROUP;

static CTUI_TIMER **g_independent;
static int g_independent_count, g_independent_cap;

static CTUI_TIMER_GROUP **g_groups;
static int g_group_count, g_group_cap;

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static CTUI_TIMER_GROUP *find_or_create_group(int duration_ms) {
  for (int i = 0; i < g_group_count; i++) {
    if (g_groups[i]->duration_ms == duration_ms) {
      return g_groups[i];
    }
  }

  if (g_group_count == g_group_cap) {
    int new_cap = g_group_cap == 0 ? 4 : g_group_cap * 2;
    g_groups = realloc(g_groups, (size_t)new_cap * sizeof(*g_groups));
    g_group_cap = new_cap;
  }

  CTUI_TIMER_GROUP *group = malloc(sizeof(*group));
  group->duration_ms = duration_ms;
  group->next_fire_ms = now_ms() + duration_ms;
  group->members = NULL;
  group->member_count = 0;
  group->member_cap = 0;
  g_groups[g_group_count++] = group;

  ctui_logf(E_INF,
            "[CTUI:TIMER] - new synchronized group @ tick %d "
            "(duration=%dms)\n",
            ctui_tick_advance(), duration_ms);
  return group;
}

/* shared by both public register functions below; group is NULL for an
 * independent timer (which then owns next_fire_ms outright) or the shared
 * group a synchronized timer is joining (which leaves next_fire_ms unused
 * in favor of group->next_fire_ms -- see the CTUI_TIMER field comment) */
static CTUI_TIMER *make_timer(int duration_ms, CTUI_WIDGET *widget,
                              int (*handler)(CTUI_WIDGET *self,
                                             CTUI_EVENT *ev),
                              CTUI_TIMER_GROUP *group) {
  CTUI_TIMER *timer = malloc(sizeof(*timer));
  timer->widget = widget;
  timer->handler = handler;
  timer->duration_ms = duration_ms;
  timer->group = group;
  timer->next_fire_ms = group ? 0 : now_ms() + duration_ms;
  return timer;
}

CTUI_TIMER *ctui_timer_register_synchronized(
    int duration_ms, CTUI_WIDGET *widget,
    int (*handler)(CTUI_WIDGET *self, CTUI_EVENT *ev)) {
  CTUI_TIMER_GROUP *group = find_or_create_group(duration_ms);
  CTUI_TIMER *timer = make_timer(duration_ms, widget, handler, group);

  if (group->member_count == group->member_cap) {
    int new_cap = group->member_cap == 0 ? 4 : group->member_cap * 2;
    group->members =
        realloc(group->members, (size_t)new_cap * sizeof(*group->members));
    group->member_cap = new_cap;
  }
  group->members[group->member_count++] = timer;

  ctui_logf(E_INF,
            "[CTUI:TIMER] - registered synchronized timer @ tick %d "
            "(widget=%p, duration=%dms, group size=%d)\n",
            ctui_tick_advance(), (void *)widget, duration_ms,
            group->member_count);
  return timer;
}

CTUI_TIMER *ctui_timer_register(int duration_ms, CTUI_WIDGET *widget,
                                int (*handler)(CTUI_WIDGET *self,
                                               CTUI_EVENT *ev)) {
  CTUI_TIMER *timer = make_timer(duration_ms, widget, handler, NULL);

  if (g_independent_count == g_independent_cap) {
    int new_cap = g_independent_cap == 0 ? 4 : g_independent_cap * 2;
    g_independent =
        realloc(g_independent, (size_t)new_cap * sizeof(*g_independent));
    g_independent_cap = new_cap;
  }
  g_independent[g_independent_count++] = timer;

  ctui_logf(E_INF,
            "[CTUI:TIMER] - registered independent timer @ tick %d "
            "(widget=%p, duration=%dms)\n",
            ctui_tick_advance(), (void *)widget, duration_ms);
  return timer;
}

static int fire(CTUI_TIMER *timer) {
  CTUI_EVENT ev = {.type = CTUI_TIMER_EVENT,
                   .scope = CTUI_EVENT_SCOPE_GLOBAL,
                   .ev_source = "timer",
                   .event_data = NULL};
  return timer->handler(timer->widget, &ev);
}

int ctui_timer_tick(void) {
  long now = now_ms();
  int changed = 0;

  for (int i = 0; i < g_group_count; i++) {
    CTUI_TIMER_GROUP *group = g_groups[i];
    if (now < group->next_fire_ms) {
      continue;
    }
    ctui_logf(E_INF,
              "[CTUI:TIMER] - synchronized group firing @ tick %d "
              "(duration=%dms, %d members)\n",
              ctui_tick_advance(), group->duration_ms, group->member_count);
    for (int m = 0; m < group->member_count; m++) {
      if (fire(group->members[m])) {
        changed = 1;
      }
    }
    group->next_fire_ms = now + group->duration_ms;
  }

  for (int i = 0; i < g_independent_count; i++) {
    CTUI_TIMER *timer = g_independent[i];
    if (now < timer->next_fire_ms) {
      continue;
    }
    ctui_logf(E_INF,
              "[CTUI:TIMER] - independent timer firing @ tick %d "
              "(widget=%p, duration=%dms)\n",
              ctui_tick_advance(), (void *)timer->widget, timer->duration_ms);
    if (fire(timer)) {
      changed = 1;
    }
    timer->next_fire_ms = now + timer->duration_ms;
  }

  return changed;
}

void ctui_timer_reset(void) {
  for (int i = 0; i < g_group_count; i++) {
    for (int m = 0; m < g_groups[i]->member_count; m++) {
      free(g_groups[i]->members[m]);
    }
    free(g_groups[i]->members);
    free(g_groups[i]);
  }
  free(g_groups);
  g_groups = NULL;
  g_group_count = 0;
  g_group_cap = 0;

  for (int i = 0; i < g_independent_count; i++) {
    free(g_independent[i]);
  }
  free(g_independent);
  g_independent = NULL;
  g_independent_count = 0;
  g_independent_cap = 0;

  ctui_logf(E_INF, "[CTUI:TIMER] - registry reset @ tick %d\n",
            ctui_tick_advance());
}
