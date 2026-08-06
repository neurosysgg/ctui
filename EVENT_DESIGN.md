# Event bubbling through `CTUI_SPLIT`/`CTUI_GROUP` — design plan

Status: **implemented (Phases 1-3).** Open questions below are resolved;
`src/core/widget.h`/`event.h`/`event.c`/`split.c`/`group.h`/`group.c`
and the four emit sites now match this doc. Written up after `ctui-mus`'s main
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

## Open questions, resolved

The plan above left four things underspecified once actually implemented
against the real `core/group.c`/`split.c`. Resolved here before writing
any code, so Design below reflects what actually got built rather than
the first draft.

1. **`ctui_group_init(group, comp)` has no `self` to assign as
   `child->parent` — unlike `ctui_split_layout(self, comp)`, which
   *is* called with the split's own wrapping widget.** `CTUI_GROUP` is
   never itself wrapped in a `CTUI_WIDGET` in current usage (it's driven
   standalone, alongside `ctui_app_render()`, not through
   `app->widgets[]`) — the original Blast-radius bullet's "set
   `child->parent = self`" doesn't type-check for group.c as written.
   **Resolution:** `CTUI_GROUP` gains its own `CTUI_WIDGET *parent`
   field (mirrors `CTUI_SPLIT.comp` — a plain struct field the caller
   sets directly, no setter function, same as `split->count`/
   `children[]` are already poked directly by app code). Defaults `NULL`
   via zero-fill (`ctui_group_make()`'s designated initializer, same
   "extend, don't replace" precedent `CTUI_WIDGET`/`CTUI_EVENT` already
   used). `ctui_group_init()` sets `member[i].parent = group->parent`
   for every member — not just `members[0]` — since bubbling needs an
   ancestor chain from whichever member actually emitted, not just the
   one `buf` gets derived from. No signature change to
   `ctui_group_make()`/`ctui_group_init()`.

2. **Given a `child->parent` pointer, how does the ancestor walk know
   whether that parent is a `CTUI_SPLIT` (has an active/inactive
   distinction) or a `CTUI_GROUP` (doesn't) — `widget_data` is `void*`,
   untyped?** Adding a "kind" tag to `CTUI_WIDGET` was one option,
   but a container-membership *check function* is more minimal and
   avoids event.c needing to know two different struct shapes.
   **Resolution:** `CTUI_WIDGET` gains
   `int (*is_active_child)(CTUI_WIDGET *parent, CTUI_WIDGET *child)`,
   `NULL` by default. `ctui_split_layout()` sets it (on `self`, once
   per call, alongside `child->parent = self`) to a `static` checker in
   `split.c` that scans `split->children[0..count)` for `child`.
   `group.c` never sets it — a `NULL` check means "always active,"
   which is simply correct for `CTUI_GROUP`: it has no `count`-style
   active/inactive subset at all, every member is always active, so
   Phase 3 pruning is *only* ever observable at a `CTUI_SPLIT` step in
   practice. That matches the case study, which is entirely about
   `CTUI_SPLIT_GRID`, not groups.

3. **Does an event's own `origin` widget get pruned by its own
   active-state, or only ancestors beyond it?** Doc text ("no handler
   *downstream* of it runs") implies the latter but doesn't say so
   explicitly. **Resolution:** `origin`'s own registered handlers
   *always* run, unconditionally — that's a direct, exact-instance
   match, not something bubbling should second-guess. The
   `is_active_child` check applies at each *hop* from a widget to its
   parent, gating whether the walk continues (and thus whether that
   parent's, and any further ancestor's, handlers get a chance) — never
   whether the widget the walk is currently sitting on gets to run its
   own. Concretely: `ctui_handle_event()` runs origin's handlers first,
   *then* checks `origin`-in-`origin->parent`; only on success does the
   walk move up and repeat.

4. **Where do the two new `CTUI_WIDGET` fields go, given `widget.h`'s
   `_Static_assert` guards that `gfx_render` is the last field (so
   `ctui_widget_make()` can't silently forget to init one)?**
   **Resolution:** append `parent`/`is_active_child` after `gfx_render`
   and move the `_Static_assert` to cover `is_active_child` instead —
   inserting earlier would silently defeat the assert per its own
   documented caveat.

One more case the original phases didn't call out: `ctui_handle_event()`
called with `scope == CTUI_EVENT_SCOPE_BUBBLE` and `ev->origin == NULL`
(a caller opted into bubbling but forgot to set the one field it needs)
logs `E_WRN` and drops the event — same reject-and-log convention as
`g_app == NULL`, not a crash.

(Small doc fix along the way: the Blast-radius section originally said
"~6 emit sites"; the actual count in this repo is 4 —
`list.c`/`menu.c`/`navbar.c`/`grid.c`. `ctui-mus`'s own widgets aren't
part of this repo's blast radius.)

## Design

### Phase 1 — parent pointers + an origin-carrying event

`CTUI_WIDGET` gains two new fields, appended after `gfx_render` (see
Resolved open question 4):

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
  int (*is_active_child)(CTUI_WIDGET *parent, CTUI_WIDGET *child);
                        /* set on a PARENT widget (self) by
                        * ctui_split_layout(), never by ctui_group_init()
                        * -- NULL means "always active," which is exactly
                        * right both for a plain widget (nothing ever
                        * walks up INTO one, since only split/group set
                        * a child's parent) and for CTUI_GROUP (no
                        * active/inactive concept at all). See Resolved
                        * open question 2. */
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
                        * change this needs at each of the 4 emit
                        * sites (list/menu/navbar/grid's own keypress
                        * handlers). */
} CTUI_EVENT;
```

All three fields default to `NULL` via zero-fill, so every existing
`(CTUI_WIDGET){...}`/`(CTUI_EVENT){...}` literal across
`src/widgets/*.c` keeps compiling unchanged — same "extend, don't
replace" approach `GFX_DESIGN.md`'s Phase 1 already used for
`CTUI_CELL`. `ctui_widget_make()`'s own struct literal is updated by
hand to init both new fields explicitly (`NULL`), same as every other
field it already sets — the `_Static_assert` only catches an omission
if the new field is *last*, and now it is.

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
that_widget)`. `ev->origin`'s own handlers always run — the
`is_active_child` check (Phase 3) only ever gates whether the walk is
allowed to *continue past* the widget it's currently on, never whether
that widget's own handlers fire (Resolved open question 3).
Registration itself (`ctui_event_register()`) doesn't
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
adds one check at each step of the ancestor walk where the parent has
a non-`NULL` `is_active_child` — in practice, only ever a
`CTUI_SPLIT` (Resolved open question 2: `CTUI_GROUP` leaves it `NULL`,
since a group has no active/inactive subset to check — every member is
always active, so a group step never prunes). The child being bubbled
*through* must currently be one of that split's active children
(present in `children[0..count)`) — otherwise the walk stops there and
the event never reaches that split's (or further ancestors') handlers,
though the child's own handlers, already run before this check,
still fired (open question 3).

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

- `src/core/widget.h` / `widget.c` — new `parent` and `is_active_child`
  fields, appended after `gfx_render` (open question 4); `parent` set
  by `ctui_split_layout()`/`ctui_group_init()` (both currently `core/
  split.c`/`core/group.c`) rather than `ctui_widget_init()` itself,
  since only a split/group actually knows a child's parent.
  `is_active_child` set only by `ctui_split_layout()` (open question 2).
- `src/core/event.h` — new `origin` field on `CTUI_EVENT`, new
  `CTUI_EVENT_SCOPE_BUBBLE` value.
- `src/core/event.c` — `ctui_handle_event()` gains the ancestor-walk
  branch for `CTUI_EVENT_SCOPE_BUBBLE`; `CTUI_EVENT_SCOPE_GLOBAL` path
  unchanged.
- `src/core/split.c` — set `child->parent = self` and
  `self->is_active_child = <static split checker>` alongside the
  existing `ctui_widget_init(child, comp)` calls, for every active
  child in each of the three modes (V/H/GRID).
- `src/core/group.h` / `group.c` — new `CTUI_WIDGET *parent` field on
  `CTUI_GROUP` itself (open question 1), set by the caller before
  `ctui_group_init()`, not a function parameter; `ctui_group_init()`
  sets `members[i].parent = group->parent` for every member.
- `src/widgets/list.c`/`menu.c`/`navbar.c`/`grid.c` — each keypress
  handler's existing `ctui_handle_event()` call sets `.origin = self`
  in the `CTUI_EVENT` literal it already builds; one line each, no
  signature changes. `.scope` stays `CTUI_EVENT_SCOPE_GLOBAL` at all
  four sites — setting `origin` is harmless under `GLOBAL` (ignored)
  and lets any future registration opt into `BUBBLE` without touching
  the emit site again.
- Zero required changes in any app's `main.c` — `CTUI_EVENT_SCOPE_
  GLOBAL` (every existing registration) behaves identically. Adopting
  bubbling anywhere is opt-in, one call site at a time.
