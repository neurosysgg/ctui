# Event bubbling through `CTUI_SPLIT`/`CTUI_GROUP` — design plan

Status: **proposed, not started.** Written up after `ctui-mus`'s main
GUI (a separate app built on this library, `../ctui-mus`) hit the
problem this solves in practice — see "Case study" below for the real
code that motivated it. Nothing in this doc has landed; it's a plan to
implement against, in the same spirit `GFX_DESIGN.md` was written
before Phase 1 existed. See `CLAUDE.md` for the philosophy/conventions
this plan is written to follow, and `docs/core-api.md`'s Event
Registry section for how dispatch works today.

## Problem

`ctui_handle_event()` (`core/event.c`) is a flat, global broadcast:
every `ctui_event_register(source, type, widget, handler)` call fires
`handler(widget, ev)` for *every* event whose `(ev_source, ev->type)`
matches, regardless of whether `widget` is currently reachable in the
render tree at all. A handler registered against a `CTUI_SPLIT` child
that isn't one of the split's current `count` active children still
runs — the registry has no concept of "currently active branch," only
"currently registered handler." Two consequences, both already called
out as known gaps before this doc existed:

- **`PROGRESS.md`'s Known issues: "`CTUI_EVENT_SCOPE` is vestigial.
  Every event is effectively global; the enum exists but nothing
  branches on it."** `CTUI_EVENT_SCOPE_GLOBAL` is the only value
  `core/event.h` defines — the enum was clearly left room to grow, but
  nothing has ever grown into it.
- **`PROGRESS.md`'s Next up: "Per-instance event identity, if/when a
  second instance of the same widget kind needs to emit
  distinguishable events."** `event.h`'s own doc comment on
  `ctui_event_register()` already flags this: "two widget instances of
  the same kind currently can't be told apart by source alone." An
  event only carries a `source` *string* (`"list"`, `"navbar"`, ...),
  never a reference to which widget instance actually produced it, so
  two same-shaped widgets sharing a source string are indistinguishable
  to every listener.

Both gaps have the same root cause: the registry dispatches on
`(source, type)` alone, with no notion of *where in the tree* the
event came from or *where in the tree* a given handler's widget
currently lives. Bubbling — walking an event from the widget that
produced it up through its actual ancestors, the way `CTUI_SPLIT`/
`CTUI_GROUP` already nest widgets — fixes both at once: an ancestor
chain is exactly "where in the tree," and a specific origin widget is
exactly "which instance."

## Constraints going in

- **Stay minimal — this augments the registry, it doesn't replace
  it.** No capture phase, no `stopPropagation()`/`preventDefault()`
  machinery, no synthetic event re-dispatch. The generic mechanism
  `CTUI_SPLIT`/`CTUI_GROUP` already have — "which children are
  currently active" (`split->count`/`children[]`,
  `group->members`/`count`) — already contains everything bubbling
  needs to know; this plan reuses that, it doesn't add a second
  parallel tree.
- **Backward compatible.** Existing `ctui_event_register(source, type,
  widget, handler)` call sites and `CTUI_EVENT_SCOPE_GLOBAL` behavior
  must keep working unchanged — bubbling is opt-in per registration
  (`CTUI_EVENT_SCOPE_BUBBLE`, see Phase 2), not a forced migration.
  Every existing `examples_apps/*/main.c` should compile and behave
  identically without touching it.
- **Only `CTUI_KEYPRESS_EVENT`-shaped "raw input" events need a new
  concept (focus) to have an origin at all.** A widget-emitted event
  (`CTUI_VALUE_CHANGED_EVENT`, emitted from inside e.g.
  `ctui_list_handle_keypress()`) already has an obvious origin: the
  `self` the emitting function was called with. Raw input from
  `ctui_input_loop()` has no such thing — the terminal doesn't know
  which widget the user means to type into. These are different
  problems with different fixes; see Phase 1 vs. Phase 3 below. Don't
  conflate them into one flag.

## Design

### Phase 1 — parent pointers + an origin-carrying event

`CTUI_WIDGET` gains one new field:

```c
struct CTUI_WIDGET {
  ...
  CTUI_WIDGET *parent; /* set by ctui_split_layout()/ctui_group_init()
                        * when binding a child, NULL for any top-level
                        * widget (ctui_app_init() never sets it). The
                        * one piece of tree structure bubbling needs
                        * that nothing currently tracks -- parents
                        * already know their children (split->children,
                        * group->members), nothing walks the other
                        * direction today. */
};
```

`CTUI_EVENT` gains one new field:

```c
typedef struct {
  CTUI_EVENTTYPE type;
  CTUI_EVENT_SCOPE scope;
  const char *ev_source;
  void *event_data;
  CTUI_WIDGET *origin; /* NULL keeps today's global-broadcast behavior
                        * (CTUI_EVENT_SCOPE_GLOBAL doesn't read this at
                        * all); set to `self` by an emitter that wants
                        * CTUI_EVENT_SCOPE_BUBBLE semantics -- see
                        * ctui_list_handle_keypress()'s existing
                        * ctui_handle_event() call for the one-line
                        * change this needs at each of the ~6 emit
                        * sites (list/menu/navbar/grid's own keypress
                        * handlers). */
} CTUI_EVENT;
```

Both fields default to `NULL` via zero-fill, so every existing
`(CTUI_WIDGET){...}`/`(CTUI_EVENT){...}` literal across
`src/widgets/*.c` keeps compiling unchanged — same "extend, don't
replace" approach `GFX_DESIGN.md`'s Phase 1 already used for
`CTUI_CELL`.

### Phase 2 — `CTUI_EVENT_SCOPE_BUBBLE` + ancestor-walking dispatch

```c
typedef enum {
  CTUI_EVENT_SCOPE_GLOBAL, /* today's behavior, unchanged: every
                            * matching (source, type) handler runs,
                            * origin ignored even if set */
  CTUI_EVENT_SCOPE_BUBBLE, /* only handlers registered on ev->origin
                            * itself, or on one of its ancestors via
                            * parent, run -- and only if every
                            * CTUI_SPLIT/CTUI_GROUP step of that walk
                            * currently has the child on the active
                            * path (see Phase 3) */
} CTUI_EVENT_SCOPE;
```

`ctui_handle_event()` grows one new branch: for `CTUI_EVENT_SCOPE_
BUBBLE`, instead of scanning every registered handler for a
`(source, type)` match, walk `ev->origin`, then `ev->origin->parent`,
then *its* `->parent`, etc. up to `NULL`, and at each widget in that
chain run only the handlers registered against `(source, type,
that_widget)`. Registration itself (`ctui_event_register()`) doesn't
change shape — a handler is still tied to one `(source, type, widget)`
triple, same as today; what changes is which triples `ctui_handle_
event()` considers reachable for a given event.

This directly fixes the second `PROGRESS.md` gap: two `CTUI_LIST`
instances (`ctui-mus`'s library and playlist views, concretely) each
emit `ev_source = "list"` today, and a handler registered against
*either* instance currently fires for *both* (`event.h`'s documented
limitation). With `origin` set to `self` at the emit site and
`CTUI_EVENT_SCOPE_BUBBLE` on the registration, a handler registered
against `library_list_widget` only ever runs when `ev->origin ==
library_list_widget` (or a bubbled ancestor match) — the two
instances stop colliding, without needing a manufactured per-instance
id, string-suffix convention, or any change to how many widgets exist.

### Phase 3 — active-branch pruning at `CTUI_SPLIT`/`CTUI_GROUP`

The walk in Phase 2 isn't just "does a parent pointer chain exist" —
it has to agree with what's actually on screen right now. A
`CTUI_SPLIT` already tracks this exactly: `split->children[0..count)`
are active, `children[count..]` (if the backing array is longer,
per `split.h`'s own doc comment on why it may be) are not. Bubbling
adds one check at each `CTUI_SPLIT`/`CTUI_GROUP` step of the ancestor
walk: the child being bubbled *through* must currently be one of that
parent's active children (present in `children[0..count)` for a
split, `members[0..count)` for a group) — otherwise the walk stops
there and the event never reaches that ancestor's (or further
ancestors') handlers.

This is the mechanism that erases the first `PROGRESS.md` gap in
practice: an event bubbling from a widget nested inside a `CTUI_SPLIT_
GRID` cell that *isn't currently tiled* (present in the split's
backing array, but past `count`) simply stops at that split — no
handler downstream of it runs, automatically, with zero app-level code
checking "is this view currently visible" by hand.

## Case study: the exact problem this replaces (`ctui-mus/src/gui/main.c`)

`ctui-mus` (a separate app built on this library) added multi-select
grid tiling to its navbar — any combination of its three views
(library/playlist/viz) can be on screen at once, tiled via
`CTUI_SPLIT_GRID`. That surfaced precisely the gap this doc describes,
worked around entirely at the app level because the mechanism above
doesn't exist yet:

- **`list_keypress_gate()`/`playlist_keypress_gate()`** each manually
  call a hand-written `nav_item_enabled(label)` helper that reaches
  into `g_navbar_widget->widget_data` to check whether their own view
  is currently one of the split's active children, before deciding
  whether to even look at a keypress. Under Phase 3, a `CTUI_LIST`
  widget nested in a currently-inactive grid cell would simply never
  see the event bubble reach it — no helper, no cross-widget pointer
  read, no risk of the check drifting out of sync with the split's
  actual `count`.
- **`playlist_keypress_gate()`** additionally has to explicitly
  swallow `CTUI_KEY_ENTER` outright, specifically to avoid
  `list_handle_value_changed()` (registered against the *library*
  `list_widget`) firing with playlist's item data instead — the exact
  "two instances, one source string" collision Phase 2 fixes directly.
  Today's workaround is a real, live comment on that function citing
  `event.h`'s own documented limitation as the reason.
- **`navbar_keypress_gate()`** exists purely to keep the navbar's own
  `ENTER`-driven toggle from also reaching whichever list views happen
  to already be visible — remapping the navbar's toggle key to `SPACE`
  and refusing to forward a real `ENTER` to `ctui_navbar_handle_
  keypress()` at all. That's a global-broadcast-vs-instance-targeting
  problem too, though on the *input* side (Phase 1's carve-out: raw
  keypresses have no origin widget without a focus concept) — out of
  this doc's scope (see Deferred), but the same family of problem.

None of this is a criticism of the workaround — it's the correct fix
*today*, matching the library's actual capabilities. It's included
here as the concrete motivating case for why the mechanism above is
worth building: three separate hand-rolled gates, in one `main.c`,
all reimplementing some slice of "only handle this event if I'm
actually reachable from here" that `CTUI_SPLIT_GRID` already knows the
answer to.

## Deferred (not this pass)

- **Focus for raw input events.** `CTUI_KEYPRESS_EVENT`'s `ev_source`
  is `"input"`, produced by `ctui_input_loop()` with no widget
  reference at all — bubbling needs an `origin` to walk from, and
  nothing today tracks "which widget currently owns keyboard focus."
  A real fix for `ctui-mus`'s `navbar_keypress_gate()` workaround needs
  this: a `ctui_focus_set(widget)`/`ctui_focus_get()` pair, `TAB`-style
  focus movement, and `ctui_input_loop()` setting `origin` to the
  focused widget before calling `ctui_handle_event()`. Real, but a
  separate design in its own right (focus movement order through a
  dynamic `CTUI_SPLIT_GRID`, what happens to focus when the focused
  widget's tile disappears, ...) — not bundled into this plan.
- **Handler unregistration.** Already a known gap
  (`PROGRESS.md`: "No handler unregistration") independent of
  bubbling; a widget that comes and goes dynamically (e.g. a future
  closable tile) would want both eventually, but bubbling's active-
  branch pruning (Phase 3) already makes a still-registered handler on
  a now-inactive widget harmless (it simply never fires), which covers
  `ctui-mus`'s actual need without requiring unregistration too.
- **`CTUI_FOCUS_EVENT`.** Declared and named
  (`ctui_eventtype_name()`) but never emitted, per `PROGRESS.md`.
  A focus system (see above) is the obvious first producer of it, once
  built.

## Blast radius summary

- `src/core/widget.h` / `widget.c` — new `parent` field, set by
  `ctui_split_layout()`/`ctui_group_init()` (both currently `core/
  split.c`/`core/group.c`) rather than `ctui_widget_init()` itself,
  since only a split/group actually knows a child's parent.
- `src/core/event.h` — new `origin` field on `CTUI_EVENT`, new
  `CTUI_EVENT_SCOPE_BUBBLE` value.
- `src/core/event.c` — `ctui_handle_event()` gains the ancestor-walk
  branch for `CTUI_EVENT_SCOPE_BUBBLE`; `CTUI_EVENT_SCOPE_GLOBAL` path
  unchanged.
- `src/core/split.c` / `src/core/group.c` — set `child->parent = self`
  alongside the existing `ctui_widget_init(child, comp)` calls.
- `src/widgets/list.c`/`menu.c`/`navbar.c`/`grid.c` — each keypress
  handler's existing `ctui_handle_event()` call sets `.origin = self`
  in the `CTUI_EVENT` literal it already builds; one line each, no
  signature changes.
- Zero required changes in any app's `main.c` — `CTUI_EVENT_SCOPE_
  GLOBAL` (every existing registration) behaves identically. Adopting
  bubbling anywhere is opt-in, one call site at a time.
