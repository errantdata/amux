#!/usr/bin/env python3
"""Interactive-path test: input round-trip, live detach, reattach, exit."""
import os, pty, time, struct, fcntl, termios, subprocess, tempfile, re, signal, sys

AMUX = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "amux")  # repo-relative: runs anywhere
SD=tempfile.mkdtemp(prefix="muxtestD.")
ENV=dict(os.environ, AMUX_SOCKET_DIR=SD)
ANSI=re.compile(rb'\x1b\[[0-9;?]*[ -/]*[@-~]')
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

def set_ws(fd,r=40,c=120): fcntl.ioctl(fd,termios.TIOCSWINSZ,struct.pack("HHHH",r,c,0,0))

def spawn(args):
    m,s=pty.openpty(); set_ws(s)
    p=subprocess.Popen(args,stdin=s,stdout=s,stderr=s,start_new_session=True,env=ENV,close_fds=True)
    os.close(s)
    return p,m

def drain(m, t=0.6):
    out=bytearray(); end=time.time()+t
    fl=fcntl.fcntl(m,fcntl.F_GETFL); fcntl.fcntl(m,fcntl.F_SETFL,fl|os.O_NONBLOCK)
    while time.time()<end:
        try: d=os.read(m,65536)
        except (OSError,BlockingIOError): time.sleep(0.02); continue
        if d: out+=d
    return bytes(out)

res=[]
def check(n,ok,d=""):
    print(f"[{'OK ' if ok else 'FAIL'}] {n}"+(f"  {d}" if d else "")); res.append(ok)

# 1) live session running `cat`; type a line, expect it round-tripped
p,m=spawn([AMUX,"-e","^\\","-c","tD","cat"])
time.sleep(0.5); drain(m,0.4)
os.write(m,b"round-trip-123\n"); o=drain(m,0.6)
check("D1: input->app->output round-trip", b"round-trip-123" in ANSI.sub(b'',o),
      repr(ANSI.sub(b'',o)[-60:]))

# 2) detach with Ctrl-\ ; the client process must exit, session stays alive
os.write(m,b"\x1c"); time.sleep(0.5)
exited = (p.poll() is not None)
try: p.wait(timeout=3)
except Exception: pass
os.close(m)
check("D2: Ctrl-\\ detaches (client exits, status %r)"%p.returncode, p.poll() is not None)
alive = subprocess.run([AMUX],env=ENV,capture_output=True,text=True).stdout
check("D3: session survived detach", "tD" in alive, alive.strip().splitlines()[-1:])

# 3) reattach, send EOF to cat -> clean exit status 0
p2,m2=spawn([AMUX,"-e","^\\","-a","tD"])
time.sleep(0.5)
os.write(m2,b"second-attach-OK\n"); o2=drain(m2,0.5)
check("D4: input works after reattach", b"second-attach-OK" in ANSI.sub(b'',o2))
os.write(m2,b"\x04"); time.sleep(0.3)          # Ctrl-D -> cat EOF -> exit 0
drain(m2,0.6); os.close(m2)
try: rc2=p2.wait(timeout=3)
except Exception: rc2="(timeout)"
check("D5: reattach then EOF -> clean exit (rc=0)", rc2==0, f"rc={rc2}")

cleanup_sessions()
import shutil; shutil.rmtree(SD,ignore_errors=True)
print("-"*40); print("PASS" if all(res) else "SOME TESTS FAILED")
sys.exit(0 if all(res) else 1)
