# amux — instant reattach, native scrolling

`amux` keeps a program running when your terminal goes away: detach, close the
laptop, drop the ssh connection, come back later and reattach. It exists to do
two things its ancestors don't.

## Reattach to a long-running session instantly

Coding agents and long builds emit enormous amounts of output. A day with a
coding agent is tens of megabytes of scrollback, and the usual options both
handle coming back to it badly: abduco replays nothing, so you reattach to a
blank screen with your agent's last question gone, while replaying the whole
buffer means watching it redraw — minutes of scrolling to reach the live prompt
at the bottom.

`amux` records **all** output, including while you were detached, but on
reattach it pushes only the part your terminal can actually keep. You land on
the live prompt immediately, with real scrollback above it. On a 43.9 MB
session that is **0.09 MB rendered instead of 43.89 MB**: you arrive at the
prompt at once, rather than waiting for tens of megabytes to redraw at your
terminal's speed.

![Reattaching to an agent session carrying 17 MB of scrollback: the recent
transcript and a live agent prompt come back immediately, and amux -H shows all
2,000,030 lines are still recorded](demo/reattach.gif)

```sh
amux -a agent            # straight to the live prompt, ~10k lines above it
amux -s 50000 -a agent   # or match your terminal's scrollback exactly
amux -s none  -a agent   # nothing but the live tail
amux -s full  -a agent   # everything, the slow way
```

Set `-s` to your terminal's own scrollback depth to fill it exactly. No
terminal reports that number, so it has to be told, not detected.
`$AMUX_REPLAY` sets the default.

## Scroll with your terminal, not against it

`amux` never takes over your screen. It passes the application's bytes through
untouched and stays on the primary screen, so your scroll wheel and trackpad
scroll your **terminal's own scrollback** — real selection, real search, real
copy — instead of being translated into arrow keys inside a copy-mode. Your
colours are your colours, and there is no prefix key to fight.

That is the tmux trade this fork refuses: no virtualized screen, no recoloured
output, no reserved status row. The session name goes in the window title via
OSC 2 instead, which costs zero rows and zero scrollback.

It manages one session, not windows and panes — pair it with
[dvtm](https://www.brain-dump.org/projects/dvtm) if you want splits.

## Relationship to abduco

`amux` is a fork of [abduco](https://www.brain-dump.org/projects/abduco) 0.6 by
Marc André Tanner, whose transparent detach/attach model is the reason this
exists rather than starting from scratch — the hard, correct parts (double-fork
session creation, socket discovery and permissions, resize propagation) are
his. The git history here is abduco's, with every upstream commit and author
intact. This fork is not affiliated with or endorsed by upstream; please report
`amux` bugs here, not to abduco.

It installs under its own name and keeps its sessions in `~/.amux`, so it sits
alongside an existing abduco install without conflict.

## What else this fork changes

**The full history stays reachable.** The ring keeps 256 MB per session
regardless of what was replayed:

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

Your existing abduco sessions stay with abduco: the socket directory is derived
from the program name, so `amux` will not see them. Drain them with `abduco`
before switching, or keep both.

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
- Native scrolling is `amux` getting out of the way, not `amux` providing it.
  An application that switches to the alternate screen or turns on mouse
  reporting (vim, less, most full-screen TUIs) claims the wheel for itself,
  exactly as it would with no session manager in between.

## License

ISC, unchanged from abduco. Copyright (c) 2013-2018 Marc André Tanner for the
original work; fork changes copyright (c) 2026 Sean Cantrell. See
[LICENSE](LICENSE).
