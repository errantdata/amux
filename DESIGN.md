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
- [x] Status indicator — OSC window title:
      session name written to the terminal title via OSC 2, pushed/popped with
      the title stack (CSI 22/23 t). Zero screen rows, zero colour/scroll
      impact. The persistent bottom-row variant was deliberately NOT built: a
      reserved row needs a scroll-region (suppresses native scrollback) or full
      screen virtualization (reintroduces the tmux colour/scroll problems).

## Known limitations (revisit later)
- Alternate-screen wrapper removed (resolved): the host terminal now owns
  scrollback. After a reattach replay, how far you can actually scroll back is
  capped by the terminal's own scrollback depth (macOS Terminal.app: Settings >
  Profiles > Window > Scrollback; Windows console: Properties > Layout > Screen
  Buffer Size height) — replaying more just scrolls off the top. Raise that to
  scroll further; the full byte history is kept in the ring regardless.
- Replaying a full 256 MB ring on reattach streams at terminal speed (~1-2 s
  worst case on a fast GPU terminal); typical sessions replay instantly. For
  alt-screen apps most of the ring is superseded frames — harmless but replayed.
- The pty WRITE path (client input -> app) still uses blocking write_all: fine
  for keystrokes, could stall on a huge paste into an app that stopped reading
  stdin. Buffer it if it ever bites.
- Read-side framing errors deliberately close the connection; large input reads
  are chunked to <=4080-byte frames.

## Tunables (currently #defined in abduco.c)
- OUTBUF_HIGHWATER     256 KiB  — per-client output queue cap before pump pauses
- OUTBUF_LOWPRIO_CAP     4 MiB  — observers go lossy past this
- RING_SIZE            256 MiB  — full-history bytes kept per session
- RING_LAG_HIGHWATER     1 MiB  — throttle the app if a real client lags this far
