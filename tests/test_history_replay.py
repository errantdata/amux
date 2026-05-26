#!/usr/bin/env python3
"""Headline test: full history replay on reattach + native (no alt-screen)."""
import os, pty, time, struct, fcntl, termios, subprocess, tempfile, re, sys
ABDUCO="/home/seanc/git/abduco/abduco"
SD=tempfile.mkdtemp(prefix="muxE."); ENV=dict(os.environ,ABDUCO_SOCKET_DIR=SD)
ANSI=re.compile(rb'\x1b\[[0-9;?]*[ -/]*[@-~]')
OSC=re.compile(rb'\x1b\][0-9].*?\x07')
def set_ws(fd): fcntl.ioctl(fd,termios.TIOCSWINSZ,struct.pack("HHHH",40,120,0,0))
def spawn(a):
    m,s=pty.openpty(); set_ws(s)
    p=subprocess.Popen(a,stdin=s,stdout=s,stderr=s,start_new_session=True,env=ENV,close_fds=True)
    os.close(s); return p,m
def drain(m,t):
    out=bytearray(); end=time.time()+t
    fl=fcntl.fcntl(m,fcntl.F_GETFL); fcntl.fcntl(m,fcntl.F_SETFL,fl|os.O_NONBLOCK)
    while time.time()<end:
        try: d=os.read(m,65536)
        except BlockingIOError: time.sleep(0.02); continue
        except OSError: break
        if d: out+=d
        else: break
    return bytes(out)
def seqrun(raw):
    clean=OSC.sub(b'',ANSI.sub(b'',raw))
    nums=[int(x) for x in re.findall(rb'\d+',clean)]
    want=1
    for n in nums:
        if n==want: want+=1
    return want-1
res=[]
def check(n,ok,d=""): print(f"[{'OK ' if ok else 'FAIL'}] {n}"+(f"  {d}" if d else "")); res.append(ok)

N=5000
# session prints N lines then becomes `cat` (stays alive, exits on EOF)
p1,m1=spawn([ABDUCO,"-e","^\\","-c","tE","sh","-c",f"seq 1 {N}; exec cat"])
o1=drain(m1,1.5)
check("E1: first attach shows full output", seqrun(o1)==N, f"run=1..{seqrun(o1)}")
# no alt-screen wrapper should be emitted by the mux itself
check("E2: mux emits no alternate-screen enter", b"\x1b[?1049h" not in o1)
os.write(m1,b"\x1c"); time.sleep(0.4)          # detach
try: p1.wait(timeout=3)
except Exception: pass
os.close(m1)

# reattach: the FULL history (1..N) must be replayed
p2,m2=spawn([ABDUCO,"-e","^\\","-a","tE"])
o2=drain(m2,2.0)
check("E3: reattach replays full history 1..%d"%N, seqrun(o2)==N, f"run=1..{seqrun(o2)}")
os.write(m2,b"\x04")                            # EOF -> cat exits 0 -> session ends
drain(m2,0.6); os.close(m2)
try: rc=p2.wait(timeout=3)
except Exception: rc="(timeout)"
check("E4: reattached session exits cleanly (rc=0)", rc==0, f"rc={rc}")

import shutil; shutil.rmtree(SD,ignore_errors=True)
print("-"*40); print("PASS" if all(res) else "SOME TESTS FAILED")
sys.exit(0 if all(res) else 1)
