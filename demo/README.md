# Demo assets

`reattach.gif` in the README is rendered from `reattach.cast`, an
[asciicast](https://docs.asciinema.org/manual/asciicast/v2/) recorded by
`record.py`.

Nothing in it is staged: `record.py` drives a real pty running the `amux` built
from this repository, against a real session, and records the bytes that come
back. The only fiction is the *content* of the session's scrollback, which is
printed filler chosen to look like an agent transcript rather than 2,000,000
integers.

## Re-rendering

The cast is committed, so changing how the GIF looks needs only
[agg](https://github.com/asciinema/agg) — no re-recording:

```sh
make demo          # agg cast -> gif
```

## Re-recording

To change what the demo *does*, edit `record.py` and:

```sh
make demo-record   # record.py -> cast, then agg -> gif
```

That takes about a minute: most of it is generating the 17 MB of history, which
happens before recording starts and so costs the GIF nothing.

## What it shows

1. `amux` — the session from yesterday is still running
2. `amux -a agent` — reattach: the recent transcript is there and the `agent>`
   prompt is live, which the demo proves by asking it something
3. `Ctrl+\` — detach
4. `amux -H agent | wc -l` — all 2,000,030 lines are still recorded

## What it deliberately does not show

**The "replaying everything crawls" contrast.** That cost is the *terminal*
drawing tens of megabytes, and a headless recorder drains the pty at memory
speed — there is no renderer in the loop to be slow. A recording would show
`-s full` completing instantly, which is the opposite of the truth on a real
terminal. Faking it with an artificial throttle would be inventing evidence, so
the measured byte counts live in `README.md` instead.

**Native scrolling.** The recording is a fixed-height terminal with no
scrollback to interact with, so a wheel scroll into real terminal scrollback
cannot be captured this way. That claim needs a screen recording of a human
scrolling in a real terminal.

Both are cases where the honest artifact is a number or a sentence rather than
a picture.

## Tuning

- `COLS`/`ROWS` in `record.py` set the terminal size, and so the GIF's aspect.
- `BULK_LINES` is the size of the history. The recorder waits for the ring to
  stop growing before it starts, so raising this only costs setup time.
- `TAIL` is what you actually see on reattach — keep it at least `ROWS` lines
  long, or the filler shows through underneath it.
- `--idle-time-limit` in the Makefile clips long pauses; `--fps-cap` trades
  smoothness against file size.
