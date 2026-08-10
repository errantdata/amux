#!/usr/bin/env python3
"""Replay window (-s): a reattach lands on the live tail instead of rendering
the whole ring.

The session emits N numbered lines and then becomes `cat` (stays alive). After
a detach we reattach with several -s settings and check exactly which lines the
server pushed:

  F1  default            -> the last REPLAY_DEFAULT_LINES lines only
  F2  -s none            -> nothing replayed, straight to live
  F3  -s full            -> the whole history, as the fork did before
  F4  -s 500             -> the last 500 lines
  F5  -s 4k              -> a byte budget, not a line count
  F6  the default replay is a small fraction of the full one (the point)
"""
import os, pty, sys, time, struct, fcntl, termios, subprocess, tempfile, re, shutil
import signal

AMUX = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "amux")  # repo-relative: runs anywhere
SD = tempfile.mkdtemp(prefix="muxF.")
ENV = dict(os.environ, AMUX_SOCKET_DIR=SD)
ENV.pop("AMUX_REPLAY", None)
ANSI = re.compile(rb'\x1b\[[0-9;?]*[ -/]*[@-~]')
OSC = re.compile(rb'\x1b\][0-9].*?\x07')

N = 20000
DEFAULT_LINES = 10000        # REPLAY_DEFAULT_LINES in amux.c

def cleanup_sessions():
    """Kill session servers this test leaves behind. A session whose command
    finished while detached stays alive on purpose (so you can attach and read
    its exit status); deleting our socket dir below would orphan those."""
    try:
        out = subprocess.run([AMUX], env=ENV, capture_output=True, timeout=10).stdout
        for line in out.splitlines()[1:]:
            parts = line.split(b'\t')
            if len(parts) >= 3 and parts[-2].strip().isdigit():
                try:
                    os.kill(int(parts[-2]), signal.SIGTERM)
                except (ProcessLookupError, PermissionError):
                    pass
    except Exception:
        pass

def set_ws(fd):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))

def spawn(args):
    m, s = pty.openpty(); set_ws(s)
    p = subprocess.Popen(args, stdin=s, stdout=s, stderr=s,
                         start_new_session=True, env=ENV, close_fds=True)
    os.close(s)
    return p, m

def drain(m, t):
    out = bytearray(); end = time.time() + t
    fl = fcntl.fcntl(m, fcntl.F_GETFL); fcntl.fcntl(m, fcntl.F_SETFL, fl | os.O_NONBLOCK)
    while time.time() < end:
        try:
            d = os.read(m, 65536)
        except BlockingIOError:
            time.sleep(0.02); continue
        except OSError:
            break
        if d: out += d
        else: break
    return bytes(out)

def nums(raw):
    """Line numbers the server actually pushed to this terminal."""
    clean = OSC.sub(b'', ANSI.sub(b'', raw))
    clean = re.sub(rb'amux:.*', b'', clean)     # epilog status lines
    return [int(x) for x in re.findall(rb'\d+', clean)]

res = []
def check(name, ok, detail=""):
    print(f"[{'OK ' if ok else 'FAIL'}] {name}" + (f"  {detail}" if detail else ""))
    res.append(ok)

def reattach(extra):
    """Reattach with the given extra args, return (numbers, payload bytes)."""
    p, m = spawn([AMUX, "-e", "^\\"] + extra + ["-a", "tF"])
    o = drain(m, 3.0)
    os.write(m, b"\x1c")                          # detach again
    time.sleep(0.3)
    try: p.wait(timeout=3)
    except Exception: pass
    os.close(m)
    return nums(o), len(o)

# ---- create the session and let it produce N lines -------------------------
# zero-padded so every line is the same width: byte counts below then track
# line counts directly, instead of being skewed by the tail's longer numbers.
# awk rather than `seq -w`, which busybox (Alpine) does not support.
FILL = "awk 'BEGIN{for(i=1;i<=%d;i++) printf \"%%05d\\n\", i}'" % N
p1, m1 = spawn([AMUX, "-e", "^\\", "-c", "tF", "sh", "-c", f"{FILL}; exec cat"])
first = nums(drain(m1, 4.0))
check("F0: session produced all %d lines" % N, first and max(first) == N,
      f"max={max(first) if first else None}")
os.write(m1, b"\x1c"); time.sleep(0.4)
try: p1.wait(timeout=3)
except Exception: pass
os.close(m1)

# ---- F1: default replay = trailing DEFAULT_LINES lines ---------------------
got, default_bytes = reattach([])
check("F1: default replays the last %d lines" % DEFAULT_LINES,
      got and max(got) == N and min(got) == N - DEFAULT_LINES + 1 and len(got) == DEFAULT_LINES,
      f"lines {min(got) if got else None}..{max(got) if got else None} n={len(got)}")

# ---- F2: -s none -> no history at all --------------------------------------
got, _ = reattach(["-s", "none"])
check("F2: -s none replays nothing", got == [], f"got {got[:5]}")

# ---- F3: -s full -> everything still retained ------------------------------
got, full_bytes = reattach(["-s", "full"])
check("F3: -s full replays the whole history 1..%d" % N,
      got and min(got) == 1 and max(got) == N and len(got) == N,
      f"lines {min(got) if got else None}..{max(got) if got else None} n={len(got)}")

# ---- F4: explicit line count ----------------------------------------------
got, _ = reattach(["-s", "500"])
check("F4: -s 500 replays the last 500 lines",
      got and max(got) == N and min(got) == N - 499 and len(got) == 500,
      f"lines {min(got) if got else None}..{max(got) if got else None} n={len(got)}")

# ---- F5: byte budget -------------------------------------------------------
got, _ = reattach(["-s", "4k"])
check("F5: -s 4k is a byte budget ending at the live tail",
      got and max(got) == N and 0 < len(got) <= 4096 // 2,
      f"n={len(got)} max={max(got) if got else None}")

# ---- F6: the whole point ---------------------------------------------------
# half the lines, fixed width -> the terminal has to render about half as much
check("F6: default reattach pushes about half of a full one (20k-line session)",
      default_bytes * 1.8 < full_bytes,
      f"default={default_bytes}B full={full_bytes}B")

# ---- tear the session down -------------------------------------------------
p9, m9 = spawn([AMUX, "-e", "^\\", "-s", "none", "-a", "tF"])
os.write(m9, b"\x04")                              # EOF -> cat exits 0
drain(m9, 1.0); os.close(m9)
try: rc = p9.wait(timeout=3)
except Exception: rc = "(timeout)"
check("F7: session still healthy, exits cleanly (rc=0)", rc == 0, f"rc={rc}")

cleanup_sessions()
shutil.rmtree(SD, ignore_errors=True)
print("-" * 40)
print("PASS" if all(res) else "SOME TESTS FAILED")
sys.exit(0 if all(res) else 1)
