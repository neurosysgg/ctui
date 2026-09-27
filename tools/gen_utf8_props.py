#!/usr/bin/env python3
"""Generates src/core/utf8_props.h: every codepoint's column width and
grapheme cluster properties, the way kitty (ctui's reference terminal)
decides them, from the Unicode Character Database.

    tools/gen_utf8_props.py [--ucd DIR] [--check]

--ucd reads the data files (FILES below, flat) from DIR; without it they
are fetched once for UNICODE_VERSION into ~/.cache/ctui/ucd-VERSION/.
--check exits 1 when the checked-in header differs from what the data
gives (drift).

Widths, first match wins (written from the Unicode data and kitty's
documented behaviour; tools/check_widths.py compares the result with a
running kitty):
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

Cluster properties (UAX #29, what core/utf8.c segments by): the
Grapheme_Cluster_Break class (the Hangul syllable block as one range,
LV/LVT follow from the codepoint), Indic_Conjunct_Break,
Extended_Pictographic, and whether the codepoint is an emoji
presentation base (VS16 after it widens the cell, VS15 narrows it).
"""

import argparse
import os
import sys
import urllib.request

UNICODE_VERSION = '17.0.0'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src',
                   'core', 'utf8_props.h')
FILES = {
    'UnicodeData.txt': 'ucd',
    'EastAsianWidth.txt': 'ucd',
    'PropList.txt': 'ucd',
    'DerivedCoreProperties.txt': 'ucd',
    'GraphemeBreakProperty.txt': 'ucd/auxiliary',
    'GraphemeBreakTest.txt': 'ucd/auxiliary',
    'emoji-data.txt': 'ucd/emoji',
    'emoji-sequences.txt': 'emoji',
}
# Grapheme_Cluster_Break values, in core/utf8.c's CTUI_GB_* order; LV and
# LVT only through HANGUL
GBP = ['Other', 'CR', 'LF', 'Control', 'Extend', 'ZWJ', 'Regional_Indicator',
       'Prepend', 'SpacingMark', 'L', 'V', 'T', 'HANGUL']
INCB = {'None': 0, 'Linker': 1, 'Consonant': 2, 'Extend': 3}
EXTPICT, EMOJI_BASE = 4, 8
HANGUL = (0xAC00, 0xD7A3)


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
                yield [x.strip() for x in line.split(';')]


def cps(spec):
    if '..' in spec:
        a, b = spec.split('..')
        return range(int(a, 16), int(b, 16) + 1)
    return range(int(spec, 16), int(spec, 16) + 1)


def props(d):
    category = {}
    first = None
    for f in lines(d, 'UnicodeData.txt'):
        cp, name, cat = int(f[0], 16), f[1], f[2]
        if name.endswith(', First>'):
            first = cp
            continue
        for c in range(first if first is not None else cp, cp + 1):
            category[c] = cat
        first = None

    wide, seen = set(), set()
    for spec, eaw, *_ in lines(d, 'EastAsianWidth.txt'):
        seen.update(cps(spec))
        if eaw in ('W', 'F'):
            wide.update(cps(spec))
    for a, b in ((0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xF900, 0xFAFF),
                 (0x20000, 0x2FFFD), (0x30000, 0x3FFFD)):
        wide.update(set(range(a, b + 1)) - seen)

    emoji, emoji_base = set(), set()
    for spec, kind, *_ in lines(d, 'emoji-sequences.txt'):
        parts = spec.split()
        base = cps(parts[0])
        if kind == 'Basic_Emoji':
            emoji_base.update(base)
            if len(parts) == 1:
                emoji.update(base)
        elif kind == 'Emoji_Keycap_Sequence':
            emoji_base.update(base)
        elif kind == 'RGI_Emoji_Flag_Sequence':
            emoji.update(int(p, 16) for p in parts)
            emoji_base.update(int(p, 16) for p in parts)
        elif kind in ('RGI_Emoji_Tag_Sequence',
                      'RGI_Emoji_Modifier_Sequence'):
            emoji.update(base)
            emoji_base.update(base)

    ignorable = set()
    for spec, prop, *_ in lines(d, 'PropList.txt'):
        if prop == 'Other_Default_Ignorable_Code_Point':
            ignorable.update(cps(spec))

    gbp = {}
    for spec, prop, *_ in lines(d, 'GraphemeBreakProperty.txt'):
        if prop not in ('LV', 'LVT'):
            gbp.update(dict.fromkeys(cps(spec), GBP.index(prop)))
    gbp.update(dict.fromkeys(range(HANGUL[0], HANGUL[1] + 1),
                             GBP.index('HANGUL')))

    flags = {}
    for f in lines(d, 'DerivedCoreProperties.txt'):
        if f[1] == 'InCB':
            for c in cps(f[0]):
                flags[c] = INCB[f[2]]
    for spec, prop, *_ in lines(d, 'emoji-data.txt'):
        if prop == 'Extended_Pictographic':
            for c in cps(spec):
                flags[c] = flags.get(c, 0) | EXTPICT
    for c in emoji_base:
        flags[c] = flags.get(c, 0) | EMOJI_BASE

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
        p = (w, gbp.get(c, 0), flags.get(c, 0))
        if p != (1, 0, 0):
            out[c] = p
    return out


def ranges(p):
    rs = []
    for c in sorted(p):
        if rs and rs[-1][1] == c - 1 and rs[-1][2] == p[c]:
            rs[-1][1] = c
        else:
            rs.append([c, c, p[c]])
    return rs


def header(rs):
    rows = [f'    {{0x{a:05X}, 0x{b:05X}, {w}, {g}, {f}}},'
            for a, b, (w, g, f) in rs]
    return f'''/* generated by tools/gen_utf8_props.py from Unicode {UNICODE_VERSION} --
 * do not edit; re-run it (--check shows drift). Every codepoint whose
 * properties differ from {{width 1, CTUI_GB_OTHER, no flags}}, as sorted
 * non-overlapping ranges. width: 0 = joins the cell before it, 2 = a
 * wide glyph; gb: its CTUI_GB_* class (core/utf8.c); flags: the
 * Indic_Conjunct_Break value (bits 0-1: 1 linker, 2 consonant, 3 extend),
 * 4 Extended_Pictographic, 8 an emoji presentation base. See the script
 * for the rules. */
#ifndef CTUI_UTF8_PROPS_H
#define CTUI_UTF8_PROPS_H

#include <stdint.h>

#define CTUI_UTF8_UNICODE_VERSION "{UNICODE_VERSION}"

/* clang-format off */
static const struct {{
  uint32_t first, last;
  unsigned char width, gb, flags;
}} ctui_utf8_props_table[] = {{
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
    text = header(ranges(props(ucd_dir(a.ucd))))
    if a.check:
        with open(OUT, encoding='utf-8') as f:
            if f.read() != text:
                print(f'{OUT}: differs from the Unicode data, re-run '
                      'tools/gen_utf8_props.py', file=sys.stderr)
                return 1
        return 0
    with open(OUT, 'w', encoding='utf-8') as f:
        f.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
