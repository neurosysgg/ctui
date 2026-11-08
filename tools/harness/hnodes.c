#define _GNU_SOURCE /* SOCK_CLOEXEC, MSG_NOSIGNAL */

#include "hnodes.h"

#include "json/buf.h"
#include "json/json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CONNS_MAX 16
#define REQS_MAX 64

typedef struct {
  uint32_t id;
  char *path, *text;
} NODE;

typedef struct {
  int fd;
  unsigned seq; /* the order it connected in */
  char *in;
  size_t in_len, in_cap;
  NODE *nodes;
  int n, cap;
} CONN;

typedef struct {
  int req, conn; /* conn: an index into conns, -1 once it went */
  int answered, err;
  char *reply;
} REQ;

struct CTUI_HNODES {
  int listen_fd;
  char dir[64], path[96];
  CONN conns[CONNS_MAX];
  int nconns;
  REQ reqs[REQS_MAX];
  int next_req;
  unsigned next_seq;
};

CTUI_HNODES *ctui_hnodes_listen(void) {
  CTUI_HNODES *h = calloc(1, sizeof *h);
  if (!h) {
    return NULL;
  }
  const char *tmp = getenv("TMPDIR");
  snprintf(h->dir, sizeof h->dir, "%s/ctui-hnodes-XXXXXX",
           tmp && *tmp && strlen(tmp) < 32 ? tmp : "/tmp");
  if (!mkdtemp(h->dir)) {
    fprintf(stderr, "ctui_hnodes: mkdtemp %s: %s\n", h->dir, strerror(errno));
    free(h);
    return NULL;
  }
  snprintf(h->path, sizeof h->path, "%s/nodes.sock", h->dir);
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  memcpy(addr.sun_path, h->path, strlen(h->path) + 1);
  h->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (h->listen_fd < 0 ||
      bind(h->listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
      listen(h->listen_fd, 8) != 0) {
    fprintf(stderr, "ctui_hnodes: %s: %s\n", h->path, strerror(errno));
    if (h->listen_fd >= 0) {
      close(h->listen_fd);
    }
    rmdir(h->dir);
    free(h);
    return NULL;
  }
  h->next_req = 1;
  return h;
}

static void conn_drop(CTUI_HNODES *h, int c) {
  CONN *k = &h->conns[c];
  if (k->fd < 0) {
    return;
  }
  close(k->fd);
  k->fd = -1;
  for (int i = 0; i < k->n; i++) {
    free(k->nodes[i].path);
    free(k->nodes[i].text);
  }
  free(k->nodes);
  free(k->in);
  *k = (CONN){.fd = -1};
  for (int i = 0; i < REQS_MAX; i++) {
    if (h->reqs[i].req && h->reqs[i].conn == c && !h->reqs[i].answered) {
      h->reqs[i].conn = -1;
    }
  }
}

void ctui_hnodes_free(CTUI_HNODES *h) {
  if (!h) {
    return;
  }
  for (int c = 0; c < h->nconns; c++) {
    conn_drop(h, c);
  }
  for (int i = 0; i < REQS_MAX; i++) {
    free(h->reqs[i].reply);
  }
  close(h->listen_fd);
  unlink(h->path);
  rmdir(h->dir);
  free(h);
}

const char *ctui_hnodes_path(const CTUI_HNODES *h) { return h->path; }

int ctui_hnodes_fds(const CTUI_HNODES *h, struct pollfd *fds, int max) {
  int n = 0;
  if (n < max) {
    fds[n++] = (struct pollfd){.fd = h->listen_fd, .events = POLLIN};
  }
  for (int c = 0; c < h->nconns && n < max; c++) {
    if (h->conns[c].fd >= 0) {
      fds[n++] = (struct pollfd){.fd = h->conns[c].fd, .events = POLLIN};
    }
  }
  return n;
}

static NODE *node_by_id(CONN *k, const CTUI_JSON *id) {
  if (!id || id->type != CTUI_JSON_NUMBER) {
    return NULL;
  }
  for (int i = 0; i < k->n; i++) {
    if (k->nodes[i].id == (uint32_t)id->number) {
      return &k->nodes[i];
    }
  }
  return NULL;
}

static char *dup_or_empty(const char *s) { return strdup(s ? s : ""); }

static void on_line(CTUI_HNODES *h, int c, const char *line) {
  CONN *k = &h->conns[c];
  CTUI_JSON *msg = ctui_json_parse(line, NULL, 0);
  if (!msg || msg->type != CTUI_JSON_OBJECT) {
    ctui_json_free(msg);
    return;
  }
  const CTUI_JSON *v;
  if ((v = ctui_json_get(msg, "pub"))) {
    const char *path = ctui_json_str(ctui_json_get(msg, "path"), NULL);
    if (path && v->type == CTUI_JSON_NUMBER) {
      if (k->n == k->cap) {
        int cap = k->cap ? k->cap * 2 : 64;
        NODE *a = realloc(k->nodes, (size_t)cap * sizeof *a);
        if (!a) {
          ctui_json_free(msg);
          return;
        }
        k->nodes = a;
        k->cap = cap;
      }
      k->nodes[k->n++] =
          (NODE){(uint32_t)v->number, strdup(path),
                 dup_or_empty(ctui_json_str(ctui_json_get(msg, "v"), NULL))};
    }
  } else if ((v = ctui_json_get(msg, "set"))) {
    NODE *n = node_by_id(k, v);
    const char *text = ctui_json_str(ctui_json_get(msg, "v"), NULL);
    if (n && text) {
      free(n->text);
      n->text = strdup(text);
    }
  } else if ((v = ctui_json_get(msg, "rm"))) {
    NODE *n = node_by_id(k, v);
    if (n) {
      free(n->path);
      free(n->text);
      *n = k->nodes[--k->n];
    }
  } else if ((v = ctui_json_get(msg, "ack")) && v->type == CTUI_JSON_NUMBER) {
    for (int i = 0; i < REQS_MAX; i++) {
      REQ *r = &h->reqs[i];
      if (r->req == (int)v->number && r->conn == c && !r->answered) {
        r->answered = 1;
        r->err = ctui_json_int(ctui_json_get(msg, "err"), EIO);
        const char *reply = ctui_json_str(ctui_json_get(msg, "reply"), NULL);
        r->reply = reply ? strdup(reply) : NULL;
      }
    }
  }
  /* hello, begin, commit: nothing to keep */
  ctui_json_free(msg);
}

static int conn_read(CTUI_HNODES *h, int c) {
  CONN *k = &h->conns[c];
  int got = 0;
  for (;;) {
    if (k->in_cap - k->in_len < 4096) {
      size_t cap = k->in_cap ? k->in_cap * 2 : 16384;
      char *in = realloc(k->in, cap);
      if (!in) {
        conn_drop(h, c);
        return 1;
      }
      k->in = in;
      k->in_cap = cap;
    }
    ssize_t r = recv(k->fd, k->in + k->in_len, k->in_cap - k->in_len - 1, 0);
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return got;
    }
    if (r <= 0) {
      conn_drop(h, c);
      return 1;
    }
    got = 1;
    k->in_len += (size_t)r;
    k->in[k->in_len] = '\0';
    char *start = k->in, *nl;
    while ((nl = memchr(start, '\n', k->in_len - (size_t)(start - k->in)))) {
      *nl = '\0';
      on_line(h, c, start);
      start = nl + 1;
    }
    size_t rest = k->in_len - (size_t)(start - k->in);
    memmove(k->in, start, rest);
    k->in_len = rest;
  }
}

int ctui_hnodes_pump(CTUI_HNODES *h) {
  int got = 0;
  int fd;
  while ((fd = accept4(h->listen_fd, NULL, NULL,
                       SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
    int c = 0;
    while (c < h->nconns && h->conns[c].fd >= 0) {
      c++;
    }
    if (c == CONNS_MAX) {
      close(fd);
      continue;
    }
    if (c == h->nconns) {
      h->nconns++;
    }
    h->conns[c] = (CONN){.fd = fd, .seq = ++h->next_seq};
    got = 1;
  }
  for (int c = 0; c < h->nconns; c++) {
    if (h->conns[c].fd >= 0) {
      got |= conn_read(h, c);
    }
  }
  return got;
}

/* the newest app's: one an app runs in its terminal (a file chooser) is
 * in front of it */
static NODE *find(const CTUI_HNODES *h, const char *path, int *conn) {
  NODE *best = NULL;
  unsigned best_seq = 0;
  for (int c = 0; c < h->nconns; c++) {
    const CONN *k = &h->conns[c];
    for (int i = 0; k->fd >= 0 && i < k->n; i++) {
      if (k->seq > best_seq && strcmp(k->nodes[i].path, path) == 0) {
        best = &k->nodes[i];
        best_seq = k->seq;
        if (conn) {
          *conn = c;
        }
        break;
      }
    }
  }
  return best;
}

const char *ctui_hnodes_get(const CTUI_HNODES *h, const char *path) {
  NODE *n = find(h, path, NULL);
  return n ? n->text : NULL;
}

int ctui_hnodes_count(const CTUI_HNODES *h) {
  int n = 0;
  for (int c = 0; c < h->nconns; c++) {
    n += h->conns[c].fd >= 0 ? h->conns[c].n : 0;
  }
  return n;
}

static int by_path(const void *a, const void *b) {
  return strcmp((*(NODE *const *)a)->path, (*(NODE *const *)b)->path);
}

int ctui_hnodes_list(const CTUI_HNODES *h, const char *prefix, FILE *out) {
  size_t pl = strlen(prefix);
  NODE **all = NULL;
  int n = 0, cap = 0;
  for (int c = 0; c < h->nconns; c++) {
    const CONN *k = &h->conns[c];
    for (int i = 0; k->fd >= 0 && i < k->n; i++) {
      NODE *nd = &k->nodes[i];
      if (strncmp(nd->path, prefix, pl) != 0 ||
          (nd->path[pl] && nd->path[pl] != '/' && pl > 0) ||
          find(h, nd->path, NULL) != nd) { /* a newer app's has it */
        continue;
      }
      if (n == cap) {
        cap = cap ? cap * 2 : 64;
        NODE **a = realloc(all, (size_t)cap * sizeof *a);
        if (!a) {
          free(all);
          return -1;
        }
        all = a;
      }
      all[n++] = nd;
    }
  }
  if (n > 1) {
    qsort(all, (size_t)n, sizeof *all, by_path);
  }
  for (int i = 0; i < n; i++) {
    fprintf(out, "  %s = ", all[i]->path);
    for (const char *t = all[i]->text ? all[i]->text : ""; *t; t++) {
      if (*t == '\n') {
        fputs("\\n", out);
      } else {
        fputc(*t, out);
      }
    }
    fputc('\n', out);
  }
  free(all);
  return n;
}

int ctui_hnodes_write(CTUI_HNODES *h, const char *path, const char *text) {
  int c;
  NODE *n = find(h, path, &c);
  if (!n) {
    return -1;
  }
  int req = h->next_req++;
  REQ *r = &h->reqs[req % REQS_MAX];
  free(r->reply);
  *r = (REQ){.req = req, .conn = c};
  CTUI_BUF b = {0};
  ctui_buf_printf(&b, "{\"write\":%d,\"node\":%u,\"v\":", req, n->id);
  ctui_json_quote(&b, text);
  ctui_buf_add(&b, "}\n", 2);
  /* small lines on a unix socket: whole, or the app is gone */
  if (send(h->conns[c].fd, b.s, b.len, MSG_NOSIGNAL) != (ssize_t)b.len) {
    conn_drop(h, c);
  }
  free(b.s);
  return req;
}

int ctui_hnodes_answer(CTUI_HNODES *h, int req, int *err, const char **reply) {
  REQ *r = &h->reqs[req % REQS_MAX];
  if (r->req != req || (!r->answered && r->conn < 0)) {
    return -1;
  }
  if (!r->answered) {
    return 0;
  }
  *err = r->err;
  *reply = r->reply;
  return 1;
}
