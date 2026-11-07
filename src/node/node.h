#ifndef CTUI_NODE_H
#define CTUI_NODE_H

#include "json/buf.h"
#include "json/json.h"

#include <stddef.h>
#include <stdint.h>

/* Control nodes: an app's functions and parameters published as typed
 * values under a path ("volume/sink/level"), which a daemon serves as
 * files (ctui-wm's ctui-nodefs: $XDG_RUNTIME_DIR/nodes/<pid>/...). The
 * app keeps the whole table here; the daemon gets a copy over a unix
 * socket and hands writes back, one at a time, to the owner's callbacks
 * on the owner's own loop. Without a daemon nothing breaks: the table
 * waits and is published whole once one answers (ctui_nodes_connect()).
 *
 * The values' text (one per file) is strict, the same here and in the
 * daemon (ctui_node_parse()): a write that doesn't parse or is out of
 * range is refused with an errno, the value doesn't move.
 *
 *   int      "-12"          writes: decimal, "+=N" / "-=N"
 *   float    "0.1"          writes: decimal (exponent ok), "+=X" / "-=X"
 *   bool     "0" / "1"      writes: "0", "1", "toggle"
 *   enum     its label      writes: a label, "+=N" / "-=N" steps
 *   string   the text       writes: the text, UTF-8, no NUL
 *   trigger  (nothing)      writes: the argument; the event itself
 *
 * One trailing newline of a write is dropped (echo). The design and why:
 * ctui-wm's notes/nodes.md. */

typedef enum {
  CTUI_NODE_INT,     /* int64_t */
  CTUI_NODE_FLOAT,   /* double */
  CTUI_NODE_BOOL,
  CTUI_NODE_ENUM,    /* an index into labels[] */
  CTUI_NODE_STRING,  /* up to max_len bytes of UTF-8 */
  CTUI_NODE_TRIGGER, /* no value: a write is the event */
} CTUI_NODE_KIND;

typedef enum {
  CTUI_NODE_LINEAR,
  CTUI_NODE_LOG, /* what a knob or dial maps its travel onto */
} CTUI_NODE_SCALE;

enum {
  CTUI_NODE_WRITABLE = 1u << 0, /* outsiders may write it (ops needed) */
  CTUI_NODE_CLAMP = 1u << 1,    /* out of range is clamped, not refused */
};

/* the most a value's text (or a trigger's argument / reply) holds */
#define CTUI_NODE_TEXT_MAX 4096
/* path rules: components of 1-63 bytes of [A-Za-z0-9_.:+@-], not
 * starting with '.', up to 16 deep, up to 255 bytes in all */
#define CTUI_NODE_NAME_MAX 63
#define CTUI_NODE_DEPTH_MAX 16
#define CTUI_NODE_PATH_MAX 255
#define CTUI_NODE_LABELS_MAX 256

/* Usually a static const per kind of node; it must outlive every node
 * published with it. */
typedef struct {
  CTUI_NODE_KIND kind;
  const char *unit;        /* "dB", "Hz", "%", "px"; NULL: none */
  double min, max, def;    /* INT / FLOAT (min > max: no range); ENUM: def
                              is the default label's index; BOOL: def != 0 */
  CTUI_NODE_SCALE scale;   /* INT / FLOAT */
  const char *const *labels; /* ENUM: NULL-terminated, at least one */
  size_t max_len;          /* STRING / TRIGGER; 0: CTUI_NODE_TEXT_MAX */
  unsigned flags;          /* CTUI_NODE_WRITABLE, CTUI_NODE_CLAMP */
} CTUI_NODE_TYPE;

/* a value, resolved: what a callback gets and a set puts. s (STRING,
 * TRIGGER) is len bytes and NUL-terminated; in a callback it lives until
 * the callback returns. */
typedef struct {
  int64_t i;    /* INT */
  double f;     /* FLOAT */
  int b;        /* BOOL: 0 / 1 */
  unsigned e;   /* ENUM */
  const char *s;
  size_t len;
} CTUI_NODE_VALUE;

typedef struct CTUI_NODES CTUI_NODES;
typedef struct CTUI_NODE CTUI_NODE;

typedef struct {
  /* an outside write, resolved and range-checked (relative writes
   * against the node's value): 0 takes it, else an errno the writer gets
   * (EINVAL, EBUSY, ...). On 0 the node takes v, unless the callback set
   * the node itself. A trigger may answer into reply (what the writer
   * reads back on the same open file). */
  int (*on_write)(CTUI_NODE *n, const CTUI_NODE_VALUE *v, CTUI_BUF *reply,
                  void *user);
  /* optional: the same, without acting on it. A batch runs every line's
   * check before any line's write, so a batch whose checks pass applies
   * whole. */
  int (*on_check)(CTUI_NODE *n, const CTUI_NODE_VALUE *v, void *user);
} CTUI_NODE_OPS;

/* --- the text (shared with the daemon) ---------------------------------- */

/* 1 if t is usable: a known kind, labels for an enum, def in range, a
 * max_len within CTUI_NODE_TEXT_MAX, ops-free flags only */
int ctui_node_type_ok(const CTUI_NODE_TYPE *t);

/* 1 if path follows the rules above (reserved names not checked) */
int ctui_node_path_ok(const char *path);
/* 1 if path's first component is one the daemon keeps for itself
 * ("name", "events", "batch") */
int ctui_node_path_reserved(const char *path);

/* text (len bytes, one trailing newline dropped) as a write to a node of
 * type t whose value is cur (relative writes; NULL refuses them): 0 with
 * the result in *out (out->s points into text, not NUL-terminated for a
 * STRING / TRIGGER -- use out->len), or EINVAL (not t's grammar, not
 * UTF-8, too long) / ERANGE (out of range, unless CLAMP) */
int ctui_node_parse(const CTUI_NODE_TYPE *t, const char *text, size_t len,
                    const CTUI_NODE_VALUE *cur, CTUI_NODE_VALUE *out);

/* 1 if len bytes of s fit a STRING / TRIGGER of type t: its max_len,
 * UTF-8, no NUL (the owner's own strings, no newline dropped) */
int ctui_node_string_ok(const CTUI_NODE_TYPE *t, const char *s, size_t len);

/* v as t's text (no newline) onto b: what a read gives. A FLOAT is the
 * shortest text that reads back the same; a TRIGGER adds nothing. */
void ctui_node_format(const CTUI_NODE_TYPE *t, const CTUI_NODE_VALUE *v,
                      CTUI_BUF *b);

/* t as the wire's JSON object onto b, and back: ctui_node_type_from_json()
 * returns one allocation (its strings inside it; free() it), NULL when j
 * isn't a usable type */
void ctui_node_type_json(const CTUI_NODE_TYPE *t, CTUI_BUF *b);
CTUI_NODE_TYPE *ctui_node_type_from_json(const CTUI_JSON *j);

/* --- an app's table ------------------------------------------------------ */

enum {
  CTUI_NODES_SINGLE = 1u << 0, /* one instance only: the daemon's by-name/ */
};

/* The socket: $CTUI_NODES_SOCK, else $XDG_RUNTIME_DIR/ctui-nodes.sock.
 * open() tries to connect once; NULL only when out of memory. */
CTUI_NODES *ctui_nodes_open(const char *name, unsigned flags);
void ctui_nodes_close(CTUI_NODES *ns);

/* connects if it isn't (the table published whole on the way): 0
 * connected, -1 no daemon (call it again later, e.g. on a timer) */
int ctui_nodes_connect(CTUI_NODES *ns);
/* the socket to poll for reading (-1 while not connected; it changes
 * when the connection drops or comes back) */
int ctui_nodes_fd(const CTUI_NODES *ns);
/* 1 while output waits for the socket to take it: poll it for writing */
int ctui_nodes_pending(const CTUI_NODES *ns);
/* reads what came, runs the callbacks, sends what waits; a dropped
 * connection closes the fd (ctui_nodes_fd() -1 after) */
void ctui_nodes_dispatch(CTUI_NODES *ns);

/* path's node, its value t's def; NULL (logged) for a bad type or path,
 * a path taken, or one under / over another node (a file can't be a
 * directory). ops NULL: read-only (CTUI_NODE_WRITABLE ignored). */
CTUI_NODE *ctui_node_publish(CTUI_NODES *ns, const char *path,
                             const CTUI_NODE_TYPE *type,
                             const CTUI_NODE_OPS *ops, void *user);
CTUI_NODE *ctui_node_publishf(CTUI_NODES *ns, const CTUI_NODE_TYPE *type,
                              const CTUI_NODE_OPS *ops, void *user,
                              const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
void ctui_node_remove(CTUI_NODE *n);
/* every node at prefix or under it ("notify/42"); how many went */
int ctui_nodes_remove_under(CTUI_NODES *ns, const char *prefix);
CTUI_NODE *ctui_nodes_find(const CTUI_NODES *ns, const char *path);

/* the owner moves a value: refused (logged) when it isn't the node's
 * kind or is out of its range; the same value again sends nothing */
void ctui_node_set_int(CTUI_NODE *n, int64_t v);
void ctui_node_set_float(CTUI_NODE *n, double v);
void ctui_node_set_bool(CTUI_NODE *n, int v);
void ctui_node_set_enum(CTUI_NODE *n, unsigned index);
void ctui_node_set_str(CTUI_NODE *n, const char *s);

/* sets between them reach readers as one change (nests) */
void ctui_nodes_begin(CTUI_NODES *ns);
void ctui_nodes_commit(CTUI_NODES *ns);

const char *ctui_node_path(const CTUI_NODE *n);
const CTUI_NODE_TYPE *ctui_node_type(const CTUI_NODE *n);
const CTUI_NODE_VALUE *ctui_node_value(const CTUI_NODE *n);
/* the value as a read gives it (no newline) */
const char *ctui_node_text(const CTUI_NODE *n);
void *ctui_node_user(const CTUI_NODE *n);
int ctui_nodes_count(const CTUI_NODES *ns);

#endif
