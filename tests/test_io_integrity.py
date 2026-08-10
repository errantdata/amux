#!/usr/bin/env python3
"""Smoke/integrity tests for amux (a fork of abduco 0.6).

Runs the binary under a real PTY (so it does NOT fall into passthrough mode)
in an isolated socket dir, and checks:
  A. integrity under back-pressure: a deliberately slow reader forces the
     server to do partial-packet socket writes, exercising the client's new
     partial-frame reassembly. All N lines must arrive, in order, uncorrupted.
  B. high-volume, fast reader: no hang, full integrity.
  C. exit-status propagation through the detach/attach machinery.
"""
import os, pty, sys, time, struct, fcntl, termios, subprocess, tempfile, re, signal

AMUX = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "amux")  # repo-relative: runs anywhere
SOCKDIR = tempfile.mkdtemp(prefix="muxtest.")
ENV = dict(os.environ, AMUX_SOCKET_DIR=SOCKDIR)
ANSI = re.compile(rb'\x1b\[[0-9;?]*[ -/]*[@-~]')
OSC = re.compile(rb'\x1b\][0-9].*?\x07')      # window title: contains "amux: <name>"

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

def set_winsize(fd, rows=40, cols=120):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

def run_under_pty(args, reader_delay=0.0, read_chunk=65536, timeout=60):
    """Spawn args with a PTY as its std{in,out,err}; return collected bytes."""
    master, slave = pty.openpty()
    set_winsize(slave)
    p = subprocess.Popen(args, stdin=slave, stdout=slave, stderr=slave,
                         start_new_session=True, env=ENV, close_fds=True)
    os.close(slave)
    out = bytearray()
    deadline = time.time() + timeout
    while True:
        if time.time() > deadline:
            os.kill(p.pid, signal.SIGKILL); break
        try:
            data = os.read(master, read_chunk)
        except OSError:
            break            # slave closed -> EIO -> child gone
        if not data:
            break
        out += data
        if reader_delay:
            time.sleep(reader_delay)
    os.close(master)
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.kill(p.pid, signal.SIGKILL)
    return bytes(out)

def longest_run_from_1(nums):
    want = 1
    for n in nums:
        if n == want:
            want += 1
    return want - 1

def extract_seq(raw):
    """De-ANSI, drop the amux epilog line, return the integer tokens."""
    # strip the OSC title first: it also contains "amux: <name>", and the
    # epilog filter below would otherwise eat it plus the first line of output
    clean = ANSI.sub(b'', OSC.sub(b'', raw))
    clean = re.sub(rb'amux:.*', b'', clean)   # epilog status line
    return [int(x) for x in re.findall(rb'\d+', clean)]

def check(name, ok, detail=""):
    print(f"[{'OK ' if ok else 'FAIL'}] {name}" + (f"  {detail}" if detail else ""))
    return ok

results = []

# ---- Test A: integrity under back-pressure (slow reader) -------------------
N = 100000
raw = run_under_pty([AMUX, "-c", "tA", "sh", "-c", f"seq 1 {N}; exit 7"],
                    reader_delay=0.002, read_chunk=1024)
nums = extract_seq(raw)
run = longest_run_from_1(nums)
results.append(check("A: slow-reader integrity 1..%d" % N, run == N,
                     f"got contiguous 1..{run}"))
results.append(check("A: exit status 7 propagated",
                     b"exit status 7" in ANSI.sub(b'', raw)))

# ---- Test B: high volume, fast reader --------------------------------------
N2 = 300000
t0 = time.time()
raw2 = run_under_pty([AMUX, "-c", "tB", "sh", "-c", f"seq 1 {N2}"],
                     reader_delay=0.0)
dt = time.time() - t0
nums2 = extract_seq(raw2)
run2 = longest_run_from_1(nums2)
results.append(check("B: fast-reader integrity 1..%d" % N2, run2 == N2,
                     f"got contiguous 1..{run2} in {dt:.2f}s"))

# ---- Test C: detached -> attach -> exit status -----------------------------
subprocess.run([AMUX, "-n", "tC", "sh", "-c", "sleep 1; exit 23"],
               env=ENV, stdin=subprocess.DEVNULL,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2)  # let it finish while detached
raw3 = run_under_pty([AMUX, "-a", "tC"])
results.append(check("C: attach to finished detached session, status 23",
                     b"exit status 23" in ANSI.sub(b'', raw3)))

# ---- cleanup ---------------------------------------------------------------
cleanup_sessions()
import shutil; shutil.rmtree(SOCKDIR, ignore_errors=True)

print("-" * 40)
print("PASS" if all(results) else "SOME TESTS FAILED")
sys.exit(0 if all(results) else 1)
