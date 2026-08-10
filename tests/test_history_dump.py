#!/usr/bin/env python3
"""amux -H: reach the history that a bounded replay does not push.

  G1  -H dumps the full retained ring to stdout and exits 0
  G2  it dumps more than a default attach replays (the reason it exists)
  G3  it does not disturb the session: still attachable and still alive
  G4  -H on an unknown session fails instead of hanging
  G5  -H works while a client is attached, and the attached client is
      unaffected (no stolen resize, no lost output)
"""
import os, pty, sys, time, struct, fcntl, termios, subprocess, tempfile, re, shutil
import signal

AMUX = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "amux")  # repo-relative: runs anywhere
SD = tempfile.mkdtemp(prefix="muxG.")
ENV = dict(os.environ, AMUX_SOCKET_DIR=SD)
ENV.pop("AMUX_REPLAY", None)
ANSI = re.compile(rb'\x1b\[[0-9;?]*[ -/]*[@-~]')
OSC = re.compile(rb'\x1b\][0-9].*?\x07')

N = 20000
DEFAULT_LINES = 10000

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
    clean = OSC.sub(b'', ANSI.sub(b'', raw))
    clean = re.sub(rb'amux:.*', b'', clean)
    return [int(x) for x in re.findall(rb'\d+', clean)]

res = []
def check(name, ok, detail=""):
    print(f"[{'OK ' if ok else 'FAIL'}] {name}" + (f"  {detail}" if detail else ""))
    res.append(ok)

# ---- a detached session holding N lines of history -------------------------
p1, m1 = spawn([AMUX, "-e", "^\\", "-c", "tG", "sh", "-c", f"seq 1 {N}; exec cat"])
drain(m1, 4.0)
os.write(m1, b"\x1c"); time.sleep(0.4)
try: p1.wait(timeout=3)
except Exception: pass
os.close(m1)

# ---- G1: dump the whole ring ----------------------------------------------
t0 = time.time()
r = subprocess.run([AMUX, "-H", "tG"], env=ENV, capture_output=True, timeout=30)
dt = time.time() - t0
dumped = nums(r.stdout)
check("G1: -H dumps the full history 1..%d, rc=0" % N,
      r.returncode == 0 and dumped and min(dumped) == 1 and max(dumped) == N
      and len(dumped) == N,
      f"rc={r.returncode} n={len(dumped)} in {dt:.2f}s")

# ---- G2: it reaches what a default attach deliberately skips ---------------
p2, m2 = spawn([AMUX, "-e", "^\\", "-a", "tG"])
replayed = nums(drain(m2, 3.0))
os.write(m2, b"\x1c"); time.sleep(0.3)
try: p2.wait(timeout=3)
except Exception: pass
os.close(m2)
check("G2: -H reaches history the default replay skips",
      len(dumped) == N and len(replayed) == DEFAULT_LINES,
      f"dump={len(dumped)} lines, attach replayed={len(replayed)}")

# ---- G3: the session is untouched by the dump ------------------------------
p3, m3 = spawn([AMUX, "-e", "^\\", "-s", "none", "-a", "tG"])
os.write(m3, b"still-alive\n")
back = drain(m3, 1.5)
check("G3: session unaffected, input still round-trips", b"still-alive" in back)

# ---- G5: dump while a client is attached -----------------------------------
r2 = subprocess.run([AMUX, "-H", "tG"], env=ENV, capture_output=True, timeout=30)
d2 = nums(r2.stdout)
check("G5a: -H works with a client attached", r2.returncode == 0 and max(d2) == N,
      f"rc={r2.returncode} n={len(d2)}")
os.write(m3, b"after-dump\n")
after = drain(m3, 1.5)
check("G5b: attached client keeps working during/after a dump",
      b"after-dump" in after)

os.write(m3, b"\x04")                            # EOF -> cat exits 0
drain(m3, 1.0); os.close(m3)
try: rc = p3.wait(timeout=3)
except Exception: rc = "(timeout)"
check("G3b: session exits cleanly afterwards (rc=0)", rc == 0, f"rc={rc}")

# ---- G4: unknown session ---------------------------------------------------
try:
    r3 = subprocess.run([AMUX, "-H", "no-such-session"], env=ENV,
                        capture_output=True, timeout=10)
    check("G4: -H on an unknown session fails without hanging",
          r3.returncode != 0 and r3.stdout == b"", f"rc={r3.returncode}")
except subprocess.TimeoutExpired:
    check("G4: -H on an unknown session fails without hanging", False, "timed out")

cleanup_sessions()
shutil.rmtree(SD, ignore_errors=True)
print("-" * 40)
print("PASS" if all(res) else "SOME TESTS FAILED")
sys.exit(0 if all(res) else 1)
