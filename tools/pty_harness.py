#!/usr/bin/env python3
"""
Drive a ctui binary under a real pty, inject keystrokes/resizes, and read
back the rendered screen as a reconstructed text grid.

Replaces the ad hoc `script -qc ...` / one-off pty.fork() snippets from
CLAUDE.md's Testing approach with a single reusable tool. Usage:

  tools/pty_harness.py ./ctui-demo
  tools/pty_harness.py ./ctui-demo --steps "wait:0.3,key:DOWN,wait:0.1,key:ENTER,wait:0.2,dump"
  tools/pty_harness.py ./ctui-demo --rows 40 --cols 100 \
      --steps "wait:0.3,resize:24x80,wait:0.3,dump"
  tools/pty_harness.py ./ctui-demo --steps "wait:0.5,dump" --grep '\\[CTUI:EVENT\\]'
  tools/pty_harness.py ./ctui-mus --emulate-kitty-shm \
      --steps "wait:1,dump" --grep '\\[CTUI:GFX\\]'

Step vocabulary (comma-separated, run in order):
  wait:SECONDS      sleep, draining output as it arrives
  key:NAME          UP/DOWN/LEFT/RIGHT/ENTER/ESC/TAB/SPACE, or any single
                     literal character (e.g. key:q)
  resize:ROWSxCOLS  TIOCSWINSZ on the pty -> real SIGWINCH to the child
  dump              print the reconstructed screen grid so far
  raw               print the raw captured bytes so far (cat -v style)

Default steps (no --steps given): "wait:1,dump" -- a one-frame smoke test.
The child is always killed at the end; ctui apps loop forever otherwise.

--emulate-kitty-shm: this harness already *is* the terminal side of the
pty (it can os.write() back to the child, same as key: already does) --
with this flag set, it also answers ctui_gfx_kitty_probe_shm()'s one-shot
`a=q,...,t=s,...` APC startup query with a synthetic `i=<id>;OK` reply,
the same shape a real Kitty terminal sends. Lets g_kitty_shm_supported
latch to 1 (and therefore the faster t=s transport actually get exercised
and show up in --grep'd log output) when driving a Kitty-protocol app
under this harness from a non-Kitty terminal/CI. Best-effort:
handshake/code-path verification only, not real shm pixel-content
checking (the harness never actually attaches to the posted shm segment).
"""
import argparse
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time

KEY_BYTES = {
    "UP": b"\x1b[A",
    "DOWN": b"\x1b[B",
    "RIGHT": b"\x1b[C",
    "LEFT": b"\x1b[D",
    "ENTER": b"\r",
    "ESC": b"\x1b",
    "TAB": b"\t",
    "SPACE": b" ",  # a bare "key: " step loses its space to the per-step
                    # strip() in main()'s steps-list comprehension, so " "
                    # needs a name like every other non-printable key here
}

CSI_FINAL = re.compile(rb"[\x40-\x7e]")


class Grid:
    def __init__(self, rows, cols):
        self.resize(rows, cols)

    def resize(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.cells = [[" "] * cols for _ in range(rows)]
        self.r = self.c = 0

    def put(self, ch):
        if 0 <= self.r < self.rows and 0 <= self.c < self.cols:
            self.cells[self.r][self.c] = ch
        self.c += 1
        if self.c >= self.cols:
            self.c = 0
            self.r = min(self.r + 1, self.rows - 1)

    def clear(self):
        self.cells = [[" "] * self.cols for _ in range(self.rows)]

    def text(self):
        return "\n".join("".join(row).rstrip() for row in self.cells)


def feed(grid, buf):
    """Replay a captured byte stream into grid, tracking cursor moves,
    clears, and printable chars; every other escape is consumed and
    ignored (colors, cursor visibility, alt-screen, ...)."""
    i, n = 0, len(buf)
    while i < n:
        b = buf[i]
        if b == 0x1B:  # ESC
            if i + 1 < n and buf[i + 1 : i + 2] == b"[":
                m = CSI_FINAL.search(buf, i + 2)
                if not m:
                    break  # incomplete sequence at end of buffer
                params = buf[i + 2 : m.start()].decode("ascii", "ignore")
                final = buf[m.start() : m.start() + 1]
                if final == b"H" or final == b"f":
                    parts = params.lstrip("?").split(";")
                    row = int(parts[0]) if parts and parts[0].isdigit() else 1
                    col = (
                        int(parts[1])
                        if len(parts) > 1 and parts[1].isdigit()
                        else 1
                    )
                    grid.r = max(0, min(grid.rows - 1, row - 1))
                    grid.c = max(0, min(grid.cols - 1, col - 1))
                elif final == b"J" and params in ("2", "3", ""):
                    grid.clear()
                i = m.end()
                continue
            else:
                i += 2  # ESC + one byte (e.g. alt-screen sequences w/o '[')
                continue
        elif b in (0x0D,):  # \r
            grid.c = 0
            i += 1
        elif b == 0x0A:  # \n
            grid.r = min(grid.r + 1, grid.rows - 1)
            grid.c = 0
            i += 1
        elif b == 0x08:  # backspace
            grid.c = max(0, grid.c - 1)
            i += 1
        elif b < 0x20:
            i += 1  # other control byte, ignore
        else:
            grid.put(chr(b))
            i += 1


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


# ctui_gfx_kitty_probe_shm() (vendor/ctui/src/core/gfx.c) sends exactly
# "\x1b_Ga=q,i=1,s=1,v=1,f=32,t=s,q=0;<base64 shm name>\x1b\\" -- matched
# loosely (require a=q and t=s, capture the image id) rather than the
# exact literal key order/set, so this keeps working across minor changes
# to that call site instead of needing to track it byte-for-byte.
KITTY_SHM_PROBE_RE = re.compile(
    rb"\x1b_Ga=q,(?=[^;]*\bi=(\d+))(?=[^;]*\bt=s\b)[^;]*;[^\x1b]*\x1b\\"
)


def make_kitty_shm_responder(master_fd):
    """Returns a drain() on_chunk callback that watches accumulated output
    for the shm probe APC and immediately writes back a synthetic
    "i=<id>;OK" reply -- see this module's own docstring
    (--emulate-kitty-shm) for why this has to happen from inside drain()'s
    read loop, not after some wait: step's full duration elapses: the
    real probe only waits CTUI_KITTY_PROBE_TIMEOUT_MS (250ms, gfx.c) for a
    reply, and a step like "wait:1" would otherwise only hand control back
    to Python (and thus a chance to reply) after the full second."""
    state = {"done": False, "pending": bytearray()}

    def on_chunk(chunk):
        if state["done"]:
            return
        state["pending"].extend(chunk)
        m = KITTY_SHM_PROBE_RE.search(bytes(state["pending"]))
        if not m:
            return
        image_id = m.group(1).decode("ascii")
        os.write(master_fd, f"\x1b_Gi={image_id};OK\x1b\\".encode("ascii"))
        state["done"] = True

    return on_chunk


def drain(fd, buf, timeout=0.05, on_chunk=None):
    """Read whatever is available right now, looping until quiet -- bounded
    to timeout total, not timeout per read. A target that keeps emitting
    output on a cadence shorter than timeout (e.g. several staggered
    sub-second periodic timers, each just quiet long enough on its own)
    would otherwise keep this loop's select() finding fresh data forever,
    since re-passing the same fixed timeout to every iteration never
    accounts for time already spent -- this hung indefinitely against the
    flicker example app before being bounded here.

    on_chunk, if given, fires synchronously right after each individual
    read -- not after this whole call returns -- so a caller that needs to
    react to output mid-drain (make_kitty_shm_responder() above) actually
    can before a real terminal's own reply deadline would expire."""
    deadline = time.time() + timeout
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            return
        r, _, _ = select.select([fd], [], [], remaining)
        if fd not in r:
            return
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            return
        if not chunk:
            return
        buf.extend(chunk)
        if on_chunk:
            on_chunk(chunk)


def run(binary, rows, cols, steps, grep, log_path, emulate_kitty_shm=False):
    pid, master_fd = pty.fork()
    if pid == 0:
        os.execvp(binary, [binary])
        os._exit(127)

    set_winsize(master_fd, rows, cols)
    on_chunk = make_kitty_shm_responder(master_fd) if emulate_kitty_shm else None
    buf = bytearray()
    # every byte ever read from the child, start to finish -- unlike buf
    # (drained into per-step, then cleared so feed() only sees each
    # step's own slice), this never shrinks. A fast target can write its
    # entire frame (or, for a Kitty image, tens of KB of base64) before
    # the *first* step's own preamble drain even returns, so a step's
    # "own" capture is frequently empty by the time its handler runs --
    # the real bytes already got drained (and buf cleared) by whichever
    # step happened to run first. `raw` needs the full running record,
    # not just this step's slice, to reliably show anything at all.
    history = bytearray()
    grid = Grid(rows, cols)

    def drain_step(timeout=0.05):
        """drains whatever's newly available into buf, folds it into
        history, feeds it to grid, then clears buf -- the same
        catch-up-before-doing-anything-else preamble every step needs."""
        drain(master_fd, buf, timeout, on_chunk)
        history.extend(buf)
        feed(grid, bytes(buf))
        del buf[:]

    try:
        for step in steps:
            drain_step()

            kind, _, arg = step.partition(":")
            if kind == "wait":
                deadline = time.time() + float(arg)
                while time.time() < deadline:
                    drain(master_fd, buf, timeout=max(0, deadline - time.time()),
                         on_chunk=on_chunk)
                history.extend(buf)
                feed(grid, bytes(buf))
                del buf[:]
            elif kind == "key":
                data = KEY_BYTES.get(arg.upper(), arg.encode())
                os.write(master_fd, data)
            elif kind == "resize":
                r, _, c = arg.partition("x")
                rows, cols = int(r), int(c)
                set_winsize(master_fd, rows, cols)
                grid.resize(rows, cols)
            elif kind == "dump":
                drain_step()
                print(grid.text())
                print("-" * cols)
            elif kind == "raw":
                # cumulative, not just what trickled in since the last
                # step -- see history's own comment above for why a
                # per-step-only capture routinely comes up empty. See
                # PROGRESS.md's Known issues for the version of this that
                # used to look broken.
                drain_step()
                sys.stdout.write(history.decode("ascii", "replace"))
                sys.stdout.write("\n" + "-" * 40 + "\n")
            else:
                print(f"unknown step: {step!r}", file=sys.stderr)
    finally:
        drain(master_fd, buf, timeout=0.2, on_chunk=on_chunk)
        feed(grid, bytes(buf))
        try:
            os.kill(pid, signal.SIGTERM)
            os.waitpid(pid, 0)
        except (ProcessLookupError, ChildProcessError):
            pass
        os.close(master_fd)

    if grep and log_path and os.path.exists(log_path):
        pattern = re.compile(grep)
        print(f"--- {log_path} matches for {grep!r} ---")
        with open(log_path, errors="replace") as f:
            for line in f:
                if pattern.search(line):
                    sys.stdout.write(line)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("binary")
    p.add_argument("--rows", type=int, default=24)
    p.add_argument("--cols", type=int, default=80)
    p.add_argument("--steps", default="wait:1,dump")
    p.add_argument("--grep", default=None, help="regex to grep from --log after the run")
    p.add_argument("--log", default="ctui.log")
    p.add_argument(
        "--emulate-kitty-shm",
        action="store_true",
        help="answer ctui_gfx_kitty_probe_shm()'s startup handshake so "
        "g_kitty_shm_supported latches to 1 without a real Kitty terminal "
        "(see this module's docstring)",
    )
    args = p.parse_args()

    steps = [s.strip() for s in args.steps.split(",") if s.strip()]
    run(args.binary, args.rows, args.cols, steps, args.grep, args.log,
       emulate_kitty_shm=args.emulate_kitty_shm)


if __name__ == "__main__":
    main()
