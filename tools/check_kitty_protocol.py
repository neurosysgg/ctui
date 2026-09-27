#!/usr/bin/env python3
"""Checks core/kitty.c's command builder against kitty itself.

    tools/check_kitty_protocol.py [--kitty-src DIR] [--kitty KITTY] [--random N]

Dev-time only (nothing of kitty enters ctui). Two checks, exit 1 on any
difference:
- drift: the key tables in src/core/kitty.c (KFLAG/KUINT/KINT lines)
  against the keymaps kitty generates its parsers from
  (DIR/gen/apc_parsers.py, default ../kitty): graphics and drag and drop.
  A protocol change in a new kitty shows here as a diff.
- oracle: a small C program builds commands with the builder -- N random
  valid transmissions/placements/deletes, and every refusal the builder
  knows -- and kitty's own graphics parser (`kitty +runpy`, a Screen as
  kitty's tests use) takes them with q=0: each accepted command must
  answer OK, each refused one must fail in kitty with the same error code
  (or, for a malformed command, get no reply at all).
"""

import argparse
import ast
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


# --- drift ---

def kitty_keymaps(src):
    """the keymaps in apc_parsers.py's parsers(), in order: graphics,
    multicell, dnd; each {key: ('flag', set) | ('uint'|'int', None)}"""
    with open(os.path.join(src, 'gen', 'apc_parsers.py'), encoding='utf-8') as f:
        tree = ast.parse(f.read())
    maps = []
    for node in ast.walk(tree):
        if (isinstance(node, (ast.Assign, ast.AnnAssign)) and
                isinstance(node.value, ast.Dict)):
            target = node.targets[0] if isinstance(node, ast.Assign) else node.target
            if getattr(target, 'id', '') != 'keymap':
                continue
            m = {}
            for k, v in zip(node.value.keys, node.value.values):
                kind = v.elts[1]
                if isinstance(kind, ast.Call):  # flag('...')
                    m[k.value] = ('flag', set(kind.args[0].value))
                else:
                    m[k.value] = (kind.value, None)
            maps.append((node.lineno, m))
    maps.sort()
    return [m for _, m in maps]


def our_tables():
    with open(os.path.join(ROOT, 'src', 'core', 'kitty.c'), encoding='utf-8') as f:
        text = f.read()
    tables = {}
    for name, body in re.findall(r'static const KITTY_KEY (\w+)_keys\[\] = \{(.*?)\};',
                                 text, re.S):
        m = {}
        for kind, key, flags in re.findall(
                r'K(FLAG|UINT|INT)\(\w+, (\w)(?:, "([^"]*)")?\)', body):
            m[key] = ('flag', set(flags)) if kind == 'FLAG' else (kind.lower(), None)
        tables[name] = m
    return tables


def diff(name, theirs, ours):
    out = []
    for k in sorted(set(theirs) | set(ours)):
        if k not in ours:
            out.append(f'{name}: kitty has key {k!r} {theirs[k]}, we don\'t')
        elif k not in theirs:
            out.append(f'{name}: we have key {k!r}, kitty doesn\'t')
        elif theirs[k] != ours[k]:
            out.append(f'{name}: key {k!r}: kitty {theirs[k]}, ours {ours[k]}')
    return out


def check_drift(src):
    maps = kitty_keymaps(src)
    if len(maps) != 3:
        return [f'expected 3 keymaps in apc_parsers.py, found {len(maps)}']
    graphics, _multicell, dnd = maps
    ours = our_tables()
    return diff('graphics', graphics, ours['gfx']) + diff('dnd', dnd, ours['dnd'])


# --- oracle ---

# cases: "EXPECT\\thex,hex,...": the escapes in order, the reply to the last
# one counted. EXPECT is OK, a kitty error code, NOREPLY (kitty's parser
# refuses it: no reply), or OURS (a refusal kitty doesn't make, see its
# comment: not sent to kitty)
DRIVER_C = r'''
#define _POSIX_C_SOURCE 200809L
#include "ctui.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static char dir[256];
static int ncase;

static void hex(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    printf("%02x", (unsigned char)s[i]);
  }
}

/* the escape kitty would get, whatever the builder thinks of it */
static size_t raw(const CTUI_KITTY_GFX *g, const void *p, size_t n,
                  char *out, size_t cap) {
  size_t len = ctui_kitty_gfx_head(g, n > 0, out, cap);
  len += ctui_util_base64_encode(p, n, out + len, cap - len);
  memcpy(out + len, "\x1b\\", 2);
  return len + 2;
}

static char *big;
#define BIG (8u << 20)

/* one case: setup escapes (built, must pass), then g; the builder's
 * verdict decides what kitty must say */
static void emit(const CTUI_KITTY_GFX *setup, int nsetup,
                 const void *const *spay, const size_t *slen,
                 const CTUI_KITTY_GFX *g, const void *p, size_t n,
                 const char *ours_only) {
  const char *err = ctui_kitty_gfx_check(g, p, n);
  if (ours_only) {
    printf("OURS\t%s\n", err ? "refused" : "ACCEPTED");
    return;
  }
  char code[32] = "OK";
  if (err) {
    size_t k = strcspn(err, ":");
    snprintf(code, sizeof code, "%.*s", (int)k, err);
    /* refused without a reply: by the parser; with no id to key a reply
     * to; "Image too large", whose reply kitty 0.49 keys to the load it
     * hasn't started yet (id 0) */
    if (strstr(err, "Malformed") || strstr(err, "without image id") ||
        (!g->i && !g->I) || strstr(err, "Image too large")) {
      snprintf(code, sizeof code, "NOREPLY");
    }
  } else if (g->a == 'd') {
    snprintf(code, sizeof code, "NOREPLY"); /* a delete never says OK */
  }
  printf("%s\t", code);
  for (int i = 0; i < nsetup; i++) {
    const char *e2 = NULL;
    size_t k = ctui_kitty_gfx_build(&setup[i], spay[i], slen[i], big, BIG, &e2);
    if (k == 0) {
      fprintf(stderr, "setup refused: %s\n", e2);
      exit(2);
    }
    hex(big, k);
    putchar(',');
  }
  hex(big, raw(g, p, n, big, BIG));
  putchar('\n');
  ncase++;
}

#define ONE(g, p, n) emit(NULL, 0, NULL, NULL, &(g), (p), (n), NULL)

static unsigned rnd(unsigned n) { return (unsigned)rand() % n; }

/* a file (t=f) or shm object (t=s) holding n bytes; its name into name */
static void make_file(char t, size_t n, char *name, size_t cap) {
  static int seq;
  int fd;
  if (t == 's') {
    snprintf(name, cap, "/ctui-kp-%d-%d", (int)getpid(), seq++);
    fd = shm_open(name, O_CREAT | O_RDWR | O_TRUNC, 0600);
  } else {
    snprintf(name, cap, "%s/f%d", dir, seq++);
    fd = open(name, O_CREAT | O_RDWR | O_TRUNC, 0600);
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)i;
    if (write(fd, &c, 1) != 1) {
      exit(3);
    }
  }
  close(fd);
}

int main(int argc, char **argv) {
  int nrand = argc > 2 ? atoi(argv[2]) : 500;
  snprintf(dir, sizeof dir, "%s", argv[1]);
  big = malloc(BIG);
  static unsigned char px[1 << 20];
  for (size_t i = 0; i < sizeof px; i++) {
    px[i] = (unsigned char)(i * 7);
  }
  srand(1);
  char name[4096];

  /* random valid transmissions: direct raw, compressed, file, shm */
  for (int k = 0; k < nrand; k++) {
    CTUI_KITTY_GFX g = {.a = rnd(2) ? 't' : 'T', .i = 1 + rnd(1000)};
    g.f = rnd(2) ? 24 : 32;
    g.s = 1 + rnd(40);
    g.v = 1 + rnd(40);
    size_t need = (size_t)g.s * g.v * (g.f == 24 ? 3 : 4);
    if (g.a == 'T') {
      g.c = rnd(5);
      g.r = rnd(5);
      g.C = rnd(2);
      g.z = (int32_t)rnd(2000) - 1000;
      g.X = rnd(3);
      g.Y = rnd(3);
      g.p = rnd(3);
    }
    switch (rnd(4)) {
    case 0:
      ONE(g, px, need + rnd(11)); /* up to kitty's 10 spare bytes */
      break;
    case 1: {
      g.o = 'z';
      size_t zn = 0;
      unsigned char *z = ctui_deflate_compress(px, need, &zn);
      ONE(g, z, zn);
      free(z);
      break;
    }
    default:
      g.t = rnd(2) ? 'f' : 's';
      g.S = rnd(2) ? (uint32_t)need : 0;
      make_file(g.t, need, name, sizeof name);
      ONE(g, name, strlen(name));
    }
  }

  /* refusals, one each (id 7 so kitty replies) */
  CTUI_KITTY_GFX g;
  g = (CTUI_KITTY_GFX){.a = 't', .i = 7, .I = 3, .f = 32, .s = 1, .v = 1};
  ONE(g, px, 4);
  g = (CTUI_KITTY_GFX){.a = 'q', .f = 32, .s = 1, .v = 1};
  ONE(g, px, 4);
  g = (CTUI_KITTY_GFX){.a = 't', .i = 7, .f = 99, .s = 1, .v = 1};
  ONE(g, px, 4);
  g = (CTUI_KITTY_GFX){.a = 't', .i = 7, .f = 32, .s = 10001, .v = 1};
  ONE(g, px, 4);
  g = (CTUI_KITTY_GFX){.a = 't', .i = 7, .f = 24, .s = 0, .v = 4};
  ONE(g, px, 4);
  g = (CTUI_KITTY_GFX){.a = 't', .i = 7, .f = 32, .s = 4, .v = 4};
  ONE(g, px, 63);
  ONE(g, px, 64 + 11);
  g.o = 'z';
  ONE(g, px, 64 + 1025);
  g = (CTUI_KITTY_GFX){.a = 't', .i = 7, .f = 32, .s = 1, .v = 1, .t = 'f'};
  memset(name, 'a', 2049);
  name[0] = '/';
  ONE(g, name, 2049);
  ONE(g, "", 0);
  make_file('f', 3, name, sizeof name);
  g.S = 3;
  ONE(g, name, strlen(name));
  g = (CTUI_KITTY_GFX){.a = 't', .i = 7, .f = 32, .s = 1, .v = 1, .t = 's'};
  ONE(g, "no-slash", 8);
  g = (CTUI_KITTY_GFX){.a = 't', .i = 7, .f = 100, .t = 'f', .S = 400000001};
  make_file('f', 8, name, sizeof name);
  ONE(g, name, strlen(name));
  g = (CTUI_KITTY_GFX){.a = 'p', .c = 1, .r = 1};
  ONE(g, NULL, 0);
  g = (CTUI_KITTY_GFX){.a = 'x', .i = 7};
  ONE(g, NULL, 0);

  /* placements against a transmitted image: a plain put, a virtual one
   * with a parent, one that is its own parent */
  CTUI_KITTY_GFX setup[2] = {
      {.a = 't', .i = 5, .f = 32, .s = 2, .v = 2, .q = 2},
      {.a = 'p', .i = 5, .p = 3, .q = 2},
  };
  const void *sp[2] = {px, NULL};
  size_t sl[2] = {16, 0};
  g = (CTUI_KITTY_GFX){.a = 'p', .i = 5, .p = 4, .c = 2, .r = 1, .z = -1};
  emit(setup, 1, sp, sl, &g, NULL, 0, NULL);
  g = (CTUI_KITTY_GFX){.a = 'p', .i = 5, .p = 4, .U = 1, .P = 5};
  emit(setup, 2, sp, sl, &g, NULL, 0, NULL);
  g = (CTUI_KITTY_GFX){.a = 'p', .i = 5, .p = 3, .P = 5, .Q = 3};
  emit(setup, 2, sp, sl, &g, NULL, 0, NULL);
  g = (CTUI_KITTY_GFX){.a = 'd', .d = 'I', .i = 5};
  emit(setup, 2, sp, sl, &g, NULL, 0, NULL);

  /* ours only: INT32_MIN (kitty computes 0 - INT32_MIN), an escape past
   * kitty's length limit (it drops the code) */
  g = (CTUI_KITTY_GFX){.a = 'p', .i = 5, .z = INT32_MIN};
  emit(NULL, 0, NULL, NULL, &g, NULL, 0, "INT32_MIN");
  const char *err = NULL;
  g = (CTUI_KITTY_GFX){.a = 't', .i = 7, .f = 32, .s = 300, .v = 200};
  printf("OURS\t%s\n",
         ctui_kitty_gfx_build(&g, px, 240000, big, BIG, &err) == 0 &&
                 strncmp(err, "E2BIG", 5) == 0
             ? "refused"
             : "ACCEPTED");
  fprintf(stderr, "%d cases\n", ncase);
  return 0;
}
'''

KITTY_PY = r'''
import sys
from kitty.config import finalize_keys, finalize_mouse_mappings
from kitty.fast_data_types import Screen, set_options
from kitty.options.types import Options, defaults
o = Options(defaults._asdict())
finalize_keys(o, {})
finalize_mouse_mappings(o, {})
set_options(o)

class CB:
    def __init__(self):
        self.buf = b''
    def write(self, data):
        self.buf += bytes(data)
    def __getattr__(self, name):
        return lambda *a, **k: None

def feed(screen, data):
    data = memoryview(data)
    while data:
        dest = screen.test_create_write_buffer()
        n = screen.test_commit_write_buffer(data, dest)
        data = data[n:]
        screen.test_parse_written_data(None)

for line in sys.stdin:
    escapes = [bytes.fromhex(h) for h in line.split(',')]
    cb = CB()
    s = Screen(cb, 24, 80, 0, 10, 20, 0, cb)
    for e in escapes[:-1]:
        feed(s, e)
    cb.buf = b''
    feed(s, escapes[-1])
    r = cb.buf.decode('latin-1')
    if not r:
        print('NOREPLY')
    else:
        body = r.split(';', 1)[1].split('\x1b', 1)[0]
        print(body.split(':', 1)[0] + '\t' + body)
'''


def check_oracle(kitty, n_random):
    with tempfile.TemporaryDirectory() as d:
        src, exe = os.path.join(d, 'drv.c'), os.path.join(d, 'drv')
        with open(src, 'w') as f:
            f.write(DRIVER_C)
        core = glob.glob(os.path.join(ROOT, 'src', '*.c')) + glob.glob(
            os.path.join(ROOT, 'src', 'core', '*.c'))
        subprocess.run(['cc', '-std=c11', '-O1', '-I', os.path.join(ROOT, 'src'),
                        '-o', exe, src, *core, '-lm'], check=True)
        out = subprocess.run([exe, d, str(n_random)], check=True,
                             capture_output=True, text=True).stdout.splitlines()
        cases = [line.split('\t', 1) for line in out]
        ours = [c for c in cases if c[0] == 'OURS']
        sent = [c for c in cases if c[0] != 'OURS']
        replies = subprocess.run([kitty, '+runpy', KITTY_PY],
                                 input='\n'.join(c[1] for c in sent) + '\n',
                                 check=True, capture_output=True,
                                 text=True).stdout.splitlines()
    bad = [f'ours-only refusal accepted: case {i}' for i, c in enumerate(ours)
           if c[1] != 'refused']
    for (want, esc), got in zip(sent, replies):
        code = got.split('\t', 1)[0]
        if code != want:
            first = bytes.fromhex(esc.split(',')[-1])[:120]
            bad.append(f'builder says {want}, kitty says {got!r}: {first!r}')
    return bad, len(sent) + len(ours)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kitty-src', default=os.path.join(ROOT, '..', 'kitty'))
    ap.add_argument('--kitty', default='kitty')
    ap.add_argument('--random', type=int, default=500)
    a = ap.parse_args()
    drift = check_drift(a.kitty_src)
    for line in drift:
        print(line)
    if not drift:
        print(f'key tables match {os.path.normpath(a.kitty_src)}/gen/apc_parsers.py')
    bad, n = check_oracle(a.kitty, a.random)
    for line in bad[:40]:
        print(line)
    ver = subprocess.run([a.kitty, '--version'], capture_output=True,
                         text=True).stdout.strip()
    if not bad:
        print(f'all {n} commands: the builder and {ver} agree')
    return 1 if drift or bad else 0


if __name__ == '__main__':
    sys.exit(main())
