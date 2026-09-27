#!/usr/bin/env python3
"""Compares ctui's widths and grapheme clusters with a running kitty.

    tools/check_widths.py [--kitty KITTY] [--random N]

Dev-time only (needs a kitty binary; nothing of kitty enters ctui). Two
checks, exit 1 on any mismatch:
- codepoints: ctui_utf8_cpwidth() for U+0000..U+10FFFF against kitty's
  own wcwidth (kitty.fast_data_types). kitty answers 2, 0, -1 (invalid:
  kitty draws nothing, ctui sends U+FFFD, so 1) or a negative class
  (ambiguous/private use/unassigned, drawn 1 cell).
- clusters: every GraphemeBreakTest.txt sequence (from the UCD cache
  tools/gen_utf8_props.py keeps), hand-picked emoji/Indic/Hangul cases
  and N random mixes of tricky codepoints are split by
  ctui_utf8_cluster(); what ctui would send for them is drawn by kitty's
  own Screen, and kitty's cells must be ctui's clusters: the same widths
  in the same order, the same text.
"""

import argparse
import glob
import os
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

DUMP_C = r'''
#include "core/utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc < 2) {
    for (uint32_t cp = 0; cp < 0x110000; cp++) {
      putchar('0' + ctui_utf8_cpwidth(cp));
    }
    return 0;
  }
  /* clusters: a line of hex codepoints in, "width:hex hex;..." out */
  char line[4096];
  while (fgets(line, sizeof line, stdin)) {
    char s[4096];
    size_t n = 0;
    for (char *t = strtok(line, " \n"); t; t = strtok(NULL, " \n")) {
      n += (size_t)ctui_utf8_encode((uint32_t)strtoul(t, NULL, 16), s + n);
    }
    s[n] = 0;
    const char *p = s;
    size_t k;
    uint32_t ch;
    int w;
    while ((k = ctui_utf8_cluster(p, (size_t)-1, &ch, &w)) > 0) {
      p += k;
      if (w == 0) {
        continue;
      }
      printf("%d:", w);
      int m;
      const uint32_t *cps = ctui_cell_cluster(ch, &m);
      if (cps == NULL) {
        printf("%x", ch);
      }
      for (int i = 0; i < m; i++) {
        printf(i ? " %x" : "%x", cps[i]);
      }
      putchar(';');
    }
    putchar('\n');
  }
  return 0;
}
'''

KITTY_PY = r'''
import sys
from kitty.fast_data_types import wcwidth
out = []
for cp in range(0x110000):
    # NUL is 0 in kitty's table but invalid where kitty draws (screen.c
    # skips it); ctui sends U+FFFD for it like for any control character
    w = 1 if cp == 0 else wcwidth(cp)
    out.append('2' if w == 2 else '0' if w == 0 else '1')
sys.stdout.write(''.join(out))
'''


KITTY_SCREEN_PY = r'''
import sys
from kitty.config import finalize_keys, finalize_mouse_mappings
from kitty.fast_data_types import Screen, set_options
from kitty.options.types import Options, defaults
o = Options(defaults._asdict())
finalize_keys(o, {})
finalize_mouse_mappings(o, {})
set_options(o)
class CB:
    def __getattr__(self, name):
        return lambda *a, **k: None
for line in sys.stdin:
    text = ''.join(chr(int(x, 16)) for x in line.split())
    s = Screen(CB(), 2, 400, 0, 10, 20, 0, CB())
    s.draw(text)
    l = s.line(0)
    ws = [l.width(i) for i in range(s.cursor.x) if l.width(i)]
    print(' '.join(map(str, ws)) + '\t' + ' '.join(f'{ord(c):x}' for c in str(l)))
'''

POOL = [0x61, 0x65, 0x20, 0x31, 0x301, 0x20E3, 0x200D, 0xFE0F, 0xFE0E, 0x200B,
        0xAD, 0x1F1E9, 0x1F1EA, 0x1F1EB, 0x1F44D, 0x1F3FD, 0x1F468, 0x1F469,
        0x2764, 0x261D, 0x231A, 0x1F600, 0x4E00, 0xE01, 0xE33, 0x1100, 0x1161,
        0x11A8, 0xAC00, 0xAC01, 0x915, 0x94D, 0x937, 0x93F, 0xD4E, 0xD15,
        0x600, 0x661, 0xE000, 0x378, 0x9, 0xA, 0xD, 0x7F, 0x85, 0xFFFE,
        0xE0067, 0x1F3F4, 0x1F9D1, 0x1F91D]

HAND = [
    '1F1E9 1F1EA', '1F1E9 1F1EA 1F1EB', '1F44D 1F3FD', '1F468 200D 1F469 200D 1F467',
    '2764 FE0F', '2764', '231A FE0E', '31 FE0F 20E3', '65 301', '65 301 302 303',
    'E01 E33', '1100 1161 11A8', 'AC00 11A8', 'D4E D15', '915 94D 937',
    '1F3F4 E0067 E0062 E0065 E006E E0067 E007F', '1F9D1 1F3FD 200D 1F91D 200D 1F9D1 1F3FF',
    '301 61', '200D 1F600', '61 200B 62', 'FE0F', '61 FE0F', '261D FE0F', '261D FE0E',
]


def ucd_tests():
    path = os.path.expanduser('~/.cache/ctui/ucd-17.0.0/GraphemeBreakTest.txt')
    if not os.path.exists(path):
        print(f'{path} missing: run tools/gen_utf8_props.py once',
              file=sys.stderr)
        return []
    out = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.split('#', 1)[0].strip()
            if line:
                cps = [x for x in line.replace('÷', ' ').replace('×', ' ').split()]
                if '0000' not in cps:
                    out.append(' '.join(cps))
    return out


def check_clusters(exe, kitty, n_random):
    import random
    rnd = random.Random(1)
    tests = ucd_tests() + HAND + [
        ' '.join(f'{rnd.choice(POOL):x}' for _ in range(rnd.randint(1, 8)))
        for _ in range(n_random)]
    ours = subprocess.run([exe, 'clusters'], input='\n'.join(tests) + '\n',
                          check=True, capture_output=True, text=True).stdout
    ours = ours.splitlines()
    sent = []
    for line in ours:
        cells = [c.split(':') for c in line.split(';') if c]
        sent.append(' '.join(c[1] for c in cells))
    theirs = subprocess.run([kitty, '+runpy', KITTY_SCREEN_PY],
                            input='\n'.join(sent) + '\n', check=True,
                            capture_output=True, text=True).stdout.splitlines()
    bad = 0
    for t, o, s_, k in zip(tests, ours, sent, theirs):
        cells = [c.split(':') for c in o.split(';') if c]
        widths = ' '.join(c[0] for c in cells)
        kw, _, ktext = k.partition('\t')
        text = ' '.join(f'{ord(c):x}' for c in
                        ''.join(chr(int(x, 16)) for x in s_.split()))
        if kw != widths or ktext.split() != text.split():
            bad += 1
            if bad <= 30:
                print(f'[{t}] ctui {o!r}, kitty widths {kw!r} text {ktext!r}')
    if bad:
        print(f'{bad} of {len(tests)} cluster tests differ', file=sys.stderr)
    return bad == 0, len(tests)


def build(d):
    src, exe = os.path.join(d, 'dump.c'), os.path.join(d, 'dump')
    with open(src, 'w') as f:
        f.write(DUMP_C)
    core = glob.glob(os.path.join(ROOT, 'src', '*.c')) + glob.glob(
        os.path.join(ROOT, 'src', 'core', '*.c'))
    subprocess.run(['cc', '-std=c11', '-O1', '-I', os.path.join(ROOT, 'src'),
                    '-o', exe, src, *core, '-lm'], check=True)
    return exe


def ctui_widths(exe):
    return subprocess.run([exe], check=True, capture_output=True,
                          text=True).stdout


def kitty_widths(kitty):
    r = subprocess.run([kitty, '+runpy', KITTY_PY], check=True,
                       capture_output=True, text=True)
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kitty', default='kitty')
    ap.add_argument('--random', type=int, default=5000)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as d:
        exe = build(d)
        ours, theirs = ctui_widths(exe), kitty_widths(a.kitty)
        ok, n = check_clusters(exe, a.kitty, a.random)
    if len(ours) != 0x110000 or len(theirs) != 0x110000:
        print('short output', len(ours), len(theirs), file=sys.stderr)
        return 1
    bad = [cp for cp in range(0x110000) if ours[cp] != theirs[cp]]
    for cp in bad[:50]:
        print(f'U+{cp:04X}: ctui {ours[cp]}, kitty {theirs[cp]}')
    if bad:
        print(f'{len(bad)} codepoints differ', file=sys.stderr)
    ver = subprocess.run([a.kitty, '--version'], capture_output=True,
                         text=True).stdout.strip()
    if not bad:
        print(f'all 1114112 codepoints agree with {ver}')
    if ok:
        print(f'all {n} cluster tests agree with {ver}')
    return 0 if ok and not bad else 1


if __name__ == '__main__':
    sys.exit(main())
