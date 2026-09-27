#!/usr/bin/env python3
"""Generates src/core/utf8_width.h: the column width of every codepoint,
the way kitty decides it (kitty is ctui's reference terminal), from the
Unicode Character Database.

    tools/gen_widths.py [--ucd DIR] [--check]

--ucd reads UnicodeData.txt, EastAsianWidth.txt, PropList.txt and
emoji-sequences.txt from DIR; without it they are fetched once for
UNICODE_VERSION into ~/.cache/ctui/ucd-VERSION/. --check exits 1 when the
checked-in header differs from what the data gives (drift).

The rules, first match wins (written from the Unicode data and kitty's
documented behaviour; tools/check_widths.py compares the result with a
running kitty codepoint by codepoint):
  2  regional indicators, East Asian Wide/Fullwidth (plus the CJK blocks'
     unassigned codepoints, which UAX #11 defaults to W), and emoji whose
     default presentation is emoji (a Basic_Emoji entry without FE0F, the
     base of a flag, tag or modifier sequence)
  0  combining marks (M*), format characters (Cf), Default_Ignorable
     codepoints, U+0000 (the skin tone modifiers are Basic_Emoji: 2 alone)
  1  everything else, including ambiguous, private use and unassigned
Invalid codepoints (Cc, surrogates, noncharacters) are left at 1:
kitty draws nothing for them, ctui_screen_flush() sends U+FFFD instead,
which is 1 cell.
"""

import argparse
import os
import sys
import urllib.request

UNICODE_VERSION = '17.0.0'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src',
                   'core', 'utf8_width.h')
FILES = {
    'UnicodeData.txt': 'ucd',
    'EastAsianWidth.txt': 'ucd',
    'PropList.txt': 'ucd',
    'emoji-sequences.txt': 'emoji',
}


def ucd_dir(arg):
    if arg:
        return arg
    d = os.path.expanduser(f'~/.cache/ctui/ucd-{UNICODE_VERSION}')
    os.makedirs(d, exist_ok=True)
    for name, sub in FILES.items():
        path = os.path.join(d, name)
        if not os.path.exists(path):
            url = f'https://www.unicode.org/Public/{UNICODE_VERSION}/{sub}/{name}'
            with urllib.request.urlopen(url) as r, open(path + '.tmp', 'wb') as f:
                f.write(r.read())
            os.rename(path + '.tmp', path)
    return d


def lines(d, name):
    with open(os.path.join(d, name), encoding='utf-8') as f:
        for line in f:
            line = line.split('#', 1)[0].strip()
            if line:
                yield line


def cps(spec):
    spec = spec.strip()
    if '..' in spec:
        a, b = spec.split('..')
        return range(int(a, 16), int(b, 16) + 1)
    return range(int(spec, 16), int(spec, 16) + 1)


def widths(d):
    category = {}
    first = None
    for line in lines(d, 'UnicodeData.txt'):
        f = line.split(';')
        cp, name, cat = int(f[0], 16), f[1], f[2]
        if name.endswith(', First>'):
            first = cp
            continue
        for c in range(first if first is not None else cp, cp + 1):
            category[c] = cat
        first = None

    wide, seen = set(), set()
    for line in lines(d, 'EastAsianWidth.txt'):
        spec, eaw = (x.strip() for x in line.split(';')[:2])
        r = cps(spec)
        seen.update(r)
        if eaw in ('W', 'F'):
            wide.update(r)
    for a, b in ((0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xF900, 0xFAFF),
                 (0x20000, 0x2FFFD), (0x30000, 0x3FFFD)):
        wide.update(set(range(a, b + 1)) - seen)

    emoji = set()
    for line in lines(d, 'emoji-sequences.txt'):
        spec, kind = (x.strip() for x in line.split(';')[:2])
        parts = spec.split()
        if kind == 'Basic_Emoji' and len(parts) == 1:
            emoji.update(cps(parts[0]))
        elif kind == 'RGI_Emoji_Flag_Sequence':
            emoji.update(int(p, 16) for p in parts)
        elif kind in ('RGI_Emoji_Tag_Sequence',
                      'RGI_Emoji_Modifier_Sequence'):
            emoji.add(int(parts[0], 16))

    ignorable = set()
    for line in lines(d, 'PropList.txt'):
        spec, prop = (x.strip() for x in line.split(';')[:2])
        if prop == 'Other_Default_Ignorable_Code_Point':
            ignorable.update(cps(spec))

    regional = set(range(0x1F1E6, 0x1F1FF + 1))
    zero = ({c for c, cat in category.items() if cat[0] == 'M' or cat == 'Cf'}
            | ignorable | {0})
    invalid = ({c for c, cat in category.items() if cat in ('Cc', 'Cs')}
               | set(range(0xFDD0, 0xFDF0))
               | {p | 0xFFFE for p in range(0, 0x110000, 0x10000)}
               | {p | 0xFFFF for p in range(0, 0x110000, 0x10000)})

    out = {}
    for c in range(0x110000):
        if c in regional or c in wide or c in emoji:
            w = 2
        elif c in zero:
            w = 0
        else:
            w = 1
        if c in invalid:
            w = 1
        if w != 1:
            out[c] = w
    return out


def ranges(w):
    rs = []
    for c in sorted(w):
        if rs and rs[-1][1] == c - 1 and rs[-1][2] == w[c]:
            rs[-1][1] = c
        else:
            rs.append([c, c, w[c]])
    return rs


def header(rs):
    rows = [f'    {{0x{a:05X}, 0x{b:05X}, {w}}},' for a, b, w in rs]
    return f'''/* generated by tools/gen_widths.py from Unicode {UNICODE_VERSION} -- do not
 * edit; re-run it (--check shows drift). Every codepoint whose column
 * width is not 1, as sorted non-overlapping ranges: 0 = combines into the
 * cell before it, 2 = a wide glyph. See the script for the rules. */
#ifndef CTUI_UTF8_WIDTH_H
#define CTUI_UTF8_WIDTH_H

#include <stdint.h>

#define CTUI_UTF8_UNICODE_VERSION "{UNICODE_VERSION}"

/* clang-format off */
static const struct {{
  uint32_t first, last;
  unsigned char width;
}} ctui_utf8_width_table[] = {{
{chr(10).join(rows)}
}};
/* clang-format on */

#endif
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ucd')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    text = header(ranges(widths(ucd_dir(a.ucd))))
    if a.check:
        with open(OUT, encoding='utf-8') as f:
            if f.read() != text:
                print(f'{OUT}: differs from the Unicode data, re-run '
                      'tools/gen_widths.py', file=sys.stderr)
                return 1
        return 0
    with open(OUT, 'w', encoding='utf-8') as f:
        f.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
