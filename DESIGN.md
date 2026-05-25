# custom-mux — a fork of abduco 0.6

Goal: abduco's transparent detach/attach persistence, with the I/O bug fixed
and a few tmux conveniences, WITHOUT tmux's screen-model side effects
(recolored output, scroll-as-arrows, prefix clunkiness).

Branch: `custom-mux` (forked from abduco 0.6, commit 8c32909).

## Why fork instead of build fresh
abduco already gets the hard, correct parts right: double-fork session
creation, socket-dir discovery/permissions, session listing, resize
propagation. Its only fatal flaw for our workload is the output I/O model.
Keep the correct machinery; replace the I/O scheduling.

## Root cause of the freeze (abduco 0.6)
- `write_all()` busy-spins on EAGAIN/EWOULDBLOCK (abduco.c) instead of
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
- [ ] Step 2 — scrollback that survives reattach:
      server-side replay ring (record last N KB of pty output regardless of
      attach state); on attach, replay the tail before live output.
- [ ] Step 3 — status row:
      sniff `ESC[?1049h/l` (+ legacy ?47/?1047) in the OUTPUT stream to track
      alt-screen state; reserve the bottom row via DECSTBM and draw session
      info only while NOT in alt-screen; plus an on-attach/on-hotkey info
      flash that works everywhere. A persistent bar OVER an alt-screen app
      (e.g. Claude Code) needs full screen virtualization — the tmux cost we
      are avoiding — so it is out of scope.

## Known limitations after step 1 (revisit later)
- The client uses the ALTERNATE SCREEN (inherited from abduco): the host
  terminal's scrollbar is inert during a session, so "native scrollback" for
  a plain shell depends on step 2's replay. Decision pending: keep alt-screen
  (clean reattach) vs. primary-screen passthrough (host scrollback works,
  messier redraw).
- The pty WRITE path (client input -> app) still uses blocking write_all: fine
  for keystrokes, could stall on a huge paste into an app that stopped reading
  stdin. Buffer it if it ever bites.
- Read-side framing errors now deliberately close the connection; large input
  reads are chunked to <=4080-byte frames.

## Tunables (currently #defined in abduco.c)
- OUTBUF_HIGHWATER   256 KiB  — throttle the app above this backlog
- OUTBUF_LOWPRIO_CAP   4 MiB  — observers go lossy past this
