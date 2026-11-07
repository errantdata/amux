#!/usr/bin/env python3
"""Reattaching to a full-screen application: scrolling must still work.

Coding agents and other full-screen programs set the terminal up once, at
startup -- alternate screen, mouse reporting, bracketed paste -- then draw
nothing but cursor-addressed diffs, with no newlines at all. Replaying a tail
of that stream cannot rebuild the screen and misses the setup entirely, so the
mouse wheel stops reaching the application and scrolls the host terminal's
scrollback instead: blank space and whatever came before amux ran.

The stand-in below behaves as those programs were measured to: it sets its
modes once, draws without a newline, keeps its transcript to itself, scrolls it
on wheel events, and repaints only when its size really changes (Node ignores a
SIGWINCH that reports the size it already knows). The checks read the bytes a
terminal would receive, plus the application's own log of what it saw.

  R1-R5    a session far past the replay budget comes back whole
  R6-R7    the wheel reaches it, and the terminal is left clean on detach
  R8-R11   the repaint: forced only when needed, survives a slow application
  R12-R14  shell history under a running or finished application
  R15-R16  sessions on the primary screen are left as they were
  R17-R18  passthrough and read-only clients
  R19-R22  the mode tracker: split, combined, cleared, restored settings
  R23      a byte-budget cut does not start inside an escape sequence
  R24      modes survive the ring wrapping past the bytes that set them
  R25-R26  the repaint, against output that continues and a second client
  R27-R28  joining mid-stream: not inside a sequence, not into a dead screen
  R29      a replay's leftovers do not outlive it
  R30      a first attach is not a reattach, however fast the application
  R31-R32  what the terminal has really been sent is what detach undoes
  R33      keyboard encoding is undone, and left off so the detach key works
  R34      forced restarts do not leave the application deaf to resizes
  R35-R36  a resize reaches an idle application, with nothing else going on
  R37      a session's last client is told it ended, every time
  R38      restarting with -f does not trip over the server it replaces
  R39-R41  the detach key, however the terminal has been told to encode it
"""
import os, pty, sys, time, struct, fcntl, termios, subprocess, tempfile, re, shutil
import select, signal

AMUX = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "amux")  # repo-relative: runs anywhere
SD = tempfile.mkdtemp(dir="/tmp", prefix="muxR.")
ENV = dict(os.environ, AMUX_SOCKET_DIR=SD)
ENV.pop("AMUX_REPLAY", None)
ROWS, COLS = 40, 120
CHURN = 12 * 1024 * 1024      # past the default 10 MB replay ceiling
SMALL = 64 * 1024             # a reattach should need no more than this

TUI = r'''
import fcntl, os, re, select, signal, struct, sys, termios, time, tty
log_path, churn, slow, flags = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4].split(",")
primary, spin, bare = "primary" in flags, "spin" in flags, "bare" in flags
def log(*words):
    with open(log_path, "a") as f:
        print(*words, file=f)
def write(text):
    data = text.encode()
    while data:
        data = data[os.write(1, data):]
def size():
    return struct.unpack("HH", fcntl.ioctl(0, termios.TIOCGWINSZ, b"\0" * 4))
LINES = ["transcript line %05d" % i for i in range(2000)]
rows, cols = size()
top = len(LINES)
def paint(why):
    body = rows - 1
    first = max(0, top - body)
    frame = ["\x1b[?2026h"]
    for i in range(body):
        frame.append("\x1b[%d;1H\x1b[K%s" % (i + 1, LINES[first + i][:cols]))
    frame.append("\x1b[%d;1H\x1b[Kstatus %s %dx%d top=%d\x1b[?2026l" % (rows, why, rows, cols, top))
    write("".join(frame))
    log("paint", why, rows, cols)
winched = [None]
def on_winch(*a):
    if winched[0] is None:
        winched[0] = time.time()
signal.signal(signal.SIGWINCH, on_winch)
saved = termios.tcgetattr(0)
tty.setraw(0)
# the real order: queries and modes on the primary screen, then the switch
if not bare:
    write("\x1b[?2004h\x1b[>7u\x1b[?1004h\x1b[?u\x1b[c\x1b[6n\x1b]11;?\x1b\\")
if bare:
    write("\x1b[?1049h\x1b[2J")
elif not primary:
    write("\x1b[?1049h\x1b[2J\x1b[>7u\x1b[?1007l\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h\x1b[?25l")
paint("startup")
sent = 0
while sent < churn:
    burst = "".join("\x1b[%d;40H\x1b[38;5;%dmspinner %08d\x1b[0m" % (rows, k % 200, k) for k in range(2000))
    write(burst)
    sent += len(burst)
log("ready")
pending = b""
while True:
    if spin:
        write("\x1b[1;90Hspin %d" % (time.time() % 10))
    # a busy application: the signal is in, but it gets round to it this late
    if winched[0] is not None and time.time() - winched[0] >= slow:
        winched[0] = None
        now = size()
        log("winch", *now)
        if now != (rows, cols):
            rows, cols = now
            paint("resize")
    try:
        if not select.select([0], [], [], 0.01)[0]:
            continue
        data = os.read(0, 4096)
    except InterruptedError:
        continue
    except OSError:
        break
    if not data:
        break
    pending += data
    while True:
        m = re.search(rb"\x1b\[<(\d+);\d+;\d+[Mm]", pending)
        if not m:
            break
        pending = pending[m.end():]
        if int(m.group(1)) == 64:
            top = max(rows - 1, top - 3)
            log("wheel-up", top)
            paint("wheel")
    if b"q" in data:
        break
log("bye")
write("\x1b[?25h\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l\x1b[<u\x1b[?1049l\x1b[<u\x1b[?1004l\x1b[?2004l")
termios.tcsetattr(0, termios.TCSANOW, saved)
'''
TUI_PATH = os.path.join(SD, "tui.py")
with open(TUI_PATH, "w") as f:
    f.write(TUI)

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

class Terminal:
    """An amux client on its own pty, recording what a terminal would receive."""
    def __init__(self, args, rows=ROWS, cols=COLS):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.proc = subprocess.Popen([AMUX] + args, stdin=slave, stdout=slave, stderr=slave,
                                     start_new_session=True, env=ENV, close_fds=True)
        os.close(slave)
        fl = fcntl.fcntl(self.master, fcntl.F_GETFL)
        fcntl.fcntl(self.master, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        self.out = bytearray()

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                data = os.read(self.master, 1 << 16)
            except BlockingIOError:
                # wait the way a terminal does: macOS hands over 1 KB per read,
                # so sleeping between reads means 100 KB/s and a false failure
                select.select([self.master], [], [], 0.01)
                continue
            except OSError:
                return
            if not data:
                return
            self.out += data

    def pump_until(self, needle, timeout=20):
        """Read until needle shows up; never sleep a fixed time and hope."""
        end = time.time() + timeout
        while needle not in self.out and time.time() < end:
            self.pump(0.05)
        return needle in self.out

    def send(self, data):
        os.write(self.master, data)

    def detach(self):
        """Detach and return exactly what the client wrote while leaving."""
        mark = len(self.out)
        self.send(b"\x1c")
        self.pump_until(b"detached", 5)
        self.pump(0.1)
        return bytes(self.out[mark:])

    def close(self):
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        os.close(self.master)

class App:
    """The stand-in, in a detached session, so it can churn with no reader."""
    count = 0
    def __init__(self, churn=0, slow=0.0, flags="alt", wrap=None):
        App.count += 1
        self.name = "r%d" % App.count
        self.log = os.path.join(SD, self.name + ".log")
        tui = ["python3", TUI_PATH, self.log, str(churn), str(slow), flags]
        if wrap:
            tui = ["sh", "-c", wrap.replace("TUI", " ".join(tui))]
        boot = Terminal(["-n", self.name] + tui)
        boot.proc.wait(timeout=10)
        boot.pump(0.05)
        boot.close()
        self.wait_for("ready", 60)
        time.sleep(0.15)                 # let the server drain the last of it

    def lines(self):
        try:
            with open(self.log) as f:
                return f.read().splitlines()
        except FileNotFoundError:
            return []

    def wait_for(self, word, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            if any(word in line for line in self.lines()):
                return True
            time.sleep(0.02)
        return False

    def paints(self):
        return [line for line in self.lines() if line.startswith("paint")]

    def attach(self, *args, **size):
        return Terminal(list(args) + ["-a", self.name], **size)

    def wait_paints(self, count, timeout=5):
        end = time.time() + timeout
        while len(self.paints()) < count and time.time() < end:
            time.sleep(0.02)
        return len(self.paints())

res = []
def check(name, ok, detail=""):
    print(f"[{'OK ' if ok else 'FAIL'}] {name}" + (f"  {detail}" if detail else ""))
    res.append(bool(ok))

def mode(number, value):
    return b"\x1b[?%d%c" % (number, ord(value))

SETUP = [mode(1049, "h"), mode(1000, "h"), mode(1002, "h"), mode(1003, "h"), mode(1006, "h"),
         mode(1004, "h"), mode(2004, "h"), mode(1007, "l"), mode(25, "l")]
QUERIES = [b"\x1b[c", b"\x1b[6n", b"\x1b]11;?", b"\x1b[?u"]

# --- a session far past the replay budget ----------------------------------
big = App(churn=CHURN)
term = big.attach()
got_frame = term.pump_until(b"status resize %dx%d" % (ROWS, COLS))
out = bytes(term.out)
check("R1: reattach restores every mode set at startup",
      all(seq in out for seq in SETUP), f"missing {[s for s in SETUP if s not in out]}")
check("R2: the application repaints the whole screen", got_frame and
      all(b"transcript line %05d" % n in out for n in (1962, 1990, 1999)))
check("R3: reattach is instant, not a 10 MB replay", len(out) < SMALL, f"{len(out):,} bytes")
check("R4: no churn from the old screen reaches the terminal", b"spinner" not in out)
check("R5: no stale query gets answered into the application",
      not any(q in out for q in QUERIES), f"{[q for q in QUERIES if q in out]}")

# --- the wheel, and leaving ------------------------------------------------
# A terminal reports the wheel only if it has been told to; otherwise the wheel
# scrolls its own scrollback and the application never hears about it.
if mode(1000, "h") in out and mode(1006, "h") in out:
    term.send(b"\x1b[<64;60;20M")
check("R6: the wheel reaches the application, which scrolls",
      big.wait_for("wheel-up", 2) and term.pump_until(b"status wheel", 2))
left = term.detach()
term.close()
check("R7: detach turns off everything left on", all(seq in left for seq in
      [mode(1000, "l"), mode(1002, "l"), mode(1003, "l"), mode(1006, "l"), mode(1004, "l"),
       mode(2004, "l"), mode(1049, "l")]) and left.count(b"\x1b[23;2t") == 1 and
      left.count(b"\x1b[r") == 1 and b"\x1b7\x1b[r\x1b8" in left,  # DECSTBM homes the cursor
      repr(left))

# --- the repaint -----------------------------------------------------------
same = App()
paints = len(same.paints())
term = same.attach()
term.pump_until(b"status resize %dx%d" % (ROWS, COLS))
sizes = [line.split()[2:] for line in same.paints()[paints:]]
check("R8: same size: forced with a real, temporary size change",
      sizes == [[str(ROWS - 1), str(COLS)], [str(ROWS), str(COLS)]], f"{sizes}")
check("R9: the alternate-screen attach leaves the primary screen alone",
      b"\x1b[2J" not in term.out and mode(1049, "h") in term.out)
term.detach()
term.close()

moved = App()
paints = len(moved.paints())
term = moved.attach(rows=30, cols=100)
term.pump_until(b"status resize 30x100")
time.sleep(1.2)                          # past REDRAW_TIMEOUT_MS: nothing more is coming
term.pump(0.05)
sizes = [line.split()[2:] for line in moved.paints()[paints:]]
check("R10: a different size repaints by itself, no forced resize",
      sizes == [["30", "100"]], f"{sizes}")
term.detach()
term.close()

slow = App(slow=0.3)
paints = len(slow.paints())
term = slow.attach()
term.pump_until(b"status resize %dx%d" % (ROWS, COLS))
sizes = [line.split()[2:] for line in slow.paints()[paints:]]
check("R11: an application too busy to react at once still repaints",
      sizes[-1:] == [[str(ROWS), str(COLS)]], f"{sizes}")
term.detach()
term.close()

# --- shell history around full-screen applications -------------------------
under = App(wrap="seq 1 300; exec TUI")
term = under.attach()
term.pump_until(b"status resize %dx%d" % (ROWS, COLS))
out = bytes(term.out)
at = out.find(mode(1049, "h"))
check("R12: output from before the application is replayed under it",
      at > 0 and b"\r\n300\r\n" in out[:at] and b"\x1b[2J" in out[:at])
check("R13: its setup and queries are not",
      not any(q in out for q in QUERIES) and out.count(mode(1049, "h")) == 1,
      f"{[q for q in QUERIES if q in out]}")
term.detach()
term.close()

gone = App(churn=CHURN, wrap="TUI; seq 6000 6009; exec cat")
term = gone.attach()
term.pump_until(b"status resize")
term.send(b"q")                          # quit it: back to the primary screen
term.pump_until(b"6009\r\n", 20)
term.detach()
term.close()
term = Terminal(["-a", gone.name])
term.pump_until(b"6009", 5)
term.pump(0.3)
out = bytes(term.out)
check("R14: a finished application's screen is not replayed",
      b"spinner" not in out and b"6000\r\n" in out and mode(1049, "h") not in out and len(out) < SMALL,
      f"{len(out):,} bytes")
term.detach()
term.close()

# --- the primary screen ----------------------------------------------------
plain = App(flags="primary", wrap="seq 1 3000; exec TUI")
term = plain.attach()
term.pump_until(b"status startup")
term.pump(0.3)
out = bytes(term.out)
check("R15: a primary-screen session gets its replay and its modes",
      b"\r\n3000\r\n" in out and b"\x1b[2J" in out and mode(2004, "h") in out)
check("R16: no alternate screen, and nothing forces a repaint",
      mode(1049, "h") not in out and not plain.lines()[-1].startswith("paint resize") and
      all(line.split()[-1] != "resize" for line in plain.paints()))
term.detach()
term.close()

# --- other kinds of client -------------------------------------------------
quiet = App()
paints = len(quiet.paints())
p = subprocess.Popen([AMUX, "-a", quiet.name], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, env=ENV)
p.wait(timeout=5)
p.stdout.close()
time.sleep(0.3)
check("R17: passthrough does not disturb the application",
      len(quiet.paints()) == paints, f"paints {paints} -> {len(quiet.paints())}")

ro = App()
term = ro.attach("-r")
check("R18: a read-only client gets the screen too",
      term.pump_until(b"status resize %dx%d" % (ROWS, COLS)) and mode(1049, "h") in term.out)
term.detach()
term.close()

# --- the mode tracker ------------------------------------------------------
def modes_from(script):
    """Run a script in a session, reattach, return what the terminal is sent."""
    App.count += 1
    name = "m%d" % App.count
    boot = Terminal(["-n", name, "sh", "-c", script])
    boot.proc.wait(timeout=10)
    boot.close()
    time.sleep(0.4)
    term = Terminal(["-s", "none", "-a", name])
    term.pump(0.3)
    out = bytes(term.out)
    term.detach()
    term.close()
    return out

out = modes_from(r"printf '\033[?10'; sleep 0.15; printf '00h\033[?1006h'; exec cat")
check("R19: a sequence split across writes is still understood",
      mode(1000, "h") in out and mode(1006, "h") in out, repr(out))
out = modes_from(r"printf '\033[?1000;1006;2004h\033[?1h\033=\033[5 q'; exec cat")
check("R20: combined modes, keypad and cursor style are carried over",
      all(seq in out for seq in [mode(1000, "h"), mode(1006, "h"), mode(2004, "h"), mode(1, "h"),
                                 b"\x1b=", b"\x1b[5 q"]), repr(out))
out = modes_from(r"printf '\033[?1000h\033[?2004h\033[?1000l\033[?2026h'; exec cat")
check("R21: what was turned off again is not restored, nor an open frame",
      mode(1000, "h") not in out and mode(2004, "h") in out and mode(2026, "h") not in out, repr(out))
out = modes_from(r"printf '\033[?1049h\033[?1000h\033c'; exec cat")
check("R22: a terminal reset clears it all",
      mode(1049, "h") not in out and mode(1000, "h") not in out, repr(out))

# --- the byte budget -------------------------------------------------------
App.count += 1
name = "b%d" % App.count
boot = Terminal(["-n", name, "sh", "-c",
                 r"i=0; while [ $i -lt 400 ]; do printf '\033[38;5;123mabcdefgh'; i=$((i+1)); done; exec cat"])
boot.proc.wait(timeout=10)
boot.close()
time.sleep(0.4)
term = Terminal(["-s", "1k", "-a", name])
term.pump(0.4)
out = bytes(term.out)
body = out[out.find(b"\x1b[2J") + 4:]
check("R23: a cut on the byte budget lands on a sequence boundary",
      body.startswith(b"\x1b[38;5;123m"), repr(body[:24]))
term.detach()
term.close()

# --- the ring wraps --------------------------------------------------------
wrapped = App(wrap="seq 1 5; python3 -c \"import sys; sys.stdout.write('x' * 79 + chr(10))\"; "
                   "exec TUI", churn=270 * 1024 * 1024)
term = wrapped.attach()
got = term.pump_until(b"status resize %dx%d" % (ROWS, COLS))
out = bytes(term.out)
dump = subprocess.run([AMUX, "-H", wrapped.name], env=ENV, stdout=subprocess.PIPE, timeout=120).stdout
check("R24: modes outlive the ring: 256 MB later they are still restored",
      got and mode(1049, "h") not in dump and all(seq in out for seq in SETUP) and len(out) < SMALL,
      f"{len(dump):,} bytes retained, {len(out):,} sent")
term.detach()
term.close()

# --- the repaint, under load ------------------------------------------------
# Output that never stops says nothing about whether the application has
# noticed the resize yet; only a quiet spell or the time limit can end the wait.
busy = App(slow=0.35, flags="alt,spin")
paints = len(busy.paints())
term = busy.attach()
got = term.pump_until(b"status resize %dx%d" % (ROWS, COLS), 5)
check("R25: output carrying on does not end the repaint early",
      got and busy.paints()[paints:] and busy.paints()[-1].split()[2:] == [str(ROWS), str(COLS)],
      f"{busy.paints()[paints:]}")
term.detach()
term.close()

second = App()
paints = len(second.paints())
first = second.attach()
end = time.time() + 5
while len(second.paints()) == paints and time.time() < end:   # into the short row
    time.sleep(0.002)
late = second.attach(rows=ROWS - 1)
got = late.pump_until(b"status resize %dx%d" % (ROWS - 1, COLS), 5)
check("R26: a client arriving in the middle of a repaint still gets one", got,
      f"{second.paints()[paints:]}")
for term in (late, first):
    term.pump(0.1)
    term.detach()
    term.close()

# --- joining mid-stream ----------------------------------------------------
App.count += 1
name = "j%d" % App.count
boot = Terminal(["-n", name, "sh", "-c",
                 r"printf '\033[?1049h\033[5;'; sleep 1.5; printf '9Hjoined'; exec sleep 60"])
boot.proc.wait(timeout=10)
boot.close()
time.sleep(0.5)
term = Terminal(["-a", name])
term.pump_until(b"joined", 4)
check("R27: joining the live stream mid-sequence keeps the sequence whole",
      b"\x1b[5;9Hjoined" in term.out, repr(bytes(term.out)[-40:]))
term.detach()
term.close()

# A replay held up by a slow terminal, with the application leaving the
# alternate screen meanwhile: 1.2 MB is close enough that it is still let run.
filler = "python3 -c \"print(('x' * 79 + chr(10)) * 15000, end='')\""
left_alt = App(wrap=f"{filler}; TUI; seq 7000 7009; exec cat")
term = left_alt.attach("-s", "full")    # and read nothing: the replay stalls
time.sleep(0.4)
term.send(b"q")
left_alt.wait_for("bye", 5)
time.sleep(0.3)                          # for the server to record that it left
term.pump_until(b"7009\r\n", 10)
out = bytes(term.out)
check("R28: leaving the alternate screen during a replay drops that screen",
      b"7009" in out and b"status" not in out and not any(q in out for q in QUERIES),
      f"{len(out):,} bytes, {[q for q in QUERIES if q in out]}")
term.detach()
term.close()

# --- a replay's leftovers --------------------------------------------------
# What a shell does around a command line: bracketed paste goes off only after
# the newline, in the bytes a replay stops short of. Without being told, the
# terminal would keep wrapping pastes for a program that never asked for that.
leftover = App(flags="bare", wrap=r"printf '\033[?2004hprompt\r\n\033[?2004l\r'; exec TUI")
term = leftover.attach()
term.pump_until(b"status resize %dx%d" % (ROWS, COLS))
out = bytes(term.out)
at = out.find(mode(1049, "h"))
check("R29: a mode a replay left on is turned back off",
      at > 0 and mode(2004, "h") in out[:at] and mode(2004, "l") in out[at:], repr(out[:80]))
term.detach()
term.close()

# --- a first attach is not a reattach --------------------------------------
lost = 0
for i in range(30):
    App.count += 1
    term = Terminal(["-c", "f%d" % App.count, "sh", "-c",
                     r"printf '\033[?1049h\033[6nfirst-frame'; exec sleep 60"])
    if not term.pump_until(b"first-frame", 2):
        lost += 1
    term.detach()
    term.close()
check("R30: an application that is quick off the mark loses nothing on first attach",
      lost == 0, f"{lost} of 30 lost their first output")

# --- detach undoes what the terminal really got ----------------------------
App.count += 1
name = "k%d" % App.count
script = (r"printf '\033[?1049h\033[?1000h\033[?1006h\033[>1u\033[>4;2m'; read x; "
          "python3 -c \"print(('y' * 79 + chr(10)) * 2000, end='')\"; "
          r"printf '\033[?1006l\033[?1000l\033[<u\033[>4m\033[?1049l'; exec cat")
term = Terminal(["-c", name, "sh", "-c", script])
term.pump_until(b"\x1b[>4;2m")
term.pump(0.2)
term.send(b"\n")                         # and stop reading, like a stalled terminal
time.sleep(0.7)
left = term.detach()
check("R31: modes queued for a stalled terminal but never sent are still undone",
      all(seq in left[-400:] for seq in [mode(1000, "l"), mode(1006, "l"), mode(1049, "l")]),
      repr(left[-160:]))
term.close()

App.count += 1
name = "e%d" % App.count
term = Terminal(["-c", name, "sh", "-c",
                 r"printf '\033[?1000h\033[?1049h\033[?1049l\033[?1000l'; exit 3"])
end = time.time() + 5
while term.proc.poll() is None and time.time() < end:
    term.pump(0.05)
term.pump(0.2)
out = bytes(term.out)
tail = out[out.rfind(mode(1000, "l")) + 8:]
check("R32: an application that cleaned up leaves nothing to undo",
      term.proc.returncode == 3 and mode(1000, "l") not in tail and mode(1049, "l") not in tail,
      repr(tail[:60]))
term.close()

# --- keyboard encoding -----------------------------------------------------
App.count += 1
name = "c%d" % App.count
script = r"printf '\033[>7u\033[>4;2m\033[?1049h\033[>5u'; exec cat"
term = Terminal(["-c", name, "sh", "-c", script])
term.pump_until(b"\x1b[>5u")
left = term.detach()
term.close()
term = Terminal(["-a", name])
term.pump(0.5)
out = bytes(term.out)
again = term.detach()
term.close()
check("R33: keyboard encoding is undone on detach, and not restored after",
      b"\x1b[<1u\x1b[?1049l\x1b[<1u" in left and b"\x1b[>4m" in left and
      not re.search(rb"\x1b\[>[0-9;]*[um]", out) and b"detached" in again, repr(left[:90]))

# --- forced restarts -------------------------------------------------------
# -f attaches to the finished session, then starts a new one from the same
# process. On a loaded machine that can beat the old server's exit and fail with
# "Address already in use", a race of its own. Retrying must not drop the attach
# that makes this a test, so each attempt gets a finished session of its own.
blocked = None
for attempt in range(20):
    App.count += 1
    name = "w%d" % App.count
    boot = Terminal(["-n", name, "true"])
    boot.proc.wait(timeout=10)
    boot.close()
    time.sleep(0.4)
    term = Terminal(["-f", "-c", name, "python3", "-c",      # directly: a shell clears its mask
                     "import signal, time; print('blocked=%d' % (signal.SIGWINCH in signal.pthread_sigmask(signal.SIG_BLOCK, []))); time.sleep(60)"])
    end = time.time() + 20
    while b"blocked=" not in term.out and term.proc.poll() is None and time.time() < end:
        term.pump(0.05)
    term.pump(0.2)
    m = re.search(rb"blocked=(\d)", term.out)
    if m:
        blocked = int(m.group(1))
        term.detach()
    term.close()
    if m:
        break
check("R34: a session restarted with -f can still be told about resizes",
      blocked == 0, f"SIGWINCH blocked: {blocked}")

# --- resizes reach an application that is doing nothing ---------------------
def set_size(term, rows, cols):
    """What a terminal does on a window resize; no controlling tty here, so the
    signal has to be sent by hand."""
    fcntl.ioctl(term.master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    term.proc.send_signal(signal.SIGWINCH)

idle = App()
term = idle.attach()
term.pump_until(b"status resize %dx%d" % (ROWS, COLS))
time.sleep(0.3)
set_size(term, 25, 90)
got = term.pump_until(b"status resize 25x90", 2)
check("R35: a window resize reaches an idle application with no keypress", got,
      f"{idle.paints()[-1:]}")
term.detach()
term.close()

pair = App()
big = pair.attach()
big.pump_until(b"status resize %dx%d" % (ROWS, COLS))
small_term = pair.attach(rows=20, cols=70)
small_term.pump_until(b"status resize 20x70")
time.sleep(0.3)
paints = len(pair.paints())
small_term.detach()
small_term.close()
got = pair.wait_paints(paints + 1, 2) > paints and \
      pair.paints()[-1].split()[2:] == [str(ROWS), str(COLS)]
check("R36: the remaining client's size applies as soon as another leaves", got,
      f"{pair.paints()[-1:]}")
big.detach()
big.close()

# --- exit ------------------------------------------------------------------
hung = 0
for i in range(150):
    App.count += 1
    term = Terminal(["-c", "x%d" % App.count, "sh", "-c", "exit 7"])
    end = time.time() + 3
    while term.proc.poll() is None and time.time() < end:
        term.pump(0.002)
    if term.proc.poll() is None:
        hung += 1
        term.proc.kill()
    term.close()
check("R37: an ending session always tells its client", hung == 0, f"{hung} of 150 hung")

failed = 0
for i in range(25):
    App.count += 1
    name = "y%d" % App.count
    boot = Terminal(["-n", name, "true"])
    boot.proc.wait(timeout=10)
    boot.close()
    time.sleep(0.05)
    term = Terminal(["-f", "-c", name, "sh", "-c", "echo restarted; exec sleep 60"])
    if not term.pump_until(b"restarted", 5):
        failed += 1
    if term.proc.poll() is None:
        term.detach()
    term.close()
check("R38: -f restarts cleanly over a session that has just ended", failed == 0,
      f"{failed} of 25 failed")

# --- the detach key --------------------------------------------------------
def detaches(key):
    App.count += 1
    term = Terminal(["-c", "d%d" % App.count, "sh", "-c", "stty raw -echo; exec cat"])
    term.pump(0.15)
    term.send(b"marker")
    term.pump_until(b"marker", 3)
    term.send(key)
    gone = term.pump_until(b"detached", 0.7)
    forwarded = bytes(term.out).count(key[-3:]) > 0
    if not gone:
        term.send(b"\x1c")
        term.pump_until(b"detached", 3)
    term.close()
    return gone, forwarded

check("R39: Ctrl-\\ under the kitty keyboard protocol detaches",
      detaches(b"\x1b[92;5u")[0] and detaches(b"\x1b[92;5:1u")[0] and detaches(b"\x1b[92:124;5;28u")[0])
check("R40: Ctrl-\\ under modifyOtherKeys detaches", detaches(b"\x1b[27;5;92~")[0])
gone, forwarded = detaches(b"\x1b[92;5:3u")
other, passed = detaches(b"\x1b[93;5u")
check("R41: a key release, or any other key, is left for the application",
      not gone and not other and passed, f"release detached={gone}, other detached={other}, passed on={passed}")

cleanup_sessions()
shutil.rmtree(SD, ignore_errors=True)
print("-" * 40)
print("PASS" if all(res) else "SOME TESTS FAILED")
sys.exit(0 if all(res) else 1)
