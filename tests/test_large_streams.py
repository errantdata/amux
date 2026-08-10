#!/usr/bin/env python3
"""Regressions for streaming a large history to a client.

Both of these failed before the fixes they guard:

  L1  Repeated -H dumps of a large, static history must each return the whole
      thing, quickly. The server only asked select() for write readiness when
      the out buffer was already non-empty, so a flush that drained it
      completely left nothing to wake the loop; with an idle application
      nothing ever did, and the client stalled mid-stream. Nondeterministic,
      because it depended on the socket swallowing a whole 256 KB write.

  L2  -H against a session that is still producing must terminate, and return
      a snapshot of the history as it was when the dump attached. The dump
      used to chase the live write head, so it stopped early on a lucky
      catch-up or never finished at all -- the `amux -H session | less` case,
      where the consumer is slow by definition.

  L3  Coverage for the ordinary replay path at the same size: -s full against
      a large idle session must deliver every line. Note this one did NOT
      reproduce the stall above -- an attached client's own traffic keeps
      waking the server loop -- so it is coverage, not a guard.
"""
import os, pty, sys, time, struct, fcntl, termios, subprocess, tempfile, re
import signal, shutil

AMUX = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "amux")
SD = tempfile.mkdtemp(prefix="muxL.")
ENV = dict(os.environ, AMUX_SOCKET_DIR=SD)
ENV.pop("AMUX_REPLAY", None)
ANSI = re.compile(rb'\x1b\[[0-9;?]*[ -/]*[@-~]')
OSC = re.compile(rb'\x1b\][0-9].*?\x07')

N = 500000          # ~4.3 MB of history
DUMP_BUDGET = 25    # seconds; the stall used to blow through any budget

def cleanup_sessions():
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
            d = os.read(m, 1 << 16)
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

def longest_run_from_1(ns):
    want = 1
    for n in ns:
        if n == want:
            want += 1
    return want - 1

def dump(session):
    """Return (bytes, seconds, timed_out)."""
    t0 = time.time()
    try:
        r = subprocess.run([AMUX, "-H", session], env=ENV,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           timeout=DUMP_BUDGET)
        return len(r.stdout), time.time() - t0, False
    except subprocess.TimeoutExpired:
        return -1, time.time() - t0, True

res = []
def check(name, ok, detail=""):
    print(f"[{'OK ' if ok else 'FAIL'}] {name}" + (f"  {detail}" if detail else ""))
    res.append(ok)

# ---- L2 first: dump a session that is actively producing -------------------
subprocess.run([AMUX, "-n", "growing", "sh", "-c", f"seq 1 {N}; sleep 600"],
               env=ENV, stdin=subprocess.DEVNULL,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
live = []
for i in range(4):
    n, dt, to = dump("growing")
    live.append((n, dt, to))
    time.sleep(0.3)
check("L2: every dump of a live session terminates",
      all(not to for _, _, to in live),
      " ".join(f"#{i+1}={'TIMEOUT' if to else str(n)+'B'}/{dt:.2f}s"
               for i, (n, dt, to) in enumerate(live)))

# ---- L1: large static history, dumped repeatedly ---------------------------
time.sleep(8)                       # let the filler finish
sizes = []
for i in range(5):
    n, dt, to = dump("growing")
    sizes.append((n, dt, to))
check("L1: repeated dumps of a static history all complete",
      all(not to for _, _, to in sizes),
      " ".join(f"{dt:.2f}s" for _, dt, _ in sizes))
full = [n for n, _, to in sizes if not to]
check("L1: and every one returns the identical, whole history",
      len(full) == 5 and len(set(full)) == 1 and full[0] > 3_000_000,
      f"sizes={sorted(set(full))}")

# ---- L3: the same stall on the ordinary replay path ------------------------
p, m = spawn([AMUX, "-e", "^\\", "-s", "full", "-a", "growing"])
got = longest_run_from_1(nums(drain(m, 25.0)))
check("L3: -s full on a large idle session delivers all %d lines" % N,
      got == N, f"contiguous 1..{got}")
os.write(m, b"\x1c"); time.sleep(0.4)
try: p.wait(timeout=5)
except Exception: pass
os.close(m)

cleanup_sessions()
shutil.rmtree(SD, ignore_errors=True)
print("-" * 40)
print("PASS" if all(res) else "SOME TESTS FAILED")
sys.exit(0 if all(res) else 1)
