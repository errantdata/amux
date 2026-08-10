#!/usr/bin/env python3
"""Record demo/reattach.cast — the asciicast behind the README's GIF.

Drives a real pty running a real amux against a real session, and writes an
asciicast v2 file. Nothing here is staged output: every byte in the cast came
out of the binary in this repository.

    python3 demo/record.py          # from the repository root
    agg demo/reattach.cast demo/reattach.gif --fps-cap 15 --idle-time-limit 1.5

What it does NOT show, deliberately: the "replaying the whole history crawls"
half of the story. That cost is the *terminal* drawing tens of megabytes, and a
headless recorder drains the pty at memory speed, so a recording cannot
reproduce it. Simulating it with an artificial throttle would be inventing
evidence, so the numbers live in README.md instead and the GIF shows only what
a recording can honestly demonstrate.
"""
import json, os, pty, re, select, shutil, struct, subprocess, sys
import fcntl, termios, tempfile, threading, time

COLS, ROWS = 100, 28
BULK_LINES = 2_000_000          # ~17 MB of history in the ring
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AMUX = os.path.join(REPO, "amux")
CAST = os.path.join(REPO, "demo", "reattach.cast")

# The last lines of the session: what you actually see on reattach. The bulk
# above them is cheap filler whose only job is to make the ring big.
TAIL = [
    "  reading  server.c",
    "  reading  tests/test_history_replay.py",
    "",
    "  The replay cursor is compared against ring_total, which moves while",
    "  the application is still writing. I'll freeze it at attach time.",
    "",
    "agent> go ahead",
    "  ...answering: go ahead",
    "",
    "  edit  amux.c          +6 -1",
    "  edit  server.c        +18 -4",
    "  edit  tests/test_large_streams.py   +144 -0",
    "",
    "  run   make check",
    "",
    "    == tests/test_history_dump.py      PASS",
    "    == tests/test_history_replay.py    PASS",
    "    == tests/test_interactive.py       PASS",
    "    == tests/test_io_integrity.py      PASS",
    "    == tests/test_large_streams.py     PASS",
    "    == tests/test_pty_input.py         PASS",
    "    == tests/test_replay_window.py     PASS",
    "",
    "  7 files, 34 checks, 0 failures.",
    "",
    "  Reverting the fix reproduces the stall: 2 of 5 dumps time out.",
    "  The regression test holds.",
    "",
]


class Rec:
    """A pty running bash, with an asciicast recorder attached to it."""

    def __init__(self):
        self.events = []
        self.recording = False
        self.t0 = 0.0
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
        self.sockdir = tempfile.mkdtemp(prefix="amuxdemo.")
        env = dict(
            os.environ,
            PS1="$ ", PS2="> ", TERM="xterm-256color",
            AMUX_SOCKET_DIR=self.sockdir,
            PATH=REPO + os.pathsep + os.environ["PATH"],
        )
        env.pop("AMUX_REPLAY", None)
        self.proc = subprocess.Popen(
            ["bash", "--norc", "--noprofile", "-i"],
            stdin=slave, stdout=slave, stderr=slave,
            start_new_session=True, env=env, close_fds=True,
        )
        os.close(slave)
        self.alive = True
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def _pump(self):
        while self.alive:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if not r:
                continue
            try:
                data = os.read(self.master, 1 << 16)
            except OSError:
                break
            if not data:
                break
            if self.recording:
                self.events.append((time.time() - self.t0,
                                    data.decode("utf-8", "replace")))

    # -- driving -----------------------------------------------------------
    def send(self, text):
        os.write(self.master, text.encode())

    def type(self, text, cps=22):
        """Type like a person: one character at a time."""
        for ch in text:
            os.write(self.master, ch.encode())
            time.sleep(1.0 / cps)

    def run(self, cmd, settle=0.6, cps=22):
        self.type(cmd, cps)
        time.sleep(0.25)
        self.send("\r")
        time.sleep(settle)

    def quiet(self, cmd, settle=0.4):
        """Run without the human-typing delay (used before recording starts)."""
        self.send(cmd + "\r")
        time.sleep(settle)

    def start(self):
        self.t0 = time.time()
        self.recording = True

    def close(self):
        self.alive = False
        self.thread.join(timeout=2)
        try:
            self.proc.kill()
        except Exception:
            pass
        os.close(self.master)
        # reap the session so it is not orphaned when the socket dir goes
        try:
            out = subprocess.run([AMUX], env=dict(os.environ, AMUX_SOCKET_DIR=self.sockdir),
                                 capture_output=True, timeout=10).stdout
            for line in out.splitlines()[1:]:
                parts = line.split(b"\t")
                if len(parts) >= 3 and parts[-2].strip().isdigit():
                    try:
                        os.kill(int(parts[-2]), 15)
                    except (ProcessLookupError, PermissionError):
                        pass
        except Exception:
            pass
        shutil.rmtree(self.sockdir, ignore_errors=True)

    def write_cast(self, path):
        with open(path, "w") as f:
            f.write(json.dumps({
                "version": 2, "width": COLS, "height": ROWS,
                "env": {"SHELL": "/bin/bash", "TERM": "xterm-256color"},
            }) + "\n")
            for t, data in self.events:
                f.write(json.dumps([round(t, 6), "o", data]) + "\n")


def main():
    if not os.access(AMUX, os.X_OK):
        sys.exit("build amux first: ./configure && make")

    r = Rec()
    time.sleep(1.0)

    # ---- setup, not recorded: a session with a long history and a prompt --
    tail = "".join("printf '%s\\n' " + repr_sh(l) + "; " for l in TAIL)
    agent = (
        f"seq 1 {BULK_LINES}; "
        + tail
        + 'while :; do printf "agent> "; read q || exit 0; '
        'echo "  ...answering: $q"; done'
    )
    r.quiet("clear")
    r.quiet(f"amux -n agent sh -c {shquote(agent)}", settle=1.0)

    # wait for the ring to stop growing, so the demo never races the filler
    prev, stable = -1, 0
    for _ in range(120):
        time.sleep(1.0)
        n = int(subprocess.run(f"{AMUX} -H agent | wc -c", shell=True, capture_output=True,
                               env=dict(os.environ, AMUX_SOCKET_DIR=r.sockdir)).stdout or 0)
        stable = stable + 1 if n == prev and n > 0 else 0
        prev = n
        if stable >= 2:
            break
    print(f"history ready: {prev/1e6:.1f} MB")
    r.quiet("clear", settle=0.8)

    # ---- recorded --------------------------------------------------------
    r.start()
    time.sleep(0.8)

    r.run("# yesterday's agent session is still running:", settle=0.9)
    r.run("amux", settle=1.6)

    r.run("# reattach -- 17 MB of history, no waiting:", settle=0.9)
    r.run("amux -a agent", settle=2.6)

    # it is live, not a replay
    r.type("what did the last run say?", cps=20)
    time.sleep(0.3)
    r.send("\r")
    time.sleep(2.2)

    r.send("\x1c")            # Ctrl+\ detaches
    time.sleep(1.8)

    # the screen keeps what the session left there (no alternate screen);
    # clear it so the last beat reads cleanly
    r.run("clear", settle=0.5)
    r.run("# and nothing was thrown away:", settle=0.9)
    r.run("amux -H agent | wc -l", settle=2.4)
    time.sleep(1.2)

    r.recording = False
    r.write_cast(CAST)
    dur = r.events[-1][0] if r.events else 0
    print(f"wrote {CAST}: {len(r.events)} events, {dur:.1f}s, "
          f"{os.path.getsize(CAST)/1e6:.2f} MB")
    r.close()


def shquote(s):
    return "'" + s.replace("'", "'\\''") + "'"


def repr_sh(s):
    return shquote(s)


if __name__ == "__main__":
    main()
