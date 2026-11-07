#define _GNU_SOURCE /* MSG_NOSIGNAL, SOCK_CLOEXEC */

#include "node.h"

#include "../ctui.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* output the daemon doesn't take, and a line from it, beyond these: the
 * connection is dropped (a daemon that hung; a broken one) */
#define OUT_MAX (8u << 20)
#define LINE_MAX_BYTES (1u << 20)

struct CTUI_NODE {
  CTUI_NODES *ns;
  uint32_t id; /* never reused: a write in flight can't reach a newer node */
  char *path;
  const CTUI_NODE_TYPE *type;
  const CTUI_NODE_OPS *ops;
  void *user;
  CTUI_NODE_VALUE v; /* v.s: str */
  char *str;
  char *text;     /* v as a read gives it */
  unsigned gen;   /* bumped by every change: did a callback set it? */
  int busy, dead; /* in a callback; removed in one (freed after) */
};

struct CTUI_NODES {
  char *name;
  unsigned flags;
  int fd;
  CTUI_NODE **by_id;   /* ids ascending (appends keep it so) */
  CTUI_NODE **by_path; /* strcmp order */
  int n, cap;
  uint32_t next_id;
  int depth; /* begin / commit */
  CTUI_BUF out;
  size_t out_off;
  char *in;
  size_t in_len, in_cap;
};

/* --- the connection ------------------------------------------------------ */

static void disconnect(CTUI_NODES *ns, const char *why) {
  if (ns->fd < 0) {
    return;
  }
  ctui_logf(E_WRN, "[CTUI:NODE] - %s: daemon connection dropped (%s) @ tick %d\n",
            ns->name, why, ctui_tick_advance());
  close(ns->fd);
  ns->fd = -1;
  free(ns->out.s);
  ns->out = (CTUI_BUF){0};
  ns->out_off = 0;
  ns->in_len = 0;
}

static void flush_out(CTUI_NODES *ns) {
  while (ns->fd >= 0 && ns->out_off < ns->out.len) {
    ssize_t w = send(ns->fd, ns->out.s + ns->out_off,
                     ns->out.len - ns->out_off, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        disconnect(ns, strerror(errno));
      }
      break;
    }
    ns->out_off += (size_t)w;
  }
  if (ns->fd < 0 || ns->out_off < ns->out.len) {
    if (ns->fd >= 0 && ns->out.len - ns->out_off > OUT_MAX) {
      disconnect(ns, "output not taken");
    }
    return;
  }
  ns->out.len = 0;
  ns->out_off = 0;
  if (ns->out.s) {
    ns->out.s[0] = '\0';
  }
}

/* line ends, unsent, in b: queued and sent as far as the socket takes */
static void send_buf(CTUI_NODES *ns, CTUI_BUF *b) {
  if (ns->fd >= 0 && b->s) {
    ctui_buf_add(&ns->out, b->s, b->len);
    ctui_buf_add(&ns->out, "\n", 1);
    flush_out(ns);
  }
  free(b->s);
  *b = (CTUI_BUF){0};
}

static void send_line(CTUI_NODES *ns, const char *line) {
  CTUI_BUF b = {0};
  ctui_buf_add(&b, line, strlen(line));
  send_buf(ns, &b);
}

static void send_pub(CTUI_NODES *ns, const CTUI_NODE *n) {
  CTUI_BUF b = {0};
  ctui_buf_printf(&b, "{\"pub\":%u,\"path\":", n->id);
  ctui_json_quote(&b, n->path);
  ctui_buf_add(&b, ",\"type\":", 8);
  /* writable only with someone to take the writes */
  CTUI_NODE_TYPE t = *n->type;
  if (!n->ops || !n->ops->on_write) {
    t.flags &= ~(unsigned)CTUI_NODE_WRITABLE;
  }
  ctui_node_type_json(&t, &b);
  if (n->type->kind != CTUI_NODE_TRIGGER) {
    ctui_buf_add(&b, ",\"v\":", 5);
    ctui_json_quote(&b, n->text);
  }
  ctui_buf_add(&b, "}", 1);
  send_buf(ns, &b);
}

static const char *socket_path(char *buf, size_t cap) {
  const char *env = getenv("CTUI_NODES_SOCK");
  if (env && *env) {
    return env;
  }
  const char *dir = getenv("XDG_RUNTIME_DIR");
  if (!dir || !*dir) {
    return NULL;
  }
  if ((size_t)snprintf(buf, cap, "%s/ctui-nodes.sock", dir) >= cap) {
    return NULL;
  }
  return buf;
}

int ctui_nodes_connect(CTUI_NODES *ns) {
  if (ns->fd >= 0) {
    return 0;
  }
  char buf[sizeof(((struct sockaddr_un *)0)->sun_path)];
  const char *path = socket_path(buf, sizeof buf);
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  if (!path || strlen(path) >= sizeof addr.sun_path) {
    return -1;
  }
  memcpy(addr.sun_path, path, strlen(path) + 1);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  /* a unix socket connects at once or not at all (EAGAIN: its backlog
   * is full -- try later like no daemon) */
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  ns->fd = fd;
  ctui_logf(E_INF, "[CTUI:NODE] - %s: connected to %s, %d nodes @ tick %d\n",
            ns->name, path, ns->n, ctui_tick_advance());

  CTUI_BUF b = {0};
  ctui_buf_add(&b, "{\"hello\":", 9);
  ctui_json_quote(&b, ns->name);
  ctui_buf_printf(&b, ",\"v\":1%s}",
                  ns->flags & CTUI_NODES_SINGLE ? ",\"single\":true" : "");
  send_buf(ns, &b);
  if (ns->depth) {
    send_line(ns, "{\"begin\":1}");
  }
  for (int i = 0; i < ns->n && ns->fd >= 0; i++) {
    send_pub(ns, ns->by_id[i]);
  }
  return ns->fd >= 0 ? 0 : -1;
}

int ctui_nodes_fd(const CTUI_NODES *ns) { return ns->fd; }

int ctui_nodes_pending(const CTUI_NODES *ns) {
  return ns->fd >= 0 && ns->out_off < ns->out.len;
}

CTUI_NODES *ctui_nodes_open(const char *name, unsigned flags) {
  CTUI_NODES *ns = calloc(1, sizeof *ns);
  if (!ns || !(ns->name = strdup(name))) {
    free(ns);
    return NULL;
  }
  ns->flags = flags;
  ns->fd = -1;
  ns->next_id = 1;
  ctui_nodes_connect(ns);
  return ns;
}

static void node_free(CTUI_NODE *n) {
  free(n->path);
  free(n->str);
  free(n->text);
  free(n);
}

void ctui_nodes_close(CTUI_NODES *ns) {
  if (!ns) {
    return;
  }
  if (ns->fd >= 0) {
    close(ns->fd);
  }
  for (int i = 0; i < ns->n; i++) {
    node_free(ns->by_id[i]);
  }
  free(ns->by_id);
  free(ns->by_path);
  free(ns->out.s);
  free(ns->in);
  free(ns->name);
  free(ns);
}

/* --- the table ----------------------------------------------------------- */

/* where path is or would go in by_path */
static int path_slot(const CTUI_NODES *ns, const char *path, int *found) {
  int lo = 0, hi = ns->n;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    int c = strcmp(ns->by_path[mid]->path, path);
    if (c == 0) {
      *found = 1;
      return mid;
    }
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  *found = 0;
  return lo;
}

static int id_slot(const CTUI_NODES *ns, uint32_t id) {
  int lo = 0, hi = ns->n;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (ns->by_id[mid]->id == id) {
      return mid;
    }
    if (ns->by_id[mid]->id < id) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return -1;
}

CTUI_NODE *ctui_nodes_find(const CTUI_NODES *ns, const char *path) {
  int found;
  int i = path_slot(ns, path, &found);
  return found ? ns->by_path[i] : NULL;
}

/* 1 if a node sits at one of path's ancestors, or under path */
static int path_conflicts(const CTUI_NODES *ns, const char *path) {
  char buf[CTUI_NODE_PATH_MAX + 2];
  size_t len = strlen(path);
  memcpy(buf, path, len + 1);
  for (size_t i = 0; i < len; i++) {
    if (buf[i] == '/') {
      buf[i] = '\0';
      int found;
      path_slot(ns, buf, &found);
      buf[i] = '/';
      if (found) {
        return 1;
      }
    }
  }
  buf[len] = '/';
  buf[len + 1] = '\0';
  int found;
  int i = path_slot(ns, buf, &found);
  return i < ns->n && strncmp(ns->by_path[i]->path, buf, len + 1) == 0;
}

static void store_text(CTUI_NODE *n, CTUI_BUF *b) {
  free(n->text);
  n->text = b->s;
  *b = (CTUI_BUF){0};
}

static void initial_value(CTUI_NODE *n) {
  const CTUI_NODE_TYPE *t = n->type;
  CTUI_NODE_VALUE v = {0};
  switch (t->kind) {
  case CTUI_NODE_INT:
    v.i = (int64_t)t->def;
    break;
  case CTUI_NODE_FLOAT:
    v.f = t->def;
    break;
  case CTUI_NODE_BOOL:
    v.b = t->def != 0;
    break;
  case CTUI_NODE_ENUM:
    v.e = (unsigned)t->def;
    break;
  default:
    v.s = "";
    break;
  }
  n->v = v;
  CTUI_BUF b = {0};
  ctui_node_format(t, &n->v, &b);
  store_text(n, &b);
}

CTUI_NODE *ctui_node_publish(CTUI_NODES *ns, const char *path,
                             const CTUI_NODE_TYPE *type,
                             const CTUI_NODE_OPS *ops, void *user) {
  if (!ctui_node_path_ok(path) || ctui_node_path_reserved(path) ||
      !ctui_node_type_ok(type)) {
    ctui_logf(E_WRN, "[CTUI:NODE] - %s: \"%s\" refused (path or type)\n",
              ns->name, path ? path : "(null)");
    return NULL;
  }
  int found;
  int at = path_slot(ns, path, &found);
  if (found || path_conflicts(ns, path)) {
    ctui_logf(E_WRN, "[CTUI:NODE] - %s: \"%s\" refused (taken, or a node "
                     "above or under it)\n",
              ns->name, path);
    return NULL;
  }
  if (ns->n == ns->cap) {
    int cap = ns->cap ? ns->cap * 2 : 32;
    CTUI_NODE **a = realloc(ns->by_id, (size_t)cap * sizeof *a);
    if (!a) {
      return NULL;
    }
    ns->by_id = a;
    CTUI_NODE **b = realloc(ns->by_path, (size_t)cap * sizeof *b);
    if (!b) {
      return NULL;
    }
    ns->by_path = b;
    ns->cap = cap;
  }
  CTUI_NODE *n = calloc(1, sizeof *n);
  if (!n || !(n->path = strdup(path))) {
    free(n);
    return NULL;
  }
  n->ns = ns;
  n->id = ns->next_id++;
  n->type = type;
  n->ops = ops;
  n->user = user;
  initial_value(n);
  if (!n->text) {
    node_free(n);
    return NULL;
  }
  ns->by_id[ns->n] = n;
  memmove(ns->by_path + at + 1, ns->by_path + at,
          (size_t)(ns->n - at) * sizeof *ns->by_path);
  ns->by_path[at] = n;
  ns->n++;
  send_pub(ns, n);
  return n;
}

CTUI_NODE *ctui_node_publishf(CTUI_NODES *ns, const CTUI_NODE_TYPE *type,
                              const CTUI_NODE_OPS *ops, void *user,
                              const char *fmt, ...) {
  char path[CTUI_NODE_PATH_MAX + 1];
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(path, sizeof path, fmt, ap);
  va_end(ap);
  if (len < 0 || (size_t)len >= sizeof path) {
    ctui_logf(E_WRN, "[CTUI:NODE] - %s: a path too long refused\n", ns->name);
    return NULL;
  }
  return ctui_node_publish(ns, path, type, ops, user);
}

void ctui_node_remove(CTUI_NODE *n) {
  if (!n || n->dead) {
    return;
  }
  CTUI_NODES *ns = n->ns;
  int i = id_slot(ns, n->id), found;
  int j = path_slot(ns, n->path, &found);
  memmove(ns->by_id + i, ns->by_id + i + 1,
          (size_t)(ns->n - i - 1) * sizeof *ns->by_id);
  memmove(ns->by_path + j, ns->by_path + j + 1,
          (size_t)(ns->n - j - 1) * sizeof *ns->by_path);
  ns->n--;
  char line[48];
  snprintf(line, sizeof line, "{\"rm\":%u}", n->id);
  send_line(ns, line);
  if (n->busy) {
    n->dead = 1;
  } else {
    node_free(n);
  }
}

int ctui_nodes_remove_under(CTUI_NODES *ns, const char *prefix) {
  size_t len = strlen(prefix);
  int removed = 0;
  ctui_nodes_begin(ns);
  CTUI_NODE *exact = ctui_nodes_find(ns, prefix);
  if (exact) {
    ctui_node_remove(exact);
    removed++;
  }
  /* everything under "prefix/" sits together in strcmp order */
  char under[CTUI_NODE_PATH_MAX + 2];
  if (len + 2 <= sizeof under) {
    memcpy(under, prefix, len);
    under[len] = '/';
    under[len + 1] = '\0';
    int found;
    int i = path_slot(ns, under, &found);
    while (i < ns->n && strncmp(ns->by_path[i]->path, under, len + 1) == 0) {
      ctui_node_remove(ns->by_path[i]);
      removed++;
    }
  }
  ctui_nodes_commit(ns);
  return removed;
}

/* --- values -------------------------------------------------------------- */

/* v (already n's kind and range) into n; nothing when its text is n's */
static void set_value(CTUI_NODE *n, const CTUI_NODE_VALUE *v) {
  CTUI_BUF b = {0};
  ctui_node_format(n->type, v, &b);
  if (!b.s || strcmp(b.s, n->text) == 0) {
    free(b.s);
    return;
  }
  CTUI_NODE_VALUE nv = *v;
  if (n->type->kind == CTUI_NODE_STRING) {
    char *s = malloc(v->len + 1);
    if (!s) {
      free(b.s);
      return;
    }
    memcpy(s, v->s, v->len);
    s[v->len] = '\0';
    free(n->str);
    n->str = s;
    nv.s = s;
  }
  n->v = nv;
  store_text(n, &b);
  n->gen++;
  if (n->dead) {
    return;
  }
  CTUI_BUF line = {0};
  ctui_buf_printf(&line, "{\"set\":%u,\"v\":", n->id);
  ctui_json_quote(&line, n->text);
  ctui_buf_add(&line, "}", 1);
  send_buf(n->ns, &line);
}

static int owner_check(const CTUI_NODE *n, CTUI_NODE_KIND kind) {
  if (!n || n->type->kind != kind) {
    ctui_logf(E_WRN, "[CTUI:NODE] - set of the wrong kind on \"%s\" refused\n",
              n ? n->path : "(null)");
    return 0;
  }
  return 1;
}

static int owner_range(const CTUI_NODE *n, double v) {
  const CTUI_NODE_TYPE *t = n->type;
  if (t->min <= t->max && (v < t->min || v > t->max)) {
    ctui_logf(E_WRN, "[CTUI:NODE] - \"%s\": %g outside [%g, %g] refused\n",
              n->path, v, t->min, t->max);
    return 0;
  }
  return 1;
}

void ctui_node_set_int(CTUI_NODE *n, int64_t v) {
  if (owner_check(n, CTUI_NODE_INT) && owner_range(n, (double)v)) {
    set_value(n, &(CTUI_NODE_VALUE){.i = v});
  }
}

void ctui_node_set_float(CTUI_NODE *n, double v) {
  if (owner_check(n, CTUI_NODE_FLOAT) && v == v && v - v == 0 &&
      owner_range(n, v)) {
    set_value(n, &(CTUI_NODE_VALUE){.f = v == 0 ? 0 : v});
  }
}

void ctui_node_set_bool(CTUI_NODE *n, int v) {
  if (owner_check(n, CTUI_NODE_BOOL)) {
    set_value(n, &(CTUI_NODE_VALUE){.b = v != 0});
  }
}

void ctui_node_set_enum(CTUI_NODE *n, unsigned index) {
  if (!owner_check(n, CTUI_NODE_ENUM)) {
    return;
  }
  unsigned count = 0;
  while (n->type->labels[count]) {
    count++;
  }
  if (index >= count) {
    ctui_logf(E_WRN, "[CTUI:NODE] - \"%s\": label %u of %u refused\n", n->path,
              index, count);
    return;
  }
  set_value(n, &(CTUI_NODE_VALUE){.e = index});
}

void ctui_node_set_str(CTUI_NODE *n, const char *s) {
  if (!owner_check(n, CTUI_NODE_STRING)) {
    return;
  }
  size_t len = strlen(s);
  if (!ctui_node_string_ok(n->type, s, len)) {
    ctui_logf(E_WRN, "[CTUI:NODE] - \"%s\": a string too long or not UTF-8 "
                     "refused\n",
              n->path);
    return;
  }
  set_value(n, &(CTUI_NODE_VALUE){.s = s, .len = len});
}

void ctui_nodes_begin(CTUI_NODES *ns) {
  if (ns->depth++ == 0) {
    send_line(ns, "{\"begin\":1}");
  }
}

void ctui_nodes_commit(CTUI_NODES *ns) {
  if (ns->depth > 0 && --ns->depth == 0) {
    send_line(ns, "{\"commit\":1}");
  }
}

const char *ctui_node_path(const CTUI_NODE *n) { return n->path; }
const CTUI_NODE_TYPE *ctui_node_type(const CTUI_NODE *n) { return n->type; }
const CTUI_NODE_VALUE *ctui_node_value(const CTUI_NODE *n) { return &n->v; }
const char *ctui_node_text(const CTUI_NODE *n) { return n->text; }
void *ctui_node_user(const CTUI_NODE *n) { return n->user; }
int ctui_nodes_count(const CTUI_NODES *ns) { return ns->n; }

/* --- writes from outside ------------------------------------------------- */

typedef struct {
  CTUI_NODE *node;
  const char *text;
  CTUI_NODE_VALUE v;
} WRITE;

static CTUI_NODE *node_of(CTUI_NODES *ns, const CTUI_JSON *id) {
  if (!id || id->type != CTUI_JSON_NUMBER || id->number < 1 ||
      id->number > UINT32_MAX) {
    return NULL;
  }
  int i = id_slot(ns, (uint32_t)id->number);
  return i < 0 ? NULL : ns->by_id[i];
}

/* w's node, its text parsed: 0 or the writer's errno */
static int prepare(WRITE *w) {
  const CTUI_NODE *n = w->node;
  if (!n) {
    return ENOENT;
  }
  if (!n->ops || !n->ops->on_write || !(n->type->flags & CTUI_NODE_WRITABLE)) {
    return EACCES;
  }
  if (!w->text) {
    return EINVAL;
  }
  return ctui_node_parse(n->type, w->text, strlen(w->text), &n->v, &w->v);
}

static int check(WRITE *w) {
  const CTUI_NODE *n = w->node;
  return n->ops->on_check ? n->ops->on_check(w->node, &w->v, n->user) : 0;
}

/* runs w's on_write: 0 or its errno; the value taken unless it set one */
static int apply(WRITE *w, CTUI_BUF *reply) {
  CTUI_NODE *n = w->node;
  if (n->dead) {
    return ENOENT;
  }
  /* a STRING's text isn't NUL-terminated where it came from */
  char *copy = NULL;
  if (w->v.s) {
    if (!(copy = malloc(w->v.len + 1))) {
      return ENOMEM;
    }
    memcpy(copy, w->v.s, w->v.len);
    copy[w->v.len] = '\0';
    w->v.s = copy;
  }
  unsigned gen = n->gen;
  int err = n->ops->on_write(n, &w->v, reply, n->user);
  if (err == 0 && gen == n->gen && n->type->kind != CTUI_NODE_TRIGGER) {
    set_value(n, &w->v);
  }
  free(copy);
  return err;
}

static void ack(CTUI_NODES *ns, double req, int err, int line,
                const CTUI_BUF *reply) {
  CTUI_BUF b = {0};
  ctui_buf_printf(&b, "{\"ack\":%.0f,\"err\":%d", req, err);
  if (line >= 0) {
    ctui_buf_printf(&b, ",\"line\":%d", line);
  }
  if (reply && reply->s && reply->len) {
    char *r = strndup(reply->s,
                      reply->len > CTUI_NODE_TEXT_MAX ? CTUI_NODE_TEXT_MAX
                                                      : reply->len);
    if (r) {
      ctui_buf_add(&b, ",\"reply\":", 9);
      ctui_json_quote(&b, r);
      free(r);
    }
  }
  ctui_buf_add(&b, "}", 1);
  send_buf(ns, &b);
}

static void handle_write(CTUI_NODES *ns, const CTUI_JSON *msg, double req) {
  WRITE w = {.node = node_of(ns, ctui_json_get(msg, "node")),
             .text = ctui_json_str(ctui_json_get(msg, "v"), NULL)};
  int err = prepare(&w);
  CTUI_BUF reply = {0};
  if (!err) {
    w.node->busy++;
    err = check(&w);
    if (!err) {
      err = apply(&w, &reply);
    }
    if (--w.node->busy == 0 && w.node->dead) {
      node_free(w.node);
    }
  }
  ack(ns, req, err, -1, &reply);
  free(reply.s);
}

/* every line parsed, every check, then every write: whole or not at all
 * (as far as the checks speak for the writes) */
static void handle_batch(CTUI_NODES *ns, const CTUI_JSON *msg, double req) {
  const CTUI_JSON *lines = ctui_json_get(msg, "w");
  if (!lines || lines->type != CTUI_JSON_ARRAY || lines->count == 0) {
    ack(ns, req, EINVAL, -1, NULL);
    return;
  }
  int count = lines->count;
  WRITE *ws = calloc((size_t)count, sizeof *ws);
  if (!ws) {
    ack(ns, req, ENOMEM, -1, NULL);
    return;
  }
  int err = 0, bad = -1;
  for (int i = 0; i < count && !err; i++) {
    const CTUI_JSON *l = &lines->items[i];
    if (l->type != CTUI_JSON_ARRAY || l->count != 2) {
      err = EINVAL;
    } else {
      ws[i].node = node_of(ns, &l->items[0]);
      ws[i].text = ctui_json_str(&l->items[1], NULL);
      err = prepare(&ws[i]);
    }
    bad = err ? i : -1;
  }
  int held = err ? 0 : count;
  for (int i = 0; i < held; i++) {
    ws[i].node->busy++;
  }
  for (int i = 0; i < held && !err; i++) {
    if ((err = check(&ws[i])) != 0) {
      bad = i;
    }
  }
  if (!err) {
    ctui_nodes_begin(ns);
    for (int i = 0; i < held && !err; i++) {
      /* a trigger's reply has nowhere to go from a batch: dropped */
      CTUI_BUF reply = {0};
      if ((err = apply(&ws[i], &reply)) != 0) {
        bad = i;
      }
      free(reply.s);
    }
    ctui_nodes_commit(ns);
  }
  for (int i = 0; i < held; i++) {
    if (--ws[i].node->busy == 0 && ws[i].node->dead) {
      node_free(ws[i].node);
    }
  }
  free(ws);
  ack(ns, req, err, bad, NULL);
}

static void handle_line(CTUI_NODES *ns, const char *line) {
  char err[96];
  CTUI_JSON *msg = ctui_json_parse(line, err, sizeof err);
  if (!msg || msg->type != CTUI_JSON_OBJECT) {
    ctui_logf(E_WRN, "[CTUI:NODE] - %s: a line from the daemon dropped (%s)\n",
              ns->name, msg ? "not an object" : err);
    ctui_json_free(msg);
    return;
  }
  const CTUI_JSON *req;
  if ((req = ctui_json_get(msg, "write")) && req->type == CTUI_JSON_NUMBER) {
    handle_write(ns, msg, req->number);
  } else if ((req = ctui_json_get(msg, "batch")) &&
             req->type == CTUI_JSON_NUMBER) {
    handle_batch(ns, msg, req->number);
  } else {
    ctui_logf(E_WRN, "[CTUI:NODE] - %s: an unknown line from the daemon\n",
              ns->name);
  }
  ctui_json_free(msg);
}

void ctui_nodes_dispatch(CTUI_NODES *ns) {
  if (ns->fd < 0) {
    return;
  }
  flush_out(ns);
  for (;;) {
    if (ns->fd < 0) {
      return;
    }
    if (ns->in_cap - ns->in_len < 4096) {
      size_t cap = ns->in_cap ? ns->in_cap * 2 : 8192;
      char *in = realloc(ns->in, cap);
      if (!in) {
        disconnect(ns, "out of memory");
        return;
      }
      ns->in = in;
      ns->in_cap = cap;
    }
    ssize_t r = recv(ns->fd, ns->in + ns->in_len, ns->in_cap - ns->in_len - 1,
                     0);
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    if (r <= 0) {
      disconnect(ns, r == 0 ? "closed" : strerror(errno));
      return;
    }
    ns->in_len += (size_t)r;
    ns->in[ns->in_len] = '\0';
    char *start = ns->in, *nl;
    while (ns->fd >= 0 && (nl = memchr(start, '\n', ns->in_len -
                                                        (size_t)(start - ns->in)))) {
      *nl = '\0';
      handle_line(ns, start);
      start = nl + 1;
    }
    if (ns->fd < 0) {
      return;
    }
    size_t rest = ns->in_len - (size_t)(start - ns->in);
    memmove(ns->in, start, rest);
    ns->in_len = rest;
    if (rest > LINE_MAX_BYTES) {
      disconnect(ns, "a line too long");
      return;
    }
  }
  flush_out(ns);
}
