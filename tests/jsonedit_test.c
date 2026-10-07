/* jsonedit.c: config edits in place. Each case pins the exact text that
 * comes out, since keeping the user's comments and layout is the point. */
#define _GNU_SOURCE /* mkdtemp, symlink, lstat */
#include "json/jsonedit.h"

#include "ctui.h"
#include "ctui_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *CONFIG =
    "// my config\n"
    "{\n"
    "  \"screens\": { \"DP-1\": {} }, // screens\n"
    "  \"wm\": {\n"
    "    // input\n"
    "    \"keyboard\": { \"layout\": \"de\", \"repeat_rate\": 40 },\n"
    "    \"pointer\": { \"accel_profile\": \"flat\" },\n"
    "  },\n"
    "  \"idle\": {\n"
    "    \"lock\": 300,\n"
    "    \"suspend\": 0 // never\n"
    "  }\n"
    "}\n";

static CTUI_JSON str(const char *s) {
  return (CTUI_JSON){.type = CTUI_JSON_STRING, .string = (char *)s};
}

static CTUI_JSON num(double n) {
  return (CTUI_JSON){.type = CTUI_JSON_NUMBER, .number = n};
}

/* one edit's result against the text expected; frees it */
static int same(char *got, const char *want) {
  int ok = got && strcmp(got, want) == 0;
  if (!ok) {
    printf("---- got:\n%s\n---- want:\n%s\n----\n", got ? got : "(NULL)", want);
  }
  free(got);
  return ok;
}

#define check(got, want, what) CTUI_TEST_ASSERT(same(got, want), what)

static void test_set(void) {
  char err[256] = "";
  CTUI_JSON v = str("us");
  check(ctui_json_edit_set(CONFIG,
                           (const char *[]){"wm", "keyboard", "layout", NULL},
                           &v, err, sizeof err),
        "// my config\n"
        "{\n"
        "  \"screens\": { \"DP-1\": {} }, // screens\n"
        "  \"wm\": {\n"
        "    // input\n"
        "    \"keyboard\": { \"layout\": \"us\", \"repeat_rate\": 40 },\n"
        "    \"pointer\": { \"accel_profile\": \"flat\" },\n"
        "  },\n"
        "  \"idle\": {\n"
        "    \"lock\": 300,\n"
        "    \"suspend\": 0 // never\n"
        "  }\n"
        "}\n",
        "a value is replaced where it is, everything else kept");

  v = num(1e300);
  check(ctui_json_edit_set("{\"a\": 1}\n", (const char *[]){"a", NULL}, &v, err,
                           sizeof err),
        "{\"a\": 1e+300}\n",
        "a number past long long's range written as it is (fuzz: the cast)");

  v = num(250);
  check(ctui_json_edit_set(
            CONFIG, (const char *[]){"wm", "keyboard", "repeat_delay", NULL},
            &v, err, sizeof err),
        "// my config\n"
        "{\n"
        "  \"screens\": { \"DP-1\": {} }, // screens\n"
        "  \"wm\": {\n"
        "    // input\n"
        "    \"keyboard\": { \"layout\": \"de\", \"repeat_rate\": 40, "
        "\"repeat_delay\": 250 },\n"
        "    \"pointer\": { \"accel_profile\": \"flat\" },\n"
        "  },\n"
        "  \"idle\": {\n"
        "    \"lock\": 300,\n"
        "    \"suspend\": 0 // never\n"
        "  }\n"
        "}\n",
        "a member added to a one-line object stays on its line");

  v = (CTUI_JSON){.type = CTUI_JSON_BOOL, .boolean = 1};
  check(ctui_json_edit_set(CONFIG,
                           (const char *[]){"wm", "touchpad", "tap", NULL}, &v,
                           err, sizeof err),
        "// my config\n"
        "{\n"
        "  \"screens\": { \"DP-1\": {} }, // screens\n"
        "  \"wm\": {\n"
        "    // input\n"
        "    \"keyboard\": { \"layout\": \"de\", \"repeat_rate\": 40 },\n"
        "    \"pointer\": { \"accel_profile\": \"flat\" },\n"
        "    \"touchpad\": { \"tap\": true },\n"
        "  },\n"
        "  \"idle\": {\n"
        "    \"lock\": 300,\n"
        "    \"suspend\": 0 // never\n"
        "  }\n"
        "}\n",
        "a missing object is made, on a line of its own, trailing comma "
        "like its siblings");

  v = num(600);
  check(ctui_json_edit_set(CONFIG, (const char *[]){"idle", "screen_off", NULL},
                           &v, err, sizeof err),
        "// my config\n"
        "{\n"
        "  \"screens\": { \"DP-1\": {} }, // screens\n"
        "  \"wm\": {\n"
        "    // input\n"
        "    \"keyboard\": { \"layout\": \"de\", \"repeat_rate\": 40 },\n"
        "    \"pointer\": { \"accel_profile\": \"flat\" },\n"
        "  },\n"
        "  \"idle\": {\n"
        "    \"lock\": 300,\n"
        "    \"suspend\": 0, // never\n"
        "    \"screen_off\": 600\n"
        "  }\n"
        "}\n",
        "no trailing comma: the last member gets one before its comment, "
        "the new one none");

  v = str("x");
  check(ctui_json_edit_set(CONFIG,
                           (const char *[]){"screens", "DP-1", "a", NULL}, &v,
                           err, sizeof err),
        "// my config\n"
        "{\n"
        "  \"screens\": { \"DP-1\": { \"a\": \"x\" } }, // screens\n"
        "  \"wm\": {\n"
        "    // input\n"
        "    \"keyboard\": { \"layout\": \"de\", \"repeat_rate\": 40 },\n"
        "    \"pointer\": { \"accel_profile\": \"flat\" },\n"
        "  },\n"
        "  \"idle\": {\n"
        "    \"lock\": 300,\n"
        "    \"suspend\": 0 // never\n"
        "  }\n"
        "}\n",
        "an empty object gets its first member");

  /* a long value: a member per line, indented from its key's line */
  CTUI_JSON items[3] = {str("a long string number one"),
                        str("a long string number two"),
                        str("and a third, ünïcode kept")};
  CTUI_JSON arr = {.type = CTUI_JSON_ARRAY, .items = items, .count = 3};
  check(ctui_json_edit_set(CONFIG, (const char *[]){"idle", "lock", NULL}, &arr,
                           err, sizeof err),
        "// my config\n"
        "{\n"
        "  \"screens\": { \"DP-1\": {} }, // screens\n"
        "  \"wm\": {\n"
        "    // input\n"
        "    \"keyboard\": { \"layout\": \"de\", \"repeat_rate\": 40 },\n"
        "    \"pointer\": { \"accel_profile\": \"flat\" },\n"
        "  },\n"
        "  \"idle\": {\n"
        "    \"lock\": [\n"
        "      \"a long string number one\",\n"
        "      \"a long string number two\",\n"
        "      \"and a third, ünïcode kept\"\n"
        "    ],\n"
        "    \"suspend\": 0 // never\n"
        "  }\n"
        "}\n",
        "a long array goes an item per line");

  v = num(0.1);
  char *s = ctui_json_edit_set("{\"a\": 1}", (const char *[]){"a", NULL}, &v,
                               err, sizeof err);
  check(s, "{\"a\": 0.1}", "numbers with the fewest digits");

  v = str("x");
  s = ctui_json_edit_set(CONFIG, (const char *[]){"idle", "lock", "x", NULL},
                         &v, err, sizeof err);
  CTUI_TEST_ASSERT(!s && strstr(err, "\"lock\" isn't an object"),
                   "a path through a number is refused");
  s = ctui_json_edit_set("{\"a\": ", (const char *[]){"a", NULL}, &v, err,
                         sizeof err);
  CTUI_TEST_ASSERT(!s && strstr(err, "line 1"), "broken text: the parse error");
  s = ctui_json_edit_set("[1]", (const char *[]){"a", NULL}, &v, err,
                         sizeof err);
  CTUI_TEST_ASSERT(!s, "a top that isn't an object is refused");

  v = num(2);
  check(ctui_json_edit_set("{}", (const char *[]){"a", "b", NULL}, &v, err,
                           sizeof err),
        "{ \"a\": { \"b\": 2 } }", "into an empty file object");
  check(ctui_json_edit_set("{\n}\n", (const char *[]){"a", NULL}, &v, err,
                           sizeof err),
        "{\n  \"a\": 2\n}\n", "an empty multi-line object gets indented");
  check(ctui_json_edit_set("{ \"a\": 1, }", (const char *[]){"b", NULL}, &v,
                           err, sizeof err),
        "{ \"a\": 1, \"b\": 2, }", "one line with a trailing comma");
}

static void test_remove(void) {
  char err[256] = "";
  check(ctui_json_edit_remove(CONFIG, (const char *[]){"wm", "pointer", NULL},
                              err, sizeof err),
        "// my config\n"
        "{\n"
        "  \"screens\": { \"DP-1\": {} }, // screens\n"
        "  \"wm\": {\n"
        "    // input\n"
        "    \"keyboard\": { \"layout\": \"de\", \"repeat_rate\": 40 },\n"
        "  },\n"
        "  \"idle\": {\n"
        "    \"lock\": 300,\n"
        "    \"suspend\": 0 // never\n"
        "  }\n"
        "}\n",
        "a member with a line of its own goes with its line");

  check(ctui_json_edit_remove(CONFIG, (const char *[]){"idle", "suspend", NULL},
                              err, sizeof err),
        "// my config\n"
        "{\n"
        "  \"screens\": { \"DP-1\": {} }, // screens\n"
        "  \"wm\": {\n"
        "    // input\n"
        "    \"keyboard\": { \"layout\": \"de\", \"repeat_rate\": 40 },\n"
        "    \"pointer\": { \"accel_profile\": \"flat\" },\n"
        "  },\n"
        "  \"idle\": {\n"
        "    \"lock\": 300\n"
        "  }\n"
        "}\n",
        "the last member: its comment with it, the one before loses its "
        "comma");

  check(ctui_json_edit_remove(
            CONFIG, (const char *[]){"wm", "keyboard", "layout", NULL}, err,
            sizeof err),
        "// my config\n"
        "{\n"
        "  \"screens\": { \"DP-1\": {} }, // screens\n"
        "  \"wm\": {\n"
        "    // input\n"
        "    \"keyboard\": { \"repeat_rate\": 40 },\n"
        "    \"pointer\": { \"accel_profile\": \"flat\" },\n"
        "  },\n"
        "  \"idle\": {\n"
        "    \"lock\": 300,\n"
        "    \"suspend\": 0 // never\n"
        "  }\n"
        "}\n",
        "one line: the first member and its comma");
  check(ctui_json_edit_remove("{ \"a\": 1, \"b\": 2 }",
                              (const char *[]){"b", NULL}, err, sizeof err),
        "{ \"a\": 1 }", "one line: the last member and the comma before");
  check(ctui_json_edit_remove(CONFIG, (const char *[]){"nope", "x", NULL}, err,
                              sizeof err),
        CONFIG, "a missing member: the text as it was");
}

static void test_files(void) {
  char dir[] = "/tmp/jsonedit-test-XXXXXX";
  CTUI_TEST_ASSERT(mkdtemp(dir) != NULL, "a scratch dir");
  char path[200], link_path[200], bak[256], err[256];
  snprintf(path, sizeof path, "%s/config.json", dir);
  snprintf(link_path, sizeof link_path, "%s/link.json", dir);
  snprintf(bak, sizeof bak, "%s.bak", path);
  FILE *f = fopen(path, "w");
  fputs("{\"a\": 1} // old\n", f);
  fclose(f);
  chmod(path, 0600);
  symlink("config.json", link_path);

  CTUI_TEST_ASSERT(
      ctui_json_edit_save(link_path, "{\"a\": ", err, sizeof err) != 0,
      "text that doesn't parse isn't saved");
  CTUI_TEST_ASSERT(
      ctui_json_edit_save(link_path, "{\"a\": 2}\n", err, sizeof err) == 0,
      "saved through the symlink");
  struct stat st;
  CTUI_TEST_ASSERT(lstat(link_path, &st) == 0 && S_ISLNK(st.st_mode),
                   "the link stays a link");
  char *now = ctui_json_edit_load(path, err, sizeof err);
  char *old = ctui_json_edit_load(bak, err, sizeof err);
  CTUI_TEST_ASSERT(now && strcmp(now, "{\"a\": 2}\n") == 0,
                   "the target has the new text");
  CTUI_TEST_ASSERT(old && strcmp(old, "{\"a\": 1} // old\n") == 0,
                   "the old one is in .bak");
  CTUI_TEST_ASSERT(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600,
                   "the mode is kept");
  free(now);
  free(old);
  CTUI_TEST_ASSERT(!ctui_json_edit_load("/nonexistent/x", err, sizeof err) &&
                       strstr(err, "/nonexistent/x"),
                   "a missing file: NULL and its name");
  unlink(path);
  unlink(bak);
  unlink(link_path);
  rmdir(dir);
}

/* arrays item by item: the comments over the items kept with them */
static const char *LIST = "{\n"
                          "  \"children\": [\n"
                          "    // the clock\n"
                          "    { \"widget\": \"clock\", \"size\": 8 },\n"
                          "    // the weather,\n"
                          "    // two lines about it\n"
                          "    { \"widget\": \"weather\" },\n"
                          "    { \"widget\": \"media\" }, // no doc\n"
                          "  ],\n"
                          "  \"inline\": [1, 2, 3],\n"
                          "  \"tight\": [ { \"a\": 1 },\n"
                          "             { \"a\": 2 } ],\n"
                          "  \"none\": [],\n"
                          "}\n";

static void test_arrays(void) {
  char err[256] = "";
  const char *kids[] = {"children", NULL};
  CTUI_JSON v = num(12);
  check(ctui_json_edit_set(LIST,
                           (const char *[]){"children", "[1]", "size", NULL},
                           &v, err, sizeof err),
        "{\n"
        "  \"children\": [\n"
        "    // the clock\n"
        "    { \"widget\": \"clock\", \"size\": 8 },\n"
        "    // the weather,\n"
        "    // two lines about it\n"
        "    { \"widget\": \"weather\", \"size\": 12 },\n"
        "    { \"widget\": \"media\" }, // no doc\n"
        "  ],\n"
        "  \"inline\": [1, 2, 3],\n"
        "  \"tight\": [ { \"a\": 1 },\n"
        "             { \"a\": 2 } ],\n"
        "  \"none\": [],\n"
        "}\n",
        "a member of an item: set through [N]");
  check(ctui_json_edit_remove(LIST, (const char *[]){"children", "[1]", NULL},
                              err, sizeof err),
        "{\n"
        "  \"children\": [\n"
        "    // the clock\n"
        "    { \"widget\": \"clock\", \"size\": 8 },\n"
        "    { \"widget\": \"media\" }, // no doc\n"
        "  ],\n"
        "  \"inline\": [1, 2, 3],\n"
        "  \"tight\": [ { \"a\": 1 },\n"
        "             { \"a\": 2 } ],\n"
        "  \"none\": [],\n"
        "}\n",
        "an item removed with its comment lines");
  check(ctui_json_edit_swap(LIST, kids, 0, err, sizeof err),
        "{\n"
        "  \"children\": [\n"
        "    // the weather,\n"
        "    // two lines about it\n"
        "    { \"widget\": \"weather\" },\n"
        "    // the clock\n"
        "    { \"widget\": \"clock\", \"size\": 8 },\n"
        "    { \"widget\": \"media\" }, // no doc\n"
        "  ],\n"
        "  \"inline\": [1, 2, 3],\n"
        "  \"tight\": [ { \"a\": 1 },\n"
        "             { \"a\": 2 } ],\n"
        "  \"none\": [],\n"
        "}\n",
        "swapped, each with its comments, the commas in place");
  CTUI_JSON w = {.type = CTUI_JSON_OBJECT, .count = 1};
  CTUI_JSON wv = str("viz");
  char *wk = "widget";
  w.items = &wv;
  w.keys = &wk;
  check(ctui_json_edit_insert(LIST, kids, 1, &w, err, sizeof err),
        "{\n"
        "  \"children\": [\n"
        "    // the clock\n"
        "    { \"widget\": \"clock\", \"size\": 8 },\n"
        "    { \"widget\": \"viz\" },\n"
        "    // the weather,\n"
        "    // two lines about it\n"
        "    { \"widget\": \"weather\" },\n"
        "    { \"widget\": \"media\" }, // no doc\n"
        "  ],\n"
        "  \"inline\": [1, 2, 3],\n"
        "  \"tight\": [ { \"a\": 1 },\n"
        "             { \"a\": 2 } ],\n"
        "  \"none\": [],\n"
        "}\n",
        "inserted before an item and its comment");
  char *got = ctui_json_edit_insert(LIST, kids, 3, &w, err, sizeof err);
  CTUI_TEST_ASSERT(got &&
                       strstr(got, "    { \"widget\": \"media\" }, // no "
                                   "doc\n    { \"widget\": \"viz\" },\n  ],"),
                   "appended after the last, the trailing comma style kept");
  free(got);
  CTUI_JSON four = num(4), zero = num(0);
  got = ctui_json_edit_insert(LIST, (const char *[]){"inline", NULL}, 3, &four,
                              err, sizeof err);
  char *got2 = ctui_json_edit_insert(LIST, (const char *[]){"inline", NULL}, 0,
                                     &zero, err, sizeof err);
  CTUI_TEST_ASSERT(got && strstr(got, "[1, 2, 3, 4]") && got2 &&
                       strstr(got2, "[0, 1, 2, 3]"),
                   "an inline list: at the end, at the start");
  free(got);
  free(got2);
  got = ctui_json_edit_remove(LIST, (const char *[]){"inline", "[2]", NULL},
                              err, sizeof err);
  got2 = ctui_json_edit_swap(LIST, (const char *[]){"inline", NULL}, 1, err,
                             sizeof err);
  CTUI_TEST_ASSERT(got && strstr(got, "[1, 2]") && got2 &&
                       strstr(got2, "[1, 3, 2]"),
                   "an inline list: the last removed, two swapped");
  free(got);
  free(got2);
  CTUI_JSON three = {.type = CTUI_JSON_OBJECT, .count = 1};
  CTUI_JSON tv = num(3);
  char *ak = "a";
  three.items = &tv;
  three.keys = &ak;
  got = ctui_json_edit_insert(LIST, (const char *[]){"tight", NULL}, 2, &three,
                              err, sizeof err);
  got2 = ctui_json_edit_swap(LIST, (const char *[]){"tight", NULL}, 0, err,
                             sizeof err);
  CTUI_TEST_ASSERT(got &&
                       strstr(got, "{ \"a\": 2 },\n             "
                                   "{ \"a\": 3 } ],") &&
                       got2 &&
                       strstr(got2, "[ { \"a\": 2 },\n             "
                                    "{ \"a\": 1 } ],"),
                   "the closing bracket on the last item's line");
  free(got);
  free(got2);
  got = ctui_json_edit_insert(LIST, (const char *[]){"none", NULL}, 0, &four,
                              err, sizeof err);
  CTUI_TEST_ASSERT(got && strstr(got, "\"none\": [ 4 ],"),
                   "into an empty list");
  free(got);
  CTUI_TEST_ASSERT(
      !ctui_json_edit_swap(LIST, kids, 2, err, sizeof err) &&
          !ctui_json_edit_insert(LIST, kids, 9, &four, err, sizeof err) &&
          !ctui_json_edit_set(LIST, (const char *[]){"children", "[7]", NULL},
                              &four, err, sizeof err) &&
          !ctui_json_edit_set(LIST, (const char *[]){"children", "x", NULL},
                              &four, err, sizeof err) &&
          !ctui_json_edit_insert(LIST, (const char *[]){"none", "x", NULL}, 0,
                                 &four, err, sizeof err),
      "past the end, a key into a list, no list: refused");
  got = ctui_json_edit_remove(LIST, (const char *[]){"children", "[9]", NULL},
                              err, sizeof err);
  CTUI_TEST_ASSERT(got && strcmp(got, LIST) == 0,
                   "removing what isn't there: the text as it was");
  free(got);

  /* several items a line (the launcher's "pinned") */
  static const char PINS[] = "{\n  \"pinned\": [\"a\", \"b\",\n"
                             "             \"c\"],\n}\n";
  check(ctui_json_edit_remove(PINS, (const char *[]){"pinned", "[1]", NULL},
                              err, sizeof err),
        "{\n  \"pinned\": [\"a\",\n             \"c\"],\n}\n",
        "the last item on its line: no blank left at the line's end");
  check(ctui_json_edit_remove(PINS, (const char *[]){"pinned", "[2]", NULL},
                              err, sizeof err),
        "{\n  \"pinned\": [\"a\", \"b\"],\n}\n",
        "the list's last item on a line of its own: ] moves up");
}

int main(void) {
  ctui_log_init(E_ALL);
  test_set();
  test_remove();
  test_arrays();
  test_files();
  ctui_log_shutdown();
  return ctui_test_summary();
}
