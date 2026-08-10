# amux — terminal session detach/attach, with history that survives

`amux` runs a program independently of your terminal: detach, close the
terminal, come back later, reattach. It is a fork of
[abduco](https://www.brain-dump.org/projects/abduco) 0.6 by Marc André Tanner,
and it keeps the thing that makes abduco good — it never redraws your screen —
while fixing the two problems that made it painful for long-running,
high-output sessions.

Unlike tmux and screen, `amux` does not virtualize your terminal. It passes the
application's bytes through untouched, so your colours are your colours, your
mouse wheel scrolls your terminal's own scrollback, and there is no prefix key
to fight. It manages one session, not windows and panes — pair it with
[dvtm](https://www.brain-dump.org/projects/dvtm) if you want splits.

## What this fork changes

**Reattaching is instant, on any size of session.** abduco replays nothing on
reattach; you come back to a blank screen. `amux` records *all* output —
including while you were detached — into a 256 MB per-session ring, and on
attach replays only the trailing part your terminal can actually retain
(10,000 lines by default). Replaying more than that just scrolls off the top,
at the cost of rendering every byte first. On a 43.9 MB session that is
**0.09 MB pushed instead of 43.89 MB** — sub-second instead of minutes.

```sh
amux -s 50000 -a work    # replay the last 50k lines
amux -s 2m    -a work    # or a byte budget
amux -s full  -a work    # the whole ring, the slow way
amux -s none  -a work    # straight to the live prompt
```

Set `-s` to your terminal's own scrollback depth to fill it exactly. No
terminal reports that number, so it has to be told, not detected. `$AMUX_REPLAY`
sets the default.

**The full history stays reachable.** The ring keeps everything regardless of
what was replayed:

```sh
amux -H work | less              # page through the whole session
amux -H work | grep -n 'error'   # or search it
amux -H work > session.log
```

The dump never throttles the application, so an unread pager cannot stall your
session.

**No more freezing under heavy output.** abduco 0.6 busy-spun on `EAGAIN` when
a client fell behind, pinning a CPU and stalling the whole loop so output
trickled out at the terminal's drain rate. Every hot-path fd is now
non-blocking and drained through `select()` write-readiness, with real
back-pressure: a slow terminal throttles the application through the pty
instead of melting a core. The input path is queued too, so a large paste into
an application that has stopped reading its stdin no longer parks the server.

**The host terminal owns the screen.** The alternate-screen wrapper is gone, so
scrolling and scrollback are your terminal's, natively. The session name goes
in the window title via OSC 2 — no reserved status row, no scroll region, no
scrollback cost.

See [DESIGN.md](DESIGN.md) for the reasoning, the invariants, and the measured
numbers.

## Install

### Homebrew (macOS and Linux)

```sh
brew install errantdata/tap/amux
```

### Prebuilt binary

Each [release](https://github.com/errantdata/amux/releases) ships tarballs for
linux-x86_64, linux-aarch64, macos-arm64 and macos-x86_64, plus `SHA256SUMS`.

```sh
tar xzf amux-1.0.0-linux-x86_64.tar.gz
sudo install -m0755 amux-1.0.0-linux-x86_64/amux /usr/local/bin/amux
```

### From source

Needs a C99 compiler and `make`; nothing else.

```sh
git clone https://github.com/errantdata/amux
cd amux
./configure && make
sudo make install                 # /usr/local by default
sudo make install-completion      # optional zsh completion
```

Builds and is tested on Linux (glibc and musl), macOS (Apple Silicon and
Intel), and should work on the BSDs as abduco does.

## Usage

```sh
amux -c work                # create a session and attach
amux -c work vim            # ...running a specific command
amux -n build make          # create detached, do not attach
amux                        # list sessions
amux -a work                # reattach
amux -A work htop           # attach, creating it if needed
```

`Ctrl+\` detaches (`-e ^z` picks another key). A session outlives its clients,
so closing the terminal — or losing an ssh connection — leaves it running.

Sessions live in `$AMUX_SOCKET_DIR`, else `$HOME/.amux`, else
`$TMPDIR/amux/$USER`, else `/tmp/amux/$USER`.

Full details: `man amux`.

### Coming from abduco

`amux` installs under its own name and keeps its sessions in `~/.amux`, so it
sits alongside an existing `abduco` without conflict. Your existing abduco
sessions stay with abduco — the socket directory is derived from the program
name, so `amux` will not see them.

`AMUX_*` environment variables are canonical, but the matching `ABDUCO_*` names
are still read when the `AMUX_*` one is unset, and `ABDUCO_SESSION` /
`ABDUCO_SOCKET` are still exported to the supervised command.

## Tests

```sh
make check          # headless: allocates its own ptys, what CI runs
./testsuite.sh      # upstream's byte-exact suite; needs a real tty
```

`make check` covers I/O integrity under back-pressure, 300k-line throughput,
exit-status propagation, interactive detach/reattach, history replay windows,
the `-H` dump, and the queued input path — the last of which measures a bare
pty as a control, because a pty's ~4 KB input queue discards large pastes no
matter who is managing the session.

## Limitations

- How far you can scroll back after reattaching is capped by your terminal's
  scrollback setting, not by the ring. The ring keeps 256 MB regardless; use
  `-H` to reach the rest.
- `-s` counts newlines in the raw byte stream, not rendered rows, so wrapped
  lines yield more screen rows than requested.
- A very large paste loses bytes past the tty's input queue. This is the
  kernel's tty layer, not `amux` — a bare pty with nothing in between loses the
  same data. It needs flow control above the pty (bracketed paste).
- The detach key is honoured only as the first byte of a read, as upstream did:
  a `Ctrl+\` appended to a giant paste is data, not a detach.

## License

ISC, unchanged from abduco. Copyright (c) 2013-2018 Marc André Tanner for the
original work; fork changes copyright (c) 2026 Sean Cantrell. See
[LICENSE](LICENSE).
