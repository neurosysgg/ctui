#!/usr/bin/env python3
"""Compares flush's bytes with an app's own in kitty's parser.
    tools/check_flush_kitty.py [--kitty KITTY] DIR
DIR is flushbench's --out: NAME.vt (a recording as replayed) and
NAME.new.vt (this build's flush of the same frames), both with
"\\e_ctui-record;frame\\e\\\\" after each frame and the recording's size
marks. Each pair is fed to two of kitty's own Screens frame by frame, and
after every frame each line must be the same in both (text, colours,
attributes: Line.as_ansi(), plain spaces at its end dropped). Dev-time only (needs a kitty binary; nothing
of kitty enters ctui). Exit 1 on any difference.
"""
import argparse
import glob
import os
import subprocess
import sys

KITTY_PY = r'''
import os
import re
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

FRAME = b'\x1b_ctui-record;frame\x1b\\'
SIZE = re.compile(rb'\x1b_ctui-record;size=(\d+);(\d+)\x1b\\')

def feed(screen, data):
    data = memoryview(data)
    while data:
        dest = screen.test_create_write_buffer()
        n = screen.test_commit_write_buffer(data, dest)
        data = data[n:]
        screen.test_parse_written_data(None)

def frames(path):
    with open(path, 'rb') as f:
        return f.read().split(FRAME)

def apply(screen, chunk):
    """chunk's bytes, a size mark resizing the screen where it is"""
    pos = 0
    for m in SIZE.finditer(chunk):
        feed(screen, chunk[pos:m.start()])
        screen.resize(int(m.group(1)), int(m.group(2)))
        pos = m.end()
    feed(screen, chunk[pos:])

def lines(screen):
    # a cell never written and a plain space look the same; kitty keeps
    # them apart, and the app's first frame may lean on a cleared screen
    return [screen.line(i).as_ansi().rstrip(' ') for i in range(screen.lines)]

bad_files = 0
for old_path in sys.argv[1:]:
    new_path = old_path[:-3] + '.new.vt'
    old, new = frames(old_path), frames(new_path)
    m = SIZE.search(old[0])
    rows, cols = int(m.group(1)), int(m.group(2))
    a = Screen(CB(), rows, cols, 0, 10, 20, 0, CB())
    b = Screen(CB(), rows, cols, 0, 10, 20, 0, CB())
    bad = 0
    nframes = min(len(old), len(new))
    for i in range(nframes):
        apply(a, old[i])
        apply(b, new[i])
        la, lb = lines(a), lines(b)
        if la != lb:
            bad += 1
            if bad <= 2:
                for r, (x, y) in enumerate(zip(la, lb)):
                    if x != y:
                        print(f'{os.path.basename(old_path)} frame {i} row {r}:'
                              f'\n  app   {x!r}\n  flush {y!r}')
                        break
    print(f'{os.path.basename(old_path)}: {nframes} frames'
          + (f', {bad} DIFFER' if bad else ''))
    bad_files += bad > 0
sys.exit(1 if bad_files else 0)
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--kitty', default='kitty')
    ap.add_argument('dir')
    a = ap.parse_args()
    olds = sorted(p for p in glob.glob(os.path.join(a.dir, '*.vt'))
                  if not p.endswith('.new.vt'))
    if not olds:
        sys.exit(f'{a.dir}: no recordings (flushbench --out DIR)')
    # +runpy exec()s inside a function: module-level names wouldn't reach
    # the script's own functions, so it runs with globals of its own
    run = ('import sys; exec(compile(sys.stdin.read(), "check_flush_kitty", '
           '"exec"), {"__name__": "__main__"})')
    sys.exit(subprocess.run([a.kitty, '+runpy', run] + olds, input=KITTY_PY,
                            text=True, check=False).returncode)


if __name__ == '__main__':
    main()
