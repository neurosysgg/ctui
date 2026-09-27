#!/usr/bin/env python3
"""Compares ctui_utf8_cpwidth() with kitty's own wcwidth, every codepoint.

    tools/check_widths.py [--kitty KITTY]

Dev-time only (needs a kitty binary; nothing of kitty enters ctui): builds
a tiny program printing ctui's width for U+0000..U+10FFFF, asks kitty
(`kitty +runpy`, kitty.fast_data_types.wcwidth) the same, and lists every
codepoint where the cells kitty advances differ from ctui's answer. kitty
answers 2, 0, -1 (invalid: kitty draws nothing, ctui sends U+FFFD, so 1)
or a negative class (ambiguous/private use/unassigned, drawn 1 cell).
Exit 1 on any mismatch.
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
int main(void) {
  for (uint32_t cp = 0; cp < 0x110000; cp++) {
    putchar('0' + ctui_utf8_cpwidth(cp));
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


def ctui_widths():
    with tempfile.TemporaryDirectory() as d:
        src, exe = os.path.join(d, 'dump.c'), os.path.join(d, 'dump')
        with open(src, 'w') as f:
            f.write(DUMP_C)
        core = glob.glob(os.path.join(ROOT, 'src', '*.c')) + glob.glob(
            os.path.join(ROOT, 'src', 'core', '*.c'))
        subprocess.run(['cc', '-std=c11', '-O1', '-I', os.path.join(ROOT, 'src'),
                        '-o', exe, src, *core, '-lm'], check=True)
        return subprocess.run([exe], check=True, capture_output=True,
                              text=True, cwd=d).stdout


def kitty_widths(kitty):
    r = subprocess.run([kitty, '+runpy', KITTY_PY], check=True,
                       capture_output=True, text=True)
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kitty', default='kitty')
    a = ap.parse_args()
    ours, theirs = ctui_widths(), kitty_widths(a.kitty)
    if len(ours) != 0x110000 or len(theirs) != 0x110000:
        print('short output', len(ours), len(theirs), file=sys.stderr)
        return 1
    bad = [cp for cp in range(0x110000) if ours[cp] != theirs[cp]]
    for cp in bad[:50]:
        print(f'U+{cp:04X}: ctui {ours[cp]}, kitty {theirs[cp]}')
    if bad:
        print(f'{len(bad)} codepoints differ', file=sys.stderr)
        return 1
    ver = subprocess.run([a.kitty, '--version'], capture_output=True,
                         text=True).stdout.strip()
    print(f'all 1114112 codepoints agree with {ver}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
