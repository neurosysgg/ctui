#!/usr/bin/env python3
"""Captures example apps' whole output for tests/harness_test.c.

  tools/harness/capture.py      (after `make examples`)

Each app runs under a pty (TERM=xterm-256color, no kitty), gets a few
keys, and its every byte goes to tests/data/harness/NAME.out; NAME.txt is
"ROWS COLS" and the grid tools/pty_harness.py's feed() makes of those
bytes -- what the C parser (tools/harness/vt.c) has to agree with.
"""
import os
import pty
import select
import signal
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import pty_harness  # noqa: E402

KEYS = {
    "gallery": ["\t", "\x1b[B", "\x1b[B", "\r", "\t", "\x1b[C", " "],
    "demo": ["\x1b[B", "\x1b[B", "\r", "\x1b[A"],
    "calculator": ["1", "2", "+", "3", "4", "=", "*", "2", "\r"],
}
ROWS, COLS = 30, 100


def drain(fd, buf, secs):
    end = time.time() + secs
    while (left := end - time.time()) > 0:
        r, _, _ = select.select([fd], [], [], left)
        if not r:
            return
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            return
        if not chunk:
            return
        buf.extend(chunk)


def capture(name, keys):
    binary = os.path.join(ROOT, "ctui-" + name)
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(os.path.join(ROOT, "tests", "data", "harness"))
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("KITTY") and k not in
               ("WAYLAND_DISPLAY", "DISPLAY")}
        env.update(TERM="xterm-256color", COLORTERM="truecolor")
        os.execve(binary, [binary], env)
    pty_harness.set_winsize(fd, ROWS, COLS)
    buf = bytearray()
    drain(fd, buf, 1.0)
    for k in keys:
        os.write(fd, k.encode())
        drain(fd, buf, 0.3)
    os.kill(pid, signal.SIGTERM)
    drain(fd, buf, 0.3)
    os.waitpid(pid, 0)
    os.close(fd)
    grid = pty_harness.Grid(ROWS, COLS)
    pty_harness.feed(grid, bytes(buf))
    out = os.path.join(ROOT, "tests", "data", "harness", name)
    with open(out + ".out", "wb") as f:
        f.write(buf)
    with open(out + ".txt", "w") as f:
        f.write(f"{ROWS} {COLS}\n{grid.text()}\n")
    print(f"{name}: {len(buf)} bytes")


for name, keys in KEYS.items():
    capture(name, keys)
