#!/usr/bin/env python3
"""Queued pty write path: client input -> application.

Before this fix the server handed each input packet to the pty with a blocking
write_all(). An application that had stopped reading its stdin (a huge paste, a
piped-in file) filled the pty and the whole server loop parked inside that
write: no output pumped, no new clients accepted.

What the fix does and does not buy:

  * it does buy responsiveness -- the server loop never blocks on the
    application, and memory stays bounded (PTYBUF_HIGHWATER / SRCBUF_HIGHWATER)
  * it does NOT make a huge burst lossless. A pty's input queue is ~4 KB and
    the tty layer silently DISCARDS the overflow; a bare pty with no amux in
    the picture loses the same data (see the control below). Delivering a
    228 KB paste intact needs flow control above the pty (bracketed paste, or
    an application that reads continuously) -- it is not something a session
    manager can buffer its way out of.

  H1  the server answers a probe while the app ignores a large backlog
  H2  amux delivers at least as much as a bare pty does under that burst
  H3  input within the tty's capacity round-trips complete and in order
  H4  a detach keypress is still instant with a backlog queued
"""
import os, pty, sys, time, struct, fcntl, termios, subprocess, tempfile, re
import signal
import threading, shutil

AMUX = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "amux")  # repo-relative: runs anywhere
SD = tempfile.mkdtemp(prefix="muxH.")
ENV = dict(os.environ, AMUX_SOCKET_DIR=SD)
ENV.pop("AMUX_REPLAY", None)
ANSI = re.compile(rb'\x1b\[[0-9;?]*[ -/]*[@-~]')
OSC = re.compile(rb'\x1b\][0-9].*?\x07')

N = 40000          # ~228 KB burst, far past any pty input queue
SMALL = 400        # ~1.5 KB, comfortably inside it
SLEEP = 4          # seconds the application ignores its stdin

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

def blocking(fd):
    fl = fcntl.fcntl(fd, fcntl.F_GETFL); fcntl.fcntl(fd, fcntl.F_SETFL, fl & ~os.O_NONBLOCK)

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

res = []
def check(name, ok, detail=""):
    print(f"[{'OK ' if ok else 'FAIL'}] {name}" + (f"  {detail}" if detail else ""))
    res.append(ok)

payload = b"".join(b"%d\n" % i for i in range(1, N + 1))
small = b"".join(b"%d\n" % i for i in range(1, SMALL + 1))

# ---- control: the same burst through a bare pty, no amux -----------------
def bare_pty_run():
    m, s = pty.openpty(); set_ws(s)
    p = subprocess.Popen(["sh", "-c", f"sleep {SLEEP}; exec cat"], stdin=s,
                         stdout=s, stderr=s, start_new_session=True)
    os.close(s)
    out = bytearray(); stop = []
    def rd():
        fl = fcntl.fcntl(m, fcntl.F_GETFL)
        fcntl.fcntl(m, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        while not stop:
            try:
                d = os.read(m, 65536)
            except BlockingIOError:
                time.sleep(0.01); continue
            except OSError:
                break
            if d: out.extend(d)
    t = threading.Thread(target=rd, daemon=True); t.start()
    os.write(m, payload)
    time.sleep(SLEEP + 4); stop.append(True); time.sleep(0.2)
    try: p.kill()
    except Exception: pass
    os.close(m)
    return longest_run_from_1(nums(bytes(out)))

control_run = bare_pty_run()
print(f"     (control: a bare pty delivers 1..{control_run} of {N})")

# ---- the same burst through amux -----------------------------------------
p1, m1 = spawn([AMUX, "-e", "^\\", "-s", "none", "-c", "tH",
                "sh", "-c", f"sleep {SLEEP}; exec cat"])
time.sleep(0.6)
blocking(m1)
t0 = time.time()
os.write(m1, payload)
write_dt = time.time() - t0
check("H0: a %d-byte paste is absorbed without blocking" % len(payload),
      write_dt < SLEEP, f"{write_dt:.2f}s")

# ---- H1: the server loop is not parked in a blocking pty write -------------
t0 = time.time()
try:
    probe = subprocess.run([AMUX], env=ENV, capture_output=True, timeout=SLEEP - 1)
    probe_dt = time.time() - t0
    ok = probe.returncode == 0 and b"tH" in probe.stdout
except subprocess.TimeoutExpired:
    probe_dt = time.time() - t0
    ok = False
check("H1: server answers a session probe during the input backlog",
      ok, f"{probe_dt:.2f}s")

# ---- H2: no loss beyond what the tty layer itself imposes ------------------
run = longest_run_from_1(nums(drain(m1, 12.0)))
# The exact point at which the tty layer starts discarding is not
# deterministic -- repeated runs of the SAME binary vary by a few percent in
# both directions -- so this asserts "same order as a bare pty, not
# systematically worse", which is the real claim. A regression that reintroduced
# buffering loss would show up as a fraction of the control, not 5% off it.
check("H2: delivers on par with a bare pty (tty queue is the limit)",
      run >= control_run * 0.8,
      f"amux 1..{run} vs bare pty 1..{control_run}")

os.write(m1, b"\x04")
drain(m1, 1.5); os.close(m1)
try: rc = p1.wait(timeout=5)
except Exception: rc = "(timeout)"
check("H2b: session exits cleanly (rc=0)", rc == 0, f"rc={rc}")

# ---- H3: input inside the tty's capacity is delivered intact ---------------
p3, m3 = spawn([AMUX, "-e", "^\\", "-s", "none", "-c", "tH3", "cat"])
time.sleep(0.6)
blocking(m3)
os.write(m3, small)
run3 = longest_run_from_1(nums(drain(m3, 4.0)))
check("H3: a %d-byte paste round-trips complete and in order" % len(small),
      run3 >= SMALL, f"contiguous 1..{run3} of {SMALL}")
os.write(m3, b"\x04")
drain(m3, 1.0); os.close(m3)
try: p3.wait(timeout=5)
except Exception: pass

# ---- H4: detach stays instant with a backlog queued ------------------------
p4, m4 = spawn([AMUX, "-e", "^\\", "-s", "none", "-c", "tH4",
                "sh", "-c", f"sleep {SLEEP}; exec cat"])
time.sleep(0.6)
blocking(m4)
os.write(m4, payload)
time.sleep(0.5)                       # let the client consume the paste first;
                                      # amux only honours the detach key when
                                      # it starts a read, as upstream always did
t0 = time.time()
os.write(m4, b"\x1c")
try:
    p4.wait(timeout=SLEEP - 1)
    detach_dt = time.time() - t0
    ok = True
except Exception:
    detach_dt = time.time() - t0
    ok = False
check("H4: detach is instant while input is still queued", ok, f"{detach_dt:.2f}s")
os.close(m4)

cleanup_sessions()
shutil.rmtree(SD, ignore_errors=True)
print("-" * 40)
print("PASS" if all(res) else "SOME TESTS FAILED")
sys.exit(0 if all(res) else 1)
