# amux — a fork of abduco 0.6

Goal: abduco's transparent detach/attach persistence, with the I/O bug fixed
and a few tmux conveniences, WITHOUT tmux's screen-model side effects
(recolored output, scroll-as-arrows, prefix clunkiness).

Forked from abduco 0.6 at commit 8c32909. The binary is named `amux` so it
coexists with upstream abduco; sessions therefore live in `~/.amux`, since
the socket directory is derived from argv[0].

## Why fork instead of build fresh
abduco already gets the hard, correct parts right: double-fork session
creation, socket-dir discovery/permissions, session listing, resize
propagation. Its only fatal flaw for our workload is the output I/O model.
Keep the correct machinery; replace the I/O scheduling.

## Root cause of the freeze (abduco 0.6)
- `write_all()` busy-spins on EAGAIN/EWOULDBLOCK (amux.c, was abduco.c) instead of
  yielding back to select().
- No per-client output buffer: the server holds one pty packet and must push
  it to every client before reading more.
- The client writes to the real terminal with a blocking write.

Chain: heavy app output -> client blocks on a slow terminal -> stops draining
its socket -> server's non-blocking send hits EAGAIN -> busy-spins, pinning a
core and stalling the loop -> output trickles out at the terminal's drain rate.

## Invariants (do not regress)
1. TRANSPARENT: never parse/rewrite the application's byte stream on the
   output path. Colors, mouse, scrolling are the host terminal's job, not
   ours. (Opt-in exception: alt-screen sniffing for the status row, step 3.)
2. NO BUSY-SPIN: every hot-path fd is non-blocking and drained via select
   write-readiness. EAGAIN means "try later", never "loop now".
   Corollary (learned the hard way): ask select() for write readiness whenever
   a client is *owed* bytes, not merely when its out buffer is non-empty. A
   flush that fully drains the buffer otherwise leaves nothing to wake the
   loop, and an idle application never wakes it either -- the client stalls
   mid-replay. See server_client_owes_data().
3. BACK-PRESSURE, DON'T DROP (real clients): if the terminal is slow, throttle
   the app through the pty. Only low-priority observers are lossy.
4. RESPONSIVE UNDER LOAD: the detach key and resize must work while output is
   backed up — the input path is never blocked by the output path.
5. WIRE FORMAT UNCHANGED: same Packet framing as upstream.

## Step plan
- [x] Step 1 — I/O rewrite:
      per-fd Buffer (append/flush/free); streaming packet reader
      (reader_fill/reader_next) tolerant of partial frames; non-blocking
      stdin/stdout on the client with save/restore; pty-read gating on a
      high-water mark; lossy bounded queues for observers.
- [x] Step 2 — full history that survives reattach:
      256 MB disk-backed ring (mmap of an unlinked tmpfile; malloc fallback)
      records ALL pty output, even while detached. Every client streams the
      ring from its own monotonic cursor, unifying replay and live output: a
      (re)attaching client starts at the oldest retained byte, replays the
      whole history, then continues live. The app is throttled
      (server_should_read_pty) while any real client lags > RING_LAG_HIGHWATER,
      so nothing is overwritten before it is consumed and a long replay can't
      be outrun. The client alternate-screen wrapper was REMOVED so the host
      terminal owns scrolling/scrollback; the client only sniffs ?1049/?47/?1047
      in the output stream so detach can restore a sane primary screen.
- [x] Step 3 — bounded replay window (-s) + history dump (-H):
      replaying the whole ring on reattach meant waiting minutes at terminal
      render speed before reaching the live prompt on a long session. A client
      now sends its replay budget in MSG_ATTACH (lines and a byte ceiling) and
      the server seeks back over that many newlines from the write head, so a
      reattach lands on the live tail at once. Default 10000 lines, overridable
      with `-s <lines|Nk|Nm|full|none>` or $AMUX_REPLAY; passthrough clients
      (which discard output anyway) replay nothing. The ring still keeps the
      full RING_SIZE — `amux -H name` streams all of it to stdout for a
      pager, grep or a file. On a 43.9 MB session a default reattach pushes
      0.09 MB instead of 43.89 MB.
- [x] Step 4 — queued pty write path:
      client input -> app went through the last blocking write_all() on a hot
      path, so an application that stopped reading its stdin parked the whole
      server loop (no output pumped, no clients accepted). The pty master is
      now non-blocking and drained through Server.pty_out via select() write
      readiness, with back-pressure in both directions: the server stops
      reading client sockets past PTYBUF_HIGHWATER, and the client stops
      reading stdin past SRCBUF_HIGHWATER, so `cat big-file | amux -p` can
      no longer be slurped into memory. Note this buys responsiveness, not
      paste integrity: see "Known limitations".
- [x] Status indicator — OSC window title:
      session name written to the terminal title via OSC 2, pushed/popped with
      the title stack (CSI 22/23 t). Zero screen rows, zero colour/scroll
      impact. The persistent bottom-row variant was deliberately NOT built: a
      reserved row needs a scroll-region (suppresses native scrollback) or full
      screen virtualization (reintroduces the tmux colour/scroll problems).

## Known limitations (revisit later)
- Alternate-screen wrapper removed (resolved): the host terminal now owns
  scrollback. How far you can scroll back after a reattach is capped by the
  terminal's own scrollback depth (macOS Terminal.app: Settings > Profiles >
  Window > Scrollback; Windows console: Properties > Layout > Screen Buffer
  Size height) — replaying past that just scrolls off the top, which is why
  -s bounds the replay by default. Set -s to match your terminal's depth to
  fill it exactly; the full byte history stays in the ring either way.
- No terminal reports its scrollback depth (CSI 19t gives the screen size, not
  the scrollback), so -s / $AMUX_REPLAY has to be told, not detected.
- -s counts newlines in the raw byte stream, not rendered rows: with wrapped
  lines you get more screen rows than you asked for, and TUI redraw churn (a
  spinner repainting one row) costs bytes without costing lines. The byte
  ceiling (lines * REPLAY_LINE_BYTES) is the backstop.
- Large pastes are lossy, and not because of us: a pty's input queue is ~4 KB
  and the tty layer silently DISCARDS the overflow. A bare pty with no amux
  involved loses the same bytes from a 228 KB burst (tests/test_pty_input.py
  measures both). The queued write path removes the server stall; delivering a
  huge paste intact needs flow control above the pty (bracketed paste, or an
  app that reads continuously) and is not something a session manager can
  buffer its way out of.
- The detach key is only honoured when it is the first byte of a read, as
  upstream always did — a Ctrl-\ appended to a giant paste is data, not a
  detach. Pressing it as its own keystroke is instant regardless of backlog.
- Read-side framing errors deliberately close the connection; large input reads
  are chunked to <=4080-byte frames.
- A dump (-H) is a snapshot: its end point is the write head as it was when the
  dump attached (Client.history_end). Chasing a live head instead means the
  dump either stops early on a lucky catch-up or never terminates, which is
  exactly the `amux -H session | less` case. tests/test_large_streams.py guards
  both this and the select() wakeup rule below.

## Tunables (currently #defined in amux.c)
- OUTBUF_HIGHWATER     256 KiB  — per-client output queue cap before pump pauses
- OUTBUF_LOWPRIO_CAP     4 MiB  — observers go lossy past this
- PTYBUF_HIGHWATER     256 KiB  — stop reading client sockets past this much
                                  input still queued for the application
- SRCBUF_HIGHWATER       4 MiB  — client stops reading stdin past this much
                                  unsent input (well above any human paste)
- RING_SIZE            256 MiB  — full-history bytes kept per session
- RING_LAG_HIGHWATER     1 MiB  — throttle the app if a real client lags this far
- REPLAY_DEFAULT_LINES   10000  — trailing lines replayed on attach (-s)
- REPLAY_LINE_BYTES      1 KiB  — byte ceiling per requested replay line

## Wire protocol changes
Framing is unchanged (same Packet header, same 4096-byte frames).
- MSG_ATTACH payload grew from a bare uint32 flags to {flags, lines, bytes};
  flags still overlays u.i, and a short attach packet is read as "full
  replay", which is what an older client means by it.
- MSG_HISTORY_END (6) is sent to an -H client once the ring has been streamed.
- CLIENT_HISTORY (1<<2) marks that client: it is never lossy (unlike a
  low-priority observer) but never throttles the application either, so
  `amux -H foo | less` cannot freeze the session behind an unread pager.
