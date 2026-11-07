/* node/node.h: the values' text (each kind, relative writes, ranges,
 * clamping, UTF-8, floats' shortest text, a comma-decimal locale), the
 * path rules, the type as JSON and back, the table (paths that clash,
 * removal under a prefix, sets that change nothing), and the client
 * against a fake daemon on a real unix socket: the table published on
 * connect, writes answered, refusals' errnos, batches whole or not at
 * all, a trigger's reply, a node removed in its own callback, the
 * connection dropped and the table published again. */
#define _GNU_SOURCE

#include "node/node.h"

#include "ctui.h"
#include "ctui_test.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const CTUI_NODE_TYPE LEVEL = {
    CTUI_NODE_FLOAT, "%", 0, 150, 40, CTUI_NODE_LINEAR, NULL, 0,
    CTUI_NODE_WRITABLE};
static const CTUI_NODE_TYPE KNOB = {
    CTUI_NODE_FLOAT, "Hz", 20, 20000, 1000, CTUI_NODE_LOG, NULL, 0,
    CTUI_NODE_WRITABLE | CTUI_NODE_CLAMP};
static const CTUI_NODE_TYPE COUNT = {
    CTUI_NODE_INT, NULL, -10, 10, 0, CTUI_NODE_LINEAR, NULL, 0,
    CTUI_NODE_WRITABLE};
static const CTUI_NODE_TYPE ANY_INT = {
    CTUI_NODE_INT, NULL, 1, 0, 0, CTUI_NODE_LINEAR, NULL, 0,
    CTUI_NODE_WRITABLE};
static const CTUI_NODE_TYPE FLAG = {
    CTUI_NODE_BOOL, NULL, 1, 0, 0, CTUI_NODE_LINEAR, NULL, 0,
    CTUI_NODE_WRITABLE};
static const char *const LAYOUTS[] = {"halves", "stack", "quad", NULL};
static const CTUI_NODE_TYPE LAYOUT = {
    CTUI_NODE_ENUM, NULL, 1, 0, 1, CTUI_NODE_LINEAR, LAYOUTS, 0,
    CTUI_NODE_WRITABLE};
static const CTUI_NODE_TYPE TITLE = {
    CTUI_NODE_STRING, NULL, 1, 0, 0, CTUI_NODE_LINEAR, NULL, 8,
    CTUI_NODE_WRITABLE};
/* writable, but published without ops: read-only after all */
static const CTUI_NODE_TYPE SHOWN = {
    CTUI_NODE_STRING, NULL, 1, 0, 0, CTUI_NODE_LINEAR, NULL, 0,
    CTUI_NODE_WRITABLE};
static const CTUI_NODE_TYPE PRESS = {
    CTUI_NODE_TRIGGER, NULL, 1, 0, 0, CTUI_NODE_LINEAR, NULL, 0,
    CTUI_NODE_WRITABLE};

/* --- the text ------------------------------------------------------------ */

static int parses(const CTUI_NODE_TYPE *t, const char *text,
                  const CTUI_NODE_VALUE *cur, CTUI_NODE_VALUE *out) {
  CTUI_NODE_VALUE scratch;
  return ctui_node_parse(t, text, strlen(text), cur, out ? out : &scratch);
}

static int formats_as(const CTUI_NODE_TYPE *t, CTUI_NODE_VALUE v,
                      const char *want) {
  CTUI_BUF b = {0};
  ctui_node_format(t, &v, &b);
  int ok = b.s && strcmp(b.s, want) == 0;
  if (!ok) {
    printf("    got \"%s\", want \"%s\"\n", b.s ? b.s : "(null)", want);
  }
  free(b.s);
  return ok;
}

static void test_text(void) {
  CTUI_NODE_VALUE v, cur = {.i = 3, .f = 40, .b = 1, .e = 1};

  CTUI_TEST_ASSERT(parses(&COUNT, "-7", NULL, &v) == 0 && v.i == -7,
                   "int: a negative number");
  CTUI_TEST_ASSERT(parses(&COUNT, "5\n", NULL, &v) == 0 && v.i == 5,
                   "int: echo's newline dropped");
  CTUI_TEST_ASSERT(parses(&COUNT, "5\n\n", NULL, &v) == EINVAL,
                   "int: only one newline dropped");
  CTUI_TEST_ASSERT(parses(&COUNT, "+5", NULL, NULL) == EINVAL &&
                       parses(&COUNT, " 5", NULL, NULL) == EINVAL &&
                       parses(&COUNT, "5x", NULL, NULL) == EINVAL &&
                       parses(&COUNT, "", NULL, NULL) == EINVAL &&
                       parses(&COUNT, "1.5", NULL, NULL) == EINVAL,
                   "int: '+5', ' 5', '5x', '', '1.5' refused");
  CTUI_TEST_ASSERT(parses(&COUNT, "11", NULL, NULL) == ERANGE,
                   "int: out of range refused");
  CTUI_TEST_ASSERT(parses(&COUNT, "+=4", &cur, &v) == 0 && v.i == 7 &&
                       parses(&COUNT, "-=13", &cur, &v) == 0 && v.i == -10,
                   "int: relative writes against the value");
  CTUI_TEST_ASSERT(parses(&COUNT, "+=8", &cur, NULL) == ERANGE,
                   "int: a relative write past the range refused");
  CTUI_TEST_ASSERT(parses(&COUNT, "+=1", NULL, NULL) == EINVAL &&
                       parses(&COUNT, "+=-1", &cur, NULL) == EINVAL,
                   "int: relative without a value, or signed twice, refused");
  CTUI_TEST_ASSERT(parses(&ANY_INT, "-9223372036854775808", NULL, &v) == 0 &&
                       v.i == INT64_MIN &&
                       parses(&ANY_INT, "9223372036854775808", NULL, NULL) ==
                           ERANGE,
                   "int: int64's limits");
  CTUI_NODE_VALUE big = {.i = INT64_MAX};
  CTUI_TEST_ASSERT(parses(&ANY_INT, "+=1", &big, NULL) == ERANGE,
                   "int: a relative write that overflows refused");

  CTUI_TEST_ASSERT(parses(&LEVEL, "42.5", NULL, &v) == 0 && v.f == 42.5 &&
                       parses(&LEVEL, ".5", NULL, &v) == 0 && v.f == 0.5 &&
                       parses(&LEVEL, "1e2", NULL, &v) == 0 && v.f == 100,
                   "float: decimals, a bare fraction, an exponent");
  CTUI_TEST_ASSERT(parses(&LEVEL, "inf", NULL, NULL) == EINVAL &&
                       parses(&LEVEL, "nan", NULL, NULL) == EINVAL &&
                       parses(&LEVEL, "0x10", NULL, NULL) == EINVAL &&
                       parses(&LEVEL, ".", NULL, NULL) == EINVAL &&
                       parses(&LEVEL, "1e", NULL, NULL) == EINVAL &&
                       parses(&LEVEL, "1,5", NULL, NULL) == EINVAL,
                   "float: inf, nan, hex, '.', '1e', '1,5' refused");
  CTUI_TEST_ASSERT(parses(&LEVEL, "200", NULL, NULL) == ERANGE &&
                       parses(&LEVEL, "-0.1", NULL, NULL) == ERANGE,
                   "float: out of range refused");
  CTUI_TEST_ASSERT(parses(&LEVEL, "+=2.5", &cur, &v) == 0 && v.f == 42.5,
                   "float: a relative write");
  CTUI_TEST_ASSERT(parses(&KNOB, "5", NULL, &v) == 0 && v.f == 20 &&
                       parses(&KNOB, "1e9", NULL, &v) == 0 && v.f == 20000,
                   "float: a CLAMP type clamps instead");
  CTUI_TEST_ASSERT(parses(&LEVEL, "1e999", NULL, NULL) == ERANGE,
                   "float: past double's range refused");

  CTUI_TEST_ASSERT(parses(&FLAG, "1", NULL, &v) == 0 && v.b == 1 &&
                       parses(&FLAG, "0", NULL, &v) == 0 && v.b == 0 &&
                       parses(&FLAG, "toggle", &cur, &v) == 0 && v.b == 0,
                   "bool: 0, 1, toggle");
  CTUI_TEST_ASSERT(parses(&FLAG, "true", NULL, NULL) == EINVAL &&
                       parses(&FLAG, "2", NULL, NULL) == EINVAL,
                   "bool: 'true', '2' refused");

  CTUI_TEST_ASSERT(parses(&LAYOUT, "quad", NULL, &v) == 0 && v.e == 2,
                   "enum: a label");
  CTUI_TEST_ASSERT(parses(&LAYOUT, "Quad", NULL, NULL) == EINVAL,
                   "enum: labels are exact");
  CTUI_TEST_ASSERT(parses(&LAYOUT, "+=1", &cur, &v) == 0 && v.e == 2 &&
                       parses(&LAYOUT, "-=1", &cur, &v) == 0 && v.e == 0,
                   "enum: steps");
  CTUI_TEST_ASSERT(parses(&LAYOUT, "+=2", &cur, NULL) == ERANGE,
                   "enum: a step past the last label refused");

  CTUI_TEST_ASSERT(parses(&TITLE, "héllo", NULL, &v) == 0 && v.len == 6 &&
                       memcmp(v.s, "héllo", 6) == 0,
                   "string: UTF-8 taken");
  CTUI_TEST_ASSERT(parses(&TITLE, "a\nb", NULL, &v) == 0 && v.len == 3,
                   "string: a newline inside kept");
  CTUI_TEST_ASSERT(parses(&TITLE, "123456789", NULL, NULL) == EINVAL &&
                       parses(&TITLE, "12345678\n", NULL, NULL) == 0,
                   "string: max_len, the dropped newline not counted");
  CTUI_TEST_ASSERT(parses(&TITLE, "a\xff", NULL, NULL) == EINVAL &&
                       ctui_node_parse(&TITLE, "a\0b", 3, NULL, &v) == EINVAL,
                   "string: bad UTF-8 and a NUL refused");
  CTUI_TEST_ASSERT(parses(&PRESS, "", NULL, &v) == 0 && v.len == 0,
                   "trigger: an empty argument");

  CTUI_TEST_ASSERT(formats_as(&LEVEL, (CTUI_NODE_VALUE){.f = 0.1}, "0.1") &&
                       formats_as(&LEVEL, (CTUI_NODE_VALUE){.f = 40}, "40") &&
                       formats_as(&LEVEL, (CTUI_NODE_VALUE){.f = 1.0 / 3},
                                  "0.3333333333333333"),
                   "format: floats' shortest text that reads back");
  CTUI_TEST_ASSERT(formats_as(&COUNT, (CTUI_NODE_VALUE){.i = -3}, "-3") &&
                       formats_as(&FLAG, (CTUI_NODE_VALUE){.b = 1}, "1") &&
                       formats_as(&LAYOUT, (CTUI_NODE_VALUE){.e = 1}, "stack") &&
                       formats_as(&PRESS, (CTUI_NODE_VALUE){0}, ""),
                   "format: int, bool, enum, trigger");

  /* a comma-decimal LC_NUMERIC (where this machine has one) changes
   * neither direction */
  const char *de = setlocale(LC_NUMERIC, "de_DE.UTF-8");
  if (de) {
    CTUI_TEST_ASSERT(parses(&LEVEL, "2.5", NULL, &v) == 0 && v.f == 2.5 &&
                         formats_as(&LEVEL, (CTUI_NODE_VALUE){.f = 2.5},
                                    "2.5"),
                     "a comma-decimal locale: '.' both ways");
    setlocale(LC_NUMERIC, "C");
  } else {
    printf("  (no de_DE.UTF-8 locale: the locale case skipped)\n");
  }
}

static void test_paths_types(void) {
  CTUI_TEST_ASSERT(ctui_node_path_ok("volume/sink/level") &&
                       ctui_node_path_ok("zones/DP-1/main") &&
                       ctui_node_path_ok("a:b+c@d.e_f"),
                   "paths: the allowed characters");
  CTUI_TEST_ASSERT(!ctui_node_path_ok("") && !ctui_node_path_ok("/a") &&
                       !ctui_node_path_ok("a/") && !ctui_node_path_ok("a//b") &&
                       !ctui_node_path_ok(".hidden") &&
                       !ctui_node_path_ok("a/../b") &&
                       !ctui_node_path_ok("a b") && !ctui_node_path_ok("ä"),
                   "paths: empty, slashes, dots, spaces, non-ASCII refused");
  char deep[64] = "";
  for (int i = 0; i < 17; i++) {
    strcat(deep, i ? "/a" : "a");
  }
  char long_name[80];
  memset(long_name, 'x', 64);
  long_name[64] = '\0';
  CTUI_TEST_ASSERT(!ctui_node_path_ok(deep) && !ctui_node_path_ok(long_name),
                   "paths: 17 deep or a 64-byte name refused");
  CTUI_TEST_ASSERT(ctui_node_path_reserved("name") &&
                       ctui_node_path_reserved("events/x") &&
                       !ctui_node_path_reserved("names") &&
                       !ctui_node_path_reserved("x/batch"),
                   "paths: the reserved names, at the top only");

  CTUI_NODE_TYPE bad = LEVEL;
  bad.def = 200;
  CTUI_TEST_ASSERT(!ctui_node_type_ok(&bad), "types: a default out of range");
  bad = LAYOUT;
  bad.labels = NULL;
  CTUI_TEST_ASSERT(!ctui_node_type_ok(&bad), "types: an enum without labels");
  bad = TITLE;
  bad.max_len = CTUI_NODE_TEXT_MAX + 1;
  CTUI_TEST_ASSERT(!ctui_node_type_ok(&bad), "types: max_len past the cap");

  const CTUI_NODE_TYPE *all[] = {&LEVEL, &KNOB,  &COUNT, &ANY_INT, &FLAG,
                                 &LAYOUT, &TITLE, &SHOWN, &PRESS};
  int same = 1;
  for (size_t i = 0; i < sizeof all / sizeof *all; i++) {
    CTUI_BUF b = {0};
    ctui_node_type_json(all[i], &b);
    char err[64];
    CTUI_JSON *j = ctui_json_parse(b.s, err, sizeof err);
    CTUI_NODE_TYPE *t = ctui_node_type_from_json(j);
    CTUI_BUF again = {0};
    if (t) {
      ctui_node_type_json(t, &again);
    }
    if (!t || strcmp(b.s, again.s) != 0) {
      printf("    %s -> %s\n", b.s, again.s ? again.s : "(refused)");
      same = 0;
    }
    free(t);
    free(again.s);
    free(b.s);
    ctui_json_free(j);
  }
  CTUI_TEST_ASSERT(same, "types: every kind through JSON and back");

  static const char *const REFUSED[] = {
      "{\"kind\":\"widget\"}",
      "{\"kind\":\"enum\",\"labels\":[]}",
      "{\"kind\":\"enum\",\"labels\":[1]}",
      "{\"kind\":\"enum\",\"labels\":[\"a\"],\"def\":1}",
      "{\"kind\":\"float\",\"min\":1}",
      "{\"kind\":\"float\",\"min\":2,\"max\":1}",
      "{\"kind\":\"string\",\"len\":5000}",
      "{\"kind\":\"string\",\"len\":-1}",
      "{\"kind\":\"int\",\"scale\":\"cubic\"}",
      "{\"kind\":\"int\",\"unit\":7}",
      "[]",
  };
  int refused = 1;
  for (size_t i = 0; i < sizeof REFUSED / sizeof *REFUSED; i++) {
    char err[64];
    CTUI_JSON *j = ctui_json_parse(REFUSED[i], err, sizeof err);
    CTUI_NODE_TYPE *t = ctui_node_type_from_json(j);
    if (t) {
      printf("    taken: %s\n", REFUSED[i]);
      refused = 0;
    }
    free(t);
    ctui_json_free(j);
  }
  CTUI_TEST_ASSERT(refused, "types: bad JSON types refused");

  /* the fuzzer's: a label that isn't UTF-8 would come back longer */
  static const char *const BAD_LABELS[] = {"ok", "\x8c\x8c", NULL};
  CTUI_NODE_TYPE bad_enum = LAYOUT;
  bad_enum.labels = BAD_LABELS;
  bad_enum.def = 0;
  CTUI_NODE_TYPE bad_unit = LEVEL;
  bad_unit.unit = "\xff";
  char lerr[64];
  CTUI_JSON *lj = ctui_json_parse("{\"kind\":\"enum\",\"labels\":[\"\x8c\"]}",
                                  lerr, sizeof lerr);
  CTUI_NODE_TYPE *lt = ctui_node_type_from_json(lj);
  CTUI_TEST_ASSERT(!ctui_node_type_ok(&bad_enum) &&
                       !ctui_node_type_ok(&bad_unit) && !lt,
                   "types: labels and units that aren't UTF-8 refused");
  free(lt);
  ctui_json_free(lj);
}

/* --- the table, no daemon ------------------------------------------------ */

static void test_table(void) {
  setenv("CTUI_NODES_SOCK", "/nonexistent/ctui-nodes.sock", 1);
  CTUI_NODES *ns = ctui_nodes_open("test", 0);
  CTUI_TEST_ASSERT(ns && ctui_nodes_fd(ns) == -1 &&
                       ctui_nodes_connect(ns) == -1,
                   "no daemon: open works, nothing connected");

  CTUI_NODE *level = ctui_node_publish(ns, "volume/sink/level", &LEVEL, NULL,
                                       NULL);
  CTUI_TEST_ASSERT(level && strcmp(ctui_node_text(level), "40") == 0,
                   "publish: the value starts at the default");
  CTUI_TEST_ASSERT(!ctui_node_publish(ns, "volume/sink/level", &LEVEL, NULL,
                                      NULL) &&
                       !ctui_node_publish(ns, "volume/sink", &FLAG, NULL,
                                          NULL) &&
                       !ctui_node_publish(ns, "volume/sink/level/x", &FLAG,
                                          NULL, NULL),
                   "publish: a taken path, a file above or under one refused");
  CTUI_TEST_ASSERT(ctui_node_publish(ns, "volume/sink-2", &FLAG, NULL, NULL) &&
                       ctui_node_publish(ns, "volume/sink.b/mute", &FLAG,
                                         NULL, NULL),
                   "publish: neighbours that only share a prefix are fine");
  CTUI_TEST_ASSERT(!ctui_node_publish(ns, "events", &FLAG, NULL, NULL) &&
                       !ctui_node_publish(ns, "x", &(CTUI_NODE_TYPE){
                                                       .kind = 99},
                                          NULL, NULL),
                   "publish: a reserved name, a bad type refused");

  CTUI_NODE *mute = ctui_node_publishf(ns, &FLAG, NULL, NULL, "volume/%s/mute",
                                       "sink");
  ctui_node_publish(ns, "volume/source/level", &LEVEL, NULL, NULL);
  ctui_node_publish(ns, "volume/sinks", &FLAG, NULL, NULL);
  CTUI_TEST_ASSERT(mute && ctui_nodes_find(ns, "volume/sink/mute") == mute &&
                       ctui_nodes_count(ns) == 6,
                   "publishf and find");

  ctui_node_set_float(level, 55.5);
  ctui_node_set_float(level, 151);
  ctui_node_set_int(level, 3);
  CTUI_TEST_ASSERT(strcmp(ctui_node_text(level), "55.5") == 0,
                   "set: in range taken; out of range, wrong kind refused");

  CTUI_TEST_ASSERT(ctui_nodes_remove_under(ns, "volume/sink") == 2 &&
                       !ctui_nodes_find(ns, "volume/sink/level") &&
                       ctui_nodes_find(ns, "volume/sink-2") &&
                       ctui_nodes_find(ns, "volume/sinks") &&
                       ctui_nodes_count(ns) == 4,
                   "remove_under: only that directory");
  ctui_nodes_close(ns);
}

/* --- against a fake daemon ----------------------------------------------- */

typedef struct {
  int listen_fd, fd;
  char dir[64], path[128];
  CTUI_BUF got; /* what the library sent, line by line */
} FAKE;

static void fake_listen(FAKE *f) {
  snprintf(f->dir, sizeof f->dir, "/tmp/ctui-node-test-XXXXXX");
  if (!mkdtemp(f->dir)) {
    perror("mkdtemp");
    exit(1);
  }
  snprintf(f->path, sizeof f->path, "%s/nodes.sock", f->dir);
  f->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strcpy(addr.sun_path, f->path);
  if (bind(f->listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
      listen(f->listen_fd, 4) != 0) {
    perror("bind");
    exit(1);
  }
  f->fd = -1;
  setenv("CTUI_NODES_SOCK", f->path, 1);
}

static void fake_accept(FAKE *f) {
  f->fd = accept(f->listen_fd, NULL, NULL);
  fcntl(f->fd, F_SETFL, O_NONBLOCK);
}

/* everything the library sent so far, appended to f->got */
static const char *fake_read(FAKE *f) {
  char buf[8192];
  ssize_t r;
  while (f->fd >= 0 && (r = read(f->fd, buf, sizeof buf)) > 0) {
    ctui_buf_add(&f->got, buf, (size_t)r);
  }
  return f->got.s ? f->got.s : "";
}

static void fake_clear(FAKE *f) {
  free(f->got.s);
  f->got = (CTUI_BUF){0};
}

static void fake_say(FAKE *f, CTUI_NODES *ns, const char *line) {
  size_t len = strlen(line);
  if (write(f->fd, line, len) != (ssize_t)len || write(f->fd, "\n", 1) != 1) {
    perror("write");
  }
  ctui_nodes_dispatch(ns);
}

static int has(FAKE *f, const char *want) {
  int ok = strstr(fake_read(f), want) != NULL;
  if (!ok) {
    printf("    want %s in:\n%s", want, fake_read(f));
  }
  return ok;
}

static int writes_seen;
static int last_level_write;

static int level_write(CTUI_NODE *n, const CTUI_NODE_VALUE *v, CTUI_BUF *reply,
                       void *user) {
  (void)n, (void)reply, (void)user;
  writes_seen++;
  last_level_write = (int)v->f;
  return 0;
}

static int level_check(CTUI_NODE *n, const CTUI_NODE_VALUE *v, void *user) {
  (void)n, (void)user;
  return v->f == 13 ? EBUSY : 0; /* the app's own refusal */
}

static const CTUI_NODE_OPS LEVEL_OPS = {level_write, level_check};

/* the owner moves the value itself: its own value wins over the write's */
static int layout_write(CTUI_NODE *n, const CTUI_NODE_VALUE *v,
                        CTUI_BUF *reply, void *user) {
  (void)reply, (void)user;
  ctui_node_set_enum(n, v->e == 0 ? 2 : v->e);
  return 0;
}

static const CTUI_NODE_OPS LAYOUT_OPS = {layout_write, NULL};

static int next_id = 41;
static CTUI_NODE *gone;

static int press_write(CTUI_NODE *n, const CTUI_NODE_VALUE *v, CTUI_BUF *reply,
                       void *user) {
  (void)user;
  if (v->len == 4 && memcmp(v->s, "gone", 4) == 0) {
    gone = n;
    ctui_node_remove(n); /* in its own callback */
    return 0;
  }
  if (v->len && v->s[v->len] != '\0') {
    return EFAULT; /* the callback's text must be NUL-terminated */
  }
  ctui_buf_printf(reply, "%d", ++next_id);
  return 0;
}

static const CTUI_NODE_OPS PRESS_OPS = {press_write, NULL};

static void test_daemon(void) {
  FAKE f = {0};
  fake_listen(&f);

  /* published before any daemon: all of it goes on connect */
  setenv("CTUI_NODES_SOCK", "/nonexistent/x.sock", 1);
  CTUI_NODES *ns = ctui_nodes_open("ctui-test", CTUI_NODES_SINGLE);
  CTUI_NODE *level = ctui_node_publish(ns, "volume/sink/level", &LEVEL,
                                       &LEVEL_OPS, NULL);
  CTUI_NODE *layout = ctui_node_publish(ns, "zones/DP-1/main/layout", &LAYOUT,
                                        &LAYOUT_OPS, NULL);
  CTUI_NODE *title = ctui_node_publish(ns, "title", &SHOWN, NULL, NULL);
  CTUI_NODE *press = ctui_node_publish(ns, "notify/new", &PRESS, &PRESS_OPS,
                                       NULL);
  ctui_node_set_str(title, "hi \"there\"\n");
  setenv("CTUI_NODES_SOCK", f.path, 1);
  CTUI_TEST_ASSERT(ctui_nodes_connect(ns) == 0 && ctui_nodes_fd(ns) >= 0,
                   "connect once a daemon listens");
  fake_accept(&f);
  CTUI_TEST_ASSERT(has(&f, "{\"hello\":\"ctui-test\",\"v\":1,\"single\":true}\n"
                           "{\"pub\":1,\"path\":\"volume/sink/level\","
                           "\"type\":{\"kind\":\"float\",\"unit\":\"%\","
                           "\"def\":40,\"min\":0,\"max\":150,\"w\":1},"
                           "\"v\":\"40\"}\n"),
                   "hello, then the table in order");
  CTUI_TEST_ASSERT(has(&f, "\"path\":\"title\",\"type\":{\"kind\":\"string\"},"
                           "\"v\":\"hi \\\"there\\\"\\u000a\"}") &&
                       has(&f, "{\"pub\":4,\"path\":\"notify/new\","
                               "\"type\":{\"kind\":\"trigger\",\"w\":1}}"),
                   "a string's current value quoted; a trigger without one");
  CTUI_TEST_ASSERT(!strstr(fake_read(&f), "\"path\":\"title\",\"type\":{\"kind\":"
                                          "\"string\",\"w\""),
                   "a node without ops isn't advertised writable");
  fake_clear(&f);

  ctui_node_set_float(level, 50);
  ctui_node_set_float(level, 50);
  CTUI_TEST_ASSERT(strcmp(fake_read(&f), "{\"set\":1,\"v\":\"50\"}\n") == 0,
                   "a set goes once; the same value again sends nothing");
  fake_clear(&f);

  fake_say(&f, ns, "{\"write\":7,\"node\":1,\"v\":\"+=10\\n\"}");
  CTUI_TEST_ASSERT(writes_seen == 1 && last_level_write == 60 &&
                       strcmp(fake_read(&f), "{\"set\":1,\"v\":\"60\"}\n"
                                             "{\"ack\":7,\"err\":0}\n") == 0,
                   "a relative write: the callback, the value taken, the "
                   "set before the ack");
  fake_clear(&f);

  fake_say(&f, ns, "{\"write\":8,\"node\":1,\"v\":\"500\"}");
  fake_say(&f, ns, "{\"write\":9,\"node\":1,\"v\":\"loud\"}");
  fake_say(&f, ns, "{\"write\":10,\"node\":1,\"v\":\"13\"}");
  fake_say(&f, ns, "{\"write\":11,\"node\":3,\"v\":\"x\"}");
  fake_say(&f, ns, "{\"write\":12,\"node\":99,\"v\":\"1\"}");
  fake_say(&f, ns, "{\"write\":13,\"node\":1}");
  char want[256];
  snprintf(want, sizeof want,
           "{\"ack\":8,\"err\":%d}\n{\"ack\":9,\"err\":%d}\n"
           "{\"ack\":10,\"err\":%d}\n{\"ack\":11,\"err\":%d}\n"
           "{\"ack\":12,\"err\":%d}\n{\"ack\":13,\"err\":%d}\n",
           ERANGE, EINVAL, EBUSY, EACCES, ENOENT, EINVAL);
  CTUI_TEST_ASSERT(strcmp(fake_read(&f), want) == 0 && writes_seen == 1 &&
                       strcmp(ctui_node_text(level), "60") == 0,
                   "refused: range, grammar, the app's check, read-only, "
                   "unknown node, no value -- nothing moved");
  fake_clear(&f);

  fake_say(&f, ns, "{\"write\":14,\"node\":2,\"v\":\"halves\"}");
  CTUI_TEST_ASSERT(strcmp(ctui_node_text(layout), "quad") == 0 &&
                       has(&f, "{\"set\":2,\"v\":\"quad\"}\n{\"ack\":14,"),
                   "the owner's own set in the callback wins");
  fake_clear(&f);

  fake_say(&f, ns, "{\"write\":15,\"node\":4,\"v\":\"Hi\\nthere\"}");
  CTUI_TEST_ASSERT(strcmp(fake_read(&f),
                          "{\"ack\":15,\"err\":0,\"reply\":\"42\"}\n") == 0,
                   "a trigger: its reply in the ack, no set");
  fake_clear(&f);

  /* a batch: one bad line, nothing applied */
  writes_seen = 0;
  fake_say(&f, ns, "{\"batch\":16,\"w\":[[1,\"20\"],[2,\"nope\"]]}");
  fake_say(&f, ns, "{\"batch\":17,\"w\":[[1,\"20\"],[1,\"13\"]]}");
  snprintf(want, sizeof want,
           "{\"ack\":16,\"err\":%d,\"line\":1}\n"
           "{\"ack\":17,\"err\":%d,\"line\":1}\n",
           EINVAL, EBUSY);
  CTUI_TEST_ASSERT(strcmp(fake_read(&f), want) == 0 && writes_seen == 0 &&
                       strcmp(ctui_node_text(level), "60") == 0,
                   "a batch with a bad line or a failed check: nothing "
                   "applied, the line said");
  fake_clear(&f);
  /* the fuzzer's: a trigger in a batch gets a reply buffer too */
  fake_say(&f, ns, "{\"batch\":30,\"w\":[[4,\"x\"]]}");
  CTUI_TEST_ASSERT(has(&f, "{\"ack\":30,\"err\":0}") && !strstr(fake_read(&f),
                                                                 "reply"),
                   "a trigger in a batch: its reply dropped, no crash");
  fake_clear(&f);
  fake_say(&f, ns, "{\"batch\":18,\"w\":[[1,\"20\"],[2,\"stack\"]]}");
  CTUI_TEST_ASSERT(strcmp(fake_read(&f),
                          "{\"begin\":1}\n{\"set\":1,\"v\":\"20\"}\n"
                          "{\"set\":2,\"v\":\"stack\"}\n{\"commit\":1}\n"
                          "{\"ack\":18,\"err\":0}\n") == 0,
                   "a batch that passes: its sets between begin and commit");
  fake_clear(&f);

  ctui_nodes_begin(ns);
  ctui_nodes_begin(ns);
  ctui_node_set_float(level, 21);
  ctui_nodes_commit(ns);
  ctui_node_set_float(level, 22);
  ctui_nodes_commit(ns);
  ctui_nodes_commit(ns);
  CTUI_TEST_ASSERT(strcmp(fake_read(&f),
                          "{\"begin\":1}\n{\"set\":1,\"v\":\"21\"}\n"
                          "{\"set\":1,\"v\":\"22\"}\n{\"commit\":1}\n") == 0,
                   "the owner's begin/commit nests; a stray commit sends "
                   "nothing");
  fake_clear(&f);

  fake_say(&f, ns, "{\"write\":19,\"node\":4,\"v\":\"gone\"}");
  CTUI_TEST_ASSERT(gone == press && !ctui_nodes_find(ns, "notify/new") &&
                       strcmp(fake_read(&f),
                              "{\"rm\":4}\n{\"ack\":19,\"err\":0}\n") == 0,
                   "a node removed in its own callback: rm, then the ack");
  fake_clear(&f);
  fake_say(&f, ns, "{\"write\":20,\"node\":4,\"v\":\"\"}");
  snprintf(want, sizeof want, "{\"ack\":20,\"err\":%d}\n", ENOENT);
  CTUI_TEST_ASSERT(strcmp(fake_read(&f), want) == 0,
                   "its id isn't reused: a late write gets ENOENT");
  fake_clear(&f);
  CTUI_NODE *again = ctui_node_publish(ns, "notify/new", &PRESS, &PRESS_OPS,
                                       NULL);
  CTUI_TEST_ASSERT(again && has(&f, "{\"pub\":5,\"path\":\"notify/new\""),
                   "the path published again gets a new id");
  fake_clear(&f);

  fake_say(&f, ns, "not json");
  fake_say(&f, ns, "{\"what\":1}");
  CTUI_TEST_ASSERT(ctui_nodes_fd(ns) >= 0 && strcmp(fake_read(&f), "") == 0,
                   "garbage from the daemon dropped, the connection kept");

  /* the daemon goes: the fd closes; it comes back: the table again */
  close(f.fd);
  f.fd = -1;
  ctui_nodes_dispatch(ns);
  CTUI_TEST_ASSERT(ctui_nodes_fd(ns) == -1, "the daemon closed: fd -1");
  ctui_node_set_float(level, 33);
  CTUI_TEST_ASSERT(ctui_nodes_connect(ns) == 0, "connect again");
  fake_accept(&f);
  CTUI_TEST_ASSERT(has(&f, "{\"hello\":\"ctui-test\"") &&
                       has(&f, "\"path\":\"volume/sink/level\"") &&
                       has(&f, "\"v\":\"33\"}") &&
                       has(&f, "\"v\":\"stack\"}") &&
                       has(&f, "{\"pub\":5,\"path\":\"notify/new\"") &&
                       !strstr(fake_read(&f), "\"pub\":4"),
                   "reconnected: the table whole, its current values, "
                   "nothing removed");
  fake_clear(&f);

  /* a line cut over two reads */
  const char *half = "{\"write\":21,\"node\":1,";
  if (write(f.fd, half, strlen(half)) < 0) {
    perror("write");
  }
  ctui_nodes_dispatch(ns);
  CTUI_TEST_ASSERT(strcmp(fake_read(&f), "") == 0, "half a line waits");
  fake_say(&f, ns, "\"v\":\"70\"}");
  CTUI_TEST_ASSERT(has(&f, "{\"ack\":21,\"err\":0}"),
                   "the rest arrives: handled");
  fake_clear(&f);

  ctui_nodes_close(ns);
  close(f.fd);
  close(f.listen_fd);
  unlink(f.path);
  rmdir(f.dir);
  fake_clear(&f);
}

int main(void) {
  ctui_log_init(E_ALL);
  test_text();
  test_paths_types();
  test_table();
  test_daemon();
  return ctui_test_summary();
}
